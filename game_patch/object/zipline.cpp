#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <common/utils/list-utils.h>
#include <xlog/xlog.h>
#include "zipline.h"
#include "alpine_rope.h"
#include "../hud/multi_spectate.h"
#include "../input/input.h"
#include "../misc/player.h"
#include "../multi/alpine_packets.h"
#include "../multi/gametype.h"
#include "../multi/multi.h"
#include "../multi/bots/server_bots.h"
#include "../multi/vehicles/vehicle.h"
#include "../rf/clutter.h"
#include "../rf/collide.h"
#include "../rf/entity.h"
#include "../rf/gameseq.h"
#include "../rf/multi.h"
#include "../rf/os/console.h"
#include "../rf/os/frametime.h"
#include "../rf/os/timestamp.h"
#include "../rf/physics.h"
#include "../rf/player/control_config.h"
#include "../rf/player/player.h"
#include "../rf/sound/sound.h"
#include "../rf/trigger.h"
#include "../rf/v3d.h"
#include "../rf/vmesh.h"
#include "object.h"

namespace
{

constexpr float zip_use_reach = 2.5f;
// Use takes a line only while the player faces it, so a button or door near an anchor stays usable.
constexpr float zip_use_view_cos = 0.25f;
// The hint re-asks the stock Use target at this rate, as the vehicles prompt does: the sweep traces.
constexpr int zip_use_probe_interval_ms = 100;
constexpr float zip_use_probe_move_sq = 0.25f;
// Where the hands close on the line, measured up from the eye.
constexpr float zip_grip_above_eye = 0.35f;
constexpr float zip_default_eye_height = 0.7f;
// The line has to be at least this high relative to the eye to be taken in the hands.
constexpr float zip_min_line_above_eye = 0.1f;
// Riders are let go this far short of an anchor, so they never slam into the pole or wall.
constexpr float zip_end_margin = 1.0f;
// A grab near an anchor closes on the first rideable point, this far inside the margin.
constexpr float zip_grab_inset = 0.05f;
// The end of a line is a dismount, not a launch: the anchor's pole or wall is right ahead.
constexpr float zip_end_dismount_speed = 10.0f;
constexpr float zip_end_pop = 1.5f;
constexpr float zip_min_speed_frac = 0.5f;
constexpr float zip_max_speed_frac = 2.0f;
// Rates (1/s) the speed relaxes toward cruise: quicker when catching up, so a grab ramps up fast,
// slower when above it, so a steep descent is actually felt.
constexpr float zip_pull_below = 3.0f;
constexpr float zip_pull_above = 1.5f;
// Cap on the velocity added on top of the ride velocity to pull the rider onto the path.
constexpr float zip_max_correction = 20.0f;
// The object pass kills anything moving faster than 100 m/s; a ride keeps well clear of it.
constexpr float zip_max_ride_speed = 70.0f;
constexpr float zip_max_total_speed = 90.0f;
constexpr int zip_attach_grace_ms = 250;
constexpr float zip_blocked_deviation = 0.5f;
constexpr float zip_teleport_deviation = 3.0f;
constexpr int zip_regrab_cooldown_ms = 500;
constexpr float zip_tangent_step = 0.05f;
// Covers the flight right after letting go, which can carry the full ride speed.
constexpr int zip_release_slam_immunity_ms = 500;
constexpr int zip_sound_retry_ms = 500;
// Bots take a line from a spot under its first few metres, and only where the whole ride is clear.
constexpr float zip_bot_grab_span = 4.0f;
constexpr float zip_bot_grab_step = 0.5f;
constexpr float zip_bot_min_ride = 2.0f;
constexpr float zip_bot_clear_step = 1.0f;
constexpr float zip_bot_clear_feet = 0.6f;
constexpr float zip_bot_landing_step = 0.05f;
constexpr int zip_bot_landing_steps = 40;
// Anchors closer in height than this make a level line.
constexpr float zip_level_epsilon = 0.01f;
// Spots are only offered where a grab has room to spare, so a bot that stops a little off one still reaches.
constexpr float zip_bot_reach_share = 0.8f;

constexpr int zip_server_sync_interval_ms = 100;

constexpr const char* zip_mesh_filename = "af_zipline1.v3m";
constexpr const char* zip_sound_filename = "af_zipline_loop1.wav";

const rf::Vector3 zip_up{0.0f, 1.0f, 0.0f};

rf::Vector3 zip_point(const AlpineZipline& zip, float s)
{
    const rope_curve::Vec3 p = rope_curve::sample_at_arclength(zip.curve, s);
    return rf::Vector3{p.x, p.y, p.z};
}

// Unit tangent toward the target end.
rf::Vector3 zip_tangent(const AlpineZipline& zip, float s)
{
    const float len = zip.curve.length;
    const float s0 = std::max(0.0f, s - zip_tangent_step);
    const float s1 = std::min(len, s + zip_tangent_step);
    rf::Vector3 t = zip_point(zip, s1) - zip_point(zip, s0);
    if (t.len_sq() < 1.0e-10f) {
        t = zip_point(zip, len) - zip_point(zip, 0.0f);
    }
    t.normalize_safe();
    return t;
}

// Down and up as in rope_curve; re-derived every step, since a mover can tilt the line mid-ride.
float zip_cruise(const AlpineZipline& zip, int dir)
{
    const float rise = zip.curve.b.y - zip.curve.a.y;
    const bool down = std::fabs(rise) <= zip_level_epsilon ? dir > 0 : (rise < 0.0f) == (dir > 0);
    return down ? zip.down_speed : zip.up_speed;
}

struct ZipNearest
{
    float s = 0.0f;
    rf::Vector3 point{};
};

bool zip_nearest(const AlpineZipline& zip, const rf::Vector3& q, float max_dist, ZipNearest& out)
{
    if (zip.points.size() < 2 || q.x < zip.aabb_min.x - max_dist || q.x > zip.aabb_max.x + max_dist
        || q.y < zip.aabb_min.y - max_dist || q.y > zip.aabb_max.y + max_dist
        || q.z < zip.aabb_min.z - max_dist || q.z > zip.aabb_max.z + max_dist) {
        return false;
    }
    float best = max_dist * max_dist;
    bool found = false;
    for (std::size_t i = 0; i + 1 < zip.points.size(); ++i) {
        const rf::Vector3& a = zip.points[i];
        const rf::Vector3 ab = zip.points[i + 1] - a;
        const float ab_sq = ab.len_sq();
        float t = ab_sq > 1.0e-10f ? (q - a).dot_prod(ab) / ab_sq : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        const rf::Vector3 p = a + ab * t;
        const float d = (q - p).len_sq();
        if (d <= best) {
            best = d;
            out.s = std::min((static_cast<float>(i) + t) * zip.sample_step, zip.curve.length);
            out.point = p;
            found = true;
        }
    }
    return found;
}

float zip_hang_offset(const rf::Entity* ep)
{
    // Measured from the live eye rather than the table offset, so the grab test, the hang height and
    // what the player sees all agree.
    float eye = ep->eye_pos.y - ep->pos.y;
    if (!std::isfinite(eye) || eye < 0.0f || eye > 3.0f) {
        eye = zip_default_eye_height;
    }
    return eye + zip_grip_above_eye;
}

rf::Vector3 zip_grip_point(const rf::Entity* ep)
{
    return ep->pos + zip_up * zip_hang_offset(ep);
}

bool zip_line_visible_from(const rf::Entity* ep, const rf::Vector3& line_point)
{
    rf::Vector3 p0 = ep->eye_pos;
    rf::Vector3 to = line_point - p0;
    const float len = to.len();
    if (len < 0.15f) {
        return true;
    }
    // Stop short of the line: a line strung against a ceiling must not count as behind it.
    rf::Vector3 p1 = p0 + to * ((len - 0.1f) / len);
    rf::GCollisionOutput col{};
    return !rf::collide_linesegment_level_solid(p0, p1, rf::CF_ANY_HIT, &col);
}

// ─── Ride ───────────────────────────────────────────────────────────────────

struct ZipRide
{
    bool active = false;
    int entity_handle = -1;
    int32_t rope_uid = -1;
    int dir = 1; // +1 toward the target end
    float s = 0.0f;
    float speed = 0.0f;
    float hang = 0.0f;
    rf::Vector3 target_pos{};
    rf::Vector3 ride_vel{};
    // What the last first pass wrote to p_data.vel, to tell whether anything else has set it since.
    rf::Vector3 written_vel{};
    // This step stopped short at the end margin, so the ride velocity overshoots the target.
    bool at_end = false;
    rf::Timestamp attach_grace;
};

// What the machine simulating a rider keeps about their rides: the local player's own, or a server
// bot's on the server.
struct ZipRider
{
    ZipRide ride;
    int32_t regrab_uid = -1;
    rf::Timestamp regrab_cooldown;
    rf::Timestamp slam_immunity;
    bool last_ride_reached_end = false;
};

ZipRider g_local;
std::unordered_map<int, ZipRider> g_bot_riders; // by entity handle

enum class ReleaseKind
{
    silent,   // keep whatever velocity the entity has
    momentum, // carry the ride velocity
    jump,
    end,      // reached an anchor
    blocked,  // held off the path by geometry
};

bool zip_local_can_ride()
{
    return !rf::is_dedicated_server && !multi_spectate_is_spectating();
}

// A menu over a multiplayer game does not stop the world, so it only blocks starting a ride.
bool zip_local_can_start()
{
    return zip_local_can_ride() && rf::gameseq_get_state() == rf::GS_GAMEPLAY;
}

rf::Entity* zip_local_entity()
{
    if (!rf::local_player) {
        return nullptr;
    }
    return rf::entity_from_handle(rf::local_player->entity_handle);
}

bool zip_typing()
{
    return rf::console::console_is_visible() || rf::multi_chat_is_say_visible();
}

void zip_sync(const ZipRider& r, const rf::Entity* ep, bool riding)
{
    if (!rf::is_multi) {
        return;
    }
    const auto dir = static_cast<int8_t>(r.ride.dir);
    if (&r != &g_local) {
        // A server bot's ride is relayed as its client's request would be. Without the entity the relay
        // ends the ride on its own.
        if (rf::Player* player = ep ? rf::player_from_entity_handle(ep->handle) : nullptr) {
            zipline_server_on_state_request(player, r.ride.rope_uid, riding, dir);
        }
        return;
    }
    if (!rf::local_player) {
        return;
    }
    if (rf::is_server) {
        zipline_server_on_state_request(rf::local_player, r.ride.rope_uid, riding, dir);
    }
    else if (is_server_minimum_af_version(1, 5)) {
        af_send_zipline_state_request(r.ride.rope_uid, riding, dir);
    }
}

void zip_launch(rf::Entity* ep, const rf::Vector3& vel)
{
    ep->p_data.vel = vel;
    rf::entity_make_freefall(ep);
    // Jump pad recipe: without a raised cap the first air step clamps the carried speed away.
    const float horiz = std::sqrt(vel.x * vel.x + vel.z * vel.z);
    if (ep->info && horiz > ep->info->max_vel) {
        ep->custom_max_vel = horiz + 1.0f;
        ep->p_data.flags |= rf::PF_USE_CUSTOM_MAX_VEL;
    }
    // The jump's window, so the ground snap cannot catch a rider on the frame they let go.
    entity_skip_land_handling_after_jump(ep);
}

void zip_release(ZipRider& r, rf::Entity* ep, ReleaseKind kind)
{
    ZipRide& ride = r.ride;
    if (!ride.active) {
        return;
    }
    if (ep && kind != ReleaseKind::silent) {
        rf::Vector3 vel = ride.ride_vel;
        if (kind == ReleaseKind::jump) {
            vel.y += rf::jump_velocity;
            ep->entity_flags |= rf::EF_JUMP_START_ANIM;
            const int snd = ep->info ? rf::foley_get_sound_handle_at(ep->info->jump_sound, 0) : -1;
            if (snd >= 0) {
                rf::snd_play(snd, rf::SOUND_GROUP_EFFECTS, 0.0f, 1.0f);
            }
        }
        else if (kind == ReleaseKind::end || kind == ReleaseKind::blocked) {
            const float speed = vel.len();
            if (speed > zip_end_dismount_speed) {
                vel *= zip_end_dismount_speed / speed;
            }
            if (kind == ReleaseKind::end) {
                vel.y += zip_end_pop;
            }
        }
        zip_launch(ep, vel);
    }
    zip_sync(r, ep, false);
    r.regrab_uid = ride.rope_uid;
    r.regrab_cooldown.set(zip_regrab_cooldown_ms);
    r.slam_immunity.set(zip_release_slam_immunity_ms);
    r.last_ride_reached_end = kind == ReleaseKind::end;
    ride.active = false;
    ride.entity_handle = -1;
}

// Where a ride riding dir lets go.
float zip_end_s(const AlpineZipline& zip, int dir)
{
    return dir > 0 ? zip.curve.length - zip_end_margin : zip_end_margin;
}

// Advances the ride by dt and decides where the rider must be at the end of this frame. False when
// the ride ended (the release has already been applied). Runs from the physics pass, so dt is the
// time the frame actually simulates: rf::frametime read ahead of the stock frame is the last one.
bool zip_ride_step(ZipRider& r, rf::Entity* ep, const AlpineZipline& zip, float dt)
{
    ZipRide& ride = r.ride;
    const float len = zip.curve.length;
    const float end_s = zip_end_s(zip, ride.dir);
    ride.s = std::clamp(ride.s, 0.0f, len);
    // The step stops at the margin and the rider lets go from there on the next one, rather than up to
    // a whole step short of it at low frame rates.
    if (ride.dir > 0 ? ride.s >= end_s : ride.s <= end_s) {
        zip_release(r, ep, ReleaseKind::end);
        return false;
    }
    rf::Vector3 travel = zip_tangent(zip, ride.s) * static_cast<float>(ride.dir);

    const float cruise = zip_cruise(zip, ride.dir);
    const float slope_accel = -rf::gravity * travel.y;
    const float pull = (ride.speed < cruise ? zip_pull_below : zip_pull_above) * (cruise - ride.speed);
    ride.speed += (slope_accel + pull) * dt;
    ride.speed = std::clamp(ride.speed, cruise * zip_min_speed_frac,
                            std::min(cruise * zip_max_speed_frac, zip_max_ride_speed));
    ride.s += static_cast<float>(ride.dir) * ride.speed * dt;
    ride.s = ride.dir > 0 ? std::min(ride.s, end_s) : std::max(ride.s, end_s);
    ride.at_end = ride.s == end_s;

    travel = zip_tangent(zip, ride.s) * static_cast<float>(ride.dir);
    ride.ride_vel = travel * ride.speed;
    ride.target_pos = zip_point(zip, ride.s) - zip_up * ride.hang;
    return true;
}

struct ZipCandidate
{
    const AlpineZipline* zip = nullptr;
    ZipNearest hit;
    int dir = 1;
};

enum class ZipGrab
{
    use,
    bot, // the way the bot's route goes
};

bool zip_dir_allowed(const AlpineZipline& zip, int dir)
{
    switch (zip.direction) {
        case rope_curve::ZipDirection::emitter_to_target:
            return dir > 0;
        case rope_curve::ZipDirection::target_to_emitter:
            return dir < 0;
        default:
            return true;
    }
}

float zip_room(const AlpineZipline& zip, float s, int dir)
{
    return dir > 0 ? zip.curve.length - s : s;
}

// Calls visit(s) for each point a bot may take hold of a line riding dir, nearest the start anchor first, until it
// returns true. Returns whether it did.
template<typename Visitor>
bool zip_visit_bot_grabs(const AlpineZipline& zip, int dir, Visitor&& visit)
{
    const float first = zip_end_margin + zip_grab_inset;
    for (float d = 0.0f; d <= zip_bot_grab_span; d += zip_bot_grab_step) {
        const float s = dir > 0 ? first + d : zip.curve.length - first - d;
        if (zip_room(zip, s, dir) <= 2.0f * zip_end_margin + zip_bot_min_ride) {
            break;
        }
        if (visit(s)) {
            return true;
        }
    }
    return false;
}

// Which way a Use grab at arc position s rides, or 0 when it cannot ride at all: where the player
// looks, and from an anchor that means away from it.
int zip_choose_dir(const rf::Entity* ep, const AlpineZipline& zip, float s)
{
    int dir = 1;
    switch (zip.direction) {
        case rope_curve::ZipDirection::emitter_to_target:
            break;
        case rope_curve::ZipDirection::target_to_emitter:
            dir = -1;
            break;
        default: {
            const rf::Vector3 fwd = zip_tangent(zip, s);
            const float look = ep->eye_orient.fvec.dot_prod(fwd);
            const float moving = ep->p_data.vel.dot_prod(fwd);
            const float d = std::fabs(look) < 0.1f ? moving : look;
            dir = d < 0.0f ? -1 : 1;
            if (zip_room(zip, s, dir) <= 2.0f * zip_end_margin) {
                dir = -dir;
            }
            break;
        }
    }
    return zip_room(zip, s, dir) > 2.0f * zip_end_margin ? dir : 0;
}

bool zip_candidate_on(const ZipRider& r, const rf::Entity* ep, const AlpineZipline& zip, float reach,
                      ZipGrab grab, int want_dir, ZipCandidate& out)
{
    if (zip.uid == r.regrab_uid && r.regrab_cooldown.valid() && !r.regrab_cooldown.elapsed()) {
        return false;
    }
    const float len = zip.curve.length;
    const float first = zip_end_margin + zip_grab_inset;
    if (len <= 2.0f * first) {
        return false;
    }
    const rf::Vector3 grip = zip_grip_point(ep);
    ZipNearest hit;
    if (!zip_nearest(zip, grip, reach, hit)) {
        return false;
    }
    // Standing at an anchor is the normal way onto a line: the hands close on the first point a
    // ride can start from rather than the anchor itself, which is also clear of the pole.
    const float s = std::clamp(hit.s, first, len - first);
    const rf::Vector3 point = zip_point(zip, s);
    if ((point - grip).len_sq() > reach * reach || point.y < ep->eye_pos.y + zip_min_line_above_eye) {
        return false;
    }
    int dir = 0;
    if (grab == ZipGrab::use) {
        const rf::Vector3 to_point = point - ep->eye_pos;
        if (to_point.dot_prod(ep->eye_orient.fvec) < zip_use_view_cos * to_point.len()) {
            return false;
        }
        dir = zip_choose_dir(ep, zip, s);
    }
    else if (zip_dir_allowed(zip, want_dir) && zip_room(zip, s, want_dir) > 2.0f * zip_end_margin) {
        dir = want_dir;
    }
    if (dir == 0 || !zip_line_visible_from(ep, point)) {
        return false;
    }
    out.zip = &zip;
    out.hit = ZipNearest{s, point};
    out.dir = dir;
    return true;
}

bool zip_find_use_candidate(const ZipRider& r, const rf::Entity* ep, ZipCandidate& out)
{
    const rf::Vector3 grip = zip_grip_point(ep);
    float best = zip_use_reach * zip_use_reach;
    bool found = false;
    for (const AlpineZipline* zip : alpine_rope_active_ziplines()) {
        ZipCandidate cand;
        if (!zip_candidate_on(r, ep, *zip, zip_use_reach, ZipGrab::use, 0, cand)) {
            continue;
        }
        const float dist_sq = (cand.hit.point - grip).len_sq();
        if (dist_sq <= best) {
            best = dist_sq;
            out = cand;
            found = true;
        }
    }
    return found;
}

rf::Timestamp g_use_probe_next;
rf::Vector3 g_use_probe_pos{};
bool g_use_probe_found = false;

bool zip_probe_stock_use(rf::Entity* ep)
{
    // Stock answers these before it looks for a target at all.
    if (rf::entity_is_carrying_corpse(ep) || (rf::local_player && rf::local_player->view_from_handle != -1)) {
        return true;
    }
    int tag_handle = -1;
    if (const rf::Object* target = rf::player_find_use_target(ep, &tag_handle)) {
        // Its trigger pass has no range test, and multiplayer Use does nothing to a corpse, a charge, or a
        // vehicle hull hit away from its seats.
        const bool idle_hull = target->type == rf::OT_ENTITY
                               && vehicle_is_synced_entity_type(static_cast<const rf::Entity*>(target))
                               && !vehicle_use_target_boards(target, tag_handle);
        const bool inert_in_multi =
            rf::is_multi && (target->type == rf::OT_CORPSE || target->type == rf::OT_WEAPON || idle_hull);
        if (target->type != rf::OT_TRIGGER && !inert_in_multi) {
            return true;
        }
    }
    // A use-key trigger fires when the player stands in its volume and it can activate - the test stock
    // applies after the sweep - unless AF's team check would turn it away.
    for (rf::Trigger& trigger : DoublyLinkedList{rf::trigger_list}) {
        if ((trigger.trigger_flags & rf::TRIGGER_FLAG_USE_KEY) == 0
            || (trigger.trigger_flags & rf::TRIGGER_FLAG_LINKED_USE) != 0) {
            continue;
        }
        if (trigger.trigger_team != rf::trigger_team_any && rf::local_player
            && trigger.trigger_team != rf::local_player->team) {
            continue;
        }
        if (rf::trigger_check_activate(&trigger, ep, true) && obj_inside_trigger(&trigger, ep)) {
            return true;
        }
    }
    return false;
}

// Anything stock Use would act on (a button, door, item, seat) takes the press ahead of a line.
bool zip_stock_use_target_present(rf::Entity* ep, bool fresh)
{
    const bool moved = (ep->pos - g_use_probe_pos).len_sq() > zip_use_probe_move_sq;
    if (fresh || moved || !g_use_probe_next.valid() || g_use_probe_next.elapsed()) {
        g_use_probe_found = zip_probe_stock_use(ep);
        g_use_probe_pos = ep->pos;
        g_use_probe_next.set(zip_use_probe_interval_ms);
    }
    return g_use_probe_found;
}

bool zip_can_grab(rf::Entity* ep)
{
    return ep && !rf::entity_is_dying(ep) && ep->host_handle == -1 && !rf::entity_is_crouching(ep)
        && !rf::entity_is_swimming(ep) && !rf::entity_is_climbing(ep);
}

void zip_grab(ZipRider& r, rf::Entity* ep, const ZipCandidate& cand)
{
    ZipRide& ride = r.ride;
    const AlpineZipline& zip = *cand.zip;
    const int dir = cand.dir;
    const rf::Vector3 fwd = zip_tangent(zip, cand.hit.s);
    const float along = ep->p_data.vel.dot_prod(fwd * static_cast<float>(dir));
    ride.active = true;
    ride.entity_handle = ep->handle;
    ride.rope_uid = zip.uid;
    ride.dir = dir;
    ride.s = cand.hit.s;
    const float cruise = zip_cruise(zip, dir);
    ride.speed = std::clamp(along, cruise * zip_min_speed_frac,
                            std::min(cruise * zip_max_speed_frac, zip_max_ride_speed));
    // Never lower than where the hands close now: a line at head height must not pull a player who
    // grabbed it from a floor down into that floor, where the contact bounces them every frame.
    ride.hang = std::min(zip_hang_offset(ep), cand.hit.point.y - ep->pos.y);
    ride.target_pos = ep->pos;
    ride.ride_vel = rf::Vector3{};
    ride.written_vel = rf::Vector3{};
    ride.attach_grace.set(zip_attach_grace_ms);

    rf::entity_make_freefall(ep);
    zip_sync(r, ep, true);
}

// Lets go of a line the rider can no longer be on, or has been pushed or held off.
void zip_update_ride(ZipRider& r, rf::Entity* rider, bool can_ride)
{
    const ZipRide& ride = r.ride;
    if (!can_ride || !rider || rf::entity_is_dying(rider) || rider->host_handle != -1 || !rider->move_mode
        || rider->move_mode->mode != rf::MOVE_MODE_FALL) {
        zip_release(r, rider, ReleaseKind::silent);
        return;
    }
    const AlpineZipline* zip = alpine_rope_find_zipline(ride.rope_uid);
    if (!zip) {
        zip_release(r, rider, ReleaseKind::momentum);
        return;
    }
    if (rf::game_paused) {
        return;
    }

    // During the grace the rider is still being pulled in from wherever the grab started.
    if (!ride.attach_grace.elapsed()) {
        return;
    }
    // Against where the last step aimed, not the line as re-solved since: a mover may have carried it on.
    const float dev = rider->pos.distance_to(ride.target_pos);
    // A pinned rider falls a whole step behind per frame, which at low frame rates alone passes the
    // teleport distance; rf::frametime here is still the step the last physics pass took.
    if (dev > zip_teleport_deviation + ride.speed * rf::frametime) {
        // Whatever moved the rider may have set its own velocity; if not, the ride's is dropped
        // rather than carried out of the teleport.
        if ((rider->p_data.vel - ride.written_vel).len_sq() < 1.0e-6f) {
            rider->p_data.vel = rf::Vector3{};
        }
        zip_release(r, rider, ReleaseKind::silent);
    }
    else if (dev > zip_blocked_deviation) {
        zip_release(r, rider, ReleaseKind::blocked);
    }
}

void zip_update_local_ride()
{
    if (!g_local.ride.active) {
        return;
    }
    rf::Entity* ep = zip_local_entity();
    rf::Entity* rider = rf::entity_from_handle(g_local.ride.entity_handle);
    zip_update_ride(g_local, rider, zip_local_can_ride() && rider == ep);
}

void zip_update_bot_riders()
{
    for (auto it = g_bot_riders.begin(); it != g_bot_riders.end();) {
        ZipRider& r = it->second;
        rf::Entity* ep = rf::entity_from_handle(it->first);
        if (r.ride.active) {
            zip_update_ride(r, ep, ep && server_bot_from_entity(ep));
        }
        const bool immune = r.slam_immunity.valid() && !r.slam_immunity.elapsed();
        const bool cooling = r.regrab_cooldown.valid() && !r.regrab_cooldown.elapsed();
        if (!r.ride.active && (!ep || (!immune && !cooling))) {
            it = g_bot_riders.erase(it);
            continue;
        }
        ++it;
    }
}

// The rider whose ride this machine simulates for ep, if one is under way.
ZipRider* zip_active_rider(const rf::Entity* ep)
{
    if (!ep) {
        return nullptr;
    }
    if (g_local.ride.active && g_local.ride.entity_handle == ep->handle) {
        return &g_local;
    }
    if (g_bot_riders.empty()) {
        return nullptr;
    }
    const auto it = g_bot_riders.find(ep->handle);
    return it != g_bot_riders.end() && it->second.ride.active ? &it->second : nullptr;
}

// ─── Hint ───────────────────────────────────────────────────────────────────

enum class ZipHint
{
    none,
    grab,
    ride,
};

ZipHint g_hint_wanted = ZipHint::none;
std::string g_hint_text;

void zip_update_hint()
{
    ZipHint wanted = ZipHint::none;
    rf::Entity* ep = zip_local_entity();
    if (g_local.ride.active) {
        wanted = ZipHint::ride;
    }
    else if (zip_local_can_start() && zip_can_grab(ep) && !zip_typing()) {
        ZipCandidate cand;
        if (zip_find_use_candidate(g_local, ep, cand) && !zip_stock_use_target_present(ep, false)) {
            wanted = ZipHint::grab;
        }
    }
    // Built on the edge only: resolving a bind name allocates on the engine heap.
    if (wanted != ZipHint::none && wanted != g_hint_wanted) {
        if (wanted == ZipHint::grab) {
            g_hint_text = "Zip line - Press [" + get_action_bind_name_or(rf::CC_ACTION_USE, "USE") + "]";
        }
        else {
            g_hint_text = "[" + get_action_bind_name_or(rf::CC_ACTION_JUMP, "JUMP") + "] Jump off   ["
                          + get_action_bind_name_or(rf::CC_ACTION_CROUCH, "CROUCH") + "] Let go";
        }
    }
    g_hint_wanted = wanted;
}

// ─── Effects ────────────────────────────────────────────────────────────────

struct RemoteRider
{
    int32_t rope_uid = -1;
    int8_t dir = 1;
    int entity_handle = -1; // latched on first sight; a new entity means a new life, off the line
};

std::unordered_map<uint8_t, RemoteRider> g_remote_riders; // by player id

struct RiderFx
{
    int32_t rope_uid = -1;
    int8_t dir = 1;
    int snd = -1;
    rf::Timestamp snd_retry;
    bool seen = false;
};

std::unordered_map<int, RiderFx> g_rider_fx; // by entity handle

rf::VMesh* g_zip_mesh = nullptr;
bool g_zip_mesh_attempted = false;
int g_zip_sound = -2;
bool g_zip_sound_failed = false;

int zip_sound_handle()
{
    if (g_zip_sound == -2) {
        g_zip_sound = rf::snd_pc_find_by_name(zip_sound_filename);
    }
    return g_zip_sound;
}

void zip_fx_stop(RiderFx& fx)
{
    if (fx.snd >= 0) {
        rf::snd_stop(fx.snd);
        fx.snd = -1;
    }
}

void zip_fx_stop_all()
{
    for (auto& [handle, fx] : g_rider_fx) {
        zip_fx_stop(fx);
    }
    g_rider_fx.clear();
}

void zip_fx_touch(rf::Entity* ep, int32_t rope_uid, int8_t dir)
{
    RiderFx& fx = g_rider_fx[ep->handle];
    fx.rope_uid = rope_uid;
    fx.dir = dir;
    fx.seen = true;
    if (g_zip_sound_failed) {
        return;
    }
    if (fx.snd >= 0 && rf::snd_is_playing(fx.snd)) {
        rf::snd_change_3d(fx.snd, ep->pos, ep->p_data.vel, 1.0f);
        return;
    }
    if (fx.snd_retry.valid() && !fx.snd_retry.elapsed()) {
        return;
    }
    const rf::Vector3 no_vel{};
    fx.snd = rf::snd_play_3d(zip_sound_handle(), ep->pos, 1.0f, no_vel, rf::SOUND_GROUP_EFFECTS);
    if (fx.snd < 0) {
        // Sound off or every channel busy; worth another try shortly.
        fx.snd_retry.set(zip_sound_retry_ms);
    }
}

// Loaded ahead of the first ride so neither the grab nor the first rendered trolley hitches.
void zip_preload_assets()
{
    if (!g_zip_mesh_attempted) {
        g_zip_mesh_attempted = true;
        g_zip_mesh = rf::vmesh_load(zip_mesh_filename, rf::MESH_TYPE_STATIC, -1);
        if (!g_zip_mesh) {
            xlog::warn("[Zipline] Failed to load mesh '{}', trolleys will not be drawn", zip_mesh_filename);
        }
    }
    if (g_zip_sound == -2) {
        const int handle = zip_sound_handle();
        if (handle < 0 || rf::snd_load_hint(handle) < 0) {
            g_zip_sound_failed = true;
            // With sound switched off nothing loads.
            if (rf::sound_enabled) {
                xlog::warn("[Zipline] Failed to load ride sound '{}'", zip_sound_filename);
            }
        }
    }
}

// A remote rider counts only while actually on the line, not after a let-go this client never got, a rider
// now in a vehicle, or a claim from nowhere near the line.
bool zip_remote_rider_on_line(int32_t rope_uid, const rf::Entity* ep)
{
    const AlpineZipline* zip = alpine_rope_find_zipline(rope_uid);
    ZipNearest hit;
    return zip && ep->host_handle == -1 && zip_nearest(*zip, zip_grip_point(ep), zip_teleport_deviation, hit);
}

void zip_update_effects()
{
    for (auto& [handle, fx] : g_rider_fx) {
        fx.seen = false;
    }

    if (g_local.ride.active) {
        if (rf::Entity* ep = rf::entity_from_handle(g_local.ride.entity_handle)) {
            zip_fx_touch(ep, g_local.ride.rope_uid, static_cast<int8_t>(g_local.ride.dir));
        }
    }

    for (auto it = g_remote_riders.begin(); it != g_remote_riders.end();) {
        rf::Player* player = rf::multi_find_player_by_id(it->first);
        rf::Entity* ep = player ? rf::entity_from_handle(player->entity_handle) : nullptr;
        RemoteRider& rider = it->second;
        if (!player || (ep && rider.entity_handle != -1 && rider.entity_handle != ep->handle)
            || (ep && rf::entity_is_dying(ep))) {
            it = g_remote_riders.erase(it);
            continue;
        }
        if (ep && ep != zip_local_entity()) {
            rider.entity_handle = ep->handle;
            if (zip_remote_rider_on_line(rider.rope_uid, ep)) {
                zip_fx_touch(ep, rider.rope_uid, rider.dir);
            }
        }
        ++it;
    }

    for (auto it = g_rider_fx.begin(); it != g_rider_fx.end();) {
        if (!it->second.seen) {
            zip_fx_stop(it->second);
            it = g_rider_fx.erase(it);
            continue;
        }
        ++it;
    }
}

// ─── Server relay ───────────────────────────────────────────────────────────

struct ZipServerState
{
    int last_handle = -1;
    bool sent_riding = false;
    int32_t sent_uid = -1;
    int8_t sent_dir = 1;
    bool want_riding = false;
    int32_t want_uid = -1;
    int8_t want_dir = 1;
    rf::Timestamp cooldown;

