#include <cstring>
#include <cmath>
#include <format>
#include <memory>
#include <optional>
#include <string_view>
#include <xlog/xlog.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/MemUtils.h>
#include <common/utils/list-utils.h>
#include <common/utils/string-utils.h>
#include <common/rfproto.h>
#include "bagman.h"
#include "gametype.h"
#include "kill.h"
#include "multi.h"
#include "server.h"
#include "server_internal.h"
#include "alpine_packets.h"
#include "../rf/multi.h"
#include "../rf/item.h"
#include "../rf/entity.h"
#include "../rf/object.h"
#include "../rf/level.h"
#include "../object/alpine_bag.h"
#include "../rf/bmpman.h"
#include "../rf/file/file.h"
#include "../rf/gameseq.h"
#include "../rf/gr/gr_light.h"
#include "../rf/os/frametime.h"
#include "../rf/player/player.h"
#include "../rf/os/timestamp.h"
#include "../rf/sound/sound.h"
#include "../rf/vmesh.h"
#include "../rf/v3d.h"
#include "../hud/multi_spectate.h"
#include <patch_common/FunHook.h>

BagmanInfo g_bagman_info;
constexpr int kPowerupTypeAmp = 1; // Multi Damage Amplifier
constexpr int kCarrierAmpDurationMs = 600000; // refreshed every second, generous safety margin
constexpr int kCarrierAmpRefreshIntervalMs = 1000;
constexpr int kScoreTickMs = 1000;
constexpr int kBagPickupUnlockDelayMs = 500; // Delay after a bag is dropped before it can be picked up
constexpr float kCarrierTickEffectiveHealth = 10.0f;
constexpr float kCarrierKillEffectiveHealth = 50.0f;

