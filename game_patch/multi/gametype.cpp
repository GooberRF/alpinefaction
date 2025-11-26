#include <xlog/xlog.h>
#include <unordered_set>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <common/utils/list-utils.h>
#include <common/version/version.h>
#include "gametype.h"
#include "multi.h"
#include "alpine_packets.h"
#include "../hud/multi_spectate.h"
#include "../sound/sound.h"
#include "../rf/os/timestamp.h"
#include "../object/event_alpine.h"
#include "../rf/gameseq.h"
#include "../rf/localize.h"

static char* const* g_af_gametype_names[8];

static char koth_name[] = "KOTH";
static char* koth_slot = koth_name;
static char dc_name[] = "DC";
static char* dc_slot = dc_name;
static char rev_name[] = "REV";
static char* rev_slot = rev_name;
static char run_name[] = "RUN";
static char* run_slot = run_name;
static char esc_name[] = "ESC";
static char* esc_slot = esc_name;

KothInfo g_koth_info; // KOTH and DC
rf::Timestamp g_local_contest_alarm_cooldown;
static int g_cap_alarm_sound_id = -1;

static bool hill_sound_position(const HillInfo& hill, rf::Vector3& pos)
{
    if (hill.trigger) {
        pos = hill.trigger->pos;
        return true;
    }

    return false;
}

static void stop_hill_sounds()
{
    for (auto& hill : g_koth_info.hills) {
        if (hill.cap_gain_sfx_playing && hill.cap_gain_sfx_handle > -1) {
            rf::snd_stop(hill.cap_gain_sfx_handle);
        }

        hill.cap_gain_sfx_playing = false;
        hill.cap_gain_sfx_handle = -1;
    }
}

void populate_gametype_table() {
    g_af_gametype_names[0] = &rf::strings::dm;
    g_af_gametype_names[1] = &rf::strings::ctf;
    g_af_gametype_names[2] = &rf::strings::teamdm;
    g_af_gametype_names[3] = &koth_slot;
    g_af_gametype_names[4] = &dc_slot;
    g_af_gametype_names[5] = &rev_slot;
    g_af_gametype_names[6] = &run_slot;
    g_af_gametype_names[7] = &esc_slot;

    for (int i = 0; i < 5; ++i) {
        const char* const* slot = g_af_gametype_names[i];
        const char* name = (slot && *slot) ? *slot : "(null)";
        //xlog::warn("GameType[{}]: {} (slot={}, name_ptr={})", i, name, static_cast<const void*>(slot), static_cast<const void*>(*slot));
    }
}

CallHook<char*(const char*, const char*)> listen_server_map_list_filename_contains_hook{
    0x00445730,
    [](const char* filename, const char* contains_str) {
        if (char* p = listen_server_map_list_filename_contains_hook.call_target(filename, contains_str))
            return p; // contains "ctf"

        for (const char* tok : multi_rfl_prefixes) {
            if (char* q = listen_server_map_list_filename_contains_hook.call_target(filename, tok))
                return q; // contains "koth" etc.
        }

        return static_cast<char*>(nullptr); // no match
    },
};

bool multi_game_type_is_team_type(rf::NetGameType game_type)
{
    switch (game_type) {
        case rf::NG_TYPE_CTF:
        case rf::NG_TYPE_TEAMDM:
        case rf::NG_TYPE_KOTH:
        case rf::NG_TYPE_DC:
        case rf::NG_TYPE_REV:
        case rf::NG_TYPE_ESC:
            return true;
        default: // DM, RUN
            return false;
    }
}

bool multi_game_type_has_hills(rf::NetGameType game_type)
{
    switch (game_type) {
        case rf::NG_TYPE_KOTH:
        case rf::NG_TYPE_DC:
        case rf::NG_TYPE_REV:
        case rf::NG_TYPE_ESC:
            return true;
        default: // DM, CTF, TDM, RUN
            return false;
    }
}

bool multi_is_game_type_with_hills()
{
    return multi_game_type_has_hills(rf::multi_get_game_type());
}

bool multi_is_team_game_type()
{
    return multi_game_type_is_team_type(rf::multi_get_game_type());
}

int multi_koth_get_red_team_score() // KOTH and DC
{
    return g_koth_info.red_team_score;
}

int multi_koth_get_blue_team_score() // KOTH and DC
{
    return g_koth_info.blue_team_score;
}

void multi_koth_set_red_team_score(int score) // KOTH and DC
{
    if (rf::is_server)
        return;

    g_koth_info.red_team_score = score;
    return;
}

void multi_koth_set_blue_team_score(int score) // KOTH and DC
{
    if (rf::is_server)
        return;

    g_koth_info.blue_team_score = score;
    return;
}

void multi_koth_reset_scores() // KOTH and DC
{
    g_koth_info.red_team_score = 0;
    g_koth_info.blue_team_score = 0;
}

bool gt_is_koth()
{
    return rf::multi_get_game_type() == rf::NetGameType::NG_TYPE_KOTH;
}

bool gt_is_dc()
{
    return rf::multi_get_game_type() == rf::NetGameType::NG_TYPE_DC;
}

bool gt_is_rev()
{
    return rf::multi_get_game_type() == rf::NetGameType::NG_TYPE_REV;
}

bool gt_is_esc()
{
    return rf::multi_get_game_type() == rf::NetGameType::NG_TYPE_ESC;
}

bool gt_is_run()
{
    return rf::multi_get_game_type() == rf::NetGameType::NG_TYPE_RUN;
}

static HillInfo* esc_find_hill_by_role(HillRole role)
{
    for (auto& hill : g_koth_info.hills) {
        if (hill.role == role)
            return &hill;
    }

    return nullptr;
}

static bool esc_role_exists(HillRole role)
{
    return esc_find_hill_by_role(role) != nullptr;
}

HillInfo* koth_find_hill_by_uid(uint8_t uid)
{
    for (auto& h : g_koth_info.hills)
        if (h.hill_uid == static_cast<int>(uid))
            return &h;
    return nullptr;
}

HillInfo* koth_find_hill_by_handler(const rf::EventCapturePointHandler* handler)
{
    if (!handler)
        return nullptr;

    for (auto& h : g_koth_info.hills) {
        if (h.handler == handler)
            return &h;
    }

    return nullptr;
}

float cylinder_radius_for_box_inscribed(const rf::Trigger* t)
{
    const float hx = 0.5f * std::fabs(t->box_size.x);
    const float hz = 0.5f * std::fabs(t->box_size.z);
    return std::min(hx, hz);
}

static inline bool trigger_oriented_cylinder_check_if_activated(const rf::Trigger* tp, const rf::Object* objp, float radius,
    float half_height, const rf::Vector3& axis, const rf::Vector3& rvec, const rf::Vector3& fvec
)
{
    if (!tp || !objp)
        return false;

    const rf::Vector3 d = objp->pos - tp->pos;
    const float y = d.dot_prod(axis);
    if (y < -half_height || y > half_height)
        return false;

    // radial distance in the boxs local XZ plane
    const float x = d.dot_prod(rvec);
    const float z = d.dot_prod(fvec);
    return (x * x + z * z) <= (radius * radius);
}

static inline bool trigger_sphere_as_full_cylinder_check_if_activated(const rf::Trigger* tp, const rf::Object* objp, bool use_trigger_up
)
{
    if (!tp || !objp)
        return false;

    const float r = tp->radius;
    if (r <= 0.0f)
        return false;

    rf::Vector3 axis = use_trigger_up ? tp->orient.uvec : rf::Vector3{0.f, 1.f, 0.f};
    axis.normalize_safe();

    // d projected onto axis gives height from center
    const rf::Vector3 d = objp->pos - tp->pos;
    const float y = d.dot_prod(axis);
    if (y < -r || y > r)
        return false;

    const rf::Vector3 radial = d - axis * y;
    return radial.len_sq() <= (r * r);
}

bool obj_inside_trigger(const rf::Trigger* t, const rf::Object* o)
{
    if (!t || !o)
        return false;
    if (t->type == 0)
        return rf::trigger_inside_bounding_sphere(const_cast<rf::Trigger*>(t), const_cast<rf::Object*>(o));
    if (t->type == 1)
        return rf::trigger_inside_bounding_box(const_cast<rf::Trigger*>(t), const_cast<rf::Object*>(o));
    return false;
}

bool obj_inside_uid_area(int check_uid, const rf::Object* o)
{
    if (check_uid < 0 || !o)
        return false;

    if (rf::Object* chk = rf::obj_lookup_from_uid(check_uid)) {
        if (chk->type == rf::ObjectType::OT_TRIGGER) {
            return obj_inside_trigger(static_cast<rf::Trigger*>(chk), o);
        }
    }

    return false;
}

