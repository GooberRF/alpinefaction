#include <cstring>
#include <ctime>
#include <format>
#include <utility>
#include <common/version/version.h>
#include <common/utils/string-utils.h>
#include <xlog/xlog.h>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include "demo.h"
#include "demo_file.h"
#include "demo_server_config.h"
#include "demo_internal.h"
#include "../server_config_snapshot.h"
#include "../network.h"
#include "../server.h"
#include "../server_internal.h"
#include "../alpine_packets.h"
#include "../../fflink/afstats_events.h"
#include "../../fflink/demo_upload.h"
#include "../../misc/alpine_settings.h"
#include "../../os/os.h"
#include "../../rf/multi.h"
#include "../../rf/player/player.h"
#include "../../rf/level.h"
#include "../../rf/os/console.h"
#include "../../rf/os/os.h"
#include "../../os/console.h"

// Join-request stash owned by network.cpp; set temporarily while synthesizing the
// recorder's join_accept so the AF extension uses the modern footer format.
extern ClientSoftware g_joining_client_version;
extern AlpineFactionJoinReqPacketExt g_joining_player_info;

namespace
{
    // Reserved source address for the virtual player; never routable as a real client
    // (real loopback clients cannot bind port 1).
    constexpr rf::NetAddr demo_recorder_addr{{0x7F000001}, 1};

    struct DemoRecordState
    {
        rf::Player* recorder = nullptr;
        DemoFileWriter writer;
        int64_t segment_start_ms = 0;
        bool capture_join_accept_pending = false;
        // sv_record was used to stop recording; suppresses auto-record until re-enabled
        bool stopped_by_command = false;
        // Recording was started by auto-record (not sv_record); enables the empty-segment
        // discard and match-mode gating - manual recordings are exempt from both
        bool auto_started = false;
        // A human player (not a bot/browser) was connected at some point during the
        // current segment; auto-recorded segments without one are discarded on close
        bool segment_had_human = false;
        // A write failed; the recorder must be destroyed at a safe frame point (see
        // capture_packet - deletion mid-send-loop is a UAF).
        bool teardown_pending = false;
    };

    DemoRecordState g_state;

    uint32_t record_time_ms()
    {
        const int64_t now = timer::get_i64(1000);
        const int64_t delta = now - g_state.segment_start_ms;
        return delta > 0 ? static_cast<uint32_t>(delta) : 0u;
    }

    void capture_packet(const void* data, size_t len, uint8_t flags)
    {
        if (!g_state.writer.is_open())
            return;
        // {u8 type, u16 size, ...}: type is the first byte of every captured packet.
        // Covers public chat, server chat_line broadcasts, and the team-chat mirror;
        // team location pings (af_ping_location) are unaffected.
        if (len > 0 && static_cast<const uint8_t*>(data)[0] == RF_GPT_CHAT_LINE && !server_demo_chat_record())
            return;
        // Keep the recorder's fabricated reliable socket looking alive so the
        // reliable-socket health check never reads it as timed out (taps fire at
        // obj-update rate in gameplay and at netgame_update rate in limbo).
        // last_packet_sent is refreshed too so the reliable-socket frame never fires a
        // 10s keepalive sendto to the fake 127.0.0.1:1 address.
        if (g_state.recorder && g_state.recorder->net_data) {
            const auto socket_id = static_cast<int>(g_state.recorder->net_data->reliable_socket);
            if (socket_id >= 0 && socket_id < rf::NET_MAX_REL_SOCKETS) {
                const int now = static_cast<int>(timer::get_i64(1000));
                rf::net_rel_sockets[socket_id].last_packet_received = now;
                rf::net_rel_sockets[socket_id].last_packet_sent = now;
            }
        }
        g_state.writer.write_packet(record_time_ms(), data, len, flags);
        // A write failure closes the file. Tear recording down cleanly instead of
        // orphaning the virtual player, but DEFER the actual player_delete: capture_packet
        // runs inside the engine's obj_update send loop (FUN_0047e630), which reads
        // player->next from the current node AFTER the send - deleting the recorder here
        // would make that read a use-after-free. The drain runs at frame level.
        if (g_state.writer.had_error()) {
            rf::console::print("Demo recording stopped: write failed");
            g_state.teardown_pending = true;
        }
    }