namespace
{

// Hardcoded per-RFL bag home positions.
struct BagHomeEntry { const char* rfl; float x, y, z; };
constexpr BagHomeEntry kBagHomePositions[] = {
    { "dm01.rfl", 14.0f, -2.0f, -10.0f },
    { "dm02.rfl", 460.0f, -30.13f, 28.0f },
    { "dm03.rfl", -218.98f, -34.99f, -75.02f },
    { "dm04.rfl", 6.55f, -47.80f, 11.38f },
    { "dm05.rfl", 43.0f, -3.75f, 54.0f },
    { "dm06.rfl", 476.0f, -28.0f, 117.0f },
    { "dm07.rfl", 102.34f, 30.94f, -140.85f },
    { "dm08.rfl", 175.50f, -9.77f, -75.25f },
    { "dm09.rfl", 10.50f, -4.75f, 0.0f },
    { "dm10.rfl", -4.0f, 8.57f, 50.0f },
    { "dm11.rfl", -10.0f, -8.50f, -0.09f },
    { "dm12.rfl", 8.27f, -0.96f, 5.52f },
    { "dm13.rfl", 52.03f, -7.05f, -0.01f },
    { "dm14.rfl", 30.01f, 0.27f, -2.40f },
    { "dm15.rfl", -5.21f, 7.01f, 7.93f },
    { "dm16.rfl", 32.45f, 11.97f, 29.15f },
    { "dm17.rfl", -26.21f, 1.95f, 25.52f },
    { "dm18.rfl", 0.0f, 4.0f, 0.0f },
    { "dm19.rfl", -14.50f, 19.0f, -4.50f },
    { "dm20.rfl", 17.47f, -7.04f, 0.10f },
    { "ctf01.rfl", 131.0f, -7.77f, -81.50f },
    { "ctf02.rfl", 0.25f, -22.80f, 0.11f },
    { "ctf03.rfl", 3.77f, -10.02f, -0.0f },
    { "ctf04.rfl", 40.0f, -3.46f, 0.0f },
    { "ctf05.rfl", 2.0f, 1.16f, 8.0f },
    { "ctf06.rfl", 0.53f, 4.99f, 0.0f },
    { "ctf07.rfl", 8.50f, -5.0f, 22.0f },
    { "dmabruptdecayrc1.rfl", 15.54f, 2.50f, 11.0f },
    { "dmabruptdecayrc2.rfl", 15.54f, 2.50f, 11.0f },
};

std::optional<rf::Vector3> lookup_hardcoded_bag_home(std::string_view filename)
{
    for (const BagHomeEntry& e : kBagHomePositions) {
        if (string_iequals(filename, e.rfl)) {
            return rf::Vector3{e.x, e.y, e.z};
        }
    }
    return std::nullopt;
}

// The bag IS a Multi Damage Amplifier item
int g_bag_aura_bitmap = -1;
int g_saved_amp_aura_bitmap = -1;  // -1 = not currently swapped
bool g_bag_bitmaps_load_attempted = false;
constexpr const char* kBagMeshFilename = "af_pickup-bag.v3m";
bool g_bag_mesh_exists = false;
bool g_bag_mesh_checked = false;
int g_bag_light_handle = -1;
float g_bag_light_pulse_phase = 0.0f;
rf::VMesh* g_bag_carrier_mesh = nullptr;
bool g_bag_carrier_mesh_load_attempted = false;

void ensure_bag_bitmaps_loaded()
{
    if (g_bag_bitmaps_load_attempted) return;
    g_bag_bitmaps_load_attempted = true;
    g_bag_aura_bitmap = rf::bm::load("af_powerup-bag.vbm", -1, true);
}

void ensure_bag_mesh_checked()
{
    if (g_bag_mesh_checked) return;
    g_bag_mesh_checked = true;
    auto file = std::make_unique<rf::File>();
    if (file->open(kBagMeshFilename) == 0) {
        g_bag_mesh_exists = true;
        file->close();
    }
}

void apply_aura_swap_if_available()
{
    if (g_bag_aura_bitmap < 0) return;
    int& engine_amp_aura = addr_as_ref<int>(0x00594584);
    if (g_saved_amp_aura_bitmap < 0) {
        g_saved_amp_aura_bitmap = engine_amp_aura;
    }
    engine_amp_aura = g_bag_aura_bitmap;
}

void revert_aura_swap_if_active()
{
    if (g_saved_amp_aura_bitmap < 0) return;
    addr_as_ref<int>(0x00594584) = g_saved_amp_aura_bitmap;
    g_saved_amp_aura_bitmap = -1;
}

rf::Entity* alive_entity_for(rf::Player* player)
{
    if (!player) return nullptr;
    if (rf::player_is_dead(player) || rf::player_is_dying(player)) return nullptr;
    rf::Entity* ep = rf::entity_from_handle(player->entity_handle);
    if (!ep) return nullptr;
    if (ep->life <= 0.0f) return nullptr;
    return ep;
}

rf::Item* item_from_handle_or_null(int handle)
{
    if (handle < 0) return nullptr;
    rf::Object* obj = rf::obj_from_handle(handle);
    if (!obj || obj->type != rf::OT_ITEM) return nullptr;
    return static_cast<rf::Item*>(obj);
}

void announce(std::string_view msg)
{
    af_broadcast_automated_chat_msg(msg);
}

void apply_effective_health_reward(rf::Player* player, float amount)
{
    if (!player || amount <= 0.0f) return;
    rf::Entity* ep = rf::entity_from_handle(player->entity_handle);
    if (!ep || ep->life <= 0.0f) return;

    const float max_life_cap = std::max(ep->life, ep->info->max_life);
    const float max_armor_cap = std::max(ep->armor, ep->info->max_armor);
    distribute_effective_health(ep, amount, max_life_cap, max_armor_cap);
}

// Create amp (bag) item at an explicit position.
void spawn_bag_item(const rf::Vector3& pos, rf::Matrix3& orient)
{
    g_bagman_info.bag_pos = pos;
    g_bagman_info.bag_item_handle = -1;

    if (g_bagman_info.bag_item_type < 0) return;

    rf::Item* item = rf::item_create(
        g_bagman_info.bag_item_type,
        "Multi Damage Amplifier",
        60,
        -1,
        &pos,
        &orient,
        -1,
        0,
        0
    );

    if (!item) {
        xlog::warn("bagman: item_create failed for bag at ({},{},{}); ", pos.x, pos.y, pos.z);
        return;
    }

    item->item_flags |= rf::IF_DROPPED | rf::IF_NO_PICKUP;
    g_bagman_info.bag_item_handle = item->handle;
    g_bagman_info.pickup_unlock_timer.set(kBagPickupUnlockDelayMs);

    // -1 for level_item_index: the bag drop is not a level-placed item.
    rf::send_item_create_packet(item, 0, -1);
}

void kill_current_bag_item()
{
    rf::Item* item = item_from_handle_or_null(g_bagman_info.bag_item_handle);
    if (item) {
        // Broadcast the item-apply packet (entity_handle = 0 to indicate "no
        // player picked it up — it just goes away") so clients remove the item
        // visually. obj_flag_dead alone only affects the server.
        rf::send_item_apply_packet(nullptr, item->handle, 0, -1, -1, -1);
        rf::obj_flag_dead(item);
    }
    g_bagman_info.bag_item_handle = -1;
}

void resolve_bag_spawn_from_placed_item()
{
    if (g_bagman_info.bag_item_type < 0) {
        xlog::warn("bagman: Multi Damage Amplifier item type not registered; "
            "cannot resolve bag spawn ({} item types loaded)",
            static_cast<int>(rf::num_item_types));
        return;
    }

    // Priority-ordered list of item classes used as bag spawn positions
    // when no other location is specified for the map.
    static const char* const kBagCandidateClasses[] = {
        "Multi Damage Amplifier",
        "Multi Invulnerability",
        "Multi Super Armor",
        "Multi Super Health",
    };

    bool position_chosen = false;

    // Priority 1: mapper-placed Bag editor object.
    if (auto bag = get_first_bag_object()) {
        g_bagman_info.spawn_pos = bag->pos;
        g_bagman_info.spawn_orient = bag->orient;
        position_chosen = true;
    }

    // Priority 2: hardcoded bag positions for specific RFLs.
    if (!position_chosen) {
        if (auto override_pos = lookup_hardcoded_bag_home(rf::level.filename.c_str())) {
            g_bagman_info.spawn_pos = *override_pos;
            g_bagman_info.spawn_orient = rf::identity_matrix;
            position_chosen = true;
        }
    }

    // Walk item_list once per candidate class. The first match (in
    // priority order) becomes the spawn position if no higher-priority
    // source chose one.
    for (const char* cls : kBagCandidateClasses) {
        int type_idx = rf::item_lookup_type(cls);
        if (type_idx < 0) continue;
        rf::Item* it = rf::item_list.next;
        while (it && it != &rf::item_list) {
            rf::Item* next = it->next;
            if (it->info_index == type_idx) {
                if (!position_chosen) {
                    g_bagman_info.spawn_pos = it->pos;
                    g_bagman_info.spawn_orient = it->orient;
                    position_chosen = true;
                }
                rf::obj_flag_dead(it);
            }
            it = next;
        }
    }

    // Last resort: the level's player start position.
    if (!position_chosen) {
        g_bagman_info.spawn_pos = rf::level.player_start_pos;
        g_bagman_info.spawn_orient = rf::level.player_start_orient;
        xlog::warn("bagman: no suitable item found in level; bag spawned at player start position");
    }

    g_bagman_info.bag_pos = g_bagman_info.spawn_pos;
    g_bagman_info.spawn_known = true;
}

} // namespace

