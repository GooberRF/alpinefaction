#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include <vector>
#include <common/utils/list-utils.h>
#include <common/utils/string-utils.h>
#include "haste_trails.h"
#include "multi.h"
#include "powerups.h"
#include "demo/demo.h"
#include "../graphics/ribbon_trail.h"
#include "../hud/multi_spectate.h"
#include "../misc/alpine_settings.h"
#include "../rf/character.h"
#include "../rf/entity.h"
#include "../rf/gameseq.h"
#include "../rf/multi.h"
#include "../rf/object.h"
#include "../rf/player/player.h"
#include "../rf/vmesh.h"
#include "../rf/gr/gr.h"

namespace
{

constexpr RibbonTrailStyle trail_style{
    .lifetime = 0.4f,
    .alpha = 0.5f,
    .band = 0.5f,
    .min_half_width = 0.02f,
    .min_half_px = 1.0f,
};
constexpr int trail_max_samples = 32;
constexpr float trail_sample_spacing = 0.15f;
constexpr float trail_teleport_dist = 3.0f;
constexpr float trail_ankle_height = 0.15f;
constexpr float trail_side_offset = 0.18f;
// Of the unhasted multiplayer run speed.
constexpr float trail_min_speed_factor = 0.8f;

struct HasteTrail
{
    int entity_handle = -1;
    RibbonTrail<2, trail_max_samples> ribbon;
    // Where the speed is measured from, and the time since.
    rf::Vector3 ref_pos{};
    float ref_time = 0.0f;
    bool has_ref = false;
    // The foot bones of `character`, -1 where it has none.
    const rf::Character* character = nullptr;
    std::array<int, 2> foot_bones{-1, -1};
    bool seen = false;

    void reset()
    {
        ribbon.reset();
        has_ref = false;
    }
};

std::vector<HasteTrail> g_trails;

bool trail_should_sample(rf::Entity* ep)
{
    return ep->info && !rf::entity_is_dying(ep) && !(ep->obj_flags & rf::OF_HIDDEN) && !rf::entity_in_vehicle(ep)
        && powerup_is_active(ep, AlpinePowerup::haste);
}

// Ankle height over the entity's lowest point. Interpolation moves remote players without updating
// p_data.bbox_min, so it comes from the object-space collision spheres, whose centres crouching swaps for the
// crouch set.
rf::Vector3 trail_anchor(const rf::Entity* ep)
{
    float lowest = -ep->radius;
    for (int i = 0; i < ep->p_data.cspheres.size(); ++i) {
        const rf::PCollisionSphere& s = ep->p_data.cspheres[i];
        const float y = ep->orient.transform_vector(s.center).y - s.radius;
        lowest = i == 0 ? y : std::min(lowest, y);
    }
    return {ep->pos.x, ep->pos.y + lowest + trail_ankle_height, ep->pos.z};
}

int find_bone(const rf::Character& ch, std::string_view suffix)
{
    const int num_bones = std::min(ch.num_bones, static_cast<int>(std::size(ch.bones)));
    for (int i = 0; i < num_bones; ++i) {
        const rf::Bone& bone = ch.bones[i];
        if (string_iends_with(std::string_view{bone.name, strnlen(bone.name, sizeof(bone.name))}, suffix)) {
            return i;
        }
    }
    return -1;
}

// Matched by suffix: every stock humanoid skeleton names its ankles "<prefix>foot-l" and "<prefix>foot-r".
std::array<rf::Vector3, 2> trail_points(HasteTrail& trail, rf::Entity* ep)
{
    const auto* ci = ep->vmesh && ep->vmesh->type == rf::MESH_TYPE_CHARACTER
        ? static_cast<const rf::CharacterInstance*>(ep->vmesh->instance)
        : nullptr;
    const rf::Character* ch = ci ? ci->base_character : nullptr;
    if (ch != trail.character) {
        trail.character = ch;
        trail.foot_bones = ch ? std::array{find_bone(*ch, "foot-l"), find_bone(*ch, "foot-r")} : std::array{-1, -1};
    }
    std::array<rf::Vector3, 2> points;
    if (trail.foot_bones[0] >= 0 && trail.foot_bones[1] >= 0) {
        for (size_t i = 0; i < points.size(); ++i) {
            rf::Matrix3 orient;
            rf::vmesh_get_prop_point_transform(ep->vmesh, trail.foot_bones[i], &ep->orient, &ep->pos, &orient,
                                               &points[i]);
        }
        return points;
    }
    const rf::Vector3 anchor = trail_anchor(ep);
    rf::Vector3 side{ep->orient.rvec.x, 0.0f, ep->orient.rvec.z};
    side.normalize_safe();
    return {anchor - side * trail_side_offset, anchor + side * trail_side_offset};
}

// A sample every trail_sample_spacing of travel: any while airborne, otherwise horizontal travel at
// trail_min_speed_factor of the unhasted run speed or faster, so starting off, stopping or pushing into a wall shows
// none.
void trail_update(HasteTrail& trail, rf::Entity* ep, float dt)
{
    if (rf::entity_is_dying(ep)) {
        trail.reset();
        return;
    }
    if (!trail_should_sample(ep)) {
        trail.ribbon.live = false;
        trail.has_ref = false;
        return;
    }
    if (!trail.has_ref || (ep->pos - trail.ref_pos).len() > trail_teleport_dist) {
        if (trail.has_ref) {
            trail.reset();
        }
        trail.ref_pos = ep->pos;
        trail.ref_time = 0.0f;
        trail.has_ref = true;
        return;
    }
    trail.ref_time += dt;
    const bool airborne = rf::entity_is_falling(ep);
    rf::Vector3 move = ep->pos - trail.ref_pos;
    if (!airborne) {
        move.y = 0.0f;
    }
    const float dist = move.len();
    // entity_set_speed (0x00427450) gives every multiplayer class this standing run speed, whatever its info->max_vel.
    const float min_speed = airborne ? 0.0f : rf::multiplayer_walk_speed * trail_min_speed_factor;
    if (dist < trail_sample_spacing) {
        if (trail.ref_time * min_speed > trail_sample_spacing) {
            trail.ribbon.live = false;
            trail.ref_pos = ep->pos;
            trail.ref_time = 0.0f;
        }
        return;
    }
    if (dist >= min_speed * trail.ref_time) {
        trail.ribbon.push(trail_points(trail, ep));
    }
    else {
        trail.ribbon.live = false;
    }
    trail.ref_pos = ep->pos;
    trail.ref_time = 0.0f;
}

} // namespace