    rf::Player* create_recorder_player()
    {
        rf::Player* player = rf::player_allocate(false);
        if (!player) {
            xlog::error("Demo recorder: player allocation failed");
            return nullptr;
        }
        player->name = "[demo]";
        // 0xFF = teamless: never matches a real team, so per-team traffic (team chat)
        // is not mirrored into the demo
        player->team = 0xFF;
        player->entity_handle = -1;
        player->is_human_player = false;
        // The Observer type gets the observer exemptions (spawning, votes, team
        // balance, rosters, gametype participation) but receives every gameplay packet
        // a current Alpine client would - AF version gates treat it as AlpineFaction
        // at the reported version, so the demo matches what a modern client sees.
        player->version_info = ClientVersionInfoProfile{
            ClientSoftware::Observer, VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_TYPE,
            MAXIMUM_RFL_VERSION, false};

        rf::PlayerNetData* net_data = player->net_data;
        net_data->addr = demo_recorder_addr;
        // send_state_info promotes to 2; from then on the stock obj_update loop serves it
        net_data->state = 1;
        // A fabricated CONNECTED slot: the capture taps intercept multi_io_send* before
        // any buffering, but several stock senders (send_boolean_packet - geomods! - and
        // the pregame_* emitters) pre-check reliable_socket != -1 BEFORE calling the
        // funnel, so -1 would silently drop their packets from the demo. The slot is
        // kept fresh in capture_packet and closed by player_delete on teardown.
        net_data->reliable_socket = demo_alloc_fake_reliable_socket(demo_recorder_addr);
        if (net_data->reliable_socket < 0) {
            xlog::warn("Demo recorder: no free reliable socket slot; not recording");
            rf::player_delete(player);
            return nullptr;
        }
        // Never NPF_WAITING_FOR_RELIABLE_SOCKET (0x4); loaded so level-change logic
        // does not wait for a client that never answers.
        net_data->flags = rf::NPF_CLIENT_IS_LOADED;
        net_data->player_id = rf::multi_alloc_player_id();
        net_data->join_time_ms = static_cast<int>(timer::get_i64(1000));
        // Full server tick rate: server_update_rate_injection clamps the interval
        // to 1000 / server_netfps
        net_data->max_update_rate = 1000000;
        rf::update_player_rate(player);
        return player;
    }

    void destroy_recorder_player()
    {
        g_state.teardown_pending = false;
        rf::Player* player = std::exchange(g_state.recorder, nullptr);
        if (!player)
            return;
        // No left_game broadcast and no chat line - the player was never announced.
        // player_delete closes the fabricated reliable socket (frees the slot); the
        // buffers were never filled because the taps intercept before buffering.
        rf::player_delete(player);
    }

    DemoHeaderInfo build_header_info()
    {
        DemoHeaderInfo header;
        header.af_version_major = VERSION_MAJOR;
        header.af_version_minor = VERSION_MINOR;
        header.af_version_patch = VERSION_PATCH;
        header.level_filename = rf::level.filename.c_str();
        header.level_checksum = 0; // best effort; not needed for playback
        header.game_type = rf::netgame.type;
        header.mod_name = rf::mod_param.found() ? rf::mod_param.get_arg() : "";
        header.server_name = rf::netgame.name.c_str();
        header.server_netfps = g_alpine_game_config.server_netfps;
        header.start_time_unix = static_cast<uint64_t>(std::time(nullptr));
        header.server_max_players = static_cast<uint32_t>(rf::netgame.max_players);
        // net_data checked too: if the recorder ever gets kicked/deleted by a code
        // path that doesn't know about it, fail soft instead of crashing here
        header.demo_player_id = (g_state.recorder && g_state.recorder->net_data)
            ? g_state.recorder->net_data->player_id : 0;
        // Set bits (defined next to AFD_KNOWN_FEATURES in demo_file.h) when this recording
        // uses features an older reader cannot meaningfully play back
        header.required_features = 0;
        // Stamp the afstats identity only on auto-recorded segments reported under a live
        // session, so this segment can be attributed to the game its events were reported
        // under. Manual sv_record and no-GSK/no-game recordings carry no identity and are
        // never uploaded. on_game_start (which increments g_game) runs earlier in the
        // level-init path than this, so the identity captured here is the current game's.
        if (g_state.auto_started) {
            if (auto id = afstats::current_reporting_game()) {
                header.afstats_session_id = id->session_id;
                header.afstats_game = id->game;
            }
        }
        return header;
    }