    bool settled() const
    {
        return want_riding == sent_riding && (!want_riding || (want_uid == sent_uid && want_dir == sent_dir));
    }
};

std::unordered_map<uint8_t, ZipServerState> g_server_riders; // by player id

// Everything an entry remembers belongs to one entity; a respawn starts off the line everywhere.
void zip_server_rebase(ZipServerState& state, int entity_handle)
{
    if (state.last_handle == entity_handle) {
        return;
    }
    state = ZipServerState{};
    state.last_handle = entity_handle;
}

void zip_server_broadcast(uint8_t player_id, ZipServerState& state)
{
    af_send_zipline_state(player_id, state.want_uid, state.want_riding, state.want_dir);
    // A listen host gets none of its own broadcasts, so it shows other riders from here.
    if (!rf::is_dedicated_server) {
        zipline_on_remote_state(player_id, state.want_uid, state.want_riding, state.want_dir);
    }
    state.sent_riding = state.want_riding;
    state.sent_uid = state.want_uid;
    state.sent_dir = state.want_dir;
    state.cooldown.set(zip_server_sync_interval_ms);
}

void zip_server_do_frame()
{
    for (auto it = g_server_riders.begin(); it != g_server_riders.end();) {
        ZipServerState& state = it->second;
        rf::Player* player = rf::multi_find_player_by_id(it->first);
        if (!player) {
            it = g_server_riders.erase(it);
            continue;
        }
        rf::Entity* ep = rf::entity_from_handle(player->entity_handle);
        if (!ep || rf::entity_is_dying(ep) || ep->handle != state.last_handle
            || (state.want_riding && !alpine_rope_find_zipline(state.want_uid))) {
            state.want_riding = false;
        }
        if (!state.settled() && (!state.cooldown.valid() || state.cooldown.elapsed())) {
            zip_server_broadcast(it->first, state);
        }
        if (!state.sent_riding && !state.want_riding) {
            it = g_server_riders.erase(it);
            continue;
        }
        ++it;
    }
}

// Follows a let-go at the end of a cruising ride (zip_release with ReleaseKind::end) for up to two seconds or until it
// meets the level: the rider comes down about where it last flew free.
rf::Vector3 zip_predict_landing(const AlpineZipline& zip, float end_s, int dir, float hang)
{
    rf::Vector3 pos = zip_point(zip, end_s) - zip_up * hang;
    const float speed = std::min(zip_cruise(zip, dir), zip_end_dismount_speed);
    rf::Vector3 vel = zip_tangent(zip, end_s) * (static_cast<float>(dir) * speed) + zip_up * zip_end_pop;
    for (int i = 0; i < zip_bot_landing_steps; ++i) {
        rf::Vector3 next = pos + vel * zip_bot_landing_step;
        vel.y -= std::fabs(rf::gravity) * zip_bot_landing_step;
        rf::GCollisionOutput col{};
        if (rf::collide_linesegment_level_solid(pos, next, rf::CF_ANY_HIT, &col)) {
            break;
        }
        pos = next;
    }
    return pos;
}

void zip_render_trolley(rf::Entity* ep, const RiderFx& fx)
{
    if (!g_zip_mesh) {
        return;
    }
    const AlpineZipline* zip = alpine_rope_find_zipline(fx.rope_uid);
    ZipNearest hit;
    if (!zip || !zip_nearest(*zip, zip_grip_point(ep), zip_teleport_deviation, hit)) {
        return;
    }
    // Mesh convention: origin on the cable, +Z along the direction of travel, +Y up.
    const rf::Vector3 fwd = zip_tangent(*zip, hit.s) * static_cast<float>(fx.dir);
    rf::Matrix3 orient;
    orient.make_quick(fwd);
    rf::Vector3 pos = zip_point(*zip, hit.s);
    rf::MeshRenderParams params{};
    params.init_defaults();
    params.orient = orient;
    rf::vmesh_render(g_zip_mesh, &pos, &orient, &params);
}

} // namespace

