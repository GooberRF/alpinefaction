#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <xlog/xlog.h>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include "sprays.h"
#include "alpine_packets.h"
#include "server.h"
#include "multi.h"
#include "../misc/player.h"
#include "../misc/alpine_settings.h"
#include "../sound/sound.h"
#include "../os/console.h"
#include "../os/os.h"
#include "../rf/geometry.h"
#include "../rf/collide.h"
#include "../rf/level.h"
#include "../rf/bmpman.h"
#include "../rf/entity.h"
#include "../rf/multi.h"
#include "../rf/os/timer.h"
#include "../rf/math/vector.h"
#include "../rf/math/matrix.h"
#include "../rf/player/player.h"
#include "../rf/player/camera.h"

namespace
{
    // Full clip-box size of a spray decal in world units.
    constexpr float kSprayExtent = 2.0f;

    // Max distance from the player to the sprayed surface.
    constexpr float kSprayMaxDistance = 3.0f;

    // In single player there is no network player id, this is a stand-in.
    constexpr uint8_t kSinglePlayerSprayerId = 0;

    // Spray textures, stretched to 1:1 aspect.
    constexpr const char* g_spray_table[] = {
        "dec_af_csp01.png",
        "rf_red01_A.tga",
        "rebel_insignia.tga",
        "dec_af_csp04.png",
        "mtl_bronze_ultor01_A.tga",
        "ultorlogo01_A.tga",
        "ultor_blue01_A.tga",
        "mtl_dm17ultor01_A.tga",
        "dec_af_csp05.png",
        "dec_af_csp06.tga",
        "dieultor_A.tga",
        "mtl_eos_rules01.tga",
        "mtl_kill_guards01.tga",
        "mtl_plague01.tga",
        "dec_af_csp07.png",
        "mtl_ultsux01.tga",
        "sucks_to_be_you.tga",
        "sld_greenX_A.tga",
        "GeoPointer01.tga",
        "GeoPointer02.tga",
        "dec_af_csp09.tga",
        "mtl_apc01_dirty.tga",
        "mtl_ultor_prop002.tga",
        "mtl_high01_dirty.tga",
        "mtl_horseplay.tga",
        "mtl_highvoltage.tga",
        "mtl_donotuseelv.tga",
        "mtl_electric_fence01.tga",
        "mtl_emergency_room01.tga",
        "mtl_cavein01.tga",
        "mtl_biohazard02.tga",
        "mtl_shower_limit.tga",
        "mtl_no_miners01.tga",
        "mtl_no_admit.tga",
        "mtl_obey_rules01.tga",
        "mtl_no_horsing02.tga",
        "mtl_heavy_equipment01.tga",
        "blastedcem01_A.tga",
        "BlastHole.tga",
        "leaf.tga",
        "AcidBlob.tga",
        "AcidBlob02.tga",
        "brownsplat01_A.tga",
        "darkbluesplat01_A.tga",
        "dec_af_csp08.png",
        "dec_af_csp10.tga",
        "dec_af_csp02.png",
        "sld_merc_decal01.tga",
        "dec_af_csp03.png",
        "circle.tga",
        "klown_a.tga",
        "mtl_cargo_logo01.tga",
    };
    constexpr size_t kSprayCount = std::size(g_spray_table);

    // Lazily-loaded, cached bitmap handles (nullopt = not yet attempted, -1 = load failed).
    std::array<std::optional<int>, kSprayCount> g_spray_bitmaps;

    struct ClientSprayState
    {
        uint16_t texture_id = 0;
        rf::Vector3 pos{};
        rf::Vector3 normal{};
        rf::GDecal* decal = nullptr; // may be null (display off or creation failed)
    };

    struct ServerSprayState
    {
        uint16_t texture_id = 0;
        rf::Vector3 pos{};
        rf::Vector3 normal{};
    };

    std::unordered_map<uint8_t, ClientSprayState> g_client_sprays;
    std::unordered_map<uint8_t, ServerSprayState> g_server_sprays;

