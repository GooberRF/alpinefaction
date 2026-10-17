#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <format>
#include <string>
#include <vector>
#include <xlog/xlog.h>
#include <patch_common/CodeInjection.h>
#include <common/utils/list-utils.h>
#include <common/utils/string-utils.h>
#include "vehicle.h"
#include "vehicle_markers.h"
#include "../alpine_packets.h"
#include "../multi.h"
#include "../../graphics/gr.h"
#include "../../graphics/gr_ghost_mesh.h"
#include "../../hud/hud_internal.h"
#include "../../hud/hud_world.h"
#include "../../hud/multi_spectate.h"
#include "../../misc/alpine_settings.h"
#include "../../misc/level.h"
#include "../../os/os.h"
#include "../../rf/clutter.h"
#include "../../rf/entity.h"
#include "../../rf/geometry.h"
#include "../../rf/gr/gr.h"
#include "../../rf/level.h"
#include "../../rf/multi.h"
#include "../../rf/os/frametime.h"
#include "../../rf/player/camera.h"
#include "../../rf/player/player.h"
#include "../../rf/vmesh.h"

namespace
{
    constexpr float marker_fade_start_dist = 120.0f; // fully bright up to here
    constexpr float marker_max_dist = 200.0f;        // nothing is drawn beyond here
    constexpr float marker_fallback_height = 2.5f;   // anchor lift when the hull mesh is absent
    constexpr int64_t marker_urgent_window_ms = 3000;
    constexpr int64_t marker_spawn_flash_ms = 1000;
    constexpr float marker_pulse_hz = 2.0f;
    constexpr float ghost_alpha_below = 0.55f;
    constexpr float ghost_alpha_above = 0.15f;
    constexpr float ghost_pulse_amplitude = 0.15f;
    constexpr float marker_label_alpha = 223.0f;
    constexpr float marker_alive_line_alpha = 0.45f;
    // World-unit quad sizes and anchor offsets, in the same convention as the KOTH hill labels.
    constexpr float marker_label_height = 0.40f;
    constexpr float marker_line2_height = 0.50f;
    constexpr float marker_label_offset = 0.62f;
    constexpr float marker_line2_offset = 0.16f;
    constexpr float marker_anchor_margin = 0.5f; // both lines are depth-tested, so keep them clear of the hull
    constexpr float marker_urgent_scale_pulse = 0.08f;
    constexpr float marker_bar_width = 0.90f;
    constexpr float marker_bar_height = 0.18f;
    constexpr float marker_bar_countdown_gap = 0.03f;
    constexpr float marker_bar_back_alpha = 0.45f;
    constexpr float marker_bar_fill_alpha = 0.75f;
    constexpr float marker_bar_full_epsilon = 0.5f;

    struct MarkerMeshEntry
    {
        std::string filename;
        rf::VMesh* mesh;
        // vmesh_create_anim_fx allocates per-instance anim state vmesh_free is the only release for;
        // vmesh_load's v3d data is the level-owned cache the jeep tire mesh deliberately only drops.
        bool anim_fx;
    };

    struct FactoryMarker
    {
        rf::VMesh* mesh = nullptr; // borrowed from g_marker_meshes; the level owns the mesh itself
        rf::Vector3 bbox_min{};
        rf::Vector3 bbox_max{};
        bool has_bbox = false;
        std::string name;       // display label, built once
        bool is_turret = false; // resolved once at level init; decides the second line's wording
        NameLabelTex name_tex;  // built lazily at the first draw
        NameLabelTex line2_tex; // rebuilt only when the displayed second line changes
        rf::GRoom* room = nullptr; // the pass that draws this marker; resolved on the first draw
        bool room_resolved = false;
    };

    // One countdown label per hull with a running auto-return, keyed by local object handle.
    struct HullMarker
    {
        int handle = -1;
        NameLabelTex tex;
    };

    // A hull below max life, or fading out since it got back to max.
    struct HullHealthBar
    {
        int handle = -1;
        int64_t full_since_ms = -1; // -1 while below max
        int64_t rise_since_ms = 0;  // when the countdown's slide up over this bar began
        float rise_from = 0.0f;     // the countdown's lift share when that slide began
    };

