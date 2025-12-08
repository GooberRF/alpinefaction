#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <thread>
#include <sstream>
#include <xlog/xlog.h>
#include <patch_common/CodeInjection.h>
#include <common/HttpRequest.h>
#include "../rf/os/console.h"
#include "../rf/trigger.h"
#include "../rf/player/camera.h"
#include "../rf/clutter.h"
#include "../rf/gameseq.h"
#include "../rf/entity.h"
#include "../rf/level.h"
#include "../rf/event.h"
#include "../rf/weapon.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../os/console.h"
#include "../sound/sound.h"
#include "achievements.h"
#include "alpine_settings.h"
#include "misc.h"

bool achievement_system_initialized = false;
bool synced_with_ff = false;
AchievementStateInfo achievement_state_info;
bool achievement_box_visible = true;
std::string achievement_box_name;
int achievement_box_icon = -1;
rf::TimestampRealtime achievement_box_timestamp;
rf::TimestampRealtime achievement_async_ff_timer;
int achievement_async_ff_key;
std::string achievement_async_ff_string;
constexpr int achievement_async_ff_interval = 5000; // ms

void AchievementManager::initialize()
{
    if (g_game_config.fflink_token.value().empty()) {
        std::string msg = "Achievements are unavailable because Alpine Faction is not linked to a FactionFiles account!";
        rf::console::printf("%s", msg);
        return;
    }

    std::vector<std::pair<AchievementName, Achievement>> predefined_achievements = {
        {AchievementName::SecretFusion, {1, "Explosive Discovery", AchievementCategory::base_campaign}},
        {AchievementName::StartTraining, {2, "Welcome to Mars", AchievementCategory::base_campaign}},
        {AchievementName::FinishTraining, {3, "Tools of the Trade", AchievementCategory::base_campaign}},
        {AchievementName::FinishCampaignEasy, {4, "Too Easy!", AchievementCategory::base_campaign}},
        {AchievementName::FinishCampaignMedium, {5, "Moderate Success", AchievementCategory::base_campaign}},
        {AchievementName::FinishCampaignHard, {6, "Tough as Nails", AchievementCategory::base_campaign}},
        {AchievementName::FinishCampaignImp, {7, "Martian All-Star", AchievementCategory::base_campaign}},
        {AchievementName::KillFish, {8, "Gone Fishin'", AchievementCategory::base_campaign}},
        {AchievementName::KillGuards, {9, "Target Practice", AchievementCategory::base_campaign}},
        {AchievementName::LockedInTram, {10, "Red Alert", AchievementCategory::base_campaign}},
        {AchievementName::MissShuttle, {11, "If only you'd been faster", AchievementCategory::base_campaign}},
        {AchievementName::Ventilation, {12, "Not a fan", AchievementCategory::base_campaign}},
        {AchievementName::DestroyGeothermal, {13, "Sabotage", AchievementCategory::base_campaign}},
        {AchievementName::DestroyTrashBot, {14, "Unsuppressed", AchievementCategory::base_campaign}},
        {AchievementName::MeetCapek, {15, "Meet Capek", AchievementCategory::base_campaign}},
        {AchievementName::CapekVoiceEnd, {16, "It Suddenly Worked", AchievementCategory::base_campaign}},
        {AchievementName::KillSnake, {17, "Who's Cleaning This Up?", AchievementCategory::base_campaign}},
        {AchievementName::GoSpaceStation, {18, "Stowaway", AchievementCategory::base_campaign}},
        {AchievementName::ComputersSpaceStation, {19, "Try a Reboot", AchievementCategory::base_campaign}},
        {AchievementName::DestroySpaceStation, {20, "...Oops", AchievementCategory::base_campaign}},
        {AchievementName::MercJail, {21, "Forget About Parker!", AchievementCategory::base_campaign}},
        {AchievementName::KillMasako, {22, "Kill Masako", AchievementCategory::base_campaign}},
        {AchievementName::UseFlashlight, {23, "Let There be Light!", AchievementCategory::singleplayer}},
        {AchievementName::UnderwaterSub, {24, "Submerged Secret", AchievementCategory::base_campaign}},
        {AchievementName::KillDrone, {25, "Heavy Equipment", AchievementCategory::base_campaign}},
        {AchievementName::KillCapekFlamethrower, {26, "Kill Capek", AchievementCategory::base_campaign}},
        {AchievementName::DropCorpse, {27, "Body Hiders", AchievementCategory::singleplayer}},
        {AchievementName::UseMedic, {28, "Healthy as a Horse", AchievementCategory::singleplayer}},
        {AchievementName::DupeC4, {29, "Double Demolition", AchievementCategory::singleplayer, AchievementType::ff_authoritative}},
        {AchievementName::ViewMonitor, {30, "Watchful Eye", AchievementCategory::singleplayer}},
        {AchievementName::EnterAesir, {31, "A Decent Descent", AchievementCategory::singleplayer}},
        {AchievementName::EnterSub, {32, "Water on Mars", AchievementCategory::singleplayer}},
        {AchievementName::EnterAPC, {33, "Tread Lightly", AchievementCategory::singleplayer}},
        {AchievementName::EnterJeep, {34, "We Don't Need Roads", AchievementCategory::singleplayer}},
        {AchievementName::EnterDriller, {35, "Totally Recalled", AchievementCategory::singleplayer}},
        {AchievementName::SecretSub, {36, "Secret Submarine", AchievementCategory::base_campaign}},
        {AchievementName::AdminFountain, {37, "Goin' for a swim, sir?", AchievementCategory::base_campaign}},
        {AchievementName::HendrixHackDoor, {38, "Hendrix Saves the Day", AchievementCategory::base_campaign}},
        {AchievementName::SubCraneButton, {39, "GeoMod Limitation", AchievementCategory::base_campaign}},
        {AchievementName::JeepWater, {40, "Amphibious Car Challenge", AchievementCategory::singleplayer}},
        {AchievementName::DestroyPumpStations, {41, "Water Under Mars", AchievementCategory::base_campaign}},
        {AchievementName::StartCampaign, {42, "Starter Rebel", AchievementCategory::base_campaign}},
        {AchievementName::FastBomb, {43, "Red Wire Redemption", AchievementCategory::base_campaign}},
        {AchievementName::FarKill, {44, "Martian Marksman", AchievementCategory::singleplayer}},
        {AchievementName::CoffeeMakers, {45, "Brewed Awakening", AchievementCategory::base_campaign, AchievementType::ff_authoritative}},
        {AchievementName::SeparateGeometry, {46, "Geological Warfare", AchievementCategory::singleplayer, AchievementType::ff_authoritative}},
        {AchievementName::GibEnemy, {47, "Messy!", AchievementCategory::singleplayer, AchievementType::ff_authoritative}},
        {AchievementName::RunOver, {48, "Crunch Time!", AchievementCategory::singleplayer, AchievementType::ff_authoritative}},
        {AchievementName::SaveBarracksMiner, {49, "Prison Break", AchievementCategory::base_campaign}},
        {AchievementName::SaveMinesMiner, {50, "Who put that there?", AchievementCategory::base_campaign}},
        {AchievementName::MedLabStealth, {51, "Trust me, I'm a doctor.", AchievementCategory::base_campaign}},
        {AchievementName::AdminStealth, {52, "Boardroom Bounty", AchievementCategory::base_campaign}},
        {AchievementName::AdminMinerBerserk, {53, "Here, hold this.", AchievementCategory::base_campaign}},
        {AchievementName::KillDavis, {54, "While I'm here...", AchievementCategory::base_campaign}},
        {AchievementName::KillCiviliansAdmin, {55, "Undercover Undertaker", AchievementCategory::base_campaign}},
        {AchievementName::SecretStash, {61, "Secret Stash", AchievementCategory::base_campaign}},
        {AchievementName::DropAPCBridge, {62, "Bridge to Nowhere", AchievementCategory::base_campaign}},
        {AchievementName::Toilets, {63, "Pipe Dreams", AchievementCategory::base_campaign, AchievementType::ff_authoritative}},
        {AchievementName::Microwaves, {64, "The Hot Take", AchievementCategory::base_campaign, AchievementType::ff_authoritative}},
        {AchievementName::DrillBit, {65, "Un-Bore-Lievable", AchievementCategory::singleplayer}},
        {AchievementName::MinerCapekPrison, {66, "Early Release Program", AchievementCategory::base_campaign}},
        {AchievementName::MissileLaunchSabotage, {67, "The Final Countdown", AchievementCategory::base_campaign}},
        {AchievementName::MedMax, {68, "Med Max", AchievementCategory::base_campaign}},
        {AchievementName::Robots100, {69, "Rust in Pieces", AchievementCategory::base_campaign, AchievementType::ff_authoritative}},
        {AchievementName::Guards500, {70, "Security Breach", AchievementCategory::base_campaign, AchievementType::ff_authoritative}},
        {AchievementName::Mercs250, {71, "Splintered Cells", AchievementCategory::base_campaign, AchievementType::ff_authoritative}},
        {AchievementName::GlassHouseShatter, {72, "Housewarming", AchievementCategory::general}},
        {AchievementName::GlassBreaks, {74, "Pane Management", AchievementCategory::base_campaign, AchievementType::ff_authoritative}},
        {AchievementName::ShatterShield, {75, "The Riot Act", AchievementCategory::singleplayer, AchievementType::ff_authoritative}},
        {AchievementName::ShootHelmets, {76, "One to the Dome", AchievementCategory::singleplayer, AchievementType::ff_authoritative}},
        {AchievementName::LockedRockets, {77, "Rocket Science", AchievementCategory::singleplayer, AchievementType::ff_authoritative}},
        {AchievementName::GeoCrater, {78, "Terraformer", AchievementCategory::singleplayer, AchievementType::ff_authoritative}},
        {AchievementName::Flame50, {79, "Flamethrower Expert", AchievementCategory::base_campaign, AchievementType::ff_authoritative}},
        {AchievementName::Pistol50, {80, "Pistol Expert", AchievementCategory::base_campaign, AchievementType::ff_authoritative}},
        {AchievementName::Turret50, {81, "Turret Expert", AchievementCategory::base_campaign, AchievementType::ff_authoritative}},
        {AchievementName::KillSubmarines, {82, "Seek and Destroy", AchievementCategory::base_campaign, AchievementType::ff_authoritative}},
        {AchievementName::Shotgun50, {83, "Shotgun Expert", AchievementCategory::base_campaign, AchievementType::ff_authoritative}},
        {AchievementName::Sniper50, {84, "Sniper Expert", AchievementCategory::base_campaign, AchievementType::ff_authoritative}},
        {AchievementName::Rail50, {85, "Rail Driver Expert", AchievementCategory::base_campaign, AchievementType::ff_authoritative}},
        {AchievementName::PR50, {86, "PR Expert", AchievementCategory::base_campaign, AchievementType::ff_authoritative}},
        {AchievementName::HMG50, {87, "HMG Expert", AchievementCategory::base_campaign, AchievementType::ff_authoritative}},
        {AchievementName::SMG50, {88, "SMG Expert", AchievementCategory::base_campaign, AchievementType::ff_authoritative}},
        {AchievementName::Baton25, {89, "Melee Expert", AchievementCategory::base_campaign, AchievementType::ff_authoritative}},
        {AchievementName::AR50, {90, "AR Expert", AchievementCategory::base_campaign, AchievementType::ff_authoritative}},
        {AchievementName::StuckEntities, {91, "Guerrilla Glue", AchievementCategory::singleplayer, AchievementType::ff_authoritative}},
    };

    for (const auto& [achievement_name, achievement] : predefined_achievements) {
        achievements[achievement_name] = achievement;
        //xlog::warn("added achievement {} with root_uid {}", achievement.name, achievement.root_uid);
    }

    achievement_system_initialized = true;
    achievement_async_ff_timer.set(achievement_async_ff_interval);
    sync_with_ff();
}

