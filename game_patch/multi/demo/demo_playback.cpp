#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <common/utils/list-utils.h>
#include <xlog/xlog.h>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include "demo.h"
#include "demo_file.h"
#include "demo_internal.h"
#include "../network.h"
#include "../multi.h"
#include "../../misc/misc.h"
#include "../../misc/alpine_settings.h"
#include "../../hud/multi_spectate.h"
#include "../../hud/hud.h"
#include "../../rf/multi.h"
#include "../../rf/level.h"
#include "../../rf/gameseq.h"
#include "../../rf/object.h"
#include "../../rf/entity.h"
#include "../../rf/corpse.h"
#include "../../rf/particle_emitter.h"
#include "../../rf/vmesh.h"
#include "../../rf/geometry.h"
#include "../../graphics/gr.h"
#include "../../rf/ui.h"
#include "../../rf/sound/sound.h"
#include "../../sound/sound.h"
#include "../../rf/player/player.h"
#include "../../rf/player/camera.h"
#include "../../rf/file/file.h"
#include "../../os/os.h"
#include "../../rf/gr/gr.h"
#include "../../rf/gr/gr_font.h"
#include "../../rf/os/frametime.h"
#include "../../rf/os/os.h"
#include "../../rf/os/timer.h"
#include "../../rf/os/console.h"
#include "../../os/console.h"

// Note: this must be called from DLL init function
// Note: we can't use global variable because that would lead to crash when launcher loads this DLL to check dependencies
static rf::CmdLineParam& get_demo_cmd_line_param()
{
    static rf::CmdLineParam demo_param{"-demo", "", true};
    return demo_param;
}

namespace
{
    // Stock packet type ids the playback pump treats specially
    constexpr uint8_t pkt_join_accept = 0x03;
    constexpr uint8_t pkt_players = 0x06;
    constexpr uint8_t pkt_state_info_done = 0x0A;
    constexpr uint8_t pkt_chat_line = 0x0C;
    constexpr uint8_t pkt_leave_limbo = 0x16;
    constexpr uint8_t pkt_obj_update = 0x26;
    constexpr uint8_t pkt_weapon_fire = 0x30;
    constexpr uint8_t pkt_sound = 0x34;
    constexpr uint8_t pkt_af_ping_location = 0x51;
    constexpr uint8_t pkt_af_obj_update = 0x53;

    // Synthetic "server" address the whole replayed session runs against; equals the
    // recorder's reserved address so recorded self-references stay coherent.
    constexpr rf::NetAddr demo_playback_addr{{0x7F000001}, 1};

    enum class PlaybackState
    {
        inactive,
        joining,              // stock join flow navigating to the server list (start_join_multi_game_sequence)
        waiting_for_level,    // join_accept fed; engine is loading the level
        feeding_state_info,   // pump until state_info_done is processed
        playing,
        finished,
    };

    struct PlaybackCtx
    {
        PlaybackState state = PlaybackState::inactive;
        DemoFileReader reader;
        DemoRecord pending;
        bool has_pending = false;
        double clock_ms = 0.0;
        float timescale = 1.0f;
        bool paused = false;
        // True pause is in effect: gameplay_sim_frame is skipped, game time (and with
        // it every rf::Timestamp deadline) is frozen and playing sounds are paused.
        // Managed exclusively by pause_fx_update()/reset_ctx() so the inc/dec of the
        // engine's game-time pause counter always stays balanced.
        bool pause_fx_applied = false;
        // Teardown must run from the networking phase (multi_io_do_frame), before
        // rf_do_state dispatches - console commands execute inside gameplay_do_frame,
        // and tearing down there lets the rest of that frame simulate/render with the
        // character data multi_stop just released (crash). The engine's own
        // connection-loss teardown is safe for exactly this frame-position reason.
        bool teardown_queued = false;
        // The recorded players packet has been fed; its processing is what triggers
        // game_new_game/GS_NEW_LEVEL on the client (multi_after_players_packet @ 0x00482080)
        bool players_fed = false;
        uint32_t duration_ms = 0; // footer value or 0 when unknown (unclean file)
        bool spectate_entered = false;
        bool in_teardown = false;
        double pending_seek_ms = -1.0;
        std::string display_name;
        // Multi-frame fast-forward ("seeking" sub-state of playing): records are fed in
        // wall-clock budgeted chunks per pump frame while rendering and audio stay
        // suppressed and the seek overlay is drawn instead of the frozen world.
        double seek_target_ms = -1.0;     // >= 0: budgeted fast-forward in progress
        double seek_start_ms = 0.0;       // clock at seek begin (progress bar denominator)
        bool seek_settle = false;         // suppression held until fresh obj_updates arrive
        bool seek_obj_update_seen = false;
        int64_t seek_settle_deadline = 0; // wall-clock cap for the settle window
        bool seek_audio_muted = false;
        // Follow-cam: auto-attach on entering gameplay and remember the followed player
        // across seeks/restarts. Targets are stored by net player_id (stable across a
        // restart of the same stream), never by pointer - the roster is rebuilt.
        bool cam_want_attached = true;    // false only when the user deliberately went freelook
        bool cam_third_person = false;
        int cam_target_id = -1;           // remembered followed player
        int cam_last_active_id = -1;      // most recent killer/attacker
        bool cam_attach_pending = false;  // retry attach each pump frame (roster fills incrementally)
        int64_t cam_attach_deadline = 0;  // wall-clock give-up
        // Freelook camera view remembered across seeks and session restarts. The
        // freelook camera rebuilds its view from control_data every frame
        // (eye_phb.x = pitch, phb.y = yaw), so the control angles must be kept
        // alongside the matrices or the restored view would snap back.
        bool cam_free_valid = false;
        rf::Vector3 cam_free_pos;
        rf::Matrix3 cam_free_orient;
        rf::Vector3 cam_free_phb;
        rf::Vector3 cam_free_eye_phb;
        // POV ping compensation: delay currently applied to non-POV entities, slewed
        // toward the desired value each frame so the world glides on target/ping changes
        float povcomp_applied_ms = 0.0f;
        // Level-timer sync: the engine ticks level.time_left in wall-clock time, but
        // pauses/seeks/timescale make demo time diverge from wall time - watching
        // longer than the match had left would run the timer out mid-demo. Captured
        // as (recorded time_left + clock) on entering gameplay; < 0 = no time limit
        // on this level (or not captured yet).
        double igt_base_left_s = -1.0;
        double igt_last_written_s = -1.0; // re-seed detection (see sync_level_timer)
    };

    PlaybackCtx g_ctx;

    // Backward seek restarts the session, but doing that synchronously from the console
    // handler (teardown + immediate rejoin mid-frame) re-enters gameplay rendering after
    // multi_stop released the character data - the same crash demo_stop had. Instead the
    // restart is queued here and performed from demo_playback_on_state_init once the
    // engine has settled in a menu state after the forced END_GAME teardown.
    struct PendingRestart
    {
        std::string path;
        double seek_ms;
        float timescale;
        bool paused;
        // Camera memory carried across the session restart (see cam_* in PlaybackCtx)
        bool cam_want_attached;
        bool cam_third_person;
        int cam_target_id;
        int cam_last_active_id;
        bool cam_free_valid;
        rf::Vector3 cam_free_pos;
        rf::Matrix3 cam_free_orient;
        rf::Vector3 cam_free_phb;
        rf::Vector3 cam_free_eye_phb;
    };
    std::optional<PendingRestart> g_pending_restart;

    bool ensure_pending_record()
    {
        if (g_ctx.has_pending)
            return true;
        if (g_ctx.reader.next_record(g_ctx.pending)) {
            g_ctx.has_pending = true;
            return true;
        }
        return false;
    }

    void consume_pending_record()
    {
        g_ctx.has_pending = false;
    }

    // A players packet body ends with a continuation byte: 1 = another roster chunk
    // follows, 2 = final chunk. send_players_packet (0x00481C70) packs at most 8
    // players per chunk, and only processing the FINAL chunk runs
    // multi_after_players_packet (the level-load trigger) - so the pregame feed must
    // not stop at an earlier chunk or the join never leaves the server list.
    bool is_final_players_packet(const DemoRecord& rec)
    {
        if (!rec.is_packet() || rec.packet_type() != pkt_players)
            return false;
        const uint8_t* data = rec.packet_data();
        const size_t len = rec.packet_len();
        const auto declared_size = static_cast<uint16_t>(data[1] | (data[2] << 8));
        const size_t body_len = std::min<size_t>(declared_size, len - 3);
        if (body_len == 0)
            return true; // malformed chunk - treat as final rather than stall the pump
        return data[3 + body_len - 1] == 2;
    }

    // The player the camera is following (or will re-attach to mid fast-forward, when
    // the live spectate state is not yet re-established); null in free camera.
    rf::Player* resolve_followed_player()
    {
        if (multi_spectate_is_following_player()) {
            if (rf::Player* target = multi_spectate_get_target_player())
                return target;
        }
        if (g_ctx.cam_target_id >= 0)
            return rf::multi_find_player_by_id(static_cast<uint8_t>(g_ctx.cam_target_id));
        return nullptr;
    }

    struct TeamScopeDecision
    {
        bool skip = false;
        // For team chat: team to impersonate around processing, so the stock handler
        // (0x00444860) - which drops team lines whose sender team differs from
        // local_player->team - displays the line with the (TEAM) tag and team color
        std::optional<rf::ubyte> spoof_local_team;
    };

