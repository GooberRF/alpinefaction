#include "waypoints_utils.h"
#include "waypoints_internal.h"
#include "alpine_settings.h"
#include "../hud/multi_spectate.h"
#include "../rf/collide.h"
#include "../rf/entity.h"
#include "../rf/gameseq.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/input.h"
#include "../rf/level.h"
#include "../rf/object.h"
#include "../rf/os/console.h"
#include "../rf/player/camera.h"
#include "../rf/player/control_config.h"
#include "../rf/player/player.h"
#include "../rf/trigger.h"
#include "../graphics/gr.h"
#include <patch_common/FunHook.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <deque>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
enum class WaypointEditorSelectionKind : int
{
    none = 0,
    waypoint = 1,
    zone = 2,
    target = 3,
};

struct WaypointEditorSelectionState
{
    WaypointEditorSelectionKind kind = WaypointEditorSelectionKind::none;
    int uid = -1;
};

enum class WaypointGizmoAxis : int
{
    none = 0,
    x = 1,
    y = 2,
    z = 3,
};

struct WaypointEditorRect
{
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    bool contains(const int px, const int py) const
    {
        return px >= x && py >= y && px < (x + w) && py < (y + h);
    }
};

struct WaypointLinkEditorDialogState
{
    bool open = false;
    bool edit_inbound_links = false;
    bool inbound_list_truncated = false;
    int waypoint_uid = 0;
    int active_field = -1;
    std::array<std::string, kMaxWaypointLinks> fields{};
};

enum class WaypointZoneCreateDialogStage : int
{
    select_type = 0,
    select_source = 1,
    enter_trigger_uid = 2,
};

struct WaypointZoneCreateDialogState
{
    bool open = false;
    WaypointZoneCreateDialogStage stage = WaypointZoneCreateDialogStage::select_type;
    WaypointZoneType selected_type = WaypointZoneType::control_point;
    std::string trigger_uid_field{};
};

struct WaypointTargetCreateDialogState
{
    bool open = false;
};

enum class WaypointTypeChangeDialogSubject : int
{
    none = 0,
    waypoint = 1,
    zone = 2,
    target = 3,
};

struct WaypointTypeChangeDialogState
{
    bool open = false;
    WaypointTypeChangeDialogSubject subject = WaypointTypeChangeDialogSubject::none;
    int uid = -1;
};

struct WaypointGizmoDragState
{
    bool active = false;
    bool moved = false;
    WaypointEditorSelectionState selection{};
    WaypointGizmoAxis axis = WaypointGizmoAxis::none;
    int zone_box_corner = -1;
    rf::Vector3 axis_dir{};
    rf::Vector3 start_center{};
    float start_axis_t = 0.0f;
};

struct WaypointEditorViewLockState
{
    bool active = false;
    rf::Matrix3 orient{};
    rf::Vector3 phb{};
    rf::Vector3 eye_phb{};
};

WaypointEditorSelectionState g_waypoint_editor_selection{};
std::deque<std::string> g_waypoint_editor_log{};
constexpr size_t kWaypointEditorLogMaxLines = 128;
bool g_waypoint_editor_mouse_ui_mode = false;
int g_waypoint_editor_mouse_x = 0;
int g_waypoint_editor_mouse_y = 0;
bool g_waypoint_editor_left_click_pressed = false;
bool g_waypoint_editor_right_click_pressed = false;
bool g_waypoint_editor_left_click_consumed = false;
WaypointLinkEditorDialogState g_waypoint_link_editor_dialog{};
WaypointZoneCreateDialogState g_waypoint_zone_create_dialog{};
WaypointTargetCreateDialogState g_waypoint_target_create_dialog{};
WaypointTypeChangeDialogState g_waypoint_type_change_dialog{};
WaypointGizmoAxis g_waypoint_gizmo_hover_axis = WaypointGizmoAxis::none;
int g_waypoint_gizmo_hover_zone_corner = -1;
WaypointGizmoDragState g_waypoint_gizmo_drag{};
WaypointEditorViewLockState g_waypoint_editor_view_lock{};
bool g_waypoint_editor_input_hooks_installed = false;
bool g_waypoint_editor_mouse_look_overridden = false;
bool g_waypoint_editor_prev_mouse_look = true;
std::array<int, 10> g_waypoint_editor_pending_numeric_key_counts{};

void push_waypoint_editor_log(std::string message)
{
    if (message.empty()) {
        return;
    }

    g_waypoint_editor_log.push_back(std::move(message));
    while (g_waypoint_editor_log.size() > kWaypointEditorLogMaxLines) {
        g_waypoint_editor_log.pop_front();
    }
}

bool waypoint_editor_runtime_mode_active()
{
    if (is_waypoint_bot_mode_active() || !g_alpine_game_config.waypoints_edit_mode) {
        return false;
    }

    if (rf::gameseq_get_state() != rf::GS_GAMEPLAY) {
        return false;
    }

    rf::Player* local_player = rf::local_player;
    if (!local_player || !local_player->cam) {
        return false;
    }

    const rf::CameraMode cam_mode = rf::camera_get_mode(*local_player->cam);
    const bool first_person_spawned = cam_mode == rf::CAMERA_FIRST_PERSON && !local_player->is_spectator;
    const bool freelook_spectating = cam_mode == rf::CAMERA_FREELOOK && multi_spectate_is_spectating();
    return first_person_spawned || freelook_spectating;
}

bool waypoint_editor_freelook_aim_mode_active();

int numeric_key_index_from_key_code(const int key_code)
{
    if (key_code >= static_cast<int>(rf::KEY_1) && key_code <= static_cast<int>(rf::KEY_9)) {
        return key_code - static_cast<int>(rf::KEY_1);
    }
    if (key_code == static_cast<int>(rf::KEY_0)) {
        return 9;
    }
    return -1;
}

rf::Entity* get_waypoint_editor_view_entity()
{
    rf::Player* local_player = rf::local_player;
    if (!local_player) {
        return nullptr;
    }
    const rf::CameraMode cam_mode = (local_player->cam != nullptr)
        ? rf::camera_get_mode(*local_player->cam)
        : rf::CAMERA_FIRST_PERSON;
    if (cam_mode == rf::CAMERA_FREELOOK) {
        if (local_player->cam && local_player->cam->camera_entity) {
            return local_player->cam->camera_entity;
        }
        return rf::entity_from_handle(local_player->entity_handle);
    }

    if (rf::Entity* player_entity = rf::entity_from_handle(local_player->entity_handle)) {
        return player_entity;
    }
    if (local_player->cam && local_player->cam->camera_entity) {
        return local_player->cam->camera_entity;
    }
    return nullptr;
}

void apply_view_orient_to_entity(rf::Entity& entity, const rf::Matrix3& orient)
{
    entity.orient = orient;
    entity.p_data.orient = orient;
    entity.p_data.next_orient = orient;
    entity.eye_orient = orient;
}

void capture_waypoint_editor_view_lock()
{
    g_waypoint_editor_view_lock = {};
    rf::Entity* view_entity = get_waypoint_editor_view_entity();
    if (!view_entity) {
        return;
    }
    if (rf::local_player && rf::local_player->cam) {
        g_waypoint_editor_view_lock.orient = rf::camera_get_orient(rf::local_player->cam);
    }
    else {
        g_waypoint_editor_view_lock.orient = view_entity->orient;
    }
    g_waypoint_editor_view_lock.phb = view_entity->control_data.phb;
    g_waypoint_editor_view_lock.eye_phb = view_entity->control_data.eye_phb;
    g_waypoint_editor_view_lock.active = true;
}

void apply_waypoint_editor_view_lock(const bool clear_after_apply)
{
    if (!g_waypoint_editor_view_lock.active) {
        return;
    }

    rf::Entity* view_entity = get_waypoint_editor_view_entity();
    if (view_entity) {
        float pitch = 0.0f;
        float roll = 0.0f;
        float yaw = 0.0f;
        g_waypoint_editor_view_lock.orient.extract_angles(&pitch, &roll, &yaw);
        rf::Vector3 locked_phb = g_waypoint_editor_view_lock.phb;
        locked_phb.set(-pitch, yaw, roll);

        apply_view_orient_to_entity(*view_entity, g_waypoint_editor_view_lock.orient);
        view_entity->control_data.phb = locked_phb;
        view_entity->control_data.eye_phb = g_waypoint_editor_view_lock.eye_phb;
        view_entity->control_data.delta_phb.zero();
        view_entity->control_data.delta_eye_phb.zero();
    }

    if (clear_after_apply) {
        g_waypoint_editor_view_lock = {};
    }
}

void apply_waypoint_editor_mouse_look_override(const bool cursor_mode_active)
{
    rf::Player* local_player = rf::local_player;
    if (!local_player) {
        g_waypoint_editor_mouse_look_overridden = false;
        return;
    }

    if (cursor_mode_active) {
        if (!g_waypoint_editor_mouse_look_overridden) {
            g_waypoint_editor_prev_mouse_look = local_player->settings.controls.mouse_look;
            g_waypoint_editor_mouse_look_overridden = true;
        }
        local_player->settings.controls.mouse_look = false;
        return;
    }

    if (g_waypoint_editor_mouse_look_overridden) {
        local_player->settings.controls.mouse_look = g_waypoint_editor_prev_mouse_look;
        g_waypoint_editor_mouse_look_overridden = false;
    }
}

bool should_block_waypoint_editor_mouse_action(
    rf::ControlConfig* const control_config,
    const rf::ControlConfigAction action)
{
    if (!control_config || !rf::local_player || control_config != &rf::local_player->settings.controls) {
        return false;
    }

    if (action == rf::CC_ACTION_SECONDARY_ATTACK) {
        return waypoint_editor_runtime_mode_active();
    }

    if (action == rf::CC_ACTION_PRIMARY_ATTACK) {
        return g_waypoint_editor_mouse_ui_mode || waypoint_editor_freelook_aim_mode_active();
    }

    return false;
}

FunHook<bool(rf::ControlConfig*, rf::ControlConfigAction, bool*)> control_config_check_pressed_waypoint_editor_hook{
    0x0043D4F0,
    [](rf::ControlConfig* control_config, rf::ControlConfigAction action, bool* just_pressed) {
        if (should_block_waypoint_editor_mouse_action(control_config, action)) {
            if (just_pressed) {
                *just_pressed = false;
            }
            return false;
        }
        return control_config_check_pressed_waypoint_editor_hook.call_target(control_config, action, just_pressed);
    },
};

FunHook<bool(rf::ControlConfig*, rf::ControlConfigAction)> control_is_control_down_waypoint_editor_hook{
    0x00430F40,
    [](rf::ControlConfig* control_config, rf::ControlConfigAction action) {
        if (should_block_waypoint_editor_mouse_action(control_config, action)) {
            return false;
        }
        return control_is_control_down_waypoint_editor_hook.call_target(control_config, action);
    },
};

void ensure_waypoint_editor_input_hooks_installed()
{
    if (g_waypoint_editor_input_hooks_installed) {
        return;
    }
    control_config_check_pressed_waypoint_editor_hook.install();
    control_is_control_down_waypoint_editor_hook.install();
    g_waypoint_editor_input_hooks_installed = true;
}

bool waypoint_editor_freelook_aim_mode_active()
{
    if (!waypoint_editor_runtime_mode_active() || g_waypoint_editor_mouse_ui_mode) {
        return false;
    }

    rf::Player* local_player = rf::local_player;
    if (!local_player || !local_player->cam) {
        return false;
    }

    return rf::camera_get_mode(*local_player->cam) == rf::CAMERA_FREELOOK;
}

void capture_waypoint_editor_mouse_input()
{
    int mouse_z = 0;
    rf::mouse_get_pos(g_waypoint_editor_mouse_x, g_waypoint_editor_mouse_y, mouse_z);
    g_waypoint_editor_left_click_pressed = rf::mouse_was_button_pressed(0) > 0;
    g_waypoint_editor_right_click_pressed = rf::mouse_was_button_pressed(1) > 0;
    g_waypoint_editor_left_click_consumed = false;
}

void apply_waypoint_editor_mouse_mode(const bool runtime_mode_active)
{
    if (!runtime_mode_active) {
        g_waypoint_editor_mouse_ui_mode = false;
        apply_waypoint_editor_mouse_look_override(false);
        if (rf::gameseq_get_state() == rf::GS_GAMEPLAY && !rf::keep_mouse_centered) {
            rf::mouse_keep_centered_enable();
            rf::mouse_set_visible(false);
        }
        return;
    }

    if (g_waypoint_editor_mouse_ui_mode) {
        apply_waypoint_editor_mouse_look_override(true);
        if (rf::keep_mouse_centered) {
            rf::mouse_keep_centered_disable();
        }
        rf::mouse_set_visible(true);
        return;
    }

    apply_waypoint_editor_mouse_look_override(false);
    if (!rf::keep_mouse_centered) {
        rf::mouse_keep_centered_enable();
    }
    rf::mouse_set_visible(false);
}

bool waypoint_editor_selection_mode_active()
{
    if (!waypoint_editor_runtime_mode_active()) {
        return false;
    }

    rf::Player* local_player = rf::local_player;
    if (!local_player || !local_player->cam) {
        return false;
    }

    const rf::CameraMode cam_mode = rf::camera_get_mode(*local_player->cam);
    return cam_mode == rf::CAMERA_FIRST_PERSON || cam_mode == rf::CAMERA_FREELOOK;
}

bool waypoint_editor_modal_dialog_open()
{
    return g_waypoint_link_editor_dialog.open
        || g_waypoint_zone_create_dialog.open
        || g_waypoint_target_create_dialog.open
        || g_waypoint_type_change_dialog.open;
}

struct ForwardTraceResult
{
    bool valid = false;
    bool hit = false;
    rf::Vector3 point{};
    const rf::GFace* face = nullptr;
};

ForwardTraceResult trace_forward_from_camera(const float max_dist)
{
    ForwardTraceResult result{};
    if (!(rf::level.flags & rf::LEVEL_LOADED) || max_dist <= 0.0f) {
        return result;
    }

    rf::Player* local_player = rf::local_player;
    if (!local_player || !local_player->cam) {
        return result;
    }

    const rf::Vector3 origin = rf::camera_get_pos(local_player->cam);
    rf::Vector3 dir = rf::camera_get_orient(local_player->cam).fvec;
    if (dir.len_sq() <= 0.0001f) {
        return result;
    }
    dir.normalize_safe();

    const rf::Vector3 end = origin + dir * max_dist;
    rf::Vector3 p0 = origin;
    rf::Vector3 p1 = end;
    rf::Entity* ignore_entity = rf::entity_from_handle(local_player->entity_handle);
    rf::LevelCollisionOut col_info{};
    col_info.face = nullptr;
    col_info.obj_handle = -1;
    const bool hit = rf::collide_linesegment_level_for_multi(
        p0,
        p1,
        ignore_entity,
        nullptr,
        &col_info,
        0.1f,
        false,
        1.0f);

    result.valid = true;
    result.hit = hit;
    result.point = hit ? col_info.hit_point : end;
    result.face = hit ? static_cast<const rf::GFace*>(col_info.face) : nullptr;
    return result;
}

bool create_new_waypoint_std_from_view()
{
    constexpr float kTraceDist = 20.0f;
    constexpr float kSurfaceYOffset = 1.0f;

    const auto trace = trace_forward_from_camera(kTraceDist);
    if (!trace.valid) {
        return false;
    }

    rf::Vector3 waypoint_pos = trace.point;
    if (trace.hit) {
        waypoint_pos.y += kSurfaceYOffset;
    }

    const int waypoint_uid = add_waypoint(
        waypoint_pos,
        WaypointType::std,
        0,
        false,
        true,
        kWaypointLinkRadius,
        -1,
        nullptr,
        true,
        static_cast<int>(WaypointDroppedSubtype::normal));
    rf::console::print(
        "Added waypoint {} (std) at {:.2f},{:.2f},{:.2f}",
        waypoint_uid,
        waypoint_pos.x, waypoint_pos.y, waypoint_pos.z);
    push_waypoint_editor_log(std::format(
        "Added waypoint {} (std) at {:.2f},{:.2f},{:.2f}",
        waypoint_uid,
        waypoint_pos.x, waypoint_pos.y, waypoint_pos.z));
    return true;
}