void bagman_force_state_sync_to(rf::Player* player)
{
    if (!gt_is_bagman_any()) return;
    af_send_bagman_state_packet(player);
}

void bagman_broadcast_state()
{
    if (!gt_is_bagman_any() || !rf::is_server) return;
    af_send_bagman_state_packet_to_all();
}

int bagman_get_red_team_score() 
{
    return g_bagman_info.red_team_score;
}

int bagman_get_blue_team_score()
{
    return g_bagman_info.blue_team_score;
}

void bagman_set_red_team_score(int v)
{
    g_bagman_info.red_team_score = v;
}

void bagman_set_blue_team_score(int v)
{
    g_bagman_info.blue_team_score = v;
}

void bagman_level_init()
{
    g_bagman_info = BagmanInfo{};

    // Drop the cached dynamic-light handle.
    g_bag_light_handle = -1;
    g_bag_light_pulse_phase = 0.0f;

    // Drop the cached carrier bag VMesh.
    g_bag_carrier_mesh = nullptr;
    g_bag_carrier_mesh_load_attempted = false;

    // Always restore engine state before deciding what to do this level.
    revert_aura_swap_if_active();

    g_bagman_info.active = gt_is_bagman_any();
    if (!g_bagman_info.active) return;

    // Apply visual swaps on clients.
    if (!rf::is_dedicated_server) {
        ensure_bag_bitmaps_loaded();
        apply_aura_swap_if_available();
    }

    // Resolve item type index on both client and server so the item_create
    // mesh swap hook can identify bag instances on either side.
    g_bagman_info.bag_item_type = rf::item_lookup_type("Multi Damage Amplifier");

    // Cache whether the bag mesh asset is present.
    ensure_bag_mesh_checked();

    g_bagman_info.state = BagState::BS_At_Spawn;
    g_bagman_info.score_tick.set(kScoreTickMs);
    g_bagman_info.carrier_amp_refresh.invalidate();
}

void bagman_level_init_post()
{
    if (!g_bagman_info.active) return;
    if (!rf::is_server) return;

    resolve_bag_spawn_from_placed_item();
    if (!g_bagman_info.spawn_known) return;

    const int delay_ms = g_alpine_server_config_active_rules.bagman.bag_spawn_delay_ms;
    if (delay_ms > 0) {
        // Defer the first bag spawn.
        g_bagman_info.state = BagState::BS_Delayed;
        g_bagman_info.spawn_delay_timer.set(delay_ms);
        bagman_broadcast_state();
    } else {
        spawn_bag_item(g_bagman_info.spawn_pos, g_bagman_info.spawn_orient);
        bagman_broadcast_state();
    }
}

static rf::Player* find_player_with_bag_powerup()
{
    for (rf::Player& pl : SinglyLinkedList{rf::player_list}) {
        if (rf::player_is_dead(&pl) || rf::player_is_dying(&pl)) continue;
        if (rf::multi_powerup_has_player(&pl, kPowerupTypeAmp)) {
            return &pl;
        }
    }
    return nullptr;
}