    // Team-scoped traffic (team chat, team location pings): show only what the
    // followed player's team would have seen; free camera sees both teams
    TeamScopeDecision check_team_scoped_record(const DemoRecord& rec, bool fast_forward)
    {
        TeamScopeDecision decision;
        if (!(rec.flags() & DEMO_PKT_TEAM_SCOPED))
            return decision;
        const uint8_t packet_type = rec.packet_type();
        if (fast_forward && packet_type == pkt_af_ping_location) {
            decision.skip = true; // transient world marker; team chat still accumulates across seeks
            return decision;
        }
        const rf::ubyte scoped_team = (rec.flags() & DEMO_PKT_TEAM1) ? 1 : 0;
        rf::Player* followed = resolve_followed_player();
        if (followed && followed->team != scoped_team) {
            decision.skip = true;
            return decision;
        }
        if (packet_type == pkt_chat_line) {
            rf::ubyte sender_team = scoped_team;
            // wire layout: {type u8, size u16}, player_id u8, is_team_msg u8, msg...
            if (rec.packet_len() >= 5) {
                if (rf::Player* sender = rf::multi_find_player_by_id(rec.packet_data()[3]))
                    sender_team = sender->team;
            }
            decision.spoof_local_team = sender_team;
        }
        return decision;
    }

    // Feeds exactly one recorded game packet through the engine dispatcher. A crafted
    // .afd could declare a sub-packet size that doesn't match the record, driving the
    // dispatcher/handlers past a tight heap block (H1). Two defenses: (1) require the
    // record to hold exactly one well-framed packet ({u8 type, u16 size, payload} with
    // size == len-3); (2) feed from a reused, zero-filled scratch buffer sized like the
    // wire path's 16KB static receive arena, so any handler over-read lands in zeroed
    // scratch instead of adjacent heap. Returns false when the record is rejected.
    bool feed_one_packet(const uint8_t* data, size_t len)
    {
        static std::array<uint8_t, 0x4000> scratch;
        if (len < 3 || len > scratch.size())
            return false;
        const size_t declared = static_cast<size_t>(data[1]) | (static_cast<size_t>(data[2]) << 8);
        if (declared != len - 3)
            return false;
        std::memset(scratch.data(), 0, scratch.size());
        std::memcpy(scratch.data(), data, len);
        rf::multi_io_process_packets(scratch.data(), len, demo_playback_addr, nullptr);
        return true;
    }

    // Returns true only when the record was actually handed to the engine dispatcher
    // (feed_one_packet accepted it). Transition detectors are computed from the raw
    // record, so callers that advance the state machine (players_fed, state_info_done)
    // must gate on this - a mis-framed record feed_one_packet drops (H1) must not
    // advance state without the engine ever processing the packet (N5 wedge).
    bool feed_packet_record(const DemoRecord& rec, bool fast_forward)
    {
        if (!rec.is_packet())
            return false;
        const uint8_t packet_type = rec.packet_type();
        // The synthesized join_accept was consumed at session start; a stray one
        // mid-stream must not re-trigger a level load
        if (packet_type == pkt_join_accept)
            return false;
        // leave_limbo at a segment tail would start loading the next level of the
        // rotation; a demo file ends at the limbo scoreboard instead
        if (packet_type == pkt_leave_limbo)
            return false;
        if (fast_forward && (packet_type == pkt_sound || packet_type == pkt_weapon_fire))
            return false; // no lasting state; skipping avoids sound/effect spam
        if (!packet_check_whitelist(packet_type)) {
            xlog::warn("Demo playback: skipping non-whitelisted packet type 0x{:02x}", packet_type);
            return false;
        }
        const TeamScopeDecision team_scope = check_team_scoped_record(rec, fast_forward);
        if (team_scope.skip)
            return false;
        // First normal-paced position update after a seek: the world has settled, the
        // post-seek suppression (overlay + mute) can end this frame
        if (!fast_forward && g_ctx.seek_settle
            && (packet_type == pkt_obj_update || packet_type == pkt_af_obj_update)) {
            g_ctx.seek_obj_update_seen = true;
        }
        std::optional<rf::ubyte> saved_local_team;
        if (team_scope.spoof_local_team && rf::local_player) {
            saved_local_team = rf::local_player->team;
            rf::local_player->team = *team_scope.spoof_local_team;
        }
        const bool fed = feed_one_packet(rec.packet_data(), rec.packet_len());
        if (saved_local_team)
            rf::local_player->team = *saved_local_team;
        return fed;
    }

    void clear_all_object_interpolation()
    {
        // All object types - items, movers, clutter and corpses keep interp rings too,
        // not just entities; a stale ring on any of them lerps across the seek jump
        for (rf::Object* obj = rf::object_list.next_obj; obj != &rf::object_list; obj = obj->next_obj) {
            if (obj->obj_interp) {
                obj->obj_interp->Clear();
            }
        }
    }

    void seek_restore_audio()
    {
        if (g_ctx.seek_audio_muted) {
            g_ctx.seek_audio_muted = false;
            set_sound_enabled(true);
        }
    }

    void reset_ctx()
    {
        // Teardown can arrive while paused (demo_stop, engine multi_stop, backward-seek
        // restart) - release the engine pause primitives so the counter never leaks
        if (g_ctx.pause_fx_applied) {
            g_ctx.pause_fx_applied = false;
            rf::timer::dec_game_paused();
            rf::snd_pause(false);
        }
        seek_restore_audio(); // teardown can arrive mid-seek
        demo_powerup_timers_reset();
        g_ctx.reader.close();
        g_ctx.state = PlaybackState::inactive;
        g_ctx.has_pending = false;
        g_ctx.clock_ms = 0.0;
        g_ctx.timescale = 1.0f;
        g_ctx.paused = false;
        g_ctx.players_fed = false;
        g_ctx.teardown_queued = false;
        g_ctx.duration_ms = 0;
        g_ctx.spectate_entered = false;
        g_ctx.pending_seek_ms = -1.0;
        g_ctx.display_name.clear();
        g_ctx.seek_target_ms = -1.0;
        g_ctx.seek_start_ms = 0.0;
        g_ctx.seek_settle = false;
        g_ctx.seek_obj_update_seen = false;
        g_ctx.seek_settle_deadline = 0;
        g_ctx.cam_want_attached = true;
        g_ctx.cam_third_person = false;
        g_ctx.cam_target_id = -1;
        g_ctx.cam_last_active_id = -1;
        g_ctx.cam_attach_pending = false;
        g_ctx.cam_attach_deadline = 0;
        g_ctx.cam_free_valid = false;
        g_ctx.povcomp_applied_ms = 0.0f;
        g_ctx.igt_base_left_s = -1.0;
        g_ctx.igt_last_written_s = -1.0;
    }

    void playback_teardown()
    {
        if (g_ctx.state == PlaybackState::inactive)
            return;
        // Keep the outbound gate up (state stays != inactive) while multi_stop runs so
        // the disconnect sequence (left_game etc.) never reaches a real socket.
        g_ctx.in_teardown = true;
        set_jump_to_multi_server_list(false); // cancel any in-flight join navigation
        // Terminal stop: the exit navigation lands on the demo browser instead of the
        // multiplayer server list. A queued backward-seek restart keeps the stock
        // server-list route - the deferred rejoin runs from that state's init.
        set_jump_to_demo_browser(!g_pending_restart);
        if (rf::is_multi) {
            rf::multi_stop();
            // Force END_GAME immediately - the exact sequence the engine's own
            // connection-loss path (multi_check_socket_status) uses. multi_stop released
            // the multiplayer character data, so the remainder of this frame must not
            // render the world (crash in gr_d3d_render_character_vif / skeleton page-in
            // when a spectated character was on screen).
            rf::gameseq_set_state(rf::GS_END_GAME, true);
        }
        else {
            // multi_start never ran (early failure) - multi_stop would early-return on
            // its started-flag, so drop the in_mp_game flag we set ourselves
            rf::remove_in_mp_flag();
            rf::gameseq_set_state(rf::GS_MAIN_MENU, false);
        }
        rf::multi_clear_current_server_addr();
        g_ctx.in_teardown = false;
        reset_ctx();
    }

    void begin_attach_retry()
    {
        g_ctx.cam_attach_pending = true;
        g_ctx.cam_attach_deadline = timer::get_i64(1000) + 5000;
    }

    // Refresh the remembered camera intent from the live spectate state. Skipped while an
    // auto-attach is still pending so a not-yet-satisfied attach doesn't read as freelook.
    void capture_camera_memory()
    {
        if (!g_ctx.spectate_entered || g_ctx.cam_attach_pending)
            return;
        const bool following = multi_spectate_is_following_player();
        g_ctx.cam_want_attached = following;
        if (following) {
            g_ctx.cam_third_person = multi_spectate_get_camera_state().third_person;
            rf::Player* target = multi_spectate_get_target_player();
            if (target && target->net_data) {
                g_ctx.cam_target_id = target->net_data->player_id;
            }
        }
        else if (multi_spectate_is_freelook() && rf::local_player && rf::local_player->cam
                 && rf::local_player->cam->camera_entity) {
            rf::Entity* cep = rf::local_player->cam->camera_entity;
            g_ctx.cam_free_pos = cep->pos;
            g_ctx.cam_free_orient = rf::camera_get_orient(rf::local_player->cam);
            g_ctx.cam_free_phb = cep->control_data.phb;
            g_ctx.cam_free_eye_phb = cep->control_data.eye_phb;
            g_ctx.cam_free_valid = true;
        }
    }

