#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <xlog/xlog.h>
#include <common/payload.h>
#include <common/utils/list-utils.h>
#include "payload.h"
#include "alpine_packets.h"
#include "kill.h"
#include "gametype.h"
#include "server_internal.h"
#include "../hud/hud.h"
#include "../hud/multi_spectate.h"
#include "../misc/level.h"
#include "../misc/misc.h"
#include "../object/event_alpine.h"
#include "../object/mover.h"
#include "../sound/sound.h"
#include "../rf/entity.h"
#include "../rf/event.h"
#include "../rf/gameseq.h"
#include "../rf/geometry.h"
#include "../rf/level.h"
#include "../rf/mover.h"
#include "../rf/multi.h"
#include "../rf/object.h"
#include "../rf/os/frametime.h"
#include "../rf/os/timestamp.h"
#include "../rf/player/player.h"
#include "../rf/sound/sound.h"
#include "../rf/vmesh.h"
#include "../rf/weapon.h"

namespace
{

constexpr float max_frame_seconds = 0.25f;
constexpr int moving_refresh_ms = 125;
// Coalesces reliable sends for pushers stepping in and out of the zone
constexpr int list_resend_min_ms = 100;
// A cart that coasts onto a checkpoint credits whoever pushed it this recently
constexpr int capture_credit_ms = 3000;
// A client further off the server's distance than this jumps there instead of easing
constexpr float net_snap_distance = 3.0f;
// The share of a client's distance error removed per second
constexpr float net_blend_rate = 6.0f;
// Riders take the cart's movement as their velocity, so easing never adds more than this to its speed
constexpr float net_max_correction_speed = 1.5f;
// A late or lost packet stops the client's extrapolation here rather than running the cart on
constexpr float net_max_extrapolation = 0.5f;
constexpr float finale_full_damage_radius = 6.0f;
constexpr float finale_damage_radius = 12.0f;
// Kills at 200 life and 200 armour: MP scales damage with no killer by as little as 0.5, and armour absorbs 2:1
constexpr float finale_max_damage = 2000.0f;
constexpr float finale_vclip_radius = 6.0f;
constexpr const char* finale_vclip = "shoulder_cannon_explode";
// From the cart reaching the end of its track to the level ending
constexpr int finale_delay_ms = 5000;
// The final stretch is the last tenth of the track, or its last 10 m if that is shorter
constexpr float final_stretch_fraction = 0.1f;
constexpr float final_stretch_max_length = 10.0f;
// Of the held weapon's magazine, on each heal tick
constexpr float resupply_magazine_share = 0.25f;
constexpr int setup_count_from = 5;
constexpr int final_stretch_alarm_ms = 950; // as KOTH's alarm for a point being taken
constexpr int overtime_tick_ms = 1000;
constexpr const char* setup_end_sound_file = "Alarm_04.wav";
constexpr const char* rollback_sound_file = "Beep06.wav";
constexpr const char* final_stretch_sound_file = "Alarm_02.wav";

// Sound cues, worked out on every machine that plays sound from its own view of the state
struct PayloadCues
{
    bool seeded = false;
    PayloadCartState state = PayloadCartState::idle;
    bool setup = true;
    std::optional<int> setup_count;
    rf::Timestamp final_stretch_alarm;
    rf::Timestamp overtime_tick;
    int setup_end_sound = -1;
    int rollback_sound = -1;
    int final_stretch_sound = -1;
};

// What a pusher's resupply has yet to hand over, so a small magazine still gets its share across ticks
struct ResupplyOwed
{
    int entity_handle = -1;
    int weapon_type = -1;
    float rounds = 0.0f;
};

struct KeyframeEvent
{
    float distance = 0.0f;
    int event_uid = -1;
};

struct PayloadInfo
{
    bool active = false;
    int cart_handle = -1;
    std::string name = payload::default_name;
    int attacking_team = 1;
    float push_radius = payload::default_push_radius;
    int rollback_delay = payload::default_rollback_delay;
    // The level's setting; a client takes the server's, which also needs a nonzero heal amount
    bool heals = true;
    bool finale = true;

    // By keyframe
    std::vector<float> keyframe_distance;
    float total_length = 0.0f;
    std::vector<PayloadCheckpoint> checkpoints;
    std::vector<KeyframeEvent> pass_events;
    // The centre of the cart's brushes in the frame of one of them, which every member turns with
    int center_object_handle = -1;
    rf::Vector3 center_local{};
    rf::Vector3 center_offset{}; // from keyframe 0, as the level places the cart
    float cart_half_height = 0.0f;

    float distance = 0.0f;
    float speed = 0.0f;
    // Keyframe events fire on the cart's first pass only
    float furthest = 0.0f;
    PayloadCartState state = PayloadCartState::idle;
    bool overtime = false;
    int next_checkpoint = 0;
    std::optional<float> countdown; // see payload_countdown_seconds
    std::vector<uint8_t> pushers;
    std::vector<uint8_t> blockers;
    uint8_t heal_serial = 0;
    uint8_t net_sequence = 0; // the server's last sent, or the client's last applied
    bool setup = true; // until this side fires When_Setup_Ends
    float setup_end_time = 0.0f; // rf::level.time

    // Server
    float hold_left = 0.0f;
    float unpushed_seconds = 0.0f;
    float overtime_unpushed_seconds = 0.0f;
    std::vector<uint8_t> last_pushers;
    rf::Timestamp capture_credit;
    rf::Timestamp heal_tick;
    rf::Timestamp finale_end;
    rf::Timestamp moving_refresh;
    rf::Timestamp list_resend;
    bool rollback_announced = false;
    bool final_stretch_reached = false;
    bool end_announced = false;
    // Unreliable sends may be lost, so changes are judged against the last reliable one
    std::optional<PayloadNetState> last_reliable;
    std::unordered_map<const rf::Player*, ResupplyOwed> resupply_owed;
    uint8_t sent_heal_serial = 0;