bool create_new_target_from_view(const WaypointTargetType target_type)
{
    constexpr float kTraceDist = 20.0f;
    constexpr float kShatterTraceDist = 10000.0f;
    rf::Vector3 target_pos{};
    int shatter_room_key = -1;
    if (target_type == WaypointTargetType::shatter) {
        if (!waypoints_trace_breakable_glass_from_camera(
                kShatterTraceDist,
                target_pos,
                shatter_room_key)) {
            rf::console::print(
                "Could not place shatter target: must look at a breakable glass surface");
            push_waypoint_editor_log(
                "Could not place shatter target: must look at a breakable glass surface");
            return false;
        }
    }
    else {
        const auto trace = trace_forward_from_camera(kTraceDist);
        if (!trace.valid) {
            return false;
        }
        target_pos = trace.point;
    }

    if (target_type == WaypointTargetType::shatter) {
        WaypointTargetDefinition shatter_constraint{};
        shatter_constraint.type = WaypointTargetType::shatter;
        shatter_constraint.identifier = shatter_room_key;
        rf::Vector3 constrained_pos{};
        if (!waypoints_constrain_shatter_target_position(
                shatter_constraint,
                target_pos,
                constrained_pos)) {
            push_waypoint_editor_log(
                std::format(
                    "Shatter constraint fallback: room {} could not reproject point ({:.2f},{:.2f},{:.2f}); using traced hit",
                    shatter_room_key,
                    target_pos.x,
                    target_pos.y,
                    target_pos.z));
        }
        else {
            target_pos = constrained_pos;
        }
    }

    const int target_uid = add_waypoint_target(target_pos, target_type);
    WaypointTargetDefinition* target = find_waypoint_target_by_uid(target_uid);
    if (target_type == WaypointTargetType::shatter && target) {
        target->identifier = shatter_room_key;
    }
    const int waypoint_ref_count = target ? static_cast<int>(target->waypoint_uids.size()) : 0;
    rf::console::print(
        "Added target {} uid {} at {:.2f},{:.2f},{:.2f} ({} waypoint refs)",
        waypoint_target_type_name(target_type),
        target_uid,
        target_pos.x, target_pos.y, target_pos.z,
        waypoint_ref_count);
    push_waypoint_editor_log(std::format(
        "Added target {} uid {} at {:.2f},{:.2f},{:.2f} ({} waypoint refs)",
        waypoint_target_type_name(target_type),
        target_uid,
        target_pos.x, target_pos.y, target_pos.z,
        waypoint_ref_count));
    return true;
}

bool create_new_zone_on_trigger(const WaypointZoneType zone_type, const int trigger_uid)
{
    rf::Object* trigger_obj = rf::obj_lookup_from_uid(trigger_uid);
    if (!trigger_obj || trigger_obj->type != rf::OT_TRIGGER) {
        rf::console::print("UID {} is not a trigger", trigger_uid);
        return false;
    }

    WaypointZoneDefinition zone{};
    zone.type = zone_type;
    zone.trigger_uid = trigger_uid;
    const int zone_uid = add_waypoint_zone_definition(zone);
    rf::console::print(
        "Added zone {} as index {} (trigger uid {})",
        waypoint_zone_type_name(zone_type),
        zone_uid,
        trigger_uid);
    push_waypoint_editor_log(std::format(
        "Added zone {} as index {} (trigger uid {})",
        waypoint_zone_type_name(zone_type),
        zone_uid,
        trigger_uid));
    return true;
}

bool create_new_box_zone_from_view(const WaypointZoneType zone_type)
{
    if (waypoint_zone_type_is_bridge(zone_type)) {
        rf::console::print(
            "Zone type {} only supports trigger source",
            waypoint_zone_type_name(zone_type));
        return false;
    }

    constexpr float kPlacementDist = 5.0f;
    constexpr float kHalfExtent = 0.5f;
    rf::Player* local_player = rf::local_player;
    if (!local_player || !local_player->cam) {
        return false;
    }
    rf::Vector3 dir = rf::camera_get_orient(local_player->cam).fvec;
    if (dir.len_sq() <= 0.0001f) {
        return false;
    }
    dir.normalize_safe();
    const rf::Vector3 center = rf::camera_get_pos(local_player->cam) + dir * kPlacementDist;

    WaypointZoneDefinition zone{};
    zone.type = zone_type;
    zone.box_min = center - rf::Vector3{kHalfExtent, kHalfExtent, kHalfExtent};
    zone.box_max = center + rf::Vector3{kHalfExtent, kHalfExtent, kHalfExtent};
    const int zone_uid = add_waypoint_zone_definition(zone);
    const auto& stored_zone = g_waypoint_zones[zone_uid];
    rf::console::print(
        "Added zone {} as index {} (box min {:.2f},{:.2f},{:.2f} max {:.2f},{:.2f},{:.2f})",
        waypoint_zone_type_name(stored_zone.type),
        zone_uid,
        stored_zone.box_min.x, stored_zone.box_min.y, stored_zone.box_min.z,
        stored_zone.box_max.x, stored_zone.box_max.y, stored_zone.box_max.z);
    push_waypoint_editor_log(std::format(
        "Added zone {} as index {} (box min {:.2f},{:.2f},{:.2f} max {:.2f},{:.2f},{:.2f})",
        waypoint_zone_type_name(stored_zone.type),
        zone_uid,
        stored_zone.box_min.x, stored_zone.box_min.y, stored_zone.box_min.z,
        stored_zone.box_max.x, stored_zone.box_max.y, stored_zone.box_max.z));
    g_waypoint_editor_selection.kind = WaypointEditorSelectionKind::zone;
    g_waypoint_editor_selection.uid = zone_uid;
    return true;
}

void clear_waypoint_editor_selection()
{
    g_waypoint_editor_selection = {};
}

bool get_waypoint_editor_camera_ray(rf::Vector3& out_origin, rf::Vector3& out_dir, float& out_max_dist)
{
    rf::Player* local_player = rf::local_player;
    if (!local_player || !local_player->cam) {
        return false;
    }

    out_origin = rf::camera_get_pos(local_player->cam);
    const rf::Matrix3 orient = rf::camera_get_orient(local_player->cam);
    out_dir = orient.fvec;
    if (out_dir.len_sq() <= 0.0001f) {
        return false;
    }
    out_dir.normalize_safe();
    out_max_dist = 10000.0f;

    rf::Entity* entity = rf::entity_from_handle(local_player->entity_handle);
    rf::LevelCollisionOut col_info{};
    col_info.face = nullptr;
    col_info.obj_handle = -1;
    rf::Vector3 p0 = out_origin;
    rf::Vector3 p1 = out_origin + out_dir * out_max_dist;
    if (rf::collide_linesegment_level_for_multi(
            p0,
            p1,
            entity,
            nullptr,
            &col_info,
            0.1f,
            false,
            1.0f)) {
        const rf::Vector3 to_hit = col_info.hit_point - out_origin;
        out_max_dist = std::sqrt(to_hit.dot_prod(to_hit)) + 0.1f;
    }
    return true;
}

std::optional<float> ray_sphere_intersection_t(
    const rf::Vector3& ray_origin,
    const rf::Vector3& ray_dir,
    const rf::Vector3& sphere_center,
    float sphere_radius)
{
    if (!std::isfinite(sphere_radius) || sphere_radius <= 0.0f) {
        return std::nullopt;
    }

    const rf::Vector3 oc = ray_origin - sphere_center;
    const float b = oc.dot_prod(ray_dir);
    const float c = oc.dot_prod(oc) - sphere_radius * sphere_radius;
    const float discriminant = b * b - c;
    if (discriminant < 0.0f) {
        return std::nullopt;
    }

    const float sqrt_disc = std::sqrt(discriminant);
    float t = -b - sqrt_disc;
    if (t < 0.0f) {
        t = -b + sqrt_disc;
    }
    if (t < 0.0f) {
        return std::nullopt;
    }
    return t;
}

bool get_zone_selection_center_and_radius(
    const WaypointZoneDefinition& zone,
    rf::Vector3& out_center,
    float& out_radius)
{
    switch (resolve_waypoint_zone_source(zone)) {
        case WaypointZoneSource::trigger_uid: {
            rf::Object* trigger_obj = rf::obj_lookup_from_uid(zone.trigger_uid);
            if (!trigger_obj || trigger_obj->type != rf::OT_TRIGGER) {
                return false;
            }

            const auto* trigger = static_cast<rf::Trigger*>(trigger_obj);
            out_center = trigger->pos;
            if (trigger->type == 1) {
                const rf::Vector3 half_extents{
                    std::fabs(trigger->box_size.x) * 0.5f,
                    std::fabs(trigger->box_size.y) * 0.5f,
                    std::fabs(trigger->box_size.z) * 0.5f,
                };
                out_radius = std::sqrt(half_extents.dot_prod(half_extents));
                return out_radius > kWaypointLinkRadiusEpsilon;
            }

            out_radius = std::fabs(trigger->radius);
            return out_radius > kWaypointLinkRadiusEpsilon;
        }
        case WaypointZoneSource::box_extents: {
            const rf::Vector3 min_bound = point_min(zone.box_min, zone.box_max);
            const rf::Vector3 max_bound = point_max(zone.box_min, zone.box_max);
            out_center = (min_bound + max_bound) * 0.5f;
            const rf::Vector3 half_extents = (max_bound - min_bound) * 0.5f;
            out_radius = std::sqrt(half_extents.dot_prod(half_extents));
            return out_radius > kWaypointLinkRadiusEpsilon;
        }
        case WaypointZoneSource::room_uid:
        default:
            return false;
    }
}

bool is_waypoint_editor_selected_waypoint(int waypoint_uid)
{
    return g_waypoint_editor_selection.kind == WaypointEditorSelectionKind::waypoint
        && g_waypoint_editor_selection.uid == waypoint_uid;
}

bool is_waypoint_editor_selected_zone(int zone_uid)
{
    return g_waypoint_editor_selection.kind == WaypointEditorSelectionKind::zone
        && g_waypoint_editor_selection.uid == zone_uid;
}

bool is_waypoint_editor_selected_target(int target_uid)
{
    return g_waypoint_editor_selection.kind == WaypointEditorSelectionKind::target
        && g_waypoint_editor_selection.uid == target_uid;
}

void update_waypoint_editor_selection()
{
    if (!waypoint_editor_selection_mode_active()) {
        clear_waypoint_editor_selection();
        return;
    }

    rf::Vector3 ray_origin{};
    rf::Vector3 ray_dir{};
    float ray_max_dist = 0.0f;
    if (!get_waypoint_editor_camera_ray(ray_origin, ray_dir, ray_max_dist)) {
        clear_waypoint_editor_selection();
        return;
    }

    WaypointEditorSelectionState best_selection{};
    float best_t = ray_max_dist;

    for (int waypoint_uid = 1; waypoint_uid < static_cast<int>(g_waypoints.size()); ++waypoint_uid) {
        const auto& node = g_waypoints[waypoint_uid];
        if (!node.valid) {
            continue;
        }

        const float selection_radius = std::max(
            0.5f,
            sanitize_waypoint_link_radius(node.link_radius) * 0.25f);
        auto hit_t = ray_sphere_intersection_t(
            ray_origin,
            ray_dir,
            node.pos,
            selection_radius);
        if (!hit_t || hit_t.value() > best_t) {
            continue;
        }
        best_t = hit_t.value();
        best_selection.kind = WaypointEditorSelectionKind::waypoint;
        best_selection.uid = waypoint_uid;
    }

    for (int zone_uid = 0; zone_uid < static_cast<int>(g_waypoint_zones.size()); ++zone_uid) {
        rf::Vector3 zone_center{};
        float zone_radius = 0.0f;
        if (!get_zone_selection_center_and_radius(g_waypoint_zones[zone_uid], zone_center, zone_radius)) {
            continue;
        }
        auto hit_t = ray_sphere_intersection_t(
            ray_origin,
            ray_dir,
            zone_center,
            std::max(zone_radius, 0.5f));
        if (!hit_t || hit_t.value() > best_t) {
            continue;
        }
        best_t = hit_t.value();
        best_selection.kind = WaypointEditorSelectionKind::zone;
        best_selection.uid = zone_uid;
    }

    constexpr float kTargetSelectionRadius = 0.9f;
    for (const auto& target : g_waypoint_targets) {
        auto hit_t = ray_sphere_intersection_t(
            ray_origin,
            ray_dir,
            target.pos,
            kTargetSelectionRadius);
        if (!hit_t || hit_t.value() > best_t) {
            continue;
        }
        best_t = hit_t.value();
        best_selection.kind = WaypointEditorSelectionKind::target;
        best_selection.uid = target.uid;
    }

    g_waypoint_editor_selection = best_selection;
}

bool cycle_selected_waypoint_movement_subtype(int direction)
{
    if (g_waypoint_editor_selection.kind != WaypointEditorSelectionKind::waypoint
        || g_waypoint_editor_selection.uid <= 0
        || g_waypoint_editor_selection.uid >= static_cast<int>(g_waypoints.size())) {
        return false;
    }

    auto& node = g_waypoints[g_waypoint_editor_selection.uid];
    if (!node.valid) {
        return false;
    }

    node.movement_subtype = cycle_waypoint_dropped_subtype(node.movement_subtype, direction);
    rf::console::print(
        "Waypoint {} movement subtype set to {}",
        g_waypoint_editor_selection.uid,
        waypoint_dropped_subtype_name(node.movement_subtype));
    push_waypoint_editor_log(std::format(
        "Waypoint {} movement subtype -> {}",
        g_waypoint_editor_selection.uid,
        waypoint_dropped_subtype_name(node.movement_subtype)));
    return true;
}

void delete_selected_waypoint_editor_object()
{
    if (g_waypoint_editor_selection.kind == WaypointEditorSelectionKind::waypoint) {
        const int waypoint_uid = g_waypoint_editor_selection.uid;
        if (remove_waypoint_by_uid(waypoint_uid)) {
            rf::console::print("Deleted waypoint {}", waypoint_uid);
            push_waypoint_editor_log(std::format("Deleted waypoint {}", waypoint_uid));
        }
    }
    else if (g_waypoint_editor_selection.kind == WaypointEditorSelectionKind::zone) {
        const int zone_uid = g_waypoint_editor_selection.uid;
        if (remove_waypoint_zone_definition(zone_uid)) {
            rf::console::print("Deleted zone {}", zone_uid);
            push_waypoint_editor_log(std::format("Deleted zone {}", zone_uid));
        }
    }
    else if (g_waypoint_editor_selection.kind == WaypointEditorSelectionKind::target) {
        const int target_uid = g_waypoint_editor_selection.uid;
        if (remove_waypoint_target_by_uid(target_uid)) {
            rf::console::print("Deleted target {}", target_uid);
            push_waypoint_editor_log(std::format("Deleted target {}", target_uid));
        }
    }

    clear_waypoint_editor_selection();
}

void handle_waypoint_editor_input()
{
    if (!waypoint_editor_selection_mode_active()
        || g_waypoint_editor_selection.kind == WaypointEditorSelectionKind::none
        || waypoint_editor_modal_dialog_open()) {
        return;
    }

    if (rf::key_get_and_reset_down_counter(rf::KEY_DELETE) > 0) {
        delete_selected_waypoint_editor_object();
    }
}

bool is_valid_waypoint_uid_local(const int waypoint_uid)
{
    return waypoint_uid > 0
        && waypoint_uid < static_cast<int>(g_waypoints.size())
        && g_waypoints[waypoint_uid].valid;
}

bool selection_is_box_extents_zone(const WaypointEditorSelectionState& selection)
{
    if (selection.kind != WaypointEditorSelectionKind::zone
        || selection.uid < 0
        || selection.uid >= static_cast<int>(g_waypoint_zones.size())) {
        return false;
    }

    return resolve_waypoint_zone_source(g_waypoint_zones[selection.uid]) == WaypointZoneSource::box_extents;
}

bool selection_is_trigger_zone(const WaypointEditorSelectionState& selection)
{
    if (selection.kind != WaypointEditorSelectionKind::zone
        || selection.uid < 0
        || selection.uid >= static_cast<int>(g_waypoint_zones.size())) {
        return false;
    }

    return resolve_waypoint_zone_source(g_waypoint_zones[selection.uid]) == WaypointZoneSource::trigger_uid;
}

std::optional<rf::Vector3> get_zone_box_corner_world_pos(
    const WaypointEditorSelectionState& selection,
    const int corner_index)
{
    if (!selection_is_box_extents_zone(selection)
        || (corner_index != 0 && corner_index != 1)) {
        return std::nullopt;
    }

    const auto& zone = g_waypoint_zones[selection.uid];
    return corner_index == 0
        ? point_min(zone.box_min, zone.box_max)
        : point_max(zone.box_min, zone.box_max);
}