    // Slaves the level timer to the demo clock. The recorded state_info seeds
    // level.time_left with the match's remaining time, but the engine then ticks
    // it in wall-clock time - pause, rewind and timescale all desync it, and
    // watching for longer than the match had left runs it to zero mid-demo.
    // Rebasing it on the demo clock every pump frame makes it show what the
    // timer actually read at this point of the match (the engine's own per-frame
    // decrement lands after this and is overwritten again next frame).
    void sync_level_timer()
    {
        if (!rf::gameseq_in_gameplay())
            return;
        if (g_ctx.igt_base_left_s < 0.0) {
            if (rf::level.time_left > 0.0f) {
                g_ctx.igt_base_left_s = static_cast<double>(rf::level.time_left) + g_ctx.clock_ms / 1000.0;
            }
            return; // no time limit on this level
        }
        // Between our writes only the engine's per-frame decrement touches the
        // value; a bigger jump means a fed packet re-seeded it (e.g. a recorded
        // time-extend vote) - adopt that as the new base instead of clobbering
        // it. Compared against the last written value, not the expected one, so
        // the clock leaps of a fast-forward can't trigger a false rebase.
        const double actual = static_cast<double>(rf::level.time_left);
        if (g_ctx.igt_last_written_s >= 0.0 && std::abs(actual - g_ctx.igt_last_written_s) > 5.0) {
            g_ctx.igt_base_left_s = actual + g_ctx.clock_ms / 1000.0;
        }
        const double expected = std::max(0.0, g_ctx.igt_base_left_s - g_ctx.clock_ms / 1000.0);
        rf::level.time_left = static_cast<float>(expected);
        g_ctx.igt_last_written_s = expected;
    }

    // Puts the freelook camera back where capture_camera_memory() last saw it.
    // Mirrors the waypoint editor's view lock: the matrices give this frame's
    // view, the control angles keep it from snapping back next frame.
    void apply_freelook_camera_memory()
    {
        if (!g_ctx.cam_free_valid || !rf::local_player || !rf::local_player->cam)
            return;
        rf::Entity* cep = rf::local_player->cam->camera_entity;
        if (!cep)
            return;
        cep->pos = g_ctx.cam_free_pos;
        cep->eye_pos = g_ctx.cam_free_pos;
        cep->p_data.pos = g_ctx.cam_free_pos;
        cep->p_data.next_pos = g_ctx.cam_free_pos;
        cep->orient = g_ctx.cam_free_orient;
        cep->p_data.orient = g_ctx.cam_free_orient;
        cep->p_data.next_orient = g_ctx.cam_free_orient;
        cep->eye_orient = g_ctx.cam_free_orient;
        cep->control_data.phb = g_ctx.cam_free_phb;
        cep->control_data.eye_phb = g_ctx.cam_free_eye_phb;
        cep->control_data.delta_phb.zero();
        cep->control_data.delta_eye_phb.zero();
        cep->p_data.vel.set(0.0f, 0.0f, 0.0f);
        // Keep the camera entity's room in sync with its position
        cep->set_room(nullptr);
        cep->update_room();
    }