void zipline_level_init()
{
    g_local = ZipRider{};
    g_bot_riders.clear();
    g_use_probe_next.invalidate();
    g_use_probe_found = false;
    g_hint_wanted = ZipHint::none;
    g_remote_riders.clear();
    zip_fx_stop_all();
    g_server_riders.clear();
    // The engine frees level meshes on unload; only the pointer is dropped.
    g_zip_mesh = nullptr;
    g_zip_mesh_attempted = false;
    g_zip_sound = -2;
    g_zip_sound_failed = false;
}

void zipline_do_frame()
{
    alpine_rope_update_ziplines();

    if (rf::is_server && rf::is_multi) {
        zip_server_do_frame();
        zip_update_bot_riders();
    }
    if (rf::is_dedicated_server) {
        return;
    }

    const bool headless = is_headless_mode();
    if (!headless && !alpine_rope_active_ziplines().empty()) {
        zip_preload_assets();
    }
    zip_update_local_ride();
    if (headless) {
        return;
    }
    zip_update_hint();
    zip_update_effects();
}

bool zipline_air_move(rf::Entity* ep)
{
    ZipRider* r = zip_active_rider(ep);
    if (!r) {
        return false;
    }
    ZipRide& ride = r->ride;
    const float dt = ep->p_data.frame_time_left;
    // p_data.pos, not Object::pos: after a contact the later passes start from where the sweep
    // stopped, and Object::pos is only committed once the frame's passes are done.
    const rf::Vector3 pos = ep->p_data.pos;
    // Later passes only exist after a contact, and the collision response has already turned the
    // velocity into a slide along it. Re-aiming at the target would drive the rider back into the
    // surface for the rest of the frame, so those passes integrate the slide as stock does.
    if ((ep->p_data.flags & rf::PF_ACCEL_APPLIED) != 0) {
        // A hit from a fast mover can add speed, and the pass loop kills anything over 100.
        rf::Vector3& slide = ep->p_data.vel;
        const float slide_speed = slide.len();
        if (slide_speed > zip_max_total_speed) {
            slide *= zip_max_total_speed / slide_speed;
        }
        ep->p_data.next_pos = pos + slide * dt;
        return true;
    }
    const AlpineZipline* zip = alpine_rope_find_zipline(ride.rope_uid);
    if (!zip) {
        zip_release(*r, ep, ReleaseKind::momentum);
        return false;
    }
    if (!zip_ride_step(*r, ep, *zip, dt)) {
        return false;
    }
    rf::Vector3 vel = ride.ride_vel;
    if (ride.at_end && dt > 1.0e-4f) {
        // The capped correction cannot take a fast ride's speed off in one step; land on the margin
        // exactly, and let the next step release with the ride velocity intact.
        vel = (ride.target_pos - pos) * (1.0f / dt);
    }
    else if (dt > 1.0e-4f) {
        rf::Vector3 corr = (ride.target_pos - pos) * (1.0f / dt) - vel;
        const float corr_len = corr.len();
        if (corr_len > zip_max_correction) {
            corr *= zip_max_correction / corr_len;
        }
        vel += corr;
    }
    const float speed = vel.len();
    if (speed > zip_max_total_speed) {
        vel *= zip_max_total_speed / speed;
    }
    ep->p_data.vel = vel;
    ride.written_vel = vel;
    // Stock air move ends by integrating next_pos, which the sweep moves toward; skipping it without
    // this would freeze the rider.
    ep->p_data.next_pos = pos + vel * dt;
    return true;
}

