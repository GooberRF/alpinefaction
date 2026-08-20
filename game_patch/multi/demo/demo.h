#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Alpine Demos ("Alpine Demos" / .afd): server-side demo recording via a virtual
// spectator player + client-side playback through the engine's own join flow.
// See research/demo-system-prd.md.

namespace rf
{
    struct Player;
    struct NetAddr;
}

// Installs all demo hooks and console commands. Called from multi_do_patch().
void demo_do_patch();

// ---- Server-side recording (demo_record.cpp) ----

// Auto-start / per-level segment rollover. Called from level_init_post_hook after
// gametype level init so the state-info snapshot includes gametype state.
void demo_server_on_level_init_post();
// Finalizes the current segment and removes the virtual player. Called from multi_stop_hook.
void demo_record_on_multi_stop();
// Server per-frame tick: runs the deferred write-failure teardown at a safe point (the
// recorder cannot be destroyed inside the send taps - that runs mid obj_update loop).
// Called from multi_io_do_frame_hook's server path.
void demo_record_server_do_frame();
// Notification that the engine is deleting a player (from player_destroy_hook). If it is
// the virtual recorder (kick-by-name, reliable-socket timeout sweep), drops the module's
// pointer and finalizes any open segment WITHOUT re-deleting the player. Cheap no-op when
// no recorder exists. Prevents the recorder Player* from dangling (C1).
void demo_record_on_player_deleted(rf::Player* player);
// True while a demo file is being written.
bool demo_record_active();
// The virtual recorder Player*, or null when not recording. Used by the netgame_update
// broadcast hook to deliver a targeted full-roster update to the recorder for capture.
rf::Player* demo_record_recorder();
// Marks the current segment as having had a human player connected. Called from the
// join flow once is_bot/is_browser classification is final; auto-recorded segments
// that never see a human (empty server or bots/browsers only) are deleted on close.
void demo_record_on_human_join();
// Capture branch for the synthesized join_accept: called from send_join_accept_packet_hook
// with the finished packet bytes (including the AF extension). Returns true when the bytes
// were written to the demo (caller must skip the real socket send).
bool demo_record_capture_join_accept(const rf::NetAddr& addr, const void* data, size_t len);
// Mirrors a team-scoped wire packet (team chat, team location ping) into the demo,
// tagged with the team it was filtered to. The teamless recorder never receives such
// packets through the send taps, so the server-side relay points call this explicitly;
// no-op unless recording.
void demo_record_capture_team_scoped(const void* data, size_t len, unsigned char team);
// Mirrors a PvP damage notification into the demo, tagged with the attacker so playback
// can filter to the spectated player. Called from the server damage path next to the
// live attacker/spectator sends; no-op unless recording.
void demo_record_pvp_damage_notify(unsigned char victim_id, float damage, bool died, bool crit, unsigned char attacker_id);
// Mirrors a crit-shot telegraph into the demo, tagged with the shooter so playback can
// filter to the spectated player. Called from crits_broadcast_shot for every weapon class;
// no-op unless recording.
void demo_record_crit_shot(unsigned char shooter_id, unsigned char weapon_type);
// Mirrors an earned award into the demo, tagged with the earner so playback can filter to
// the spectated player. Called from grant_award next to the live award sends; no-op unless
// recording.
void demo_record_award(unsigned char award_id, unsigned char victim_player_id, unsigned char earner_id);
// Reliable-socket slot of the recorder's virtual player, or -1. Used to keep the
// deferred reliable queue (send_queues_rel_add_packet) away from the fabricated slot -
// its drain path sends via net_rel_send for real, bypassing the capture taps.
int demo_record_reliable_socket();

// RAII guard that temporarily unlinks the demo recorder from rf::player_list so stock
// roster/count code (players packet) never sees it. No-op when there is no recorder or
// the recipient is the recorder itself.
class DemoRosterHideGuard
{
public:
    explicit DemoRosterHideGuard(rf::Player* recipient);
    ~DemoRosterHideGuard();
    DemoRosterHideGuard(const DemoRosterHideGuard&) = delete;
    DemoRosterHideGuard& operator=(const DemoRosterHideGuard&) = delete;

private:
    rf::Player* m_unlinked = nullptr;
    rf::Player* m_next = nullptr;
    rf::Player* m_prev = nullptr;
};

