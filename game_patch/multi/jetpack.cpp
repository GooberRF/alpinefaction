#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <xlog/xlog.h>
#include "jetpack.h"
#include "bagman.h"
#include "gametype.h"
#include "multi.h"
#include "server_internal.h"
#include "alpine_packets.h"
#include "../rf/multi.h"
#include "../rf/entity.h"
#include "../rf/object.h"
#include "../rf/gameseq.h"
#include "../rf/particle_emitter.h"
#include "../rf/os/console.h"
#include "../rf/os/frametime.h"
#include "../rf/os/timestamp.h"
#include "../rf/player/player.h"
#include "../rf/player/control_config.h"
#include "../rf/sound/sound.h"
#include "../rf/vmesh.h"
#include "../rf/v3d.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../input/input.h"
#include "../hud/hud.h"
#include "../hud/hud_internal.h"
#include "../hud/multi_spectate.h"
#include "../misc/alpine_settings.h"

constexpr float JETPACK_BURN_TIME_S = 5.0f;
constexpr float JETPACK_RECHARGE_TIME_S = 7.5f;
constexpr int JETPACK_RECHARGE_DELAY_MS = 1000;
constexpr int JETPACK_ENGAGE_GRACE_MS = 250;
constexpr float JETPACK_THRUST_ACCEL = 19.62f;      // ~2x gravity
constexpr float JETPACK_MAX_CLIMB_SPEED = 9.0f;
// Fraction of the tank that has to come back before a dry pack answers the jump
// key again. Without it, a player holding jump at empty sputters one thrust frame
// per recharge delay forever, pinned at 0% and broadcasting on/off to everyone.
constexpr float JETPACK_EMPTY_LOCKOUT_REFUEL = 0.05f;
constexpr int JETPACK_GAUGE_FADE_MS = 500;
constexpr const char* JETPACK_MESH_FILENAME = "af-jetpack1.v3m";

// Thrust state travels on the reliable channel, where sends are coalesced into a
// per-socket buffer rather than costing a window slot each, so the pressure a
// stream like this puts on a connection is bytes and flush rate, not packet
// count. Two things are worth bounding anyway: the steady-state reliable traffic
// a lobby full of thrusting players generates, and the 1->N amplification a
// player flapping the jump key would otherwise get, where one keypress costs the
// server a rebroadcast to every other client. Both the client's outgoing state
// and the server's rebroadcast of it are limited to one send per interval per
// player; whatever the limit holds back is settled by the per-frame flushes
// below, so the wire always converges on the true state within one interval.
constexpr int JETPACK_STATE_SYNC_INTERVAL_MS = 250;

// Discoverability. The gauge shows itself off for a moment on every spawn, and
// a keybind hint offers itself under the reticle until the pack is first used.
constexpr int JETPACK_REVEAL_MS = 2500;
constexpr const char* JETPACK_GAUGE_LABEL = "JETPACK";
constexpr int JETPACK_LABEL_GAP_PX = 4;             // between label and bar top
constexpr int JETPACK_TEXT_ALPHA = 200;
constexpr int JETPACK_TEXT_SHADOW_ALPHA = 160;
constexpr int JETPACK_TEXT_SHADOW_OFFSET_PX = 1;
constexpr int JETPACK_HINT_FADE_MS = 150;
constexpr int JETPACK_HINT_OFFSET_PX = 48;          // below screen center
constexpr int JETPACK_HINT_OFFSET_PX_BIG = 72;
constexpr const char* JETPACK_HINT_FALLBACK_KEY = "JUMP";

// Engine effects borrowed from the punctured-brush steam jet.
constexpr const char* JETPACK_EMITTER_TYPE_NAME = "pipesteam";
constexpr const char* JETPACK_SOUND_NAME = "steam";

// Entity-local spawn point for the exhaust. Approximates the back of the torso;
// this becomes a real prop point once the dedicated jetpack mesh exists.
constexpr float JETPACK_EXHAUST_OFFSET_X = 0.0f;
constexpr float JETPACK_EXHAUST_OFFSET_Y = 0.4f;
constexpr float JETPACK_EXHAUST_OFFSET_Z = -0.35f;

// $particle flags "damages" in emitters.tbl. "pipesteam" sets it, which would
// make the exhaust hurt whoever walks through it.
constexpr int PARTICLE_FLAG2_DAMAGES = 0x1;