bool zipline_local_is_riding(const rf::Entity* ep)
{
    return g_local.ride.active && ep && ep->handle == g_local.ride.entity_handle;
}

bool zipline_entity_simulates_ride(const rf::Entity* ep)
{
    return zip_active_rider(ep) != nullptr;
}

bool zipline_execute_action(rf::Player* player, rf::ControlConfigAction action)
{
    // A bot client's navigation takes lines itself (zipline_bot_grab), so its own presses never start one.
    if (!player || player != rf::local_player || client_bot_launch_enabled()) {
        return false;
    }
    rf::Entity* ep = zip_local_entity();
    if (g_local.ride.active) {
        // Taken from the dispatched action rather than polled, so a jump bound to the mouse wheel
        // lets go too: polling never reads the wheel.
        if (!zipline_local_is_riding(ep)) {
            return false;
        }
        if (action == rf::CC_ACTION_JUMP) {
            zip_release(g_local, ep, ReleaseKind::jump);
            return true;
        }
        if (action == rf::CC_ACTION_CROUCH) {
            zip_release(g_local, ep, ReleaseKind::momentum);
            return true;
        }
        return false;
    }
    if (action != rf::CC_ACTION_USE || !zip_local_can_start() || !zip_can_grab(ep)) {
        return false;
    }
    ZipCandidate cand;
    if (!zip_find_use_candidate(g_local, ep, cand) || zip_stock_use_target_present(ep, true)) {
        return false;
    }
    zip_grab(g_local, ep, cand);
    return true;
}