    // Client
    bool net_seeded = false;
    float net_distance = 0.0f;
    float net_speed = 0.0f;
    float net_age = 0.0f;
    bool net_setup = true;

    PayloadCues cues;
};

PayloadInfo g_payload;

// rf::multi_time_limit is netgame.max_time_seconds, which outlives the level
struct TimeBonus
{
    float base_limit = 0.0f;
    float limit = 0.0f; // as the last bonus left it
};
std::optional<TimeBonus> g_time_bonus;

const char* team_name(int team)
{
    return team == 0 ? "RED" : "BLUE";
}

HillInfo* checkpoint_hill(const PayloadCheckpoint& checkpoint)
{
    return koth_find_hill_by_handler(checkpoint.handler);
}

rf::Mover* cart()
{
    return g_payload.active ? rf::mover_from_handle(g_payload.cart_handle) : nullptr;
}

float final_stretch_start()
{
    return g_payload.total_length
        - std::min(g_payload.total_length * final_stretch_fraction, final_stretch_max_length);
}

float rollback_floor()
{
    return g_payload.next_checkpoint > 0 ? g_payload.checkpoints[g_payload.next_checkpoint - 1].distance : 0.0f;
}

PayloadCartPlacement placement_at(float distance)
{
    const auto& kd = g_payload.keyframe_distance;
    const int last_segment = static_cast<int>(kd.size()) - 2;
    // The last keyframe at or before the distance, so a zero length segment never wins over the one after it
    const auto after = std::upper_bound(kd.begin(), kd.end(), distance);
    const int segment = std::clamp(static_cast<int>(after - kd.begin()) - 1, 0, last_segment);
    const float length = kd[segment + 1] - kd[segment];
    const float fraction = length > 0.0f ? std::clamp((distance - kd[segment]) / length, 0.0f, 1.0f) : 1.0f;
    return {segment, fraction, g_payload.speed};
}

rf::Mover* find_linked_mover(const rf::Event& handler)
{
    for (const int handle : handler.links) {
        if (rf::Object* obj = rf::obj_from_handle(handle); obj && obj->type == rf::OT_MOVER) {
            return static_cast<rf::Mover*>(obj);
        }
    }
    return nullptr;
}

bool validate_cart(rf::Mover& mover, int handler_uid)
{
    const char* problem = nullptr;
    if (!rfl_version_minimum(306) || AlpineLevelProperties::instance().legacy_movers) {
        problem = "needs RFL 306 or later with legacy movers off";
    }
    else if (mover.keyframe_move_type != rf::MoverKeyframeMoveType::MKMT_ONE_WAY) {
        problem = "must be One Way";
    }
    else if (rf::mover_rotates_in_place(&mover)) {
        problem = "must not Rotate in Place";
    }
    else if (mover.keyframes.size() < 2) {
        problem = "needs at least 2 keyframes";
    }
    if (problem) {
        xlog::warn("[Payload] The cart linked from Payload_Handler {} {}; the level has no cart", handler_uid, problem);
        return false;
    }
    return true;
}

void build_track(const rf::Mover& mover)
{
    const int count = mover.keyframes.size();
    g_payload.keyframe_distance.assign(count, 0.0f);
    for (int i = 0; i + 1 < count; ++i) {
        g_payload.keyframe_distance[i + 1] = g_payload.keyframe_distance[i] + alpine_mover_segment_length(&mover, i);
    }
    g_payload.total_length = g_payload.keyframe_distance.back();

    for (int i = 0; i < count; ++i) {
        const rf::MoverKeyframe* kf = mover.keyframes[i];
        rf::Event* event = rf::event_lookup_from_uid(kf->event_uid);
        if (!event) {
            continue;
        }
        if (event->event_type == std::to_underlying(rf::EventType::Capture_Point_Handler)) {
            xlog::warn("[Payload] Keyframe {} names Capture_Point_Handler {}; a checkpoint is a Payload_Checkpoint, so "
                       "it is ignored", i, event->uid);
            continue;
        }
        if (event->event_type != std::to_underlying(rf::EventType::Payload_Checkpoint)) {
            // Keyframe 0 is where the cart starts, so it is never passed
            if (i > 0) {
                g_payload.pass_events.push_back({g_payload.keyframe_distance[i], kf->event_uid});
            }
            continue;
        }
        auto* handler = static_cast<EventPayloadCheckpoint*>(event);
        if (!(g_payload.keyframe_distance[i] > 0.0f)) {
            xlog::warn("[Payload] Keyframe {} names checkpoint {}, but the cart starts there; ignored", i,
                       handler->uid);
            continue;
        }
        const bool repeated = std::any_of(g_payload.checkpoints.begin(), g_payload.checkpoints.end(),
            [handler](const PayloadCheckpoint& cp) { return cp.handler == handler; });
        if (repeated) {
            xlog::warn("[Payload] Checkpoint {} is named by more than one keyframe; keyframe {} ignored",
                       handler->uid, i);
            continue;
        }
        if (static_cast<int>(g_payload.checkpoints.size()) >= max_hills) {
            xlog::warn("[Payload] Checkpoint {} on keyframe {} is past the {} a level can have; ignored", handler->uid,
                       i, max_hills);
            continue;
        }
        const float level_bonus = AlpineLevelProperties::instance().payload_rules.checkpoint_time_bonus;
        g_payload.checkpoints.push_back({i, g_payload.keyframe_distance[i], kf->pos, handler,
                                         payload::checkpoint_hold(handler->hold_seconds),
                                         payload::checkpoint_time_bonus(handler->time_bonus, level_bonus)});
    }
    if (g_payload.checkpoints.empty()) {
        xlog::warn("[Payload] The cart has no checkpoints: no keyframe's event is a Payload_Checkpoint");
    }
}

void find_cart_center(const rf::Mover& mover)
{
    constexpr float big = std::numeric_limits<float>::max();
    rf::Vector3 lo{big, big, big};
    rf::Vector3 hi{-big, -big, -big};
    bool any = false;
    for (const int handle : mover.brush_handles) {
        rf::Object* obj = rf::obj_from_handle(handle);
        if (!obj || obj->type != rf::OT_MOVER_BRUSH) {
            continue;
        }
        const rf::GSolid* solid = static_cast<rf::MoverBrush*>(obj)->geometry;
        if (!solid) {
            continue;
        }
        // A moving brush keeps its bounds in its own frame
        const rf::Vector3 local_center = (solid->bbox_min + solid->bbox_max) * 0.5f;
        const rf::Vector3 half = (solid->bbox_max - solid->bbox_min) * 0.5f;
        const rf::Matrix3& m = obj->orient;
        const rf::Vector3 center = obj->pos + m.transform_vector(local_center);
        const rf::Vector3 world_half{
            std::abs(m.rvec.x) * half.x + std::abs(m.uvec.x) * half.y + std::abs(m.fvec.x) * half.z,
            std::abs(m.rvec.y) * half.x + std::abs(m.uvec.y) * half.y + std::abs(m.fvec.y) * half.z,
            std::abs(m.rvec.z) * half.x + std::abs(m.uvec.z) * half.y + std::abs(m.fvec.z) * half.z,
        };
        lo = {std::min(lo.x, center.x - world_half.x), std::min(lo.y, center.y - world_half.y),
              std::min(lo.z, center.z - world_half.z)};
        hi = {std::max(hi.x, center.x + world_half.x), std::max(hi.y, center.y + world_half.y),
              std::max(hi.z, center.z + world_half.z)};
        if (!any) {
            g_payload.center_object_handle = handle;
            any = true;
        }
    }

    rf::Vector3 center = mover.keyframes[0]->pos;
    if (any) {
        center = (lo + hi) * 0.5f;
        g_payload.cart_half_height = 0.5f * (hi.y - lo.y);
    }
    else if (mover.object_handles.size() > 0) {
        if (rf::Object* obj = rf::obj_from_handle(mover.object_handles[0])) {
            g_payload.center_object_handle = obj->handle;
            center = obj->pos;
            g_payload.cart_half_height = obj->radius;
        }
    }
    if (rf::Object* ref = rf::obj_from_handle(g_payload.center_object_handle)) {
        const rf::Vector3 d = center - ref->pos;
        g_payload.center_local = {d.dot_prod(ref->orient.rvec), d.dot_prod(ref->orient.uvec),
                                  d.dot_prod(ref->orient.fvec)};
    }
    g_payload.center_offset = center - mover.keyframes[0]->pos;
}

bool in_push_zone(const rf::Object& obj, const rf::Vector3& center)
{
    const rf::Vector3 d = obj.pos - center;
    const float r = g_payload.push_radius;
    return std::abs(d.y) <= r + g_payload.cart_half_height && d.x * d.x + d.z * d.z <= r * r;
}

void notify_teams(std::string_view attackers, std::string_view defenders, int seconds, HudNotificationType type)
{
    for (rf::Player& pl : SinglyLinkedList{rf::player_list}) {
        const std::string_view text = pl.team == g_payload.attacking_team ? attackers : defenders;
        af_send_hud_notification(text, seconds, static_cast<int>(type), true, &pl);
    }
}

void notify_all(std::string_view text, int seconds, HudNotificationType type)
{
    af_broadcast_hud_notification(text, seconds, static_cast<int>(type), true);
}

void greet(rf::Player& player)
{
    const bool attacking = player.team == g_payload.attacking_team;
    af_send_hud_notification(attacking ? "Push the cart to the end!" : "Stop the cart!", 5,
                             static_cast<int>(HudNotificationType::Generic), true, &player);
}

void play_finale_effect()
{
    const auto center = payload_cart_center();
    const int vclip = rf::vclip_lookup(finale_vclip);
    if (!center || vclip < 0) {
        return;
    }
    rf::Vector3 pos = *center;
    rf::vclip_play_3d(vclip, nullptr, nullptr, &pos, finale_vclip_radius, -1, nullptr, true);
}

void damage_near_finale(rf::Vector3 center)
{
    for (rf::Player& pl : SinglyLinkedList{rf::player_list}) {
        if (!player_is_countable(pl)) {
            continue;
        }
        const rf::Entity* ep = rf::entity_from_handle(pl.entity_handle);
        const float dist = (ep->pos - center).len();
        if (dist >= finale_damage_radius) {
            continue;
        }
        const float falloff = dist <= finale_full_damage_radius
            ? 1.0f
            : 1.0f - (dist - finale_full_damage_radius) / (finale_damage_radius - finale_full_damage_radius);
        rf::obj_damage(ep->handle, finale_max_damage * falloff, -1, -1, rf::DT_EXPLOSIVE, &center, -1, 0);
    }
}

void fire_pass_events()
{
    if (g_payload.distance <= g_payload.furthest) {
        return;
    }
    for (const KeyframeEvent& ke : g_payload.pass_events) {
        if (ke.distance > g_payload.furthest && ke.distance <= g_payload.distance) {
            if (rf::Event* event = rf::event_lookup_from_uid(ke.event_uid)) {
                rf::event_activate_from_trigger(event->handle, g_payload.cart_handle, -1);
            }
        }
    }
    g_payload.furthest = g_payload.distance;
}

void capture_checkpoint(const PayloadCheckpoint& cp, bool at_end)
{
    if (!payload_checkpoint_captured(cp)) {
        koth_set_capture_point_owner(cp.handler, static_cast<int>(team_hill_owner(g_payload.attacking_team)));
        auto& score = g_payload.attacking_team == 0 ? g_koth_info.red_team_score : g_koth_info.blue_team_score;
        ++score;
        rf::send_team_scores_packet(nullptr);
    }
    if (at_end) {
        return;
    }
    g_payload.hold_left = std::max(g_payload.hold_left, cp.hold_seconds);

    const auto& server_bonus = g_alpine_server_config_active_rules.payload.checkpoint_time_bonus;
    const float bonus = server_bonus ? *server_bonus : cp.time_bonus;
    std::string added;
    if (rf::multi_time_limit > 0.0f && bonus > 0.0f) {
        if (!g_time_bonus || g_time_bonus->limit != rf::multi_time_limit) {
            g_time_bonus = TimeBonus{rf::multi_time_limit};
        }
        // From now if payload overtime ran the clock out
        rf::multi_time_limit = std::max(rf::multi_time_limit, rf::level.time) + bonus;
        g_time_bonus->limit = rf::multi_time_limit;
        const int seconds = static_cast<int>(std::lround(bonus));
        added = std::format(" +{}:{:02}", seconds / 60, seconds % 60);
    }
    const HillInfo* hill = checkpoint_hill(cp);
    const std::string name = hill ? hill->name : std::string{"Checkpoint"};
    notify_teams(std::format("{} captured!{}", name, added), std::format("{} lost!{}", name, added), 4,
                 HudNotificationType::Generic);
}

// Each side fires it for its own world, as movers aren't synced. A late joiner fires it on its first state, so the
// doors it opens are open for the joiner too.
void end_setup()
{
    g_payload.setup = false;
    g_payload.setup_end_time = rf::level.time;
    rf::activate_all_events_of_type(rf::EventType::When_Setup_Ends, -1, -1, true);
}

void begin_finale()
{
    g_payload.state = PayloadCartState::finale;
    g_payload.distance = g_payload.total_length;
    g_payload.speed = 0.0f;
    g_payload.countdown.reset();
    if (g_payload.finale) {
        if (const auto center = payload_cart_center()) {
            damage_near_finale(*center);
        }
        if (!rf::is_dedicated_server) {
            play_finale_effect();
        }
    }
    g_payload.finale_end.set(finale_delay_ms);
    const int seconds = static_cast<int>(std::lround(std::max(rf::level.time - g_payload.setup_end_time, 0.0f)));
    const std::string text = std::format("The cart reached the end in {:02}:{:02}! {} wins!", seconds / 60,
                                         seconds % 60, team_name(g_payload.attacking_team));
    notify_all(text, 5, HudNotificationType::GenericBig);
    g_payload.end_announced = true;
}

void sample_push_zone(std::vector<rf::Player*>& attackers, std::vector<rf::Player*>& defenders)
{
    const auto center = payload_cart_center();
    if (!center) {
        return;
    }
    for (rf::Player& pl : SinglyLinkedList{rf::player_list}) {
        if (!player_is_countable(pl)) {
            continue;
        }
        const rf::Entity* ep = rf::entity_from_handle(pl.entity_handle);
        if (!in_push_zone(*ep, *center)) {
            continue;
        }
        (pl.team == g_payload.attacking_team ? attackers : defenders).push_back(&pl);
    }
}

std::vector<uint8_t> player_ids(const std::vector<rf::Player*>& players)
{
    std::vector<uint8_t> ids;
    ids.reserve(players.size());
    for (const rf::Player* pl : players) {
        if (pl->net_data) {
            ids.push_back(pl->net_data->player_id);
        }
    }
    return ids;
}

// A share of the held weapon's magazine into its reserve, as far as the reserve has room. Weapons without a magazine
// have no reserve apart from what they fire.
void resupply_held_weapon(rf::Player& player)
{
    rf::Entity* ep = rf::entity_from_handle(player.entity_handle);
    if (!ep || ep->life <= 0.0f) {
        return;
    }
    const int weapon_type = ep->ai.current_primary_weapon;
    if (weapon_type < 0 || weapon_type >= rf::num_weapon_types || rf::weapon_is_melee(weapon_type)) {
        return;
    }
    const rf::WeaponInfo& info = rf::weapon_types[weapon_type];
    if (!rf::weapon_uses_clip(weapon_type) || info.ammo_type < 0
        || info.ammo_type >= static_cast<int>(std::size(ep->ai.ammo))) {
        return;
    }
    ResupplyOwed& owed = g_payload.resupply_owed[&player];
    if (owed.entity_handle != ep->handle || owed.weapon_type != weapon_type) {
        owed = {ep->handle, weapon_type, 0.0f};
    }
    int& reserve = ep->ai.ammo[info.ammo_type];
    const int room = info.max_ammo - reserve;
    if (room <= 0) {
        // Nothing builds up while the reserve is full
        owed.rounds = 0.0f;
        return;
    }
    owed.rounds += static_cast<float>(info.clip_size) * resupply_magazine_share;
    const int added = std::min(static_cast<int>(owed.rounds), room);
    if (added <= 0) {
        return;
    }
    // What a full reserve cut off is dropped, so a reload before the next tick can't release it all at once
    owed.rounds = added == room ? 0.0f : owed.rounds - static_cast<float>(added);
    reserve += added;
    af_send_reserve_ammo(&player, weapon_type, reserve);
}

void heal_pushers(const std::vector<rf::Player*>& attackers)
{
    if (!payload_heals_now()) {
        return;
    }
    if (g_payload.heal_tick.valid() && !g_payload.heal_tick.elapsed()) {
        return;
    }
    g_payload.heal_tick.set(payload::heal_tick_ms);
    const float amount = payload_rules().heal_per_second;
    bool healed = false;
    for (rf::Player* pl : attackers) {
        healed |= apply_effective_health_reward(pl, amount);
        resupply_held_weapon(*pl);
    }
    if (healed) {
        ++g_payload.heal_serial;
    }
}

bool timer_due(const rf::Timestamp& timer)
{
    return !timer.valid() || timer.elapsed();
}

void maybe_broadcast()
{
    PayloadNetState now = payload_build_net_state();
    const auto& last = g_payload.last_reliable;
    const bool state_changed = !last || now.state != last->state || now.overtime != last->overtime
        || now.heals != last->heals || now.setup != last->setup || now.next_checkpoint != last->next_checkpoint
        || now.countdown_seconds.has_value() != last->countdown_seconds.has_value()
        || (now.speed == 0.0f) != (last->speed == 0.0f);
    const bool lists_due =
        last && (now.pushers != last->pushers || now.blockers != last->blockers) && timer_due(g_payload.list_resend);
    const bool reliable = state_changed || lists_due;
    const bool healed = now.heal_serial != g_payload.sent_heal_serial;
    const bool refresh = now.speed != 0.0f && timer_due(g_payload.moving_refresh);
    if (!reliable && !healed && !refresh) {
        return;
    }
    ++g_payload.net_sequence;
    af_send_payload_state_packet_to_all(reliable);
    g_payload.moving_refresh.set(moving_refresh_ms);
    g_payload.sent_heal_serial = now.heal_serial;
    if (reliable) {
        g_payload.list_resend.set(list_resend_min_ms);
        g_payload.last_reliable = std::move(now);
    }
}

void server_tick(float dt)
{
    if (g_payload.state == PayloadCartState::finale) {
        maybe_broadcast();
        return;
    }

    const payload::LevelRules rules = payload_rules();
    std::vector<rf::Player*> attackers;
    std::vector<rf::Player*> defenders;
    sample_push_zone(attackers, defenders);
    g_payload.pushers = player_ids(attackers);
    g_payload.blockers = player_ids(defenders);
    if (!g_payload.pushers.empty()) {
        g_payload.last_pushers = g_payload.pushers;
        g_payload.capture_credit.set(capture_credit_ms);
    }

    const float floor = rollback_floor();
    float target = 0.0f;
    g_payload.countdown.reset();
    if (g_payload.setup && g_payload.hold_left <= 0.0f) {
        end_setup();
    }
    const bool held = g_payload.hold_left > 0.0f;
    if (held) {
        // It stays put whoever is in the zone, and the rollback countdown starts afresh once the hold ends
        g_payload.state = PayloadCartState::held;
        g_payload.hold_left = std::max(0.0f, g_payload.hold_left - dt);
        g_payload.countdown = g_payload.hold_left;
        g_payload.speed = 0.0f;
        g_payload.unpushed_seconds = 0.0f;
        g_payload.rollback_announced = false;
    }
    else if (!attackers.empty()) {
        g_payload.unpushed_seconds = 0.0f;
        g_payload.rollback_announced = false;
        // Any defender in the zone blocks the cart; it still counts as pushed
        if (defenders.empty()) {
            g_payload.state = PayloadCartState::pushing;
            target = rules.push_speed * payload::push_multiplier(static_cast<int>(attackers.size()));
        }
        else {
            g_payload.state = PayloadCartState::blocked;
        }
    }
    else {
        g_payload.unpushed_seconds += dt;
        const bool can_roll_back = g_payload.rollback_delay >= 0 && g_payload.distance > floor;
        if (can_roll_back && g_payload.unpushed_seconds >= static_cast<float>(g_payload.rollback_delay)) {
            g_payload.state = PayloadCartState::rolling_back;
            target = -rules.rollback_speed * rules.push_speed;
            if (!g_payload.rollback_announced) {
                g_payload.rollback_announced = true;
                notify_all("The cart is rolling back!", 3, HudNotificationType::Generic);
            }
        }
        else {
            g_payload.state = PayloadCartState::idle;
            if (can_roll_back) {
                g_payload.countdown = static_cast<float>(g_payload.rollback_delay) - g_payload.unpushed_seconds;
            }
        }
    }

    // Eased so riders and client prediction never see a jump in speed
    const float step = rules.cart_accel * dt;
    g_payload.speed += std::clamp(target - g_payload.speed, -step, step);
    g_payload.distance += g_payload.speed * dt;
    if (g_payload.distance <= floor) {
        g_payload.distance = floor;
        g_payload.speed = std::max(g_payload.speed, 0.0f);
    }
    if (g_payload.distance >= g_payload.total_length) {
        g_payload.distance = g_payload.total_length;
        g_payload.speed = 0.0f;
    }

    const int count = static_cast<int>(g_payload.checkpoints.size());
    while (g_payload.next_checkpoint < count
           && g_payload.distance >= g_payload.checkpoints[g_payload.next_checkpoint].distance) {
        const PayloadCheckpoint& cp = g_payload.checkpoints[g_payload.next_checkpoint];
        ++g_payload.next_checkpoint;
        capture_checkpoint(cp, cp.distance >= g_payload.total_length);
    }

    if (!g_payload.final_stretch_reached && g_payload.distance >= final_stretch_start()
        && g_payload.distance < g_payload.total_length) {
        g_payload.final_stretch_reached = true;
        notify_all("The cart is nearly there!", 3, HudNotificationType::Generic);
    }

    if (g_payload.overtime) {
        const bool time_up = rf::multi_time_limit > 0.0f && rf::level.time >= rf::multi_time_limit;
        if (!time_up) {
            g_payload.overtime = false;
        }
        // A held cart keeps overtime going as a pushed one does: the attackers can't push it
        g_payload.overtime_unpushed_seconds =
            g_payload.hold_left <= 0.0f && attackers.empty() ? g_payload.overtime_unpushed_seconds + dt : 0.0f;
    }

    if (g_payload.distance >= g_payload.total_length) {
        begin_finale();
    }
    else {
        heal_pushers(attackers);
    }
    maybe_broadcast();
}

void client_tick(float dt)
{
    if (!g_payload.net_seeded) {
        return;
    }
    const float floor = rollback_floor();
    g_payload.net_age += dt;
    const float target = std::clamp(
        g_payload.net_distance + g_payload.net_speed * std::min(g_payload.net_age, net_max_extrapolation), floor,
        g_payload.total_length);
    g_payload.speed = g_payload.net_speed;
    g_payload.distance = std::clamp(g_payload.distance + g_payload.speed * dt, floor, g_payload.total_length);
    const float error = target - g_payload.distance;
    if (std::abs(error) > net_snap_distance) {
        g_payload.distance = target;
    }
    else {
        const float max_step = net_max_correction_speed * dt;
        g_payload.distance += std::clamp(error * std::min(1.0f, net_blend_rate * dt), -max_step, max_step);
    }
    if (g_payload.countdown) {
        g_payload.countdown = std::max(0.0f, *g_payload.countdown - dt);
    }
    if (g_payload.setup && !g_payload.net_setup) {
        end_setup();
    }
}

void load_cue_sounds()
{
    PayloadCues& cues = g_payload.cues;
    cues.setup_end_sound = rf::snd_get_handle(setup_end_sound_file, 10.0f, 1.0f, 1.0f);
    cues.rollback_sound = rf::snd_get_handle(rollback_sound_file, 10.0f, 1.0f, 1.0f);
    cues.final_stretch_sound = rf::snd_get_handle(final_stretch_sound_file, 10.0f, 1.0f, 1.0f);
}

void play_cue(int sound)
{
    if (sound >= 0) {
        rf::snd_play(sound, 0, 0.0f, 1.0f);
    }
}

// Changes are judged against the last frame, so a late joiner's first state plays nothing
void play_cues()
{
    if (!rf::local_player || (!rf::is_server && !g_payload.net_seeded)) {
        return;
    }
    PayloadCues& cues = g_payload.cues;
    const PayloadCartState state = g_payload.state;
    const std::optional<int> setup_count = g_payload.setup ? payload_countdown_whole_seconds() : std::nullopt;
    // Only a lower count is new, as a late state can wind the count back up by a fraction of a second
    const bool new_setup_count = setup_count && (!cues.setup_count || *setup_count < *cues.setup_count);
    if (cues.seeded) {
        if (new_setup_count && *setup_count <= setup_count_from) {
            play_cue(stock_sound_id::ann_one + *setup_count - 1);
        }
        if (cues.setup && !g_payload.setup) {
            play_cue(cues.setup_end_sound);
        }

        // Team cues follow the local player's own team, so a spectator gets none
        const bool on_team = !multi_spectate_is_spectating();
        const bool attacker = rf::local_player->team == g_payload.attacking_team;
        if (on_team && attacker && state == PayloadCartState::rolling_back && cues.state != state) {
            play_cue(cues.rollback_sound);
        }
        const bool final_push = on_team && !attacker && state == PayloadCartState::pushing
            && g_payload.distance >= final_stretch_start();
        if (final_push && timer_due(cues.final_stretch_alarm)) {
            play_cue(cues.final_stretch_sound);
            cues.final_stretch_alarm.set(final_stretch_alarm_ms);
        }
        // Overtime ends once its grace runs out with no attacker at the cart
        const bool overtime_running_out = g_payload.overtime && g_payload.pushers.empty()
            && state != PayloadCartState::held && state != PayloadCartState::finale;
        if (overtime_running_out && timer_due(cues.overtime_tick)) {
            play_cue(stock_sound_id::bomb_tick);
            cues.overtime_tick.set(overtime_tick_ms);
        }
    }
    cues.seeded = true;
    cues.state = state;
    cues.setup = g_payload.setup;
    if (new_setup_count || !setup_count) {
        cues.setup_count = setup_count;
    }
}

} // namespace