const Achievement* AchievementManager::get_achievement(AchievementName achievement_name)
{
    auto it = achievements.find(achievement_name);
    if (it != achievements.end()) {
        return &it->second; // Return a pointer to the Achievement struct
    }
    return nullptr; // Return nullptr if not found
}

void AchievementManager::process_ff_response(const std::string& response, int expected_key, bool is_initial_sync)
{
    if (response.empty()) {
        xlog::warn("No response from FF API.");
        return;
    }

    std::istringstream response_stream(response);
    std::string key_token;
    std::getline(response_stream, key_token, ',');

    try {
        int key_received = std::stoi(key_token);

        // If this is an update, validate the key
        if (!is_initial_sync && key_received != expected_key) {
            xlog::debug("Response received but key mismatch, ignoring.");
            return;
        }

        if (is_initial_sync && synced_with_ff) {
            xlog::debug("Initial sync response received but already synced, ignoring.");
            return;
        }

        // Process unlocked achievements
        std::string achievement_id_str;
        auto& manager = AchievementManager::get_instance();
        auto& achievements = manager.get_achievements_mutable();

        while (std::getline(response_stream, achievement_id_str, ',')) {
            int achievement_id = std::stoi(achievement_id_str);
            AchievementName achievement_name = static_cast<AchievementName>(achievement_id); // received facet UID to enum

            auto it = achievements.find(achievement_name);
            if (it != achievements.end() && !it->second.unlocked) {
                it->second.unlocked = true;
                xlog::info("Achievement '{}' unlocked via FF.", it->second.name);

                if (is_initial_sync) {
                    it->second.notified = true;
                }
                else if (!it->second.notified && it->second.type == AchievementType::ff_authoritative) {
                    manager.show_notification(it->second);
                }
            }
        }

        if (is_initial_sync) {
            synced_with_ff = true;
            std::string username = g_game_config.fflink_username.value();
            rf::console::printf("Successfully initialized Alpine Faction achievements for FactionFiles account %s", username.c_str());
        }
        else {
            xlog::debug("Successfully processed FF update [{}].", expected_key);
            achievement_async_ff_string.clear();
            achievement_async_ff_key = 0; // Reset key
        }
    }
    catch (...) {
        xlog::warn("Ignoring invalid response format from FF API: {}", response);
    }
}

void AchievementManager::sync_with_ff()
{
    std::string token = g_game_config.fflink_token.value();
    if (token.empty()) {
        xlog::info("Skipping achievement sync: No FactionFiles authentication token found.");
        return;
    }

    std::thread([token]() {
        xlog::info("Syncing achievements with FF...");

        std::string url = "https://link.factionfiles.com/afachievement/v1/initial/" + encode_uri_component(token);
        std::string response;

        try {
            HttpSession session("Alpine Faction v1.2.1 Achievement Sync");
            HttpRequest request(url, "GET", session);
            request.send();

            char buffer[4096];
            std::ostringstream response_stream;
            while (size_t bytes_read = request.read(buffer, sizeof(buffer))) {
                response_stream.write(buffer, bytes_read);
            }

            response = response_stream.str();
            AchievementManager::get_instance().process_ff_response(response, 0, true);
        }
        catch (const std::exception& e) {
            xlog::error("Failed to sync achievements with FF: {}", e.what());
        }
    }).detach();
}