bool player_inside_hill_trigger(const HillInfo& h, const rf::Player& p)
{
    auto ent = rf::entity_from_handle(p.entity_handle);
    if (!ent || !h.trigger)
        return false;

    if (h.trigger->type == 0 && h.handler->sphere_to_cylinder) {
        return trigger_sphere_as_full_cylinder_check_if_activated(h.trigger, ent, g_koth_info.rules.cyl_use_trigger_up);
    }

    if (h.trigger->type == 1 && h.handler->sphere_to_cylinder) {
        const rf::Trigger* t = h.trigger;
        const float radius = cylinder_radius_for_box_inscribed(t);
        const float half_h = 0.5f * std::fabs(t->box_size.y);
        const rf::Vector3 u = t->orient.uvec;
        const rf::Vector3 r = t->orient.rvec;
        const rf::Vector3 f = t->orient.fvec;
        return trigger_oriented_cylinder_check_if_activated(t, ent, radius, half_h, u, r, f);
    }

    // fallback to stock checks
    return obj_inside_trigger(h.trigger, ent);
}

bool player_is_countable(rf::Player& p)
{
    auto ent = rf::entity_from_handle(p.entity_handle);
    if (!ent)
        return false; // invalid entity
    if (rf::entity_is_dying(ent))
        return false; // dying
    if (ent->life <= 0)
        return false; // dead
    return true;
}

Presence sample_presence(const HillInfo& h)
{
    Presence out;
    auto plist = SinglyLinkedList{rf::player_list};
    for (auto& pl : plist) {
        if (!player_is_countable(pl))
            continue;
        if (!player_inside_hill_trigger(h, pl))
            continue;

        if (pl.team == 0)
            ++out.red;
        else if (pl.team == 1)
            ++out.blue;
    }
    return out;
}

static int players_on_team(const Presence& pres, HillOwner team)
{
    switch (team) {
        case HillOwner::HO_Red:
            return pres.red;
        case HillOwner::HO_Blue:
            return pres.blue;
        default:
            return 0;
    }
}

static float rate_bonus_for_players(int player_count)
{
    if (player_count <= 1)
        return 1.0f;
    if (player_count == 2)
        return 1.5f;
    return 2.0f; // 3 or more
}

static int scaled_capture_rate(const HillInfo& h, int base_rate, int player_count)
{
    if (base_rate <= 0)
        return base_rate;

    float multiplier = (h.capture_rate > 0.0f) ? h.capture_rate : 1.0f;
    multiplier *= rate_bonus_for_players(player_count);
    multiplier *= 0.25f; // internal cap rate multiplier knob

    int scaled = static_cast<int>(std::lround(base_rate * multiplier));
    if (base_rate > 0)
        scaled = std::max(scaled, 1);
    return scaled;
}

static int effective_rate_per_sec(const HillInfo& h, int base_rate, const Presence& pres, HillOwner team)
{
    return scaled_capture_rate(h, base_rate, players_on_team(pres, team));
}

static std::vector<uint8_t> on_capture_collect_player_ids_on_hill_for_team(const HillInfo& h, HillOwner team)
{
    std::vector<uint8_t> ids;
    auto plist = SinglyLinkedList{rf::player_list};
    for (auto& pl : plist) {
        if (!player_is_countable(pl))
            continue;
        if ((team == HillOwner::HO_Red && pl.team != 0) || (team == HillOwner::HO_Blue && pl.team != 1))
            continue;

        if (!player_inside_hill_trigger(h, pl))
            continue;
        if (pl.net_data) {
            ids.push_back(pl.net_data->player_id);
            rf::player_add_score(&pl, 3);
            //xlog::warn("added 3 score to {} for being one of the cappers", pl.name);
        }
    }
    return ids;
}

rf::Trigger* koth_resolve_trigger_from_uid(int uid)
{
    if (uid < 0)
        return nullptr;
    if (auto* o = rf::obj_lookup_from_uid(uid)) {
        if (o->type == rf::ObjectType::OT_TRIGGER)
            return static_cast<rf::Trigger*>(o);
    }
    return nullptr;
}

inline bool local_player_on_owner_team(HillOwner owner)
{
    if (!rf::local_player)
        return false;

    const bool local_is_blue = rf::local_player->team != 0;
    switch (owner) {
        case HillOwner::HO_Red:
            return !local_is_blue;
        case HillOwner::HO_Blue:
            return local_is_blue;
        default:
            return false;
    }
}

inline int capture_sfx_for_local(HillOwner owner)
{
    if (owner == HillOwner::HO_Neutral)
        return 66; // Flag_Return
    return local_player_on_owner_team(owner) ? 63 : 67; // Flag_Capture : Flag_Steal
}

static void notify_capture_point_captured(const HillInfo& h, HillOwner new_owner)
{
    if (!h.handler)
        return;

    if (new_owner != HillOwner::HO_Red && new_owner != HillOwner::HO_Blue)
        return;

    auto events = rf::find_all_events_by_type(rf::EventType::When_Captured);
    if (events.empty())
        return;

    const int handler_handle = h.handler->handle;
    const int owner_token = (new_owner == HillOwner::HO_Red) ? -2 : -3;

    for (auto* event : events) {
        if (!event)
            continue;

        const auto it = std::find(event->links.begin(), event->links.end(), handler_handle);
        if (it != event->links.end()) {
            event->activate(handler_handle, owner_token, true);
        }
    }
}

void koth_local_announce_hill_captured(const HillInfo* h, HillOwner new_owner, const uint8_t* ids, size_t ids_len)
{
    if (!h)
        return;

    // Build captor names from IDs
    std::vector<std::string> names;
    names.reserve(ids_len);
    for (size_t i = 0; i < ids_len; ++i) {
        const uint8_t pid = ids[i];
        if (rf::Player* p = rf::multi_find_player_by_id(pid)) {
            names.emplace_back(p->name ? p->name : std::string{"Player "} + std::to_string(pid));
        }
    }

    std::string names_csv;
    if (!names.empty()) {
        names_csv = names[0];
        for (size_t i = 1; i < names.size(); ++i) {
            names_csv += ", ";
            names_csv += names[i];
        }
    }
    else {
        names_csv = "unknown";
    }

    const char* team_name = (new_owner == HillOwner::HO_Red) ? "RED" : (new_owner == HillOwner::HO_Blue) ? "BLUE" : "NEUTRAL";

    const std::string msg_str =
        new_owner == HillOwner::HO_Neutral ? 
        std::format("{} was reset to NEUTRAL by {}", h->name.empty() ? "Hill" : h->name, names_csv) :
        std::format("{} was captured by {} for the {} team", h->name.empty() ? "Hill" : h->name, names_csv, team_name);
    rf::String msg = msg_str.c_str();

    rf::ChatMsgColor color_id = rf::ChatMsgColor::white_white;
    if (new_owner == HillOwner::HO_Red)
        color_id = rf::ChatMsgColor::red_red;
    if (new_owner == HillOwner::HO_Blue)
        color_id = rf::ChatMsgColor::blue_blue;

    rf::multi_chat_print(msg, color_id, {});

    const int sfx = capture_sfx_for_local(new_owner);
    if (sfx >= 0) {
        rf::snd_play(sfx, 0, 0.0, 1.0);
    }

    if (new_owner == HillOwner::HO_Red || new_owner == HillOwner::HO_Blue) {
        notify_capture_point_captured(*h, new_owner);
    }
}

void koth_local_announce_hill_captured_vector(const HillInfo* h, HillOwner new_owner, const std::vector<uint8_t>& ids)
{
    koth_local_announce_hill_captured(h, new_owner, ids.data(), ids.size());
}

// --- logging helpers ---
static const char* to_string(HillOwner o)
{
    switch (o) {
    case HillOwner::HO_Red:
        return "RED";
    case HillOwner::HO_Blue:
        return "BLUE";
    default:
        return "NEUTRAL";
    }
}

static const char* to_string(HillState s)
{
    switch (s) {
    case HillState::HS_Idle:
        return "Idle";
    case HillState::HS_LeanRedGrowing:
        return "RedGrowing";
    case HillState::HS_LeanRedShrinking:
        return "RedShrinking";
    case HillState::HS_LeanBlueGrowing:
        return "BlueGrowing";
    case HillState::HS_LeanBlueShrinking:
        return "BlueShrinking";
    default:
        return "?";
    }
}