void haste_trails_clear()
{
    g_trails.clear();
}

void haste_trails_do_frame()
{
    if (g_trails.empty() && !powerup_any_active(AlpinePowerup::haste)) {
        return;
    }
    if (!rf::is_multi || rf::is_dedicated_server || is_headless_mode() || !g_alpine_game_config.rendering_enabled) {
        haste_trails_clear();
        return;
    }
    // Runs from rf_do_frame, outside the sim frame the demo pause freezes. A seek clears the trails as it lands.
    if (rf::game_paused || demo_playback_is_seeking()) {
        return;
    }
    const float dt = cosmetic_frame_dt();
    if (!(dt > 0.0f)) {
        return;
    }

    for (HasteTrail& trail : g_trails) {
        trail.seen = false;
        trail.ribbon.age(dt, trail_style.lifetime);
    }
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        rf::Entity* ep = rf::entity_from_handle(player.entity_handle);
        if (!ep) {
            continue;
        }
        auto it = std::ranges::find(g_trails, ep->handle, &HasteTrail::entity_handle);
        if (it == g_trails.end()) {
            if (!trail_should_sample(ep)) {
                continue;
            }
            it = g_trails.insert(g_trails.end(), HasteTrail{.entity_handle = ep->handle});
        }
        it->seen = true;
        trail_update(*it, ep, dt);
    }
    std::erase_if(g_trails, [](const HasteTrail& trail) {
        return !trail.seen || (trail.ribbon.count == 0 && !trail.has_ref);
    });
}

void haste_trails_render()
{
    if (g_trails.empty() || !rf::is_multi) {
        return;
    }
    const rf::gr::Color& color = powerup_color(AlpinePowerup::haste);
    bool texture_cleared = false;
    for (HasteTrail& trail : g_trails) {
        rf::Entity* ep = rf::entity_from_handle(trail.entity_handle);
        if (!ep || trail.ribbon.count == 0 || (ep->obj_flags & rf::OF_HIDDEN)
            || multi_spectate_view_is_first_person_of(ep)) {
            continue;
        }
        float fade = 0.0f;
        float half_width = 0.0f;
        if (!ribbon_trail_view(ep->pos, trail_style, fade, half_width)) {
            continue;
        }
        if (!texture_cleared) {
            rf::gr::set_texture(-1, -1);
            texture_cleared = true;
        }
        const bool sampling = trail_should_sample(ep);
        std::array<rf::Vector3, 2> head{};
        if (sampling) {
            head = trail_points(trail, ep);
        }
        ribbon_trail_render(trail.ribbon, sampling ? &head : nullptr, 1.0f, trail_style, fade, half_width, color);
    }
}
