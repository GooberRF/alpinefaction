#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <random>
#include <vector>
#include <xlog/xlog.h>
#include <common/utils/string-utils.h>
#include "scavenger.h"
#include "salvage.h"
#include "gametype.h"
#include "multi.h"
#include "server_internal.h"
#include "alpine_packets.h"
#include "../main/main.h"
#include "../misc/waypoints.h"
#include "../rf/entity.h"
#include "../rf/gameseq.h"
#include "../rf/gr/gr_light.h"
#include "../rf/item.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"

namespace
{

constexpr int kScavStateRefreshIntervalMs = 1000;
// A zone is rolled no closer than the capture radius plus this to the objective, while candidates allow.
constexpr float kScavZoneMinObjectiveDist = 5.0f;
// Further below the zone than this there is no floor to it, and it stays at its item's height.
constexpr float kScavZoneFloorTraceDist = 3.0f;
// Carrier origin heights above the zone floor that count as inside the zone.
constexpr float kScavZoneCaptureMinHeight = -0.5f;
constexpr float kScavZoneCaptureMaxHeight = 2.5f;
constexpr float kScavSpawnObjectiveFullWeightDist = 40.0f;
constexpr float kScavSpawnEnemyFullWeightDist = 30.0f;
constexpr float kScavSpawnMinWeight = 0.05f;
constexpr float kScavZoneLightHeight = 0.4f;
constexpr float kScavZoneLightRadius = 1.5f;

// Level-placed item classes whose positions never host the objective or the zone.
constexpr const char* const kScavExcludedCandidateClasses[] = {
    "base_red",
    "base_blue",
    "CTF Banner Red",
    "CTF Banner Blue",
    "Brainstem",
    "keycard",
    "Demo_K000",
};

struct ScavCandidate
{
    rf::Vector3 pos;
    rf::Matrix3 orient;
};

struct ScavengerInfo
{
    bool active = false;
    std::vector<ScavCandidate> candidates; // server-side only
    int objective_index = -1;
    int zone_index = -1;
    bool zone_known = false;
    rf::Vector3 zone_pos{};
    float zone_radius = 0.0f;
    float zone_floor_y = 0.0f;
    rf::Timestamp zone_timer;
    rf::Timestamp refresh_timer;
    rf::Vector3 captured_pos{};
    float captured_radius = 0.0f;
    float captured_floor_y = 0.0f;
    int light_handle = -1;
    float light_pulse_phase = 0.0f;
};

ScavengerInfo g_scav_info;
// Session-monotonic, so a renderer comparing against its last seen value also catches a new level.
uint32_t g_zone_change_serial = 0;
uint32_t g_zone_capture_serial = 0;

bool is_excluded_candidate_class(const rf::Item& item)
{
    if (!item.info) return true;
    const char* cls = item.info->cls_name.c_str();
    return std::any_of(std::begin(kScavExcludedCandidateClasses), std::end(kScavExcludedCandidateClasses),
        [cls](const char* excluded) { return string_iequals(cls, excluded); });
}

void collect_candidates()
{
    g_scav_info.candidates.clear();
    for (rf::Item* it = rf::item_list.next; it && it != &rf::item_list; it = it->next) {
        if (is_excluded_candidate_class(*it)) continue;
        g_scav_info.candidates.push_back(ScavCandidate{it->pos, it->orient});
    }
}

float trace_zone_floor_y(const rf::Vector3& pos)
{
    rf::Vector3 hit{};
    return trace_ground_below_point(pos, kScavZoneFloorTraceDist, &hit) ? hit.y : pos.y;
}

// The one place the zone changes, on either side. Only the server traces floor_y: clients handle
// packets on every multi frame, including ones with no level geometry to trace against.
void set_zone(const rf::Vector3& pos, float radius, float floor_y, bool captured)
{
    if (captured && g_scav_info.zone_known) {
        g_scav_info.captured_pos = g_scav_info.zone_pos;
        g_scav_info.captured_radius = g_scav_info.zone_radius;
        g_scav_info.captured_floor_y = g_scav_info.zone_floor_y;
        ++g_zone_capture_serial;
    }
    const bool moved = !g_scav_info.zone_known || pos != g_scav_info.zone_pos;
    // After a capture the new zone always gets its appear burst, even on the retired spot.
    if (captured || moved) {
        ++g_zone_change_serial;
    }
    g_scav_info.zone_pos = pos;
    g_scav_info.zone_radius = radius;
    g_scav_info.zone_floor_y = floor_y;
    g_scav_info.zone_known = true;
}

// A cylinder on the zone floor; the drawn ring sits just inside its edge.
bool zone_contains(const rf::Vector3& pos)
{
    const float dx = pos.x - g_scav_info.zone_pos.x;
    const float dz = pos.z - g_scav_info.zone_pos.z;
    const float dy = pos.y - g_scav_info.zone_floor_y;
    const float r = g_scav_info.zone_radius;
    return dx * dx + dz * dz <= r * r && dy >= kScavZoneCaptureMinHeight && dy <= kScavZoneCaptureMaxHeight;
}

void place_zone(int zone_index, bool captured)
{
    g_scav_info.zone_index = zone_index;
    const rf::Vector3& pos = g_scav_info.candidates[zone_index].pos;
    set_zone(pos, g_alpine_server_config_active_rules.scavenger.zone_radius, trace_zone_floor_y(pos), captured);
    g_scav_info.zone_timer.set(g_alpine_server_config_active_rules.scavenger.zone_timeout_ms);
    xlog::info("scavenger: zone roll at ({:.2f},{:.2f},{:.2f})", pos.x, pos.y, pos.z);
}

std::vector<int> candidates_excluding(std::initializer_list<int> excluded)
{
    std::vector<int> pool;
    for (int i = 0; i < static_cast<int>(g_scav_info.candidates.size()); ++i) {
        if (std::find(excluded.begin(), excluded.end(), i) == excluded.end()) {
            pool.push_back(i);
        }
    }
    return pool;
}

int pick_uniform(const std::vector<int>& pool)
{
    std::uniform_int_distribution<size_t> dist(0, pool.size() - 1);
    return pool[dist(g_rng)];
}

// Uniform over the farthest half of the candidates from the objective's current position, avoiding
// spots near it and the objective and previous two positions, as far as enough candidates remain.
int roll_zone_index(int objective_index, const rf::Vector3& anchor)
{
    const float min_dist = g_alpine_server_config_active_rules.scavenger.zone_radius + kScavZoneMinObjectiveDist;
    const auto far_from_anchor = [&anchor, min_dist](std::vector<int> pool) {
        std::erase_if(pool, [&anchor, min_dist](int i) {
            return (g_scav_info.candidates[i].pos - anchor).len_sq() <= min_dist * min_dist;
        });
        return pool;
    };

    std::vector<int> pool = far_from_anchor(
        candidates_excluding({objective_index, g_scav_info.objective_index, g_scav_info.zone_index}));
    if (pool.empty()) {
        pool = far_from_anchor(candidates_excluding({objective_index}));
    }
    if (pool.empty()) {
        pool = candidates_excluding({objective_index});
    }
    std::sort(pool.begin(), pool.end(), [&anchor](int a, int b) {
        return (g_scav_info.candidates[a].pos - anchor).len_sq() > (g_scav_info.candidates[b].pos - anchor).len_sq();
    });
    pool.resize((pool.size() + 1) / 2);
    return pick_uniform(pool);
}

void roll_positions(bool captured)
{
    std::vector<int> pool = candidates_excluding({g_scav_info.objective_index, g_scav_info.zone_index});
    if (pool.empty()) {
        pool = candidates_excluding({g_scav_info.objective_index});
    }
    const int objective_index = pick_uniform(pool);
    const ScavCandidate& objective = g_scav_info.candidates[objective_index];
    const int zone_index = roll_zone_index(objective_index, objective.pos);

    g_scav_info.objective_index = objective_index;
    salvage_set_spawn(objective.pos, objective.orient);
    place_zone(zone_index, captured);
}

// Re-rolls the zone around wherever the objective is now. True when it moved.
bool roll_zone()
{
    rf::Vector3 anchor = salvage_get_spawn_pos();
    salvage_get_flag_pos(&anchor);
    const int prev_zone_index = g_scav_info.zone_index;
    place_zone(roll_zone_index(g_scav_info.objective_index, anchor), false);
    return g_scav_info.zone_index != prev_zone_index;
}

void on_zone_timeout()
{
    switch (salvage_get_state()) {
        case SalFlagState::AtSpawn:
            roll_positions(false);
            salvage_respawn_at_home();
            af_broadcast_automated_chat_msg("The cartridge and target have moved!");
            break;
        case SalFlagState::Delayed:
            roll_positions(false);
            af_broadcast_automated_chat_msg("The cartridge and target have moved!");
            break;
        case SalFlagState::Carried:
        case SalFlagState::Dropped:
            if (roll_zone()) {
                af_broadcast_automated_chat_msg("The target has moved!");
            }
            break;
    }
    salvage_broadcast_state();
}

// delete_light: only mid-level is the zone light still alive. At level init and multi shutdown
// the outgoing level already took it, so its handle is just a stale number.
void reset_scavenger_state(bool delete_light)
{
    if (delete_light) {
        scavenger_clear_zone_light();
    }
    g_scav_info = ScavengerInfo{};
}

} // namespace