    // Auto-follow candidate: remembered target, then the most recent killer/attacker,
    // then any player from the roster (alive preferred)
    rf::Player* find_attach_candidate()
    {
        auto usable = [](rf::Player* p) {
            return p && p != rf::local_player && !p->is_browser;
        };
        if (g_ctx.cam_target_id >= 0) {
            rf::Player* p = rf::multi_find_player_by_id(static_cast<uint8_t>(g_ctx.cam_target_id));
            if (usable(p))
                return p;
        }
        if (g_ctx.cam_last_active_id >= 0) {
            rf::Player* p = rf::multi_find_player_by_id(static_cast<uint8_t>(g_ctx.cam_last_active_id));
            if (usable(p))
                return p;
        }
        rf::Player* fallback = nullptr;
        for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
            if (!usable(&p))
                continue;
            if (!rf::player_is_dead(&p))
                return &p;
            if (!fallback)
                fallback = &p;
        }
        return fallback;
    }

    void cam_do_frame()
    {
        if (!g_ctx.spectate_entered || !rf::gameseq_in_gameplay() || g_ctx.seek_target_ms >= 0.0)
            return;
        if (!g_ctx.cam_attach_pending) {
            // Steady state: track what the user last had so seeks/restarts restore it
            capture_camera_memory();
            return;
        }
        // The roster is fed incrementally, so the attach retries until a player exists
        if (rf::Player* candidate = find_attach_candidate()) {
            multi_spectate_apply_camera_state({true, g_ctx.cam_third_person}, candidate);
            if (multi_spectate_is_following_player()) {
                g_ctx.cam_attach_pending = false;
                if (candidate->net_data) {
                    g_ctx.cam_target_id = candidate->net_data->player_id;
                }
                return;
            }
        }
        if (timer::get_i64(1000) >= g_ctx.cam_attach_deadline) {
            g_ctx.cam_attach_pending = false; // nobody to follow - stay in freelook
        }
    }

    // Kills replayed during a fast-forward leave freshly-dying entities and corpses
    // behind: death animations and corpse decay run on sim time, which barely advances
    // while records are burst-fed, so minutes worth of kills all arrive "fresh" at the
    // target. Live, none of them would still be visible after that much time - cull
    // them while the settle overlay hides the frame. obj_flag_dead only queues
    // OF_DELAYED_DELETE, so flagging during iteration is safe.
    void cull_seek_death_leftovers()
    {
        for (auto& entity : DoublyLinkedList{rf::entity_list}) {
            if (rf::entity_is_dying(&entity)) {
                rf::obj_flag_dead(&entity);
            }
        }
        for (auto& corpse : DoublyLinkedList{rf::corpse_list}) {
            rf::obj_flag_dead(&corpse);
        }
    }

    // The interp rings run on wall-clock time, which the pause freeze cannot stop:
    // the per-frame advance (multi_obj_interp_update @ 0x00483BE0) steps interp_time
    // by the timer delta since frame_time_us (timer_get(1000) ms as patched by
    // obj_interp_too_fast_fix), and sample insertion (ObjInterp::set_next_pos_orient
    // @ 0x00483360) records the timer_get(1000) gap since last_update_time into
    // arrive_time_diff - whose 20-entry average is the physics-extrapolation step
    // and re-anchor headroom for every later sample. The freeze stops the sim and
    // the packet feed but not the wall clock, so after a long pause the first fed
    // obj_update would log the whole pause as one arrival gap, polluting the average
    // (and with it the spline endpoint prediction) until it leaves the ring ~20
    // samples later - every remote player jerks around for that stretch. Stamping
    // the per-ring clocks to "now" on unfreeze makes the pause invisible to both
    // consumers; interp_time and the recorded keyframes are untouched, so playback
    // resumes exactly where it stopped.
    void rebase_interp_clocks_after_freeze()
    {
        const auto now_ms = static_cast<uint32_t>(rf::timer::get(1000));
        for (rf::Object* obj = rf::object_list.next_obj; obj != &rf::object_list; obj = obj->next_obj) {
            if (rf::ObjInterp* interp = obj->obj_interp) {
                interp->frame_time_us = now_ms;
                if (interp->last_update_time != static_cast<uint32_t>(-1)) { // -1 = no sample yet (Clear)
                    interp->last_update_time = now_ms;
                }
            }
        }
    }

    // True pause: freeze the world simulation, freeze game time (every rf::Timestamp
    // deadline - fire waits, corpse decay, emitter spawns - holds, so nothing catches
    // up in a burst on resume) and pause playing sounds. Never engaged while a seek is
    // in flight: the post-seek culls flag objects with obj_flag_dead, whose deferred
    // deletes only execute inside obj_move_all - the settle window needs live sim
    // frames to reap them; the freeze (re-)engages the frame the settle ends.
    // Gated on spectate_entered (gameplay reached), NOT on the current gameseq state:
    // opening the Esc menu leaves GS_GAMEPLAY, and dropping the freeze there resumed
    // the paused sounds and game time behind the menu.
    void pause_fx_update()
    {
        const bool want = g_ctx.state == PlaybackState::playing && g_ctx.paused
            && !demo_playback_is_seeking() && g_ctx.spectate_entered;
        if (want == g_ctx.pause_fx_applied)
            return;
        g_ctx.pause_fx_applied = want;
        if (want) {
            rf::timer::inc_game_paused();
            rf::snd_pause(true);
        }
        else {
            rf::timer::dec_game_paused();
            rf::snd_pause(false);
            rebase_interp_clocks_after_freeze();
        }
    }

    // The freelook camera entity is normally flown by obj_move_all, which the sim
    // freeze skips. Its input keeps arriving while frozen (player_process_controls ->
    // player_free_cam_update -> controls_read into ai.ci), so running the engine's own
    // per-entity physics step for just this entity reproduces stock movement exactly -
    // same movemode axis mapping, mouselook feel, acceleration and world collision.
    // Mirrors obj_move_all pass 0 + the obj_process_physics per-entity sequence.
    void paused_freelook_camera_physics()
    {
        if (!rf::local_player || !rf::local_player->cam)
            return;
        rf::Camera* cam = rf::local_player->cam;
        if (cam->mode != rf::CAMERA_FREELOOK || !cam->camera_entity)
            return;
        rf::Entity* cep = cam->camera_entity;
        if (cep->p_data.flags & rf::PF_SKIP_SIM_ONCE) {
            cep->p_data.flags &= ~rf::PF_SKIP_SIM_ONCE; // just entered freelook - skip one frame like obj_move_all
            return;
        }
        rf::physics_frame_init(&cep->p_data);
        cep->p_data.flags |= rf::PF_SIMULATED_THIS_FRAME;
        rf::physics_simulate_entity(cep);
        if (cep->p_data.flags & rf::PF_COLLIDE_WORLD) {
            rf::collide_object_world(cep);
        }
        rf::physics_update_entity(cep);
        // Commit the physics result to the object transform - obj_process_physics ends
        // its per-object sequence with exactly this (Object::move @ 0x0048A230 with
        // &p_data.pos); without it the entity's rendered position never changes
        cep->move(&cep->p_data.pos);
        cep->p_data.flags &= ~rf::PF_SIMULATED_THIS_FRAME;
        // Room tracking normally happens in a later obj_move_all pass - keep it in sync
        // ourselves so crossing rooms while paused doesn't break visibility culling
        // (same pattern as the dropped static cameras in multi_spectate_camera_do_frame)
        cep->set_room(nullptr);
        cep->update_room();
    }

    // While the world is frozen for demo pause the whole sim frame is skipped - object
    // movement, weapon fire regeneration, particles, corpse decay. Only the camera
    // system is kept alive so perspective switching, third-person orbit, static cams
    // and freelook flying keep working; input processing and rendering run outside
    // this function and are unaffected.
    FunHook<void()> gameplay_sim_frame_hook{
        0x00433260,
        []() {
            if (g_ctx.pause_fx_applied) {
                paused_freelook_camera_physics();
                rf::cameras_do_frame();
                return;
            }
            gameplay_sim_frame_hook.call_target();
        },
    };

    // The sim freeze does not stop render-side animation: player_fpgun_process/render
    // advance the first-person weapon meshes through vmesh_process with rf::frametime
    // directly from the render path. Zeroing the delta while frozen freeze-frames every
    // mesh animation without touching the render itself.
    FunHook<void(rf::VMesh*, float, int, rf::Vector3*, rf::Matrix3*, int)> vmesh_process_hook{
        0x00503360,
        [](rf::VMesh* vmesh, float frametime, int increment_only, rf::Vector3* pos, rf::Matrix3* orient,
           int lod_level) {
            if (g_ctx.pause_fx_applied) {
                frametime = 0.0f;
            }
            vmesh_process_hook.call_target(vmesh, frametime, increment_only, pos, orient, lod_level);
        },
    };

    // Rotating items spin in item_render (render path, not the sim), advancing their
    // angle by rf::frametime unless the engine's own pause flag is set. That flag can't
    // be set for demo pause (controls_read/controls_process gate ALL input on it - the
    // unpause key would stop working), so answer "paused" at this one call site instead.
    CallHook<bool()> item_render_game_is_paused_hook{
        0x0045906A,
        []() {
            return item_render_game_is_paused_hook.call_target() || g_ctx.pause_fx_applied;
        },
    };

    // Transient effects age on frametime/game time, which barely advances while records
    // are burst-fed - minutes worth of explosions, smoke, vclips, glass shards, geomod
    // rocks and burn fires all arrive "fresh" at the target and would pop on screen the
    // moment the overlay drops. Recycle them all while the settle window hides the frame.
    // Decals and geomod craters are deliberately NOT touched: they are persistent state
    // that legitimately exists at the seek target. Effects spawned moments before the
    // target are wiped too - they were about to expire and the overlay hides the gap.
    void cull_seek_effect_leftovers()
    {
        // First: destroys each live explosion's owned emitters and rebuilds the pool,
        // so the particle clear below never touches an already-freed emitter
        rf::explosion_shut_down();
        rf::vclip_level_release();
        rf::glass_shard_level_init();
        rf::geomod_debris_level_init();
        // entity_fire_destroy(fire, false) unlinks and advances the list head
        while (rf::entity_fire_list) {
            rf::entity_fire_destroy(rf::entity_fire_list, false);
        }
        for (auto& debris : DoublyLinkedList{rf::debris_list}) {
            rf::obj_flag_dead(&debris);
        }
        // Last: every remaining particle back to the pool (emitter records survive and
        // re-prime on their spawn timers)
        rf::particle_level_release();
        explosion_flash_lights_level_init();
    }

    // A burst that ends between a fire-ON and fire-OFF obj_update leaves the entity's
    // weapon latch stuck on - entity_process_post would keep firing it forever. Clear
    // every latch; a genuinely-firing entity is re-latched by the first normal-paced
    // obj_update, which is exactly what the settle window waits for.
    void cull_seek_fire_latches()
    {
        for (auto& entity : DoublyLinkedList{rf::entity_list}) {
            for (int weapon_type = 0; weapon_type < 64; ++weapon_type) {
                if (entity.ai.weapon_is_on[weapon_type]) {
                    rf::entity_turn_weapon_off(entity.handle, weapon_type);
                }
            }
        }
        multi_spectate_reset_action_anim_edge_state();
    }

    void seek_begin(double target_ms)
    {
        if (g_ctx.seek_target_ms < 0.0) {
            // Fresh seek (not a mid-seek retarget)
            g_ctx.seek_start_ms = g_ctx.clock_ms;
            if (!g_ctx.seek_audio_muted) {
                // Cutscene-skip mute pattern: silence everything currently audible and
                // keep snd_play* rejecting new sounds until the post-seek settle ends
                rf::snd_pause(true);
                rf::snd_stop_all_paused();
                set_sound_enabled(false);
                g_ctx.seek_audio_muted = true;
            }
        }
        g_ctx.seek_target_ms = target_ms;
        g_ctx.seek_settle = false;
    }

    void seek_finish()
    {
        g_ctx.clock_ms = g_ctx.seek_target_ms;
        if (g_ctx.duration_ms > 0) {
            g_ctx.clock_ms = std::min(g_ctx.clock_ms, static_cast<double>(g_ctx.duration_ms));
        }
        g_ctx.seek_target_ms = -1.0;
        // The interp rings hold pre-seek positions; clear so objects snap to the
        // post-seek state instead of lerping across the jump (teleport precedent)
        clear_all_object_interpolation();
        g_ctx.povcomp_applied_ms = 0.0f; // rings wiped - ramp the delay back up from zero
        cull_seek_death_leftovers();
        cull_seek_effect_leftovers();
        cull_seek_fire_latches();
        // Hold the overlay + mute until fresh obj_updates repopulate the rings, capped
        // by wall clock (the demo may be paused or at EOF and never deliver one)
        g_ctx.seek_settle = true;
        g_ctx.seek_obj_update_seen = false;
        g_ctx.seek_settle_deadline = timer::get_i64(1000) + 500;
        // Re-attach if the followed player was destroyed during the fast-forward
        if (g_ctx.cam_want_attached && !multi_spectate_is_following_player()) {
            begin_attach_retry();
        }
    }

    // Starts a playback session for an already validated reader: hand off to the stock
    // join sequence (same machinery as -url direct connect). It navigates main menu ->
    // multi menu -> server list, then multi_join_game() marks the join pending against
    // our synthetic address and sends a join_req (swallowed by the psnet gate). The
    // recorded join_accept is fed once that pending state is reached - from there the
    // server-list state processing drives the level load exactly like a live join.
    //
    // already_in_server_list: the deferred backward-seek restart runs from the
    // server-list state init - join directly like the jump machinery does at that
    // point. Forcing a menu re-init from inside a state init corrupts the gameseq
    // flow (crash on backward seek).
    void session_start(double seek_ms, bool already_in_server_list)
    {
        g_ctx.state = PlaybackState::joining; // activates the outbound gate
        set_jump_to_demo_browser(false);      // a new session supersedes any armed exit navigation
        g_ctx.pending_seek_ms = seek_ms;
        rf::set_in_mp_flag(); // in_mp_game: makes multi_do_frame/our pump run every frame
        if (already_in_server_list) {
            multi_join_game(demo_playback_addr, {});
        }
        else {
            start_join_multi_game_sequence(demo_playback_addr, {});
            // The jump machinery only runs from rf_init_state; force a main-menu re-init
            // to trigger it from an idle menu (same kick the console `levelm` command uses)
            rf::gameseq_set_state(rf::GS_MAIN_MENU, true);
        }
    }

    // The recorded server's config gates client graphical options (outlines, fullbright
    // meshes, lightmaps-only, screenshake/muzzle-flash toggles, FPS/FOV caps, footsteps).
    // A demo viewer is not a competitor - permit them all. Runs after the recorded
    // join_accept populated g_af_server_info and before the level loads, so the
    // level-init evaluations pick the permissive flags up naturally.
    void ungate_client_options()
    {
        auto& info = get_af_server_info_mutable();
        if (!info) {
            return;
        }
        info->allow_fb_mesh = true;
        info->allow_lmap = true;
        info->allow_no_ss = true;
        info->allow_no_mf = true;
        info->unlimited_fps = true;
        info->allow_footsteps = true;
        info->allow_outlines = true;
        info->allow_outlines_xray = true;
        info->max_fov.reset(); // no FOV cap while watching
        // The only gated option evaluated before this point (in the join_accept injection)
        evaluate_footsteps();
    }

    // In the joining state with the pending-join guards satisfied: feed the recorded
    // join_accept. Its handler runs level_set_level_to_load and multi_start(1, addr)
    // (the reliable connect inside is faked by psnet_rel_connect_to_server_hook).
    void feed_join_accept()
    {
        while (ensure_pending_record()) {
            if (g_ctx.pending.is_packet() && g_ctx.pending.packet_type() == pkt_join_accept) {
                g_ctx.state = PlaybackState::waiting_for_level;
                feed_one_packet(g_ctx.pending.packet_data(), g_ctx.pending.packet_len());
                consume_pending_record();
                if (!rf::is_multi) {
                    // The handler rejected the packet (unsupported game type popup etc.)
                    rf::console::print("Demo playback failed to start");
                    playback_teardown();
                    return;
                }
                ungate_client_options();
                return;
            }
            consume_pending_record();
        }
        rf::console::print("Demo file contains no join_accept packet - cannot play");
        playback_teardown();
    }

    bool demo_play_start(const std::string& path, double seek_ms, bool already_in_server_list = false)
    {
        auto result = g_ctx.reader.open(path);
        using OpenResult = DemoFileReader::OpenResult;
        switch (result) {
        case OpenResult::cant_open:
            rf::console::print("Cannot open demo file: {}", path);
            return false;
        case OpenResult::bad_magic:
            rf::console::print("Not an Alpine demo file: {}", path);
            return false;
        case OpenResult::newer_format:
        case OpenResult::missing_features:
            rf::console::print("This demo was recorded by a newer Alpine Faction version - cannot play");
            g_ctx.reader.close();
            return false;
        case OpenResult::bad_header:
            rf::console::print("Demo file header is corrupted: {}", path);
            g_ctx.reader.close();
            return false;
        case OpenResult::ok:
            break;
        }

        const auto& header = g_ctx.reader.header();
        if (header.level_filename.empty()) {
            rf::console::print("Cannot play demo: the file does not name its level");
            g_ctx.reader.close();
            return false;
        }
        // Belt and braces for a future game mode that forgot to allocate a required_features
        // bit: an unknown netgame type would ride the join flow into undefined UI/scoring
        if (header.game_type < 0 || header.game_type >= rf::NG_TYPE_UNK) {
            rf::console::print("This demo was recorded by a newer Alpine Faction version - cannot play");
            g_ctx.reader.close();
            return false;
        }
        // A missing level is not fatal: playback rides the stock join flow, whose
        // level-load step (game_new_game_gameseq_set_next_state_hook) autodownloads
        // missing levels from FactionFiles exactly like a live join would.
        rf::File level_file;
        if (!level_file.find(header.level_filename.c_str())) {
            rf::console::print("Level {} is not installed - trying to download it from FactionFiles",
                               header.level_filename);
        }

        // Pre-scan: harvest the footer (duration) and count skippable packet types.
        uint32_t records = 0;
        uint32_t non_whitelisted = 0;
        uint32_t last_t_ms = 0;
        DemoRecord rec;
        while (g_ctx.reader.next_record(rec)) {
            ++records;
            last_t_ms = rec.t_ms;
            if (rec.is_packet() && !packet_check_whitelist(rec.packet_type()))
                ++non_whitelisted;
        }
        if (records == 0) {
            rf::console::print("Demo file contains no records: {}", path);
            g_ctx.reader.close();
            return false;
        }
        if (non_whitelisted > 0) {
            rf::console::print("Demo contains {} packets of unsupported types - they will be skipped",
                               non_whitelisted);
        }
        g_ctx.duration_ms = g_ctx.reader.has_footer() ? g_ctx.reader.footer().duration_ms : last_t_ms;
        if (!g_ctx.reader.has_footer()) {
            rf::console::print(
                "Demo file has no end marker (still recording, or the server was stopped mid-recording) - "
                "playing anyway; the duration shown may grow");
        }
        if (!g_ctx.reader.rewind_to_records()) {
            rf::console::print("Failed to re-open demo file: {}", path);
            return false;
        }

        auto slash_pos = path.find_last_of("\\/");
        g_ctx.display_name = slash_pos == std::string::npos ? path : path.substr(slash_pos + 1);
        rf::console::print("Playing demo {} ({}, {}:{:02})", g_ctx.display_name, header.level_filename,
                           g_ctx.duration_ms / 60000, g_ctx.duration_ms / 1000 % 60);
        session_start(seek_ms, already_in_server_list);
        return true;
    }

    // While a demo session owns the client, the engine must never talk to the network:
    // join_req retries, state_info_request spam, client_in_game, obj_update, chat and
    // every AF client packet all funnel through this single sendto wrapper.
    FunHook<int(void*, unsigned, int, void*, int)> psnet_send_internal_hook{
        0x00528820,
        [](void* data, unsigned len, int a3, void* addr, int type) -> int {
            if (demo_playback_active()) {
                return static_cast<int>(len) + 1; // pretend the datagram was sent
            }
            return psnet_send_internal_hook.call_target(data, len, a3, addr, type);
        },
    };

    // Client-side reliable-socket health check: the replayed session never completes a
    // reliable handshake, so the socket (if any) would eventually read as timed out and
    // trigger a disconnect. Skip it entirely during playback.
    FunHook<void()> multi_check_socket_status_hook{
        0x0046E980,
        []() {
            if (demo_playback_active())
                return;
            multi_check_socket_status_hook.call_target();
        },
    };

    // Stock psnet_rel_connect_to_server is a SYNCHRONOUS blocking loop: it Sleep(10)-spins
    // waiting for a connect-ack with a 60 second timeout and no frame pump. During playback
    // nothing can answer (the psnet gate swallows the connect packets), so it would freeze
    // the client for a minute and then fail the join (socket stays -1, join-failed popup).
    // Fabricate an already-connected reliable socket instead, mirroring the fields the
    // stock success path fills in.
    FunHook<void(int*, rf::NetAddr*)> psnet_rel_connect_to_server_hook{
        0x0052A4B0,
        [](int* socket_out, rf::NetAddr* addr) {
            if (!demo_playback_active()) {
                psnet_rel_connect_to_server_hook.call_target(socket_out, addr);
                return;
            }
            const int slot = demo_alloc_fake_reliable_socket(*addr);
            if (slot < 0) {
                return; // socket stays -1; join_accept handler will show join-failed
            }
            rf::net_rel_last_connect_socket = slot;
            *socket_out = slot;
        },
    };

    // ---- POV ping compensation ("povcomp") ----
    //
    // The demo stream is on the server's timeline: the followed player's aim samples and
    // their fire/kill events are mutually consistent (both arrived ~ping/2 late), but the
    // world they were aiming at is displayed ~(ping + their client's interp delay) ahead
    // of what they actually saw. Realign by evaluating every other entity's interp ring
    // that far in the past while following a player, so the crosshair lines up with
    // targets the way the shooter saw them - the same rewind the server's lag
    // compensation applied when it validated their hits. The ring holds real recorded
    // history (20 keyframes, ~633ms at 30 netfps), so this stays smooth, unlike the
    // considered alternative of extrapolating the POV camera forward by ping, which
    // predicts beyond the recorded data and jitters exactly where the viewer is looking.
    // Projectiles/corpses/movers are not biased: tracers must leave the (un-delayed) POV
    // muzzle, and alignment only matters against players.

    bool g_povcomp_enabled = true;
    int g_povcomp_override_ms = -1; // >= 0: fixed delay instead of ping-derived

    constexpr int povcomp_max_ms = 450; // interp ring depth bounds usable delay anyway
    constexpr float povcomp_slew_ms_per_s = 300.0f;

    // Downstream latency to the followed player + their client's interp buffer.
    // ObjInterp::set_next_pos_orient anchors interp_time at 2.2x the average
    // sample-arrival interval behind the newest keyframe (flt_59F50C), so the
    // recorded client viewed remote entities ~ping + 2.2 * update interval in
    // the past relative to the server timeline the demo is recorded on.
    constexpr float povcomp_interp_headroom = 2.2f;

    int povcomp_desired_ms()
    {
        if (!g_povcomp_enabled || g_ctx.state != PlaybackState::playing || demo_playback_is_seeking())
            return 0;
        if (!g_ctx.spectate_entered || !multi_spectate_is_following_player())
            return 0;
        rf::Player* target = multi_spectate_get_target_player();
        if (!target || !target->net_data)
            return 0;
        int desired = g_povcomp_override_ms;
        if (desired < 0) {
            const uint32_t netfps = std::max(g_ctx.reader.header().server_netfps, 1u);
            const float interval_ms = 1000.0f / static_cast<float>(netfps);
            desired = target->net_data->ping + static_cast<int>(povcomp_interp_headroom * interval_ms);
        }
        return std::clamp(desired, 0, povcomp_max_ms);
    }

    // Slew the applied delay so the world glides instead of popping when the followed
    // target (and thus ping) changes, a netgame_update revises the ping, or the mode
    // is toggled
    void povcomp_do_frame()
    {
        const auto desired = static_cast<float>(povcomp_desired_ms());
        const float step = povcomp_slew_ms_per_s * rf::frametime;
        if (g_ctx.povcomp_applied_ms < desired) {
            g_ctx.povcomp_applied_ms = std::min(g_ctx.povcomp_applied_ms + step, desired);
        }
        else {
            g_ctx.povcomp_applied_ms = std::max(g_ctx.povcomp_applied_ms - step, desired);
        }
    }

    // Clamp the bias so the biased evaluation time stays within the ring's recorded
    // span. interp_time and time_array are 16-bit server ms ticks; blind subtraction
    // near a numeric wrap would read as ~65s in the future and trip determine_frame's
    // 5000ms staleness cutoff. time_array[0] is always the oldest sample (insertion
    // shifts the arrays down when full). Returns 0 when biasing is unsafe this frame.
    uint16_t povcomp_safe_bias(rf::ObjInterp* interp, int desired_ms)
    {
        // flags bit 0 = ring empty/unanchored: set by Clear(), cleared once a sample
        // anchors interp_time (frame_start skips processing while it is set)
        if ((interp->flags & 1) != 0 || interp->num < 2)
            return 0; // ring unusable; the stock path holds the current pos anyway
        const auto avail = static_cast<uint16_t>(interp->interp_time - interp->time_array[0]);
        if (avail > 0x1388)
            return 0; // stale/wrapped ring - leave it to the stock staleness handling
        return static_cast<uint16_t>(std::min<int>(desired_ms, avail));
    }

    int povcomp_bias_for(rf::Entity* entity)
    {
        if (!demo_playback_active() || g_ctx.povcomp_applied_ms < 1.0f)
            return 0;
        // Resolved fresh per call: target switches take effect instantly and a dead
        // target (entity_handle resolving to nothing) just leaves the whole world
        // coherently delayed
        rf::Player* target = multi_spectate_get_target_player();
        if (target && entity->handle == target->entity_handle)
            return 0; // the POV entity stays on demo time
        return static_cast<int>(g_ctx.povcomp_applied_ms);
    }

    void povcomp_interp_call(rf::Entity* entity, auto& hook)
    {
        rf::ObjInterp* interp = entity->obj_interp;
        const int desired = interp ? povcomp_bias_for(entity) : 0;
        const uint16_t bias = desired > 0 ? povcomp_safe_bias(interp, desired) : 0;
        if (bias == 0) {
            hook.call_target(entity);
            return;
        }
        // Save/call/restore keeps every other consumer of interp_time (frame_start
        // progression, sample-insertion staleness, lag comp) seeing the true value
        const uint16_t saved = interp->interp_time;
        interp->interp_time = static_cast<uint16_t>(saved - bias);
        hook.call_target(entity);
        interp->interp_time = saved;
    }

    // The two evaluation entry points physics_simulate_entity calls for every remote
    // entity with the network-interpolated physics flag - exactly the player entities
    FunHook<void(rf::Entity*)> multi_obj_interp_orient_hook{
        0x00484650,
        [](rf::Entity* entity) { povcomp_interp_call(entity, multi_obj_interp_orient_hook); },
    };

    FunHook<void(rf::Entity*)> multi_obj_interp_pos_hook{
        0x00484770,
        [](rf::Entity* entity) { povcomp_interp_call(entity, multi_obj_interp_pos_hook); },
    };

    ConsoleCommand2 demo_povcomp_cmd{
        "demo_povcomp",
        [](std::optional<std::string> arg) {
            if (arg) {
                if (*arg == "on" || *arg == "auto") {
                    g_povcomp_enabled = true;
                    g_povcomp_override_ms = -1;
                }
                else if (*arg == "off") {
                    g_povcomp_enabled = false;
                }
                else {
                    int value = 0;
                    auto [ptr, ec] = std::from_chars(arg->data(), arg->data() + arg->size(), value);
                    if (ec != std::errc{} || ptr != arg->data() + arg->size()) {
                        rf::console::print("Usage: demo_povcomp [on|off|<delay ms>]");
                        return;
                    }
                    g_povcomp_enabled = true;
                    g_povcomp_override_ms = std::clamp(value, 0, povcomp_max_ms);
                }
            }
            if (!g_povcomp_enabled) {
                rf::console::print("Demo POV ping compensation: off");
            }
            else if (g_povcomp_override_ms >= 0) {
                rf::console::print("Demo POV ping compensation: on (override {} ms)", g_povcomp_override_ms);
            }
            else {
                rf::console::print("Demo POV ping compensation: on (auto, currently ~{} ms)",
                                   static_cast<int>(g_ctx.povcomp_applied_ms));
            }
        },
        "Ping compensation while following a player in demo playback - delays other players "
        "to match what the followed player saw when they aimed",
        "demo_povcomp [on|off|<delay ms>]",
    };

    std::optional<double> parse_seek_target(const std::string& arg)
    {
        if (arg.empty())
            return std::nullopt;
        // mm:ss
        if (auto colon = arg.find(':'); colon != std::string::npos) {
            int minutes = 0;
            int seconds = 0;
            auto [p1, ec1] = std::from_chars(arg.data(), arg.data() + colon, minutes);
            auto [p2, ec2] = std::from_chars(arg.data() + colon + 1, arg.data() + arg.size(), seconds);
            if (ec1 != std::errc{} || ec2 != std::errc{})
                return std::nullopt;
            return (minutes * 60.0 + seconds) * 1000.0;
        }
        // +N / -N relative seconds, N absolute seconds
        double seconds = 0.0;
        const bool relative = arg[0] == '+' || arg[0] == '-';
        const char* begin = arg[0] == '+' ? arg.data() + 1 : arg.data();
        auto [ptr, ec] = std::from_chars(begin, arg.data() + arg.size(), seconds);
        if (ec != std::errc{})
            return std::nullopt;
        if (relative)
            return g_ctx.clock_ms + seconds * 1000.0;
        return seconds * 1000.0;
    }

    ConsoleCommand2 demo_play_cmd{
        "demo_play",
        [](std::string filename) {
            if (g_ctx.state != PlaybackState::inactive) {
                rf::console::print("A demo is already playing - use demo_stop first");
                return;
            }
            if (rf::is_multi) {
                rf::console::print("Cannot play a demo while in a multiplayer game - disconnect first");
                return;
            }
            g_pending_restart.reset(); // an explicit demo_play supersedes a queued restart
            demo_play_start(demo_file_resolve_path(filename), -1.0);
        },
        "Play a demo file from the demos folder",
        "demo_play <filename>",
    };

    ConsoleCommand2 demo_stop_cmd{
        "demo_stop",
        []() {
            if (g_ctx.state == PlaybackState::inactive && !g_pending_restart) {
                rf::console::print("No demo is playing");
                return;
            }
            g_pending_restart.reset(); // cancel a queued backward-seek restart
            // Deferred to the pump (networking phase) - see teardown_queued
            g_ctx.teardown_queued = true;
            rf::console::print("Demo playback stopped");
        },
        "Stop demo playback",
    };

    ConsoleCommand2 demo_pause_cmd{
        "demo_pause",
        []() {
            if (g_ctx.state == PlaybackState::inactive) {
                rf::console::print("No demo is playing");
                return;
            }
            g_ctx.paused = !g_ctx.paused;
            rf::console::print("Demo playback {}", g_ctx.paused ? "paused" : "resumed");
        },
        "Pause/resume demo playback",
    };

    ConsoleCommand2 demo_timescale_cmd{
        "demo_timescale",
        [](std::optional<float> scale) {
            if (scale) {
                g_ctx.timescale = std::clamp(*scale, 0.05f, 10.0f);
            }
            rf::console::print("Demo timescale: {:.2f}", g_ctx.timescale);
        },
        "Set demo playback speed multiplier",
        "demo_timescale [0.05-10]",
    };

    ConsoleCommand2 demo_seek_cmd{
        "demo_seek",
        [](std::string target) {
            if (g_ctx.state != PlaybackState::playing && g_ctx.state != PlaybackState::finished) {
                rf::console::print("No demo is playing");
                return;
            }
            auto target_ms = parse_seek_target(target);
            if (!target_ms || *target_ms < 0.0) {
                rf::console::print("Usage: demo_seek <mm:ss | seconds | +/-seconds>");
                return;
            }
            demo_playback_request_seek(*target_ms);
        },
        "Seek within the playing demo (backward seek reloads the level)",
        "demo_seek <mm:ss | seconds | +/-seconds>",
    };

    ConsoleCommand2 demo_info_cmd{
        "demo_info",
        [](std::string filename) {
            DemoFileReader reader;
            auto path = demo_file_resolve_path(filename);
            const auto open_result = reader.open(path);
            if (open_result != DemoFileReader::OpenResult::ok
                && open_result != DemoFileReader::OpenResult::missing_features) {
                rf::console::print("Cannot parse demo file: {}", path);
                return;
            }
            // Consume records to reach the footer (streams cannot seek)
            uint32_t records = 0;
            uint32_t last_t_ms = 0;
            DemoRecord rec;
            while (reader.next_record(rec)) {
                ++records;
                last_t_ms = rec.t_ms;
            }
            const auto& header = reader.header();
            rf::console::print("Demo file: {}", path);
            rf::console::print("  Format: {}.{}, recorded by Alpine Faction {}.{}.{}", header.format_major,
                               header.format_minor, header.af_version_major, header.af_version_minor,
                               header.af_version_patch);
            if (open_result == DemoFileReader::OpenResult::missing_features) {
                rf::console::print("  Playback requires a newer Alpine Faction version (features 0x{:08X})",
                                   header.required_features);
            }
            rf::console::print("  Server: {}", header.server_name);
            rf::console::print("  Level: {} (game type {})", header.level_filename, header.game_type);
            if (!header.mod_name.empty()) {
                rf::console::print("  Mod: {}", header.mod_name);
            }
            rf::console::print("  Net FPS: {}, max players: {}", header.server_netfps, header.server_max_players);
            const uint32_t duration = reader.has_footer() ? reader.footer().duration_ms : last_t_ms;
            rf::console::print("  Duration: {}:{:02} ({} packets{})", duration / 60000, duration / 1000 % 60,
                               records, reader.has_footer() ? "" : ", no end marker - unclean recording");
        },
        "Print information about a demo file",
        "demo_info <filename>",
    };
}