void AchievementManager::send_update_to_ff()
{
    if (achievement_async_ff_string.empty()) {
        xlog::debug("No pending achievement updates to send.");
        return;
    }

    std::string token = g_game_config.fflink_token.value();
    if (token.empty()) {
        xlog::warn("Skipping achievement update: No FactionFiles authentication token found.");
        return;
    }

    std::thread([token]() {
        xlog::debug("Sending achievement updates to FF [{}]...", achievement_async_ff_key);

        std::string url =
            "https://link.factionfiles.com/afachievement/v1/update/" + encode_uri_component(token);
        std::string response;

        try {
            HttpSession session("Alpine Faction v1.2.1 Achievement Push");
            HttpRequest request(url, "POST", session);
            request.set_content_type("application/x-www-form-urlencoded");

            std::string post_string =
                "key=" + std::to_string(achievement_async_ff_key) + "," + achievement_async_ff_string;
            xlog::debug("sending post string {}", post_string);
            request.send(post_string);

            char buffer[4096];
            std::ostringstream response_stream;
            while (size_t bytes_read = request.read(buffer, sizeof(buffer))) {
                response_stream.write(buffer, bytes_read);
            }

            response = response_stream.str();
            AchievementManager::get_instance().process_ff_response(response, achievement_async_ff_key, false);
        }
        catch (const std::exception& e) {
            xlog::error("Failed to send achievement updates: {}", e.what());
        }
    }).detach();
}

void AchievementManager::add_key_to_ff_update_map()
{
    if (!achievement_async_ff_string.empty()) {
        xlog::debug("Skipping new update generation: Previous update still awaiting FF acknowledgment.");
        return;
    }

    std::ostringstream oss;
    bool first = true;

    // Build the new update string from accumulated `pending_count` values
    for (auto& [achievement_name, achievement] : achievements) {
        if (achievement.pending_count > 0) {
            if (!first) {
                oss << ",";
            }
            oss << achievement.root_uid << "=" << achievement.pending_count;
            first = false;
            achievement.pending_count = 0; // Reset after adding to string
        }
    }

    // Add logged kills to the update string
    auto& logged_kills = get_logged_kills_mutable();
    if (!logged_kills.empty()) {
        if (!oss.str().empty()) {
            oss << ","; // Add separator
        }

        bool first_kill = true;
        for (const auto& kill : logged_kills) {
            if (!first_kill) {
                oss << ","; // Separate each kill entry
            }
            oss << "kill="
                << kill.rfl_filename << "/"
                << kill.tc_mod_name << "/"
                << kill.entity_class_name << "/"
                << kill.damage_type << "/"
                << kill.likely_weapon_id;
            first_kill = false;
        }

        logged_kills.clear(); // Clear kills after adding to string
    }

    // Add logged uses to the update string
    auto& logged_uses = get_logged_uses_mutable();
    if (!logged_uses.empty()) {
        if (!oss.str().empty()) {
            oss << ","; // Add separator
        }

        bool first_use = true;
        for (const auto& use : logged_uses) {
            if (!first_use) {
                oss << ","; // Separate each use entry
            }
            oss << "use="
                << use.rfl_filename << "/"
                << use.tc_mod_name << "/"
                << use.used_uid << "/"
                << use.type;
            first_use = false;
        }

        logged_uses.clear(); // Clear uses after adding to string
    }

    std::string update_string = oss.str();

    if (!update_string.empty()) {
        // Generate a new unique key for this update
        std::uniform_int_distribution<int> dist(1, INT_MAX);
        achievement_async_ff_key = dist(g_rng);
        achievement_async_ff_string = update_string;

        //xlog::warn("Queued new achievement update [{}]: {}", achievement_async_ff_key, achievement_async_ff_string);
        send_update_to_ff(); // Send immediately
    }
    else {
        //xlog::warn("No new achievements pending for FF submission.");
    }
}

void AchievementManager::grant_achievement(AchievementName achievement, int count)
{
    auto it = achievements.find(achievement);
    if (it == achievements.end()) {
        return; // Achievement doesn't exist
    }

    if (rf::mod_param.found()) {
        auto mod_name = rf::mod_param.get_arg();

        switch (it->second.category) {
            case AchievementCategory::base_campaign:
                // Only allow base campaign achievements if no mod is loaded
                return;

            case AchievementCategory::kava:
                // Only allow Kava achievements if Kava mod is loaded
                if (!string_iequals(mod_name, "Kava")) {
                    return;
                }
                break;

            default:
                break; // no restriction
        }
    }


    // Show notification now for basic achievements, ff_auth achievements notify based on response from FF
    if (it->second.type == AchievementType::basic && !it->second.notified) {
        show_notification(it->second);
    }

    // Increment pending count if the achievement is either locked or ff_auth
    if (!it->second.unlocked || it->second.type == AchievementType::ff_authoritative) {
        it->second.pending_count += count;
        //xlog::warn("Incremented achievement '{}' (root_uid: {}) by {}. Pending count now {}", it->second.name, it->second.root_uid, count, it->second.pending_count);
    }
}

void AchievementManager::log_kill(int entity_uid, const std::string& rfl_filename, const std::string& tc_mod,
                                  const std::string& class_name, int damage_type, int likely_weapon)
{
    LoggedKill new_kill;
    new_kill.entity_uid = entity_uid;
    new_kill.rfl_filename = rfl_filename;
    new_kill.tc_mod_name = tc_mod;
    new_kill.entity_class_name = class_name;
    new_kill.damage_type = damage_type;
    new_kill.likely_weapon_id = likely_weapon;

    logged_kills.push_back(new_kill);

    //xlog::warn("Kill logged: entity_uid={}, map={}, mod={}, class={}, damage_type={}, likely_weapon={}", entity_uid, rfl_filename, tc_mod, class_name, damage_type, likely_weapon);
}

void AchievementManager::log_use(int used_uid, const std::string& rfl_filename, const std::string& tc_mod, int type)
{
    LoggedUse new_use;
    new_use.used_uid = used_uid;
    new_use.rfl_filename = rfl_filename;
    new_use.tc_mod_name = tc_mod;
    new_use.type = type;

    logged_uses.push_back(new_use);

    //xlog::warn("Use logged: uid={}, map={}, mod={}, type={}", used_uid, rfl_filename, tc_mod, type);
}

void AchievementManager::show_notification(Achievement& achievement)
{
    achievement.notified = true;
    //xlog::warn("Achievement notified: {}", achievement.name);

    // clear previous info before repopulating
    clear_achievement_notification();

    // drawn in draw_achievement_box
    achievement_box_icon = rf::bm::load("af_achtrophy1.tga", -1, true);
    achievement_box_name = achievement.name;
    achievement_box_timestamp.set(10000); // 10 seconds
    achievement_box_visible = true;
    play_local_sound_2d(get_custom_sound_id(0), 0, 1.0f);
}

// forcefully grant achievement
void grant_achievement(AchievementName achievement, int count) {
    if (achievement_system_initialized) {
        AchievementManager::get_instance().grant_achievement(achievement, count);
    }    
}

void grant_achievement_sp(AchievementName achievement, int count) {
    if (!rf::is_multi) {
        grant_achievement(achievement, count);
    }
}

void grant_achievement_bomb_defuse(AchievementName achievement) {
    // confirm we finished the game on the same difficulty we started
    if (rf::game_get_skill_level() == achievement_state_info.game_start_difficulty) {
        grant_achievement(achievement);
    }
}