bool payload_active()
{
    return g_payload.active && rf::is_multi && gt_is_payload();
}

int payload_attacking_team()
{
    return g_payload.attacking_team;
}

int payload_defending_team()
{
    return 1 - g_payload.attacking_team;
}

rf::Mover* payload_cart_mover()
{
    return payload_active() ? cart() : nullptr;
}

std::optional<rf::Vector3> payload_cart_center()
{
    if (!payload_active()) {
        return std::nullopt;
    }
    const rf::Object* ref = rf::obj_from_handle(g_payload.center_object_handle);
    if (!ref) {
        return std::nullopt;
    }
    return ref->pos + ref->orient.transform_vector(g_payload.center_local);
}

const std::string& payload_cart_name()
{
    return g_payload.name;
}

float payload_push_radius()
{
    return g_payload.push_radius;
}

float payload_cart_half_height()
{
    return g_payload.cart_half_height;
}

bool payload_heals()
{
    if (!rf::is_server) {
        return g_payload.heals;
    }
    return g_payload.heals && payload_rules().heal_per_second > 0.0f;
}

std::optional<uint8_t> payload_heal_serial()
{
    if (!rf::is_server && !g_payload.net_seeded) {
        return std::nullopt;
    }
    return g_payload.heal_serial;
}

float payload_distance()
{
    return g_payload.distance;
}