static std::vector<HillRole> esc_path_for_team(HillOwner team)
{
    std::vector<HillRole> path;
    path.reserve(5);

    switch (team) {
        case HillOwner::HO_Red:
            path.push_back(HillRole::HR_RedBase);
            if (esc_role_exists(HillRole::HR_RedForward))
                path.push_back(HillRole::HR_RedForward);
            path.push_back(HillRole::HR_Center);
            if (esc_role_exists(HillRole::HR_BlueForward))
                path.push_back(HillRole::HR_BlueForward);
            path.push_back(HillRole::HR_BlueBase);
            break;

        case HillOwner::HO_Blue:
            path.push_back(HillRole::HR_BlueBase);
            if (esc_role_exists(HillRole::HR_BlueForward))
                path.push_back(HillRole::HR_BlueForward);
            path.push_back(HillRole::HR_Center);
            if (esc_role_exists(HillRole::HR_RedForward))
                path.push_back(HillRole::HR_RedForward);
            path.push_back(HillRole::HR_RedBase);
            break;

        default:
            break;
    }

    return path;
}

static bool hill_has_spawn_uid(const HillInfo& hill, int rp_uid)
{
    return std::find(hill.mp_spawn_uids.begin(), hill.mp_spawn_uids.end(), rp_uid) != hill.mp_spawn_uids.end();
}

static bool rev_should_enable_respawn_point(int rp_uid)
{
    return std::any_of(g_koth_info.hills.begin(), g_koth_info.hills.end(), [rp_uid](const HillInfo& hill) {
        if (hill.lock_status != HillLockStatus::HLS_Available)
            return false;

        return std::find(hill.mp_spawn_uids.begin(), hill.mp_spawn_uids.end(), rp_uid) != hill.mp_spawn_uids.end();
    });
}

static HillInfo* esc_next_hill_in_path(const HillInfo* current, HillOwner team)
{
    if (!gt_is_esc())
        return nullptr;

    if (!current)
        return nullptr;

    if (team != HillOwner::HO_Red && team != HillOwner::HO_Blue)
        return nullptr;

    // Build progression path for this team
    const auto path = esc_path_for_team(team);

    // Find the current hill's role in that path
    const auto it = std::find(path.begin(), path.end(), current->role);
    if (it == path.end())
        return nullptr; // current hill isn't in the path for this team
    if (it + 1 == path.end())
        return nullptr; // already at the last hill in the path

    const HillRole next_role = *(it + 1);

    return esc_find_hill_by_role(next_role);
}

static bool esc_is_defense_hill_for_team(const HillInfo* h, HillOwner team)
{
    if (!h)
        return false;

    if (team != HillOwner::HO_Red && team != HillOwner::HO_Blue)
        return false;

    if (h->ownership != team)
        return false;

    HillInfo* next = esc_next_hill_in_path(h, team);

    if (!next) {
        return true;
    }

    if (next->ownership == HillOwner::HO_Neutral) {
        return true;
    }

    if (next->ownership != team) {
        return true;
    }

    return false;
}

static bool esc_should_enable_respawn_point_for_team(int rp_uid, HillOwner team)
{
    if (!gt_is_esc())
        return false;

    if (team != HillOwner::HO_Red && team != HillOwner::HO_Blue)
        return false;

    for (auto& hill : g_koth_info.hills) {
        if (!esc_is_defense_hill_for_team(&hill, team))
            continue;
        if (!hill_has_spawn_uid(hill, rp_uid))
            continue;

        return true; // this spawn is associated with at least one qualifying hill
    }

    return false;
}

static void koth_update_respawn_points(HillInfo* h) {
    if (!h || h->mp_spawn_uids.empty())
        return;

    auto lock_status = h->lock_status;

    for (int rp_uid : h->mp_spawn_uids) {
        if (auto* rp = get_alpine_respawn_point_by_uid(rp_uid)) {

            if (gt_is_rev()) { 
                // REV: enable spawns when any linked hill is available
                set_alpine_respawn_point_enabled(rp, rev_should_enable_respawn_point(rp_uid));
            }
            else if (gt_is_esc()) {
                // ESC: enable spawns only on the team's current defense hill
                const bool enable_for_red =
                    rp->red_team && esc_should_enable_respawn_point_for_team(rp_uid, HillOwner::HO_Red);
                const bool enable_for_blue =
                    rp->blue_team && esc_should_enable_respawn_point_for_team(rp_uid, HillOwner::HO_Blue);

                const bool enabled = enable_for_red || enable_for_blue;
                set_alpine_respawn_point_enabled(rp, enabled);
            }
            else {
                // KOTH/DC: enable spawns and swap based on the team that controls the hill
                auto owner = h->ownership;
                set_alpine_respawn_point_teams(rp, owner == HillOwner::HO_Red, owner == HillOwner::HO_Blue);

                bool enabled = (owner != HillOwner::HO_Neutral)
                    && (lock_status == HillLockStatus::HLS_Available);
                set_alpine_respawn_point_enabled(rp, enabled);
            }

            //if (rp->enabled)
                //xlog::warn("mp spawn {} ({}) status - enabled {}, red {}, blue {}", rp->name, rp_uid, rp->enabled, rp->red_team, rp->blue_team);
        }
    }
}

static void esc_update_all_respawn_points()
{
    if (!gt_is_esc())
        return;

    for (auto& hill : g_koth_info.hills) {
        koth_update_respawn_points(&hill);
    }
}

static void rev_recalculate_stage_locks()
{
    if (!gt_is_rev())
        return;

    if (g_koth_info.hills.empty())
        return;

    std::vector<int> unique_stages;
    unique_stages.reserve(g_koth_info.hills.size());

    for (const auto& hill : g_koth_info.hills) {
        if (std::find(unique_stages.begin(), unique_stages.end(), hill.stage) == unique_stages.end()) {
            unique_stages.push_back(hill.stage);
        }
    }

    std::sort(unique_stages.begin(), unique_stages.end());

    bool all_previous_complete = true;

    for (int stage : unique_stages) {
        bool stage_complete = true;

        for (const auto& hill : g_koth_info.hills) {
            if (hill.stage != stage)
                continue;

            if (hill.ownership != HillOwner::HO_Red) {
                stage_complete = false;
                break;
            }
        }

        const bool stage_should_be_available = all_previous_complete;

        for (auto& hill : g_koth_info.hills) {
            if (hill.stage != stage)
                continue;

            if (hill.lock_status == HillLockStatus::HLS_Permalocked)
                continue;

            const HillLockStatus desired_status =
                stage_should_be_available ? HillLockStatus::HLS_Available : HillLockStatus::HLS_Locked;

            if (hill.lock_status != desired_status) {
                hill.lock_status = desired_status;
                koth_update_respawn_points(&hill);
            }
        }

        if (!stage_complete) {
            all_previous_complete = false;
        }
    }
}

static std::optional<HillRole> esc_prerequisite_for_team(HillRole target, HillOwner team)
{
    if (team != HillOwner::HO_Red && team != HillOwner::HO_Blue)
        return std::nullopt;

    const auto path = esc_path_for_team(team);
    const auto it = std::find(path.begin(), path.end(), target);
    if (it == path.end())
        return std::nullopt;

    // Walk backwards along the lane until we find a role that actually exists.
    auto scan = it;
    while (scan != path.begin()) {
        --scan;
        if (esc_role_exists(*scan)) {
            return *scan;
        }
    }

    // No existing prereq role before target -> no prerequisite
    return std::nullopt;
}

static bool esc_team_can_attack_role(HillRole target, HillOwner team)
{
    if (team != HillOwner::HO_Red && team != HillOwner::HO_Blue)
        return false;

    if (auto* hill = esc_find_hill_by_role(target)) {
        if (hill->ownership == team)
            return true; // already owned by this team
    }

    const auto prereq = esc_prerequisite_for_team(target, team);
    if (!prereq)
        return true; // no prerequisite, always available

    if (auto* hill = esc_find_hill_by_role(*prereq))
        return hill->ownership == team;

    return false;
}

bool esc_team_can_attack_hill(const HillInfo& hill, HillOwner team)
{
    if (!gt_is_esc())
        return false;

    if (team == HillOwner::HO_Neutral)
        return false;

    if (hill.ownership == team)
        return false;

    return esc_team_can_attack_role(hill.role, team);
}

static void esc_apply_initial_ownerships()
{
    if (!gt_is_esc())
        return;

    for (auto& hill : g_koth_info.hills) {
        switch (hill.role) {
        case HillRole::HR_RedBase:
        case HillRole::HR_RedForward:
            hill.ownership = HillOwner::HO_Red;
            break;
        case HillRole::HR_BlueBase:
        case HillRole::HR_BlueForward:
            hill.ownership = HillOwner::HO_Blue;
            break;
        case HillRole::HR_Center:
        default:
            hill.ownership = HillOwner::HO_Neutral;
            break;
        }
    }
}