static void on_pickup(rf::Player* player)
{
    rf::Entity* ep = alive_entity_for(player);

    g_bagman_info.carrier = player;
    g_bagman_info.state = BagState::BS_Carried;
    g_bagman_info.bag_item_handle = -1; // engine consumed the item
    if (ep) g_bagman_info.bag_pos = ep->pos;
    g_bagman_info.score_tick.set(kScoreTickMs);
    g_bagman_info.carrier_amp_refresh.set(kCarrierAmpRefreshIntervalMs);
    g_bagman_info.return_timer.invalidate();

    // Extend the amp duration so the carrier visibly holds the bag indefinitely
    rf::multi_powerup_add(player, kPowerupTypeAmp, kCarrierAmpDurationMs);

    announce(std::format("{} has the bag!", player->name.c_str()));
    bagman_broadcast_state();
}

// Drop the bag at an explicit position
static void drop_bag_at_position(rf::Player* prev_carrier, const rf::Vector3& drop_pos)
{
    g_bagman_info.carrier = nullptr;
    g_bagman_info.state = BagState::BS_Dropped;
    g_bagman_info.bag_pos = drop_pos;
    g_bagman_info.return_timer.set(g_alpine_server_config_active_rules.bagman.bag_return_time_ms);

    if (prev_carrier) {
        rf::multi_powerup_remove(prev_carrier, kPowerupTypeAmp);
    }

    spawn_bag_item(drop_pos, g_bagman_info.spawn_orient);

    if (prev_carrier) {
        announce(std::format("{} dropped the bag!", prev_carrier->name.c_str()));
    } else {
        announce("The bag has been dropped!");
    }

    bagman_broadcast_state();
}

// Drop the bag from the carrier's entity using the same engine path that the
// stock drop_amps option uses on death.
static void drop_bag_from_entity(rf::Player* prev_carrier, rf::Entity* ep)
{
    if (prev_carrier) {
        // Remove the amp immediately on death so the previous bagman's HUD
        // indicator isn't wrong.
        rf::multi_powerup_remove(prev_carrier, kPowerupTypeAmp);
        if (ep) {
            ep->entity_flags2 &= ~rf::EF2_POWERUP_DAMAGE_AMP;
        }
    }

    g_bagman_info.carrier = nullptr;
    g_bagman_info.state = BagState::BS_Dropped;
    g_bagman_info.return_timer.set(g_alpine_server_config_active_rules.bagman.bag_return_time_ms);

    rf::Vector3 drop_pos = ep ? ep->pos : g_bagman_info.bag_pos;
    rf::Matrix3 drop_orient = ep ? ep->orient : g_bagman_info.spawn_orient;
    spawn_bag_item(drop_pos, drop_orient);

    if (prev_carrier) {
        announce(std::format("{} dropped the bag!", prev_carrier->name.c_str()));
    } else {
        announce("The bag has been dropped!");
    }

    bagman_broadcast_state();
}

void bagman_play_return_sound()
{
    rf::snd_play(65, 0, 0.0f, 1.0f);
}

static void on_return()
{
    g_bagman_info.state = BagState::BS_At_Spawn;
    g_bagman_info.return_timer.invalidate();

    // Replace the dropped item with one at the home position
    kill_current_bag_item();
    spawn_bag_item(g_bagman_info.spawn_pos, g_bagman_info.spawn_orient);

    announce("The bag has returned.");
    bagman_play_return_sound();
    bagman_broadcast_state();
}