float payload_total_length()
{
    return g_payload.total_length;
}

PayloadCartState payload_state()
{
    return g_payload.state;
}

std::optional<int> payload_countdown_whole_seconds()
{
    if (!g_payload.countdown) {
        return std::nullopt;
    }
    return std::max(1, static_cast<int>(std::ceil(*g_payload.countdown)));
}

bool payload_defenders_have_cart()
{
    return g_payload.state == PayloadCartState::blocked || g_payload.state == PayloadCartState::rolling_back;
}

bool payload_heals_now()
{
    return payload_heals() && !payload_in_setup();
}

bool payload_in_setup()
{
    return payload_active() && g_payload.setup;
}

bool payload_in_overtime()
{
    return g_payload.overtime;
}

std::optional<float> payload_countdown_seconds()
{
    return g_payload.countdown;
}

const std::vector<PayloadCheckpoint>& payload_checkpoints()
{
    return g_payload.checkpoints;
}

int payload_next_checkpoint_index()
{
    return g_payload.next_checkpoint;
}

float payload_next_checkpoint_progress()
{
    const auto& cps = g_payload.checkpoints;
    const int count = static_cast<int>(cps.size());
    const int next = std::clamp(g_payload.next_checkpoint, 0, count);
    const float from = next > 0 ? cps[next - 1].distance : 0.0f;
    const float to = next < count ? cps[next].distance : g_payload.total_length;
    const float span = to - from;
    return span > 0.0f ? std::clamp((payload_distance() - from) / span, 0.0f, 1.0f) : 1.0f;
}

