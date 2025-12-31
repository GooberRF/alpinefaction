#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <common/utils/string-utils.h>
#include "../multi/multi.h"
#include "../os/os.h"
#include "../rf/math/vector.h"
#include "../rf/math/matrix.h"
#include "../rf/os/timestamp.h"
#include "../purefaction/pf_packets.h"
#include <unordered_set>

// Forward declarations
namespace rf
{
    struct Player;
}

inline rf::Timestamp g_respawn_timer_local;
inline bool g_spawned_in_current_level = false; // relevant if force respawn is on
inline bool g_local_queued_delayed_spawn = false;
inline std::unordered_set<rf::Player*> g_local_player_spectators{};
inline std::string g_local_player_spectators_spawned_string{};
inline std::string g_local_player_spectators_unspawned_string{};
inline bool g_headlamp_toggle_enabled = true;

std::string build_local_spawn_string(bool can_respawn);
void set_local_spawn_delay(bool can_respawn, bool force_respawn, int spawn_delay);
void reset_local_delayed_spawn();
void find_player(const StringMatcher& query, std::function<void(rf::Player*)> consumer);
void play_local_hit_sound(bool died);
bool is_player_minimum_af_client_version(
    const rf::Player* player,
    const int version_major,
    const int version_minor,
    const int version_patch,
    const bool only_release = false
);
bool is_server_minimum_af_version(int version_major, int version_minor);
void player_multi_level_post_init();
void update_player_flashlight();
void ping_looked_at_location();
void fpgun_play_random_idle_anim();
void set_headlamp_toggle_enabled(bool enabled);
bool player_is_idle(const rf::Player* player);