    // Emits the synthetic "joining client" byte stream for the recorder: join_accept,
    // roster, full world snapshot and the AF join packets that live outside
    // send_state_info. Everything flows through the capture taps into the file.
    void emit_snapshot()
    {
        rf::Player* recorder = g_state.recorder;
        // Re-assert per-segment invariants - the stock level-change flow mutates
        // per-player net flags/state (send_state_info below promotes state back to 2)
        recorder->net_data->flags |= rf::NPF_CLIENT_IS_LOADED;
        recorder->net_data->flags &= ~rf::NPF_WAITING_FOR_RELIABLE_SOCKET;

        // Synthesized join_accept: send_join_accept_packet_hook captures the finished
        // bytes (incl. AlpineFactionJoinAcceptPacketExt) and skips the socket send.
        // Stash a modern AF client identity so the ext uses the footer format.
        const auto saved_version = g_joining_client_version;
        const auto saved_info = g_joining_player_info;
        g_joining_client_version = ClientSoftware::AlpineFaction;
        g_joining_player_info = {};
        g_joining_player_info.af_signature = ALPINE_FACTION_SIGNATURE;
        g_joining_player_info.version_major = VERSION_MAJOR;
        g_joining_player_info.version_minor = VERSION_MINOR;
        g_joining_player_info.version_patch = VERSION_PATCH;
        g_joining_player_info.version_type = VERSION_TYPE;
        g_joining_player_info.max_rfl_version = MAXIMUM_RFL_VERSION;
        g_state.capture_join_accept_pending = true;
        rf::send_join_accept_packet(&demo_recorder_addr, recorder);
        g_state.capture_join_accept_pending = false;
        g_joining_client_version = saved_version;
        g_joining_player_info = saved_info;

        // Roster (recorder excluded via DemoRosterHideGuard recipient rule).
        rf::send_players_packet(recorder);
        // Full world snapshot; promotes net_data->state to 2. Alpine injections add
        // trigger, spray, koth/bagman/salvage/pit state.
        rf::send_state_info(recorder);
        // AF "new joiner" packets outside send_state_info.
        af_send_server_info_packet(recorder);
        // Current scores/pings so playback has a sane scoreboard from the start.
        rf::send_netgame_update_packet(recorder);
    }

    void start_segment()
    {
        auto path = demo_file_build_new_path(
            std::string{get_filename_without_ext(rf::level.filename.c_str())});
        if (path.empty())
            return;
        g_state.segment_start_ms = timer::get_i64(1000);
        if (!g_state.writer.open(path, build_header_info()))
            return;
        // First record of the segment: the server's base config (mutators/gametype
        // settings/flags) for a future details pane. Recorder is server-side, so it reads
        // config directly. One snapshot per segment at start is sufficient.
        g_state.writer.write_server_info(encode_server_config_block(server_config::capture_server_config_snapshot()));
        // Humans persisting from the previous level count; mid-segment joins are
        // reported through demo_record_on_human_join
        g_state.segment_had_human = !get_clients(false, false).empty();
        emit_snapshot();
        rf::console::print("Demo recording started: {}", path);
    }

    void close_segment()
    {
        if (!g_state.writer.is_open())
            return;
        const bool was_auto = g_state.auto_started;
        g_state.writer.close(record_time_ms());
        // A finalization failure (footer write / flush) leaves a possibly-truncated file:
        // don't announce a clean stop or enqueue it for upload.
        const bool finalize_ok = !g_state.writer.had_error();
        // An empty auto segment is never uploaded, whether or not its delete succeeds: on a
        // failed delete the empty file is left on disk but still not enqueued.
        const bool discard = g_state.auto_started && !g_state.segment_had_human;
        if (discard) {
            if (demo_file_delete(g_state.writer.path())) {
                rf::console::print("Demo discarded (no players): {}", g_state.writer.path());
            }
            return;
        }
        if (!finalize_ok) {
            rf::console::print("Demo recording stopped with a write error - not uploading: {}",
                               g_state.writer.path());
            return;
        }
        rf::console::print("Demo recording stopped: {}", g_state.writer.path());
        // Only auto-recorded, kept segments are candidates for FactionFiles upload; the
        // uploader re-checks header identity and size before enqueuing.
        if (was_auto) {
            fflink::demo_upload_on_segment_closed(g_state.writer.path());
        }
    }