void log_kill(int entity_uid, const std::string& class_name, int damage_type, int likely_weapon) {
    if (!achievement_system_initialized) {
        return;
    }

    // Check if the entity UID exists in logged_kills
    auto& logged_kills = AchievementManager::get_instance().get_logged_kills_mutable();
    auto it = std::find_if(logged_kills.begin(), logged_kills.end(),
        [entity_uid](const LoggedKill& kill) { return kill.entity_uid == entity_uid; });

    if (it != logged_kills.end()) {
        return; // kill is already logged - workaround for explosive damage kills being logged twice
    }

    // inline helper to sanitize strings for later sending to FF
    auto sanitize = [](std::string& str) {
        str.erase(std::remove_if(str.begin(), str.end(), [](char c) { return c == ',' || c == '/'; }), str.end());
    };

    // make strings
    std::string rfl_filename = rf::level.filename;
    std::string tc_mod_name = rf::mod_param.found() ? rf::mod_param.get_arg() : "";
    std::string entity_class_name = class_name;

    // sanitize strings
    sanitize(rfl_filename);    
    sanitize(tc_mod_name);
    sanitize(entity_class_name);

    // log the kill
    AchievementManager::get_instance().log_kill(entity_uid, rfl_filename,
        tc_mod_name, entity_class_name, damage_type, likely_weapon);
}

void log_use(int used_uid, int type) {
    if (!achievement_system_initialized) {
        return;
    }

    // Check if the used UID exists in logged_uses
    auto& logged_uses = AchievementManager::get_instance().get_logged_uses_mutable();
    auto it = std::find_if(logged_uses.begin(), logged_uses.end(),
        [used_uid](const LoggedUse& use) { return use.used_uid == used_uid; });

    if (it != logged_uses.end()) {
        return; // use is already logged, do not log again
    }

    // inline helper to sanitize strings for later sending to FF
    auto sanitize = [](std::string& str) {
        str.erase(std::remove_if(str.begin(), str.end(), [](char c) { return c == ',' || c == '/'; }), str.end());
    };

    // make strings
    std::string rfl_filename = rf::level.filename;
    std::string tc_mod_name = rf::mod_param.found() ? rf::mod_param.get_arg() : "";

    // sanitize strings
    sanitize(rfl_filename);    
    sanitize(tc_mod_name);

    // log the use
    AchievementManager::get_instance().log_use(used_uid, rfl_filename, tc_mod_name, type);
}

void initialize_achievement_manager() {
    // build achievement storage and sync with FF
    if (!rf::is_server && !rf::is_dedicated_server) {
        AchievementManager::get_instance().initialize();
    }
}

void draw_achievement_box_content(int x, int y) {
    if (achievement_box_icon != -1) {
        rf::gr::set_color(255, 255, 255, 0xCC);
        rf::gr::bitmap(achievement_box_icon, x + 4, y + 4);
    }

    int str_x = x + 278;
    int str_y = y + 8;

    rf::gr::set_color(255, 255, 255, 0xCC);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, str_x, str_y, "Achievement Unlocked!", 0);

    rf::gr::set_color(255, 255, 180, 0xCC);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, str_x, str_y + 30, achievement_box_name.c_str(), 0);
}

void draw_achievement_box() {
    int w = 480;
    int h = 72;
    int x = rf::gr::screen_width() - w - 4;

    // Timing variables
    const int total_time = 10000;   // Total time the box is displayed (match timestamp set) 
    const int animation_time = 500; // Time for rise & fall (0.5s each)
    int time_left = achievement_box_timestamp.time_until();
    int time_elapsed = total_time - time_left;

    // Base Y positions
    int y_base = rf::gr::screen_height() - h - 4;
    int y_hidden = y_base + h + 10; // fully off screen position
    int y = y_base; // fully visible position

    // Animation logic
    if (time_elapsed < animation_time) { // Rising up
        float progress = static_cast<float>(time_elapsed) / animation_time;
        y = y_hidden - static_cast<int>(progress * (y_hidden - y_base));
    }
    else if (time_left < animation_time) { // Falling down
        float progress = static_cast<float>(animation_time - time_left) / animation_time;
        y = y_base + static_cast<int>(progress * (y_hidden - y_base));
    }

    rf::gr::set_color(0, 0, 0, 0x80);
    rf::gr::rect(x, y, w, h);
    rf::gr::set_color(79, 216, 255, 0x80);
    rf::gr::rect_border(x, y, w, h);

    draw_achievement_box_content(x, y);
}

bool are_any_objects_alive(std::initializer_list<int> uids) {
    for (int uid : uids) {
        rf::Object* obj = rf::obj_lookup_from_uid(uid);
        if (obj) {
            if (obj->type == rf::OT_ENTITY) {
                auto entity = static_cast<rf::Entity*>(obj);
                if (entity->life > 0) {
                    //xlog::info("still alive: {}", obj->uid);
                    return true;
                }
            }
            else if (obj->type == rf::OT_CORPSE) {
            }
            else {
                //xlog::info("still an object: {}", obj->uid);
                return true;
            }
        }
    }
    return false; // All objects are dead
}

// called in main rf_do_frame
// runs independent of gamestate
void achievement_system_do_frame() {
    if (!achievement_system_initialized) {
        return; // not initialized
    }

    if (achievement_box_visible) {
        if (!achievement_box_timestamp.valid() || achievement_box_timestamp.elapsed()) {
            clear_achievement_notification();
        }
        else {
            draw_achievement_box();
        }
    }

    if (!achievement_async_ff_timer.valid() || achievement_async_ff_timer.elapsed()) {
        if (!synced_with_ff) {
            // Retry initial sync if it hasn't been completed yet
            xlog::warn("Initial FF achievement sync not completed. Retrying...");
            AchievementManager::get_instance().sync_with_ff();
        } 
        else {
            // Only proceed with updates if sync was successful
            //xlog::warn("Timer elapsed, trying to send an update to FF");

            if (!achievement_async_ff_string.empty()) {
                xlog::warn("Previous FF update [{}] still pending acknowledgement. Retrying.", achievement_async_ff_key);
                AchievementManager::get_instance().send_update_to_ff();
            } else {
                //xlog::warn("No pending FF update, attempting to build a new one.");
                AchievementManager::get_instance().add_key_to_ff_update_map();
            }
        }

        // Reset timer for next attempt
        achievement_async_ff_timer.set(achievement_async_ff_interval);
    }
}

void clear_achievement_notification() {
    achievement_box_visible = false;
    achievement_box_timestamp.invalidate();
    achievement_box_name.clear();
    achievement_box_icon = -1;
}

bool is_achievement_system_initialized() {
    return achievement_system_initialized;
}