namespace
{

struct JetpackEffectState
{
    rf::ParticleEmitter* emitter = nullptr;
    int snd_instance = -1;
};

// Keyed by entity handle. Holds an entry only while that entity is thrusting.
std::unordered_map<int, JetpackEffectState> g_jetpack_effects;

float g_jetpack_fuel = 1.0f;
bool g_jetpack_thrusting = false;
// Latched when a burn empties the tank, cleared once JETPACK_EMPTY_LOCKOUT_REFUEL
// has come back. Blocks the empty-tank sputter loop.
bool g_jetpack_tank_empty = false;
int g_jetpack_local_entity_handle = -1;
rf::Timestamp g_jetpack_fall_grace;
rf::Timestamp g_jetpack_recharge_delay;

// Outgoing throttle for the local player's thrust state: what the wire was last
// told, and how long until it may be told again.
bool g_jetpack_sent_thrust = false;
rf::Timestamp g_jetpack_sync_cooldown;

// Server-side rebroadcast throttle, one entry per player who has ever asked for
// a state this level. `desired` is the last thing the client asked for,
// `last_broadcast` the last thing the other clients were told, and `last_handle`
// the entity both of those were about.
struct JetpackServerSyncState
{
    int last_handle = -1;
    bool last_broadcast = false;
    bool desired = false;
    rf::Timestamp cooldown;
};

std::unordered_map<uint8_t, JetpackServerSyncState> g_jetpack_server_sync;

bool g_jetpack_gauge_was_full = true;
rf::TimestampRealtime g_jetpack_gauge_fade;
rf::TimestampRealtime g_jetpack_gauge_reveal;

// Level-scoped: true once the local player has thrusted even a single time in
// the current level, which retires the keybind hint until the next level.
bool g_jetpack_has_thrusted = false;
bool g_jetpack_hint_wanted = false;
float g_jetpack_hint_fade = 0.0f;   // 0..1 ramp toward g_jetpack_hint_wanted
std::string g_jetpack_hint_text;

rf::VMesh* g_jetpack_mesh = nullptr;
bool g_jetpack_mesh_load_attempted = false;
// The jetpack mesh's own $prop_flag index, resolved once when the mesh loads.
// -1 means the mesh has no such prop point, which makes it unusable: there is
// nothing to lay over the character's prop point, so the render path skips it.
int g_jetpack_mesh_prop_idx = -1;

// $prop_flag index per character vmesh. The lookup is a string compare against
// every prop point in the mesh, and the render path would otherwise pay for it
// once per rendered player per frame. Misses are memoized as -1 too, so a
// character without the prop point costs one scan per vmesh per level rather
// than one per frame forever. `character` is VMesh::mesh, the character data the
// index was resolved against, kept as a validity token for the key.
struct JetpackCarrierPropIdx
{
    const void* character;
    int idx;
};
std::unordered_map<rf::VMesh*, JetpackCarrierPropIdx> g_jetpack_carrier_prop_idx;

// -2 = not looked up yet, -1 = the engine has no such emitter type.
int g_jetpack_emitter_type_idx = -2;
int g_jetpack_sound_handle = -2;

// Fills `out` with a jetpack-safe copy of the tbl template. The engine copies
// the type field by field while creating an emitter, so a caller-owned copy is
// enough to override what the table says (the same trick the spawn delay fix in
// particle.cpp uses).
bool jetpack_exhaust_emitter_type(rf::ParticleEmitterType* out)
{
    if (g_jetpack_emitter_type_idx == -2) {
        g_jetpack_emitter_type_idx = rf::particle_emitter_type_lookup(JETPACK_EMITTER_TYPE_NAME);
        if (g_jetpack_emitter_type_idx < 0) {
            xlog::warn("jetpack: particle emitter type '{}' not found", JETPACK_EMITTER_TYPE_NAME);
        }
    }
    if (g_jetpack_emitter_type_idx < 0) {
        return false;
    }
    // 0x007B2770 is an array of ParticleEmitterType POINTERS.
    *out = *(&rf::g_particle_emitter_types)[g_jetpack_emitter_type_idx];

    // The steam is decoration only; it must never damage an entity that flies
    // through it. Everything else the table asks for is kept.
    out->particle_flags2 &= ~PARTICLE_FLAG2_DAMAGES;

    // The emitters.tbl parser leaves uid and active_distance untouched, so tbl
    // templates carry whatever the allocation happened to contain. Both are
    // copied into the emitter and a stray pair would let the AF active distance
    // cull silently drop the exhaust.
    out->uid = 0;
    out->active_distance = 0.0f;
    return true;
}

// World-space spawn point of the exhaust.
rf::Vector3 jetpack_exhaust_pos(rf::Entity* ep)
{
    rf::Vector3 local{JETPACK_EXHAUST_OFFSET_X, JETPACK_EXHAUST_OFFSET_Y, JETPACK_EXHAUST_OFFSET_Z};
    return ep->pos + ep->orient.transform_vector(local);
}

// Straight down, in world space. Must stay unit length: "pipesteam" is a
// dirdepend emitter, so the spawn velocity is scaled by the length of this.
rf::Vector3 jetpack_exhaust_dir()
{
    return rf::Vector3{0.0f, -1.0f, 0.0f};
}

int jetpack_thrust_sound_handle()
{
    if (g_jetpack_sound_handle == -2) {
        const int foley_id = rf::foley_lookup_by_name(JETPACK_SOUND_NAME);
        g_jetpack_sound_handle = foley_id >= 0 ? rf::foley_get_sound_handle(foley_id) : -1;
    }
    return g_jetpack_sound_handle;
}

void jetpack_effect_destroy(JetpackEffectState& state)
{
    if (state.emitter) {
        state.emitter->destroy();
        state.emitter = nullptr;
    }
    if (state.snd_instance >= 0) {
        rf::snd_stop(state.snd_instance);
        state.snd_instance = -1;
    }
}

void jetpack_effect_start(rf::Entity* ep)
{
    auto& state = g_jetpack_effects[ep->handle];

    if (!state.emitter) {
        rf::ParticleEmitterType type{};
        if (jetpack_exhaust_emitter_type(&type)) {
            rf::Vector3 pos = jetpack_exhaust_pos(ep);
            // NOTE: Unparented and carried by hand every frame, which is how the game
            // drives its own dynamic emitters (punctured brush steam, flamethrower
            // flames). Parenting to the entity would only rebase pos/dir into
            // entity space; it would not get the emitter updated, because the
            // game only ticks emitters chained onto Object::emitter_list_head
            // and particle_emitter_create never links them there.
            state.emitter = rf::particle_emitter_create(0, type, ep->room, pos, false);
            if (state.emitter) {
                state.emitter->pos = pos;
                state.emitter->dir = jetpack_exhaust_dir();
                state.emitter->room = ep->room;
                state.emitter->activate();
            }
        }
    }

    if (state.snd_instance < 0) {
        const int snd_handle = jetpack_thrust_sound_handle();
        if (snd_handle >= 0) {
            rf::Vector3 no_vel{};
            state.snd_instance =
                rf::snd_play_3d(snd_handle, ep->pos, 1.0f, no_vel, rf::SOUND_GROUP_EFFECTS);
        }
    }
}

void jetpack_effect_stop(int entity_handle)
{
    auto it = g_jetpack_effects.find(entity_handle);
    if (it == g_jetpack_effects.end()) {
        return;
    }
    jetpack_effect_destroy(it->second);
    g_jetpack_effects.erase(it);
}

void jetpack_destroy_all_effects()
{
    for (auto& [handle, state] : g_jetpack_effects) {
        jetpack_effect_destroy(state);
    }
    g_jetpack_effects.clear();
}

// Keeps live effects glued to their entity and reaps entries whose entity died
// or disappeared, which is what self-heals a stuck "on" state after a death,
// a disconnect or a dropped packet.
void jetpack_update_effects()
{
    for (auto it = g_jetpack_effects.begin(); it != g_jetpack_effects.end();) {
        rf::Entity* ep = rf::entity_from_handle(it->first);
        if (!ep || rf::entity_is_dying(ep)) {
            jetpack_effect_destroy(it->second);
            it = g_jetpack_effects.erase(it);
            continue;
        }

        if (rf::ParticleEmitter* emitter = it->second.emitter) {
            // Move it onto the entity first, then let it spawn. The room has to
            // stay current or the emitter is skipped when its room is drawn, and
            // it has to be a real room because particles are created in it.
            emitter->pos = jetpack_exhaust_pos(ep);
            emitter->dir = jetpack_exhaust_dir();
            emitter->room = ep->room;
            if (emitter->room) {
                emitter->update();
            }
        }
        if (it->second.snd_instance >= 0) {
            if (rf::snd_is_playing(it->second.snd_instance)) {
                rf::snd_change_3d(it->second.snd_instance, ep->pos, ep->p_data.vel, 1.0f);
            }
            else {
                // The looping foley should never stop on its own, but if it does
                // the thruster would go silent for the rest of the burn.
                const int snd_handle = jetpack_thrust_sound_handle();
                rf::Vector3 no_vel{};
                it->second.snd_instance = snd_handle >= 0
                    ? rf::snd_play_3d(snd_handle, ep->pos, 1.0f, no_vel, rf::SOUND_GROUP_EFFECTS)
                    : -1;
            }
        }
        ++it;
    }
}

// True while the local sync layer is free to put another state on the wire.
bool jetpack_sync_cooldown_clear()
{
    return !g_jetpack_sync_cooldown.valid() || g_jetpack_sync_cooldown.elapsed();
}

// Forgets what the wire was told, which is what a fresh entity needs: every
// remote side starts a new entity switched off, and the handle carried by the
// broadcasts changes with it.
void jetpack_reset_local_sync()
{
    g_jetpack_sent_thrust = false;
    g_jetpack_sync_cooldown.invalidate();
}

// Sends the local player's thrust state when it differs from what the wire was
// last told and the throttle allows it. Called both on change — so an ordinary
// press costs no added latency — and once per frame, which is what settles a
// state the throttle held back. A state that flapped back to what was already
// sent settles for free: the flush finds nothing to do.
void jetpack_sync_local_thrust()
{
    if (g_jetpack_sent_thrust == g_jetpack_thrusting || !jetpack_sync_cooldown_clear()) {
        return;
    }

    const bool on = g_jetpack_thrusting;
    if (rf::is_server) {
        // Listen host: nothing to request, relay to the other clients directly.
        // This is the host's own state, so it never goes through the server-side
        // rebroadcast throttle, which only tracks states asked for by clients.
        if (g_jetpack_local_entity_handle != -1) {
            af_send_jetpack_state(static_cast<uint32_t>(g_jetpack_local_entity_handle), on);
        }
    }
    else {
        af_send_jetpack_state_request(on);
    }
    g_jetpack_sent_thrust = on;
    g_jetpack_sync_cooldown.set(JETPACK_STATE_SYNC_INTERVAL_MS);
}

void jetpack_set_local_thrust(bool on)
{
    if (g_jetpack_thrusting == on) {
        return;
    }
    g_jetpack_thrusting = on;
    if (on) {
        g_jetpack_has_thrusted = true;
    }

    const int handle = g_jetpack_local_entity_handle;
    if (rf::Entity* ep = rf::entity_from_handle(handle)) {
        jetpack_apply_entity_thrust(ep, on);
    }
    else if (!on) {
        jetpack_effect_stop(handle);
    }

    jetpack_sync_local_thrust();
}

// Nothing an entry remembers outlives the entity it was recorded for: every
// remote side starts a new entity switched off, and the handle the broadcast
// carries changes with it. So a respawn empties the entry instead of letting a
// dedup or a held-back state leak onto the new entity.
void jetpack_server_rebase_entry(JetpackServerSyncState& state, int entity_handle)
{
    if (state.last_handle == entity_handle) {
        return;
    }
    state.last_handle = entity_handle;
    state.last_broadcast = false;
    state.desired = false;
    state.cooldown.invalidate();
}

// Relays `on` for whatever entity `player` owns right now and re-arms that
// player's throttle. Answers false when there is no live entity to talk about,
// which is the caller's cue to drop the entry rather than broadcast a stale
// handle.
bool jetpack_server_broadcast_state(rf::Player* player, JetpackServerSyncState& state, bool on)
{
    rf::Entity* entity = player ? rf::entity_from_handle(player->entity_handle) : nullptr;
    if (!entity) {
        return false;
    }
    af_send_jetpack_state(static_cast<uint32_t>(player->entity_handle), on);
    // A listen host receives none of its own broadcasts — the receive path bails
    // out on a server — so this is the only thing that shows the host a remote
    // player's steam and engine noise. Deliberately here and not inside
    // af_send_jetpack_state: that is also called for the host's OWN state, which
    // has already applied its effects locally, and would double-apply.
    if (!rf::is_dedicated_server) {
        jetpack_apply_entity_thrust(entity, on);
    }
    state.last_handle = player->entity_handle;
    state.last_broadcast = on;
    state.cooldown.set(JETPACK_STATE_SYNC_INTERVAL_MS);
    return true;
}

// Settles every rebroadcast the throttle held back, and reaps the entries of
// players who have since left. Runs on dedicated and listen hosts alike.
void jetpack_server_do_frame()
{
    for (auto it = g_jetpack_server_sync.begin(); it != g_jetpack_server_sync.end();) {
        auto& state = it->second;

        // "Nothing held back" is the steady state and resolving the player below
        // is an O(players) list walk, so the cheap tests come first. The cost of
        // deferring them is that an idle entry outlives the player who owns it:
        // harmless, because the map is bounded by the player id space, every
        // request for that id rebases the entry first, and jetpack_level_init
        // drops the map wholesale.
        if (state.desired == state.last_broadcast) {
            ++it;
            continue;
        }
        if (state.cooldown.valid() && !state.cooldown.elapsed()) {
            ++it;
            continue;
        }

        // Re-resolved here: by the time a held-back state is due, its player may
        // have left or respawned into a different entity.
        rf::Player* player = rf::multi_find_player_by_id(it->first);
        if (!player) {
            it = g_jetpack_server_sync.erase(it);
            continue;
        }

        jetpack_server_rebase_entry(state, player->entity_handle);
        if (state.desired == state.last_broadcast) {
            // The state being held back belonged to the previous entity and the
            // rebase just dropped it. A fresh entity starts off everywhere.
            ++it;
            continue;
        }

        if (!jetpack_server_broadcast_state(player, state, state.desired)) {
            // No entity to attach the state to. Dropped silently: the client
            // sweeps its own effects, and a fresh entity starts off anyway.
            it = g_jetpack_server_sync.erase(it);
            continue;
        }
        ++it;
    }
}

// The player's own jump binding, spelled the way the options menu spells it.
// Resolving it allocates an engine-heap rf::String, so the line it goes into is
// built on the edge where the hint becomes wanted, never on a frame it is drawn.
void jetpack_rebuild_hint_text()
{
    const rf::String bind = get_action_bind_name(rf::CC_ACTION_JUMP);
    const char* name = bind.c_str();
    const std::string key = (!name || !*name || std::strcmp(name, "?") == 0)
        ? std::string{JETPACK_HINT_FALLBACK_KEY} : std::string{name};
    g_jetpack_hint_text = "Jetpack - Hold [" + key + "]";
}

void jetpack_reset_local_state()
{
    g_jetpack_fuel = 1.0f;
    g_jetpack_tank_empty = false;
    g_jetpack_fall_grace.invalidate();
    g_jetpack_recharge_delay.invalidate();
    g_jetpack_gauge_was_full = true;
    g_jetpack_gauge_fade.invalidate();
    // Dropped here so the every-frame reset while dead hides the gauge again;
    // the spawn path re-arms it immediately afterwards.
    g_jetpack_gauge_reveal.invalidate();
    g_jetpack_hint_wanted = false;
}

// True while a fresh spawn is still being shown the gauge.
bool jetpack_gauge_reveal_active()
{
    return g_jetpack_gauge_reveal.valid() && !g_jetpack_gauge_reveal.elapsed();
}

void jetpack_update_local_player()
{
    rf::Entity* ep = rf::local_player
        ? rf::entity_from_handle(rf::local_player->entity_handle) : nullptr;

    // A new entity handle means a new life, which refills the tank.
    if (ep && ep->handle != g_jetpack_local_entity_handle) {
        jetpack_set_local_thrust(false);
        g_jetpack_local_entity_handle = ep->handle;
        // Anything the throttle was still holding back belonged to the old
        // entity, so it is dropped rather than sent under the new handle.
        jetpack_reset_local_sync();
        jetpack_reset_local_state();
        // Show the full gauge off for a moment, so a player who joins a jetpack
        // server sees the mutator exists.
        g_jetpack_gauge_reveal.set(JETPACK_REVEAL_MS);
    }

    if (!ep || rf::entity_is_dying(ep)) {
        jetpack_set_local_thrust(false);
        jetpack_reset_local_state();
        if (!ep) {
            g_jetpack_local_entity_handle = -1;
        }
        return;
    }

    const bool typing = rf::console::console_is_visible() || rf::multi_chat_is_say_visible();
    const bool falling = rf::entity_is_falling(ep) && !rf::entity_in_vehicle(ep)
        && !rf::entity_is_swimming(ep) && !rf::entity_is_climbing(ep);

    // The pack is only active after a moment of sustained falling, so an ordinary
    // held jump does not sip fuel on the way up.
    if (falling) {
        if (!g_jetpack_fall_grace.valid()) {
            g_jetpack_fall_grace.set(JETPACK_ENGAGE_GRACE_MS);
        }
    }
    else {
        g_jetpack_fall_grace.invalidate();
    }

    const bool grace_elapsed = g_jetpack_fall_grace.valid() && g_jetpack_fall_grace.elapsed();
    // The raw poll and the effective press are kept apart: thrust must not answer
    // the key while the player is typing, but the hint below must not offer
    // itself to a player who is already holding jump with the chat box open.
    const bool jump_down =
        rf::control_is_control_down(&rf::local_player->settings.controls, rf::CC_ACTION_JUMP);
    const bool jump_held = !typing && jump_down;
    const bool want_thrust = falling && grace_elapsed && jump_held
        && g_jetpack_fuel > 0.0f && !g_jetpack_tank_empty;

    const float dt = rf::frametime;
    if (want_thrust) {
        // The climb cap only limits what the thruster adds; a player already
        // rising faster (jump pad, explosion) is never slowed down by it.
        const float boosted = ep->p_data.vel.y + JETPACK_THRUST_ACCEL * dt;
        ep->p_data.vel.y = std::min(boosted, std::max(ep->p_data.vel.y, JETPACK_MAX_CLIMB_SPEED));

        g_jetpack_fuel = std::max(0.0f, g_jetpack_fuel - dt / JETPACK_BURN_TIME_S);
        if (g_jetpack_fuel <= 0.0f) {
            g_jetpack_tank_empty = true;
        }
        g_jetpack_recharge_delay.set(JETPACK_RECHARGE_DELAY_MS);
    }
    else if (g_jetpack_fuel < 1.0f
        && (!g_jetpack_recharge_delay.valid() || g_jetpack_recharge_delay.elapsed())) {
        g_jetpack_fuel = std::min(1.0f, g_jetpack_fuel + dt / JETPACK_RECHARGE_TIME_S);
    }

    // Released only once a usable amount of fuel is back, so a player who runs
    // dry with jump still held gets one clean cutoff instead of a sputter per
    // recharge delay for as long as the key is down.
    if (g_jetpack_tank_empty && g_jetpack_fuel >= JETPACK_EMPTY_LOCKOUT_REFUEL) {
        g_jetpack_tank_empty = false;
    }

    // The gauge shows while the tank is short and fades out once it tops up. A
    // spawn reveal counts as "not full" for its whole duration, so when it
    // expires the same was_full transition starts the usual fade-out.
    if (g_jetpack_fuel >= 1.0f && !jetpack_gauge_reveal_active()) {
        if (!g_jetpack_gauge_was_full) {
            g_jetpack_gauge_fade.set(0);
        }
        g_jetpack_gauge_was_full = true;
    }
    else {
        g_jetpack_gauge_was_full = false;
        g_jetpack_gauge_fade.invalidate();
    }

    // The hint offers itself only while the pack would actually answer the jump
    // key this instant and the player is not already pressing it. `!jump_down`
    // rather than `!jump_held`, so holding jump and opening the chat box does not
    // make the hint appear.
    const bool hint_wanted = !g_jetpack_has_thrusted && falling && grace_elapsed
        && g_jetpack_fuel > 0.0f && !g_jetpack_tank_empty && !typing && !jump_down;
    if (hint_wanted && !g_jetpack_hint_wanted) {
        // Built on the rising edge only: this is what keeps the draw path free of
        // the engine-heap allocation that resolving the bind name costs.
        jetpack_rebuild_hint_text();
    }
    g_jetpack_hint_wanted = hint_wanted;

    jetpack_set_local_thrust(want_thrust);
}

void jetpack_ensure_mesh_loaded()
{
    if (g_jetpack_mesh_load_attempted) {
        return;
    }
    g_jetpack_mesh_load_attempted = true;
    g_jetpack_mesh = rf::vmesh_load(JETPACK_MESH_FILENAME, rf::MESH_TYPE_STATIC, -1);
    if (!g_jetpack_mesh) {
        xlog::warn("jetpack: failed to load mesh '{}'", JETPACK_MESH_FILENAME);
        return;
    }
    // Resolved once, here, instead of on every frame the pack is drawn.
    g_jetpack_mesh_prop_idx = rf::vmesh_lookup_prop_point(g_jetpack_mesh, "$prop_flag");
    if (g_jetpack_mesh_prop_idx < 0) {
        xlog::warn("jetpack: mesh '{}' has no $prop_flag prop point, pack will not be drawn",
            JETPACK_MESH_FILENAME);
    }
}

// $prop_flag index for a character's vmesh, memoized for the level.
int jetpack_carrier_prop_idx(rf::VMesh* vmesh)
{
    // Re-resolve when the character behind the vmesh changed: a mid-level character
    // switch frees one character vmesh and can hand the same VMesh* address back for
    // a different character, which would otherwise serve the old character's index.
    const void* character = vmesh->mesh;
    auto it = g_jetpack_carrier_prop_idx.find(vmesh);
    if (it != g_jetpack_carrier_prop_idx.end() && it->second.character == character) {
        return it->second.idx;
    }
    const int idx = rf::vmesh_lookup_prop_point(vmesh, "$prop_flag");
    g_jetpack_carrier_prop_idx.insert_or_assign(vmesh, JetpackCarrierPropIdx{character, idx});
    return idx;
}

bool jetpack_viewer_is_first_person(rf::Entity* ep)
{
    if (!rf::local_player) {
        return false;
    }
    if (multi_spectate_is_spectating()) {
        if (!multi_spectate_is_first_person()) {
            return false;
        }
        rf::Player* target = multi_spectate_get_target_player();
        return target && ep->handle == target->entity_handle;
    }
    return ep->handle == rf::local_player->entity_handle;
}

// Solve the world transform that lays `mesh`'s $prop_flag exactly over the
// character's $prop_flag. There is no engine attachment for this, so it is
// re-derived every frame; only the prop point lookups are cached (`mesh_prop_idx`
// comes from the caller, the character's from jetpack_carrier_prop_idx).
bool jetpack_solve_attachment(rf::Entity* ep, rf::VMesh* mesh, int mesh_prop_idx,
    rf::Vector3* out_pos, rf::Matrix3* out_orient)
{
    const int carrier_prop_idx = jetpack_carrier_prop_idx(ep->vmesh);
    if (carrier_prop_idx < 0) {
        return false;
    }

    rf::Vector3 carrier_prop_pos{};
    rf::Matrix3 carrier_prop_orient{};
    rf::vmesh_get_prop_point_transform(
        ep->vmesh, carrier_prop_idx, &ep->orient, &ep->pos,
        &carrier_prop_orient, &carrier_prop_pos);

    rf::Vector3 mesh_prop_local_pos{};
    rf::Matrix3 mesh_prop_local_orient{};
    rf::Vector3 zero_pos{0.0f, 0.0f, 0.0f};
    rf::Matrix3 ident = rf::identity_matrix;
    rf::vmesh_get_prop_point_transform(
        mesh, mesh_prop_idx, &ident, &zero_pos,
        &mesh_prop_local_orient, &mesh_prop_local_pos);

    // For an orthonormal rotation, inverse == transpose.
    rf::Matrix3 mesh_prop_local_inv = mesh_prop_local_orient;
    mesh_prop_local_inv.transpose();
    rf::Matrix3 orient_world = carrier_prop_orient;
    orient_world.mul(mesh_prop_local_inv);

    *out_pos = carrier_prop_pos - orient_world.transform_vector(mesh_prop_local_pos);
    *out_orient = orient_world;
    return true;
}

// Gates shared by every piece of jetpack HUD: all of it belongs to the living
// local player and none of it survives a hidden HUD or a spectate view.
bool jetpack_hud_is_visible()
{
    if (!rf::is_multi || !jetpacks_are_active()) {
        return false;
    }
    if (is_hud_effectively_hidden() || multi_spectate_is_spectating()) {
        return false;
    }
    if (!rf::local_player) {
        return false;
    }
    rf::Entity* ep = rf::entity_from_handle(rf::local_player->entity_handle);
    return ep && !rf::entity_is_dying(ep);
}

// Gauge alpha for this frame, or -1 when the gauge is fully hidden. Full alpha
// whenever the tank is short or a spawn reveal is running, then a brief fade.
int jetpack_gauge_alpha()
{
    if (g_jetpack_fuel < 1.0f || jetpack_gauge_reveal_active()) {
        return 255;
    }
    if (!g_jetpack_gauge_fade.valid()) {
        return -1;
    }
    const int elapsed = g_jetpack_gauge_fade.time_since();
    if (elapsed >= JETPACK_GAUGE_FADE_MS) {
        g_jetpack_gauge_fade.invalidate();
        return -1;
    }
    return 255 - (255 * elapsed) / JETPACK_GAUGE_FADE_MS;
}

// Shadow pass then main pass, so HUD text keeps its edges over bright geometry
// (the technique multi_hud's big notification uses). `x` is the text center.
void jetpack_draw_shadowed_string(int x, int y, const char* text, int font, int alpha_scale)
{
    rf::gr::set_color(0, 0, 0, static_cast<rf::ubyte>((JETPACK_TEXT_SHADOW_ALPHA * alpha_scale) / 255));
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, x + JETPACK_TEXT_SHADOW_OFFSET_PX,
        y + JETPACK_TEXT_SHADOW_OFFSET_PX, text, font);
    rf::gr::set_color(255, 255, 255, static_cast<rf::ubyte>((JETPACK_TEXT_ALPHA * alpha_scale) / 255));
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, x, y, text, font);
}