void bagman_do_frame()
{
    if (!rf::is_server) return;
    if (!gt_is_bagman_any()) {
        if (g_bagman_info.active) {
            g_bagman_info = BagmanInfo{};
        }
        return;
    }
    if (!g_bagman_info.spawn_known) return;

    if (g_bagman_info.carrier) {
        rf::Entity* raw_ep = rf::entity_from_handle(g_bagman_info.carrier->entity_handle);
        rf::Entity* ep = alive_entity_for(g_bagman_info.carrier);
        const bool still_has_amp = ep && rf::multi_powerup_has_player(g_bagman_info.carrier, kPowerupTypeAmp);

        if (!still_has_amp) {
            // Carrier died, lost the powerup, or disconnected — drop from their
            // entity using the engine's mechanism.
            drop_bag_from_entity(g_bagman_info.carrier, raw_ep);
        } else {
            g_bagman_info.bag_pos = ep->pos;

            // Refresh amp so it never times out while alive and carrying
            if (g_bagman_info.carrier_amp_refresh.elapsed()) {
                rf::multi_powerup_add(g_bagman_info.carrier, kPowerupTypeAmp, kCarrierAmpDurationMs);
                g_bagman_info.carrier_amp_refresh.set(kCarrierAmpRefreshIntervalMs);
            }

            // Score tick during active gameplay
            if (rf::gameseq_get_state() == rf::GameState::GS_GAMEPLAY
                && g_bagman_info.score_tick.elapsed()) {
                rf::player_add_score(g_bagman_info.carrier, 1);
                if (gt_is_tbm()) {
                    if (g_bagman_info.carrier->team == 0) {
                        g_bagman_info.red_team_score++;
                    } else {
                        g_bagman_info.blue_team_score++;
                    }
                }
                apply_effective_health_reward(g_bagman_info.carrier, kCarrierTickEffectiveHealth);
                g_bagman_info.score_tick.set(kScoreTickMs);

                // Broadcast immediately so clients see the score increment
                bagman_broadcast_state();
            }
        }
    } else {
        // Honour the initial bag spawn delay.
        if (g_bagman_info.spawn_delay_timer.valid()) {
            if (g_bagman_info.spawn_delay_timer.elapsed()) {
                g_bagman_info.spawn_delay_timer.invalidate();
                g_bagman_info.state = BagState::BS_At_Spawn;
                spawn_bag_item(g_bagman_info.spawn_pos, g_bagman_info.spawn_orient);
                announce("The bag is now available!");
                bagman_broadcast_state();
            }
            return;
        }

        // Clear the post-spawn IF_NO_PICKUP gate after a small delay to
        // ensure the dying player doesn't pick up the dropped bag.
        if (g_bagman_info.pickup_unlock_timer.valid()
            && g_bagman_info.pickup_unlock_timer.elapsed()) {
            g_bagman_info.pickup_unlock_timer.invalidate();
            if (rf::Item* bag = item_from_handle_or_null(g_bagman_info.bag_item_handle)) {
                bag->item_flags &= ~rf::IF_NO_PICKUP;
            }
        }

        // Recovery: state says the bag should be on the ground but no item
        // is associated with our handle.
        const bool item_missing = item_from_handle_or_null(g_bagman_info.bag_item_handle) == nullptr;
        if (item_missing
            && (g_bagman_info.state == BagState::BS_At_Spawn
                || g_bagman_info.state == BagState::BS_Dropped)
            && !find_player_with_bag_powerup()
            && (!g_bagman_info.bag_respawn_retry_timer.valid()
                || g_bagman_info.bag_respawn_retry_timer.elapsed())) {
            xlog::warn("bagman: bag item missing in state {} — attempting respawn at ({},{},{})",
                static_cast<int>(g_bagman_info.state),
                g_bagman_info.bag_pos.x, g_bagman_info.bag_pos.y, g_bagman_info.bag_pos.z);
            const rf::Vector3 retry_pos =
                g_bagman_info.state == BagState::BS_At_Spawn
                    ? g_bagman_info.spawn_pos
                    : g_bagman_info.bag_pos;
            spawn_bag_item(retry_pos, g_bagman_info.spawn_orient);
            if (g_bagman_info.bag_item_handle >= 0) {
                g_bagman_info.bag_respawn_retry_timer.invalidate();
                bagman_broadcast_state();
            } else {
                g_bagman_info.bag_respawn_retry_timer.set(2000);
            }
        }

        // No carrier. The engine handles physical pickup; we detect it
        // by checking who now holds the amp powerup.
        rf::Player* pickup_player = find_player_with_bag_powerup();
        if (pickup_player) {
            on_pickup(pickup_player);
        } else if (g_bagman_info.state == BagState::BS_Dropped &&
                   g_bagman_info.return_timer.valid() &&
                   g_bagman_info.return_timer.elapsed()) {
            on_return();
        }
    }
}

void bagman_on_player_disconnect(rf::Player* player)
{
    if (!gt_is_bagman_any()) return;
    if (g_bagman_info.carrier != player) return;

    if (rf::is_server) {
        // Server-authoritative: drop the bag at the carrier's position and
        // broadcast the new state.
        rf::Entity* ep = rf::entity_from_handle(player->entity_handle);
        if (ep) {
            drop_bag_from_entity(player, ep);
        } else {
            drop_bag_at_position(player, g_bagman_info.bag_pos);
        }
    }
    else {
        // Client: the carrier Player struct is about to be destroyed. Drop
        // our reference now so the render/HUD paths don't dereference freed
        // memory in the window before the next bagman state packet arrives.
        g_bagman_info.carrier = nullptr;
    }
}

void bagman_on_entity_will_die(rf::Entity* ep)
{
    if (!rf::is_server || !gt_is_bagman_any() || !ep) return;
    rf::Player* player = rf::player_from_entity_handle(ep->handle);
    if (!player || player != g_bagman_info.carrier) return;
    rf::Player* killer = rf::player_from_entity_handle(ep->killer_handle);
    if (killer && killer != player) {
        apply_effective_health_reward(killer, kCarrierKillEffectiveHealth);
    }

    drop_bag_from_entity(player, ep);
}

bool bagman_local_player_is_carrier()
{
    return gt_is_bagman_any() &&
    g_bagman_info.carrier != nullptr
    && rf::local_player == g_bagman_info.carrier;
}

bool bagman_viewer_is_carrier_first_person()
{
    if (!gt_is_bagman_any() || !g_bagman_info.carrier) return false;
    if (rf::local_player == g_bagman_info.carrier) return true;
    if (multi_spectate_is_first_person()
        && multi_spectate_get_target_player() == g_bagman_info.carrier) {
        return true;
    }
    return false;
}