    // Both sized once per level; never resized while rendering.
    std::vector<FactoryMarker> g_markers;
    std::vector<MarkerMeshEntry> g_marker_meshes;
    // Bounded by the live hull count, which the factory count bounds in turn.
    std::vector<HullMarker> g_hull_markers;
    std::vector<std::pair<int, int64_t>> g_hull_scratch; // reused; never freed while rendering
    int g_hull_scratch_frame = -1;
    std::vector<HullHealthBar> g_health_bars; // bounded by the live hull count
    rf::Entity* g_viewer_hull = nullptr;
    int g_viewer_hull_frame = -1;

    rf::VMesh* marker_resolve_mesh(const char* filename)
    {
        if (!filename || filename[0] == '\0') {
            return nullptr;
        }
        for (const MarkerMeshEntry& e : g_marker_meshes) {
            if (e.filename == filename) {
                return e.mesh;
            }
        }
        // vmesh_load hard-sets STATIC, which a .vfx must not be built as.
        const bool anim_fx = string_iends_with(filename, ".vfx");
        rf::VMesh* mesh = anim_fx ? rf::vmesh_create_anim_fx(filename, -1)
                                  : rf::vmesh_load(filename, rf::MESH_TYPE_STATIC, -1);
        if (!mesh) {
            xlog::warn("[vehicle] respawn marker: failed to load hull mesh '{}'", filename);
        }
        g_marker_meshes.push_back({std::string{filename}, mesh, anim_fx});
        return mesh;
    }

    std::string marker_display_name(int entity_type)
    {
        const char* base = vehicle_class_display_name(vehicle_damage_class_from_type(entity_type));
        if (!base || base[0] == '\0') {
            return "Vehicle";
        }
        std::string out{base};
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
        return out;
    }

    // Bare whole seconds, never M:SS - the auto-return window is read as a plain count.
    std::string marker_seconds_text(int64_t ms_left)
    {
        return std::format("{}", static_cast<int>((std::max<int64_t>(ms_left, 0) + 999) / 1000));
    }

    // The label slot for this hull, created on first use; the caller has already bounded the count.
    NameLabelTex& hull_marker_tex(int handle)
    {
        for (HullMarker& h : g_hull_markers) {
            if (h.handle == handle) {
                return h.tex;
            }
        }
        g_hull_markers.push_back(HullMarker{handle, NameLabelTex{}});
        return g_hull_markers.back().tex;
    }

    // A detail room is drawn inside the room it hangs off, so that is the pass its marker joins.
    rf::GRoom* marker_render_room(rf::GRoom* room)
    {
        if (room && room->is_detail && room->room_to_render_with) {
            return room->room_to_render_with;
        }
        return room;
    }

    bool room_is_rendering(rf::GRoom* room)
    {
        if (!room) {
            return false;
        }
        rf::GRoom** rooms = nullptr;
        int num_rooms = 0;
        rf::g_get_room_render_list(&rooms, &num_rooms);
        for (int i = 0; i < num_rooms; ++i) {
            if (rooms[i] == room) {
                return true;
            }
        }
        return false;
    }

    // A marker belongs to the pass of its own room, which runs before that room's liquid surface so
    // the water blends over it. The late pass takes only what no room pass will draw.
    bool marker_pass_takes(rf::GRoom* marker_room, rf::GRoom* room_filter)
    {
        if (room_filter) {
            return marker_room == room_filter;
        }
        return !room_is_rendering(marker_room);
    }

    rf::GRoom* factory_marker_room(FactoryMarker& m, const rf::Vector3& anchor)
    {
        if (!m.room_resolved) {
            m.room_resolved = true;
            m.room = marker_render_room(rf::g_level_solid ? rf::find_room(rf::g_level_solid, &anchor) : nullptr);
        }
        return m.room;
    }