void jetpack_draw_fuel_gauge(int alpha_scale)
{
    const bool big = g_alpine_game_config.big_hud;
    const int bar_w = big ? 16 : 10;
    const int bar_h = big ? 180 : 120;
    const int margin = big ? 60 : 40;
    const int border = big ? 2 : 1;

    const int x = rf::gr::clip_width() - margin - bar_w;
    const int y = (rf::gr::clip_height() - bar_h) / 2;

    rf::gr::set_color(0, 0, 0, static_cast<rf::ubyte>((128 * alpha_scale) / 255));
    rf::gr::rect(x, y, bar_w, bar_h);

    rf::gr::set_color(255, 255, 255, static_cast<rf::ubyte>((170 * alpha_scale) / 255));
    hud_rect_border(x, y, bar_w, bar_h, border);

    const int inner_h = bar_h - 2 * border;
    const int fill_h = static_cast<int>(inner_h * std::clamp(g_jetpack_fuel, 0.0f, 1.0f));
    if (fill_h > 0) {
        rf::gr::set_color(255, 165, 0, static_cast<rf::ubyte>((220 * alpha_scale) / 255));
        rf::gr::rect(x + border, y + border + (inner_h - fill_h), bar_w - 2 * border, fill_h);
    }

    // Names the bar, sitting directly above it and centered on it.
    const int font = hud_get_small_font();
    const int label_y = y - rf::gr::get_font_height(font) - JETPACK_LABEL_GAP_PX;
    jetpack_draw_shadowed_string(x + bar_w / 2, label_y, JETPACK_GAUGE_LABEL, font, alpha_scale);
}