// handler for achivements awarded by specific trigger activation
void achievement_check_trigger(rf::Trigger* trigger) {
    if (!trigger) {
        return;
    }

    int trigger_uid = trigger->uid;
    rf::String rfl_filename = rf::level.filename;

    // single player
    if (!rf::is_multi) {
        switch (trigger_uid) {
            // base campaign
            case 6736: {
                if (string_iequals(rfl_filename, "l9s3.rfl")) {
                    grant_achievement(AchievementName::SecretFusion); // secret fusion
                }
                break;
            }

            case 8656: {
                if (string_iequals(rfl_filename, "l1s1.rfl")) {
                    grant_achievement(AchievementName::StartCampaign); // game start
                }
                break;
            }

            case 7661: {
                if (string_iequals(rfl_filename, "train02.rfl")) {
                    grant_achievement(AchievementName::FinishTraining); // finish training
                }
                break;
            }

            case 7961: {
                if (string_iequals(rfl_filename, "train01.rfl")) {
                    grant_achievement(AchievementName::StartTraining); // start training
                }
                break;
            }

            case 4234: {
                if (string_iequals(rfl_filename, "l10s3.rfl")) {
                    grant_achievement(AchievementName::UnderwaterSub); // drop sub in underwater base
                }
                break;
            }

            case 3350: {
                if (string_iequals(rfl_filename, "l4s4.rfl")) {
                    grant_achievement(AchievementName::Ventilation); // exit ventilation
                }
                break;
            }

            case 4502: {
                if (string_iequals(rfl_filename, "l5s2.rfl")) {
                    grant_achievement(AchievementName::DestroyGeothermal); // destroy geothermal
                }
                break;
            }

            case 8571: {
                if (string_iequals(rfl_filename, "l19s1.rfl")) {
                    grant_achievement(AchievementName::MercJail); // merc prison
                }
                break;
            }

            case 7095: {
                if (string_iequals(rfl_filename, "l6s3.rfl")) {
                    grant_achievement(AchievementName::AdminFountain); // l6s3 fountain
                }
                break;
            }

            case 4029: {
                if (string_iequals(rfl_filename, "l5s3.rfl")) {
                    grant_achievement(AchievementName::SubCraneButton); // crane button
                }
                break;
            }

            case 5658: {
                if (string_iequals(rfl_filename, "l2s2a.rfl")) {
                    grant_achievement(AchievementName::SaveBarracksMiner); // barracks miner prison
                }
                break;
            }

            case 357: {
                if (string_iequals(rfl_filename, "l1s3.rfl")) {
                    grant_achievement(AchievementName::DropAPCBridge);
                }
                break;
            }

            case 10414:     // in cell
            case 9918: {    // "2" button. "ALL" button links to event 11768 (handled in event section)
                if (string_iequals(rfl_filename, "l11s2.rfl")) {
                    grant_achievement(AchievementName::MinerCapekPrison);
                }
                break;
            }

            case 9617: {
                if (string_iequals(rfl_filename, "l14s3.rfl")) {
                    grant_achievement(AchievementName::MissileLaunchSabotage);
                }
                break;
            }

            case 6937: {
                if (string_iequals(rfl_filename, "L6S3.rfl") &&
                    rf::player_is_undercover() &&
                    !rf::player_undercover_alarm_is_on()) {
                    grant_achievement(AchievementName::AdminStealth); // admin stealth finish
                }
                break;
            }

            // Kava (to be supported in the future)
            /*
            case 26156: {
                if (string_iequals(rfl_filename, "rfrev_kva00b.rfl")) {
                    grant_achievement(AchievementName::KavaSurface);
                }
                break;
            }

            case 26161: {
                if (string_iequals(rfl_filename, "rfrev_kva00b.rfl")) {
                    grant_achievement(AchievementName::Kava00bSecret1); // rover
                }
                break;
            }

            case 26365: {
                if (string_iequals(rfl_filename, "rfrev_kva00b.rfl")) {
                    grant_achievement(AchievementName::Kava00bSecret2); // toolshed
                }
                break;
            }*/

            default:
                break;
        }
    }
}

// handler for achivements awarded by specific event activation
void achievement_check_event(rf::Event* event) {
    if (!event) {
        return;
    }

    int event_uid = event->uid;
    rf::String rfl_filename = rf::level.filename;

    // single player
    if (!rf::is_multi) {
        switch (event_uid) {
            // base campaign
            case 20787: {
                if (string_iequals(rfl_filename, "l17s2.rfl")) {
                    grant_achievement(AchievementName::ComputersSpaceStation); // computers in space station
                }
                break;
            }

            case 9613: {
                if (string_iequals(rfl_filename, "l15s4.rfl")) {
                    grant_achievement(AchievementName::GoSpaceStation); // stowaway on shuttle
                }
                break;
            }

            case 18248: {
                if (string_iequals(rfl_filename, "l17s4.rfl")) {
                    grant_achievement(AchievementName::DestroySpaceStation); // destroy space station
                }
                break;
            }

            case 10626: {
                if (string_iequals(rfl_filename, "l11s3.rfl") &&
                    rf::local_player_entity->ai.current_primary_weapon != 12) { // flamethrower
                    grant_achievement(AchievementName::KillCapekFlamethrower); // kill capek with not flamethrower
                }
                break;
            }

            case 7005: {
                if (string_iequals(rfl_filename, "l10s4.rfl")) {
                    grant_achievement(AchievementName::CapekVoiceEnd); // end of capek message series
                }
                break;
            }

            case 10367: {
                if (string_iequals(rfl_filename, "l8s4.rfl")) {
                    grant_achievement(AchievementName::MeetCapek); // meet capek
                }
                break;
            }

            case 4407: {
                if (string_iequals(rfl_filename, "l5s2.rfl")) {
                    grant_achievement(AchievementName::DestroyPumpStations); // when pump stations dead geothermal
                }
                break;
            }

            case 9729: {
                if (string_iequals(rfl_filename, "l1s1.rfl")) {
                    grant_achievement(AchievementName::SaveMinesMiner); // save mines miner
                }
                break;
            }

            case 9370: {
                if (string_iequals(rfl_filename, "l8s2.rfl")) {
                    grant_achievement(AchievementName::MedLabStealth); // finished med labs doctor mission
                }
                break;
            }

            default:
                break;
        }
    }
}

// handler for achivements awarded by specific entities dying
void achievement_check_entity_death(rf::Entity* entity) {
    if (!entity) {
        return;
    }

    // single player
    if (!rf::is_multi) {
        int entity_uid = entity->uid;
        rf::String entity_script_name = entity->name;
        rf::String entity_class_name = entity->info->name;
        rf::String rfl_filename = rf::level.filename;
        bool killed_by_player = rf::local_player_entity ? entity->killer_handle == rf::local_player_entity->handle : false;

        //xlog::warn("entity died {}, {}, {}, {}, killer {}", entity_uid, entity_script_name, entity_class_name, rfl_filename, killed_by_player);

        // special handling since this entity is created by code and has no reliable UID
        if (string_iequals(rfl_filename, "l20s2.rfl") && entity_script_name == "masako_endgame") {
            grant_achievement(AchievementName::KillMasako); // kill masako
            return;
        }
    
        switch (entity_uid) {
            // base campaign
            case 7007: {
                if (string_iequals(rfl_filename, "l10s4.rfl")) {
                    grant_achievement(AchievementName::KillSnake); // kill big snake
                }
                break;
            }

            case 10696: {
                if (string_iequals(rfl_filename, "l7s4.rfl")) {
                    grant_achievement(AchievementName::DestroyTrashBot); // kill trash bot
                }
                break;
            }

            case 844: {
                if (string_iequals(rfl_filename, "l3s4.rfl")) {
                    grant_achievement(AchievementName::KillDrone); // kill drone
                }
                break;
            }

            case 3828: {
                if (string_iequals(rfl_filename, "l6s3.rfl")) {
                    grant_achievement(AchievementName::KillDavis); // kill davis
                }
                break;
            }

            default:
                break;
        }

        // base campaign
        if (string_iequals(rfl_filename, "l1s1.rfl")) {
            static const std::initializer_list<int> all_fish_uids = {9142, 9146, 9147, 9338};

            if (!are_any_objects_alive(all_fish_uids)) {
                grant_achievement(AchievementName::KillFish);
            }
        }

        if (string_iequals(rfl_filename, "l2s2a.rfl")) {
            static const std::initializer_list<int> all_guard_uids = {
                8384, 8385, 8393, 8386, 8394, 8395, 8377, 8378, 8327, 8326, 8388, 8382, 8383, 8390
            };

            if (!are_any_objects_alive(all_guard_uids)) {
                grant_achievement(AchievementName::KillGuards);
            }
        }


        if (string_iequals(rfl_filename, "l6s1.rfl")) {
            static const std::initializer_list<int> all_civilian_uids = {
                4563, 4564, 4570, 4803, 4838, 5137, 5226, 4555, 4562, 4565, 4804, 4839, 4964, 5138, 5134, 4556
            };

            if (!are_any_objects_alive(all_civilian_uids) && rf::player_is_undercover() && !rf::player_undercover_alarm_is_on()) {
                grant_achievement(AchievementName::KillCiviliansAdmin);
            }
        }

        // kava (to be supported in the future)
        /*
        if (string_iequals(rfl_filename, "rfrev_kva00b.rfl")) {
            static const std::initializer_list<int> all_aa_turrets = {
                21, 33469, 33481, 33493, 33505, 33517, 33529, 33541, 33553, 33565, 33577, 33589, 33601, 33613,
                33625, 33637, 33649, 33661, 33673, 33685, 33697, 33709
            };

            if (!are_any_objects_alive(all_aa_turrets)) {
                grant_achievement(AchievementName::KavaAATurrets);
            }
        }*/

    }
}