bool zipline_bot_grab(rf::Entity* ep, int32_t rope_uid, int dir)
{
    if (!ep) {
        return false;
    }
    const bool local = client_bot_launch_enabled() && ep == zip_local_entity();
    const bool server_bot = !local && rf::is_server && rf::is_multi && server_bot_from_entity(ep);
    if (local ? !zip_local_can_ride() : !server_bot) {
        return false;
    }
    // A bot that has not ridden lately has no entry; it gets one only when it takes hold.
    static const ZipRider no_rides;
    const auto it = local ? g_bot_riders.end() : g_bot_riders.find(ep->handle);
    const ZipRider& known = local ? g_local : (it != g_bot_riders.end() ? it->second : no_rides);
    const AlpineZipline* zip = alpine_rope_find_zipline(rope_uid);
    ZipCandidate cand;
    if (known.ride.active || !zip_can_grab(ep) || !zip
        || !zip_candidate_on(known, ep, *zip, zip_use_reach, ZipGrab::bot, dir, cand)) {
        return false;
    }
    zip_grab(local ? g_local : g_bot_riders[ep->handle], ep, cand);
    return true;
}

void zipline_collect_bot_routes(std::vector<ZiplineBotRoute>& out)
{
    out.clear();
    const float first = zip_end_margin + zip_grab_inset;
    const float hang = zip_default_eye_height + zip_grip_above_eye;
    for (const AlpineZipline* zip : alpine_rope_all_ziplines()) {
        for (const int dir : {1, -1}) {
            if (!zip_dir_allowed(*zip, dir)) {
                continue;
            }
            ZiplineBotRoute route;
            route.rope_uid = zip->uid;
            route.dir = dir;
            zip_visit_bot_grabs(*zip, dir, [&](float s) {
                route.grab_points.push_back(zip_point(*zip, s));
                return false;
            });
            if (route.grab_points.empty()) {
                continue;
            }
            // The body hangs under the line for the whole ride; anything it would sweep into holds it off
            // the line and drops it mid-ride.
            const float start_s = dir > 0 ? first : zip->curve.length - first;
            const float end_s = zip_end_s(*zip, dir);
            bool clear = true;
            rf::Vector3 prev = zip_point(*zip, start_s) - zip_up * hang;
            for (float s = start_s; clear && (dir > 0 ? s < end_s : s > end_s);) {
                s = dir > 0 ? std::min(s + zip_bot_clear_step, end_s) : std::max(s - zip_bot_clear_step, end_s);
                const rf::Vector3 next = zip_point(*zip, s) - zip_up * hang;
                for (const float drop : {0.0f, zip_bot_clear_feet}) {
                    rf::GCollisionOutput col{};
                    rf::Vector3 p0 = prev - zip_up * drop;
                    rf::Vector3 p1 = next - zip_up * drop;
                    if (rf::collide_linesegment_level_solid(p0, p1, rf::CF_ANY_HIT, &col)) {
                        clear = false;
                    }
                }
                prev = next;
            }
            if (!clear) {
                continue;
            }
            route.landing_pos = zip_predict_landing(*zip, end_s, dir, hang);
            out.push_back(std::move(route));
        }
    }
}