bool payload_checkpoint_captured(const PayloadCheckpoint& checkpoint)
{
    const HillInfo* hill = checkpoint_hill(checkpoint);
    return hill && hill->ownership == team_hill_owner(g_payload.attacking_team);
}

std::optional<rf::Vector3> payload_checkpoint_pos(const HillInfo& hill)
{
    if (!payload_active()) {
        return std::nullopt;
    }
    for (const PayloadCheckpoint& cp : g_payload.checkpoints) {
        if (cp.handler == hill.handler) {
            return payload_track_point(cp.distance);
        }
    }
    return std::nullopt;
}

const std::vector<uint8_t>& payload_pusher_ids()
{
    return g_payload.pushers;
}

int payload_useful_pushers()
{
    return std::min(static_cast<int>(g_payload.pushers.size()), payload::max_useful_pushers);
}

const std::vector<uint8_t>& payload_blocker_ids()
{
    return g_payload.blockers;
}

rf::Vector3 payload_track_point(float distance)
{
    rf::Mover* mover = payload_cart_mover();
    if (!mover) {
        return {};
    }
    const PayloadCartPlacement at = placement_at(std::clamp(distance, 0.0f, g_payload.total_length));
    return alpine_mover_segment_point(mover, at.segment, at.fraction)
        + alpine_mover_turned_offset(mover, at.segment, at.fraction, g_payload.center_offset);
}

