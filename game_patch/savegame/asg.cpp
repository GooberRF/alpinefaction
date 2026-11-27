#include <algorithm>
#include <cstring>
#include <ctime>
#include <deque>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <common/error/Win32Error.h>
#include <common/utils/os-utils.h>
#include <common/utils/string-utils.h>
#include <common/version/version.h>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <xlog/xlog.h>
#include "asg.h"
#include "asg_format.h"
#include "asg_internal.h"
#include "../misc/alpine_options.h"
#include "../misc/alpine_settings.h"
#include "../rf/entity.h"
#include "../rf/event.h"
#include "../rf/file/file.h"
#include "../rf/gameseq.h"
#include "../rf/hud.h"
#include "../rf/input.h"
#include "../rf/level.h"
#include "../rf/multi.h"
#include "../rf/os/console.h"
#include "../rf/os/os.h"
#include "../rf/os/timer.h"
#include "../rf/player/player.h"
#include "../rf/save_restore.h"
#include "../rf/trigger.h"
#include "../rf/ui.h"
#include "../ui/menu_scene.h"

namespace
{
    // ─── Game state ───

    struct PonrEntry
    {
        std::string level;
        std::vector<std::string> levels_to_keep;
    };

    // Whether new games, and games continued from an .svl, save in the Alpine format: the alpine_savegame_format
    // setting, or af_game.tbl's "Require Alpine Savegame Format". An .asg loads whatever this says.
    bool asg_enabled()
    {
        return g_alpine_game_config.alpine_savegame_format
            || get_option_or_default<bool>(AlpineOptionID::RequireAlpineSavegameFormat, false);
    }

    // The format of the running game: picked from asg_enabled() when a new game loads its first level or an .svl
    // loads, and set by an .asg load. Changing the setting mid-game takes effect at the next new game or load.
    std::optional<bool> g_session_asg;
    // The state of the levels this game has left, as stock keeps in its save data
    std::vector<asg::LevelState> g_levels;
    std::vector<asg::HudMessage> g_hud_log;
    // Stock caps these at 64 (0x004B8610)
    std::deque<rf::PersistentGoalEvent> g_persistent_goals;
    std::vector<PonrEntry> g_ponr;

    // A checked .asg whose level is loading
    struct PendingLoad
    {
        std::filesystem::path path;
        asg::SaveModel model;
    };
    std::optional<PendingLoad> g_pending_load;

    // While a restore puts objects back, so Alpine's death hooks leave the level's logic alone
    bool g_restoring = false;

    bool session_asg()
    {
        if (!g_session_asg) {
            g_session_asg = asg_enabled();
            xlog::info("[ASG] This game uses the {} savegame format", *g_session_asg ? "Alpine" : "stock");
        }
        return *g_session_asg;
    }

    // For code that must not pick the format itself
    bool in_asg_session()
    {
        return g_session_asg.value_or(false);
    }

    // A loaded .svl continues in the Alpine format when it is enabled, holding only the save's level: the earlier
    // levels are in stock's save data, which an .asg cannot take over
    void start_svl_session()
    {
        g_session_asg = asg_enabled();
        g_levels.clear();
        g_hud_log.clear();
        g_pending_load.reset();
    }

    void adopt_stock_hud_log();

    void show_message(const std::string& text)
    {
        xlog::warn("[ASG] {}", text);
        rf::console::print("{}", text);
        if (rf::gameseq_in_gameplay()) {
            rf::hud_msg(text.c_str(), 0, 0, nullptr);
        }
        else {
            rf::ui::popup_message("Savegame", text.c_str(), nullptr, false);
        }
    }

    // For a load of an .svl that starts outside the load menu: CONTINUE runs `load`. Where stock opens no popup,
    // over another one among them, the save loads unasked.
    void confirm_svl_load(rf::ui::PopupCallbackPtr load)
    {
        if (!asg_svl_load_converts()) {
            load();
            return;
        }
        if (!rf::ui::popup_is_active()) {
            const char* choices[2] = {"CONTINUE", "CANCEL"};
            rf::ui::PopupCallbackPtr callbacks[2] = {load, [] {}};
            int keys[2] = {rf::KEY_C, rf::KEY_ESC};
            rf::ui::popup_custom(asg_svl_load_title, asg_svl_load_text, 2, choices, callbacks, 0, keys);
            if (rf::ui::popup_is_active()) {
                return;
            }
        }
        show_message("This save uses the original format: it continues in the Alpine format");
        load();
    }