// handler for achivements awarded by clutter dying
void achievement_check_clutter_death(rf::Clutter* clutter) {
    if (!clutter) {
        return;
    }

    // single player
    if (!rf::is_multi) {
        int clutter_uid = clutter->uid;
        rf::String clutter_script_name = clutter->name;
        rf::String clutter_class_name = clutter->info->cls_name;
        rf::String rfl_filename = rf::level.filename;

        //xlog::warn("clutter died {}, {}, {}, {}", clutter_uid, clutter_script_name, clutter_class_name, rfl_filename);

        /* if (string_iequals(rfl_filename, "l6s1.rfl")) {
            switch (clutter_uid) {
                case 4638:
                case 4722:
                case 4735:
                case 4754:
                case 4818:
                case 4962:
                case 4615: {
                    if (!are_any_objects_alive({4638, 4722, 4735, 4754, 4818, 4962, 4615})) {
                        grant_achievement(AchievementName::NoTime);
                    }
                    break;
                }
                default:
                    break;
            }
        }*/
    }
}

// handler for achivements awarded by items being picked up
void achievement_check_item_picked_up(rf::Item* item) {
    if (!item) {
        return;
    }

    // single player
    if (!rf::is_multi) {
        int item_uid = item->uid;
        rf::String item_script_name = item->name;
        rf::String item_class_name = item->info->cls_name;
        rf::String rfl_filename = rf::level.filename;

        //xlog::warn("item picked up {}, {}, {}, {}", item_uid, item_script_name, item_class_name, rfl_filename);

        // clear MedMax achievement cache if not the right map
        if (!string_iequals(rfl_filename, "train01.rfl") && !achievement_state_info.train01_med_max_uids.empty()) {
            achievement_state_info.train01_med_max_uids.clear();
        }

        switch (item_uid) {
            // base campaign
            case 7737:
            case 7742:
            case 7743:
            case 7744:
            case 7746:
            case 7747:
            case 7748: {
                if (string_iequals(rfl_filename, "l4s3.rfl")) {
                    grant_achievement(AchievementName::SecretStash); // 7 items behind geo secret in l4s3
                }
                break;
            }

            case 7443:
            case 8546:
            case 8547:
            case 7442:
            case 7444: {
                if (string_iequals(rfl_filename, "train01.rfl")) {
                    achievement_state_info.train01_med_max_uids.push_back(item_uid);
                    if (achievement_state_info.train01_med_max_uids.size() >= 5) {
                        grant_achievement(AchievementName::MedMax);
                        achievement_state_info.train01_med_max_uids.clear();
                    }
                }
                break;
            }
        }
    }
}

bool entity_is_player_or_vehicle_or_turret(rf::Entity* ep) {
    // Check if the entity is the local player
    if (rf::entity_is_local_player(ep))
        return true;

    // Check if the entity is a vehicle
    if (rf::entity_is_vehicle(ep) || rf::entity_is_turret(ep)) {
        int first_leech = rf::entity_get_first_leech(ep);
        if (first_leech >= 0) {
            rf::Entity* entity = rf::entity_from_handle(first_leech);
            if (entity && rf::entity_is_local_player(entity))
                return true;
        }
    }

    return false;
}

void achievement_player_killed_entity(rf::Entity* entity, int lethal_damage_type, int killer_handle) {
    if (!entity) {
        return;
    }

    rf::Entity* killer_entity = rf::entity_from_handle(killer_handle);

    if (!killer_entity) {
        return;
    }

    if (!entity_is_player_or_vehicle_or_turret(killer_entity)) {
        return;
    }

    if (rf::entity_is_dying(entity)) {
        return;
    }

    // NOTE: explosives have multiple instances of lethal damage sometimes
    // NOTE: does not handle fire or crushing deaths

    // is only called in single player
    int entity_uid = entity->uid;
    rf::String entity_script_name = entity->name;
    rf::String entity_class_name = entity->info->name;
    //rf::String rfl_filename = rf::level.filename;

    int weapon = -1;        // invalid or unknown weapon
    float distance = 0.0f;  // fallback distance if player entity is invalid

    auto player_entity = rf::local_player_entity;

    if (player_entity) { // turrets dont assign player as responsible
        if (rf::entity_is_on_turret(player_entity)) {
            weapon = 133; // Turret
        }
        else if (rf::entity_in_vehicle(player_entity)) {
            auto vehicle = rf::entity_from_handle(player_entity->host_handle);

            if (vehicle->info->name == "APC") {
                weapon = 128; // APC
            }
            else if (vehicle->info->name == "Driller01") {
                weapon = 129; // Driller
            }
            else if (vehicle->info->name == "Fighter01" || vehicle->info->name == "masako_fighter") {
                weapon = 130; // Aesir
            }
            else if (vehicle->info->name == "Jeep01") {
                weapon = 131; // Jeep
            }
            else if (vehicle->info->name == "sub") {
                weapon = 132; // Submarine
            }
        }
        else if (lethal_damage_type == 4) {
            if (rf::weapon_is_flamethrower(12)) {
                weapon = 12; // fire damage can only be from flamethrower
            }
            else {
                weapon = player_entity->ai.current_primary_weapon; // use current weapon if weapon 12 isnt flamethrower
            }
        }
        else if (lethal_damage_type != 9) {
            // any damage type other than crush uses current weapon
            weapon = player_entity->ai.current_primary_weapon;
        }

        if (weapon == 1) {
            weapon = 0; // use remote charge ID instead of detonator
        }

        // calculate distance to killed entity
        if (player_entity) {
            distance = player_entity->pos.distance_to(entity->pos);
        }
    }

    //xlog::warn("player killed {} ({}) with weapon {}, damage {}, damage type {}, dist {}", entity_script_name, entity_uid, weapon, lethal_damage, lethal_damage_type, distance);

    if (distance >= 100.0f) {
        // do not award far kill achievements unless in first person mode
        if (rf::camera_get_mode(*rf::local_player->cam) == rf::CameraMode::CAMERA_FIRST_PERSON) {
            grant_achievement_sp(AchievementName::FarKill); // kill from 100m or more

        // kava (to be supported in the future)
        /*
        if (distance >= 200.0f) {
            grant_achievement_sp(AchievementName::FartherKill); // kill from 200m or more
        }*/
        }
    }

    // log kills for statistics achievements
    log_kill(entity_uid, entity_class_name, lethal_damage_type, weapon);
}

