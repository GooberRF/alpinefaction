#include <ctime>
#include <windows.h>
#include <shellapi.h>
#include <common/config/GameConfig.h>
#include <common/config/BuildConfig.h>
#include <common/utils/os-utils.h>
#include <crash_handler_stub.h>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <common/version/version.h>
#include <xlog/ConsoleAppender.h>
#include <xlog/FileAppender.h>
#include <xlog/Win32Appender.h>
#include <xlog/xlog.h>
#include "main.h"
#include "../os/console.h"
#include "../os/os.h"
#include "../bmpman/bmpman.h"
#include "../debug/debug.h"
#include "../graphics/gr.h"
#include "../graphics/legacy/gr_d3d.h"
#include "../hud/hud.h"
#include "../hud/hud_world.h"
#include "../hud/multi_scoreboard.h"
#include "../hud/multi_spectate.h"
#include "../object/object.h"
#include "../multi/multi.h"
#include "../multi/gametype.h"
#include "../multi/server.h"
#include "../multi/server_internal.h"
#include "../multi/alpine_packets.h"
#include "../misc/misc.h"
#include "../misc/achievements.h"
#include "../misc/alpine_options.h"
#include "../misc/alpine_settings.h"
#include "../misc/vpackfile.h"
#include "../misc/high_fps.h"
#include "../misc/player.h"
#include "../input/input.h"
#include "../rf/gr/gr.h"
#include "../rf/multi.h"
#include "../rf/level.h"
#include "../rf/os/os.h"
#include "../rf/save_restore.h"
#include "../rf/gameseq.h"

#ifdef HAS_EXPERIMENTAL
#include "../experimental/experimental.h"
#endif

GameConfig g_game_config;
AlpineCoreConfig g_alpine_system_config;
HMODULE g_hmodule;

std::mt19937 g_rng;

void initialize_random_generator() {
    // seed rng with the current time
    auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
    g_rng.seed(static_cast<unsigned long>(seed));
}

CallHook<void()> rf_init_hook{
    0x004B27CD,
    []() {
        auto start_ticks = GetTickCount();
        xlog::info("Initializing game...");
        initialize_alpine_core_config();
        rf_init_hook.call_target();
        vpackfile_disable_overriding();
        xlog::info("Game initialized ({} ms).", GetTickCount() - start_ticks);
    },
};

void execute_startup_scripts() {
    for (int i = 0; i < rf::cmdline_num_args; ++i) {
        const rf::CmdArg& cmd_arg = rf::cmdline_args[i];
        if (!cmd_arg.arg || cmd_arg.is_done) {
            continue;
        }
        if (!std::strcmp(cmd_arg.arg, "-script") && i + 1 < rf::cmdline_num_args) {
            const rf::CmdArg& script_arg = rf::cmdline_args[i + 1];
            if (script_arg.arg) {
                console_run_script(script_arg.arg);
            }
            ++i;
        }
    }
}

CodeInjection after_full_game_init_hook{
    0x004B26C6,
    []() {
        multi_spectate_after_full_game_init();
#if !defined(NDEBUG) && defined(HAS_EXPERIMENTAL)
        experimental_init_after_game();
#endif
        console_init();
        multi_after_full_game_init();
        debug_init();
        load_world_hud_assets();
        execute_startup_scripts();

        xlog::info("Game fully initialized");
        xlog::LoggerConfig::get().flush_appenders();
    },
};

CodeInjection cleanup_game_hook{
    0x004B2821,
    []() {
        debug_cleanup();
    },
};

static void maybe_autosave()
{
    static int pending_autosave = 0;
    if (rf::gameseq_get_state() == rf::GS_LEVEL_TRANSITION && g_alpine_game_config.autosave) {
        pending_autosave = 5; // wait 5 frames for the game state to fully stabilize
    }
    if (pending_autosave > 0) {
        pending_autosave--;
        if (pending_autosave == 0 && rf::sr::can_save_now() && rf::gameseq_get_state() != rf::GS_BOMB_DEFUSE) {
            xlog::info("Performing autosave");
            auto save_filename = std::string{rf::sr::savegame_path} + "autosave.svl";
            if (!rf::sr::save_game(save_filename.c_str(), rf::local_player)) {
                xlog::error("Autosave failed");
            }
        }
    }
}