    template<typename V>
    auto find_level(V& levels, std::string_view filename)
    {
        auto it = std::find_if(levels.begin(), levels.end(), [&](const auto& lvl) {
            return string_iequals(lvl.filename, filename);
        });
        return it != levels.end() ? &*it : nullptr;
    }

    void upsert_level(std::vector<asg::LevelState>& levels, asg::LevelState lvl)
    {
        if (auto stored = find_level(levels, lvl.filename)) {
            *stored = std::move(lvl);
        }
        else {
            levels.push_back(std::move(lvl));
        }
    }

    // Stock stores at most 4 levels and, when full, drops one the leaving level's PONR entry does not list. Here
    // leaving a level with an entry drops every stored level it does not list, and the store has no cap.
    void apply_ponr(std::vector<asg::LevelState>& levels, std::string_view leaving_level)
    {
        const auto ponr = std::find_if(g_ponr.begin(), g_ponr.end(), [&](const PonrEntry& e) {
            return string_iequals(e.level, leaving_level);
        });
        if (ponr == g_ponr.end()) {
            return;
        }
        std::erase_if(levels, [&](const asg::LevelState& lvl) {
            return !string_iequals(lvl.filename, leaving_level)
                && std::none_of(ponr->levels_to_keep.begin(), ponr->levels_to_keep.end(),
                                [&](const std::string& keep) { return string_iequals(keep, lvl.filename); });
        });
    }

    std::vector<PonrEntry> parse_ponr(std::string_view text)
    {
        const auto value = [](std::string_view line) {
            return std::string{unquote(trim(line.substr(line.find(':') + 1)))};
        };
        std::vector<PonrEntry> out;
        for (std::string_view line : string_split(text, '\n')) {
            line = trim(line.substr(0, line.find("//")));
            if (string_istarts_with(line, "#End")) {
                break;
            }
            if (string_istarts_with(line, "$Level:")) {
                out.push_back({value(line), {}});
            }
            else if (string_istarts_with(line, "+Save:") && !out.empty()) {
                out.back().levels_to_keep.push_back(value(line));
            }
        }
        return out;
    }

    void load_ponr()
    {
        rf::File file;
        std::string text;
        if (file.open("ponr.tbl") == 0) {
            text.resize(std::max(file.size(), 0));
            text.resize(std::max(file.read(text.data(), static_cast<int>(text.size())), 0));
            file.close();
        }
        g_ponr = parse_ponr(text);
        xlog::info("[ASG] {} PONR entries", g_ponr.size());
    }

    // ─── Save and load ───

    bool has_player_record(const asg::LevelState& lvl)
    {
        const auto& entities = lvl.entities.entities;
        return std::any_of(entities.begin(), entities.end(), [](const asg::EntityRecord& r) {
            return r.base.uid == asg::player_entity_uid;
        });
    }

