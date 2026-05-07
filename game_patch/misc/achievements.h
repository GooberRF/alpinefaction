#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include "../rf/trigger.h"
#include "../rf/event.h"
#include "../rf/clutter.h"

enum class AchievementName : int
{
    SecretFusion = 1,
    StartTraining = 2,
    FinishTraining = 3,
    FinishCampaignEasy = 4,
    FinishCampaignMedium = 5,
    FinishCampaignHard = 6,
    FinishCampaignImp = 7,
    KillFish = 8,
    KillGuards = 9,
    LockedInTram = 10,
    MissShuttle = 11,
    Ventilation = 12,
    DestroyGeothermal = 13,
    DestroyTrashBot = 14,
    MeetCapek = 15,
    CapekVoiceEnd = 16,
    KillSnake = 17,
    GoSpaceStation = 18,
    ComputersSpaceStation = 19,
    DestroySpaceStation = 20,
    MercJail = 21,
    KillMasako = 22,
    UseFlashlight = 23,
    UnderwaterSub = 24,
    KillDrone = 25,
    KillCapekFlamethrower = 26,
    DropCorpse = 27,
    UseMedic = 28,
    DupeC4 = 29,
    ViewMonitor = 30,
    EnterAesir = 31,
    EnterSub = 32,
    EnterAPC = 33,
    EnterJeep = 34,
    EnterDriller = 35,
    SecretSub = 36,
    AdminFountain = 37,
    HendrixHackDoor = 38,
    SubCraneButton = 39,
    JeepWater = 40,
    DestroyPumpStations = 41,
    StartCampaign = 42,
    FastBomb = 43,
    FarKill = 44,
    CoffeeMakers = 45,
    SeparateGeometry = 46,
    GibEnemy = 47,
    RunOver = 48,
    SaveBarracksMiner = 49,
    SaveMinesMiner = 50,
    MedLabStealth = 51,
    AdminStealth = 52,
    AdminMinerBerserk = 53,
    KillDavis = 54,
    KillCiviliansAdmin = 55,
    StuckEntities = 61,
    SecretStash = 62,
    DropAPCBridge = 63,
    Toilets = 64,
    Microwaves = 65,
    DrillBit = 66,
    MinerCapekPrison = 67,
    MissileLaunchSabotage = 68,
    MedMax = 69,
    Robots100 = 70,
    Guards500 = 71,
    Mercs250 = 72,
    GlassHouseShatter = 73,
    GlassBreaks = 74,
    ShatterShield = 75,
    ShootHelmets = 76,
    LockedRockets = 77,
    GeoCrater = 78,
    Flame50 = 79,
    Pistol50 = 80,
    Turret50 = 81,
    KillSubmarines = 82,
    Shotgun50 = 83,
    Sniper50 = 84,
    Rail50 = 85,
    PR50 = 86,
    HMG50 = 87,
    SMG50 = 88,
    Baton25 = 89,
    AR50 = 90,
};

enum class AchievementType : int
{
    basic,                  // activates on client immediately, should only be used for binary achievements
    ff_authoritative        // activates based on FF response, should be used for achievements with counters
};

enum class AchievementCategory : int
{
    general,                // non-gameplay events, or events that don't fit anywhere else
    //statistics,             // derived from kill stats from FF (currently unused)
    singleplayer,           // singleplayer mode, not necessarily base campaign
    multiplayer,            // multiplayer mode
    base_campaign,          // base campaign
    kava,                   // kava mod campaign
};

struct Achievement
{
    int root_uid = -1;
    std::string name = "";
    //std::string icon = ""; // af_achico_000.tga
    AchievementCategory category = AchievementCategory::general;
    AchievementType type = AchievementType::basic;
    int pending_count = 0; // local storage, pending sending to ff
    bool unlocked = false; // if true, don't attempt to send to ff again
    bool notified = false; // if true, don't notify them again
};

struct LoggedKill
{
    int entity_uid = -1; // not sent to ff, used to identify duplicates
    std::string rfl_filename = "";
    std::string tc_mod_name = "";
    std::string entity_class_name = "";
    int damage_type = -1;
    int likely_weapon_id = -1;
};

struct LoggedUse
{
    std::string rfl_filename = "";
    std::string tc_mod_name = "";
    int used_uid = -1;
    int type = 1; // 1 = object uid, 2 = face id
};

struct AchievementStateInfo
{
    int glass_house_shatters = 0;
    std::vector<int> train01_med_max_uids;
    rf::GameDifficultyLevel game_start_difficulty = rf::GameDifficultyLevel::DIFFICULTY_EASY; // only set on l1s1
};

class AchievementManager
{
public:
    static AchievementManager& get_instance()
    {
        static AchievementManager instance;
        return instance;
    }

    void initialize();
    void grant_achievement(AchievementName achievement, int count);
    bool is_unlocked(int uid) const;
    void send_update_to_ff();
    void sync_with_ff();
    void add_key_to_ff_update_map();

    // Main-thread callbacks invoked by the fflink::achievements client when its
    // HTTP exchange completes. Apply the parsed result to local achievement state.
    // initial_sync = true for the bootstrap GET; false for incremental update POSTs.
    void apply_ff_unlocks(const std::vector<int>& unlocked_uids, bool initial_sync);

    const std::unordered_map<AchievementName, Achievement>& get_achievements() const {
    return achievements;
    }

    std::unordered_map<AchievementName, Achievement>& get_achievements_mutable() {
        return achievements;
    }

    std::vector<LoggedKill>& get_logged_kills_mutable() {
        return logged_kills;
    }

    std::vector<LoggedUse>& get_logged_uses_mutable() {
        return logged_uses;
    }

    void log_kill(int entity_uid, const std::string& rfl_filename, const std::string& tc_mod,
        const std::string& class_name, int damage_type, int likely_weapon);

    void log_use(int used_uid, const std::string& rfl_filename, const std::string& tc_mod, int type);

private:
    std::unordered_map<AchievementName, Achievement> achievements;
    std::vector<LoggedKill> logged_kills;
    std::vector<LoggedUse> logged_uses;

    void show_notification(Achievement& achievement);
    const Achievement* get_achievement(AchievementName achievement_name);
};

bool is_achievement_system_initialized();

void grant_achievement(AchievementName achievement, int count = 1);
void grant_achievement_sp(AchievementName achievement, int count = 1);
//void log_kill(int entity_uid, std::string& class_name, int damage_type, int likely_weapon);
void achievement_check_trigger(rf::Trigger* trigger);
void achievement_check_event(rf::Event* event);
void achievement_check_entity_death(rf::Entity* entity);
void achievement_check_clutter_death(rf::Clutter* clutter);
void achievement_check_item_picked_up(rf::Item* item);
void achievement_player_killed_entity(rf::Entity* entity, int lethal_damage_type, int killer_handle);

void achievement_system_do_frame();
void clear_achievement_notification();
void reset_achievement_state_info();