    int get_cached_spray_bitmap(uint16_t id)
    {
        if (id >= kSprayCount) {
            return -1;
        }
        if (!g_spray_bitmaps[id].has_value()) {
            int bm = rf::bm::load(g_spray_table[id], -1, true);
            if (bm >= 0) {
                rf::bm::texture_add_ref(bm);
            }
            else {
                xlog::warn("sprays: failed to load spray texture '{}'", g_spray_table[id]);
            }
            g_spray_bitmaps[id] = bm;
        }
        return *g_spray_bitmaps[id];
    }

    bool is_finite_vec(const rf::Vector3& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    // Build an explicit, upright orientation for the decal.
    // We cannot use Matrix3::make_quick here because it derives rvec/uvec from the matrix's own
    // zero-initialized fvec rather than from the passed vector, yielding a fixed/degenerate basis.
    rf::Matrix3 build_spray_orient(const rf::Vector3& normal)
    {
        rf::Vector3 f = normal;
        f.normalize_safe();
        const rf::Vector3 ref = (std::fabs(f.y) < 0.98f) ? rf::Vector3{0.0f, 1.0f, 0.0f}
                                                         : rf::Vector3{1.0f, 0.0f, 0.0f};
        rf::Vector3 r = ref.cross(f);
        r.normalize_safe();
        rf::Vector3 u = f.cross(r); // unit length (f, r orthonormal)

        rf::Matrix3 m;
        m.rvec = r;
        m.uvec = u;
        m.fvec = f;
        return m;
    }

    bool spray_level_active()
    {
        return (rf::level.flags & rf::LEVEL_LOADED) != 0 && rf::g_level_solid != nullptr;
    }

    void spray_destroy_decal(rf::GDecal* decal)
    {
        if (decal && spray_level_active()) {
            rf::g_decal_destroy(decal);
        }
    }

    rf::GDecal* create_spray_decal(uint16_t texture_id, const rf::Vector3& pos, const rf::Vector3& normal, bool play_sound)
    {
        if (rf::is_dedicated_server || !spray_level_active()) {
            return nullptr;
        }
        int bm = get_cached_spray_bitmap(texture_id);
        if (bm < 0) {
            return nullptr;
        }

        rf::Vector3 n = normal;
        n.normalize_safe();

        rf::GDecalCreateInfo dci{};
        dci.pos = pos;
        dci.orient = build_spray_orient(n);
        dci.extents = {kSprayExtent, kSprayExtent, kSprayExtent};
        dci.texture = bm;
        dci.room = rf::find_room(rf::g_level_solid, &pos);
        dci.alpha = 255;
        dci.flags = 0;
        dci.object_handle = -1;
        dci.solid = rf::g_level_solid;
        dci.scale = 1.0f;

        rf::GDecal* decal = rf::g_decal_add(&dci);

        if (decal && play_sound) {
            int sound_id = get_spray_sound_id();
            if (sound_id >= 0) {
                play_local_sound_3d(static_cast<uint16_t>(sound_id), pos, 0, 1.0f);
            }
        }
        return decal;
    }

    // Clean up spray decals.
    FunHook<void(rf::GDecal*)> g_decal_destroy_hook{
        0x004D6C50,
        [](rf::GDecal* decal) {
            g_decal_destroy_hook.call_target(decal);
            for (auto& [player_id, state] : g_client_sprays) {
                if (state.decal == decal) {
                    state.decal = nullptr;
                }
            }
        },
    };

    // Newest decal renders on top. Stock function inserts a new decal poly at the HEAD of
    // its face's poly list and the renderer draws that list head-first, so overlapping decals show
    // the OLDEST on top. Append at the TAIL instead so the most recently created decal draws last
    // (on top).
    void __fastcall decal_face_poly_insert_newest_on_top(rf::GFace* face, int, rf::DecalPoly* poly)
    {
        poly->next_for_face = nullptr;
        if (face->decal_list == nullptr) {
            face->decal_list = poly;
        }
        else {
            rf::DecalPoly* node = face->decal_list;
            while (node->next_for_face != nullptr) {
                node = node->next_for_face;
            }
            node->next_for_face = poly;
        }
    }

    FunHook<void __fastcall(rf::GFace*, int, rf::DecalPoly*)> decal_face_poly_insert_hook{
        0x004E36D0,
        decal_face_poly_insert_newest_on_top,
    };

    // Ensure sprays don't survive a level change in SP or MP.
    CodeInjection level_set_to_load_clear_sprays_patch{
        0x0045E2E0,
        []() {
            sprays_level_init();
        },
    };

    ConsoleCommand2 cl_sprays_cmd{
        "cl_sprays",
        []() {
            g_alpine_game_config.spray_display = !g_alpine_game_config.spray_display;
            sprays_apply_display_toggle();
            rf::console::print("Local spray display is {}", g_alpine_game_config.spray_display ? "enabled" : "disabled");
        },
        "Toggle whether players' sprays are displayed locally",
        "cl_sprays",
    };

    ConsoleCommand2 spray_cmd{
        "spray",
        [](std::optional<int> spray_id) {
            if (spray_id) {
                if (*spray_id < 0 || !is_valid_spray_id(static_cast<uint16_t>(*spray_id))) {
                    rf::console::print("Invalid spray id {}. Valid range is 0-{}.", *spray_id, spray_count() - 1);
                    return;
                }
                g_alpine_game_config.set_selected_spray_index(*spray_id);
            }
            rf::console::print("Selected spray: {} ({})", g_alpine_game_config.selected_spray_index,
                spray_texture_name(static_cast<uint16_t>(g_alpine_game_config.selected_spray_index)));
            if (!spray_id) {
                rf::console::print("Available sprays:");
                for (int i = 0; i < spray_count(); ++i) {
                    rf::console::print("  {} - {}", i, spray_texture_name(static_cast<uint16_t>(i)));
                }
            }
        },
        "Select the spray to use, or list all available sprays",
        "spray [id]",
    };
}

int spray_count()
{
    return static_cast<int>(kSprayCount);
}

bool is_valid_spray_id(uint16_t spray_id)
{
    return spray_id < kSprayCount;
}

const char* spray_texture_name(uint16_t spray_id)
{
    return is_valid_spray_id(spray_id) ? g_spray_table[spray_id] : nullptr;
}

int spray_get_bitmap(uint16_t id)
{
    return get_cached_spray_bitmap(id);
}

void sprays_do_patch()
{
    g_decal_destroy_hook.install();
    decal_face_poly_insert_hook.install();
    level_set_to_load_clear_sprays_patch.install();
    cl_sprays_cmd.register_cmd();
    spray_cmd.register_cmd();
}

void sprays_level_init()
{
    g_server_sprays.clear();
    g_client_sprays.clear();
}

void sprays_on_player_destroyed(rf::Player* player)
{
    if (!player || !player->net_data) {
        return;
    }
    const uint8_t player_id = player->net_data->player_id;

    if (rf::is_server) {
        g_server_sprays.erase(player_id);
    }

    auto it = g_client_sprays.find(player_id);
    if (it != g_client_sprays.end()) {
        spray_destroy_decal(it->second.decal); // no-op if the level is being torn down
        g_client_sprays.erase(player_id);
    }
}

// Sync sprays for late joiners.
void sprays_force_state_sync_to(rf::Player* player)
{
    if (!rf::is_server || !player || !player->net_data) {
        return;
    }
    if (!is_player_minimum_af_client_version(player, 1, 4, 0)) {
        return;
    }
    for (const auto& [player_id, state] : g_server_sprays) {
        af_send_spray_to_player(player_id, state.texture_id, state.pos, state.normal, AF_SPRAY_FLAG_SILENT, player);
    }
}

void sprays_handle_spray_request(rf::Player* player, uint16_t texture_id, const rf::Vector3& pos_in, const rf::Vector3& normal_in)
{
    if (!rf::is_server || !player || !player->net_data) {
        return;
    }

    //xlog::info("sprays: request from '{}' (id={}, pos=({:.2f}, {:.2f}, {:.2f}))", player->name, texture_id, pos_in.x, pos_in.y, pos_in.z);

    if (!server_sprays_enabled()) {
        //xlog::info("sprays: rejected '{}': sprays disabled in config", player->name);
        return;
    }

    if (!is_valid_spray_id(texture_id)) {
        //xlog::warn("sprays: rejected '{}': invalid spray id {}", player->name, texture_id);
        //af_send_server_console_msg("Spray rejected: invalid spray", player);
        return;
    }

    if (!is_finite_vec(pos_in) || !is_finite_vec(normal_in)) {
        //xlog::warn("sprays: rejected '{}': non-finite pos/normal", player->name);
        //af_send_server_console_msg("Spray rejected: invalid data", player);
        return;
    }

    rf::Vector3 normal = normal_in;
    const float nlen = normal.len();
    if (!std::isfinite(nlen) || nlen < 0.0001f) {
        //xlog::warn("sprays: rejected '{}': degenerate normal", player->name);
        //af_send_server_console_msg("Spray rejected: invalid data", player);
        return;
    }
    normal /= nlen;

    rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
    if (!entity) {
        //xlog::info("sprays: rejected '{}': no live entity", player->name);
        return; // no live entity to spray from
    }

    // Use the closer of feet and eye to the hit point. Measuring only from the feet
    // (entity->pos) falsely rejects a player standing against a tall wall who aims high on it;
    // the eye (entity->eye_pos, maintained every server tick for AI aiming) covers that. Taking
    // the min keeps the anti-abuse cap intact while staying robust even if eye_pos were stale.
    const float dist = std::min(entity->pos.distance_to(pos_in), entity->eye_pos.distance_to(pos_in));
    if (dist > kSprayMaxDistance + 1.0f) {
        //xlog::info("sprays: rejected '{}': too far ({:.2f} > {:.2f})", player->name, dist, kSprayMaxDistance);
        af_send_automated_chat_msg("Spray rejected: too far from surface", player);
        return;
    }

    const int64_t now = timer::get_i64(1000);
    if (player->last_spray_ms && (now - *player->last_spray_ms) < server_spray_cooldown_ms()) {
        //xlog::info("sprays: rejected '{}': on cooldown ({} ms since last)", player->name, now - *player->last_spray_ms);
        af_send_automated_chat_msg("Spray rejected: on cooldown", player);
        return;
    }

    // Spray accepted.
    player->last_spray_ms = now;
    const uint8_t player_id = player->net_data->player_id;
    g_server_sprays[player_id] = ServerSprayState{texture_id, pos_in, normal};
    //xlog::info("sprays: accepted spray from '{}' (player_id={}), broadcasting", player->name, player_id);

    // Broadcast to every client (including the requester).
    // On a listen server also render locally for the host.
    af_broadcast_spray(player_id, texture_id, pos_in, normal);
    if (!rf::is_dedicated_server) {
        sprays_apply_client_state(player_id, texture_id, pos_in, normal, true);
    }
}

void sprays_apply_client_state(uint8_t player_id, uint16_t texture_id, const rf::Vector3& pos, const rf::Vector3& normal, bool play_sound)
{
    if (rf::is_dedicated_server) {
        return; // dedicated servers do not render
    }
    if (!is_valid_spray_id(texture_id)) {
        xlog::debug("sprays: ignoring spray with unknown id {} for player_id {}", texture_id, player_id);
        return; // ignore unknown ids (forward compatibility)
    }

    // Replace this player's previous spray everywhere.
    auto it = g_client_sprays.find(player_id);
    if (it != g_client_sprays.end()) {
        spray_destroy_decal(it->second.decal); // hook nulls the pointer; no-op during teardown
    }

    ClientSprayState state{};
    state.texture_id = texture_id;
    state.pos = pos;
    state.normal = normal;
    state.decal = g_alpine_game_config.spray_display
        ? create_spray_decal(texture_id, pos, normal, play_sound)
        : nullptr;

    if (g_alpine_game_config.spray_display && !state.decal) {
        xlog::warn("sprays: decal creation failed for player_id {} (id={}, pos=({:.2f}, {:.2f}, {:.2f}))",
            player_id, texture_id, pos.x, pos.y, pos.z);
    }
    else {
        //xlog::info("sprays: applied spray for player_id {} (id={}, decal={})", player_id, texture_id, state.decal != nullptr);
    }

    g_client_sprays[player_id] = state;
}

void sprays_apply_display_toggle()
{
    if (rf::is_dedicated_server) {
        return;
    }
    if (g_alpine_game_config.spray_display) {
        for (auto& [player_id, state] : g_client_sprays) {
            if (!state.decal) {
                state.decal = create_spray_decal(state.texture_id, state.pos, state.normal, false);
            }
        }
    }
    else {
        for (auto& [player_id, state] : g_client_sprays) {
            spray_destroy_decal(state.decal); // hook nulls state.decal
            state.decal = nullptr;
        }
    }
}

// On-screen feedback for user-visible guard failures.
static void spray_chat_feedback(const char* text)
{
    if (!rf::is_multi) {
        rf::console::print("{}", text);
        return;
    }
    rf::String msg{text};
    rf::String prefix;
    rf::multi_chat_print(msg, rf::ChatMsgColor::white_white, prefix);
}

void sprays_handle_spray_action()
{
    xlog::debug("sprays: spray action fired");

    // Sprays are usable in single player as well as multiplayer.
    const bool single_player = !rf::is_multi;

    // Whether sprays are allowed: a networked client learns this from the server's af_server_info
    // (delivered in the join-accept); the listen-server host IS the server, so it consults its own
    // config directly (it never receives an af_server_info for itself). Single player has no
    // server, so it is always allowed here (still subject to the local toggle / valid selection).
    if (!single_player) {
        if (rf::is_server) {
            if (!server_sprays_enabled()) {
                spray_chat_feedback("Sprays are disabled on this server");
                return;
            }
        }
        else {
            const auto& server_info = get_af_server_info();
            if (!server_info.has_value() || !server_info->allow_sprays) {
                spray_chat_feedback("This server does not allow sprays");
                return;
            }
        }
    }

    if (!g_alpine_game_config.spray_display) {
        return;
    }

    const uint16_t spray_id = static_cast<uint16_t>(g_alpine_game_config.selected_spray_index);
    if (!is_valid_spray_id(spray_id)) {
        return;
    }

    // Local re-request throttle to avoid packet spam; the real cooldown is server-side.
    static std::optional<int64_t> last_request_ms;
    const int64_t now_ms = timer::get_i64(1000);
    if (last_request_ms && now_ms - *last_request_ms < 1000) {
        return;
    }

    rf::Player* player = rf::local_player;
    if (!player || !player->cam || !rf::level.geometry) {
        return;
    }

    const rf::Vector3 eye = rf::camera_get_pos(player->cam);
    const rf::Matrix3 orient = rf::camera_get_orient(player->cam);

    // Trace against the level's static solid geometry for a spray location.
    rf::Vector3 p0 = eye;
    rf::Vector3 p1 = eye + orient.fvec * 50.0f; // server enforces the real max distance
    rf::GCollisionOutput out;
    const bool hit = rf::collide_linesegment_level_solid(p0, p1, 0, &out);
    if (!hit || out.num_hits == 0) {
        xlog::debug("sprays: raycast hit no level geometry within 50m");
        rf::console::print("No surface in view to spray");
        return;
    }

    last_request_ms = now_ms;
    xlog::debug("sprays: sending spray request (id={}, hit=({:.2f}, {:.2f}, {:.2f}), normal=({:.2f}, {:.2f}, {:.2f}), dist={:.2f})",
        spray_id, out.hit_point.x, out.hit_point.y, out.hit_point.z,
        out.normal.x, out.normal.y, out.normal.z, eye.distance_to(out.hit_point));

    // Single player: no server and no networking, so just place the spray locally for yourself.
    // A networked client sends the request to the server; the listen-server host has no separate
    // server to receive a packet, so it runs the server-side handler directly in-process.
    if (single_player) {
        sprays_apply_client_state(kSinglePlayerSprayerId, spray_id, out.hit_point, out.normal, true);
    }
    else if (rf::is_server) {
        sprays_handle_spray_request(rf::local_player, spray_id, out.hit_point, out.normal);
    }
    else {
        af_send_spray_request(spray_id, out.hit_point, out.normal);
    }
}