    bool save_asg(const std::filesystem::path& path, rf::Player& player)
    {
        if (!rf::entity_from_handle(player.entity_handle)) {
            return false;
        }
        asg::SaveModel m;
        m.header = {
            .writer_version = PRODUCT_NAME_VERSION,
            .mod_name = asg_running_mod_name(),
            .current_level = rf::level.filename.c_str(),
            .level_title = rf::level.name.c_str(),
            .difficulty = static_cast<uint8_t>(rf::game_get_skill_level()),
            .game_time = rf::level.global_time,
            .saved_at = static_cast<uint64_t>(std::time(nullptr)),
        };
        asg::LevelState current = asg::capture_level(true);
        // A save without the player cannot be loaded
        if (!has_player_record(current)) {
            show_message(std::format("Cannot save {}: the player is not in the level", path.filename().string()));
            return false;
        }
        asg::capture_game(m.game);
        asg::capture_player(player, m.player);
        apply_ponr(g_levels, m.header.current_level);
        m.levels = g_levels;
        upsert_level(m.levels, std::move(current));

        const std::optional<std::vector<uint8_t>> bytes = asg_encode(m);
        if (!bytes) {
            show_message(std::format("Cannot save {}: the save data cannot be compressed", path.filename().string()));
            return false;
        }
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        try {
            write_file_replacing(path.string(), {reinterpret_cast<const char*>(bytes->data()), bytes->size()});
        }
        catch (const Win32Error& e) {
            show_message(std::format("Cannot save {}: cannot write save file (error {})", path.filename().string(),
                                     e.error()));
            return false;
        }
        if (g_pending_load && g_pending_load->path == path) {
            g_pending_load.reset();
        }
        xlog::info("[ASG] Saved {} ({} bytes, {} levels)", path.string(), bytes->size(), m.levels.size());
        return true;
    }

    constexpr uintmax_t max_save_file_size = 128u * 1024 * 1024;