CodeInjection ai_drop_corpse_achievement_patch{
    0x00409ADB,
    []() {
        std::string rfl_filename = rf::level.filename;
        if (!string_iequals(rfl_filename, "train01.rfl") &&
            !string_iequals(rfl_filename, "train02.rfl")) {
            grant_achievement_sp(AchievementName::DropCorpse); // drop a corpse
        }
    },
};

CodeInjection riot_shield_shatter_achievement_patch{
    0x00410330,
    []() {
        grant_achievement_sp(AchievementName::ShatterShield);
    },
};

CodeInjection entity_shoot_off_helmet_achievement_patch{
    0x00429E7F,
    []() {
        grant_achievement_sp(AchievementName::ShootHelmets);
    },
};

CodeInjection player_fire_locked_on_rocket_achievement_patch{
    0x004AD002,
    []() {
        grant_achievement_sp(AchievementName::LockedRockets);
    },
};

// rocket and fusion impact explosion geos
CodeInjection geomod_weapon_hit_wall_achievement_patch{
    0x004C533D,
    [](auto& regs) {
        rf::Weapon* wp = regs.esi;
        if (wp) {
            auto pp = rf::player_from_entity_handle(wp->parent_handle);
            if (pp) {
                grant_achievement_sp(AchievementName::GeoCrater);
            }
        }
    },
};

// grenade and remote charge explosion geos
CodeInjection weapon_process_pre_geomod_achievement_patch{
    0x004C6C25,
    [](auto& regs) {
        rf::Weapon* wp = regs.esi;
        if (wp) {
            auto pp = rf::player_from_entity_handle(wp->parent_handle);
            if (pp) {
                grant_achievement_sp(AchievementName::GeoCrater);
            }
        }
    },
};

CodeInjection ai_medic_activate_achievement_patch{
    0x0040A73C,
    []() {
        std::string rfl_filename = rf::level.filename;
        if (!string_iequals(rfl_filename, "train01.rfl") &&
            !string_iequals(rfl_filename, "train02.rfl")) {
            grant_achievement_sp(AchievementName::UseMedic); // get healed by a medic
        }
    },
};

CodeInjection player_handle_use_keypress_remote_charge_achievement_patch{
    0x004A18EA,
    []() {
        grant_achievement_sp(AchievementName::DupeC4); // pick up a remote charge
    },
};

CodeInjection player_attach_to_security_camera_achievement_patch{
    0x004A1950,
    []() {
        std::string rfl_filename = rf::level.filename;
        if (!string_iequals(rfl_filename, "train01.rfl") &&
            !string_iequals(rfl_filename, "train02.rfl")) {
            grant_achievement_sp(AchievementName::ViewMonitor); // security monitor
        }
    },
};

CodeInjection separated_solids_achievement_patch{
    0x004666A0,
    []() {
        grant_achievement_sp(AchievementName::SeparateGeometry);
    },
};

CodeInjection drill_bit_maintenance_achievement_patch{
    0x00421693,
    []() {
        grant_achievement_sp(AchievementName::DrillBit);
    },
};

CodeInjection stuck_with_remote_charge_achievement_patch{
    0x004C5C33,
    [](auto& regs) {
        rf::AiInfo* aip = regs.edx;

        if (aip) {
            if (aip->ep->uid == 4707 && string_iequals(rf::level.filename, "l5s4.rfl")) {
                grant_achievement_sp(AchievementName::AdminMinerBerserk);
            }
            else {
                rf::Entity* ep = regs.ebx;
                if (ep && !rf::entity_is_slippery(ep) && !rf::entity_is_vehicle(ep)) {
                    grant_achievement_sp(AchievementName::StuckEntities);
                }
            }
        }
    },
};

CodeInjection glass_shatter_achievement_patch{
    0x00490C6B,
    [](auto& regs) {
        if (!rf::is_multi) {
            rf::String rfl_filename = rf::level.filename;

            //xlog::warn("{}, {}", achievement_state_info.glass_house_shatter_timestamp, achievement_state_info.glass_house_shatters);

            if (string_iequals(rfl_filename, "glass_house.rfl")) {
                achievement_state_info.glass_house_shatters += 1;
                if (achievement_state_info.glass_house_shatters == 64) {
                    grant_achievement_sp(AchievementName::GlassHouseShatter);
                }
            }

            rf::GFace* face = regs.esi;

            if (face) {
                //xlog::warn("shattered face {}", face->attributes.face_id);
                log_use(face->attributes.face_id, 2); // log the shatter
            }
        }
    },
};

// only hits on clutter with a valid use sound configured
CodeInjection clutter_use_achievement_patch{
    0x00410EB1,
    [](auto& regs) {
        rf::Clutter* clutter = regs.esi;
        if (clutter) {
            auto cls_name = clutter->info->cls_name;
            if (cls_name == "coffeemaker" ||
                cls_name == "Microwave" ||
                cls_name == "Toilet" ||
                cls_name == "Toilet2" ||
                cls_name == "Urinal" ||
                cls_name == "Urinal2") {
                log_use(clutter->uid, 1); // log the use
            }
        }
    },
};

// collision with entity with mass < 30
CodeInjection entity_crush_entity_achievement_patch{
    0x0041A07A,
    [](auto& regs) {
        rf::Entity* entity = regs.esi;
        rf::Entity* hit_entity = regs.edi;
        if (hit_entity && entity && rf::entity_is_local_player_or_player_attached(entity)) {
            grant_achievement_sp(AchievementName::RunOver); // 200 vehicle crushing deaths

            if (is_achievement_system_initialized()) {
                achievement_player_killed_entity(hit_entity, 9, entity->handle);
            }
        }
    },
};

// collision with high mass entity
CodeInjection entity_crush_entity_achievement_patch2{
    0x0041A0EF,
    [](auto& regs) {
        rf::Entity* entity = regs.esi;
        rf::Entity* hit_entity = regs.edi;
        if (hit_entity && entity && rf::entity_is_local_player_or_player_attached(entity)) {
            grant_achievement_sp(AchievementName::RunOver); // 200 vehicle crushing deaths

            if (is_achievement_system_initialized()) {
                achievement_player_killed_entity(hit_entity, 9, entity->handle);
            }
        }
    },
};

CodeInjection bomb_defuse_achievement_patch{
    0x0043BA23,
    [](auto& regs) {
        bool fast_bomb_achievement = false;

        switch (rf::game_get_skill_level()) {
            case rf::GameDifficultyLevel::DIFFICULTY_EASY:
                grant_achievement_bomb_defuse(AchievementName::FinishCampaignEasy); // finish campaign on easy
                fast_bomb_achievement = rf::bomb_defuse_time_left >= 42.34f;
                break;
            case rf::GameDifficultyLevel::DIFFICULTY_MEDIUM:
                grant_achievement_bomb_defuse(AchievementName::FinishCampaignMedium); // finish campaign on medium
                fast_bomb_achievement = rf::bomb_defuse_time_left >= 26.77f;
                break;
            case rf::GameDifficultyLevel::DIFFICULTY_HARD:
                grant_achievement_bomb_defuse(AchievementName::FinishCampaignHard); // finish campaign on hard
                fast_bomb_achievement = rf::bomb_defuse_time_left >= 21.03f;
                break;
            case rf::GameDifficultyLevel::DIFFICULTY_IMPOSSIBLE:
                grant_achievement_bomb_defuse(AchievementName::FinishCampaignImp); // finish campaign on impossible
                fast_bomb_achievement = rf::bomb_defuse_time_left >= 10.26f;
                break;
        }

        // do not award achievement if static bomb code is on
        if (fast_bomb_achievement && !g_alpine_game_config.static_bomb_code) {
            grant_achievement_sp(AchievementName::FastBomb); // defuse bomb within 20 seconds
        }
    },
};