void scavenger_level_init()
{
    reset_scavenger_state(false);
    g_scav_info.active = gt_is_scavenger();
}

void scavenger_level_init_post()
{
    if (!g_scav_info.active || !rf::is_server) return;

    collect_candidates();
    salvage_remove_level_ctf_flags();
    multi_hide_level_items({}, false);

    if (g_scav_info.candidates.size() < 2) {
        xlog::warn("scavenger: level has {} usable item positions, at least 2 are needed; no objective",
            g_scav_info.candidates.size());
        return;
    }

    roll_positions(false);
    salvage_enter_delayed(g_alpine_server_config_active_rules.scavenger.objective_spawn_delay_ms);
    salvage_broadcast_state();
}

void scavenger_do_frame()
{
    if (!rf::is_server) return;
    if (!gt_is_scavenger()) {
        if (g_scav_info.active) {
            reset_scavenger_state(true);
        }
        return;
    }
    if (!g_scav_info.zone_known) return;
    if (rf::gameseq_get_state() != rf::GameState::GS_GAMEPLAY) return;

    if (salvage_get_state() == SalFlagState::Carried) {
        rf::Player* carrier = salvage_get_carrier();
        rf::Entity* ep = salvage_get_alive_carrier_entity();
        if (carrier && ep && zone_contains(ep->pos)) {
            salvage_capture_flag(carrier, g_scav_info.zone_pos);
            roll_positions(true);
            salvage_broadcast_state();
            return;
        }
    }

    if (g_scav_info.zone_timer.valid() && g_scav_info.zone_timer.elapsed()) {
        on_zone_timeout();
        return;
    }

    // The zone countdown always runs, so clients are corrected every second whatever the state.
    if (!g_scav_info.refresh_timer.valid() || g_scav_info.refresh_timer.elapsed()) {
        salvage_broadcast_state();
    }
}