    // `show` false only logs the reason, for a caller that reports the failure itself
    std::optional<asg::SaveModel> read_save(const std::filesystem::path& path, bool show = true)
    {
        const auto fail = [&](const std::string& text) {
            if (show) {
                show_message(text);
            }
            else {
                xlog::error("[ASG] {}", text);
            }
            return std::nullopt;
        };
        std::error_code ec;
        const uintmax_t size = std::filesystem::file_size(path, ec);
        if (!ec && size > max_save_file_size) {
            return fail(std::format("Cannot load {}: the file is too large", path.filename().string()));
        }
        std::ifstream f{path, std::ios::binary};
        if (ec || !f) {
            return fail(std::format("Cannot open {}", path.filename().string()));
        }
        const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{}};
        asg::SaveModel m;
        std::string error;
        if (!asg_decode(bytes, m, error)) {
            return fail(std::format("Cannot load {}: {}", path.filename().string(), error));
        }
        const std::string mod = asg_running_mod_name();
        if (!string_iequals(m.header.mod_name, mod)) {
            return fail(std::format("{} was saved with mod \"{}\", but the running mod is \"{}\"",
                                    path.filename().string(), m.header.mod_name, mod));
        }
        const asg::LevelState* current = find_level(m.levels, m.header.current_level);
        if (!current) {
            return fail(std::format("{} holds no state for its level {}", path.filename().string(),
                                    m.header.current_level));
        }
        // restore_player needs it, and fails only once the level's objects have been restored
        if (!has_player_record(*current)) {
            return fail(std::format("{} holds no player", path.filename().string()));
        }
        return m;
    }

    bool restore_current_level(const asg::SaveModel* file, rf::Player* player)
    {
        const asg::LevelState* lvl = find_level(g_levels, rf::level.filename.c_str());
        if (!lvl) {
            xlog::info("[ASG] No stored state for {}", rf::level.filename);
            return false;
        }
        if (lvl->rfl_checksum != rf::level.checksum) {
            show_message(std::format("{} has changed since this game was saved; some objects may be out of place",
                                     rf::level.filename));
        }
        const bool transition = rf::gameseq_get_state() == rf::GS_LEVEL_TRANSITION;
        xlog::info("[ASG] Restoring {}{}", rf::level.filename, transition ? " (transition)" : "");
        rf::timer::inc_game_paused();
        g_restoring = true;
        asg::RefResolver refs;
        asg::restore_level(*lvl, transition, refs);
        bool ok = true;
        if (file && !transition) {
            const auto& entities = lvl->entities.entities;
            const auto record = std::find_if(entities.begin(), entities.end(), [](const auto& r) {
                return r.base.uid == asg::player_entity_uid;
            });
            ok = player && record != entities.end() && asg::restore_player(file->player, *record, *player, refs);
            if (ok) {
                asg::restore_game(file->game);
                rf::game_set_skill_level(
                    static_cast<rf::GameDifficultyLevel>(std::min<int>(file->header.difficulty, 3)));
            }
            else {
                xlog::error("[ASG] Cannot restore the player");
            }
        }
        g_restoring = false;
        if (ok) {
            rf::trigger_disable_unlinked();
        }
        rf::timer::dec_game_paused();
        return ok;
    }

    // ─── Hooks ───

    FunHook<bool(const char*, rf::Player*)> sr_save_game_hook{
        0x004B3B30,
        [](const char* filename, rf::Player* player) {
            if (!session_asg()) {
                return sr_save_game_hook.call_target(filename, player);
            }
            std::filesystem::path path{filename};
            path.replace_extension(".asg");
            if (!player || !save_asg(path, *player)) {
                return false;
            }
            asg_set_last_save(path);
            return true;
        },
    };

    // The quick save is whichever of quicksav.asg and quicksav.svl is newer; quicksav.svl is also the fallback
    // for a quicksav.asg that cannot be loaded
    FunHook<void()> do_quick_load_hook{
        0x004B5EB0,
        []() {
            const std::filesystem::path dir{rf::sr::savegame_path};
            const auto asg_path = dir / "quicksav.asg";
            const auto svl_path = dir / "quicksav.svl";
            std::error_code ec;
            if (std::filesystem::exists(asg_path, ec)) {
                const auto asg_time = std::filesystem::last_write_time(asg_path, ec);
                const bool has_svl = std::filesystem::exists(svl_path, ec);
                const bool svl_newer = has_svl && std::filesystem::last_write_time(svl_path, ec) > asg_time;
                if (!svl_newer && (asg_load_game(asg_path) || !has_svl)) {
                    return;
                }
            }
            // Stock loads quicksav.svl only when its header reads
            if (rf::sr::read_header(svl_path.string().c_str(), nullptr, nullptr, nullptr)) {
                confirm_svl_load([] { do_quick_load_hook.call_target(); });
            }
        },
    };

    // The death popup's button that loads the last save, after the popup has closed
    FunHook<void()> death_load_last_save_hook{
        0x00433120,
        []() {
            if (!string_iends_with(rf::sr::last_save_filename, ".svl")) {
                death_load_last_save_hook.call_target();
                return;
            }
            // CANCEL leaves the player dead, and the death screen shows its popup again
            confirm_svl_load([] { death_load_last_save_hook.call_target(); });
        },
    };

    FunHook<bool()> sr_transitional_save_hook{
        0x004B52E0,
        []() {
            if (!session_asg()) {
                return sr_transitional_save_hook.call_target();
            }
            asg::LevelState lvl = asg::capture_level(false);
            apply_ponr(g_levels, lvl.filename);
            upsert_level(g_levels, std::move(lvl));
            return true;
        },
    };

    // A new game; also run by stock when a load fails
    FunHook<void()> sr_reset_save_data_hook{
        0x004B52C0,
        []() {
            sr_reset_save_data_hook.call_target();
            g_session_asg.reset();
            g_levels.clear();
            g_hud_log.clear();
            g_pending_load.reset();
        },
    };

    // Called by the level loader (0x00461187) with the save file, or "auto.svl" for the stored state of the level
    FunHook<bool(const char*, rf::Player*)> sr_load_level_state_hook{
        0x004B47A0,
        [](const char* filename, rf::Player* player) {
            const std::filesystem::path path{filename ? filename : ""};
            const bool from_file = string_iequals(path.extension().string(), ".asg");
            std::optional<PendingLoad> pending = std::exchange(g_pending_load, std::nullopt);
            if (!from_file) {
                if (!string_iequals(path.string(), "auto.svl")) {
                    // asg_level_load_begin has picked the session's format, so in the Alpine one the persistent
                    // goals the stock load adds go to its list
                    const bool result = sr_load_level_state_hook.call_target(filename, player);
                    if (result && in_asg_session()) {
                        adopt_stock_hud_log();
                    }
                    return result;
                }
                if (!session_asg()) {
                    return sr_load_level_state_hook.call_target(filename, player);
                }
            }
            else if (!pending || pending->path != path) {
                // The level loader reports a failure itself
                auto model = read_save(path, false);
                if (!model) {
                    return false;
                }
                pending = PendingLoad{path, std::move(*model)};
            }
            if (from_file) {
                g_session_asg = true;
                g_levels = std::move(pending->model.levels);
            }
            return restore_current_level(from_file ? &pending->model : nullptr, player);
        },
    };

    // The stock load menu runs it before it loads the level of a save
    FunHook<bool(const char*, char*, int*, char*)> sr_read_header_hook{
        0x004B3850,
        [](const char* filename, char* level, int* game_time, char* title) {
            if (!filename || !string_iends_with(filename, ".asg")) {
                return sr_read_header_hook.call_target(filename, level, game_time, title);
            }
            asg::Header header;
            if (!asg_read_header(filename, header)) {
                return false;
            }
            if (level) {
                level[header.current_level.copy(level, 31)] = '\0';
            }
            if (game_time) {
                std::memcpy(game_time, &header.game_time, sizeof(*game_time));
            }
            if (title) {
                title[header.level_title.copy(title, 47)] = '\0';
            }
            return true;
        },
    };

    // Stock logs into an 80-entry ring; the log is mirrored into g_hud_log, which has no cap
    FunHook<void(const char*, int16_t)> hud_save_persona_msg_hook{
        0x00437FB0,
        [](const char* msg, int16_t persona) {
            const int newest = rf::sr::logged_message_newest_index();
            hud_save_persona_msg_hook.call_target(msg, persona);
            const int added = rf::sr::logged_message_newest_index();
            if (added == newest || !in_asg_session()) {
                return;
            }
            const rf::sr::LoggedHudMessage* m = rf::sr::logged_message_get(added);
            g_hud_log.push_back({m->message, m->time_string, m->persona_index, m->display_height});
        },
    };

    // The message log panel's reads. It walks back from the newest index, so a list whose newest entry is last
    // and whose count is its size never wraps.
    CallHook<int()> message_log_count_hook{
        {0x00454F34, 0x00454F5E, 0x00454FE4, 0x00454FF1, 0x00455097, 0x00455277, 0x004552F0, 0x00455430},
        []() {
            return in_asg_session() ? static_cast<int>(g_hud_log.size()) : message_log_count_hook.call_target();
        },
    };

    CallHook<int()> message_log_newest_index_hook{
        {0x00454F25, 0x004550A4, 0x004552DF},
        []() {
            return in_asg_session() ? static_cast<int>(g_hud_log.size()) - 1
                                    : message_log_newest_index_hook.call_target();
        },
    };

    // The log an .svl load has read into stock's ring, oldest first
    void adopt_stock_hud_log()
    {
        constexpr int ring_size = 80;
        const int newest = rf::sr::logged_message_newest_index();
        const int count = std::min(message_log_count_hook.call_target(), ring_size);
        g_hud_log.clear();
        for (int i = count - 1; i >= 0; --i) {
            const rf::sr::LoggedHudMessage* m = rf::sr::logged_message_get((newest - i + ring_size) % ring_size);
            g_hud_log.push_back({m->message, m->time_string, m->persona_index, m->display_height});
        }
    }

    CallHook<rf::sr::LoggedHudMessage*(int)> message_log_get_hook{
        {0x00454F47, 0x004550B7, 0x0045530B},
        [](int index) {
            if (!in_asg_session()) {
                return message_log_get_hook.call_target(index);
            }
            static rf::sr::LoggedHudMessage out;
            out = {};
            if (index >= 0 && index < static_cast<int>(g_hud_log.size())) {
                const auto& m = g_hud_log[index];
                m.text.copy(out.message, sizeof(out.message) - 1);
                out.time_string = m.time_string;
                out.persona_index = m.persona;
                out.display_height = m.display_height;
            }
            return &out;
        },
    };

    FunHook<rf::PersistentGoalEvent*(const char*)> event_lookup_persistent_goal_event_hook{
        0x004B8680,
        [](const char* name) -> rf::PersistentGoalEvent* {
            if (!in_asg_session()) {
                return event_lookup_persistent_goal_event_hook.call_target(name);
            }
            for (auto& goal : g_persistent_goals) {
                if (string_iequals(goal.name.c_str(), name)) {
                    return &goal;
                }
            }
            return nullptr;
        },
    };

    FunHook<void(const char*, int, int)> event_add_persistent_goal_event_hook{
        0x004B8610,
        [](const char* name, int initial_count, int count) {
            if (!in_asg_session()) {
                event_add_persistent_goal_event_hook.call_target(name, initial_count, count);
                return;
            }
            // As stock, an existing goal keeps its counts
            for (const auto& goal : g_persistent_goals) {
                if (string_iequals(goal.name.c_str(), name)) {
                    return;
                }
            }
            auto& goal = g_persistent_goals.emplace_back();
            goal.name = name;
            goal.initial_count = initial_count;
            goal.count = count;
        },
    };

    // Run by level init, except on a level transition
    FunHook<void()> event_clear_persistent_goal_events_hook{
        0x004BDA10,
        []() {
            event_clear_persistent_goal_events_hook.call_target();
            g_persistent_goals.clear();
        },
    };

    FunHook<bool()> sr_parse_ponr_table_hook{
        0x004B36F0,
        []() {
            const bool result = sr_parse_ponr_table_hook.call_target();
            load_ponr();
            return result;
        },
    };
}