bool demo_playback_active()
{
    return g_ctx.state != PlaybackState::inactive;
}

DemoRecordedAfVersion demo_playback_recorded_af_version()
{
    if (!g_ctx.reader.is_open()) {
        return {};
    }
    const auto& header = g_ctx.reader.header();
    return {header.af_version_major, header.af_version_minor, header.af_version_patch};
}

bool demo_playback_is_seeking()
{
    return g_ctx.seek_target_ms >= 0.0 || g_ctx.seek_settle;
}

bool demo_playback_in_seek_burst()
{
    return g_ctx.seek_target_ms >= 0.0;
}

double demo_playback_clock_ms()
{
    return std::max(g_ctx.clock_ms, 0.0);
}

uint32_t demo_playback_duration_ms()
{
    return g_ctx.duration_ms;
}

bool demo_playback_paused()
{
    return g_ctx.paused;
}

bool demo_playback_sim_frozen()
{
    return g_ctx.pause_fx_applied;
}

bool demo_playback_finished()
{
    return g_ctx.state == PlaybackState::finished;
}

void demo_playback_stop()
{
    if (g_ctx.state == PlaybackState::inactive) {
        return;
    }
    g_pending_restart.reset();
    // Deferred to the pump (networking phase) - see teardown_queued
    g_ctx.teardown_queued = true;
}