std::optional<rf::Vector3> get_selection_center(const WaypointEditorSelectionState& selection)
{
    switch (selection.kind) {
        case WaypointEditorSelectionKind::waypoint:
            if (is_valid_waypoint_uid_local(selection.uid)) {
                return g_waypoints[selection.uid].pos;
            }
            return std::nullopt;

        case WaypointEditorSelectionKind::zone:
            if (selection.uid < 0 || selection.uid >= static_cast<int>(g_waypoint_zones.size())) {
                return std::nullopt;
            }
            if (resolve_waypoint_zone_source(g_waypoint_zones[selection.uid]) == WaypointZoneSource::trigger_uid) {
                rf::Object* trigger_obj = rf::obj_lookup_from_uid(g_waypoint_zones[selection.uid].trigger_uid);
                if (!trigger_obj || trigger_obj->type != rf::OT_TRIGGER) {
                    return std::nullopt;
                }
                return trigger_obj->pos;
            }
            if (resolve_waypoint_zone_source(g_waypoint_zones[selection.uid]) == WaypointZoneSource::box_extents) {
                const auto min_bound = get_zone_box_corner_world_pos(selection, 0);
                const auto max_bound = get_zone_box_corner_world_pos(selection, 1);
                if (!min_bound || !max_bound) {
                    return std::nullopt;
                }
                return (min_bound.value() + max_bound.value()) * 0.5f;
            }
            return std::nullopt;

        case WaypointEditorSelectionKind::target: {
            WaypointTargetDefinition* target = find_waypoint_target_by_uid(selection.uid);
            if (!target) {
                return std::nullopt;
            }
            return target->pos;
        }
        case WaypointEditorSelectionKind::none:
        default:
            return std::nullopt;
    }
}

void rebuild_target_waypoint_refs(WaypointTargetDefinition& target)
{
    target.waypoint_uids =
        (target.type == WaypointTargetType::jump)
            ? collect_target_link_waypoint_uids(target.pos)
            : collect_target_waypoint_uids(target.pos);
    if (target.waypoint_uids.empty()) {
        target.waypoint_uids = collect_target_waypoint_uids(target.pos);
    }
    normalize_target_waypoint_uids(target.waypoint_uids);
}

bool apply_selection_translation(
    const WaypointEditorSelectionState& selection,
    const rf::Vector3& delta,
    const int zone_box_corner = -1)
{
    if (delta.len_sq() <= (kWaypointLinkRadiusEpsilon * kWaypointLinkRadiusEpsilon)) {
        return false;
    }

    switch (selection.kind) {
        case WaypointEditorSelectionKind::waypoint:
            if (!is_valid_waypoint_uid_local(selection.uid)) {
                return false;
            }
            g_waypoints[selection.uid].pos += delta;
            return true;

        case WaypointEditorSelectionKind::zone:
            if (selection.uid < 0 || selection.uid >= static_cast<int>(g_waypoint_zones.size())) {
                return false;
            }
            if (resolve_waypoint_zone_source(g_waypoint_zones[selection.uid]) == WaypointZoneSource::trigger_uid) {
                return false;
            }
            if (resolve_waypoint_zone_source(g_waypoint_zones[selection.uid]) == WaypointZoneSource::box_extents) {
                if (zone_box_corner == 0) {
                    g_waypoint_zones[selection.uid].box_min += delta;
                }
                else if (zone_box_corner == 1) {
                    g_waypoint_zones[selection.uid].box_max += delta;
                }
                else {
                    g_waypoint_zones[selection.uid].box_min += delta;
                    g_waypoint_zones[selection.uid].box_max += delta;
                }
                return true;
            }
            return false;

        case WaypointEditorSelectionKind::target: {
            WaypointTargetDefinition* target = find_waypoint_target_by_uid(selection.uid);
            if (!target) {
                return false;
            }

            if (target->type == WaypointTargetType::shatter) {
                if (target->identifier == -1) {
                    rf::Vector3 snapped_pos{};
                    int snapped_room_key = -1;
                    if (!waypoints_find_nearest_breakable_glass_face_point(
                            target->pos,
                            snapped_pos,
                            snapped_room_key)) {
                        return false;
                    }
                    target->identifier = snapped_room_key;
                    target->pos = snapped_pos;
                }

                const rf::Vector3 desired_pos = target->pos + delta;
                rf::Vector3 constrained_pos{};
                if (!waypoints_constrain_shatter_target_position(
                        *target,
                        desired_pos,
                        constrained_pos)) {
                    return false;
                }

                if (distance_sq(target->pos, constrained_pos)
                    <= (kWaypointLinkRadiusEpsilon * kWaypointLinkRadiusEpsilon)) {
                    return false;
                }

                target->pos = constrained_pos;
                return true;
            }

            target->pos += delta;
            return true;
        }
        case WaypointEditorSelectionKind::none:
        default:
            return false;
    }
}

void finalize_selection_after_translation(const WaypointEditorSelectionState& selection)
{
    if (selection.kind == WaypointEditorSelectionKind::zone
        && selection.uid >= 0
        && selection.uid < static_cast<int>(g_waypoint_zones.size())
        && resolve_waypoint_zone_source(g_waypoint_zones[selection.uid]) == WaypointZoneSource::box_extents) {
        auto& zone = g_waypoint_zones[selection.uid];
        const rf::Vector3 min_bound = point_min(zone.box_min, zone.box_max);
        const rf::Vector3 max_bound = point_max(zone.box_min, zone.box_max);
        zone.box_min = min_bound;
        zone.box_max = max_bound;
    }

    if (selection.kind == WaypointEditorSelectionKind::waypoint
        || selection.kind == WaypointEditorSelectionKind::zone) {
        refresh_all_waypoint_zone_refs();
        return;
    }

    if (selection.kind == WaypointEditorSelectionKind::target) {
        if (WaypointTargetDefinition* target = find_waypoint_target_by_uid(selection.uid)) {
            rebuild_target_waypoint_refs(*target);
        }
    }
}

std::string selection_debug_name(const WaypointEditorSelectionState& selection)
{
    switch (selection.kind) {
        case WaypointEditorSelectionKind::waypoint:
            return std::format("waypoint {}", selection.uid);
        case WaypointEditorSelectionKind::zone:
            return std::format("zone {}", selection.uid);
        case WaypointEditorSelectionKind::target:
            return std::format("target {}", selection.uid);
        case WaypointEditorSelectionKind::none:
        default:
            return "none";
    }
}

std::optional<float> closest_axis_t_to_camera_ray(
    const rf::Vector3& axis_origin,
    const rf::Vector3& axis_dir,
    const rf::Vector3& ray_origin,
    const rf::Vector3& ray_dir)
{
    const float b = axis_dir.dot_prod(ray_dir);
    const float denom = 1.0f - (b * b);
    if (std::fabs(denom) <= 1e-4f) {
        return std::nullopt;
    }

    const rf::Vector3 w0 = axis_origin - ray_origin;
    const float d = axis_dir.dot_prod(w0);
    const float e = ray_dir.dot_prod(w0);
    const float t = (b * e - d) / denom;
    if (!std::isfinite(t)) {
        return std::nullopt;
    }
    return t;
}

rf::Vector3 gizmo_axis_dir(const WaypointGizmoAxis axis)
{
    switch (axis) {
        case WaypointGizmoAxis::x:
            return {1.0f, 0.0f, 0.0f};
        case WaypointGizmoAxis::y:
            return {0.0f, 1.0f, 0.0f};
        case WaypointGizmoAxis::z:
            return {0.0f, 0.0f, 1.0f};
        case WaypointGizmoAxis::none:
        default:
            return {};
    }
}

bool project_world_to_screen(const rf::Vector3& world_pos, float& out_sx, float& out_sy)
{
    rf::gr::Vertex v{};
    if (!rf::gr::rotate_vertex(&v, &world_pos)) {
        rf::gr::project_vertex(&v);
        if (v.flags & rf::gr::VF_PROJECTED) {
            out_sx = v.sx;
            out_sy = v.sy;
            return true;
        }
    }
    return false;
}

float distance_sq_point_to_segment_2d(
    const float px,
    const float py,
    const float ax,
    const float ay,
    const float bx,
    const float by)
{
    const float vx = bx - ax;
    const float vy = by - ay;
    const float wx = px - ax;
    const float wy = py - ay;

    const float len_sq = vx * vx + vy * vy;
    if (len_sq <= 0.0001f) {
        return wx * wx + wy * wy;
    }

    const float t = std::clamp((wx * vx + wy * vy) / len_sq, 0.0f, 1.0f);
    const float cx = ax + (vx * t);
    const float cy = ay + (vy * t);
    const float dx = px - cx;
    const float dy = py - cy;
    return dx * dx + dy * dy;
}

struct GizmoAxisHover
{
    WaypointGizmoAxis axis = WaypointGizmoAxis::none;
    float dist_sq = std::numeric_limits<float>::max();
};

std::optional<GizmoAxisHover> determine_hovered_gizmo_axis(
    const rf::Vector3& gizmo_center,
    const float axis_length,
    const int pointer_x,
    const int pointer_y)
{
    float center_sx = 0.0f;
    float center_sy = 0.0f;
    if (!project_world_to_screen(gizmo_center, center_sx, center_sy)) {
        return std::nullopt;
    }

    constexpr float kAxisPickThresholdSq = 32.0f * 32.0f;
    WaypointGizmoAxis best_axis = WaypointGizmoAxis::none;
    float best_dist_sq = kAxisPickThresholdSq;

    static constexpr std::array<WaypointGizmoAxis, 3> kAxes{
        WaypointGizmoAxis::x,
        WaypointGizmoAxis::y,
        WaypointGizmoAxis::z,
    };

    for (const auto axis : kAxes) {
        const rf::Vector3 axis_end = gizmo_center + gizmo_axis_dir(axis) * axis_length;
        float end_sx = 0.0f;
        float end_sy = 0.0f;
        if (!project_world_to_screen(axis_end, end_sx, end_sy)) {
            continue;
        }

        const float dist_sq = distance_sq_point_to_segment_2d(
            static_cast<float>(pointer_x),
            static_cast<float>(pointer_y),
            center_sx,
            center_sy,
            end_sx,
            end_sy);
        if (dist_sq <= best_dist_sq) {
            best_dist_sq = dist_sq;
            best_axis = axis;
        }
    }

    if (best_axis == WaypointGizmoAxis::none) {
        return std::nullopt;
    }
    return GizmoAxisHover{best_axis, best_dist_sq};
}

void end_waypoint_gizmo_drag(const bool finalize)
{
    if (finalize && g_waypoint_gizmo_drag.active && g_waypoint_gizmo_drag.moved) {
        finalize_selection_after_translation(g_waypoint_gizmo_drag.selection);
        push_waypoint_editor_log(std::format(
            "Moved {} with translation gizmo",
            selection_debug_name(g_waypoint_gizmo_drag.selection)));
    }

    g_waypoint_gizmo_drag = {};
    g_waypoint_gizmo_hover_axis = WaypointGizmoAxis::none;
    g_waypoint_gizmo_hover_zone_corner = -1;
}

void begin_waypoint_gizmo_drag(
    const WaypointGizmoAxis axis,
    const rf::Vector3& selection_center,
    const int zone_box_corner = -1)
{
    if (axis == WaypointGizmoAxis::none) {
        return;
    }

    rf::Vector3 ray_origin{};
    rf::Vector3 ray_dir{};
    float ray_max_dist = 0.0f;
    if (!get_waypoint_editor_camera_ray(ray_origin, ray_dir, ray_max_dist)) {
        return;
    }

    const rf::Vector3 axis_dir = gizmo_axis_dir(axis);
    const float start_t = closest_axis_t_to_camera_ray(
            selection_center,
            axis_dir,
            ray_origin,
            ray_dir)
            .value_or(0.0f);

    g_waypoint_gizmo_drag.active = true;
    g_waypoint_gizmo_drag.moved = false;
    g_waypoint_gizmo_drag.selection = g_waypoint_editor_selection;
    g_waypoint_gizmo_drag.axis = axis;
    g_waypoint_gizmo_drag.zone_box_corner = zone_box_corner;
    g_waypoint_gizmo_drag.axis_dir = axis_dir;
    g_waypoint_gizmo_drag.start_center = selection_center;
    g_waypoint_gizmo_drag.start_axis_t = start_t;
}

void update_waypoint_gizmo_drag()
{
    if (!g_waypoint_gizmo_drag.active) {
        return;
    }

    if (!rf::mouse_button_is_down(0)) {
        end_waypoint_gizmo_drag(true);
        return;
    }

    rf::Vector3 ray_origin{};
    rf::Vector3 ray_dir{};
    float ray_max_dist = 0.0f;
    if (!get_waypoint_editor_camera_ray(ray_origin, ray_dir, ray_max_dist)) {
        return;
    }

    auto axis_t = closest_axis_t_to_camera_ray(
        g_waypoint_gizmo_drag.start_center,
        g_waypoint_gizmo_drag.axis_dir,
        ray_origin,
        ray_dir);
    if (!axis_t) {
        return;
    }

    const float axis_delta = axis_t.value() - g_waypoint_gizmo_drag.start_axis_t;
    const rf::Vector3 desired_center =
        g_waypoint_gizmo_drag.start_center + g_waypoint_gizmo_drag.axis_dir * axis_delta;

    auto current_center =
        g_waypoint_gizmo_drag.zone_box_corner >= 0
            ? get_zone_box_corner_world_pos(
                  g_waypoint_gizmo_drag.selection,
                  g_waypoint_gizmo_drag.zone_box_corner)
            : get_selection_center(g_waypoint_gizmo_drag.selection);
    if (!current_center) {
        end_waypoint_gizmo_drag(false);
        return;
    }

    const rf::Vector3 delta = desired_center - current_center.value();
    if (apply_selection_translation(
            g_waypoint_gizmo_drag.selection,
            delta,
            g_waypoint_gizmo_drag.zone_box_corner)) {
        g_waypoint_gizmo_drag.moved = true;
    }
}

bool consume_left_click_in_rect(const WaypointEditorRect& rect)
{
    if (!g_waypoint_editor_mouse_ui_mode
        || !g_waypoint_editor_left_click_pressed
        || g_waypoint_editor_left_click_consumed
        || !rect.contains(g_waypoint_editor_mouse_x, g_waypoint_editor_mouse_y)) {
        return false;
    }

    g_waypoint_editor_left_click_consumed = true;
    return true;
}

bool draw_waypoint_editor_button(
    const WaypointEditorRect& rect,
    const char* label,
    const int font_id,
    const bool enabled = true,
    const bool highlighted = false)
{
    const bool hovered = rect.contains(g_waypoint_editor_mouse_x, g_waypoint_editor_mouse_y);

    rf::Color fill = enabled
        ? (highlighted
              ? (hovered ? rf::Color{74, 112, 72, 235} : rf::Color{52, 86, 53, 225})
              : (hovered ? rf::Color{88, 88, 88, 228} : rf::Color{58, 58, 58, 220}))
        : rf::Color{38, 38, 38, 180};

    rf::gr::set_color(fill);
    rf::gr::rect(rect.x, rect.y, rect.w, rect.h);
    rf::gr::set_color(
        enabled
            ? (highlighted ? rf::Color{170, 235, 170, 255} : rf::Color{200, 200, 200, 255})
            : rf::Color{110, 110, 110, 255});
    rf::gr::rect_border(rect.x, rect.y, rect.w, rect.h);

    const auto [text_w, text_h] = rf::gr::get_string_size(label, font_id);
    const int text_x = rect.x + (rect.w - text_w) / 2;
    const int text_y = rect.y + (rect.h - text_h) / 2;
    rf::gr::set_color(
        enabled
            ? (highlighted ? rf::Color{236, 255, 236, 255} : rf::Color{255, 255, 255, 255})
            : rf::Color{150, 150, 150, 255});
    rf::gr::string(text_x, text_y, label, font_id, no_overdraw_2d_text);

    return enabled && hovered && consume_left_click_in_rect(rect);
}

std::optional<int> parse_link_field_waypoint_uid(const std::string& field)
{
    std::string compact{};
    compact.reserve(field.size());
    for (const char ch : field) {
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            compact.push_back(ch);
        }
    }

    if (compact.empty()) {
        return std::nullopt;
    }

    int waypoint_uid = 0;
    const char* const begin = compact.data();
    const char* const end = begin + compact.size();
    const auto [ptr, ec] = std::from_chars(begin, end, waypoint_uid);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }

    return waypoint_uid;
}

std::vector<int> collect_inbound_waypoint_uids(const int waypoint_uid, bool& out_truncated)
{
    std::vector<int> inbound{};
    inbound.reserve(kMaxWaypointLinks);
    out_truncated = false;

    for (int candidate_uid = 1; candidate_uid < static_cast<int>(g_waypoints.size()); ++candidate_uid) {
        if (!is_valid_waypoint_uid_local(candidate_uid)
            || candidate_uid == waypoint_uid
            || !waypoint_has_link_to(candidate_uid, waypoint_uid)) {
            continue;
        }

        if (inbound.size() >= static_cast<size_t>(kMaxWaypointLinks)) {
            out_truncated = true;
            break;
        }

        inbound.push_back(candidate_uid);
    }

    return inbound;
}