static void server_maybe_broadcast_state(HillInfo& h, const Presence& pres)
{
    const uint8_t prog_bucket = static_cast<uint8_t>(h.capture_progress / 5);

    bool changed = false;
    changed |= (h.net_last_red != static_cast<uint8_t>(std::clamp(pres.red, 0, 255)));
    changed |= (h.net_last_blue != static_cast<uint8_t>(std::clamp(pres.blue, 0, 255)));
    changed |= (h.net_last_state != h.state);
    changed |= (h.net_last_dir != h.steal_dir);
    changed |= (h.net_last_prog_bucket != prog_bucket);
    changed |= (h.net_last_lock_status != h.lock_status);

    if (!changed)
        return;

    af_send_koth_hill_state_packet_to_all(h, pres);

    // snapshot what we sent
    h.net_last_red = static_cast<uint8_t>(std::clamp(pres.red, 0, 255));
    h.net_last_blue = static_cast<uint8_t>(std::clamp(pres.blue, 0, 255));
    h.net_last_state = h.state;
    h.net_last_dir = h.steal_dir;
    h.net_last_prog_bucket = prog_bucket;
    h.net_last_lock_status = h.lock_status;
}

static void esc_recalculate_stage_locks()
{
    if (!gt_is_esc())
        return;

    if (g_koth_info.hills.empty())
        return;

    bool any_change = false;

    for (auto& hill : g_koth_info.hills) {
        if (hill.lock_status == HillLockStatus::HLS_Permalocked)
            continue;

        const bool red_can_attack = (hill.ownership != HillOwner::HO_Red)
            && esc_team_can_attack_role(hill.role, HillOwner::HO_Red);
        const bool blue_can_attack = (hill.ownership != HillOwner::HO_Blue)
            && esc_team_can_attack_role(hill.role, HillOwner::HO_Blue);
        const bool neutral_can_be_attacked = (hill.ownership == HillOwner::HO_Neutral)
            && (esc_team_can_attack_role(hill.role, HillOwner::HO_Red)
                || esc_team_can_attack_role(hill.role, HillOwner::HO_Blue));

        const bool available = red_can_attack || blue_can_attack || neutral_can_be_attacked;

        const HillLockStatus desired_status = available ? HillLockStatus::HLS_Available : HillLockStatus::HLS_Locked;

        if (hill.lock_status != desired_status) {
            hill.lock_status = desired_status;
            any_change = true;
        }
    }

    if (any_change && rf::is_multi && rf::is_server) {
        for (auto& hill : g_koth_info.hills) {
            const Presence pres = sample_presence(hill);
            server_maybe_broadcast_state(hill, pres);
        }
    }
}

static void koth_apply_ownership(HillInfo& h, HillOwner new_owner, bool announce = true, HillOwner scoring_team = HillOwner::HO_Neutral)
{
    if (gt_is_rev() && new_owner == HillOwner::HO_Blue)
        return;

    if (h.ownership == new_owner)
        return;

    HillOwner old_owner = h.ownership;

    // Reset hill capture state
    h.ownership = new_owner;
    h.steal_dir = HillOwner::HO_Neutral;
    h.state = HillState::HS_Idle;
    h.capture_milli = 0;
    h.capture_progress = 0;
    h.hold_ms_accum = 0;

    //xlog::warn("[KOTH] {}: OWNER {} -> {}", h.name.c_str(), to_string(old_owner), to_string(new_owner));

    if (rf::is_server) {
        if (gt_is_rev() && new_owner == HillOwner::HO_Red) {
            h.lock_status = HillLockStatus::HLS_Permalocked; // permalock captured point
        }

        if (gt_is_rev()) {
            // REV: update respawns on per-point basis, then recalculate stage locks
            koth_update_respawn_points(&h);
            rev_recalculate_stage_locks();
        }
        else if (gt_is_esc()) {
            // ESC: stage locks first, then recompute all respawns
            esc_recalculate_stage_locks();
            esc_update_all_respawn_points();
        }
        else {
            // KOTH / DC: per-hill is fine
            koth_update_respawn_points(&h);
        }

        if (new_owner == HillOwner::HO_Red || new_owner == HillOwner::HO_Blue) {
            notify_capture_point_captured(h, new_owner);
        }
    
        if (announce) {
            //auto ids = on_capture_collect_player_ids_on_hill_for_team(h, new_owner);
            HillOwner reward_team = scoring_team;
            if (reward_team == HillOwner::HO_Neutral)
                reward_team = new_owner;

            std::vector<uint8_t> ids;
            if (reward_team == HillOwner::HO_Red || reward_team == HillOwner::HO_Blue)
                ids = on_capture_collect_player_ids_on_hill_for_team(h, reward_team);
            const uint8_t uid8 = static_cast<uint8_t>(std::clamp(h.hill_uid, 0, 255));
            af_send_koth_hill_captured_packet_to_all(uid8, new_owner, ids);
        }
    }
}

bool koth_set_capture_point_owner(rf::EventCapturePointHandler* handler, int owner, bool announce)
{
    if (!rf::is_multi || !rf::is_server)
        return false; // ignore on client

    HillInfo* hill = koth_find_hill_by_handler(handler);
    if (!hill)
        return false;

    auto owner_enum = static_cast<HillOwner>(owner);

    if (hill->ownership == owner_enum) {
        bool adjusted = false;
        if (gt_is_rev() && owner_enum == HillOwner::HO_Red && hill->lock_status != HillLockStatus::HLS_Permalocked) {
            hill->lock_status = HillLockStatus::HLS_Permalocked;
            koth_update_respawn_points(hill);
            rev_recalculate_stage_locks();
            adjusted = true;
        }

        return adjusted;
    }

    HillOwner previous_owner = hill->ownership;
    koth_apply_ownership(*hill, owner_enum, announce);
    return previous_owner != owner_enum;
}

bool esc_all_points_owned_by_one_team()
{
    if (!gt_is_esc() || g_koth_info.hills.empty())
        return false;

    bool any_red = false;
    bool any_blue = false;

    for (const auto& hill : g_koth_info.hills) {
        any_red |= (hill.ownership == HillOwner::HO_Red);
        any_blue |= (hill.ownership == HillOwner::HO_Blue);
    }

    return (any_red && !any_blue) || (any_blue && !any_red);
}

void update_hill_server_rev(HillInfo& h, int dt_ms)
{
    // only the currently available point can be captured
    if (h.lock_status != HillLockStatus::HLS_Available)
        return;

    // blue doesn't attack
    if (h.steal_dir == HillOwner::HO_Blue) {
        h.steal_dir = HillOwner::HO_Neutral;
    }

    const Presence pres = sample_presence(h);
    const bool red_only = (pres.red > 0 && pres.blue == 0);
    const bool blue_only = (pres.blue > 0 && pres.red == 0);
    const bool both = (pres.red > 0 && pres.blue > 0);
    const bool empty = (pres.red == 0 && pres.blue == 0);

    auto inc_rate = [&](int rate_per_sec, HillOwner team) {
        const int effective = effective_rate_per_sec(h, rate_per_sec, pres, team);
        h.capture_milli = std::min(100000, h.capture_milli + effective * dt_ms);
        h.capture_progress = static_cast<uint8_t>(h.capture_milli / 1000);
    };
    auto dec_rate = [&](int rate_per_sec, HillOwner team) {
        const int effective = effective_rate_per_sec(h, rate_per_sec, pres, team);
        h.capture_milli = std::max(0, h.capture_milli - effective * dt_ms);
        h.capture_progress = static_cast<uint8_t>(h.capture_milli / 1000);
        if (h.capture_milli == 0)
            h.steal_dir = HillOwner::HO_Neutral;
    };

    // contested
    if (both) {
        h.state = HillState::HS_Idle;
        return;
    }

    // slow decay of red's progress
    if (empty) {
        if (h.capture_milli > 0)
            dec_rate(g_koth_info.rules.drain_empty_rate, HillOwner::HO_Neutral);
        h.state = HillState::HS_Idle;
        return;
    }

    // make forward progress
    if (red_only) {
        if (gt_is_esc() && !esc_team_can_attack_role(h.role, HillOwner::HO_Red)) {
            h.state = HillState::HS_Idle;
            return;
        }
        if (h.steal_dir == HillOwner::HO_Neutral)
            h.steal_dir = HillOwner::HO_Red;
        h.state = HillState::HS_LeanRedGrowing;
        inc_rate(g_koth_info.rules.grow_rate, HillOwner::HO_Red);
        if (h.capture_milli >= 100000) {
            // on cap, permalock, unlock next
            koth_apply_ownership(h, HillOwner::HO_Red);
        }
        return;
    }

    // quick drain of red's progress
    if (blue_only) {
        if (gt_is_esc() && !esc_team_can_attack_role(h.role, HillOwner::HO_Blue)) {
            h.state = HillState::HS_Idle;
            return;
        }
        if (h.capture_milli > 0) {
            h.state = HillState::HS_LeanRedShrinking;
            dec_rate(g_koth_info.rules.drain_defended_rate, HillOwner::HO_Blue);
        }
        else {
            h.state = HillState::HS_Idle;
        }
        return;
    }

    // fallback
    h.state = HillState::HS_Idle;
    return;
}