// Extract the first LOD-0 VifLodMesh from a static VMesh by walking through
// the V3d header.
rf::VifLodMesh* static_vmesh_lod0(rf::VMesh* vmesh)
{
    if (!vmesh || vmesh->type != rf::MESH_TYPE_STATIC) return nullptr;
    auto* v3d = static_cast<rf::V3d*>(vmesh->instance);
    if (!v3d || v3d->num_meshes < 1 || !v3d->meshes) return nullptr;
    return v3d->meshes[0].vu;
}

// Walk the client's item list to find the bag pickup. bag_item_handle is
// server-only state, so clients have to look it up by item class.
rf::Item* find_client_side_bag_pickup_item()
{
    if (g_bagman_info.bag_item_type < 0) return nullptr;
    rf::Item* it = rf::item_list.next;
    while (it && it != &rf::item_list) {
        if (it->info_index == g_bagman_info.bag_item_type) {
            return it;
        }
        it = it->next;
    }
    return nullptr;
}

bool bagman_query_pickup_bag_outline(
    rf::VifLodMesh** out_lod_mesh, rf::Vector3* out_pos, rf::Matrix3* out_orient)
{
    if (!gt_is_bagman_any()) return false;
    if (g_bagman_info.state != BagState::BS_At_Spawn
        && g_bagman_info.state != BagState::BS_Dropped) return false;

    rf::Item* bag_item = find_client_side_bag_pickup_item();
    if (!bag_item || !bag_item->vmesh) return false;

    rf::VifLodMesh* lod = static_vmesh_lod0(bag_item->vmesh);
    if (!lod) return false;

    *out_lod_mesh = lod;
    *out_pos = bag_item->pos;
    // For spinning items the stock engine renders with an orient derived
    // from spin_angle, not item->orient. Mirror that so the outline rotates
    // in sync with the visible mesh.
    if (bag_item->info && (bag_item->info->flags & rf::IIF_SPINS_IN_MULTI)) {
        out_orient->set_from_angles(0.0f, 0.0f, -bag_item->spin_angle);
    }
    else {
        *out_orient = bag_item->orient;
    }
    return true;
}

void bagman_tick_pickup_spin()
{
    if (!gt_is_bagman_any()) return;
    if (g_bagman_info.state != BagState::BS_At_Spawn
        && g_bagman_info.state != BagState::BS_Dropped) return;

    rf::Item* bag = find_client_side_bag_pickup_item();
    if (!bag || !bag->info) return;
    if (!(bag->info->flags & rf::IIF_SPINS_IN_MULTI)) return;

    const float spin_rate = addr_as_ref<float>(0x005897A8);
    const float two_pi = addr_as_ref<float>(0x005894AC);
    bag->spin_angle += spin_rate * rf::frametime;
    if (bag->spin_angle > two_pi) {
        bag->spin_angle -= two_pi;
    }
}

bool bagman_query_carrier_bag_outline(
    rf::VifLodMesh** out_lod_mesh, rf::Vector3* out_pos, rf::Matrix3* out_orient)
{
    if (!gt_is_bagman_any()) return false;
    if (g_bagman_info.state != BagState::BS_Carried) return false;
    if (!g_bagman_info.carrier) return false;
    if (bagman_viewer_is_carrier_first_person()) return false;

    rf::Entity* ep = rf::entity_from_handle(g_bagman_info.carrier->entity_handle);
    if (!ep || !ep->vmesh) return false;
    if (rf::entity_is_dying(ep)) return false;

    if (!g_bag_carrier_mesh_load_attempted) {
        g_bag_carrier_mesh_load_attempted = true;
        g_bag_carrier_mesh = rf::vmesh_load(kBagMeshFilename, rf::MESH_TYPE_STATIC, -1);
    }
    if (!g_bag_carrier_mesh) return false;

    rf::VifLodMesh* lod = static_vmesh_lod0(g_bag_carrier_mesh);
    if (!lod) return false;

    // Carrier's $prop_flag in WORLD space.
    const int carrier_prop_idx = rf::vmesh_lookup_prop_point(ep->vmesh, "$prop_flag");
    if (carrier_prop_idx < 0) return false;

    rf::Vector3 carrier_prop_pos{};
    rf::Matrix3 carrier_prop_orient{};
    rf::vmesh_get_prop_point_transform(
        ep->vmesh, carrier_prop_idx, &ep->orient, &ep->pos,
        &carrier_prop_orient, &carrier_prop_pos);

    // Bag mesh's $prop_flag in BAG-LOCAL space.
    const int bag_prop_idx = rf::vmesh_lookup_prop_point(g_bag_carrier_mesh, "$prop_flag");
    if (bag_prop_idx < 0) return false;

    rf::Vector3 bag_prop_local_pos{};
    rf::Matrix3 bag_prop_local_orient{};
    rf::Vector3 zero_pos{0.0f, 0.0f, 0.0f};
    rf::Matrix3 ident = rf::identity_matrix;
    rf::vmesh_get_prop_point_transform(
        g_bag_carrier_mesh, bag_prop_idx, &ident, &zero_pos,
        &bag_prop_local_orient, &bag_prop_local_pos);

    // Solve for the bag world transform so its $prop_flag overlays the
    // carrier's $prop_flag. For an orthonormal rotation, inverse == transpose.
    rf::Matrix3 bag_prop_local_inv = bag_prop_local_orient;
    bag_prop_local_inv.transpose();
    rf::Matrix3 bag_orient_world = carrier_prop_orient;
    bag_orient_world.mul(bag_prop_local_inv);
    rf::Vector3 offset = bag_orient_world.transform_vector(bag_prop_local_pos);

    *out_lod_mesh = lod;
    *out_pos = carrier_prop_pos - offset;
    *out_orient = bag_orient_world;
    return true;
}