void demo_playback_toggle_pause()
{
    if (g_ctx.state != PlaybackState::playing)
        return;
    g_ctx.paused = !g_ctx.paused;
}

bool demo_playback_can_seek()
{
    return g_ctx.state == PlaybackState::playing || g_ctx.state == PlaybackState::finished;
}

bool demo_playback_restart_pending()
{
    return g_pending_restart.has_value();
}

void demo_playback_request_seek(double target_ms)
{
    if (!demo_playback_can_seek())
        return;
    target_ms = std::max(target_ms, 0.0);
    capture_camera_memory(); // seeks restore the camera the user last had
    if (target_ms >= g_ctx.clock_ms && g_ctx.state == PlaybackState::playing) {
        // Forward seek: hand the target to the pump (networking phase). Running
        // the fast-forward here - inside gameplay_do_frame - fed packets in a
        // frame phase their handlers don't expect (crash class documented in the
        // PRD progress log). The pump chews it in wall-clock budgeted chunks.
        g_ctx.pending_seek_ms = target_ms;
    }
    else {
        // Backward seek (or seek after EOF): reload the level by restarting the
        // session from the file start, then fast-forward to the target. Teardown
        // is deferred to the pump; the restart to demo_playback_on_state_init.
        g_pending_restart = PendingRestart{
            g_ctx.reader.path(), target_ms, g_ctx.timescale, g_ctx.paused,
            g_ctx.cam_want_attached, g_ctx.cam_third_person, g_ctx.cam_target_id,
            g_ctx.cam_last_active_id, g_ctx.cam_free_valid, g_ctx.cam_free_pos,
            g_ctx.cam_free_orient, g_ctx.cam_free_phb, g_ctx.cam_free_eye_phb};
        g_ctx.teardown_queued = true;
        rf::console::print("Seeking backward - reloading the level...");
    }
}