void scavenger_on_multi_shutdown()
{
    reset_scavenger_state(false);
}

void scavenger_broadcast_state()
{
    if (!rf::is_server) return;
    af_send_scavenger_state_packet_to_all();
    g_scav_info.refresh_timer.set(kScavStateRefreshIntervalMs);
}

const rf::Vector3& scavenger_get_zone_pos()
{
    return g_scav_info.zone_pos;
}

float scavenger_get_zone_radius()
{
    return g_scav_info.zone_radius;
}

float scavenger_get_zone_floor_y()
{
    return g_scav_info.zone_floor_y;
}

int scavenger_get_zone_time_left_ms()
{
    if (!g_scav_info.zone_timer.valid()) return 0;
    return std::max(0, g_scav_info.zone_timer.time_until());
}

bool scavenger_get_zone(rf::Vector3* out_pos)
{
    if (!gt_is_scavenger() || !g_scav_info.zone_known) return false;
    *out_pos = g_scav_info.zone_pos;
    return true;
}

void scavenger_apply_zone_from_packet(const rf::Vector3& zone_pos, float zone_radius, float zone_floor_y,
                                      uint32_t time_left_ms, bool captured)
{
    set_zone(zone_pos, zone_radius, zone_floor_y, captured);
    g_scav_info.zone_timer.set(static_cast<int>(std::min<uint32_t>(time_left_ms, 600000)));
}

bool scavenger_get_zone_view(ScavengerZoneView* out)
{
    if (!gt_is_scavenger() || !g_scav_info.zone_known) return false;
    out->pos = g_scav_info.zone_pos;
    out->radius = g_scav_info.zone_radius;
    out->floor_y = g_scav_info.zone_floor_y;
    out->time_left_ms = scavenger_get_zone_time_left_ms();
    out->change_serial = g_zone_change_serial;
    out->capture_serial = g_zone_capture_serial;
    out->captured_pos = g_scav_info.captured_pos;
    out->captured_radius = g_scav_info.captured_radius;
    out->captured_floor_y = g_scav_info.captured_floor_y;
    return true;
}

void scavenger_clear_zone_light()
{
    if (g_scav_info.light_handle >= 0) {
        rf::gr::light_delete(g_scav_info.light_handle, 0);
        g_scav_info.light_handle = -1;
    }
}

void scavenger_update_zone_light(const rf::Color& color, float floor_y)
{
    scavenger_clear_zone_light();
    if (!gt_is_scavenger() || !g_scav_info.zone_known) return;

    const float intensity = salvage_advance_pickup_light_pulse(g_scav_info.light_pulse_phase);
    if (intensity < 0.0f) return;

    rf::Vector3 pos = g_scav_info.zone_pos;
    pos.y = floor_y + kScavZoneLightHeight;
    g_scav_info.light_handle = rf::gr::light_create_point(
        &pos,
        kScavZoneLightRadius,
        intensity,
        color.red / 255.0f, color.green / 255.0f, color.blue / 255.0f,
        true,
        rf::gr::LightShadowcastCondition::SHADOWCAST_EDITOR,
        0);
}

bool scavenger_spawn_weighting_active()
{
    return gt_is_scavenger() && g_scav_info.zone_known && salvage_spawn_is_known();
}

double scavenger_spawn_point_weight(const rf::Vector3& point, float nearest_enemy_dist_sq)
{
    rf::Vector3 objective_pos = salvage_get_spawn_pos();
    salvage_get_flag_pos(&objective_pos);
    const float d_objective = std::min((point - objective_pos).len(), (point - g_scav_info.zone_pos).len());
    const float w_objective =
        std::clamp(d_objective / kScavSpawnObjectiveFullWeightDist, kScavSpawnMinWeight, 1.0f);

    float w_enemy = 1.0f;
    if (nearest_enemy_dist_sq < std::numeric_limits<float>::max()) {
        w_enemy = std::clamp(std::sqrt(nearest_enemy_dist_sq) / kScavSpawnEnemyFullWeightDist,
            kScavSpawnMinWeight, 1.0f);
    }
    return static_cast<double>(w_objective) * w_enemy;
}