void jetpack_draw_thrust_hint(int alpha_scale)
{
    // Normally built by jetpack_update_local_player on the frame the hint became
    // wanted; this only covers the case where nothing has built it yet. A player
    // who rebinds jump mid-fall picks the new name up on the next fall.
    if (g_jetpack_hint_text.empty()) {
        jetpack_rebuild_hint_text();
    }

    const bool big = g_alpine_game_config.big_hud;
    const int offset = big ? JETPACK_HINT_OFFSET_PX_BIG : JETPACK_HINT_OFFSET_PX;
    const int x = rf::gr::clip_width() / 2;
    const int y = rf::gr::clip_height() / 2 + offset;
    jetpack_draw_shadowed_string(x, y, g_jetpack_hint_text.c_str(), hud_get_default_font(), alpha_scale);
}

} // namespace

bool jetpacks_are_active()
{
    if (!rf::is_multi) {
        return false;
    }
    if (rf::is_server) {
        return g_alpine_server_config_active_rules.mutators.jetpacks_enabled;
    }
    const auto& info = get_af_server_info();
    return info.has_value() && info->jetpacks;
}

void jetpack_apply_entity_thrust(rf::Entity* ep, bool on)
{
    if (rf::is_dedicated_server || !ep) {
        return;
    }
    if (on) {
        // Gates for the packet path: a server the client does not trust must not
        // be able to make it allocate emitters and sound instances where nothing
        // reaps them — with the mutator off, outside gameplay, or on an entity no
        // player owns. The local player's own engage path already satisfies all
        // three, so ordinary play is unchanged. Switching OFF is never gated.
        if (!jetpacks_are_active()
            || rf::gameseq_get_state() != rf::GameState::GS_GAMEPLAY
            || !rf::player_from_entity_handle(ep->handle)) {
            return;
        }
        jetpack_effect_start(ep);
    }
    else {
        jetpack_effect_stop(ep->handle);
    }
}