void demo_playback_note_player_activity(rf::Player* player)
{
    if (g_ctx.state == PlaybackState::inactive)
        return;
    if (!player || player == rf::local_player || player->is_browser || !player->net_data)
        return;
    g_ctx.cam_last_active_id = player->net_data->player_id;
}

void demo_playback_do_frame()
{
    if (g_ctx.state == PlaybackState::inactive)
        return;

    // Deferred demo_stop / backward-seek teardown: running it here - the networking
    // phase, before rf_do_state dispatches - means the forced END_GAME takes effect
    // for this whole frame and nothing renders the released character data
    if (g_ctx.teardown_queued) {
        playback_teardown();
        return;
    }

    // No live server feeds obj_updates, so the stock broken-connection machinery
    // (icon, sim pause, 20s timeout popup) must be kept quiet - critical while paused
    rf::multi_connection_broken_timestamp.set(450);

    // Keep the fabricated reliable socket looking alive so no timeout path ever fires
    if (rf::local_player && rf::local_player->net_data) {
        const int socket_id = static_cast<int>(rf::local_player->net_data->reliable_socket);
        if (socket_id >= 0 && socket_id < rf::NET_MAX_REL_SOCKETS) {
            rf::net_rel_sockets[socket_id].last_packet_received = static_cast<int>(timer::get_i64(1000));
        }
    }

    // End the post-seek settle window once fresh position updates arrived - or on the
    // wall-clock cap / leaving the playing state: restore audio, drop the seek overlay
    if (g_ctx.seek_settle
        && (g_ctx.seek_obj_update_seen || timer::get_i64(1000) >= g_ctx.seek_settle_deadline
            || g_ctx.state != PlaybackState::playing)) {
        g_ctx.seek_settle = false;
        seek_restore_audio();
    }

    // After the settle check so a settle that ends this frame re-freezes this frame
    pause_fx_update();

    switch (g_ctx.state) {
    case PlaybackState::joining:
        // The stock join navigation has arrived and marked the join pending - answer
        // its join_req with the recorded join_accept
        if (rf::gameseq_get_state() == rf::GS_MULTI_SERVER_LIST && rf::multi_join_game_is_connecting()) {
            xlog::info("Demo playback: join pending, feeding recorded join_accept");
            feed_join_accept();
        }
        break;

    case PlaybackState::waiting_for_level:
        // Phase 1: feed the pregame reliable stream up through the players packet -
        // processing it is what makes the client run game_new_game and enter
        // GS_NEW_LEVEL (multi_after_players_packet @ 0x00482080). Without this the
        // client would sit in the server list forever.
        if (!g_ctx.players_fed) {
            while (g_ctx.state == PlaybackState::waiting_for_level && ensure_pending_record()) {
                const bool is_players = is_final_players_packet(g_ctx.pending);
                const bool fed = feed_packet_record(g_ctx.pending, false);
                consume_pending_record();
                // Only advance once the engine actually processed the roster - a
                // mis-framed players record feed_one_packet dropped keeps the pump
                // searching for a real final chunk instead of wedging pregame (N5).
                if (is_players && fed) {
                    g_ctx.players_fed = true;
                    xlog::info("Demo playback: player roster fed, level load should start");
                    break;
                }
            }
            if (g_ctx.state == PlaybackState::waiting_for_level && !g_ctx.players_fed
                && !ensure_pending_record()) {
                rf::console::print("Demo file ends before the player roster - cannot play");
                playback_teardown();
            }
            break;
        }
        // Phase 2: the level has loaded and the engine is asking for state info
        if (rf::gameseq_get_state() == rf::GS_MULTI_GETTING_STATE_INFO) {
            g_ctx.state = PlaybackState::feeding_state_info;
            // Feed the pregame stream (players, state info) in one go, like a live
            // server answering state_info_request. A fed handler can in principle end
            // the session (multi_stop -> reset_ctx), so re-check the state per record.
            while (g_ctx.state == PlaybackState::feeding_state_info && ensure_pending_record()) {
                const DemoRecord& rec = g_ctx.pending;
                const bool is_done_marker = rec.is_packet() && rec.packet_type() == pkt_state_info_done;
                const bool fed = feed_packet_record(rec, false);
                g_ctx.clock_ms = rec.t_ms;
                consume_pending_record();
                // Only complete the handshake if the marker was actually processed; a
                // mis-framed state_info_done feed_one_packet dropped must not fake it (N5).
                if (is_done_marker && fed) {
                    g_ctx.state = PlaybackState::playing;
                    xlog::info("Demo playback: state info complete, entering playback");
                    break;
                }
            }
            if (g_ctx.state == PlaybackState::feeding_state_info) {
                // EOF before state_info_done - nothing more to feed
                g_ctx.state = PlaybackState::finished;
            }
        }
        break;

    case PlaybackState::playing: {
        if (g_ctx.pending_seek_ms >= 0.0 && rf::gameseq_in_gameplay()) {
            seek_begin(g_ctx.pending_seek_ms);
            g_ctx.pending_seek_ms = -1.0;
        }
        if (!g_ctx.spectate_entered && rf::gameseq_in_gameplay()) {
            multi_spectate_enter_freelook();
            g_ctx.spectate_entered = true;
            if (g_ctx.cam_want_attached) {
                begin_attach_retry();
            }
            else {
                // Freelook survived a session restart - put the camera back
                apply_freelook_camera_memory();
            }
        }
        if (g_ctx.seek_target_ms >= 0.0) {
            // Budgeted fast-forward: feed records for a slice of wall clock, then yield
            // so the frame presents (seek overlay) and input stays responsive
            const int64_t budget_end = timer::get_i64(1000) + 10;
            while (g_ctx.state == PlaybackState::playing && ensure_pending_record()
                   && static_cast<double>(g_ctx.pending.t_ms) <= g_ctx.seek_target_ms) {
                g_ctx.clock_ms = static_cast<double>(g_ctx.pending.t_ms); // progress display
                if (!g_ctx.reader.has_footer() && g_ctx.pending.t_ms > g_ctx.duration_ms) {
                    g_ctx.duration_ms = g_ctx.pending.t_ms; // see paced feed below
                }
                feed_packet_record(g_ctx.pending, true);
                consume_pending_record();
                if (timer::get_i64(1000) >= budget_end) {
                    break;
                }
            }
            if (g_ctx.state != PlaybackState::playing) {
                break; // a fed handler ended the session
            }
            if (ensure_pending_record() && static_cast<double>(g_ctx.pending.t_ms) <= g_ctx.seek_target_ms) {
                break; // budget exhausted - resume next frame (no clock advance / paced feed)
            }
            seek_finish(); // target reached or EOF
        }
        cam_do_frame();
        povcomp_do_frame();
        if (!g_ctx.paused) {
            g_ctx.clock_ms += static_cast<double>(rf::frametime) * 1000.0 * g_ctx.timescale;
        }
        while (g_ctx.state == PlaybackState::playing && ensure_pending_record()
               && static_cast<double>(g_ctx.pending.t_ms) <= g_ctx.clock_ms) {
            // Footerless file: the pre-scan duration is a snapshot, and the rewind
            // reopened the file - a demo still being recorded can outgrow it. Keep
            // the reported total honest as later records stream in.
            if (!g_ctx.reader.has_footer() && g_ctx.pending.t_ms > g_ctx.duration_ms) {
                g_ctx.duration_ms = g_ctx.pending.t_ms;
            }
            feed_packet_record(g_ctx.pending, false);
            consume_pending_record();
        }
        if (g_ctx.state == PlaybackState::playing && !ensure_pending_record()) {
            g_ctx.state = PlaybackState::finished;
        }
        break;
    }

    case PlaybackState::finished:
        // Stay in spectate on the final state (limbo scoreboard); HUD shows "demo ended"
        break;

    default:
        break;
    }

    if (g_ctx.state == PlaybackState::playing || g_ctx.state == PlaybackState::finished) {
        // After the feeds/seek so the timer reflects this frame's final clock; also
        // freezes it once the demo ended while the user lingers on the last frame
        sync_level_timer();
    }
}