    // Releases the label of every hull that no longer has a running auto-return.
    void hull_markers_retain(const std::vector<std::pair<int, int64_t>>& live)
    {
        for (std::size_t i = g_hull_markers.size(); i-- > 0;) {
            const int handle = g_hull_markers[i].handle;
            const bool keep = std::any_of(live.begin(), live.end(),
                                          [handle](const auto& e) { return e.first == handle; });
            if (keep) {
                continue;
            }
            world_hud_release_text_label(g_hull_markers[i].tex);
            g_hull_markers.erase(g_hull_markers.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    // World-height extent of the oriented box's eight corners, relative to its origin.
    void rotated_bbox_height_range(const rf::Vector3& bbox_min, const rf::Vector3& bbox_max,
                                   const rf::Matrix3& orient, float& out_bottom, float& out_top)
    {
        out_bottom = std::numeric_limits<float>::max();
        out_top = std::numeric_limits<float>::lowest();
        for (int c = 0; c < 8; ++c) {
            const float lx = (c & 1) ? bbox_max.x : bbox_min.x;
            const float ly = (c & 2) ? bbox_max.y : bbox_min.y;
            const float lz = (c & 4) ? bbox_max.z : bbox_min.z;
            const float y = orient.rvec.y * lx + orient.uvec.y * ly + orient.fvec.y * lz;
            out_bottom = std::min(out_bottom, y);
            out_top = std::max(out_top, y);
        }
    }

    // Highest world-space Y of the local bbox once the object's orient is applied, relative to its origin.
    float rotated_bbox_top(const rf::Vector3& bbox_min, const rf::Vector3& bbox_max, const rf::Matrix3& orient)
    {
        float bottom = 0.0f;
        float top = 0.0f;
        rotated_bbox_height_range(bbox_min, bbox_max, orient, bottom, top);
        return top;
    }

    // Top of the hull in world units above its origin, from its own mesh bbox.
    float hull_anchor_height(rf::Entity* ep)
    {
        rf::Vector3 bbox_min{};
        rf::Vector3 bbox_max{};
        if (ep->vmesh) {
            rf::vmesh_get_bbox(ep->vmesh, &bbox_min, &bbox_max);
            if (bbox_max.y > bbox_min.y) {
                return rotated_bbox_top(bbox_min, bbox_max, ep->orient);
            }
        }
        return marker_fallback_height;
    }

    // Geometry past the level's far clip is culled away, so anything drawn out there hangs in the void.
    bool marker_beyond_far_clip(float distance)
    {
        return distance > level_projection_far();
    }

    // Alpha for a marker at this position; 0 also means "culled", so callers need no second test.
    float marker_distance_fade(const rf::Vector3& pos, const rf::Vector3& cam_pos)
    {
        const float distance = pos.distance_to(cam_pos);
        if (distance > marker_max_dist || marker_beyond_far_clip(distance)) {
            return 0.0f;
        }
        if (distance <= marker_fade_start_dist) {
            return 1.0f;
        }
        return std::clamp(1.0f - (distance - marker_fade_start_dist)
                                     / (marker_max_dist - marker_fade_start_dist),
                          0.0f, 1.0f);
    }

    // 0..1, peaking twice a second. The 2 Hz period divides 1000 ms exactly, so the modulo is lossless.
    float marker_pulse(int64_t now)
    {
        const float t = static_cast<float>(now % 1000) / 1000.0f;
        return 0.5f + 0.5f * std::sin(vehicle_two_pi * marker_pulse_hz * t);
    }

    float health_bar_fade(const HullHealthBar& b, int64_t now)
    {
        if (b.full_since_ms < 0) {
            return 1.0f;
        }
        return std::clamp(1.0f - static_cast<float>(now - b.full_since_ms) / hud_vehicle_bar_fade_ms, 0.0f, 1.0f);
    }

    // The bar's visibility, and the share of its height the countdown is lifted by: rises as the bar
    // appears, falls as it fades.
    float health_bar_lift(const HullHealthBar& b, int64_t now)
    {
        const float t = std::clamp(static_cast<float>(now - b.rise_since_ms) / hud_vehicle_bar_rise_ms, 0.0f, 1.0f);
        return std::min(b.rise_from + (1.0f - b.rise_from) * t, health_bar_fade(b, now));
    }

    // The hull whose own bar the HUD already shows: the local player's, or the followed spectate target's.
    rf::Entity* health_bar_viewer_hull()
    {
        if (g_viewer_hull_frame == rf::frame_count) {
            return g_viewer_hull;
        }
        rf::Player* viewer = rf::local_player;
        if (multi_spectate_is_spectating() && !multi_spectate_is_freelook()) {
            if (rf::Player* target = multi_spectate_get_target_player()) {
                viewer = target;
            }
        }
        g_viewer_hull = viewer ? vehicle_ridden_hull(rf::entity_from_handle(viewer->entity_handle)) : nullptr;
        g_viewer_hull_frame = rf::frame_count;
        return g_viewer_hull;
    }

    // Once a frame: track every live hull below max life and start the fade of any back at max. The
    // viewer's hull gets no entry, so leaving it fades its bar in afresh.
    void health_bars_refresh(int64_t now)
    {
        const rf::Entity* const viewer_hull = health_bar_viewer_hull();
        for (rf::Entity& ep : DoublyLinkedList{rf::entity_list}) {
            if (&ep == viewer_hull || !vehicle_is_synced_entity_type(&ep) || rf::entity_is_dying(&ep)) {
                continue;
            }
            const float max_life = vehicle_hud_max_life(&ep);
            if (max_life <= 0.0f) {
                continue;
            }
            const bool damaged = vehicle_hud_life(&ep) < max_life - marker_bar_full_epsilon;
            auto it = std::find_if(g_health_bars.begin(), g_health_bars.end(),
                                   [&ep](const HullHealthBar& b) { return b.handle == ep.handle; });
            if (damaged) {
                if (it == g_health_bars.end()) {
                    g_health_bars.push_back(HullHealthBar{ep.handle, -1, now, 0.0f});
                }
                else if (it->full_since_ms >= 0) {
                    it->rise_from = health_bar_lift(*it, now);
                    it->rise_since_ms = now;
                    it->full_since_ms = -1;
                }
            }
            else if (it != g_health_bars.end() && it->full_since_ms < 0) {
                it->full_since_ms = now;
            }
        }
        std::erase_if(g_health_bars, [now, viewer_hull](const HullHealthBar& b) {
            rf::Entity* ep = rf::entity_from_handle(b.handle);
            return ep == viewer_hull || !vehicle_is_synced_entity_type(ep) || rf::entity_is_dying(ep)
                || (b.full_since_ms >= 0 && now - b.full_since_ms >= hud_vehicle_bar_fade_ms);
        });
    }

    // 0 for a hull with no bar drawn over it, so its countdown keeps the plain anchor.
    float health_bar_countdown_lift(const rf::Entity* ep, const rf::Entity* viewer_hull, int64_t now)
    {
        if (!g_alpine_game_config.vehicle_health_bars || ep == viewer_hull) {
            return 0.0f;
        }
        for (const HullHealthBar& b : g_health_bars) {
            if (b.handle == ep->handle) {
                return health_bar_lift(b, now);
            }
        }
        return 0.0f;
    }

    // Left-aligned fill beside the dark remainder; the two quads never overlap.
    void render_health_bar(const rf::Vector3& anchor, float frac, float alpha)
    {
        rf::ubyte r, g, b, a;
        hud_vehicle_bar_fill_color(frac, r, g, b, a);
        const float fill_w = marker_bar_width * frac;
        const float back_w = marker_bar_width - fill_w;
        if (fill_w > 1e-4f) {
            do_render_world_hud_rect(anchor, marker_line2_offset, -0.5f * back_w, fill_w, marker_bar_height,
                                     WorldHUDRenderMode::no_overdraw, false, true,
                                     rf::Color{r, g, b, static_cast<rf::ubyte>(255.0f * marker_bar_fill_alpha * alpha)});
        }
        if (back_w > 1e-4f) {
            do_render_world_hud_rect(anchor, marker_line2_offset, 0.5f * fill_w, back_w, marker_bar_height,
                                     WorldHUDRenderMode::no_overdraw, false, true,
                                     rf::Color{0, 0, 0, static_cast<rf::ubyte>(255.0f * marker_bar_back_alpha * alpha)});
        }
    }
} // namespace

std::string vehicle_marker_countdown_text(int64_t ms_left)
{
    const int secs = static_cast<int>((std::max<int64_t>(ms_left, 0) + 999) / 1000);
    if (secs >= 60) {
        return std::format("{}:{:02}", secs / 60, secs % 60);
    }
    return std::format("{}", secs);
}

void vehicle_markers_level_init()
{
    for (const MarkerMeshEntry& e : g_marker_meshes) {
        if (e.mesh && e.anim_fx) {
            rf::vmesh_free(e.mesh);
        }
    }
    for (FactoryMarker& m : g_markers) {
        world_hud_release_text_label(m.name_tex);
        world_hud_release_text_label(m.line2_tex);
    }
    for (HullMarker& h : g_hull_markers) {
        world_hud_release_text_label(h.tex);
    }
    g_markers.clear();
    g_marker_meshes.clear();
    g_hull_markers.clear();
    g_hull_scratch.clear();
    g_hull_scratch_frame = -1;
    g_health_bars.clear();
    g_viewer_hull = nullptr;
    g_viewer_hull_frame = -1;
}

void vehicle_markers_level_init_post()
{
    vehicle_markers_level_init();
    if (!rf::is_multi || rf::is_dedicated_server || !vehicle_level_has_factories()) {
        return;
    }

    const int count = vehicle_factory_ui_count();
    g_markers.resize(count);
    for (int i = 0; i < count; ++i) {
        const AlpineVehicleFactoryInfo* info = vehicle_factory(i);
        if (!info) {
            continue;
        }
        FactoryMarker& m = g_markers[i];
        const int entity_type = rf::entity_lookup_type(info->vehicle_class.c_str());
        if (!vehicle_is_synced_entity_type(entity_type)) {
            continue;
        }
        m.name = marker_display_name(entity_type);
        m.is_turret = rf::entity_types[entity_type].use_function == rf::ENTITY_USE_TURRET;
        // Read from the live tbl, so this level's mesh override is already folded in.
        m.mesh = marker_resolve_mesh(rf::entity_types[entity_type].vmesh_filename.c_str());
        if (!m.mesh) {
            continue;
        }
        rf::vmesh_get_bbox(m.mesh, &m.bbox_min, &m.bbox_max);
        if (m.bbox_max.y > m.bbox_min.y) {
            m.has_bbox = true;
        }
    }
}

static void vehicle_markers_render_pass(rf::GRoom* room_filter)
{
    if (rf::is_dedicated_server || !rf::is_multi || !vehicle_level_has_factories()) {
        return;
    }
    // The room pass also runs for an ATX monitor or Projection Camera capture, whose view is not the
    // local player's: markers anchored to his camera would be drawn into the texture.
    if (rf::monitor_render_in_progress) {
        return;
    }
    rf::Camera* camera = rf::local_player ? rf::local_player->cam : nullptr;
    if (!camera) {
        return;
    }

    const rf::Vector3 cam_pos = rf::camera_get_pos(camera);
    const int64_t now = timer::get_i64(1000);
    rf::Entity* const viewer_hull = health_bar_viewer_hull();

    if (g_alpine_game_config.vehicle_health_bars) {
        for (const HullHealthBar& b : g_health_bars) {
            rf::Entity* ep = rf::entity_from_handle(b.handle);
            if (!ep || rf::entity_is_dying(ep) || ep == viewer_hull
                || !marker_pass_takes(marker_render_room(ep->room), room_filter)) {
                continue;
            }
            const float alpha = health_bar_lift(b, now) * marker_distance_fade(ep->pos, cam_pos);
            if (alpha <= 0.0f) {
                continue;
            }
            const float max_life = vehicle_hud_max_life(ep);
            const float frac = max_life > 0.0f ? std::clamp(vehicle_hud_life(ep) / max_life, 0.0f, 1.0f) : 1.0f;

            rf::Vector3 anchor = ep->pos;
            anchor.y += hull_anchor_height(ep) + marker_anchor_margin;
            anchor.y += 0.5f * marker_bar_height * world_hud_label_scale(anchor, false);
            render_health_bar(anchor, frac, alpha);
        }
    }

    if (!g_alpine_game_config.vehicle_respawn_markers || !vehicle_level_has_factories() || g_markers.empty()) {
        return;
    }

    const int label_font = get_world_hud_label_bitmap_font();
    const float pulse = marker_pulse(now);
    const int count = std::min(static_cast<int>(g_markers.size()), vehicle_factory_ui_count());

    for (int i = 0; i < count; ++i) {
        FactoryMarker& m = g_markers[i];
        if (m.name.empty()) {
            continue;
        }
        const AlpineVehicleFactoryInfo* info = vehicle_factory(i);
        const VehicleFactoryUi* ui = vehicle_factory_ui(i);
        if (!info || !ui || ui->state == AF_VEHICLE_FACTORY_GIVEN_UP) {
            continue;
        }

        const float fade = marker_distance_fade(info->pos, cam_pos);
        if (fade <= 0.0f) {
            continue;
        }
        const bool room_known = m.room_resolved;
        if (room_known && !marker_pass_takes(m.room, room_filter)) {
            continue;
        }

        const bool pending = ui->state == AF_VEHICLE_FACTORY_PENDING;
        const int64_t ms_left = pending ? std::max<int64_t>(ui->deadline_ms - now, 0) : 0;
        const bool urgent = pending && ms_left <= marker_urgent_window_ms;
        const bool flashing = ui->spawned_ms > 0 && now - ui->spawned_ms < marker_spawn_flash_ms;
        const rf::Color team = hud_team_color(info->team);

        float line2_height = marker_line2_height;
        if (urgent) {
            line2_height *= 1.0f + marker_urgent_scale_pulse * pulse;
        }

        rf::Vector3 anchor = info->pos;
        anchor.y += m.has_bbox ? rotated_bbox_top(m.bbox_min, m.bbox_max, info->orient) : marker_fallback_height;
        anchor.y += marker_anchor_margin;
        // Lift by the scaled half-height of the taller quad so its bottom edge clears the hull at any distance.
        anchor.y += 0.5f * std::max(marker_label_height, line2_height) * world_hud_label_scale(anchor, false);

        if (!room_known && !marker_pass_takes(factory_marker_room(m, anchor), room_filter)) {
            continue;
        }

        if (pending && m.mesh && m.has_bbox) {
            float alpha_below = ghost_alpha_below;
            if (urgent) {
                alpha_below += ghost_pulse_amplitude * (pulse * 2.0f - 1.0f);
            }
            const float progress = vehicle_factory_ui_progress(i, now);
            // The shader compares WORLD height, so a pitched or rolled factory needs the oriented extent.
            float bottom = 0.0f;
            float top = 0.0f;
            rotated_bbox_height_range(m.bbox_min, m.bbox_max, info->orient, bottom, top);
            const float fill_y = info->pos.y + bottom + progress * (top - bottom);
            gr::render_ghost_mesh(m.mesh, info->pos, info->orient, alpha_below, ghost_alpha_above,
                                  fill_y, &team);
        }

        const float label_alpha = flashing ? 255.0f : marker_label_alpha;
        world_hud_ensure_text_label(m.name_tex, m.name, label_font);
        do_render_world_hud_text_label(
            m.name_tex, anchor, marker_label_offset, marker_label_height,
            WorldHUDRenderMode::no_overdraw, false, true,
            rf::Color{team.red, team.green, team.blue, static_cast<rf::ubyte>(label_alpha * fade)});

        // A turret's TAKEN is only "somebody is in it right now"; a vehicle's is for the hull's life.
        const char* const taken_text = m.is_turret ? "IN USE" : "TAKEN";
        const std::string line2 = pending ? vehicle_marker_countdown_text(ms_left)
                                 : ui->state == AF_VEHICLE_FACTORY_ALIVE_TAKEN ? taken_text
                                                                               : "READY";
        float line2_alpha = pending ? 1.0f : marker_alive_line_alpha;
        if (urgent) {
            line2_alpha *= 0.6f + 0.4f * pulse;
        }
        if (flashing) {
            line2_alpha = 1.0f;
        }
        world_hud_ensure_text_label(m.line2_tex, line2, label_font);
        do_render_world_hud_text_label(
            m.line2_tex, anchor, marker_line2_offset, line2_height,
            WorldHUDRenderMode::no_overdraw, false, true,
            rf::Color{hud_amber_color.red, hud_amber_color.green, hud_amber_color.blue,
                      static_cast<rf::ubyte>(255.0f * line2_alpha * fade)});
    }

    // Second marker type: the auto-return countdown, riding the hull rather than the factory.
    if (g_hull_scratch_frame != rf::frame_count) {
        g_hull_scratch.clear();
        vehicle_unoccupied_hulls(g_hull_scratch);
        hull_markers_retain(g_hull_scratch);
        g_hull_scratch_frame = rf::frame_count;
    }
    for (const auto& [handle, deadline_ms] : g_hull_scratch) {
        rf::Entity* ep = rf::entity_from_handle(handle);
        if (!ep || rf::entity_is_dying(ep)) {
            continue;
        }
        const float fade = marker_distance_fade(ep->pos, cam_pos);
        if (fade <= 0.0f) {
            continue;
        }

        if (!marker_pass_takes(marker_render_room(ep->room), room_filter)) {
            continue;
        }

        const int64_t ms_left = std::max<int64_t>(deadline_ms - now, 0);
        const bool urgent = ms_left <= marker_urgent_window_ms;

        float alpha = 1.0f;
        float height = marker_line2_height;
        if (urgent) {
            alpha *= 0.6f + 0.4f * pulse;
            height *= 1.0f + marker_urgent_scale_pulse * pulse;
        }

        rf::Vector3 anchor = ep->pos;
        anchor.y += hull_anchor_height(ep) + marker_anchor_margin;
        const float scale = world_hud_label_scale(anchor, false);
        anchor.y += 0.5f * height * scale;
        // The health bar sits below.
        anchor.y += (marker_bar_height + marker_bar_countdown_gap) * scale
                  * health_bar_countdown_lift(ep, viewer_hull, now);

        const rf::Color team = hud_team_color(vehicle_hull_team(handle));
        NameLabelTex& tex = hull_marker_tex(handle);
        world_hud_ensure_text_label(tex, marker_seconds_text(ms_left), label_font);
        do_render_world_hud_text_label(
            tex, anchor, marker_line2_offset, height, WorldHUDRenderMode::no_overdraw, false, true,
            rf::Color{team.red, team.green, team.blue,
                      static_cast<rf::ubyte>(255.0f * alpha * fade)});
    }
}

// The room's liquid surface is rendered a few instructions later, so a marker queued here is
// blended under the water instead of being depth-rejected by it.
static CodeInjection before_room_liquid_render_hook{
    0x004D40F6,
    [](auto& regs) {
        rf::GRoom* room = regs.edi;
        if (!is_headless_mode() && room) {
            vehicle_markers_render_pass(room);
        }
    },
};

void vehicle_markers_render()
{
    // Dropped while off, so turning them back on rebuilds from live hulls instead of replaying stale fades.
    if (!g_alpine_game_config.vehicle_health_bars) {
        g_health_bars.clear();
    }
    else if (!rf::is_dedicated_server && rf::is_multi && vehicle_level_has_factories()) {
        health_bars_refresh(timer::get_i64(1000));
    }
    vehicle_markers_render_pass(nullptr);
}

void vehicle_markers_apply_patch()
{
    before_room_liquid_render_hook.install();
}