void open_waypoint_link_editor_dialog(const bool edit_inbound_links)
{
    if (g_waypoint_editor_selection.kind != WaypointEditorSelectionKind::waypoint
        || !is_valid_waypoint_uid_local(g_waypoint_editor_selection.uid)) {
        return;
    }

    g_waypoint_link_editor_dialog = {};
    g_waypoint_link_editor_dialog.open = true;
    g_waypoint_link_editor_dialog.edit_inbound_links = edit_inbound_links;
    g_waypoint_link_editor_dialog.waypoint_uid = g_waypoint_editor_selection.uid;
    g_waypoint_link_editor_dialog.active_field = 0;

    std::vector<int> seed_links{};
    if (edit_inbound_links) {
        seed_links = collect_inbound_waypoint_uids(
            g_waypoint_link_editor_dialog.waypoint_uid,
            g_waypoint_link_editor_dialog.inbound_list_truncated);
    }
    else {
        const auto& node = g_waypoints[g_waypoint_link_editor_dialog.waypoint_uid];
        seed_links.reserve(node.num_links);
        for (int i = 0; i < node.num_links; ++i) {
            seed_links.push_back(node.links[i]);
        }
    }

    for (int i = 0; i < static_cast<int>(seed_links.size()) && i < kMaxWaypointLinks; ++i) {
        g_waypoint_link_editor_dialog.fields[i] = std::to_string(seed_links[i]);
    }

    push_waypoint_editor_log(std::format(
        "Opened {} link editor for waypoint {}",
        edit_inbound_links ? "inbound" : "outbound",
        g_waypoint_link_editor_dialog.waypoint_uid));
}

void close_waypoint_link_editor_dialog(const bool save_changes)
{
    if (!g_waypoint_link_editor_dialog.open) {
        return;
    }

    if (!save_changes) {
        g_waypoint_editor_pending_numeric_key_counts.fill(0);
        g_waypoint_link_editor_dialog = {};
        return;
    }

    const int waypoint_uid = g_waypoint_link_editor_dialog.waypoint_uid;
    if (!is_valid_waypoint_uid_local(waypoint_uid)) {
        g_waypoint_editor_pending_numeric_key_counts.fill(0);
        g_waypoint_link_editor_dialog = {};
        return;
    }

    std::unordered_set<int> unique_waypoints{};
    std::vector<int> edited_waypoints{};
    edited_waypoints.reserve(kMaxWaypointLinks);

    for (const auto& field : g_waypoint_link_editor_dialog.fields) {
        const auto parsed_uid = parse_link_field_waypoint_uid(field);
        if (!parsed_uid) {
            continue;
        }

        const int candidate_uid = parsed_uid.value();
        if (!is_valid_waypoint_uid_local(candidate_uid)
            || candidate_uid == waypoint_uid
            || !unique_waypoints.insert(candidate_uid).second) {
            continue;
        }

        edited_waypoints.push_back(candidate_uid);
    }

    if (g_waypoint_link_editor_dialog.edit_inbound_links) {
        remove_waypoint_links_to_from_all(waypoint_uid);
        for (const int from_uid : edited_waypoints) {
            link_waypoint(from_uid, waypoint_uid);
        }
        push_waypoint_editor_log(std::format(
            "Saved inbound links for waypoint {} ({} link(s))",
            waypoint_uid,
            static_cast<int>(edited_waypoints.size())));
    }
    else {
        remove_waypoint_links_from_all(waypoint_uid);
        for (const int to_uid : edited_waypoints) {
            link_waypoint(waypoint_uid, to_uid);
        }
        push_waypoint_editor_log(std::format(
            "Saved outbound links for waypoint {} ({} link(s))",
            waypoint_uid,
            static_cast<int>(edited_waypoints.size())));
    }

    g_waypoint_editor_pending_numeric_key_counts.fill(0);
    g_waypoint_link_editor_dialog = {};
}

void open_waypoint_zone_create_dialog()
{
    g_waypoint_zone_create_dialog = {};
    g_waypoint_editor_pending_numeric_key_counts.fill(0);
    g_waypoint_zone_create_dialog.open = true;
    g_waypoint_zone_create_dialog.stage = WaypointZoneCreateDialogStage::select_type;
    g_waypoint_zone_create_dialog.selected_type = WaypointZoneType::control_point;
    push_waypoint_editor_log("Opened zone creation dialog");
}

void close_waypoint_zone_create_dialog()
{
    g_waypoint_editor_pending_numeric_key_counts.fill(0);
    g_waypoint_zone_create_dialog = {};
}

void open_waypoint_target_create_dialog()
{
    g_waypoint_target_create_dialog = {};
    g_waypoint_target_create_dialog.open = true;
    push_waypoint_editor_log("Opened target creation dialog");
}

void close_waypoint_target_create_dialog()
{
    g_waypoint_target_create_dialog = {};
}

int normalize_waypoint_subtype_for_type(const WaypointType type, const int subtype)
{
    switch (type) {
        case WaypointType::std:
        case WaypointType::std_new:
        case WaypointType::jump_pad:
        case WaypointType::crater:
        case WaypointType::tele_entrance:
        case WaypointType::tele_exit:
        case WaypointType::lift_body:
        case WaypointType::lift_entrance:
        case WaypointType::lift_exit:
        case WaypointType::ladder:
            return 0;
        case WaypointType::respawn:
            return std::clamp(subtype, 0, 3);
        case WaypointType::ctf_flag:
            return std::clamp(subtype, 0, 1);
        case WaypointType::item:
            return subtype;
        default:
            return subtype;
    }
}

void open_waypoint_type_change_dialog_for_selection()
{
    WaypointTypeChangeDialogState dialog{};
    dialog.open = true;
    dialog.uid = g_waypoint_editor_selection.uid;
    switch (g_waypoint_editor_selection.kind) {
        case WaypointEditorSelectionKind::waypoint:
            if (!is_valid_waypoint_uid_local(dialog.uid)) {
                return;
            }
            dialog.subject = WaypointTypeChangeDialogSubject::waypoint;
            break;
        case WaypointEditorSelectionKind::zone:
            if (dialog.uid < 0 || dialog.uid >= static_cast<int>(g_waypoint_zones.size())) {
                return;
            }
            dialog.subject = WaypointTypeChangeDialogSubject::zone;
            break;
        case WaypointEditorSelectionKind::target:
            if (!find_waypoint_target_by_uid(dialog.uid)) {
                return;
            }
            dialog.subject = WaypointTypeChangeDialogSubject::target;
            break;
        case WaypointEditorSelectionKind::none:
        default:
            return;
    }

    g_waypoint_type_change_dialog = dialog;
    push_waypoint_editor_log(std::format(
        "Opened type change dialog for {}",
        selection_debug_name(g_waypoint_editor_selection)));
}

void close_waypoint_type_change_dialog()
{
    g_waypoint_type_change_dialog = {};
}

bool apply_waypoint_type_change(const WaypointType new_type)
{
    const int waypoint_uid = g_waypoint_type_change_dialog.uid;
    if (!is_valid_waypoint_uid_local(waypoint_uid)) {
        return false;
    }

    auto& node = g_waypoints[waypoint_uid];
    const WaypointType old_type = node.type;
    if (old_type == new_type) {
        return true;
    }

    node.type = new_type;
    node.subtype = normalize_waypoint_subtype_for_type(new_type, node.subtype);
    refresh_all_waypoint_zone_refs();
    rf::console::print(
        "Waypoint {} type changed {} -> {}",
        waypoint_uid,
        waypoint_type_name(old_type),
        waypoint_type_name(new_type));
    push_waypoint_editor_log(std::format(
        "Waypoint {} type changed {} -> {}",
        waypoint_uid,
        waypoint_type_name(old_type),
        waypoint_type_name(new_type)));
    return true;
}

bool apply_zone_type_change(const WaypointZoneType new_type)
{
    const int zone_uid = g_waypoint_type_change_dialog.uid;
    if (zone_uid < 0 || zone_uid >= static_cast<int>(g_waypoint_zones.size())) {
        return false;
    }

    auto& zone = g_waypoint_zones[zone_uid];
    const WaypointZoneType old_type = zone.type;
    if (old_type == new_type) {
        return true;
    }

    const WaypointZoneSource source = resolve_waypoint_zone_source(zone);
    if (waypoint_zone_type_is_bridge(new_type) && source != WaypointZoneSource::trigger_uid) {
        rf::console::print(
            "Zone type {} only supports trigger source",
            waypoint_zone_type_name(new_type));
        return false;
    }

    zone.type = new_type;
    if (waypoint_zone_type_is_bridge(zone.type)) {
        if (!std::isfinite(zone.duration) || zone.duration < 0.0f) {
            zone.duration = 5.0f;
        }
        zone.on = false;
        zone.timer.invalidate();
    }
    else {
        zone.bridge_waypoint_uids.clear();
        zone.on = false;
        zone.timer.invalidate();
    }

    refresh_all_waypoint_zone_refs();
    rf::console::print(
        "Zone {} type changed {} -> {}",
        zone_uid,
        waypoint_zone_type_name(old_type),
        waypoint_zone_type_name(new_type));
    push_waypoint_editor_log(std::format(
        "Zone {} type changed {} -> {}",
        zone_uid,
        waypoint_zone_type_name(old_type),
        waypoint_zone_type_name(new_type)));
    return true;
}

bool apply_target_type_change(const WaypointTargetType new_type)
{
    WaypointTargetDefinition* target = find_waypoint_target_by_uid(g_waypoint_type_change_dialog.uid);
    if (!target) {
        return false;
    }

    const WaypointTargetType old_type = target->type;
    if (old_type == new_type) {
        return true;
    }

    int new_shatter_room_key = -1;
    rf::Vector3 new_shatter_pos{};
    if (new_type == WaypointTargetType::shatter) {
        if (!waypoints_find_nearest_breakable_glass_face_point(
                target->pos,
                new_shatter_pos,
                new_shatter_room_key)) {
            rf::console::print(
                "Target {} type change to shatter failed: target is not on breakable glass",
                target->uid);
            push_waypoint_editor_log(std::format(
                "Target {} type change to shatter failed: target is not on breakable glass",
                target->uid));
            return false;
        }
    }

    target->type = new_type;
    if (new_type == WaypointTargetType::shatter) {
        target->identifier = new_shatter_room_key;
        target->pos = new_shatter_pos;
    }
    else if (old_type == WaypointTargetType::shatter) {
        target->identifier = -1;
    }
    rebuild_target_waypoint_refs(*target);
    rf::console::print(
        "Target {} type changed {} -> {}",
        target->uid,
        waypoint_target_type_name(old_type),
        waypoint_target_type_name(new_type));
    push_waypoint_editor_log(std::format(
        "Target {} type changed {} -> {}",
        target->uid,
        waypoint_target_type_name(old_type),
        waypoint_target_type_name(new_type)));
    return true;
}

bool create_zone_from_trigger_uid_field()
{
    const auto parsed_uid = parse_link_field_waypoint_uid(g_waypoint_zone_create_dialog.trigger_uid_field);
    if (!parsed_uid || parsed_uid.value() < 0) {
        rf::console::print("Invalid trigger UID '{}'", g_waypoint_zone_create_dialog.trigger_uid_field);
        return false;
    }

    if (!create_new_zone_on_trigger(g_waypoint_zone_create_dialog.selected_type, parsed_uid.value())) {
        return false;
    }

    close_waypoint_zone_create_dialog();
    return true;
}

void append_text_from_key_counter(std::string& field, const rf::Key key, const char ch)
{
    const int count = rf::key_get_and_reset_down_counter(key);
    constexpr size_t kMaxFieldChars = 12;
    for (int i = 0; i < count && field.size() < kMaxFieldChars; ++i) {
        field.push_back(ch);
    }
}

void append_text_from_count(std::string& field, const int count, const char ch)
{
    constexpr size_t kMaxFieldChars = 12;
    for (int i = 0; i < count && field.size() < kMaxFieldChars; ++i) {
        field.push_back(ch);
    }
}

void process_link_editor_keyboard_input()
{
    if (!g_waypoint_link_editor_dialog.open || !g_waypoint_editor_mouse_ui_mode) {
        return;
    }

    if (rf::key_get_and_reset_down_counter(rf::KEY_ESC) > 0) {
        close_waypoint_link_editor_dialog(false);
        return;
    }

    if (rf::key_get_and_reset_down_counter(rf::KEY_ENTER) > 0
        || rf::key_get_and_reset_down_counter(rf::KEY_PADENTER) > 0) {
        close_waypoint_link_editor_dialog(true);
        return;
    }

    if (g_waypoint_link_editor_dialog.active_field < 0
        || g_waypoint_link_editor_dialog.active_field >= kMaxWaypointLinks) {
        return;
    }

    auto& field = g_waypoint_link_editor_dialog.fields[g_waypoint_link_editor_dialog.active_field];

    int backspace_count = rf::key_get_and_reset_down_counter(rf::KEY_BACKSP);
    while (backspace_count-- > 0 && !field.empty()) {
        field.pop_back();
    }

    const int minus_count = rf::key_get_and_reset_down_counter(rf::KEY_MINUS)
        + rf::key_get_and_reset_down_counter(rf::KEY_PADMINUS);
    if (minus_count > 0 && field.empty()) {
        field.push_back('-');
    }

    append_text_from_count(
        field,
        waypoints_utils_consume_numeric_key(static_cast<int>(rf::KEY_0))
            + rf::key_get_and_reset_down_counter(rf::KEY_0),
        '0');
    append_text_from_count(
        field,
        waypoints_utils_consume_numeric_key(static_cast<int>(rf::KEY_1))
            + rf::key_get_and_reset_down_counter(rf::KEY_1),
        '1');
    append_text_from_count(
        field,
        waypoints_utils_consume_numeric_key(static_cast<int>(rf::KEY_2))
            + rf::key_get_and_reset_down_counter(rf::KEY_2),
        '2');
    append_text_from_count(
        field,
        waypoints_utils_consume_numeric_key(static_cast<int>(rf::KEY_3))
            + rf::key_get_and_reset_down_counter(rf::KEY_3),
        '3');
    append_text_from_count(
        field,
        waypoints_utils_consume_numeric_key(static_cast<int>(rf::KEY_4))
            + rf::key_get_and_reset_down_counter(rf::KEY_4),
        '4');
    append_text_from_count(
        field,
        waypoints_utils_consume_numeric_key(static_cast<int>(rf::KEY_5))
            + rf::key_get_and_reset_down_counter(rf::KEY_5),
        '5');
    append_text_from_count(
        field,
        waypoints_utils_consume_numeric_key(static_cast<int>(rf::KEY_6))
            + rf::key_get_and_reset_down_counter(rf::KEY_6),
        '6');
    append_text_from_count(
        field,
        waypoints_utils_consume_numeric_key(static_cast<int>(rf::KEY_7))
            + rf::key_get_and_reset_down_counter(rf::KEY_7),
        '7');
    append_text_from_count(
        field,
        waypoints_utils_consume_numeric_key(static_cast<int>(rf::KEY_8))
            + rf::key_get_and_reset_down_counter(rf::KEY_8),
        '8');
    append_text_from_count(
        field,
        waypoints_utils_consume_numeric_key(static_cast<int>(rf::KEY_9))
            + rf::key_get_and_reset_down_counter(rf::KEY_9),
        '9');

    append_text_from_key_counter(field, rf::KEY_PAD0, '0');
    append_text_from_key_counter(field, rf::KEY_PAD1, '1');
    append_text_from_key_counter(field, rf::KEY_PAD2, '2');
    append_text_from_key_counter(field, rf::KEY_PAD3, '3');
    append_text_from_key_counter(field, rf::KEY_PAD4, '4');
    append_text_from_key_counter(field, rf::KEY_PAD5, '5');
    append_text_from_key_counter(field, rf::KEY_PAD6, '6');
    append_text_from_key_counter(field, rf::KEY_PAD7, '7');
    append_text_from_key_counter(field, rf::KEY_PAD8, '8');
    append_text_from_key_counter(field, rf::KEY_PAD9, '9');
}