int payload_winning_team()
{
    if (!payload_active()) {
        return -1;
    }
    return g_payload.state == PayloadCartState::finale ? g_payload.attacking_team : payload_defending_team();
}

void payload_level_init()
{
    g_payload = PayloadInfo{};
}

void payload_level_close()
{
    // Unless something else has set the limit since
    if (g_time_bonus && rf::multi_time_limit == g_time_bonus->limit) {
        rf::multi_time_limit = g_time_bonus->base_limit;
    }
    g_time_bonus.reset();
    g_payload = PayloadInfo{};
}

void payload_level_init_post()
{
    if (!rf::is_multi || !gt_is_payload()) {
        return;
    }
    const auto handlers = rf::find_all_events_by_type(rf::EventType::Payload_Handler);
    if (handlers.empty()) {
        xlog::warn("[Payload] The level has no Payload_Handler event, so it has no cart");
        return;
    }
    if (handlers.size() > 1) {
        xlog::warn("[Payload] The level has {} Payload_Handler events; only {} is used", handlers.size(),
                   handlers.front()->uid);
    }
    const auto* handler = static_cast<const EventPayloadHandler*>(handlers.front());
    rf::Mover* mover = find_linked_mover(*handler);
    if (!mover) {
        xlog::warn("[Payload] Payload_Handler {} links to no mover, so the level has no cart", handler->uid);
        return;
    }
    if (!validate_cart(*mover, handler->uid)) {
        return;
    }

    alpine_mover_path_ensure_init();
    g_payload.cart_handle = mover->handle;
    g_payload.name = payload::name(handler->name.c_str());
    g_payload.attacking_team = payload::attacking_team(handler->attacking_team);
    g_payload.push_radius = payload::push_radius(handler->push_radius);
    g_payload.rollback_delay = payload::rollback_delay(handler->rollback_delay);
    g_payload.heals = payload::heals(handler->no_healing);
    g_payload.finale = payload::has_finale(handler->no_finale);
    build_track(*mover);
    if (!(g_payload.total_length > 0.0f)) {
        xlog::warn("[Payload] The cart's keyframes are all in one place, so the level has no cart");
        g_payload = PayloadInfo{};
        return;
    }
    find_cart_center(*mover);
    if (g_payload.center_object_handle < 0) {
        xlog::warn("[Payload] The cart has no brushes or objects, so the level has no cart");
        g_payload = PayloadInfo{};
        return;
    }
    g_payload.active = true;
    if (!rf::is_dedicated_server) {
        load_cue_sounds();
    }
    if (rf::is_server) {
        // Held as a checkpoint holds it, so the teams can take their places
        g_payload.hold_left = g_alpine_server_config_active_rules.payload.setup_time;
    }
    xlog::info("[Payload] Cart {}: {} keyframes, {:.1f} m of track, {} checkpoints, {} attacks", mover->uid,
               mover->keyframes.size(), g_payload.total_length, g_payload.checkpoints.size(),
               team_name(g_payload.attacking_team));
}