std::vector<asg::HudMessage>& asg::hud_message_log()
{
    return g_hud_log;
}

std::deque<rf::PersistentGoalEvent>& asg::persistent_goals()
{
    return g_persistent_goals;
}

std::string asg_running_mod_name()
{
    return rf::mod_param.found() ? rf::mod_param.get_arg() : "";
}

bool asg_game_saves_asg()
{
    return session_asg();
}

bool asg_svl_load_converts()
{
    return asg_enabled();
}

bool asg_load_game(const std::filesystem::path& path)
{
    auto model = read_save(path);
    if (!model) {
        return false;
    }
    const std::string level = model->header.current_level;
    xlog::info("[ASG] Loading {} (level {})", path.string(), level);
    g_pending_load = PendingLoad{path, std::move(*model)};
    rf::level_set_level_to_load(rf::String{level.c_str()}, rf::String{path.string().c_str()});
    rf::gameseq_set_state(rf::GS_NEW_LEVEL, false);
    return true;
}

void asg_set_last_save(const std::filesystem::path& path)
{
    const std::string name = path.string();
    auto& out = rf::sr::last_save_filename;
    // A truncated path would name another file
    out[name.size() < sizeof(out) ? name.copy(out, name.size()) : 0] = '\0';
}

bool asg_restoring()
{
    return g_restoring;
}