void update_hill_server(HillInfo& h, int dt_ms)
{
    if (gt_is_esc() && h.lock_status != HillLockStatus::HLS_Available) {
        h.state = HillState::HS_Idle;
        return;
    }

    const Presence pres = sample_presence(h);
    const bool red_only = (pres.red > 0 && pres.blue == 0);
    const bool blue_only = (pres.blue > 0 && pres.red == 0);
    const bool both = (pres.red > 0 && pres.blue > 0);
    const bool empty = (pres.red == 0 && pres.blue == 0);

    if (gt_is_esc()) {
        if (red_only && !esc_team_can_attack_role(h.role, HillOwner::HO_Red)) {
            h.state = HillState::HS_Idle;
            return;
        }

        if (blue_only && !esc_team_can_attack_role(h.role, HillOwner::HO_Blue)) {
            h.state = HillState::HS_Idle;
            return;
        }
    }

    // milli-percent helpers: rate is %/sec, dt_ms is ms
    auto inc_rate = [&](int rate_per_sec, HillOwner team) {
        const int effective = effective_rate_per_sec(h, rate_per_sec, pres, team);
        h.capture_milli = std::min(100000, h.capture_milli + effective * dt_ms);
        h.capture_progress = static_cast<uint8_t>(h.capture_milli / 1000);
    };
    auto dec_rate = [&](int rate_per_sec, HillOwner team) {
        const int effective = effective_rate_per_sec(h, rate_per_sec, pres, team);
        h.capture_milli = std::max(0, h.capture_milli - effective * dt_ms);
        h.capture_progress = static_cast<uint8_t>(h.capture_milli / 1000);
        if (h.capture_milli == 0)
            h.steal_dir = HillOwner::HO_Neutral;
    };

    // handle scoring only during gameplay
    if (!gt_is_esc() && !gt_is_rev() && rf::gameseq_get_state() == rf::GameState::GS_GAMEPLAY) {
        if (h.ownership == HillOwner::HO_Red || h.ownership == HillOwner::HO_Blue) {
            const HillOwner opp = opposite(h.ownership);
            const bool opp_on = team_present(opp, pres);
            const bool hostile_progress = hostile_progress_present_server(h);

            if (!opp_on && !hostile_progress) { // if uncontested and no partial progress from opponent
                h.hold_ms_accum += dt_ms;
                while (h.hold_ms_accum >= g_koth_info.rules.ms_per_point) {
                    if (h.ownership == HillOwner::HO_Red) {
                        ++g_koth_info.red_team_score;
                        //xlog::warn("[KOTH] {}: RED +1 (total R={} B={})", h.name.c_str(), g_koth_info.red_team_score, g_koth_info.blue_team_score);
                    }
                    else {
                        ++g_koth_info.blue_team_score;
                        //xlog::warn("[KOTH] {}: BLUE +1 (total R={} B={})", h.name.c_str(), g_koth_info.red_team_score, g_koth_info.blue_team_score);
                    }
                    h.hold_ms_accum -= g_koth_info.rules.ms_per_point;
                }
            }
        }
    }

    // --- change-driven logging helper (compare to last tick) ---
    auto emit_change_logs = [&]() {
        /* if (h.dbg_last_red != pres.red || h.dbg_last_blue != pres.blue) {
            xlog::warn("[KOTH] {}: presence R={} B={}", h.name.c_str(), pres.red, pres.blue);
            h.dbg_last_red = pres.red;
            h.dbg_last_blue = pres.blue;
        }
        if (h.dbg_last_owner != h.ownership) {
            xlog::warn("[KOTH] {}: OWNER {} -> {}", h.name.c_str(), to_string(h.dbg_last_owner),
                       to_string(h.ownership));
            h.dbg_last_owner = h.ownership;
        }
        if (h.dbg_last_dir != h.steal_dir) {
            xlog::warn("[KOTH] {}: steal_dir -> {}", h.name.c_str(), to_string(h.steal_dir));
            h.dbg_last_dir = h.steal_dir;
        }
        if (h.dbg_last_state != h.state) {
            xlog::warn("[KOTH] {}: state {} -> {}", h.name.c_str(), to_string(h.dbg_last_state), to_string(h.state));
            h.dbg_last_state = h.state;
        }
        uint8_t bucket = static_cast<uint8_t>(h.capture_progress / 5);
        if (bucket != h.dbg_last_prog_bucket) {
            xlog::warn("[KOTH] {}: progress {}% (dir={}, owner={})", h.name.c_str(), int(h.capture_progress),
                       to_string(h.steal_dir), to_string(h.ownership));
            h.dbg_last_prog_bucket = bucket;
        }*/
    };

    // contested: freeze
    if (both) {
        h.state = HillState::HS_Idle;
        emit_change_logs();
        return;
    }

    // empty: bleed back to 0
    if (empty) {
        if (h.capture_milli > 0)
            dec_rate(g_koth_info.rules.drain_empty_rate, HillOwner::HO_Neutral);
        h.state = HillState::HS_Idle;
        emit_change_logs();
        return;
    }

    // pre-first-capture (neutral -> first owner)
    if (h.ownership == HillOwner::HO_Neutral) {
        const int grow = g_koth_info.rules.grow_rate;
        // reuse the defended drain as a "neutral flip clear" rate; add a separate rule if you want
        const int neutral_clear = g_koth_info.rules.drain_defended_rate;

        auto clear_then_grow = [&](HillOwner losing, HillOwner winning, HillState clear_state, HillState grow_state) {
            if (h.steal_dir == losing && h.capture_milli > 0) {
                // Clear the other team's partial progress first
                h.state = clear_state;
                dec_rate(neutral_clear, winning); // dec_rate sets steal_dir = Neutral on reaching 0
                return;                  // keep clearing until 0
            }

            if (h.steal_dir == HillOwner::HO_Neutral) {
                h.steal_dir = winning; // adopt winning side only when cleared
            }

            if (h.steal_dir == winning) {
                h.state = grow_state;
                inc_rate(grow, winning);
                if (h.capture_milli >= 100000) {
                    koth_apply_ownership(h, winning);
                }
            }
        };

        if (red_only) {
            clear_then_grow(HillOwner::HO_Blue, HillOwner::HO_Red, HillState::HS_LeanBlueShrinking, HillState::HS_LeanRedGrowing);
        }
        else if (blue_only) {
            clear_then_grow(HillOwner::HO_Red, HillOwner::HO_Blue, HillState::HS_LeanRedShrinking, HillState::HS_LeanBlueGrowing);
        }

        emit_change_logs();
        return;
    }

    // owned paths
    const HillOwner owner = h.ownership;
    const HillOwner attackers = opposite(owner);
    const bool attackers_only = team_present(attackers, pres) && !team_present(owner, pres);
    const bool owner_only = team_present(owner, pres) && !team_present(attackers, pres);

    if (attackers_only) {
        if (h.steal_dir != attackers && h.capture_milli > 0) {
            // drain hostile progress quickly
            h.state = (owner == HillOwner::HO_Red) ? HillState::HS_LeanRedShrinking : HillState::HS_LeanBlueShrinking;
            dec_rate(g_koth_info.rules.drain_defended_rate, attackers);
        }
        else {
            h.steal_dir = attackers;
            h.state = (attackers == HillOwner::HO_Red) ? HillState::HS_LeanRedGrowing : HillState::HS_LeanBlueGrowing;
            inc_rate(g_koth_info.rules.grow_rate, attackers);
            if (h.capture_milli >= 100000) {
                //koth_apply_ownership(h, attackers);
                if (g_koth_info.rules.require_neutral_to_capture) {
                    koth_apply_ownership(h, HillOwner::HO_Neutral, true, attackers);
                }
                else {
                    koth_apply_ownership(h, attackers);
                }
                emit_change_logs();
                return;
            }
        }
        emit_change_logs();
        return;
    }

    if (owner_only) {
        if (h.capture_milli > 0) {
            h.state = (owner == HillOwner::HO_Red) ? HillState::HS_LeanBlueShrinking : HillState::HS_LeanRedShrinking;
            dec_rate(g_koth_info.rules.drain_defended_rate, owner);
        }
        else {
            h.state = HillState::HS_Idle;
        }
        emit_change_logs();
        return;
    }

    // fallback
    h.state = HillState::HS_Idle;
    emit_change_logs();
}