CodeInjection player_handle_use_vehicle_achievement_patch{
    0x004A1DE5,
    [](auto& regs) {
        rf::Entity* entity = regs.esi;
        if (entity) {
            //xlog::warn("entered vehicle {} ({}) uid {}", entity->name, entity->info->name, entity->uid);
            if (entity->info->name == "Fighter01") {
                grant_achievement_sp(AchievementName::EnterAesir); // enter an aesir
            }
            else if (entity->info->name == "sub") {
                grant_achievement_sp(AchievementName::EnterSub); // enter a sub

                if (entity->uid == 8163 && string_iequals(rf::level.filename, "l5s3.rfl")) {
                    grant_achievement_sp(AchievementName::SecretSub); // hidden sub after geothermal
                }
            }
            else if (entity->info->name == "APC") {
                grant_achievement_sp(AchievementName::EnterAPC); // enter an APC
            }
            else if (entity->info->name == "Jeep01") {
                grant_achievement_sp(AchievementName::EnterJeep); // enter a jeep
            }
            else if (entity->info->name == "Driller01") {
                grant_achievement_sp(AchievementName::EnterDriller); // enter a driller
            }
        }
    },
};

// vehicle player is piloting enters water
CodeInjection entity_update_water_status_achievement_patch{
    0x004291B4,
    [](auto& regs) {
        rf::Entity* vehicle = regs.esi;
        if (vehicle) {
            //xlog::warn("vehicle {} ({}) uid {}", vehicle->name, vehicle->info->name, vehicle->uid);
            if (vehicle->info->name == "Jeep01") {
                grant_achievement_sp(AchievementName::JeepWater); // submerge jeep in water
            }
        }
    },
};

// after delay duration, only on events that forward messages
CodeInjection event_activate_links_achievement_patch{
    0x004B8B0A,
    [](auto& regs) {
        rf::Event* event = regs.ecx;

        if (!event) {
            return;
        }

        int event_uid = event->uid;
        rf::String rfl_filename = rf::level.filename;

        // single player
        if (!rf::is_multi) {
            switch (event_uid) {
                case 8711: {
                    if (string_iequals(rfl_filename, "l3s2.rfl")) {
                        grant_achievement(AchievementName::LockedInTram); // train in miner registration
                    }
                    break;
                }

                case 2014: {
                    if (string_iequals(rfl_filename, "l3s3.rfl")) {
                        grant_achievement(AchievementName::MissShuttle); // shuttle blow up (maybe below slow isn't such a bad thing)
                    }
                    break;
                }

                case 9458: {
                    if (string_iequals(rfl_filename, "l4s1a.rfl")) {
                        grant_achievement(AchievementName::HendrixHackDoor); // abandoned mines door
                    }
                    break;
                }

                case 11768: {
                    if (string_iequals(rfl_filename, "l11s2.rfl")) {
                        grant_achievement(AchievementName::MinerCapekPrison);
                    }
                    break;
                }

                case 3337: {
                    if (string_iequals(rfl_filename, "l4s4.rfl")) {
                        grant_achievement(AchievementName::HendrixHackDoor);
                    }
                    break;
                }

                default:
                    break;
            }
        }
    },
};

ConsoleCommand2 debug_achievement_cmd{
    "dbg_achievement",
    [](int uid) {
        if (!achievement_system_initialized) {
            rf::console::print("The achievement system isn't initialized!");
            return;
        }

        // Cast UID to AchievementName
        AchievementName achievement_name = static_cast<AchievementName>(uid);

        rf::console::print("Checking for an achievement with UID {}...", uid);

        if (!synced_with_ff) {
            xlog::warn("Achievements are not synced with FactionFiles!");
        }

        auto& manager = AchievementManager::get_instance();
        const auto& achievements = manager.get_achievements();

        auto it = achievements.find(achievement_name);
        if (it == achievements.end()) {
            rf::console::print("Achievement UID {} not found!", uid);
            return;
        }

        const Achievement& achievement = it->second;

        std::string msg = std::format(
            "Achievement UID {} found!\n"
            "Name: {}\n"
            "Unlocked? {}\n"
            "Notified? {}\n"
            "Type: {}\n"
            "Category: {}\n"
            "Pending Count: {}\n",
            //"Icon: {}\n"
            uid,
            achievement.name,
            achievement.unlocked ? "Yes" : "No",
            achievement.notified ? "Yes" : "No",
            static_cast<int>(achievement.type),
            static_cast<int>(achievement.category),
            achievement.pending_count
            //achievement.icon
        );

        rf::console::printf(msg.c_str());
    },
    "Look up information for an achievement by UID",
    "dbg_achievement <uid>",
};

ConsoleCommand2 debug_reset_achievements_cmd{
    "dbg_resetachievements",
    []() {
        if (!achievement_system_initialized) {
            rf::console::print("The achievement system isn't initialized!");
            return;
        }

        auto& manager = AchievementManager::get_instance();
        auto& achievements = manager.get_achievements_mutable();

        for (auto& [name, achievement] : achievements) {
            achievement.notified = false;
        }

        rf::console::print("Achievement notifications have been reset.");
    },
    "Reset achievement notifications",
    "dbg_resetachievements"
};

void reset_achievement_state_info() {
    if (rf::is_multi) {
        return;
    }

    AchievementStateInfo* state_info = &achievement_state_info;
    state_info->glass_house_shatters = 0;
    state_info->train01_med_max_uids.clear();

    // store starting difficulty, checked for achievements on bomb defuse
    if (string_iequals(rf::level.filename, "l1s1.rfl")) {
        state_info->game_start_difficulty = rf::game_get_skill_level();
    }
}

void achievements_apply_patch()
{
    // Achievement hooks
    ai_drop_corpse_achievement_patch.install();
    riot_shield_shatter_achievement_patch.install();
    entity_shoot_off_helmet_achievement_patch.install();
    player_fire_locked_on_rocket_achievement_patch.install();
    geomod_weapon_hit_wall_achievement_patch.install();
    weapon_process_pre_geomod_achievement_patch.install();
    ai_medic_activate_achievement_patch.install();
    player_handle_use_keypress_remote_charge_achievement_patch.install();
    player_attach_to_security_camera_achievement_patch.install();
    separated_solids_achievement_patch.install();
    drill_bit_maintenance_achievement_patch.install();
    stuck_with_remote_charge_achievement_patch.install();
    glass_shatter_achievement_patch.install();
    clutter_use_achievement_patch.install();
    entity_crush_entity_achievement_patch.install();
    entity_crush_entity_achievement_patch2.install();
    bomb_defuse_achievement_patch.install();
    player_handle_use_vehicle_achievement_patch.install();
    entity_update_water_status_achievement_patch.install();
    event_activate_links_achievement_patch.install();

    // Console commands
    debug_achievement_cmd.register_cmd();
    debug_reset_achievements_cmd.register_cmd();
}