void jetpack_server_on_state_request(rf::Player* player, bool on)
{
    if (!rf::is_server || !player || !player->net_data) {
        return;
    }

    auto& state = g_jetpack_server_sync[player->net_data->player_id];
    jetpack_server_rebase_entry(state, player->entity_handle);
    state.desired = on;
    if (state.desired == state.last_broadcast) {
        // The other clients already believe this, so a repeated request — or one
        // that flapped back inside the interval — costs no rebroadcast at all.
        return;
    }
    if (!state.cooldown.valid() || state.cooldown.elapsed()) {
        // Broadcast failures leave desired != last_broadcast, which is what makes
        // jetpack_server_do_frame retry or drop the entry.
        jetpack_server_broadcast_state(player, state, on);
    }
}

void jetpack_do_frame()
{
    // Ahead of the dedicated-server early-out: the rebroadcast throttle has to
    // settle on every host, and a dedicated server does nothing else here.
    if (rf::is_server && rf::is_multi && jetpacks_are_active()) {
        jetpack_server_do_frame();
    }

    if (rf::is_dedicated_server) {
        return;
    }

    const bool in_gameplay = rf::gameseq_get_state() == rf::GameState::GS_GAMEPLAY;
    const bool active = rf::is_multi && in_gameplay && jetpacks_are_active();

    if (!active) {
        jetpack_set_local_thrust(false);
        // Unconditional: a disconnect, limbo, the menus and single player all
        // land here, and every one of them used to leave live emitters holding
        // slots in the engine's fixed pool. Destroying an emitter is safe in all
        // of those states — the engine never returns one to the pool by itself.
        jetpack_destroy_all_effects();
    }
    else {
        jetpack_ensure_mesh_loaded();
        jetpack_update_local_player();
        jetpack_update_effects();
    }

    // Settles a state the throttle held back, including the switch-off above, so
    // the wire converges on the real state within one interval either way.
    jetpack_sync_local_thrust();
}