void demo_playback_render()
{
    if (g_ctx.state == PlaybackState::inactive || rf::is_dedicated_server)
        return;
    if (g_ctx.state != PlaybackState::playing && g_ctx.state != PlaybackState::finished)
        return;
    if (g_alpine_game_config.spectate_mode_minimal_ui)
        return;

    const uint32_t total_ms = g_ctx.duration_ms;
    const auto clock_ms = static_cast<uint32_t>(std::max(g_ctx.clock_ms, 0.0));
    std::string text = std::format("DEMO {}  {}:{:02} / {}:{:02}", g_ctx.display_name, clock_ms / 60000,
                                   clock_ms / 1000 % 60, total_ms / 60000, total_ms / 1000 % 60);
    if (g_ctx.state == PlaybackState::finished) {
        text += "  [ended - demo_stop to exit]";
    }
    else if (g_ctx.paused) {
        text += "  [paused]";
    }
    else if (g_ctx.timescale != 1.0f) {
        text += std::format("  [x{:.2f}]", g_ctx.timescale);
    }
    if (g_ctx.povcomp_applied_ms >= 1.0f) {
        text += std::format("  [povcomp ~{}ms]", static_cast<int>(g_ctx.povcomp_applied_ms));
    }
    rf::gr::set_color(255, 255, 255, 225);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, rf::gr::screen_width() / 2, 5, text.c_str(),
                           hud_get_default_font());
}

// Same shape as the level download progress bar (multi/level_download.cpp - not exported)
static void render_seek_progress_bar(int x, int y, int w, int h, float progress)
{
    const int border = 2;
    const int inner_w = w - 2 * border;
    const int inner_h = h - 2 * border;
    const int progress_w = std::clamp(static_cast<int>(static_cast<float>(inner_w) * progress), 0, inner_w);

    rf::gr::set_color(0x40, 0x40, 0x40, 0xFF);
    rf::gr::rect(x, y, w, h);
    if (progress_w > 0) {
        rf::gr::set_color(0, 0x80, 0, 0xFF);
        rf::gr::rect(x + border, y + border, progress_w, inner_h);
    }
    if (progress_w < inner_w) {
        rf::gr::set_color(0, 0, 0, 0xFF);
        rf::gr::rect(x + border + progress_w, y + border, inner_w - progress_w, inner_h);
    }
}

void demo_playback_render_seek_overlay()
{
    if (rf::is_dedicated_server || !demo_playback_is_seeking())
        return;

    const int scr_w = rf::gr::screen_width();
    const int scr_h = rf::gr::screen_height();

    // The world render is skipped while seeking, so the backbuffer holds stale data -
    // cover the whole frame
    rf::gr::set_color(0, 0, 0, 255);
    rf::gr::rect(0, 0, scr_w, scr_h);

    rf::gr::set_color(255, 255, 255, 255);
    const int title_y = scr_h / 2 - static_cast<int>(40.0f * rf::ui::scale_y);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, scr_w / 2, title_y, "SEEKING...", rf::ui::large_font);

    const bool settling = g_ctx.seek_target_ms < 0.0; // target reached, waiting for fresh updates
    const double target_ms = settling ? g_ctx.clock_ms : g_ctx.seek_target_ms;
    const auto clock = static_cast<uint32_t>(std::max(g_ctx.clock_ms, 0.0));
    const auto target = static_cast<uint32_t>(std::max(target_ms, 0.0));
    std::string time_text = std::format("{}:{:02} / {}:{:02}", clock / 60000, clock / 1000 % 60, target / 60000,
                                        target / 1000 % 60);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, scr_w / 2, scr_h / 2, time_text.c_str(), hud_get_default_font());

    float progress = 1.0f; // full during settle
    if (!settling) {
        const double span = std::max(1.0, g_ctx.seek_target_ms - g_ctx.seek_start_ms);
        progress = static_cast<float>(std::clamp((g_ctx.clock_ms - g_ctx.seek_start_ms) / span, 0.0, 1.0));
    }
    const int bar_w = scr_w / 3;
    const int bar_h = std::max(10, static_cast<int>(12.0f * rf::ui::scale_y));
    const int bar_y = scr_h / 2 + static_cast<int>(30.0f * rf::ui::scale_y);
    render_seek_progress_bar((scr_w - bar_w) / 2, bar_y, bar_w, bar_h, progress);
}

void demo_playback_on_state_init(int state)
{
    // Perform a queued backward-seek restart once Alpine's exiting-game navigation has
    // landed in the server browser (guaranteed after the forced END_GAME teardown while
    // g_in_mp_game is set). Joining directly from this state init matches exactly what
    // the jump machinery does here for -url connects - no recursive state re-init.
    if (!g_pending_restart || g_ctx.state != PlaybackState::inactive) {
        return;
    }
    if (state != rf::GS_MULTI_SERVER_LIST) {
        return;
    }
    // Take the request first - demo_play_start re-enters playback state
    const PendingRestart restart = *g_pending_restart;
    g_pending_restart.reset();
    if (demo_play_start(restart.path, restart.seek_ms, true)) {
        g_ctx.timescale = restart.timescale;
        g_ctx.paused = restart.paused;
        g_ctx.cam_want_attached = restart.cam_want_attached;
        g_ctx.cam_third_person = restart.cam_third_person;
        g_ctx.cam_target_id = restart.cam_target_id;
        g_ctx.cam_last_active_id = restart.cam_last_active_id;
        g_ctx.cam_free_valid = restart.cam_free_valid;
        g_ctx.cam_free_pos = restart.cam_free_pos;
        g_ctx.cam_free_orient = restart.cam_free_orient;
        g_ctx.cam_free_phb = restart.cam_free_phb;
        g_ctx.cam_free_eye_phb = restart.cam_free_eye_phb;
    }
}

void demo_playback_on_multi_stop()
{
    // multi_stop initiated outside playback_teardown (user disconnect, engine error):
    // drop the session state so the gates deactivate and the client can join real
    // servers again.
    if (g_ctx.state != PlaybackState::inactive && !g_ctx.in_teardown) {
        // Quitting out of a demo (Esc menu) returns to the demo browser, like demo_stop
        if (!g_pending_restart) {
            set_jump_to_demo_browser(true);
        }
        reset_ctx();
    }
}

bool demo_playback_start_from_menu(const std::string& name)
{
    if (g_ctx.state != PlaybackState::inactive) {
        rf::console::print("A demo is already playing - use demo_stop first");
        return false;
    }
    if (rf::is_multi) {
        rf::console::print("Cannot play a demo while in a multiplayer game - disconnect first");
        return false;
    }
    g_pending_restart.reset(); // starting from the menu supersedes a queued restart
    return demo_play_start(demo_file_resolve_path(name), -1.0);
}

bool demo_playback_handle_startup_param()
{
    if (!get_demo_cmd_line_param().found()) {
        return false;
    }
    // Quoted paths with spaces arrive as one arg with the quotes already removed
    // (os_parse_params_hook replaces the stock tokenizer)
    std::string name = get_demo_cmd_line_param().get_arg();
    if (name.empty()) {
        xlog::warn("-demo: missing demo file name");
        return false;
    }
    // Same arming the -url startup connect uses: the join sequence flags are set here
    // and the jump machinery in rf_init_state_hook drives the menus once the engine
    // reaches the main menu (intermediate bootup states fast-forward via
    // rf_state_is_closed_hook while a jump is armed)
    return demo_play_start(demo_file_resolve_path(name), -1.0);
}

void demo_playback_do_patch()
{
    // Init cmd line param
    get_demo_cmd_line_param();

    psnet_send_internal_hook.install();
    multi_check_socket_status_hook.install();
    psnet_rel_connect_to_server_hook.install();
    multi_obj_interp_orient_hook.install();
    multi_obj_interp_pos_hook.install();
    gameplay_sim_frame_hook.install();
    vmesh_process_hook.install();
    item_render_game_is_paused_hook.install();

    demo_play_cmd.register_cmd();
    demo_stop_cmd.register_cmd();
    demo_pause_cmd.register_cmd();
    demo_timescale_cmd.register_cmd();
    demo_seek_cmd.register_cmd();
    demo_info_cmd.register_cmd();
    demo_povcomp_cmd.register_cmd();
}