void payload_do_frame()
{
    // In multiplayer the level keeps running beneath menus, so the cart must too, or its riders are left behind
    if (!payload_active() || !(rf::level.flags & rf::LEVEL_LOADED) || !gameseq_is_stacked(rf::GS_GAMEPLAY)) {
        return;
    }
    const float dt = std::clamp(rf::frametime, 0.0f, max_frame_seconds);
    if (rf::is_server) {
        server_tick(dt);
    }
    else {
        client_tick(dt);
    }
    fire_pass_events();
    if (!rf::is_dedicated_server) {
        play_cues();
    }
}

bool payload_round_over(bool time_up)
{
    if (!payload_active()) {
        return time_up;
    }
    if (g_payload.state == PayloadCartState::finale) {
        return g_payload.finale_end.valid() && g_payload.finale_end.elapsed();
    }
    if (!time_up) {
        return false;
    }
    // A held cart counts as pushed, since the attackers can't push it. The hold is set on the capture tick, a tick
    // before the state follows.
    const bool pushed = g_payload.hold_left > 0.0f || !g_payload.pushers.empty();
    if (!g_payload.overtime && pushed) {
        g_payload.overtime = true;
        g_payload.overtime_unpushed_seconds = 0.0f;
        notify_teams("OVERTIME: keep pushing!", "OVERTIME: stop the cart!", 4, HudNotificationType::GenericBig);
        return false;
    }
    if (g_payload.overtime && (pushed || g_payload.overtime_unpushed_seconds < payload_rules().overtime_grace)) {
        return false;
    }
    if (!g_payload.end_announced) {
        g_payload.end_announced = true;
        const std::string text = std::format("The cart was stopped! {} wins!", team_name(payload_defending_team()));
        notify_all(text, 5, HudNotificationType::GenericBig);
    }
    return true;
}