bool zipline_bot_reaches_line(const rf::Vector3& stand_pos, const rf::Vector3& line_point, bool jump)
{
    if (line_point.y < stand_pos.y + zip_default_eye_height + zip_min_line_above_eye) {
        return false;
    }
    const rf::Vector3 grip = stand_pos + zip_up * (zip_default_eye_height + zip_grip_above_eye);
    // A jump carries the hands straight up through every height to the top of it.
    const float rise = jump ? rf::jump_velocity * rf::jump_velocity / (2.0f * std::fabs(rf::gravity)) : 0.0f;
    float dy = line_point.y - grip.y;
    dy = dy > rise ? dy - rise : std::min(dy, 0.0f);
    const float dx = line_point.x - grip.x;
    const float dz = line_point.z - grip.z;
    const float reach = zip_use_reach * zip_bot_reach_share;
    return dx * dx + dy * dy + dz * dz <= reach * reach;
}

bool zipline_bot_ride_open(int32_t rope_uid, int dir, const rf::Vector3& stand_pos, float* ride_seconds)
{
    const AlpineZipline* zip = alpine_rope_find_zipline(rope_uid);
    if (!zip || !zip_dir_allowed(*zip, dir)) {
        return false;
    }
    return zip_visit_bot_grabs(*zip, dir, [&](float s) {
        if (!zipline_bot_reaches_line(stand_pos, zip_point(*zip, s), true)) {
            return false;
        }
        if (ride_seconds) {
            *ride_seconds = std::fabs(zip_end_s(*zip, dir) - s) / zip_cruise(*zip, dir);
        }
        return true;
    });
}