FunHook<int()> rf_do_frame_hook{
    0x004B2D90,
    []() {
        debug_do_frame_pre();
        rf::os_poll();
        high_fps_update();
        server_do_frame();
        koth_do_frame();
        int result = rf_do_frame_hook.call_target();
        maybe_autosave();
        debug_do_frame_post();
        multi_level_download_update();
        return result;
    },
};

CodeInjection after_level_render_hook{
    0x00432375,
    []() {
#if !defined(NDEBUG) && defined(HAS_EXPERIMENTAL)
        experimental_render_in_game();
#endif
        debug_render();
        hud_world_do_frame();
    },
};

CodeInjection after_frame_render_hook{
    0x004B2DC2,
    []() {
        if (!rf::is_dedicated_server) {
            // Draw on top (after scene)
            frametime_render_ui();
            achievement_system_do_frame();
#if !defined(NDEBUG) && defined(HAS_EXPERIMENTAL)
            experimental_render();
#endif
            debug_render_ui();
            g_solid_render_ui();
        }
    },
};

FunHook<int(rf::String&, rf::String&, char*)> level_load_hook{
    0x0045C540,
    [](rf::String& level_filename, rf::String& save_filename, char* error) {
        xlog::info("Loading level: {}", level_filename);
        evaluate_pow2tex(level_filename);
        if (!save_filename.empty())
            xlog::info("Restoring game from save file: {}", save_filename);

        // attempt to load level_info tbl file
        load_level_info_config(level_filename);

        int ret = level_load_hook.call_target(level_filename, save_filename, error);
        if (ret != 0)
            xlog::warn("Loading failed: {}", error);
        else {
            multi_spectate_level_init();
        }
        return ret;
    },
};

FunHook<void(bool)> level_init_post_hook{
    0x00435DF0,
    [](bool transition) {
        level_init_post_hook.call_target(transition);
        xlog::info("Level loaded: {}{}", rf::level.filename, transition ? " (transition)" : "");

        apply_maximum_fps(); // set maximum FPS based on game state
        process_queued_spawn_points_from_items();
        populate_world_hud_sprite_events();
        reset_achievement_state_info();
        multi_level_init_post_gametypes();

        if (!rf::is_dedicated_server) {
            explosion_flash_lights_level_init();
            evaluate_fullbright_meshes();
            set_levelmod_autotexture_ppm();
            if (!rf::is_multi) {
                update_player_flashlight();
            }
            else {
                toggle_chat_menu(ChatMenuType::None);
                player_multi_level_post_init();
                multi_hud_level_init();
                // listen server host spawns before this init, so we need to run on_local_spawn for them again
                if (rf::is_server) {
                    multi_hud_on_local_spawn();
                }
            }

            if (g_alpine_game_config.try_disable_textures) {
                evaluate_lightmaps_only();
            }
            if (g_alpine_game_config.try_disable_weapon_shake) {
                evaluate_restrict_disable_ss();
            }
            if (g_alpine_game_config.try_disable_muzzle_flash_lights) {
                evaluate_restrict_disable_muzzle_flash();
            }
        }

        if (rf::is_dedicated_server || rf::is_server) {
            if (g_match_info.match_active) {
                af_broadcast_automated_chat_msg(
                    "=========== MATCH LIVE ==========="
                );
            }
            else if (g_match_info.pre_match_queued) {
                start_pre_match();
            }
        }
    },
};