// ---- Client-side playback (demo_playback.cpp) ----

// True while demo playback owns the multiplayer session.
bool demo_playback_active();
// Recording build's AF version while a demo is loaded for playback; all zeros otherwise.
// The designated hook for packet decoders that must branch on the wire layout the
// recording build wrote (compare with version_is_older from multi/multi.h).
struct DemoRecordedAfVersion
{
    int major = 0;
    int minor = 0;
    int patch = 0;
};
DemoRecordedAfVersion demo_playback_recorded_af_version();
// True while a seek fast-forward or its post-seek settle window is active - world
// rendering and audio are suppressed and the seek overlay is drawn instead.
bool demo_playback_is_seeking();
// True only during the fast-forward burst itself (not the settle window). Used to
// suppress transient effect creation while records are burst-fed; effects created
// during settle come from normal-paced packets and are legitimate.
bool demo_playback_in_seek_burst();
// Current playback position (>= 0). Only meaningful while playback is active.
double demo_playback_clock_ms();
// Total demo length, or 0 when unknown (footerless file - grows as records stream in).
uint32_t demo_playback_duration_ms();
bool demo_playback_paused();
// True while the world simulation is frozen for demo pause (paused, in gameplay and
// not seeking). The paused freelook camera drive keys on this.
bool demo_playback_sim_frozen();
// True once the demo reached its end (a seek can restart the session).
bool demo_playback_finished();
// Toggles pause; no-op unless playback is in the playing state.
void demo_playback_toggle_pause();
// Requests playback teardown (deferred to the pump's networking phase, like demo_stop).
// Cancels any queued backward-seek restart. Used when an external failure - e.g. the
// level autodownload of the demo's map - makes the session unplayable.
void demo_playback_stop();
// True when a seek can be requested (playing or finished).
bool demo_playback_can_seek();
// True while a backward-seek session restart is queued but has not re-entered
// playback yet (the window in which demo_playback_active() reads false).
bool demo_playback_restart_pending();
// Requests a seek to target_ms (clamped >= 0). Forward seeks are handed to the pump
// (networking phase); backward seeks or seeks after EOF queue a session restart.
// Never seeks synchronously - safe to call from input/render phases.
void demo_playback_request_seek(double target_ms);
// Per-frame pump; called from multi_io_do_frame_hook.
void demo_playback_do_frame();
// HUD overlay (file name, clock, pause/timescale); called from hud render.
void demo_playback_render();
// Powerup respawn timers (demo_powerup_timers.cpp); called from hud render right
// after demo_playback_render. Gated on the DemoPowerupTimers setting.
void demo_powerup_timers_render();
// Full-screen "SEEKING..." overlay with progress; called from after_frame_render_hook
// (the world render - including the HUD - is skipped while seeking).
void demo_playback_render_seek_overlay();
// Records a player as recently active (killer/attacker) for auto-follow target
// selection. No-op unless playback is active.
void demo_playback_note_player_activity(rf::Player* player);
// Engine-initiated multi_stop during playback (e.g. user disconnect) - resets playback state.
void demo_playback_on_multi_stop();
// Gameseq state-init notification (called from rf_init_state_hook). Runs the deferred
// session restart a backward seek queued, once the engine has settled in a menu state.
// Takes the raw rf::GameState value to keep this header light.
void demo_playback_on_state_init(int state);
// Starts playback of a demo by name (no path/extension) from a menu context, with the
// same guards as the demo_play console command. Returns true when playback started.
bool demo_playback_start_from_menu(const std::string& name);
// Handles the -demo <file> command line param (set by the launcher when a .afd file
// is opened) - launches straight into playback of that demo. Called from
// multi_after_full_game_init like the other startup params; returns true when
// startup playback began (callers skip -url/-levelm handling).
bool demo_playback_handle_startup_param();