bool zipline_entity_is_riding(const rf::Entity* ep)
{
    if (!ep) {
        return false;
    }
    if (g_rider_fx.find(ep->handle) != g_rider_fx.end() || zip_active_rider(ep)) {
        return true;
    }
    // A dedicated server builds no rider effects, but it simulates server bots a rider can brush past.
    if (rf::is_dedicated_server) {
        for (const auto& [id, state] : g_server_riders) {
            if (state.want_riding && state.last_handle == ep->handle) {
                return zip_remote_rider_on_line(state.want_uid, ep);
            }
        }
    }
    return false;
}

bool zipline_last_ride_reached_end(const rf::Entity* ep)
{
    if (!ep) {
        return false;
    }
    if (rf::local_player && ep->handle == rf::local_player->entity_handle) {
        return g_local.last_ride_reached_end;
    }
    const auto it = g_bot_riders.find(ep->handle);
    return it != g_bot_riders.end() && it->second.last_ride_reached_end;
}

bool zipline_slam_immune(const rf::Entity* ep)
{
    if (!ep) {
        return false;
    }
    const ZipRider* r = nullptr;
    if (rf::local_player && ep->handle == rf::local_player->entity_handle) {
        r = &g_local;
    }
    else if (const auto it = g_bot_riders.find(ep->handle); it != g_bot_riders.end()) {
        r = &it->second;
    }
    if (!r) {
        return false;
    }
    return (r->ride.active && r->ride.entity_handle == ep->handle)
        || (r->slam_immunity.valid() && !r->slam_immunity.elapsed());
}