void process_zone_create_trigger_uid_keyboard_input()
{
    if (!g_waypoint_zone_create_dialog.open
        || g_waypoint_zone_create_dialog.stage != WaypointZoneCreateDialogStage::enter_trigger_uid
        || !g_waypoint_editor_mouse_ui_mode) {
        return;
    }

    if (rf::key_get_and_reset_down_counter(rf::KEY_ESC) > 0) {
        close_waypoint_zone_create_dialog();
        return;
    }

    if (rf::key_get_and_reset_down_counter(rf::KEY_ENTER) > 0
        || rf::key_get_and_reset_down_counter(rf::KEY_PADENTER) > 0) {
        create_zone_from_trigger_uid_field();
        return;
    }

    auto& field = g_waypoint_zone_create_dialog.trigger_uid_field;
    int backspace_count = rf::key_get_and_reset_down_counter(rf::KEY_BACKSP);
    while (backspace_count-- > 0 && !field.empty()) {
        field.pop_back();
    }

    append_text_from_count(
        field,
        waypoints_utils_consume_numeric_key(static_cast<int>(rf::KEY_0))
            + rf::key_get_and_reset_down_counter(rf::KEY_0),
        '0');
    append_text_from_count(
        field,
        waypoints_utils_consume_numeric_key(static_cast<int>(rf::KEY_1))
            + rf::key_get_and_reset_down_counter(rf::KEY_1),
        '1');
    append_text_from_count(
        field,
        waypoints_utils_consume_numeric_key(static_cast<int>(rf::KEY_2))
            + rf::key_get_and_reset_down_counter(rf::KEY_2),
        '2');
    append_text_from_count(
        field,
        waypoints_utils_consume_numeric_key(static_cast<int>(rf::KEY_3))
            + rf::key_get_and_reset_down_counter(rf::KEY_3),
        '3');
    append_text_from_count(
        field,
        waypoints_utils_consume_numeric_key(static_cast<int>(rf::KEY_4))
            + rf::key_get_and_reset_down_counter(rf::KEY_4),
        '4');
    append_text_from_count(
        field,
        waypoints_utils_consume_numeric_key(static_cast<int>(rf::KEY_5))
            + rf::key_get_and_reset_down_counter(rf::KEY_5),
        '5');
    append_text_from_count(
        field,
        waypoints_utils_consume_numeric_key(static_cast<int>(rf::KEY_6))
            + rf::key_get_and_reset_down_counter(rf::KEY_6),
        '6');
    append_text_from_count(
        field,
        waypoints_utils_consume_numeric_key(static_cast<int>(rf::KEY_7))
            + rf::key_get_and_reset_down_counter(rf::KEY_7),
        '7');
    append_text_from_count(
        field,
        waypoints_utils_consume_numeric_key(static_cast<int>(rf::KEY_8))
            + rf::key_get_and_reset_down_counter(rf::KEY_8),
        '8');
    append_text_from_count(
        field,
        waypoints_utils_consume_numeric_key(static_cast<int>(rf::KEY_9))
            + rf::key_get_and_reset_down_counter(rf::KEY_9),
        '9');

    append_text_from_key_counter(field, rf::KEY_PAD0, '0');
    append_text_from_key_counter(field, rf::KEY_PAD1, '1');
    append_text_from_key_counter(field, rf::KEY_PAD2, '2');
    append_text_from_key_counter(field, rf::KEY_PAD3, '3');
    append_text_from_key_counter(field, rf::KEY_PAD4, '4');
    append_text_from_key_counter(field, rf::KEY_PAD5, '5');
    append_text_from_key_counter(field, rf::KEY_PAD6, '6');
    append_text_from_key_counter(field, rf::KEY_PAD7, '7');
    append_text_from_key_counter(field, rf::KEY_PAD8, '8');
    append_text_from_key_counter(field, rf::KEY_PAD9, '9');
}

void execute_waypoint_editor_console_command(const char* command, std::string_view log_line)
{
    if (!command) {
        return;
    }
    rf::console::do_command(command);
    if (!log_line.empty()) {
        push_waypoint_editor_log(std::string{log_line});
    }
}

rf::Color debug_waypoint_color(WaypointType type)
{
    switch (type) {
        case WaypointType::std:
            return {255, 255, 255, 150};
        case WaypointType::std_new:
            return {255, 255, 255, 75};
        case WaypointType::item:
            return {255, 220, 0, 150};
        case WaypointType::respawn:
            return {0, 220, 255, 150};
        case WaypointType::jump_pad:
            return {0, 255, 120, 150};
        case WaypointType::lift_body:
            return {110, 150, 255, 150};
        case WaypointType::lift_entrance:
            return {140, 180, 255, 150};
        case WaypointType::lift_exit:
            return {80, 120, 255, 150};
        case WaypointType::ladder:
            return {255, 170, 70, 150};
        case WaypointType::ctf_flag:
            return {255, 70, 70, 150};
        case WaypointType::crater:
            return {200, 70, 255, 150};
        case WaypointType::tele_entrance:
            return {255, 140, 60, 150};
        case WaypointType::tele_exit:
            return {255, 80, 220, 150};
        default:
            return {200, 200, 200, 150};
    }
}

float debug_waypoint_sphere_scale(WaypointType type)
{
    if (type == WaypointType::std || type == WaypointType::std_new) {
        return 0.125f;
    }
    return 0.25f;
}

rf::Color debug_waypoint_zone_color(WaypointZoneType type)
{
    switch (type) {
        case WaypointZoneType::control_point:
            return {200, 70, 255, 150};
        case WaypointZoneType::damaging_liquid_room:
            return {70, 160, 255, 150};
        case WaypointZoneType::damage_zone:
            return {255, 150, 70, 150};
        case WaypointZoneType::instant_death_zone:
            return {255, 60, 60, 150};
        case WaypointZoneType::bridge_use:
            return {120, 255, 120, 150};
        case WaypointZoneType::bridge_prox:
            return {80, 220, 255, 150};
        case WaypointZoneType::high_power_zone:
            return {255, 220, 80, 150};
        default:
            return {200, 200, 200, 150};
    }
}

rf::Color debug_waypoint_target_color(WaypointTargetType type)
{
    switch (type) {
        case WaypointTargetType::explosion:
            return {255, 120, 40, 150};
        case WaypointTargetType::shatter:
            return {120, 220, 255, 150};
        case WaypointTargetType::jump:
            return {140, 255, 120, 150};
        default:
            return {200, 200, 200, 150};
    }
}

void draw_debug_wire_box(const std::array<rf::Vector3, 8>& corners)
{
    static constexpr std::array<std::array<int, 2>, 12> kBoxEdges{{
        {0, 1},
        {0, 2},
        {0, 4},
        {1, 3},
        {1, 5},
        {2, 3},
        {2, 6},
        {3, 7},
        {4, 5},
        {4, 6},
        {5, 7},
        {6, 7},
    }};

    for (const auto& edge : kBoxEdges) {
        rf::gr::line_vec(corners[edge[0]], corners[edge[1]], no_overdraw_2d_line);
    }
}

bool draw_debug_trigger_zone_bounds(const rf::Trigger& trigger, const rf::Color& color, rf::Vector3& out_center)
{
    out_center = trigger.pos;
    rf::gr::set_color(color);

    if (trigger.type == 1) {
        const rf::Vector3 half_extents{
            std::fabs(trigger.box_size.x) * 0.5f,
            std::fabs(trigger.box_size.y) * 0.5f,
            std::fabs(trigger.box_size.z) * 0.5f,
        };
        const auto& orient = trigger.orient;
        const rf::Vector3 center = trigger.pos;
        const rf::Vector3 r = orient.rvec * half_extents.x;
        const rf::Vector3 u = orient.uvec * half_extents.y;
        const rf::Vector3 f = orient.fvec * half_extents.z;

        const std::array<rf::Vector3, 8> corners{
            center - r - u - f,
            center - r - u + f,
            center - r + u - f,
            center - r + u + f,
            center + r - u - f,
            center + r - u + f,
            center + r + u - f,
            center + r + u + f,
        };
        draw_debug_wire_box(corners);
        return true;
    }

    const float radius = std::fabs(trigger.radius);
    if (radius <= 0.0f) {
        return false;
    }
    rf::gr::sphere(trigger.pos, radius, no_overdraw_2d_line);
    return true;
}

bool draw_debug_extent_zone_bounds(const WaypointZoneDefinition& zone, const rf::Color& color, rf::Vector3& out_center)
{
    const rf::Vector3 min_bound = point_min(zone.box_min, zone.box_max);
    const rf::Vector3 max_bound = point_max(zone.box_min, zone.box_max);
    out_center = (min_bound + max_bound) * 0.5f;

    const std::array<rf::Vector3, 8> corners{
        rf::Vector3{min_bound.x, min_bound.y, min_bound.z},
        rf::Vector3{min_bound.x, min_bound.y, max_bound.z},
        rf::Vector3{min_bound.x, max_bound.y, min_bound.z},
        rf::Vector3{min_bound.x, max_bound.y, max_bound.z},
        rf::Vector3{max_bound.x, min_bound.y, min_bound.z},
        rf::Vector3{max_bound.x, min_bound.y, max_bound.z},
        rf::Vector3{max_bound.x, max_bound.y, min_bound.z},
        rf::Vector3{max_bound.x, max_bound.y, max_bound.z},
    };

    rf::gr::set_color(color);
    draw_debug_wire_box(corners);
    return true;
}

bool draw_debug_target_bounds(const WaypointTargetDefinition& target, const rf::Color& color, rf::Vector3& out_center)
{
    constexpr float kHalfExtent = 0.5f; // 1x1x1 debug box
    out_center = target.pos;

    const std::array<rf::Vector3, 8> corners{
        rf::Vector3{target.pos.x - kHalfExtent, target.pos.y - kHalfExtent, target.pos.z - kHalfExtent},
        rf::Vector3{target.pos.x - kHalfExtent, target.pos.y - kHalfExtent, target.pos.z + kHalfExtent},
        rf::Vector3{target.pos.x - kHalfExtent, target.pos.y + kHalfExtent, target.pos.z - kHalfExtent},
        rf::Vector3{target.pos.x - kHalfExtent, target.pos.y + kHalfExtent, target.pos.z + kHalfExtent},
        rf::Vector3{target.pos.x + kHalfExtent, target.pos.y - kHalfExtent, target.pos.z - kHalfExtent},
        rf::Vector3{target.pos.x + kHalfExtent, target.pos.y - kHalfExtent, target.pos.z + kHalfExtent},
        rf::Vector3{target.pos.x + kHalfExtent, target.pos.y + kHalfExtent, target.pos.z - kHalfExtent},
        rf::Vector3{target.pos.x + kHalfExtent, target.pos.y + kHalfExtent, target.pos.z + kHalfExtent},
    };

    rf::gr::set_color(color);
    draw_debug_wire_box(corners);
    return true;
}

struct WaypointZoneDebugRenderInfo
{
    bool renderable = false;
    bool selected = false;
    rf::Vector3 center{};
    WaypointZoneType type = WaypointZoneType::control_point;
    int uid = -1;
    int identifier = -1;
    rf::Color color{};
};

struct WaypointTargetDebugRenderInfo
{
    bool renderable = false;
    bool selected = false;
    rf::Vector3 center{};
    WaypointTargetType type = WaypointTargetType::explosion;
    int uid = -1;
    int identifier = -1;
    rf::Color color{};
    std::vector<int> waypoint_uids{};
};

void draw_centered_debug_label(float sx, float sy, const char* text, const rf::Color& color)
{
    const auto [text_w, text_h] = rf::gr::get_string_size(text, -1);
    const int x = static_cast<int>(sx) - (text_w / 2);
    const int y = static_cast<int>(sy) - (text_h / 2);
    rf::gr::set_color(color.red, color.green, color.blue, 255);
    rf::gr::string(x, y, text, -1, no_overdraw_2d_text);
}

void draw_debug_waypoint_zones(bool show_membership_arrows, bool show_labels)
{
    if (g_waypoint_zones.empty()) {
        return;
    }

    std::vector<WaypointZoneDebugRenderInfo> zone_infos(g_waypoint_zones.size());

    for (int zone_index = 0; zone_index < static_cast<int>(g_waypoint_zones.size()); ++zone_index) {
        const auto& zone = g_waypoint_zones[zone_index];
        auto& zone_info = zone_infos[zone_index];
        zone_info.type = zone.type;
        zone_info.uid = zone_index;
        zone_info.identifier = zone.identifier;
        zone_info.selected = is_waypoint_editor_selected_zone(zone_index);
        zone_info.color = debug_waypoint_zone_color(zone.type);
        if (zone_info.selected) {
            zone_info.color.alpha = 255;
        }

        switch (resolve_waypoint_zone_source(zone)) {
            case WaypointZoneSource::trigger_uid: {
                rf::Object* trigger_obj = rf::obj_lookup_from_uid(zone.trigger_uid);
                if (!trigger_obj || trigger_obj->type != rf::OT_TRIGGER) {
                    break;
                }

                const auto* trigger = static_cast<rf::Trigger*>(trigger_obj);
                zone_info.renderable = draw_debug_trigger_zone_bounds(*trigger, zone_info.color, zone_info.center);
                break;
            }
            case WaypointZoneSource::box_extents:
                zone_info.renderable = draw_debug_extent_zone_bounds(zone, zone_info.color, zone_info.center);
                break;
            case WaypointZoneSource::room_uid:
            default:
                break;
        }
    }

    if (show_membership_arrows) {
        for (int wp_index = 1; wp_index < static_cast<int>(g_waypoints.size()); ++wp_index) {
            const auto& node = g_waypoints[wp_index];
            if (!node.valid) {
                continue;
            }

            for (int zone_index : node.zones) {
                if (zone_index < 0 || zone_index >= static_cast<int>(zone_infos.size())) {
                    continue;
                }
                const auto& zone_info = zone_infos[zone_index];
                if (!zone_info.renderable) {
                    continue;
                }

                rf::gr::line_arrow(
                    node.pos.x, node.pos.y, node.pos.z,
                    zone_info.center.x, zone_info.center.y, zone_info.center.z,
                    zone_info.color.red, zone_info.color.green, zone_info.color.blue);
            }
        }

        for (int zone_index = 0; zone_index < static_cast<int>(g_waypoint_zones.size()); ++zone_index) {
            const auto& zone = g_waypoint_zones[zone_index];
            if (!waypoint_zone_type_is_bridge(zone.type)) {
                continue;
            }

            const auto& zone_info = zone_infos[zone_index];
            if (!zone_info.renderable) {
                continue;
            }

            for (const int waypoint_uid : zone.bridge_waypoint_uids) {
                if (waypoint_uid <= 0 || waypoint_uid >= static_cast<int>(g_waypoints.size())) {
                    continue;
                }
                const auto& waypoint = g_waypoints[waypoint_uid];
                if (!waypoint.valid) {
                    continue;
                }

                rf::gr::line_arrow(
                    zone_info.center.x, zone_info.center.y, zone_info.center.z,
                    waypoint.pos.x, waypoint.pos.y, waypoint.pos.z,
                    255, 32, 32);
            }
        }
    }

    for (const auto& zone_info : zone_infos) {
        if ((!show_labels && !zone_info.selected) || !zone_info.renderable) {
            continue;
        }

        rf::Vector3 label_pos = zone_info.center;
        label_pos.y += 0.3f;
        rf::gr::Vertex dest{};
        if (!rf::gr::rotate_vertex(&dest, &label_pos)) {
            rf::gr::project_vertex(&dest);
            if (dest.flags & 1) {
                const auto label_sv = waypoint_zone_type_name(zone_info.type);
                char label[96]{};
                std::snprintf(
                    label, sizeof(label), "%.*s (%d : %d)",
                    static_cast<int>(label_sv.size()), label_sv.data(),
                    zone_info.uid, zone_info.identifier);
                draw_centered_debug_label(dest.sx, dest.sy, label, zone_info.color);
            }
        }
    }
}

void draw_debug_waypoint_targets(bool show_waypoint_arrows, bool show_labels)
{
    if (g_waypoint_targets.empty()) {
        return;
    }

    std::vector<WaypointTargetDebugRenderInfo> target_infos{};
    target_infos.reserve(g_waypoint_targets.size());
    for (const auto& target : g_waypoint_targets) {
        WaypointTargetDebugRenderInfo info{};
        info.type = target.type;
        info.uid = target.uid;
        info.identifier = target.identifier;
        info.selected = is_waypoint_editor_selected_target(target.uid);
        info.color = debug_waypoint_target_color(target.type);
        if (info.selected) {
            info.color.alpha = 255;
        }
        info.waypoint_uids = target.waypoint_uids;
        info.renderable = draw_debug_target_bounds(target, info.color, info.center);
        target_infos.push_back(std::move(info));
    }

    if (show_waypoint_arrows) {
        for (const auto& target_info : target_infos) {
            if (!target_info.renderable) {
                continue;
            }
            for (int waypoint_uid : target_info.waypoint_uids) {
                if (waypoint_uid <= 0 || waypoint_uid >= static_cast<int>(g_waypoints.size())) {
                    continue;
                }
                const auto& waypoint = g_waypoints[waypoint_uid];
                if (!waypoint.valid) {
                    continue;
                }

                rf::gr::line_arrow(
                    target_info.center.x, target_info.center.y, target_info.center.z,
                    waypoint.pos.x, waypoint.pos.y, waypoint.pos.z,
                    target_info.color.red, target_info.color.green, target_info.color.blue);
            }
        }
    }

    for (const auto& target_info : target_infos) {
        if ((!show_labels && !target_info.selected) || !target_info.renderable) {
            continue;
        }

        rf::Vector3 label_pos = target_info.center;
        label_pos.y += 0.3f;
        rf::gr::Vertex dest{};
        if (!rf::gr::rotate_vertex(&dest, &label_pos)) {
            rf::gr::project_vertex(&dest);
            if (dest.flags & 1) {
                const auto label_sv = waypoint_target_type_name(target_info.type);
                char label[96]{};
                std::snprintf(
                    label, sizeof(label), "%.*s (%d : %d)",
                    static_cast<int>(label_sv.size()), label_sv.data(),
                    target_info.uid, target_info.identifier);
                draw_centered_debug_label(dest.sx, dest.sy, label, target_info.color);
            }
        }
    }
}

