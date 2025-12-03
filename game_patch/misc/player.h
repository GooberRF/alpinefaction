#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <common/utils/string-utils.h>
#include "../multi/multi.h"
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

struct PlayerNetGameSaveData
{
    rf::Vector3 pos;
    rf::Matrix3 orient;
};

struct PlayerAdditionalData
{
    ClientVersion client_version = ClientVersion::unknown;
    uint8_t client_version_major = 0;
    uint8_t client_version_minor = 0;
    uint8_t client_version_patch = 0;
    uint8_t client_version_type = 0;
    uint32_t max_rfl_version = 200;
    std::optional<pf_pure_status> received_ac_status{};
    bool is_bot_player = false;
    bool is_muted = false;
    int last_hitsound_sent_ms = 0;
    int last_critsound_sent_ms = 0;
    std::map<std::string, PlayerNetGameSaveData> saves;
    rf::Vector3 last_teleport_pos;
    rf::TimestampRealtime last_teleport_timestamp;
    std::optional<int> last_spawn_point_index;
    int last_activity_ms = 0;
    rf::TimestampRealtime idle_check_timestamp;
    rf::TimestampRealtime idle_kick_timestamp;
    rf::Timestamp respawn_timer; // only used when configured in ADS
    uint8_t damage_handicap = 0; // percentile
    std::optional<rf::Player*> spectatee{};
    bool remote_server_cfg_sent = false;

    bool is_spectator() const {
        if (rf::is_server) {
            return spectatee.has_value();
        } else {
            return received_ac_status == std::optional{pf_pure_status::af_spectator};
        }
    }

    bool is_bot() const {
        if (rf::is_server) {
            return is_bot_player;
        } else {
            return received_ac_status == std::optional{pf_pure_status::af_bot}
                || received_ac_status
                == std::optional{pf_pure_status::af_spawn_disabled_bot};
        }
    }

    bool is_spawn_disabled_bot() const {
        if (rf::is_server) {
            return is_bot_player
                && g_alpine_server_config_active_rules.ideal_player_count < 32
                && multi_num_spawned_players()
                >= g_alpine_server_config_active_rules.ideal_player_count;
        } else {
            return received_ac_status
                == std::optional{pf_pure_status::af_spawn_disabled_bot};
        }
    }

    bool is_browser() const {
        if (rf::is_server) {
            return client_version == ClientVersion::browser;
        } else {
            return received_ac_status == std::optional{pf_pure_status::rfsb};
        }
    }
};

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
void reset_player_additional_data(const rf::Player* player);
PlayerAdditionalData& get_player_additional_data(const rf::Player* player);
void play_local_hit_sound(bool died);
bool is_player_minimum_af_client_version(
    const rf::Player* player,
    int version_major,
    int version_minor,
    bool only_release = false
);
bool is_server_minimum_af_version(int version_major, int version_minor);
void player_multi_level_post_init();
void update_player_flashlight();
void ping_looked_at_location();
void fpgun_play_random_idle_anim();
void set_headlamp_toggle_enabled(bool enabled);
bool is_player_idle(const rf::Player* player);