void zipline_render_attachment(rf::Entity* ep)
{
    // A monitor shows the body even to its own first-person viewer.
    if (!ep || g_rider_fx.empty() || (!rf::monitor_render_in_progress && multi_spectate_view_is_first_person_of(ep))) {
        return;
    }
    if (auto it = g_rider_fx.find(ep->handle); it != g_rider_fx.end()) {
        zip_render_trolley(ep, it->second);
    }
}

void zipline_render_first_person()
{
    for (const auto& [handle, fx] : g_rider_fx) {
        rf::Entity* ep = rf::entity_from_handle(handle);
        if (ep && multi_spectate_view_is_first_person_of(ep)) {
            zip_render_trolley(ep, fx);
        }
    }
}

const std::string& zipline_hint_text()
{
    static const std::string none;
    return g_hint_wanted == ZipHint::none ? none : g_hint_text;
}

void zipline_server_on_state_request(rf::Player* player, int32_t rope_uid, bool riding, int8_t dir)
{
    if (!rf::is_server || !rf::is_multi || !player || !player->net_data) {
        return;
    }
    rf::Entity* ep = rf::entity_from_handle(player->entity_handle);
    if (riding && (!ep || rf::entity_is_dying(ep) || !alpine_rope_find_zipline(rope_uid))) {
        return;
    }
    const uint8_t id = player->net_data->player_id;
    auto found = g_server_riders.find(id);
    if (!riding && found == g_server_riders.end()) {
        return;
    }
    ZipServerState& state = g_server_riders[id];
    zip_server_rebase(state, ep ? ep->handle : -1);
    state.want_riding = riding;
    if (riding) {
        state.want_uid = rope_uid;
        state.want_dir = dir;
    }
    if (!state.settled() && (!state.cooldown.valid() || state.cooldown.elapsed())) {
        zip_server_broadcast(id, state);
    }
}

void zipline_on_remote_state(uint8_t player_id, int32_t rope_uid, bool riding, int8_t dir)
{
    if (rf::is_dedicated_server) {
        return;
    }
    if (rf::local_player && rf::local_player->net_data && rf::local_player->net_data->player_id == player_id) {
        return;
    }
    if (!riding) {
        g_remote_riders.erase(player_id);
        return;
    }
    RemoteRider& rider = g_remote_riders[player_id];
    rider.rope_uid = rope_uid;
    rider.dir = dir;
    // A riding packet always describes the rider's current life (the server rebases per entity), so
    // it re-latches rather than trusting an entity an unsent "off" left behind.
    rf::Player* player = rf::multi_find_player_by_id(player_id);
    rf::Entity* ep = player ? rf::entity_from_handle(player->entity_handle) : nullptr;
    rider.entity_handle = ep ? ep->handle : -1;
}

void zipline_send_state_to(rf::Player* player)
{
    if (!rf::is_server || !player) {
        return;
    }
    for (const auto& [id, state] : g_server_riders) {
        if (state.sent_riding) {
            af_send_zipline_state(id, state.sent_uid, true, state.sent_dir, player);
        }
    }
}