void draw_debug_waypoints()
{
    if (g_debug_waypoints_mode == 0) {
        return;
    }
    const bool show_links = g_debug_waypoints_mode >= 1;
    const bool show_spheres = g_debug_waypoints_mode >= 2;
    const bool show_labels = g_debug_waypoints_mode >= 3;
    const bool show_zone_membership_arrows = g_debug_waypoints_mode >= 2;
    const bool show_zone_labels = g_debug_waypoints_mode >= 3;
    const bool show_target_waypoint_arrows = g_debug_waypoints_mode >= 2;
    const bool show_target_labels = g_debug_waypoints_mode >= 3;
    draw_debug_waypoint_zones(show_zone_membership_arrows, show_zone_labels);
    draw_debug_waypoint_targets(show_target_waypoint_arrows, show_target_labels);
    for (int i = 1; i < static_cast<int>(g_waypoints.size()); ++i) {
        const auto& node = g_waypoints[i];
        if (!node.valid) {
            continue;
        }
        const bool selected = is_waypoint_editor_selected_waypoint(i);
        if (show_spheres || selected) {
            auto color = debug_waypoint_color(node.type);
            if (selected) {
                color.alpha = 255;
            }
            rf::gr::set_color(color);
            const float debug_radius = sanitize_waypoint_link_radius(node.link_radius) * debug_waypoint_sphere_scale(node.type);
            rf::gr::sphere(node.pos, debug_radius, no_overdraw_2d_line);
        }
        if (show_labels || selected) {
            rf::Vector3 label_pos = node.pos;
            label_pos.y += 0.3f;
            rf::gr::Vertex dest;
            if (!rf::gr::rotate_vertex(&dest, &label_pos)) {
                rf::gr::project_vertex(&dest);
                if (dest.flags & 1) {
                    const auto type_name = waypoint_type_name(node.type);
                    const auto movement_subtype_name = waypoint_dropped_subtype_name(node.movement_subtype);
                    char buf[128];
                    if (waypoint_type_is_standard(node.type)) {
                        if (node.identifier >= 0) {
                            std::snprintf(
                                buf, sizeof(buf), "%.*s (%d : %.*s : %d)",
                                static_cast<int>(type_name.size()), type_name.data(),
                                i,
                                static_cast<int>(movement_subtype_name.size()), movement_subtype_name.data(),
                                node.identifier);
                        }
                        else {
                            std::snprintf(
                                buf, sizeof(buf), "%.*s (%d : %.*s)",
                                static_cast<int>(type_name.size()), type_name.data(),
                                i,
                                static_cast<int>(movement_subtype_name.size()), movement_subtype_name.data());
                        }
                    }
                    else if (node.identifier >= 0) {
                        std::snprintf(
                            buf, sizeof(buf), "%.*s (%d : %d : %.*s : %d)",
                            static_cast<int>(type_name.size()), type_name.data(),
                            i,
                            node.subtype,
                            static_cast<int>(movement_subtype_name.size()), movement_subtype_name.data(),
                            node.identifier);
                    }
                    else {
                        std::snprintf(
                            buf, sizeof(buf), "%.*s (%d : %d : %.*s)",
                            static_cast<int>(type_name.size()), type_name.data(),
                            i,
                            node.subtype,
                            static_cast<int>(movement_subtype_name.size()), movement_subtype_name.data());
                    }
                    draw_centered_debug_label(dest.sx, dest.sy, buf, rf::Color{255, 255, 255, 255});
                }
            }
        }
        if (show_links) {
            for (int j = 0; j < node.num_links; ++j) {
                int link = node.links[j];
                if (link <= 0 || link >= static_cast<int>(g_waypoints.size())) {
                    continue;
                }
                const auto& dest = g_waypoints[link];
                rf::gr::line_arrow(node.pos.x, node.pos.y, node.pos.z, dest.pos.x, dest.pos.y, dest.pos.z, 0, 255, 0);
            }
        }
    }
}
void update_waypoint_gizmo_interaction()
{
    if (!waypoint_editor_freelook_aim_mode_active()
        || g_waypoint_editor_selection.kind == WaypointEditorSelectionKind::none
        || waypoint_editor_modal_dialog_open()
        || selection_is_trigger_zone(g_waypoint_editor_selection)) {
        g_waypoint_gizmo_hover_axis = WaypointGizmoAxis::none;
        g_waypoint_gizmo_hover_zone_corner = -1;
        if (g_waypoint_gizmo_drag.active) {
            end_waypoint_gizmo_drag(true);
        }
        return;
    }

    constexpr float kGizmoAxisLength = 1.5f;
    const int pointer_x = rf::gr::clip_width() / 2;
    const int pointer_y = rf::gr::clip_height() / 2;
    const bool box_zone_selection = selection_is_box_extents_zone(g_waypoint_editor_selection);
    auto selection_center = get_selection_center(g_waypoint_editor_selection);
    if (!box_zone_selection && !selection_center) {
        g_waypoint_gizmo_hover_axis = WaypointGizmoAxis::none;
        g_waypoint_gizmo_hover_zone_corner = -1;
        if (g_waypoint_gizmo_drag.active) {
            end_waypoint_gizmo_drag(false);
        }
        return;
    }

    if (!g_waypoint_gizmo_drag.active) {
        g_waypoint_gizmo_hover_axis = WaypointGizmoAxis::none;
        g_waypoint_gizmo_hover_zone_corner = -1;
        if (box_zone_selection) {
            float best_dist_sq = std::numeric_limits<float>::max();
            for (int corner_index = 0; corner_index < 2; ++corner_index) {
                auto corner_pos = get_zone_box_corner_world_pos(g_waypoint_editor_selection, corner_index);
                if (!corner_pos) {
                    continue;
                }
                auto hover = determine_hovered_gizmo_axis(
                    corner_pos.value(),
                    kGizmoAxisLength,
                    pointer_x,
                    pointer_y);
                if (!hover || hover->dist_sq >= best_dist_sq) {
                    continue;
                }
                best_dist_sq = hover->dist_sq;
                g_waypoint_gizmo_hover_axis = hover->axis;
                g_waypoint_gizmo_hover_zone_corner = corner_index;
            }
        }
        else if (selection_center) {
            auto hover = determine_hovered_gizmo_axis(
                selection_center.value(),
                kGizmoAxisLength,
                pointer_x,
                pointer_y);
            if (hover) {
                g_waypoint_gizmo_hover_axis = hover->axis;
            }
        }
    }
    else {
        g_waypoint_editor_selection = g_waypoint_gizmo_drag.selection;
        g_waypoint_gizmo_hover_axis = g_waypoint_gizmo_drag.axis;
        g_waypoint_gizmo_hover_zone_corner = g_waypoint_gizmo_drag.zone_box_corner;
    }

    if (!g_waypoint_gizmo_drag.active
        && g_waypoint_editor_left_click_pressed
        && g_waypoint_gizmo_hover_axis != WaypointGizmoAxis::none) {
        auto drag_center =
            g_waypoint_gizmo_hover_zone_corner >= 0
                ? get_zone_box_corner_world_pos(
                      g_waypoint_editor_selection,
                      g_waypoint_gizmo_hover_zone_corner)
                : selection_center;
        if (drag_center) {
            begin_waypoint_gizmo_drag(
                g_waypoint_gizmo_hover_axis,
                drag_center.value(),
                g_waypoint_gizmo_hover_zone_corner);
        }
    }

    if (g_waypoint_gizmo_drag.active) {
        update_waypoint_gizmo_drag();
    }
}

void draw_waypoint_editor_gizmo()
{
    if (!waypoint_editor_freelook_aim_mode_active()
        || g_waypoint_editor_selection.kind == WaypointEditorSelectionKind::none
        || waypoint_editor_modal_dialog_open()
        || selection_is_trigger_zone(g_waypoint_editor_selection)) {
        return;
    }

    constexpr float kAxisLength = 1.5f;
    constexpr float kAxisThicknessOffset = 0.04f;
    const WaypointGizmoAxis active_axis = g_waypoint_gizmo_drag.active
        ? g_waypoint_gizmo_drag.axis
        : WaypointGizmoAxis::none;
    const bool box_zone_selection = selection_is_box_extents_zone(g_waypoint_editor_selection);
    const int active_corner = g_waypoint_gizmo_drag.active
        ? g_waypoint_gizmo_drag.zone_box_corner
        : -1;

    rf::Matrix3 camera_orient = rf::identity_matrix;
    if (rf::local_player && rf::local_player->cam) {
        camera_orient = rf::camera_get_orient(rf::local_player->cam);
    }
    const rf::Vector3 side_offset = camera_orient.rvec * kAxisThicknessOffset;
    const rf::Vector3 up_offset = camera_orient.uvec * kAxisThicknessOffset;

    static constexpr std::array<WaypointGizmoAxis, 3> kAxes{
        WaypointGizmoAxis::x,
        WaypointGizmoAxis::y,
        WaypointGizmoAxis::z,
    };

    std::vector<std::pair<rf::Vector3, int>> gizmo_centers{};
    if (box_zone_selection) {
        auto min_corner = get_zone_box_corner_world_pos(g_waypoint_editor_selection, 0);
        auto max_corner = get_zone_box_corner_world_pos(g_waypoint_editor_selection, 1);
        if (!min_corner || !max_corner) {
            return;
        }
        gizmo_centers.emplace_back(min_corner.value(), 0);
        gizmo_centers.emplace_back(max_corner.value(), 1);
    }
    else {
        auto selection_center = get_selection_center(g_waypoint_editor_selection);
        if (!selection_center) {
            return;
        }
        gizmo_centers.emplace_back(selection_center.value(), -1);
    }

    for (const auto& [gizmo_center, corner_index] : gizmo_centers) {
        for (const auto axis : kAxes) {
            rf::Color color =
                axis == WaypointGizmoAxis::x ? rf::Color{255, 90, 90, 255}
                : axis == WaypointGizmoAxis::y ? rf::Color{110, 255, 120, 255}
                : rf::Color{120, 180, 255, 255};

            const bool axis_is_active =
                axis == active_axis
                && (!g_waypoint_gizmo_drag.active || corner_index == active_corner);
            const bool axis_is_hovered =
                !g_waypoint_gizmo_drag.active
                && axis == g_waypoint_gizmo_hover_axis
                && (!box_zone_selection || corner_index == g_waypoint_gizmo_hover_zone_corner);
            if (!axis_is_active && !axis_is_hovered) {
                color.alpha = 180;
            }

            rf::gr::set_color(color);
            const rf::Vector3 axis_end = gizmo_center + gizmo_axis_dir(axis) * kAxisLength;
            rf::gr::line_vec(gizmo_center, axis_end, no_overdraw_2d_line);
            rf::gr::line_vec(gizmo_center + side_offset, axis_end + side_offset, no_overdraw_2d_line);
            rf::gr::line_vec(gizmo_center - side_offset, axis_end - side_offset, no_overdraw_2d_line);
            rf::gr::line_vec(gizmo_center + up_offset, axis_end + up_offset, no_overdraw_2d_line);
            rf::gr::line_vec(gizmo_center - up_offset, axis_end - up_offset, no_overdraw_2d_line);
            rf::gr::sphere(axis_end, 0.085f, no_overdraw_2d_line);

            float sx = 0.0f;
            float sy = 0.0f;
            if (project_world_to_screen(axis_end, sx, sy)) {
                const char axis_label[2]{
                    axis == WaypointGizmoAxis::x ? 'X' : axis == WaypointGizmoAxis::y ? 'Y' : 'Z',
                    '\0'
                };
                draw_centered_debug_label(sx, sy, axis_label, color);
            }
        }
    }
}

void draw_waypoint_editor_log_box(const WaypointEditorRect& rect, const int font_id)
{
    rf::gr::set_color(26, 26, 26, 220);
    rf::gr::rect(rect.x, rect.y, rect.w, rect.h);
    rf::gr::set_color(140, 140, 140, 255);
    rf::gr::rect_border(rect.x, rect.y, rect.w, rect.h);

    rf::gr::set_color(255, 255, 255, 255);
    const std::string log_title =
        std::format("Log ({})", static_cast<int>(g_waypoint_editor_log.size()));
    rf::gr::string(rect.x + 8, rect.y + 6, log_title.c_str(), font_id, no_overdraw_2d_text);

    const int line_h = std::max(1, rf::gr::get_font_height(font_id) + 1);
    const int body_top = rect.y + 26;
    const int body_bottom = rect.y + rect.h - 6;

    if (g_waypoint_editor_log.empty()) {
        rf::gr::set_color(160, 160, 160, 255);
        rf::gr::string(rect.x + 8, body_top, "No log entries yet.", font_id, no_overdraw_2d_text);
    }
    else {
        const int usable_height = std::max(1, body_bottom - body_top);
        const int max_visible_lines = std::max(1, usable_height / line_h);
        const int line_count = static_cast<int>(g_waypoint_editor_log.size());
        const int first_visible_line = std::max(0, line_count - max_visible_lines);
        int line_y = body_top;
        for (int i = first_visible_line; i < line_count; ++i) {
            if (line_y >= body_bottom) {
                break;
            }
            rf::gr::set_color(215, 215, 215, 255);
            rf::gr::string(rect.x + 8, line_y, g_waypoint_editor_log[i].c_str(), font_id, no_overdraw_2d_text);
            line_y += line_h;
        }
    }
}

void draw_waypoint_editor_cursor()
{
    if (!waypoint_editor_runtime_mode_active() || !g_waypoint_editor_mouse_ui_mode) {
        return;
    }

    const int x = std::clamp(g_waypoint_editor_mouse_x, 0, std::max(0, rf::gr::clip_width() - 1));
    const int y = std::clamp(g_waypoint_editor_mouse_y, 0, std::max(0, rf::gr::clip_height() - 1));

    constexpr int kHalfLen = 7;
    rf::gr::set_color(0, 0, 0, 220);
    rf::gr::line(
        static_cast<float>(x - kHalfLen - 1),
        static_cast<float>(y),
        static_cast<float>(x + kHalfLen + 1),
        static_cast<float>(y),
        no_overdraw_2d_line);
    rf::gr::line(
        static_cast<float>(x),
        static_cast<float>(y - kHalfLen - 1),
        static_cast<float>(x),
        static_cast<float>(y + kHalfLen + 1),
        no_overdraw_2d_line);

    rf::gr::set_color(255, 255, 255, 255);
    rf::gr::line(
        static_cast<float>(x - kHalfLen),
        static_cast<float>(y),
        static_cast<float>(x + kHalfLen),
        static_cast<float>(y),
        no_overdraw_2d_line);
    rf::gr::line(
        static_cast<float>(x),
        static_cast<float>(y - kHalfLen),
        static_cast<float>(x),
        static_cast<float>(y + kHalfLen),
        no_overdraw_2d_line);
}

void draw_waypoint_editor_freelook_reticle()
{
    if (!waypoint_editor_runtime_mode_active()) {
        return;
    }

    rf::Player* local_player = rf::local_player;
    if (!local_player || !local_player->cam
        || rf::camera_get_mode(*local_player->cam) != rf::CAMERA_FREELOOK) {
        return;
    }

    const int cx = rf::gr::clip_width() / 2;
    const int cy = rf::gr::clip_height() / 2;
    constexpr int kHalfLen = 9;

    rf::gr::set_color(0, 0, 0, 220);
    rf::gr::line(
        static_cast<float>(cx - kHalfLen - 1),
        static_cast<float>(cy),
        static_cast<float>(cx + kHalfLen + 1),
        static_cast<float>(cy),
        no_overdraw_2d_line);
    rf::gr::line(
        static_cast<float>(cx),
        static_cast<float>(cy - kHalfLen - 1),
        static_cast<float>(cx),
        static_cast<float>(cy + kHalfLen + 1),
        no_overdraw_2d_line);

    rf::gr::set_color(255, 245, 170, 255);
    rf::gr::line(
        static_cast<float>(cx - kHalfLen),
        static_cast<float>(cy),
        static_cast<float>(cx + kHalfLen),
        static_cast<float>(cy),
        no_overdraw_2d_line);
    rf::gr::line(
        static_cast<float>(cx),
        static_cast<float>(cy - kHalfLen),
        static_cast<float>(cx),
        static_cast<float>(cy + kHalfLen),
        no_overdraw_2d_line);
}