class RfConsoleLogAppender : public xlog::Appender
{
    std::vector<std::pair<std::string, xlog::Level>> m_startup_buf;

public:
    RfConsoleLogAppender()
    {
#ifdef NDEBUG
        set_level(xlog::Level::warn);
#endif
    }

protected:
    void append([[maybe_unused]] xlog::Level level, const std::string& str) override
    {
        static auto& console_inited = addr_as_ref<bool>(0x01775680);
        if (console_inited) {
            flush_startup_buf();

            rf::Color color = color_from_level(level);
            rf::console::output(str.c_str(), &color);
        }
        else {
            m_startup_buf.emplace_back(str, level);
        }
    }

    void flush() override
    {
        static auto& console_inited = addr_as_ref<bool>(0x01775680);
        if (console_inited) {
            flush_startup_buf();
        }
    }

private:
    [[nodiscard]] static rf::Color color_from_level(xlog::Level level)
    {
        switch (level) {
            case xlog::Level::error:
                return rf::Color{255, 0, 0};
            case xlog::Level::warn:
                return rf::Color{255, 255, 0};
            case xlog::Level::info:
                return rf::Color{195, 195, 195};
            default:
                return rf::Color{127, 127, 127};
        }
    }

    void flush_startup_buf()
    {
        for (auto& p : m_startup_buf) {
            rf::Color color = color_from_level(p.second);
            rf::console::output(p.first.c_str(), &color);
        }
        m_startup_buf.clear();
    }
};

static std::string& get_log_file_path_name()
{
    static std::string log_file_path_name;
    if (log_file_path_name.empty()) {
        std::string dedicated_server_name;
        int argc;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; i < argc; ++i) {
            if (!std::wcscmp(argv[i], L"-dedicated") && i + 1 < argc) {
                LPWSTR next_arg = argv[i + 1];
                dedicated_server_name.resize(std::wcslen(next_arg));
                std::wcstombs(dedicated_server_name.data(), next_arg, dedicated_server_name.size());
            }
            else if (!std::wcscmp(argv[i], L"-ads") && i + 1 < argc) {
                LPWSTR next_arg = argv[i + 1];
                dedicated_server_name.resize(std::wcslen(next_arg));
                std::wcstombs(dedicated_server_name.data(), next_arg, dedicated_server_name.size());
                size_t slash_pos = dedicated_server_name.find_last_of("/\\");
                if (slash_pos != std::string::npos)
                    dedicated_server_name.erase(0, slash_pos + 1);
                size_t dot_pos = dedicated_server_name.find_last_of('.');
                if (dot_pos != std::string::npos)
                    dedicated_server_name.erase(dot_pos);
            }
        }
        LocalFree(argv);

        if (!dedicated_server_name.empty()) {
            log_file_path_name = "logs\\AlpineFaction-dedicated-";
            log_file_path_name += dedicated_server_name;
            log_file_path_name += ".log";
        }
        else {
            log_file_path_name = "logs\\AlpineFaction.log";
        }
    }
    return log_file_path_name;
}

void init_logging()
{
    auto& log_file_path_name = get_log_file_path_name();

    CreateDirectoryA("logs", nullptr);
    xlog::LoggerConfig::get()
        .add_appender<xlog::FileAppender>(log_file_path_name, false, true)
        // .add_appender<xlog::ConsoleAppender>()
        // .add_appender<xlog::Win32Appender>()
        .add_appender<RfConsoleLogAppender>();
    xlog::info("Alpine Faction {} ({}), build date: {} {}", VERSION_STR, VERSION_CODE, __DATE__, __TIME__);

    auto now = std::time(nullptr);
    auto* tm = std::gmtime(&now);
    char time_str[256];
    std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);
    xlog::info("Current UTC time: {}", time_str);

    xlog::info("Command line: {}", GetCommandLineA());
}

