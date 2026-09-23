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
#include <common/utils/string-utils.h>
#include "vehicle.h"
#include "vehicle_markers.h"
#include "../alpine_packets.h"
#include "../../graphics/gr_ghost_mesh.h"
#include "../../hud/hud_world.h"
#include "../../misc/alpine_settings.h"
#include "../../misc/level.h"
#include "../../os/os.h"
#include "../../rf/clutter.h"
#include "../../rf/entity.h"
#include "../../rf/geometry.h"
#include "../../rf/gr/gr.h"
#include "../../rf/level.h"
#include "../../rf/multi.h"
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

    // Both sized once per level; never resized while rendering.
    std::vector<FactoryMarker> g_markers;
    std::vector<MarkerMeshEntry> g_marker_meshes;
    // Bounded by the live hull count, which the factory count bounds in turn.
    std::vector<HullMarker> g_hull_markers;
    std::vector<std::pair<int, int64_t>> g_hull_scratch; // reused; never freed while rendering

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

    rf::Color marker_team_color(int team)
    {
        if (team != rf::TEAM_RED && team != rf::TEAM_BLUE) {
            return rf::Color{255, 255, 255, 255};
        }
        const uint32_t packed = team == rf::TEAM_RED ? g_alpine_game_config.outlines_color_team_r
                                                     : g_alpine_game_config.outlines_color_team_b;
        const auto comps = extract_color_components(packed);
        return rf::Color{static_cast<rf::ubyte>(std::get<0>(comps)),
                         static_cast<rf::ubyte>(std::get<1>(comps)),
                         static_cast<rf::ubyte>(std::get<2>(comps)), 255};
    }

    std::string marker_countdown_text(int64_t ms_left)
    {
        const int secs = static_cast<int>((std::max<int64_t>(ms_left, 0) + 999) / 1000);
        if (secs >= 60) {
            return std::format("{}:{:02}", secs / 60, secs % 60);
        }
        return std::format("{}", secs);
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

    // Highest world-space Y of the local bbox once the object's orient is applied, relative to its origin.
    float rotated_bbox_top(const rf::Vector3& bbox_min, const rf::Vector3& bbox_max, const rf::Matrix3& orient)
    {
        float top = std::numeric_limits<float>::lowest();
        for (int c = 0; c < 8; ++c) {
            const float lx = (c & 1) ? bbox_max.x : bbox_min.x;
            const float ly = (c & 2) ? bbox_max.y : bbox_min.y;
            const float lz = (c & 4) ? bbox_max.z : bbox_min.z;
            top = std::max(top, orient.rvec.y * lx + orient.uvec.y * ly + orient.fvec.y * lz);
        }
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
        const float far_clip = rf::level.distance_fog_far_clip;
        return far_clip > 0.0f && distance > far_clip;
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
} // namespace

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
    if (rf::is_dedicated_server || !rf::is_multi || !g_alpine_game_config.vehicle_respawn_markers) {
        return;
    }
    // The room pass also runs for an ATX monitor or Projection Camera capture, whose view is not the
    // local player's: markers anchored to his camera would be drawn into the texture.
    if (rf::monitor_render_in_progress) {
        return;
    }
    if (!vehicle_level_has_factories() || g_markers.empty()) {
        return;
    }
    rf::Camera* camera = rf::local_player ? rf::local_player->cam : nullptr;
    if (!camera) {
        return;
    }

    const int label_font = get_world_hud_label_bitmap_font();
    const rf::Vector3 cam_pos = rf::camera_get_pos(camera);
    const int64_t now = timer::get_i64(1000);
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

        const bool pending = ui->state == AF_VEHICLE_FACTORY_PENDING;
        const int64_t ms_left = pending ? std::max<int64_t>(ui->deadline_ms - now, 0) : 0;
        const bool urgent = pending && ms_left <= marker_urgent_window_ms;
        const bool flashing = ui->spawned_ms > 0 && now - ui->spawned_ms < marker_spawn_flash_ms;
        const rf::Color team = marker_team_color(info->team);

        float line2_height = marker_line2_height;
        if (urgent) {
            line2_height *= 1.0f + marker_urgent_scale_pulse * pulse;
        }

        rf::Vector3 anchor = info->pos;
        anchor.y += m.has_bbox ? rotated_bbox_top(m.bbox_min, m.bbox_max, info->orient) : marker_fallback_height;
        anchor.y += marker_anchor_margin;
        // Lift by the scaled half-height of the taller quad so its bottom edge clears the hull at any distance.
        anchor.y += 0.5f * std::max(marker_label_height, line2_height) * world_hud_label_scale(anchor, false);

        if (!marker_pass_takes(factory_marker_room(m, anchor), room_filter)) {
            continue;
        }

        if (pending && m.mesh && m.has_bbox) {
            float alpha_below = ghost_alpha_below;
            if (urgent) {
                alpha_below += ghost_pulse_amplitude * (pulse * 2.0f - 1.0f);
            }
            const float progress = vehicle_factory_ui_progress(i, now);
            const float fill_y =
                info->pos.y + m.bbox_min.y + progress * (m.bbox_max.y - m.bbox_min.y);
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
        const std::string line2 = pending ? marker_countdown_text(ms_left)
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
            rf::Color{255, 220, 64, static_cast<rf::ubyte>(255.0f * line2_alpha * fade)});
    }

    // Second marker type: the auto-return countdown, riding the hull rather than the factory.
    g_hull_scratch.clear();
    vehicle_unoccupied_hulls(g_hull_scratch);
    hull_markers_retain(g_hull_scratch);
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
        // Lift by the scaled half-height of the quad so its bottom edge clears the hull at any distance.
        anchor.y += 0.5f * height * world_hud_label_scale(anchor, false);

        const rf::Color team = marker_team_color(vehicle_hull_team(handle));
        NameLabelTex& tex = hull_marker_tex(handle);
        world_hud_ensure_text_label(tex, marker_seconds_text(ms_left), label_font);
        do_render_world_hud_text_label(
            tex, anchor, marker_line2_offset, height, WorldHUDRenderMode::no_overdraw, false, true,
            rf::Color{team.red, team.green, team.blue,
                      static_cast<rf::ubyte>(255.0f * alpha * fade)});
    }
}

void vehicle_markers_render_room(rf::GRoom* room)
{
    if (room) {
        vehicle_markers_render_pass(room);
    }
}

void vehicle_markers_render()
{
    vehicle_markers_render_pass(nullptr);
}