static void update_hill_client_predict(HillInfo& h, int dt_ms)
{
    const Presence pres = sample_presence(h);

    const bool red_only = (pres.red > 0 && pres.blue == 0);
    const bool blue_only = (pres.blue > 0 && pres.red == 0);
    const bool both = (pres.red > 0 && pres.blue > 0);
    const bool empty = (pres.red == 0 && pres.blue == 0);

    if (gt_is_esc()) {
        if (red_only && !esc_team_can_attack_role(h.role, HillOwner::HO_Red)) {
            h.state = HillState::HS_Idle;
            return;
        }

        if (blue_only && !esc_team_can_attack_role(h.role, HillOwner::HO_Blue)) {
            h.state = HillState::HS_Idle;
            return;
        }
    }

    auto inc_rate = [&](int rate_per_sec, HillOwner team) {
        const int effective = effective_rate_per_sec(h, rate_per_sec, pres, team);
        h.capture_milli = std::min(100000, h.capture_milli + effective * dt_ms);
        h.capture_progress = static_cast<uint8_t>(h.capture_milli / 1000);
    };
    auto dec_rate = [&](int rate_per_sec, HillOwner team) {
        const int effective = effective_rate_per_sec(h, rate_per_sec, pres, team);
        h.capture_milli = std::max(0, h.capture_milli - effective * dt_ms);
        h.capture_progress = static_cast<uint8_t>(h.capture_milli / 1000);
        if (h.capture_milli == 0)
            h.steal_dir = HillOwner::HO_Neutral;
    };

    if (both) {
        h.state = HillState::HS_Idle;
        return;
    }
    if (empty) {
        if (h.capture_milli > 0)
            dec_rate(g_koth_info.rules.drain_empty_rate, HillOwner::HO_Neutral);
        h.state = HillState::HS_Idle;
        return;
    }

    // Neutral: clear old side’s partial first, then grow new side
    if (h.ownership == HillOwner::HO_Neutral) {
        const int grow = g_koth_info.rules.grow_rate;
        const int neutral_clear = g_koth_info.rules.drain_defended_rate;

        auto clear_then_grow = [&](HillOwner losing, HillOwner winning, HillState clear_state, HillState grow_state) {
            if (h.steal_dir == losing && h.capture_milli > 0) {
                h.state = clear_state;
                dec_rate(neutral_clear, winning);
                return;
            }
            if (h.steal_dir == HillOwner::HO_Neutral)
                h.steal_dir = winning;

            if (h.steal_dir == winning) {
                h.state = grow_state;
                inc_rate(grow, winning);
                if (h.capture_milli >= 100000) {
                    // Local flip; no announce
                    koth_apply_ownership(h, winning, false);
                }
            }
        };

        if (red_only) {
            clear_then_grow(HillOwner::HO_Blue, HillOwner::HO_Red, HillState::HS_LeanBlueShrinking, HillState::HS_LeanRedGrowing);
        }
        else if (blue_only) {
            clear_then_grow(HillOwner::HO_Red, HillOwner::HO_Blue, HillState::HS_LeanRedShrinking, HillState::HS_LeanBlueGrowing);
        }
        return;
    }

    // Owned: attackers steal, owner defends
    const HillOwner owner = h.ownership;
    const HillOwner attackers = opposite(owner);
    const bool attackers_only =
        (attackers == HillOwner::HO_Red) ? (pres.red > 0 && pres.blue == 0) : (pres.blue > 0 && pres.red == 0);
    const bool owner_only =
        (owner == HillOwner::HO_Red) ? (pres.red > 0 && pres.blue == 0) : (pres.blue > 0 && pres.red == 0);

    if (attackers_only) {
        if (h.steal_dir != attackers && h.capture_milli > 0) {
            h.state = (owner == HillOwner::HO_Red) ? HillState::HS_LeanRedShrinking : HillState::HS_LeanBlueShrinking;
            dec_rate(g_koth_info.rules.drain_defended_rate, attackers);
        }
        else {
            h.steal_dir = attackers;
            h.state = (attackers == HillOwner::HO_Red) ? HillState::HS_LeanRedGrowing : HillState::HS_LeanBlueGrowing;
            inc_rate(g_koth_info.rules.grow_rate, attackers);
            if (h.capture_milli >= 100000) {
                koth_apply_ownership(h, attackers, false);
            }
        }
        return;
    }

    if (owner_only) {
        if (h.capture_milli > 0) {
            h.state = (owner == HillOwner::HO_Red) ? HillState::HS_LeanBlueShrinking : HillState::HS_LeanRedShrinking;
            dec_rate(g_koth_info.rules.drain_defended_rate, owner);
        }
        else {
            h.state = HillState::HS_Idle;
        }
        return;
    }

    // fallback (shouldn't hit due to early returns)
    h.state = HillState::HS_Idle;
}

static void update_hill_client_predict_rev(HillInfo& h, int dt_ms)
{
    // protection, should never happen
    if (h.steal_dir == HillOwner::HO_Blue)
        h.steal_dir = HillOwner::HO_Neutral;

    const Presence pres = sample_presence(h);
    const bool red_only  = (pres.red  > 0 && pres.blue == 0);
    const bool blue_only = (pres.blue > 0 && pres.red  == 0);
    const bool both      = (pres.red  > 0 && pres.blue > 0);
    const bool empty     = (pres.red  == 0 && pres.blue == 0);

    auto inc_rate = [&](int rate_per_sec, HillOwner team) {
        const int effective = effective_rate_per_sec(h, rate_per_sec, pres, team);
        h.capture_milli = std::min(100000, h.capture_milli + effective * dt_ms);
        h.capture_progress = static_cast<uint8_t>(h.capture_milli / 1000);
    };
    auto dec_rate = [&](int rate_per_sec, HillOwner team) {
        const int effective = effective_rate_per_sec(h, rate_per_sec, pres, team);
        h.capture_milli = std::max(0, h.capture_milli - effective * dt_ms);
        h.capture_progress = static_cast<uint8_t>(h.capture_milli / 1000);
        if (h.capture_milli == 0) h.steal_dir = HillOwner::HO_Neutral;
    };

    if (both) { h.state = HillState::HS_Idle; return; }

    if (empty) {
        if (h.capture_milli > 0) dec_rate(g_koth_info.rules.drain_empty_rate, HillOwner::HO_Neutral);
        h.state = HillState::HS_Idle;
        return;
    }

    if (red_only) {
        if (h.steal_dir == HillOwner::HO_Neutral) h.steal_dir = HillOwner::HO_Red;
        h.state = HillState::HS_LeanRedGrowing;
        inc_rate(g_koth_info.rules.grow_rate, HillOwner::HO_Red);
        if (h.capture_milli >= 100000) {
            // flip locally, will be confirmed by server
            koth_apply_ownership(h, HillOwner::HO_Red, false);
        }
        return;
    }

    if (blue_only) {
        // blue presence only drains red progress
        if (h.capture_milli > 0) {
            h.state = HillState::HS_LeanRedShrinking;
            dec_rate(g_koth_info.rules.drain_defended_rate, HillOwner::HO_Blue);
        } else {
            h.state = HillState::HS_Idle;
        }
        return;
    }

    h.state = HillState::HS_Idle;
}

static void koth_client_predict_tick(int dt_ms)
{
    // client-only; server is authoritative
    if (!rf::is_multi || rf::is_server || rf::is_dedicated_server)
        return;

    // score prediction in KOTH/DC
    if (gt_is_koth() || gt_is_dc()) {
        for (auto& h : g_koth_info.hills) {
            if (h.lock_status != HillLockStatus::HLS_Available)
                continue; // hill is locked

            if (h.ownership == HillOwner::HO_Neutral) {
                h.client_hold_ms_accum = 0;
                continue;
            }

            const bool opp_on = (h.ownership == HillOwner::HO_Red) ? (h.net_last_blue > 0)
                : (h.ownership == HillOwner::HO_Blue) ? (h.net_last_red > 0)
                : false;

            const HillOwner attackers = opposite(h.ownership);
            const bool hostile_progress = (h.steal_dir == attackers) && (h.capture_progress > 0);

            if (rf::gameseq_get_state() == rf::GameState::GS_GAMEPLAY) {
                if (!opp_on && !hostile_progress) {
                    h.client_hold_ms_accum += dt_ms;
                    while (h.client_hold_ms_accum >= g_koth_info.rules.ms_per_point) {
                        if (h.ownership == HillOwner::HO_Red)
                            ++g_koth_info.red_team_score;
                        else
                            ++g_koth_info.blue_team_score;
                        h.client_hold_ms_accum -= g_koth_info.rules.ms_per_point;
                    }
                }
                else {
                    h.client_hold_ms_accum = 0;
                }
            }
        }
    }

    // predict capture percentage clientside
    for (auto& h : g_koth_info.hills) {
        if (h.lock_status != HillLockStatus::HLS_Available)
            continue; // locked

        if (gt_is_rev()) {
            update_hill_client_predict_rev(h, dt_ms);
        }
        else {
            update_hill_client_predict(h, dt_ms);
        }
    }
}