void log_system_info()
{
    try {
        xlog::info("Real system version: {}", get_real_os_version());
        xlog::info("Emulated system version: {}", get_os_version());
        auto wine_ver = get_wine_version();
        if (wine_ver)
            xlog::info("Running on Wine: {}", wine_ver.value());

        xlog::info("Running as {} (elevation type: {})", is_current_user_admin() ? "admin" : "user", get_process_elevation_type());
        xlog::info("CPU Brand: {}", get_cpu_brand());
        xlog::info("CPU ID: {}", get_cpu_id());
        LARGE_INTEGER qpc_freq;
        QueryPerformanceFrequency(&qpc_freq);
        xlog::info("QPC Frequency: {:08X} {:08X}", static_cast<DWORD>(qpc_freq.HighPart), qpc_freq.LowPart);
    }
    catch (std::exception& e) {
        xlog::error("Failed to read system info: {}", e.what());
    }
}

void load_config()
{
    // Load config
    try {
        if (!g_game_config.load())
            xlog::warn("Configuration has not been found in registry!");
    }
    catch (std::exception& e) {
        xlog::error("Failed to load configuration: {}", e.what());
    }

    // Log information from config
    xlog::info("Resolution: {}x{}x{}", g_game_config.res_width.value(), g_game_config.res_height.value(), g_game_config.res_bpp.value());
    xlog::info("Window Mode: {}", static_cast<int>(g_game_config.wnd_mode.value()));
    //xlog::info("Max FPS: {}", g_game_config.max_fps.value());
    xlog::info("Allow Overwriting Game Files: {}", g_game_config.allow_overwrite_game_files.value());
}

void init_crash_handler()
{
    char current_dir[MAX_PATH] = ".";
    GetCurrentDirectoryA(std::size(current_dir), current_dir);
    auto& log_file_path_name = get_log_file_path_name();

    CrashHandlerConfig config;
    config.this_module_handle = g_hmodule;
    std::snprintf(config.log_file, std::size(config.log_file), "%s\\%s", current_dir, log_file_path_name.c_str());
    std::snprintf(config.output_dir, std::size(config.output_dir), "%s\\logs", current_dir);
    std::snprintf(config.app_name, std::size(config.app_name), "AlpineFaction");
    config.add_known_module("RF");
    config.add_known_module("AlpineFaction");

    CrashHandlerStubInstall(config);
}

extern "C" void subhook_unk_opcode_handler(uint8_t* opcode)
{
    xlog::error("SubHook unknown opcode 0x{:x} at {}", *opcode, static_cast<void*>(opcode));
}

extern "C" DWORD __declspec(dllexport) Init([[maybe_unused]] void* unused)
{
    DWORD start_ticks = GetTickCount();

    // Init logging and crash dump support first
    init_logging();
    init_crash_handler();

    // Init random number generator
    initialize_random_generator();

    // Enable Data Execution Prevention
    if (!SetProcessDEPPolicy(PROCESS_DEP_ENABLE))
        xlog::warn("SetProcessDEPPolicy failed (error {})", GetLastError());

    log_system_info();
    load_config();

    // General game hooks
    rf_init_hook.install();
    after_full_game_init_hook.install();
    cleanup_game_hook.install();
    rf_do_frame_hook.install();
    after_level_render_hook.install();
    after_frame_render_hook.install();
    level_load_hook.install();
    level_init_post_hook.install();

    // Init modules
    console_apply_patches();
    gr_apply_patch();
    bm_apply_patch();
    os_apply_patch();
    hud_apply_patches();
    multi_do_patch();
    multi_scoreboard_apply_patch();
    gametype_do_patch();
    vpackfile_apply_patches();
    multi_spectate_appy_patch();
    high_fps_init();
    object_do_patch();
    misc_init();
    server_init();
    dedi_cfg_init();
    mouse_apply_patch();
    key_apply_patch();
#if !defined(NDEBUG) && defined(HAS_EXPERIMENTAL)
    experimental_init();
#endif
    debug_apply_patches();

    xlog::info("Installing hooks took {} ms", GetTickCount() - start_ticks);

    return 1; // success
}

BOOL WINAPI DllMain(HINSTANCE instance_handle, [[maybe_unused]] DWORD fdw_reason, [[maybe_unused]] LPVOID lpv_reserved)
{
    g_hmodule = instance_handle;
    DisableThreadLibraryCalls(instance_handle);
    return TRUE;
}
