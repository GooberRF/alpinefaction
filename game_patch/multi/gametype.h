#pragma once

#include <optional>
#include <xlog/xlog.h>
#include "../rf/trigger.h"
#include "../rf/os/string.h"
#include "../rf/os/timestamp.h"
#include "../rf/multi.h"

// forward declarations
namespace rf
{
    struct EventCapturePointHandler;
}

static const char* const multi_rfl_prefixes[] = {
    // "m_",
    // "dm",
    // "ctf",
    "koth",
    "dc",
    "rev",
    "run",
    "esc"};

enum class HillOwner : int
{
    HO_Neutral = 0,
    HO_Red = 1,
    HO_Blue = 2,
};

enum class HillState : int
{
    HS_Idle = 0,
    HS_LeanRedGrowing = 1,
    HS_LeanRedShrinking = 2,
    HS_LeanBlueGrowing = 3,
    HS_LeanBlueShrinking = 4,
};

enum class HillRole : int
{
    HR_Center = 0,
    HR_RedBase = 1,
    HR_BlueBase = 2,
    HR_RedForward = 3,
    HR_BlueForward = 4,
};

enum class HillLockStatus : int
{
    HLS_Available = 0,
    HLS_Locked = 1,
    HLS_Permalocked = 2,
};

struct Presence
{
    int red = 0;
    int blue = 0;
    bool contested() const
    {
        return red > 0 && blue > 0;
    }
    bool empty() const
    {
        return red == 0 && blue == 0;
    }
};

struct PresencePlayerIDs
{
    std::vector<uint8_t> player_ids;
};

struct HillInfo
{
    int hill_uid = -1;
    std::string name = "";
    int trigger_uid = -1; // used once to lookup trigger
    rf::Trigger* trigger = nullptr; // set on koth init during map init
    float outline_offset = 0.0f;
    float capture_rate = 0.0f;
    rf::EventCapturePointHandler* handler = nullptr; // used for world HUD icon
    HillRole role = HillRole::HR_Center;
    HillOwner ownership = HillOwner::HO_Neutral;
    HillOwner steal_dir = HillOwner::HO_Neutral;
    HillState state = HillState::HS_Idle;
    HillLockStatus lock_status = HillLockStatus::HLS_Available;
    uint8_t capture_progress = 0; // 0-100
    int hold_ms_accum = 0;
    int capture_milli = 0; // 0 - 100000 (100% = 100000)
    std::vector<int> mp_spawn_uids;
    int stage = 0; // order of hills in REV
    int cap_gain_sfx_handle = -1;
    bool cap_gain_sfx_playing = false;

    // debug tracking
    int dbg_last_red = -1;
    int dbg_last_blue = -1;
    HillOwner dbg_last_owner = HillOwner::HO_Neutral;
    HillOwner dbg_last_dir = HillOwner::HO_Neutral;
    HillState dbg_last_state = HillState::HS_Idle;
    uint8_t dbg_last_prog_bucket = 255; // progress/5 bucket for quieter logs

    // clientside prediction
    uint8_t net_last_red = 0;
    uint8_t net_last_blue = 0;
    HillOwner net_last_dir = HillOwner::HO_Neutral;
    HillState net_last_state = HillState::HS_Idle;
    uint8_t net_last_prog_bucket = 255;
    HillLockStatus net_last_lock_status = HillLockStatus::HLS_Available;
    int client_hold_ms_accum = 0;

    // clientside visual smoothing
    bool vis_contested = false;
    int vis_last_flip_ms = 0;
};

struct CPGTRules // capture point game types
{
    int grow_rate = 20;           // attackers alone, toward flip/capture
    int drain_empty_rate = 10;    // no one on point, progress bleeds toward 0
    int drain_defended_rate = 50; // owner present, attackers absent -> drain faster
    int ms_per_point = 1000;      // scoring tick while owned and not pressured
    bool cyl_use_trigger_up = false; // if sphere is treated as a cylinder, true = use trigger dir to build cylinder, false = use world up
    bool require_neutral_to_capture = false; // if true, attackers must neutralize before flipping ownership
};

struct KothInfo
{
    CPGTRules rules;
    int red_team_score = 0;
    int blue_team_score = 0;
    std::vector<HillInfo> hills;
};

inline HillOwner opposite(HillOwner o)
{
    return (o == HillOwner::HO_Red)    ? HillOwner::HO_Blue
           : (o == HillOwner::HO_Blue) ? HillOwner::HO_Red
                                       : HillOwner::HO_Neutral;
}

static inline int rate_to_step(int rate_per_sec, int dt_ms)
{
    // clamp so we dont skip past boundaries on long frames
    return std::max(0, (rate_per_sec * dt_ms) / 1000);
}

static inline bool team_present(HillOwner team, const Presence& p)
{
    return (team == HillOwner::HO_Red) ? (p.red > 0) : (team == HillOwner::HO_Blue) ? (p.blue > 0) : false;
}

static inline bool hostile_progress_present_server(const HillInfo& h)
{
    if (h.ownership != HillOwner::HO_Red && h.ownership != HillOwner::HO_Blue)
        return false;
    const HillOwner opp = opposite(h.ownership);
    return (h.steal_dir == opp) && (h.capture_milli > 0);
}

extern KothInfo g_koth_info;

bool multi_game_type_is_team_type(rf::NetGameType game_type);
bool multi_game_type_has_hills(rf::NetGameType game_type);
bool multi_is_game_type_with_hills();
bool multi_is_team_game_type();
bool gt_is_koth();
bool gt_is_dc();
bool gt_is_rev();
bool gt_is_esc();
bool gt_is_run();
bool rev_all_points_permalocked();
bool esc_all_points_owned_by_one_team();
bool esc_team_can_attack_hill(const HillInfo& hill, HillOwner team);
int multi_koth_get_red_team_score();
int multi_koth_get_blue_team_score();
void multi_koth_set_red_team_score(int score);
void multi_koth_set_blue_team_score(int score);
HillInfo* koth_find_hill_by_uid(uint8_t uid);
HillInfo* koth_find_hill_by_handler(const rf::EventCapturePointHandler* handler);
bool koth_set_capture_point_owner(rf::EventCapturePointHandler* handler, int owner, bool announce = true);
rf::Trigger* koth_resolve_trigger_from_uid(int uid);
void koth_force_broadcast_all_hill_states();
void koth_local_announce_hill_captured(const HillInfo* h, HillOwner new_owner, const uint8_t* ids, size_t ids_len);
void koth_local_announce_hill_captured_vector(const HillInfo* h, HillOwner new_owner, const std::vector<uint8_t>& ids);
void multi_level_init_post_gametypes();
void koth_do_frame();
void gametype_do_patch();
void populate_gametype_table();