void koth_force_broadcast_all_hill_states() {
    if (!rf::is_multi || !rf::is_dedicated_server || !rf::is_server || !multi_is_game_type_with_hills())
        return;

    for (auto& h : g_koth_info.hills) {
        const Presence pres = sample_presence(h);
        server_maybe_broadcast_state(h, pres);
    }
}

void koth_do_frame() // fires every frame on both server and client
{
    if (!rf::is_multi || !multi_is_game_type_with_hills())
        return;

    if (rf::gameseq_get_state() != rf::GameState::GS_GAMEPLAY)
        return;

    // server tick
    if (rf::is_dedicated_server || rf::is_server) {
        static int last_srv = rf::timer_get(1000);
        const int now_srv = rf::timer_get(1000);
        int dt_srv = now_srv - last_srv;
        if (dt_srv > 0) {
            last_srv = now_srv;
            const int dt_ms = std::clamp(dt_srv, 0, 250);

            for (auto& h : g_koth_info.hills) {
                // Only need to update hills if they are available
                if (h.lock_status == HillLockStatus::HLS_Available) {
                    if (gt_is_rev()) {
                        update_hill_server_rev(h, dt_ms);
                    }
                    else {
                        update_hill_server(h, dt_ms);
                    }
                }

                // But, after the authoritative update, broadcast state if something relevant changed (including availability)
                const Presence pres = sample_presence(h);
                server_maybe_broadcast_state(h, pres);
            }
        }
    }

    // client prediction tick
    if (!rf::is_server && !rf::is_dedicated_server) {
        static int last_cli = rf::timer_get(1000);
        const int now_cli = rf::timer_get(1000);
        int dt_cli = now_cli - last_cli;
        if (dt_cli > 0) {
            last_cli = now_cli;
            const int dt_ms = std::clamp(dt_cli, 0, 250);
            koth_client_predict_tick(dt_ms);
        }
    }

    // play progress sounds locally
    if (!rf::is_dedicated_server) {
        if (rf::local_player && !multi_spectate_is_spectating() && multi_is_game_type_with_hills() && rf::gameseq_get_state() == rf::GameState::GS_GAMEPLAY) {
            const bool local_is_blue = (rf::local_player->team != 0);
            const HillOwner local_team_owner = local_is_blue ? HillOwner::HO_Blue : HillOwner::HO_Red;
            const HillState enemy_growing_state = local_is_blue ? HillState::HS_LeanRedGrowing : HillState::HS_LeanBlueGrowing;
            const HillState my_growing_state = local_is_blue ? HillState::HS_LeanBlueGrowing : HillState::HS_LeanRedGrowing;
            bool any_local_owned_enemy_progress = false;

            for (auto& h : g_koth_info.hills) {
                if (h.hill_uid < 0)
                    continue;

                rf::Vector3 hill_pos{};
                const bool have_pos = hill_sound_position(h, hill_pos);

                const bool should_play_cap_gain =
                    (h.state == my_growing_state) &&
                    (h.capture_milli < 100000) &&
                    (!gt_is_rev() || (gt_is_rev() && !local_is_blue));

                // decide if play alarm
                // KOTH/REV = when capturing from neutral or my team, DC = only when capturing from my team
                const bool ours_or_neutral =
                    (h.ownership == local_team_owner) || ((gt_is_koth() || gt_is_rev()) && (h.ownership == HillOwner::HO_Neutral));

                const bool play_alarm = ours_or_neutral && h.state == enemy_growing_state;

                if (play_alarm) {
                    any_local_owned_enemy_progress = true;
                }

                // play hum while gaining capture progress for our team
                if (should_play_cap_gain) {
                    if (!h.cap_gain_sfx_playing) {
                        h.cap_gain_sfx_handle =
                            have_pos ? rf::snd_play_3d(get_custom_sound_id(4), hill_pos, 5.0f, rf::Vector3{}, 0)
                                     : rf::snd_play(get_custom_sound_id(4), 0, 0.0, 5.0);
                        h.cap_gain_sfx_playing = true;
                    }
                }
                else if (h.cap_gain_sfx_playing) {
                    if (h.cap_gain_sfx_handle > -1) {
                        rf::snd_stop(h.cap_gain_sfx_handle);
                    }

                    h.cap_gain_sfx_playing = false;
                    h.cap_gain_sfx_handle = -1;
                }
            }

            // play alarm if enemy is capturing our owned hill
            if (any_local_owned_enemy_progress) {
                if (!g_local_contest_alarm_cooldown.valid() || g_local_contest_alarm_cooldown.elapsed()) {
                    rf::snd_play(g_cap_alarm_sound_id, 0, 0.0, 1.0);
                    g_local_contest_alarm_cooldown.set(950);
                }
            }
            else if (g_local_contest_alarm_cooldown.valid() && g_local_contest_alarm_cooldown.elapsed()) {
                g_local_contest_alarm_cooldown.invalidate();
            }
        }
        else {
            stop_hill_sounds();
        }
    }
}

static int build_hills_from_capture_point_events()
{
    stop_hill_sounds();
    g_koth_info.hills.clear();

    const auto gt = rf::multi_get_game_type();

    if (!multi_game_type_has_hills(gt)) // only build hills in modes that have hills
        return int(g_koth_info.hills.size());

    int game_type_max_hills =
        gt == rf::NetGameType::NG_TYPE_KOTH ? 1 : 32;

    auto events = find_all_events_by_type(rf::EventType::Capture_Point_Handler);
    std::unordered_set<int> seen_uids;

    int idx = 0;
    for (rf::Event* e : events) {
        if (!e || int(g_koth_info.hills.size()) >= game_type_max_hills) // respect gametype max hills
            continue;

        auto* cp = static_cast<rf::EventCapturePointHandler*>(e);

        // find trigger_uid from event links
        if (cp->trigger_uid < 0 && !e->links.empty()) {
            for (int handle : e->links) {
                if (auto* o = rf::obj_from_handle(handle)) {
                    if (o->type == rf::ObjectType::OT_TRIGGER) {
                        cp->trigger_uid = o->uid;
                        break;
                    }
                }
            }
        }

        // resolve trigger pointer
        rf::Trigger* trig = koth_resolve_trigger_from_uid(cp->trigger_uid);
        if (!trig) {
            xlog::warn("KOTH: Capture point '{}' has invalid trigger UID {}, skipping", cp->name.c_str(), cp->trigger_uid);
            continue;
        }

        // fix a case where a mapper specified the same trigger for multiple capture point handlers
        if (!seen_uids.insert(cp->trigger_uid).second) {
            xlog::warn("KOTH: Duplicate capture point for trigger UID {}, ignoring subsequent entries", cp->trigger_uid);
            continue;
        }

        HillInfo h{};
        h.hill_uid = ++idx;
        // use name if set by mapper, if not name it "Hill" if only one, or "Control Point X" if multiple
        h.name = cp->name.empty() ? (events.size() <= 1 ? "Hill" : std::format("Point {}", idx)) : cp->name;
        h.trigger_uid = cp->trigger_uid;
        h.trigger = trig;
        h.outline_offset = cp->outline_offset;
        h.capture_rate = cp->capture_rate;
        if (cp->position >= static_cast<int>(HillRole::HR_Center)
            && cp->position <= static_cast<int>(HillRole::HR_BlueForward)) {
            h.role = static_cast<HillRole>(cp->position);
        }
        h.stage = cp->stage;
        h.handler = cp;
        h.ownership = HillOwner::HO_Neutral;
        h.state = HillState::HS_Idle;
        h.lock_status = HillLockStatus::HLS_Available;
        h.capture_progress = 0;
        h.capture_milli = 0;
        h.hold_ms_accum = 0;

        // build vector of respawn points associated with hill
        if (!e->links.empty()) {
            for (int linked_uid : e->links) {
                if (auto* rp = get_alpine_respawn_point_by_uid(linked_uid)) {
                    h.mp_spawn_uids.push_back(rp->uid);
                }
            }
        }

        g_koth_info.hills.push_back(std::move(h));
    }

    // Sort for REV by stage
    if (gt == rf::NetGameType::NG_TYPE_REV && !g_koth_info.hills.empty()) {
        std::sort(g_koth_info.hills.begin(), g_koth_info.hills.end(),
            [](const HillInfo& a, const HillInfo& b) { return a.stage < b.stage;
            });
    }

    // Sort for ESC by role
    else if (gt == rf::NetGameType::NG_TYPE_ESC && !g_koth_info.hills.empty()) {
        auto esc_role_index = [](HillRole role) -> int {
            switch (role) {
            case HillRole::HR_RedBase:
                return 0;
            case HillRole::HR_RedForward:
                return 1;
            case HillRole::HR_Center:
                return 2;
            case HillRole::HR_BlueForward:
                return 3;
            case HillRole::HR_BlueBase:
                return 4;
            default:
                return std::numeric_limits<int>::max();
            }
        };

        std::sort(g_koth_info.hills.begin(), g_koth_info.hills.end(), [&](const HillInfo& a, const HillInfo& b) {
            const int ai = esc_role_index(a.role);
            const int bi = esc_role_index(b.role);

            if (ai != bi) {
                return ai < bi;
            }

            return a.hill_uid < b.hill_uid;
        });
    }

    esc_apply_initial_ownerships();
    rev_recalculate_stage_locks();
    esc_recalculate_stage_locks();

    for (auto& h : g_koth_info.hills) {
        koth_update_respawn_points(&h);
    }

    //xlog::warn("GT: discovered {} capture point(s)", int(g_koth_info.hills.size()));
    return int(g_koth_info.hills.size());
}

