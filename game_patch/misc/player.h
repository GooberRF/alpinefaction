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
};

static rf::Timestamp g_respawn_timer_local;
static bool g_spawned_in_current_level = false; // relevant if force respawn is on
static bool g_local_queued_delayed_spawn = false;

std::string build_local_spawn_string(bool can_respawn);
void set_local_spawn_delay(bool can_respawn, bool force_respawn, int spawn_delay);
void reset_local_delayed_spawn();
void find_player(const StringMatcher& query, std::function<void(rf::Player*)> consumer);
void reset_player_additional_data(const rf::Player* player);
PlayerAdditionalData& get_player_additional_data(rf::Player* player);
void play_local_hit_sound(bool died);
void handle_chat_message_sound(std::string message);
bool is_player_minimum_af_client_version(rf::Player* player, int version_major, int version_minor);
bool is_server_minimum_af_version(int version_major, int version_minor);
void player_multi_level_post_init();
void update_player_flashlight();
void ping_looked_at_location();