void jetpack_render_attachment(rf::Entity* ep)
{
    if (!rf::is_multi || !jetpacks_are_active()) {
        return;
    }
    if (!ep || !ep->vmesh || !g_jetpack_mesh || g_jetpack_mesh_prop_idx < 0
        || rf::entity_is_dying(ep)) {
        return;
    }
    if (!rf::player_from_entity_handle(ep->handle)) {
        return;
    }
    if (jetpack_viewer_is_first_person(ep)) {
        return;
    }
    if (gt_is_bagman_any() && g_bagman_info.carrier
        && ep->handle == g_bagman_info.carrier->entity_handle) {
        return;
    }

    rf::Vector3 pos{};
    rf::Matrix3 orient{};
    if (!jetpack_solve_attachment(ep, g_jetpack_mesh, g_jetpack_mesh_prop_idx, &pos, &orient)) {
        return;
    }

    rf::MeshRenderParams params{};
    params.init_defaults();
    params.orient = orient;
    rf::vmesh_render(g_jetpack_mesh, &pos, &orient, &params);
}

void jetpack_render_hud()
{
    if (!jetpack_hud_is_visible()) {
        // Nothing is drawn this frame, so drop the hint ramp: it eases back in
        // from nothing once the HUD returns.
        g_jetpack_hint_fade = 0.0f;
        return;
    }

    const int gauge_alpha = jetpack_gauge_alpha();
    if (gauge_alpha >= 0) {
        jetpack_draw_fuel_gauge(gauge_alpha);
    }

    // Ease the hint in and out rather than popping it on the frame the pack
    // becomes usable — the condition can flicker while a fall starts.
    const float target = g_jetpack_hint_wanted ? 1.0f : 0.0f;
    const float step = rf::frametime * (1000.0f / JETPACK_HINT_FADE_MS);
    g_jetpack_hint_fade = g_jetpack_hint_fade < target
        ? std::min(target, g_jetpack_hint_fade + step)
        : std::max(target, g_jetpack_hint_fade - step);
    if (g_jetpack_hint_fade > 0.0f) {
        jetpack_draw_thrust_hint(static_cast<int>(g_jetpack_hint_fade * 255.0f));
    }
}