// Render the cosmetic bag mesh on the carrier's back. The green outline is
// queued separately by the d3d11 outline renderer (which compares this mesh's
// lod against the per-frame cached carrier-bag lod), so no MRF flag is needed
// here — that keeps the outline working even when the carrier is portal-culled.
CodeInjection bagman_render_carrier_attachment_patch{
    0x00421c08,
    [](auto& regs) {
        // The hooked entity must be the carrier for the transform to be
        // valid (bagman_query_carrier_bag_outline derives from the carrier
        // entity). Gate on that before doing the shared query.
        if (!gt_is_bagman_any() || !g_bagman_info.carrier) return;
        auto* ep = reinterpret_cast<rf::Entity*>(regs.esi.value);
        if (!ep || ep->handle != g_bagman_info.carrier->entity_handle) return;

        rf::VifLodMesh* lod = nullptr;
        rf::Vector3 bag_pos_world{};
        rf::Matrix3 bag_orient_world{};
        if (!bagman_query_carrier_bag_outline(&lod, &bag_pos_world, &bag_orient_world)) {
            return;
        }

        rf::MeshRenderParams params{};
        params.init_defaults();
        params.orient = bag_orient_world;
        rf::vmesh_render(g_bag_carrier_mesh, &bag_pos_world, &bag_orient_world, &params);
    },
};

void bagman_update_dynamic_light()
{
    if (g_bag_light_handle >= 0) {
        rf::gr::light_delete(g_bag_light_handle, 0);
        g_bag_light_handle = -1;
    }

    if (!gt_is_bagman_any()) return;
    if (g_bagman_info.state == BagState::BS_Carried) return;

    // Match stock game dynamic light pulse for amps/flags.
    const float pulse_period   = addr_as_ref<float>(0x005947A0);
    const float sine_amplitude = addr_as_ref<float>(0x005947A4);
    const float sine_base      = addr_as_ref<float>(0x00594798);
    const float radius         = addr_as_ref<float>(0x0059479C);
    if (pulse_period <= 0.0f) return;

    g_bag_light_pulse_phase = std::fmod(g_bag_light_pulse_phase + rf::frametime, pulse_period);

    constexpr float kTwoPi = 6.28318530718f;
    const float intensity = sine_base + sine_amplitude * std::sin(kTwoPi * g_bag_light_pulse_phase / pulse_period);

    rf::Vector3 pos = g_bagman_info.bag_pos;
    g_bag_light_handle = rf::gr::light_create_point(
        &pos,
        radius,
        intensity,
        0.0f, 1.0f, 0.0f,
        true,
        rf::gr::LightShadowcastCondition::SHADOWCAST_EDITOR,
        0);
}

CodeInjection bagman_carrier_no_amp_damage_patch1{
    0x004C60CE,
    [](auto& regs) {
        if (gt_is_bagman_any()) {
            regs.eip = 0x004C60DC;
        }
    },
};

CodeInjection bagman_carrier_no_amp_damage_patch2{
    0x00489246,
    [](auto& regs) {
        if (gt_is_bagman_any()) {
            regs.eip = 0x00489283;
        }
    },
};

CodeInjection bagman_carrier_no_amp_damage_patch3{
    0x0046F485,
    [](auto& regs) {
        if (gt_is_bagman_any()) {
            regs.eip = 0x0046F493;
        }
    },
};

CodeInjection bagman_carrier_no_amp_fire_sound_patch1{
    0x0042612B,
    [](auto& regs) {
        if (gt_is_bagman_any()) {
            regs.eip = 0x00426154;
        }
    },
};

CodeInjection bagman_carrier_no_amp_fire_sound_patch2{
    0x00426236,
    [](auto& regs) {
        if (gt_is_bagman_any()) {
            regs.eip = 0x0042625F;
        }
    },
};

CodeInjection bagman_carrier_no_amp_fire_sound_patch3{
    0x00426AA3,
    [](auto& regs) {
        if (gt_is_bagman_any()) {
            regs.eip = 0x00426AC3;
        }
    },
};

CodeInjection bagman_carrier_no_amp_fire_sound_patch4{
    0x0041AAA7,
    [](auto& regs) {
        if (gt_is_bagman_any()) {
            regs.eip = 0x0041AAD3;
        }
    },
};