void render_waypoint_link_editor_dialog(const int font_id)
{
    if (!g_waypoint_link_editor_dialog.open || !g_waypoint_editor_mouse_ui_mode) {
        return;
    }

    const int font_h = rf::gr::get_font_height(font_id);
    const int row_h = std::max(20, font_h + 8);
    const int window_w = 440;
    const int window_h = 120 + (row_h * kMaxWaypointLinks);
    const int window_x = (rf::gr::clip_width() - window_w) / 2;
    const int window_y = (rf::gr::clip_height() - window_h) / 2;

    rf::gr::set_color(10, 10, 10, 240);
    rf::gr::rect(window_x, window_y, window_w, window_h);
    rf::gr::set_color(180, 180, 180, 255);
    rf::gr::rect_border(window_x, window_y, window_w, window_h);

    const char* title = g_waypoint_link_editor_dialog.edit_inbound_links
        ? "Edit inbound links"
        : "Edit outbound links";
    rf::gr::set_color(255, 255, 255, 255);
    rf::gr::string(window_x + 10, window_y + 8, title, font_id, no_overdraw_2d_text);

    const std::string subtitle = std::format(
        "Waypoint {} ({} fields)",
        g_waypoint_link_editor_dialog.waypoint_uid,
        kMaxWaypointLinks);
    rf::gr::set_color(185, 185, 185, 255);
    rf::gr::string(window_x + 10, window_y + 28, subtitle.c_str(), font_id, no_overdraw_2d_text);

    if (g_waypoint_link_editor_dialog.inbound_list_truncated) {
        rf::gr::set_color(255, 180, 90, 255);
        rf::gr::string(
            window_x + 10,
            window_y + 46,
            "Inbound list was truncated to max link count.",
            font_id,
            no_overdraw_2d_text);
    }

    int field_y = window_y + 64;
    for (int field_index = 0; field_index < kMaxWaypointLinks; ++field_index) {
        const std::string label = std::format("{}:", field_index + 1);
        rf::gr::set_color(230, 230, 230, 255);
        rf::gr::string(window_x + 14, field_y + 2, label.c_str(), font_id, no_overdraw_2d_text);

        WaypointEditorRect field_rect{window_x + 62, field_y, 180, row_h};
        const bool active_field = g_waypoint_link_editor_dialog.active_field == field_index;
        rf::Color fill = active_field ? rf::Color{75, 95, 130, 230} : rf::Color{45, 45, 45, 220};
        rf::gr::set_color(fill);
        rf::gr::rect(field_rect.x, field_rect.y, field_rect.w, field_rect.h);
        rf::gr::set_color(185, 185, 185, 255);
        rf::gr::rect_border(field_rect.x, field_rect.y, field_rect.w, field_rect.h);

        const char* field_text = g_waypoint_link_editor_dialog.fields[field_index].empty()
            ? ""
            : g_waypoint_link_editor_dialog.fields[field_index].c_str();
        rf::gr::set_color(255, 255, 255, 255);
        rf::gr::string(field_rect.x + 6, field_rect.y + 2, field_text, font_id, no_overdraw_2d_text);

        if (consume_left_click_in_rect(field_rect)) {
            g_waypoint_link_editor_dialog.active_field = field_index;
        }

        field_y += row_h;
    }

    const int button_y = window_y + window_h - 40;
    const WaypointEditorRect save_button{window_x + 250, button_y, 82, 28};
    const WaypointEditorRect cancel_button{window_x + 340, button_y, 82, 28};
    if (draw_waypoint_editor_button(save_button, "Save", font_id)) {
        close_waypoint_link_editor_dialog(true);
    }
    if (draw_waypoint_editor_button(cancel_button, "Cancel", font_id)) {
        close_waypoint_link_editor_dialog(false);
    }
}

void render_waypoint_zone_create_dialog(const int font_id)
{
    if (!g_waypoint_zone_create_dialog.open || !g_waypoint_editor_mouse_ui_mode) {
        return;
    }

    const int font_h = rf::gr::get_font_height(font_id);
    const int row_h = std::max(24, font_h + 8);
    const int window_w = 520;
    const int window_h = 320;
    const int window_x = (rf::gr::clip_width() - window_w) / 2;
    const int window_y = (rf::gr::clip_height() - window_h) / 2;

    rf::gr::set_color(10, 10, 10, 240);
    rf::gr::rect(window_x, window_y, window_w, window_h);
    rf::gr::set_color(180, 180, 180, 255);
    rf::gr::rect_border(window_x, window_y, window_w, window_h);

    rf::gr::set_color(255, 255, 255, 255);
    rf::gr::string(window_x + 10, window_y + 8, "Create Zone", font_id, no_overdraw_2d_text);

    static constexpr std::array<WaypointZoneType, 7> kZoneTypes{
        WaypointZoneType::control_point,
        WaypointZoneType::damaging_liquid_room,
        WaypointZoneType::damage_zone,
        WaypointZoneType::instant_death_zone,
        WaypointZoneType::bridge_use,
        WaypointZoneType::bridge_prox,
        WaypointZoneType::high_power_zone,
    };

    int content_y = window_y + 36;
    if (g_waypoint_zone_create_dialog.stage == WaypointZoneCreateDialogStage::select_type) {
        rf::gr::set_color(205, 205, 205, 255);
        rf::gr::string(window_x + 10, content_y, "1) Select zone type", font_id, no_overdraw_2d_text);
        content_y += font_h + 8;

        const int button_gap = 6;
        const int column_w = (window_w - 20 - button_gap) / 2;
        for (int i = 0; i < static_cast<int>(kZoneTypes.size()); ++i) {
            const int row = i / 2;
            const int col = i % 2;
            const int bx = window_x + 10 + col * (column_w + button_gap);
            const int by = content_y + row * (row_h + 5);
            const auto zone_name = waypoint_zone_type_name(kZoneTypes[i]);
            if (draw_waypoint_editor_button(
                    {bx, by, column_w, row_h},
                    std::string{zone_name}.c_str(),
                    font_id)) {
                g_waypoint_zone_create_dialog.selected_type = kZoneTypes[i];
                g_waypoint_zone_create_dialog.stage = WaypointZoneCreateDialogStage::select_source;
            }
        }

        if (draw_waypoint_editor_button(
                {window_x + window_w - 96, window_y + window_h - 38, 82, 28},
                "Cancel",
                font_id)) {
            close_waypoint_zone_create_dialog();
        }
        return;
    }

    const auto selected_zone_name = waypoint_zone_type_name(g_waypoint_zone_create_dialog.selected_type);
    const std::string selected_zone_line = std::format("Selected type: {}", selected_zone_name);
    rf::gr::set_color(205, 205, 205, 255);
    rf::gr::string(
        window_x + 10,
        content_y,
        selected_zone_line.c_str(),
        font_id,
        no_overdraw_2d_text);
    content_y += font_h + 10;

    if (g_waypoint_zone_create_dialog.stage == WaypointZoneCreateDialogStage::select_source) {
        rf::gr::set_color(205, 205, 205, 255);
        rf::gr::string(window_x + 10, content_y, "2) Choose source", font_id, no_overdraw_2d_text);
        content_y += font_h + 8;

        const WaypointEditorRect trigger_button{window_x + 10, content_y, window_w - 20, row_h};
        if (draw_waypoint_editor_button(trigger_button, "Trigger UID", font_id)) {
            g_waypoint_zone_create_dialog.stage = WaypointZoneCreateDialogStage::enter_trigger_uid;
            g_waypoint_zone_create_dialog.trigger_uid_field.clear();
            g_waypoint_editor_pending_numeric_key_counts.fill(0);
        }
        content_y += row_h + 6;

        const bool box_supported = !waypoint_zone_type_is_bridge(g_waypoint_zone_create_dialog.selected_type);
        if (draw_waypoint_editor_button(
                {window_x + 10, content_y, window_w - 20, row_h},
                "Box Extents",
                font_id,
                box_supported)) {
            if (create_new_box_zone_from_view(g_waypoint_zone_create_dialog.selected_type)) {
                close_waypoint_zone_create_dialog();
            }
        }

        const WaypointEditorRect back_button{window_x + window_w - 184, window_y + window_h - 38, 82, 28};
        const WaypointEditorRect cancel_button{window_x + window_w - 96, window_y + window_h - 38, 82, 28};
        if (draw_waypoint_editor_button(back_button, "Back", font_id)) {
            g_waypoint_zone_create_dialog.stage = WaypointZoneCreateDialogStage::select_type;
            g_waypoint_zone_create_dialog.trigger_uid_field.clear();
            g_waypoint_editor_pending_numeric_key_counts.fill(0);
        }
        if (draw_waypoint_editor_button(cancel_button, "Cancel", font_id)) {
            close_waypoint_zone_create_dialog();
        }
        return;
    }

    if (g_waypoint_zone_create_dialog.stage == WaypointZoneCreateDialogStage::enter_trigger_uid) {
        rf::gr::set_color(205, 205, 205, 255);
        rf::gr::string(window_x + 10, content_y, "Trigger UID:", font_id, no_overdraw_2d_text);
        content_y += font_h + 6;

        const WaypointEditorRect field_rect{window_x + 10, content_y, 220, row_h};
        rf::gr::set_color(75, 95, 130, 230);
        rf::gr::rect(field_rect.x, field_rect.y, field_rect.w, field_rect.h);
        rf::gr::set_color(185, 185, 185, 255);
        rf::gr::rect_border(field_rect.x, field_rect.y, field_rect.w, field_rect.h);
        rf::gr::set_color(255, 255, 255, 255);
        rf::gr::string(
            field_rect.x + 6,
            field_rect.y + 2,
            g_waypoint_zone_create_dialog.trigger_uid_field.c_str(),
            font_id,
            no_overdraw_2d_text);

        const WaypointEditorRect create_button{window_x + window_w - 272, window_y + window_h - 38, 82, 28};
        const WaypointEditorRect back_button{window_x + window_w - 184, window_y + window_h - 38, 82, 28};
        const WaypointEditorRect cancel_button{window_x + window_w - 96, window_y + window_h - 38, 82, 28};
        if (draw_waypoint_editor_button(create_button, "Create", font_id)) {
            create_zone_from_trigger_uid_field();
        }
        if (draw_waypoint_editor_button(back_button, "Back", font_id)) {
            g_waypoint_zone_create_dialog.stage = WaypointZoneCreateDialogStage::select_source;
            g_waypoint_editor_pending_numeric_key_counts.fill(0);
        }
        if (draw_waypoint_editor_button(cancel_button, "Cancel", font_id)) {
            close_waypoint_zone_create_dialog();
        }
    }
}

void render_waypoint_target_create_dialog(const int font_id)
{
    if (!g_waypoint_target_create_dialog.open || !g_waypoint_editor_mouse_ui_mode) {
        return;
    }

    const int font_h = rf::gr::get_font_height(font_id);
    const int row_h = std::max(24, font_h + 8);
    const int window_w = 360;
    const int window_h = 210;
    const int window_x = (rf::gr::clip_width() - window_w) / 2;
    const int window_y = (rf::gr::clip_height() - window_h) / 2;

    rf::gr::set_color(10, 10, 10, 240);
    rf::gr::rect(window_x, window_y, window_w, window_h);
    rf::gr::set_color(180, 180, 180, 255);
    rf::gr::rect_border(window_x, window_y, window_w, window_h);

    rf::gr::set_color(255, 255, 255, 255);
    rf::gr::string(window_x + 10, window_y + 8, "Create Target", font_id, no_overdraw_2d_text);
    rf::gr::set_color(205, 205, 205, 255);
    rf::gr::string(window_x + 10, window_y + 8 + font_h + 4, "Select target type", font_id, no_overdraw_2d_text);

    const WaypointEditorRect explosion_button{window_x + 10, window_y + 52, window_w - 20, row_h};
    const WaypointEditorRect shatter_button{window_x + 10, window_y + 52 + row_h + 6, window_w - 20, row_h};
    const WaypointEditorRect jump_button{window_x + 10, window_y + 52 + (row_h + 6) * 2, window_w - 20, row_h};
    if (draw_waypoint_editor_button(explosion_button, "explosion", font_id)) {
        if (create_new_target_from_view(WaypointTargetType::explosion)) {
            close_waypoint_target_create_dialog();
        }
    }
    if (draw_waypoint_editor_button(shatter_button, "shatter", font_id)) {
        if (create_new_target_from_view(WaypointTargetType::shatter)) {
            close_waypoint_target_create_dialog();
        }
    }
    if (draw_waypoint_editor_button(jump_button, "jump", font_id)) {
        if (create_new_target_from_view(WaypointTargetType::jump)) {
            close_waypoint_target_create_dialog();
        }
    }

    if (draw_waypoint_editor_button(
            {window_x + window_w - 96, window_y + window_h - 38, 82, 28},
            "Cancel",
            font_id)) {
        close_waypoint_target_create_dialog();
    }
}

void render_waypoint_type_change_dialog(const int font_id)
{
    if (!g_waypoint_type_change_dialog.open || !g_waypoint_editor_mouse_ui_mode) {
        return;
    }

    const int font_h = rf::gr::get_font_height(font_id);
    const int row_h = std::max(24, font_h + 8);
    const int window_w = 620;
    const int window_h = 360;
    const int window_x = (rf::gr::clip_width() - window_w) / 2;
    const int window_y = (rf::gr::clip_height() - window_h) / 2;

    rf::gr::set_color(10, 10, 10, 240);
    rf::gr::rect(window_x, window_y, window_w, window_h);
    rf::gr::set_color(180, 180, 180, 255);
    rf::gr::rect_border(window_x, window_y, window_w, window_h);

    const char* subject_label = "Selection";
    switch (g_waypoint_type_change_dialog.subject) {
        case WaypointTypeChangeDialogSubject::waypoint:
            subject_label = "Waypoint";
            break;
        case WaypointTypeChangeDialogSubject::zone:
            subject_label = "Zone";
            break;
        case WaypointTypeChangeDialogSubject::target:
            subject_label = "Target";
            break;
        case WaypointTypeChangeDialogSubject::none:
        default:
            break;
    }

    rf::gr::set_color(255, 255, 255, 255);
    rf::gr::string(
        window_x + 10,
        window_y + 8,
        std::format("Change {} Type", subject_label).c_str(),
        font_id,
        no_overdraw_2d_text);

    int button_y = window_y + 34;
    const int button_gap = 6;
    const int content_w = window_w - 20;

    if (g_waypoint_type_change_dialog.subject == WaypointTypeChangeDialogSubject::waypoint) {
        static constexpr std::array<WaypointType, 13> kWaypointTypes{
            WaypointType::std,
            WaypointType::std_new,
            WaypointType::item,
            WaypointType::respawn,
            WaypointType::jump_pad,
            WaypointType::lift_body,
            WaypointType::lift_entrance,
            WaypointType::lift_exit,
            WaypointType::ladder,
            WaypointType::ctf_flag,
            WaypointType::crater,
            WaypointType::tele_entrance,
            WaypointType::tele_exit,
        };
        const int columns = 2;
        const int cell_w = (content_w - button_gap) / columns;
        for (int i = 0; i < static_cast<int>(kWaypointTypes.size()); ++i) {
            const int row = i / columns;
            const int col = i % columns;
            const int bx = window_x + 10 + col * (cell_w + button_gap);
            const int by = button_y + row * (row_h + 5);
            const auto type_name = waypoint_type_name(kWaypointTypes[i]);
            if (draw_waypoint_editor_button(
                    {bx, by, cell_w, row_h},
                    std::string{type_name}.c_str(),
                    font_id)) {
                if (apply_waypoint_type_change(kWaypointTypes[i])) {
                    close_waypoint_type_change_dialog();
                }
            }
        }
    }
    else if (g_waypoint_type_change_dialog.subject == WaypointTypeChangeDialogSubject::zone) {
        static constexpr std::array<WaypointZoneType, 7> kZoneTypes{
            WaypointZoneType::control_point,
            WaypointZoneType::damaging_liquid_room,
            WaypointZoneType::damage_zone,
            WaypointZoneType::instant_death_zone,
            WaypointZoneType::bridge_use,
            WaypointZoneType::bridge_prox,
            WaypointZoneType::high_power_zone,
        };
        const bool source_is_trigger =
            g_waypoint_type_change_dialog.uid >= 0
            && g_waypoint_type_change_dialog.uid < static_cast<int>(g_waypoint_zones.size())
            && resolve_waypoint_zone_source(g_waypoint_zones[g_waypoint_type_change_dialog.uid])
                == WaypointZoneSource::trigger_uid;

        for (int i = 0; i < static_cast<int>(kZoneTypes.size()); ++i) {
            const int by = button_y + i * (row_h + 5);
            const auto type_name = waypoint_zone_type_name(kZoneTypes[i]);
            const bool enabled = !waypoint_zone_type_is_bridge(kZoneTypes[i]) || source_is_trigger;
            if (draw_waypoint_editor_button(
                    {window_x + 10, by, content_w, row_h},
                    std::string{type_name}.c_str(),
                    font_id,
                    enabled)) {
                if (apply_zone_type_change(kZoneTypes[i])) {
                    close_waypoint_type_change_dialog();
                }
            }
        }
    }
    else if (g_waypoint_type_change_dialog.subject == WaypointTypeChangeDialogSubject::target) {
        static constexpr std::array<WaypointTargetType, 3> kTargetTypes{
            WaypointTargetType::explosion,
            WaypointTargetType::shatter,
            WaypointTargetType::jump,
        };
        for (int i = 0; i < static_cast<int>(kTargetTypes.size()); ++i) {
            const int by = button_y + i * (row_h + 5);
            const auto type_name = waypoint_target_type_name(kTargetTypes[i]);
            if (draw_waypoint_editor_button(
                    {window_x + 10, by, content_w, row_h},
                    std::string{type_name}.c_str(),
                    font_id)) {
                if (apply_target_type_change(kTargetTypes[i])) {
                    close_waypoint_type_change_dialog();
                }
            }
        }
    }

    if (draw_waypoint_editor_button(
            {window_x + window_w - 96, window_y + window_h - 38, 82, 28},
            "Cancel",
            font_id)) {
        close_waypoint_type_change_dialog();
    }
}