    bool recording_possible()
    {
        return rf::is_multi && rf::is_server && !rf::level.filename.empty();
    }

    void start_recording(bool auto_started)
    {
        if (g_state.writer.is_open() || !recording_possible())
            return;
        // Never (re)start on top of a recorder awaiting the deferred write-failure
        // teardown - destroy it first so a fresh segment gets a fresh virtual player.
        if (g_state.teardown_pending) {
            destroy_recorder_player();
        }
        if (!g_state.recorder) {
            g_state.recorder = create_recorder_player();
            if (!g_state.recorder)
                return;
        }
        g_state.auto_started = auto_started;
        start_segment();
        if (!g_state.writer.is_open()) {
            // Segment file could not be opened - do not leave an idle virtual player around
            destroy_recorder_player();
        }
    }

    void stop_recording()
    {
        close_segment();
        destroy_recorder_player();
    }

    FunHook<void(rf::Player*, const void*, int)> multi_io_send_hook{
        0x00479370,
        [](rf::Player* player, const void* packet, int len) {
            if (player && player->is_observer()) {
                // Capture before buffering; never let recorder traffic reach a socket
                capture_packet(packet, len, 0);
                return;
            }
            multi_io_send_hook.call_target(player, packet, len);
        },
    };

    FunHook<void(rf::Player*, const void*, int, int)> multi_io_send_reliable_hook{
        0x00479480,
        [](rf::Player* player, const void* data, int len, int require_in_game) {
            if (player && player->is_observer()) {
                capture_packet(data, len, DEMO_PKT_RELIABLE);
                return;
            }
            multi_io_send_reliable_hook.call_target(player, data, len, require_in_game);
        },
    };

    // send_new_player_packet (0x0047A460) is the one stock broadcast that bypasses
    // the multi_io_send* funnels: it calls psnet_rel_send(reliable_socket, ...)
    // directly per recipient, so the taps above never see it and mid-game joiners
    // were missing from demos (no scoreboard row, unspectatable on playback).
    // Divert the call aimed at the recorder's fake socket into the demo file.
    CallHook<void(int, const void*, int)> send_new_player_packet_rel_send_hook{
        0x0047A559,
        [](int socket_id, const void* data, int len) {
            rf::Player* recorder = g_state.recorder;
            if (recorder && recorder->net_data
                && socket_id == static_cast<int>(recorder->net_data->reliable_socket)) {
                capture_packet(data, len, DEMO_PKT_RELIABLE);
                return;
            }
            send_new_player_packet_rel_send_hook.call_target(socket_id, data, len);
        },
    };

    ConsoleCommand2 sv_record_cmd{
        "sv_record",
        []() {
            if (!rf::is_server) {
                rf::console::print("This command can only be used on a server");
                return;
            }
            if (demo_record_active()) {
                g_state.stopped_by_command = true;
                stop_recording();
            }
            else {
                if (!recording_possible()) {
                    rf::console::print("Cannot record a demo right now (no level loaded?)");
                    return;
                }
                g_state.stopped_by_command = false;
                start_recording(false);
                // Note: a demo started mid-level begins with a full state snapshot;
                // chat/kills from before the start are not in the file.
            }
        },
        "Toggle server-side demo recording for the current level",
    };
}

bool demo_record_active()
{
    return g_state.writer.is_open();
}

rf::Player* demo_record_recorder()
{
    return g_state.recorder;
}

void demo_record_on_player_deleted(rf::Player* player)
{
    // The engine is freeing this player right now. If it is our virtual recorder
    // (kicked by name, or reaped by the reliable-socket timeout sweep), drop our raw
    // pointer WITHOUT calling player_delete again and finalize any open segment.
    // Otherwise g_state.recorder would dangle (use-after-free / heap corruption).
    if (!g_state.recorder || player != g_state.recorder)
        return;
    g_state.recorder = nullptr;
    g_state.teardown_pending = false; // the engine is doing the teardown for us
    close_segment();
    g_state.stopped_by_command = false;
    g_state.auto_started = false;
}

bool demo_record_capture_join_accept(const rf::NetAddr& addr, const void* data, size_t len)
{
    if (!g_state.capture_join_accept_pending || !(addr == demo_recorder_addr))
        return false;
    capture_packet(data, len, DEMO_PKT_SYNTHESIZED);
    return true;
}

