#pragma once

#include <cstdint>
#include "../rf/math/vector.h"
#include "../rf/gr/gr.h"

// Scavenger: one neutral objective (the Salvage flag, driven by salvage.cpp) carried by either team
// to a neutral capture zone. Both positions are rolled from the level's item positions.

// The zone's last stretch before it re-rolls: countdown shown, colour and arc urgency.
constexpr int kScavZoneCountdownVisibleMs = 30000;
constexpr rf::Color kScavZoneColorIdle{135, 58, 255, 255};
constexpr rf::Color kScavZoneColorUrgent{255, 170, 40, 255};

void scavenger_level_init();
void scavenger_level_init_post();
void scavenger_do_frame();
void scavenger_on_multi_shutdown();

// Server: sends the state packet to every client. Reached through salvage_broadcast_state.
void scavenger_broadcast_state();

const rf::Vector3& scavenger_get_zone_pos();
float scavenger_get_zone_radius();
float scavenger_get_zone_floor_y();
int scavenger_get_zone_time_left_ms();
// The capture zone, from whichever side is asking. False outside Scavenger or before it is known.
bool scavenger_get_zone(rf::Vector3* out_pos);
// captured: the packet carried a capture, which retired the previous zone.
void scavenger_apply_zone_from_packet(const rf::Vector3& zone_pos, float zone_radius, float zone_floor_y,
                                      uint32_t time_left_ms, bool captured);

struct ScavengerZoneView
{
    rf::Vector3 pos;
    float radius;
    // Floor under pos, or pos.y when none is close: the visual sits on it and capture is measured from it.
    float floor_y;
    int time_left_ms;
    // Bumped whenever the zone moves or first appears. Never reset, so a new level is a change too.
    uint32_t change_serial;
    // Bumped whenever a capture retires a zone, which captured_pos/captured_radius describe.
    uint32_t capture_serial;
    rf::Vector3 captured_pos;
    float captured_radius;
    float captured_floor_y;
};
// The zone for the client-side renderers. False outside Scavenger or before the zone is known.
bool scavenger_get_zone_view(ScavengerZoneView* out);

// Pulsing point light on the zone. Client-side and per frame: delete + recreate like
// salvage_update_dynamic_light. Update only deletes it when no zone is live.
void scavenger_clear_zone_light();
void scavenger_update_zone_light(const rf::Color& color, float floor_y);

// True once the objective and zone positions exist, so respawn points can be weighted by them.
bool scavenger_spawn_weighting_active();
// Roulette weight of a respawn point. nearest_enemy_dist_sq is FLT_MAX when no enemy is alive.
double scavenger_spawn_point_weight(const rf::Vector3& point, float nearest_enemy_dist_sq);