void render_waypoint_editor_overlay_panel()
{
    if (!waypoint_editor_runtime_mode_active()) {
        return;
    }

    const int font_id = -1;
    const int font_h = rf::gr::get_font_height(font_id);

    const int panel_margin = 16;
    const int panel_w = 380;
    const int panel_h = std::max(260, rf::gr::clip_height() - panel_margin * 2);
    const int panel_x = rf::gr::clip_width() - panel_w - panel_margin;
    const int panel_y = panel_margin;

    rf::gr::set_color(12, 12, 12, 220);
    rf::gr::rect(panel_x, panel_y, panel_w, panel_h);
    rf::gr::set_color(170, 170, 170, 255);
    rf::gr::rect_border(panel_x, panel_y, panel_w, panel_h);

    rf::gr::set_color(255, 255, 255, 255);
    rf::gr::string(panel_x + 10, panel_y + 8, "Waypoint Editor", font_id, no_overdraw_2d_text);

    const std::string mode_line = std::format(
        "Right click: {}",
        g_waypoint_editor_mouse_ui_mode ? "UI cursor" : "Mouse aim");
    rf::gr::set_color(200, 200, 200, 255);
    rf::gr::string(panel_x + 10, panel_y + 8 + font_h + 2, mode_line.c_str(), font_id, no_overdraw_2d_text);

    int button_y = panel_y + 8 + (font_h * 2) + 14;
    const int button_h = std::max(26, font_h + 8);
    const int button_w = panel_w - 20;
    const bool main_controls_enabled = !waypoint_editor_modal_dialog_open();

    if (draw_waypoint_editor_button(
            {panel_x + 10, button_y, button_w, button_h},
            "Reset to default grid",
            font_id,
            main_controls_enabled)) {
        execute_waypoint_editor_console_command("waypoints_reset", "Reset waypoints to default grid");
    }
    button_y += button_h + 6;

    if (draw_waypoint_editor_button(
            {panel_x + 10, button_y, button_w, button_h},
            "Generate waypoints (INTENSE)",
            font_id,
            main_controls_enabled)) {
        execute_waypoint_editor_console_command("waypoints_generate", "Triggered waypoint generation");
    }
    button_y += button_h + 6;

    if (draw_waypoint_editor_button(
            {panel_x + 10, button_y, button_w, button_h},
            "Save AWP file",
            font_id,
            main_controls_enabled)) {
        execute_waypoint_editor_console_command("waypoints_save", "Saved AWP file");
    }
    button_y += button_h + 6;

    if (draw_waypoint_editor_button(
            {panel_x + 10, button_y, button_w, button_h},
            "Load AWP file",
            font_id,
            main_controls_enabled)) {
        execute_waypoint_editor_console_command("waypoints_load", "Loaded AWP file");
    }
    button_y += button_h + 6;

    const int tri_gap = 6;
    const int tri_w = (button_w - tri_gap * 2) / 3;
    const int tri_x0 = panel_x + 10;
    const int tri_x1 = tri_x0 + tri_w + tri_gap;
    const int tri_x2 = tri_x1 + tri_w + tri_gap;
    const int debug_mode = std::clamp(g_debug_waypoints_mode, 0, 3);
    const std::string debug_button_label =
        debug_mode > 0 ? std::format("Debug ({})", debug_mode) : "Debug";

    if (draw_waypoint_editor_button(
            {tri_x0, button_y, tri_w, button_h},
            debug_button_label.c_str(),
            font_id,
            main_controls_enabled,
            debug_mode > 0)) {
        execute_waypoint_editor_console_command("waypoints_debug", "Cycled waypoint debug mode");
    }
    if (draw_waypoint_editor_button(
            {tri_x1, button_y, tri_w, button_h},
            "Drop",
            font_id,
            main_controls_enabled,
            g_drop_waypoints)) {
        execute_waypoint_editor_console_command("waypoints_drop", "Toggled waypoint auto-drop");
    }
    if (draw_waypoint_editor_button(
            {tri_x2, button_y, tri_w, button_h},
            "Clean",
            font_id,
            main_controls_enabled)) {
        execute_waypoint_editor_console_command("waypoints_clean", "Cleaned waypoints");
    }
    button_y += button_h + 6;

    if (draw_waypoint_editor_button(
            {tri_x0, button_y, tri_w, button_h},
            "New WP",
            font_id,
            main_controls_enabled)) {
        create_new_waypoint_std_from_view();
    }
    if (draw_waypoint_editor_button(
            {tri_x1, button_y, tri_w, button_h},
            "New Zone",
            font_id,
            main_controls_enabled)) {
        open_waypoint_zone_create_dialog();
    }
    if (draw_waypoint_editor_button(
            {tri_x2, button_y, tri_w, button_h},
            "New Target",
            font_id,
            main_controls_enabled)) {
        open_waypoint_target_create_dialog();
    }
    button_y += button_h + 6;

    const int section_y = button_y + 6;
    const int section_h = 190;
    const WaypointEditorRect selection_section{panel_x + 10, section_y, panel_w - 20, section_h};

    rf::gr::set_color(22, 22, 22, 220);
    rf::gr::rect(selection_section.x, selection_section.y, selection_section.w, selection_section.h);
    rf::gr::set_color(140, 140, 140, 255);
    rf::gr::rect_border(selection_section.x, selection_section.y, selection_section.w, selection_section.h);

    rf::gr::set_color(240, 240, 240, 255);
    rf::gr::string(selection_section.x + 8, selection_section.y + 6, "Selection", font_id, no_overdraw_2d_text);

    if (g_waypoint_editor_selection.kind != WaypointEditorSelectionKind::none) {
        const std::string selected_name = selection_debug_name(g_waypoint_editor_selection);
        rf::gr::set_color(175, 210, 255, 255);
        rf::gr::string(
            selection_section.x + 8,
            selection_section.y + 6 + font_h + 2,
            selected_name.c_str(),
            font_id,
            no_overdraw_2d_text);

        if (g_waypoint_editor_selection.kind == WaypointEditorSelectionKind::waypoint) {
            int action_y = selection_section.y + 8 + (font_h * 2) + 8;
            const int action_h = std::max(24, font_h + 6);
            const int action_w = selection_section.w - 16;
            const int action_gap = 6;
            const int half_w = (action_w - action_gap) / 2;
            const int action_x = selection_section.x + 8;
            const bool waypoint_actions_enabled = main_controls_enabled;

            if (draw_waypoint_editor_button(
                    {action_x, action_y, half_w, action_h},
                    "Edit inbound links",
                    font_id,
                    waypoint_actions_enabled)) {
                open_waypoint_link_editor_dialog(true);
            }
            if (draw_waypoint_editor_button(
                    {action_x + half_w + action_gap, action_y, half_w, action_h},
                    "Edit outbound links",
                    font_id,
                    waypoint_actions_enabled)) {
                open_waypoint_link_editor_dialog(false);
            }
            action_y += action_h + 4;

            if (draw_waypoint_editor_button(
                    {action_x, action_y, half_w, action_h},
                    "Change type",
                    font_id,
                    waypoint_actions_enabled)) {
                open_waypoint_type_change_dialog_for_selection();
            }
            if (draw_waypoint_editor_button(
                    {action_x + half_w + action_gap, action_y, half_w, action_h},
                    "Cycle movement type",
                    font_id,
                    waypoint_actions_enabled)) {
                cycle_selected_waypoint_movement_subtype(1);
            }
            action_y += action_h + 4;

            if (draw_waypoint_editor_button(
                    {action_x, action_y, half_w, action_h},
                    "Auto link",
                    font_id,
                    waypoint_actions_enabled)) {
                WaypointAutoLinkStats auto_link_stats{};
                if (waypoints_auto_link_nearby(g_waypoint_editor_selection.uid, auto_link_stats)) {
                    rf::console::print(
                        "Waypoint {} auto-link: {} nearby, {} links added",
                        g_waypoint_editor_selection.uid,
                        auto_link_stats.candidate_waypoints,
                        auto_link_stats.links_added);
                    push_waypoint_editor_log(std::format(
                        "Waypoint {} auto-link: {} nearby, +{} links",
                        g_waypoint_editor_selection.uid,
                        auto_link_stats.candidate_waypoints,
                        auto_link_stats.links_added));
                }
            }
            if (draw_waypoint_editor_button(
                    {action_x + half_w + action_gap, action_y, half_w, action_h},
                    "Delete waypoint",
                    font_id,
                    waypoint_actions_enabled)) {
                delete_selected_waypoint_editor_object();
            }
        }
        else if (g_waypoint_editor_selection.kind == WaypointEditorSelectionKind::zone) {
            int action_y = selection_section.y + 8 + (font_h * 2) + 8;
            const int action_h = std::max(24, font_h + 6);
            const int action_w = selection_section.w - 16;
            const int action_gap = 6;
            const int half_w = (action_w - action_gap) / 2;
            const int action_x = selection_section.x + 8;
            const bool zone_actions_enabled = main_controls_enabled;

            if (draw_waypoint_editor_button(
                    {action_x, action_y, half_w, action_h},
                    "Change type",
                    font_id,
                    zone_actions_enabled)) {
                open_waypoint_type_change_dialog_for_selection();
            }
            if (draw_waypoint_editor_button(
                    {action_x + half_w + action_gap, action_y, half_w, action_h},
                    "Delete zone",
                    font_id,
                    zone_actions_enabled)) {
                delete_selected_waypoint_editor_object();
            }
        }
        else if (g_waypoint_editor_selection.kind == WaypointEditorSelectionKind::target) {
            int action_y = selection_section.y + 8 + (font_h * 2) + 8;
            const int action_h = std::max(24, font_h + 6);
            const int action_w = selection_section.w - 16;
            const int action_gap = 6;
            const int half_w = (action_w - action_gap) / 2;
            const int action_x = selection_section.x + 8;
            const bool target_actions_enabled = main_controls_enabled;

            if (draw_waypoint_editor_button(
                    {action_x, action_y, half_w, action_h},
                    "Change type",
                    font_id,
                    target_actions_enabled)) {
                open_waypoint_type_change_dialog_for_selection();
            }
            if (draw_waypoint_editor_button(
                    {action_x + half_w + action_gap, action_y, half_w, action_h},
                    "Delete target",
                    font_id,
                    target_actions_enabled)) {
                delete_selected_waypoint_editor_object();
            }
        }
    }

    const int log_y = selection_section.y + selection_section.h + 10;
    const int log_h = std::max(48, panel_y + panel_h - log_y - 10);
    draw_waypoint_editor_log_box({panel_x + 10, log_y, panel_w - 20, log_h}, font_id);

    render_waypoint_link_editor_dialog(font_id);
    render_waypoint_zone_create_dialog(font_id);
    render_waypoint_target_create_dialog(font_id);
    render_waypoint_type_change_dialog(font_id);
    draw_waypoint_editor_freelook_reticle();
    draw_waypoint_editor_cursor();
}

void reset_waypoint_editor_runtime_state(const bool clear_log)
{
    clear_waypoint_editor_selection();
    g_waypoint_editor_mouse_ui_mode = false;
    apply_waypoint_editor_mouse_look_override(false);
    g_waypoint_link_editor_dialog = {};
    g_waypoint_zone_create_dialog = {};
    g_waypoint_target_create_dialog = {};
    g_waypoint_type_change_dialog = {};
    g_waypoint_editor_pending_numeric_key_counts.fill(0);
    g_waypoint_gizmo_hover_axis = WaypointGizmoAxis::none;
    g_waypoint_gizmo_hover_zone_corner = -1;
    g_waypoint_gizmo_drag = {};
    g_waypoint_editor_view_lock = {};
    if (clear_log) {
        g_waypoint_editor_log.clear();
    }
}
} // namespace

void waypoints_utils_level_init()
{
    ensure_waypoint_editor_input_hooks_installed();
    reset_waypoint_editor_runtime_state(true);
    push_waypoint_editor_log("Waypoint editor ready");
}

void waypoints_utils_level_reset()
{
    reset_waypoint_editor_runtime_state(true);
}

void waypoints_utils_on_level_unloaded()
{
    reset_waypoint_editor_runtime_state(false);
}

void waypoints_utils_do_frame()
{
    capture_waypoint_editor_mouse_input();

    const bool runtime_mode_active = waypoint_editor_runtime_mode_active();
    if (!runtime_mode_active) {
        if (g_waypoint_gizmo_drag.active) {
            end_waypoint_gizmo_drag(false);
        }
        apply_waypoint_editor_view_lock(true);
        reset_waypoint_editor_runtime_state(false);
        apply_waypoint_editor_mouse_mode(false);
        return;
    }

    if (g_waypoint_editor_right_click_pressed) {
        g_waypoint_editor_mouse_ui_mode = !g_waypoint_editor_mouse_ui_mode;
        if (g_waypoint_editor_mouse_ui_mode) {
            push_waypoint_editor_log("Entered cursor edit mode");
            capture_waypoint_editor_view_lock();
            apply_waypoint_editor_view_lock(false);
            if (g_waypoint_gizmo_drag.active) {
                end_waypoint_gizmo_drag(true);
            }
        }
        else {
            push_waypoint_editor_log("Returned to mouse aim mode");
            apply_waypoint_editor_view_lock(true);
            g_waypoint_link_editor_dialog = {};
            g_waypoint_zone_create_dialog = {};
            g_waypoint_target_create_dialog = {};
            g_waypoint_type_change_dialog = {};
            g_waypoint_editor_pending_numeric_key_counts.fill(0);
        }
    }

    apply_waypoint_editor_mouse_mode(true);

    if (g_waypoint_editor_mouse_ui_mode) {
        apply_waypoint_editor_view_lock(false);
    }

    if (g_waypoint_gizmo_drag.active) {
        g_waypoint_editor_selection = g_waypoint_gizmo_drag.selection;
    }
    else {
        update_waypoint_editor_selection();
    }

    if (g_waypoint_link_editor_dialog.open && g_waypoint_editor_mouse_ui_mode) {
        process_link_editor_keyboard_input();
    }
    else if (g_waypoint_zone_create_dialog.open
             && g_waypoint_zone_create_dialog.stage == WaypointZoneCreateDialogStage::enter_trigger_uid
             && g_waypoint_editor_mouse_ui_mode) {
        process_zone_create_trigger_uid_keyboard_input();
    }
    else {
        handle_waypoint_editor_input();
    }

    update_waypoint_gizmo_interaction();
}

void waypoints_utils_render_overlay()
{
    render_waypoint_editor_overlay_panel();
}

void waypoints_utils_render_debug()
{
    draw_debug_waypoints();
    draw_waypoint_editor_gizmo();
}

void waypoints_utils_log(std::string_view message)
{
    if (message.empty()) {
        return;
    }
    push_waypoint_editor_log(std::string{message});
}

bool waypoints_utils_link_editor_text_input_active()
{
    return waypoint_editor_runtime_mode_active()
        && g_waypoint_editor_mouse_ui_mode
        && (g_waypoint_link_editor_dialog.open
            || (g_waypoint_zone_create_dialog.open
                && g_waypoint_zone_create_dialog.stage == WaypointZoneCreateDialogStage::enter_trigger_uid));
}

void waypoints_utils_capture_numeric_key(const int key_code, const int count)
{
    if (count <= 0 || !waypoints_utils_link_editor_text_input_active()) {
        return;
    }

    const int index = numeric_key_index_from_key_code(key_code);
    if (index < 0 || index >= static_cast<int>(g_waypoint_editor_pending_numeric_key_counts.size())) {
        return;
    }

    g_waypoint_editor_pending_numeric_key_counts[index] += count;
}

int waypoints_utils_consume_numeric_key(const int key_code)
{
    const int index = numeric_key_index_from_key_code(key_code);
    if (index < 0 || index >= static_cast<int>(g_waypoint_editor_pending_numeric_key_counts.size())) {
        return 0;
    }
    const int count = g_waypoint_editor_pending_numeric_key_counts[index];
    g_waypoint_editor_pending_numeric_key_counts[index] = 0;
    return count;
}

bool waypoints_utils_should_block_mouse_look()
{
    const bool should_block = waypoint_editor_runtime_mode_active() && g_waypoint_editor_mouse_ui_mode;
    if (should_block) {
        apply_waypoint_editor_view_lock(false);
    }
    return should_block;
}