bool payload_finale_reached()
{
    return payload_active() && g_payload.state == PayloadCartState::finale;
}

bool payload_player_in_push_zone(const rf::Player& player)
{
    const auto center = payload_cart_center();
    const rf::Entity* ep = rf::entity_from_handle(player.entity_handle);
    return center && ep && in_push_zone(*ep, *center);
}

bool payload_player_credited_for_capture(const rf::Player& player)
{
    if (payload_player_in_push_zone(player)) {
        return true;
    }
    const bool coasting = g_payload.pushers.empty() && g_payload.capture_credit.valid()
        && !g_payload.capture_credit.elapsed();
    return coasting && player.net_data
        && std::find(g_payload.last_pushers.begin(), g_payload.last_pushers.end(), player.net_data->player_id)
            != g_payload.last_pushers.end();
}

std::vector<rf::Event*> payload_checkpoint_handlers()
{
    std::vector<rf::Event*> handlers;
    handlers.reserve(g_payload.checkpoints.size());
    for (const PayloadCheckpoint& cp : g_payload.checkpoints) {
        handlers.push_back(cp.handler);
    }
    return handlers;
}

std::optional<PayloadCartPlacement> payload_cart_placement(const rf::Mover* mp)
{
    if (!mp || mp->handle != g_payload.cart_handle || !payload_active()) {
        return std::nullopt;
    }
    return placement_at(g_payload.distance);
}

bool payload_is_cart(int mover_handle)
{
    return mover_handle == g_payload.cart_handle && payload_active();
}

void payload_force_state_sync_to(rf::Player* player)
{
    if (!rf::is_server || !player || !payload_active()) {
        return;
    }
    af_send_payload_state_packet(player);
    if (g_payload.state != PayloadCartState::finale) {
        greet(*player);
    }
}

void payload_greet_listen_host()
{
    if (rf::is_server && !rf::is_dedicated_server && rf::local_player && payload_active()) {
        greet(*rf::local_player);
    }
}

PayloadNetState payload_build_net_state()
{
    PayloadNetState net;
    net.sequence = g_payload.net_sequence;
    net.state = g_payload.state;
    net.overtime = g_payload.overtime;
    net.heals = payload_heals();
    net.setup = g_payload.setup;
    net.heal_serial = g_payload.heal_serial;
    net.next_checkpoint = static_cast<uint8_t>(std::clamp(g_payload.next_checkpoint, 0, 255));
    net.countdown_seconds = g_payload.countdown;
    net.distance = g_payload.distance;
    net.speed = g_payload.speed;
    net.pushers = g_payload.pushers;
    net.blockers = g_payload.blockers;
    return net;
}

void payload_apply_net_state(const PayloadNetState& net)
{
    if (rf::is_server || !payload_active()) {
        return;
    }
    const bool first = !g_payload.net_seeded;
    // Unreliable refreshes can arrive out of order
    if (!first && static_cast<int8_t>(net.sequence - g_payload.net_sequence) <= 0) {
        return;
    }
    g_payload.net_sequence = net.sequence;
    const bool finale_began = g_payload.state != PayloadCartState::finale && net.state == PayloadCartState::finale;
    g_payload.state = net.state;
    g_payload.overtime = net.overtime;
    g_payload.heals = net.heals;
    g_payload.net_setup = net.setup;
    g_payload.heal_serial = net.heal_serial;
    g_payload.next_checkpoint = std::min<int>(net.next_checkpoint, static_cast<int>(g_payload.checkpoints.size()));
    g_payload.countdown = net.countdown_seconds;
    g_payload.pushers = net.pushers;
    g_payload.blockers = net.blockers;
    g_payload.net_distance = std::clamp(net.distance, 0.0f, g_payload.total_length);
    g_payload.net_speed = g_payload.state == PayloadCartState::finale ? 0.0f : net.speed;
    g_payload.net_age = 0.0f;
    if (first) {
        // A late joiner takes the world as it is: what the cart already passed has already fired
        g_payload.distance = g_payload.net_distance;
        g_payload.furthest = g_payload.distance;
        g_payload.net_seeded = true;
    }
    if (finale_began && !first && g_payload.finale) {
        g_payload.distance = g_payload.net_distance;
        play_finale_effect();
    }
}

payload::LevelRules payload_rules()
{
    payload::LevelRules rules = AlpineLevelProperties::instance().payload_rules;
    if (rf::is_server) {
        const PayloadConfig& cfg = g_alpine_server_config_active_rules.payload;
        rules.checkpoint_time_bonus = cfg.checkpoint_time_bonus.value_or(rules.checkpoint_time_bonus);
        rules.heal_per_second = cfg.heal_per_tick.value_or(rules.heal_per_second);
        rules.overtime_grace = cfg.overtime_grace.value_or(rules.overtime_grace);
        rules.attacker_respawn = cfg.attacker_respawn.applied_to(rules.attacker_respawn);
        rules.defender_respawn = cfg.defender_respawn.applied_to(rules.defender_respawn);
    }
    return rules;
}

int payload_respawn_extra_ms(int team)
{
    if (!payload_active()) {
        return 0;
    }
    const payload::LevelRules rules = payload_rules();
    const payload::RespawnExtras& extras =
        team == g_payload.attacking_team ? rules.attacker_respawn : rules.defender_respawn;
    float seconds = extras.base + extras.per_checkpoint * static_cast<float>(g_payload.next_checkpoint);
    if (g_payload.final_stretch_reached) {
        seconds += extras.final_stretch;
    }
    return static_cast<int>(std::lround(seconds * 1000.0f));
}