void jetpack_level_init()
{
    // The engine does NOT hand dynamically created emitters back to its fixed
    // 128-slot pool: the free list is built once at startup, and a level unload
    // only empties each emitter's particle list, leaving the emitter itself
    // allocated and correctly linked. So these have to be destroyed, not just
    // forgotten. Doing it after the unload is safe. Vmeshes are the opposite,
    // the engine does free those, so the cached pointer is only dropped.
    jetpack_destroy_all_effects();
    g_jetpack_mesh = nullptr;
    g_jetpack_mesh_load_attempted = false;
    g_jetpack_mesh_prop_idx = -1;
    g_jetpack_carrier_prop_idx.clear();
    g_jetpack_emitter_type_idx = -2;
    g_jetpack_sound_handle = -2;

    g_jetpack_thrusting = false;
    g_jetpack_local_entity_handle = -1;
    jetpack_reset_local_sync();
    g_jetpack_server_sync.clear();
    jetpack_reset_local_state();
    // The keybind hint is level-scoped: it re-teaches once per level and retires
    // again after the player's first thrust. The fade has to be dropped too — the
    // HUD render path does not run during limbo, so a hint left at full alpha
    // would otherwise ghost onto the next level's first frame.
    g_jetpack_has_thrusted = false;
    g_jetpack_hint_fade = 0.0f;
}