int demo_record_reliable_socket()
{
    if (!g_state.recorder || !g_state.recorder->net_data)
        return -1;
    return static_cast<int>(g_state.recorder->net_data->reliable_socket);
}

void demo_record_capture_team_scoped(const void* data, size_t len, unsigned char team)
{
    if (!g_state.writer.is_open())
        return;
    // The recorder is teamless (0xFF) so team-filtered relay loops never reach it;
    // callers mirror the wire bytes here instead, tagged with the scoped team so
    // playback can filter to the followed player's team.
    const uint8_t flags = DEMO_PKT_TEAM_SCOPED | (team == 1 ? DEMO_PKT_TEAM1 : 0);
    capture_packet(data, len, flags);
}

void demo_record_pvp_damage_notify(unsigned char victim_id, float damage, bool died, bool crit, unsigned char attacker_id)
{
    if (!g_state.recorder || !g_state.writer.is_open())
        return;
    // Goes through the multi_io_send tap into the file like every other recorder packet
    af_send_damage_notify_packet_for_demo(victim_id, damage, died, crit, attacker_id, g_state.recorder);
}

void demo_record_crit_shot(unsigned char shooter_id, unsigned char weapon_type)
{
    if (!g_state.recorder || !g_state.writer.is_open())
        return;
    af_send_crit_shot_packet(shooter_id, weapon_type, g_state.recorder);
}

void demo_record_award(unsigned char award_id, unsigned char victim_player_id, unsigned char earner_id)
{
    if (!g_state.recorder || !g_state.writer.is_open())
        return;
    af_send_award_for_demo(g_state.recorder, award_id, victim_player_id, earner_id);
}

void demo_server_on_level_init_post()
{
    if (!rf::is_multi || !rf::is_server)
        return;
    // On match-mode servers auto-record only covers live matches, not the idle and
    // pre-match/ready-up levels in between; match_active was flipped by the limbo
    // hook before this level's init, so it identifies the level now starting.
    const bool auto_record_wanted = !server_is_match_mode_enabled() || g_match_info.match_active;
    if (demo_record_active()) {
        // Segment rollover: the limbo phase was captured at the tail of the closing
        // file; the virtual player persists across the boundary like a real client.
        close_segment();
        if (g_state.auto_started && !auto_record_wanted) {
            // Match ended (or was canceled) - don't roll into a non-match level
            destroy_recorder_player();
            return;
        }
        start_segment();
        if (!g_state.writer.is_open()) {
            destroy_recorder_player();
        }
    }
    else if (server_demo_auto_record() && !g_state.stopped_by_command && auto_record_wanted) {
        start_recording(true);
    }
}

void demo_record_on_multi_stop()
{
    if (g_state.recorder || g_state.writer.is_open()) {
        stop_recording();
    }
    g_state.stopped_by_command = false;
    g_state.auto_started = false;
}

void demo_record_on_human_join()
{
    if (g_state.writer.is_open()) {
        g_state.segment_had_human = true;
    }
}

void demo_record_server_do_frame()
{
    // Runs the deferred write-failure teardown (see capture_packet) at frame level,
    // outside the engine's obj_update send loop, so destroying the recorder is safe.
    if (g_state.teardown_pending) {
        destroy_recorder_player();
    }
}

DemoRosterHideGuard::DemoRosterHideGuard(rf::Player* recipient)
{
    rf::Player* recorder = g_state.recorder;
    if (!recorder || recorder == recipient)
        return;
    // player_list is a circular doubly-linked list; single-threaded server, so a
    // temporary unlink around the packing call is safe.
    rf::Player* next = recorder->next;
    rf::Player* prev = recorder->prev;
    if (next == recorder)
        return; // sole element - packing a roster with only the recorder is moot
    prev->next = next;
    next->prev = prev;
    if (rf::player_list == recorder)
        rf::player_list = next;
    m_unlinked = recorder;
    m_next = next;
    m_prev = prev;
}

DemoRosterHideGuard::~DemoRosterHideGuard()
{
    if (!m_unlinked)
        return;
    m_unlinked->next = m_next;
    m_unlinked->prev = m_prev;
    m_prev->next = m_unlinked;
    m_next->prev = m_unlinked;
}

void demo_record_do_patch()
{
    multi_io_send_hook.install();
    multi_io_send_reliable_hook.install();
    send_new_player_packet_rel_send_hook.install();
    sv_record_cmd.register_cmd();
}