void asg_level_load_begin(const char* state_filename)
{
    asg::snapshot_class_debris();
    asg::clear_deferred_alpine_restore();
    const std::string_view name{state_filename ? state_filename : ""};
    if (string_iends_with(name, ".asg")) {
        g_session_asg = true;
    }
    else if (!name.empty()) {
        start_svl_session();
        xlog::info("[ASG] {} is a stock savegame: this game continues in the {} format", name,
                   *g_session_asg ? "Alpine" : "stock");
    }
    // A new game's first level; the menu scene and multiplayer levels are not a game that saves
    else if (!rf::is_multi && !menu_scene_active()) {
        session_asg();
    }
}

void asg_level_init_post()
{
    // RF2 carves need the geoable flags set in destruction_level_init_post, which runs just before this
    g_restoring = true;
    asg::replay_rf2_geomods();
    g_restoring = false;
    asg::run_deferred_alpine_restore();
}

void asg_apply_patches()
{
    sr_save_game_hook.install();
    do_quick_load_hook.install();
    death_load_last_save_hook.install();
    sr_transitional_save_hook.install();
    sr_reset_save_data_hook.install();
    sr_load_level_state_hook.install();
    sr_read_header_hook.install();
    sr_parse_ponr_table_hook.install();

    // Uncapped HUD message log
    hud_save_persona_msg_hook.install();
    message_log_count_hook.install();
    message_log_newest_index_hook.install();
    message_log_get_hook.install();

    // Uncapped persistent goals
    event_lookup_persistent_goal_event_hook.install();
    event_add_persistent_goal_event_hook.install();
    event_clear_persistent_goal_events_hook.install();

}