CodeInjection bagman_carrier_no_amp_fire_sound_patch5{
    0x0041ABAF,
    [](auto& regs) {
        if (gt_is_bagman_any()) {
            regs.eip = 0x0041ABD8;
        }
    },
};

CodeInjection bagman_carrier_amp_pickup_sound_swap_patch{
    0x0042D1E4,
    [](auto& regs) {
        if (gt_is_bagman_any()) {
            *reinterpret_cast<int*>(static_cast<uintptr_t>(regs.esp)) = 0x40;
        }
    },
};

// Block dying entities from picking up the bag.
CodeInjection bagman_block_dying_bag_pickup_patch{
    0x0045959D,
    [](auto& regs) {
        if (!gt_is_bagman_any()) return;
        if (g_bagman_info.bag_item_handle < 0) return;
        auto* item = reinterpret_cast<rf::Item*>(regs.esi.value);
        if (!item || item->handle != g_bagman_info.bag_item_handle) return;
        auto* toucher = reinterpret_cast<rf::Entity*>(regs.edi.value);
        if (!toucher) return;
        if (rf::entity_is_dying(toucher)) {
            regs.eip = 0x0045995C;
        }
    },
};

CodeInjection bagman_suppress_amp_pickup_msg_patch{
    0x0045A100,
    [](auto& regs) {
        if (!gt_is_bagman_any()) return;
        if (g_bagman_info.bag_item_type < 0) return;
        auto* item_info = *reinterpret_cast<rf::ItemInfo**>(static_cast<uintptr_t>(regs.esp) + 8);
        if (item_info == &rf::item_info[g_bagman_info.bag_item_type]) {
            regs.eip = 0x0045A1E8;
        }
    },
};

FunHook<rf::Item*(int, const char*, int, int, const rf::Vector3*, rf::Matrix3*, int, bool, bool)>
    item_create_bagman_bag_mesh_swap_hook{
    0x00459100,
    [](int type, const char* name, int count, int parent_handle,
       const rf::Vector3* pos, rf::Matrix3* orient, int respawn_time,
       bool permanent, bool from_packet) -> rf::Item* {
        rf::Item* item = item_create_bagman_bag_mesh_swap_hook.call_target(
            type, name, count, parent_handle, pos, orient,
            respawn_time, permanent, from_packet);

        if (item && g_bag_mesh_exists && gt_is_bagman_any()
            && type == g_bagman_info.bag_item_type) {
            rf::item_restore_mesh(item, kBagMeshFilename);
        }
        return item;
    },
};

CodeInjection bagman_carrier_amp_light_color_patch{
    0x0041EC7C,
    [](auto& regs) {
        // Only change the color in bagman.
        if (!gt_is_bagman_any()) return;

        // Suppress dynamic light for dying players since amp (bag) item already emits a glow.
        // Without this, the glow would double up until the player fully dies.
        rf::Entity* ep = regs.esi;
        if (ep && rf::entity_is_dying(ep)) {
            regs.eax = static_cast<uint32_t>(-1);
            regs.eip = 0x0041EE21;
            return;
        }

        // Set light to green. Pulses from stock game code.
        const uint32_t esp = regs.esp;
        addr_as_ref<uint32_t>(esp + 12) = 0x00000000; // R = 0.0
        addr_as_ref<uint32_t>(esp + 16) = 0x3F800000; // G = 1.0
        addr_as_ref<uint32_t>(esp + 20) = 0x00000000; // B = 0.0
    },
};

void bagman_do_patch()
{
    // Remove 4x damage bonus for amp (bag) holder
    bagman_carrier_no_amp_damage_patch1.install();
    bagman_carrier_no_amp_damage_patch2.install();
    bagman_carrier_no_amp_damage_patch3.install();

    // Suppress the amped weapon-fire sound for amp (bag) holder
    bagman_carrier_no_amp_fire_sound_patch1.install();
    bagman_carrier_no_amp_fire_sound_patch2.install();
    bagman_carrier_no_amp_fire_sound_patch3.install();
    bagman_carrier_no_amp_fire_sound_patch4.install();
    bagman_carrier_no_amp_fire_sound_patch5.install();

    // Swap amp pickup sound (84) for flag pickup sound (64) in bagman
    bagman_carrier_amp_pickup_sound_swap_patch.install();

    // Suppress the "Damage Amp picked up" HUD message in bagman
    bagman_suppress_amp_pickup_msg_patch.install();

    // Block dying entities from picking up the bag
    bagman_block_dying_bag_pickup_patch.install();

    // Render the cosmetic bag mesh on the carrier each frame
    bagman_render_carrier_attachment_patch.install();

    // Bagman's dynamic light is green instead of purple
    bagman_carrier_amp_light_color_patch.install();

    // Amp (bag) pickup uses a new mesh in bagman mode
    item_create_bagman_bag_mesh_swap_hook.install();
}