void hill_mode_level_init()
{
    clear_koth_name_textures(); // clear hill labels
    stop_hill_sounds();
    multi_koth_reset_scores();
    g_cap_alarm_sound_id = rf::snd_pc_find_by_name("Alarm_02.wav");
}

void hill_mode_level_init_post()
{
    if (!rf::is_multi)
        return;

    const int n = build_hills_from_capture_point_events(); // need events to be loaded before building hills

    //xlog::warn("KOTH: {} capture points found in this map, gt {}", n, static_cast<int>(rf::netgame.type));
}

void multi_level_init_post_gametypes()
{
    hill_mode_level_init_post();
}

// pre level being loaded
CodeInjection multi_level_init_gametypes_injection{
    0x0046E466,
    [](auto& regs) {
        hill_mode_level_init();
    },
};

// sync scores every 5ms during gameplay to account for clientside prediction errors
CodeInjection send_team_score_server_do_frame_patch{
    0x0046E5B4,
    [](auto& regs) {
        if (multi_is_team_game_type() && !gt_is_rev()) {
            regs.eip = 0x0046E5C3; // send team_scores packet
        }
    },
};

void send_koth_hill_state_packet_to_player(rf::Player* pp)
{
    for (auto& h : g_koth_info.hills) {
        const Presence pres = sample_presence(h);
        af_send_koth_hill_state_packet(pp, h, pres);
    }
}

// sync hill state(s) and scores for late joiners
CodeInjection send_team_score_state_info_patch{
    0x0048183F,
    [](auto& regs) {
        auto game_type = rf::multi_get_game_type();
        // send hill state packet on join
        if (multi_game_type_has_hills(game_type)) {
            if (rf::Player* pp = regs.edi) {
                send_koth_hill_state_packet_to_player(pp);
            }
        }

        // send team_scores packet
        if (multi_game_type_is_team_type(game_type) && !gt_is_rev()) {
            regs.eip = 0x00481859; 
        }
    },
};

// sync hill state(s) and scores on level change
CodeInjection send_team_score_change_level_patch{
    0x0047BF97,
    [](auto& regs) {
        auto game_type = rf::multi_get_game_type();
        if (multi_game_type_is_team_type(game_type)) {
            // send a hill state update on match end to ensure all players have accurate scores
            if (multi_game_type_has_hills(game_type)) {
                rf::Player* pp = regs.eax;
                if (pp)
                    send_koth_hill_state_packet_to_player(pp);
            }

            //regs.eip = 0x0047BFA6; // continue to send team_scores packet (unnecessary, not processed by clients)
        }
    },
};

// send new gametype scores in the stock team_scores packet
CodeInjection send_team_score_patch{
    0x00472151,
    [](auto& regs) {
        if (gt_is_koth() || gt_is_dc()) {
            // both int16_t on the wire
            const uint16_t red_score = (uint16_t)std::clamp(multi_koth_get_red_team_score(), 0, 0xFFFF);
            const uint16_t blue_score = (uint16_t)std::clamp(multi_koth_get_blue_team_score(), 0, 0xFFFF);
            regs.si = red_score;
            regs.ax = blue_score;
            regs.eip = 0x00472176; // use stock game packet send
        }
    },
};

// receive new gametype scores in the stock team_scores packet
CodeInjection process_team_score_patch{
    0x0047221D,
    [](auto& regs) {
        if (gt_is_koth() || gt_is_dc()) {
            // both int16_t on the wire
            int red_score = regs.esi;
            int blue_score = regs.edi;
            multi_koth_set_red_team_score(red_score);
            multi_koth_set_blue_team_score(blue_score);
        }
    },
};

CodeInjection player_fpgun_load_meshes_patch{
    0x004AE5CD,
    [](auto& regs) {
        if (multi_is_team_game_type()) {
            regs.eip = 0x004AE5E3; // set team for fpgun load
        }
    },
};

CodeInjection player_create_entity_team_skins_patch{
    0x004A44E1,
    [](auto& regs) {
        if (multi_is_team_game_type()) {
            regs.eip = 0x004A44F7; // set team for player entity create
        }
    },
};

CodeInjection multi_hud_render_target_name_color_patch{
    0x0047806F,
    [](auto& regs) {
        // base game logic path draws name green if mode != CTF AND != TDM
        if (multi_is_team_game_type()) {
            regs.eip = 0x0047807E; // draw team colored name
        }
    },
};

CallHook<int()> multi_get_game_type_non_team_mode_hook{
    {
        0x004A4234, // player_create_entity for team coloured player models
        0x004808EF, // multi_spawn_player_server_side for "You are on team X" for listen server host
        0x00443FC9, // chat_add_msg for [Team] prefix on team chat
        0x00444A93, // multi_chat_say_show
        0x00476CA8, // multi_hud_level_init for "You are on team X" for client
        0x004827E1, // multi_get_new_player_team on join
    },
    []() {
        // all are calls to multi_get_game_type in the stock game
        // stock game uses "!multi_get_game_type()" to check for non-team modes because DM is 0
        return multi_is_team_game_type() ? 1 : 0;
    },
};

void gametype_do_patch()
{
    // index rfl files for new gamemodes when opening listen server create menu
    listen_server_map_list_filename_contains_hook.install();

    // patch gametype name references to use new table
    write_mem<uint32_t>((0x0044A8D3) + 3, (uint32_t)(uintptr_t)&g_af_gametype_names[0]); // multi_join_game_compare_func
    write_mem<uint32_t>((0x0044A8FB) + 3, (uint32_t)(uintptr_t)&g_af_gametype_names[0]); // multi_join_game_compare_func
    write_mem<uint32_t>((0x0044C1EB) + 3, (uint32_t)(uintptr_t)&g_af_gametype_names[0]); // multi_join_game_render_row
    write_mem<uint32_t>((0x0044C227) + 3, (uint32_t)(uintptr_t)&g_af_gametype_names[0]); // multi_join_game_render_row
    write_mem<uint32_t>((0x0044C724) + 3, (uint32_t)(uintptr_t)&g_af_gametype_names[0]); // multi_join_game_init

    // patch listen server create menu gametype select field to use new table
    const uintptr_t base = (uintptr_t)&g_af_gametype_names[0];
    const uintptr_t end = base + sizeof(g_af_gametype_names);
    const uintptr_t kMovBase = 0x004459B1;
    const uintptr_t kCmpEnd = 0x004459CE;
    write_mem<uint32_t>(kMovBase + 1, (uint32_t)base);
    write_mem<uint32_t>(kCmpEnd + 2, (uint32_t)end);

    // team_score packet expansion to support new team gametypes
    send_team_score_server_do_frame_patch.install();
    send_team_score_state_info_patch.install();
    send_team_score_change_level_patch.install();
    send_team_score_patch.install();
    process_team_score_patch.install();

    // load team coloured fpgun arms in new team gametypes
    player_fpgun_load_meshes_patch.install();

    // fix bug where team fpgun skins not loaded for listen server hosts if a
    // custom gamemode is the first loaded mode after game launch
    player_create_entity_team_skins_patch.install();

    // colour target player names in new team gametypes
    multi_hud_render_target_name_color_patch.install();

    // initialize new gametype level settings
    multi_level_init_gametypes_injection.install();

    // handle new non-team modes
    multi_get_game_type_non_team_mode_hook.install();
}
