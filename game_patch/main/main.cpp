#include <ctime>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <typeinfo>
#ifdef __GNUC__
#include <cxxabi.h> // abi::__cxa_demangle for the __cxa_throw hook (MinGW/GCC only)
#endif
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
#include "../bmpman/atx.h"
#include "../bmpman/bmpman.h"
#include "../debug/debug.h"
#include "../graphics/gr.h"
#include "../graphics/d3d11/gr_d3d11_mesh.h"
#include "../hud/hud.h"
#include "../hud/hud_world.h"
#include "../hud/multi_scoreboard.h"
#include "../hud/multi_spectate.h"
#include "../object/object.h"
#include "../multi/multi.h"
#include "../multi/gametype.h"
#include "../multi/bagman.h"
#include "../multi/server.h"
#include "../multi/server_internal.h"
#include "../multi/alpine_packets.h"
#include "../fflink/fflink.h"
#include "../misc/misc.h"
#include "../misc/achievements.h"
#include "../misc/spray_picker.h"
#include "../misc/alpine_options.h"
#include "../misc/alpine_settings.h"
#include "../misc/vpackfile.h"
#include "../misc/high_fps.h"
#include "../misc/player.h"
#include "../misc/waypoints.h"
#include "../misc/level.h"
#include "../object/alpine_corona.h"
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

#include "../multi/bots/bot_main.h"

GameConfig g_game_config;
AlpineCoreConfig g_alpine_system_config;
HMODULE g_hmodule;
std::time_t g_process_startup_time;

std::mt19937 g_rng;

void initialize_random_generator() {
    // seed rng with the current time
    auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
    g_rng.seed(static_cast<unsigned long>(seed));
}

// ============================================================================
// TEMPORARY crash diagnostics (remove once the MinGW dedicated-server launch
// crash is pinned). A first-chance vectored exception handler logs the ORIGINAL
// faulting instruction + a stack walk at xlog::warn BEFORE the process's SEH
// unwinder runs. In MinGW builds that unwinder corrupts memory and hides the
// real fault behind a secondary "write to RF .text" crash, so catching it
// first-chance is the only way to see the true site. Everything is rendered as
// `module+offset` so AlpineFaction.dll frames can be resolved in Ghidra.
// ============================================================================
namespace {

uintptr_t g_af_diag_base = 0;
uintptr_t g_af_diag_end = 0;
volatile long g_af_diag_seq = 0;
volatile long g_af_diag_in_handler = 0;

std::string af_diag_addr_to_str(uintptr_t addr)
{
    HMODULE mod = nullptr;
    if (addr && GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(addr), &mod) && mod) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(mod, path, sizeof(path));
        const char* base = std::strrchr(path, '\\');
        base = base ? base + 1 : path;
        const uintptr_t off = addr - reinterpret_cast<uintptr_t>(mod);
        return std::format("{}+0x{:X}", base, off);
    }
    return std::format("0x{:08X}", addr);
}

// Scan the stack upward from `sp` for return addresses into AlpineFaction.dll and
// log them as `AlpineFaction.dll+offset`. RF's boot loop omits the frame pointer,
// so a plain EBP walk can't reach AF code; this recovers it regardless.
void af_diag_scan_af_frames(long seq, uintptr_t sp)
{
    if (!g_af_diag_base || !g_af_diag_end) {
        return;
    }
    int found = 0;
    for (uintptr_t p = sp; p < sp + 0x3000 && found < 48; p += sizeof(uintptr_t)) {
        if (IsBadReadPtr(reinterpret_cast<void*>(p), sizeof(uintptr_t))) {
            break;
        }
        const uintptr_t v = *reinterpret_cast<uintptr_t*>(p);
        if (v >= g_af_diag_base && v < g_af_diag_end) {
            xlog::warn("[AF-DIAG #{}]   stack+0x{:04X} -> AlpineFaction.dll+0x{:X}",
                       seq, static_cast<uint32_t>(p - sp), v - g_af_diag_base);
            ++found;
        }
    }
}

LONG WINAPI af_diag_veh(EXCEPTION_POINTERS* info)
{
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    const DWORD code = rec->ExceptionCode;
    // Only error-severity exceptions (access violations 0xC0000005, MSVC C++
    // throws 0xE06D7363, etc.). Skip breakpoints/status codes to avoid noise.
    if ((code & 0xC0000000u) != 0xC0000000u) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // Re-entrancy guard: if logging itself faults, don't recurse.
    if (InterlockedExchange(&g_af_diag_in_handler, 1)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const long seq = InterlockedIncrement(&g_af_diag_seq);
    const CONTEXT* ctx = info->ContextRecord;
    const uintptr_t fault_ip = reinterpret_cast<uintptr_t>(rec->ExceptionAddress);

    xlog::warn("[AF-DIAG #{}] first-chance exception 0x{:08X} at {}",
               seq, code, af_diag_addr_to_str(fault_ip));
    if (code == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        const auto kind = rec->ExceptionInformation[0];
        const char* op = kind == 1 ? "write" : (kind == 8 ? "execute" : "read");
        xlog::warn("[AF-DIAG #{}]   {} to 0x{:08X}", seq, op,
                   static_cast<uint32_t>(rec->ExceptionInformation[1]));
    }
    xlog::warn("[AF-DIAG #{}]   EAX={:08X} EBX={:08X} ECX={:08X} EDX={:08X} ESI={:08X} EDI={:08X}",
               seq, ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx, ctx->Esi, ctx->Edi);
    xlog::warn("[AF-DIAG #{}]   EIP={:08X} ESP={:08X} EBP={:08X}",
               seq, ctx->Eip, ctx->Esp, ctx->Ebp);
    // Flush the essential fault info now, in case the stack walk below faults.
    xlog::LoggerConfig::get().flush_appenders();

    // Walk the EBP frame chain (valid for functions with a normal frame).
    uintptr_t ebp = ctx->Ebp;
    for (int i = 0; i < 24 && ebp; ++i) {
        if (IsBadReadPtr(reinterpret_cast<void*>(ebp), sizeof(uintptr_t) * 2)) {
            break;
        }
        const uintptr_t ret = *reinterpret_cast<uintptr_t*>(ebp + sizeof(uintptr_t));
        const uintptr_t next = *reinterpret_cast<uintptr_t*>(ebp);
        if (ret) {
            xlog::warn("[AF-DIAG #{}]   ebp-frame[{}] {}", seq, i, af_diag_addr_to_str(ret));
        }
        if (next <= ebp) {
            break; // stack grows down; a valid saved EBP must increase
        }
        ebp = next;
    }

    // Raw stack scan for return addresses into AlpineFaction.dll. RF's boot loop
    // has no EBP frame, so the EBP chain above breaks before reaching AF code.
    af_diag_scan_af_frames(seq, ctx->Esp);

    xlog::LoggerConfig::get().flush_appenders();
    InterlockedExchange(&g_af_diag_in_handler, 0);
    return EXCEPTION_CONTINUE_SEARCH; // observe only; let normal handling proceed
}

} // namespace

#ifdef __GNUC__
// MinGW/GCC C++ throws go through __cxa_throw and, with DWARF-2 EH, raise no OS
// exception — so the vectored handler above never sees them. Hook __cxa_throw in
// our own module to log every throw (type name + AF stack) at its origin, before
// any unwinding. This is where the MinGW dedicated-server crash may originate.
extern "C" void __cxa_throw(void* thrown_object, std::type_info* tinfo, void (*dest)(void*));

static FunHook<void(void*, std::type_info*, void (*)(void*))> cxa_throw_hook{
    reinterpret_cast<uintptr_t>(&__cxa_throw),
    [](void* thrown_object, std::type_info* tinfo, void (*dest)(void*)) {
        const long seq = InterlockedIncrement(&g_af_diag_seq);
        const char* mangled = tinfo ? tinfo->name() : "(no type_info)";
        int status = -1;
        char* demangled = tinfo ? abi::__cxa_demangle(mangled, nullptr, nullptr, &status) : nullptr;
        xlog::warn("[AF-DIAG #{}] C++ throw of type '{}'", seq,
                   (demangled && status == 0) ? demangled : mangled);
        std::free(demangled); // free(nullptr) is a no-op
        volatile int stack_anchor = 0;
        af_diag_scan_af_frames(seq, reinterpret_cast<uintptr_t>(&stack_anchor));
        xlog::LoggerConfig::get().flush_appenders();
        cxa_throw_hook.call_target(thrown_object, tinfo, dest); // unwinds; never returns
        __builtin_unreachable();
    },
};
#endif // __GNUC__

static void install_crash_diagnostics()
{
    // Record this DLL's address range so the stack scan can pick out AF frames.
    HMODULE self = nullptr;
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&install_crash_diagnostics), &self) && self) {
        g_af_diag_base = reinterpret_cast<uintptr_t>(self);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_af_diag_base + dos->e_lfanew);
        g_af_diag_end = g_af_diag_base + nt->OptionalHeader.SizeOfImage;
    }
    AddVectoredExceptionHandler(1 /* call first */, &af_diag_veh);
#ifdef __GNUC__
    cxa_throw_hook.install(); // log MinGW C++ throws at their source
    xlog::warn("[AF-DIAG] __cxa_throw hook armed (C++ throw logging)");
#endif
    xlog::warn("[AF-DIAG] first-chance exception logging armed (AlpineFaction.dll base 0x{:08X}, size 0x{:X})",
               g_af_diag_base, static_cast<uint32_t>(g_af_diag_end - g_af_diag_base));
}

CallHook<void()> rf_init_hook{
    0x004B27CD,
    [] {
        const uint64_t start_ticks = GetTickCount64();
        xlog::info("Initializing game...");
        initialize_alpine_core_config();
        rf_init_hook.call_target();
        vpackfile_disable_overriding();
        xlog::info("Game initialized ({} ms).", GetTickCount64() - start_ticks);
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
        xlog::warn("[AF-DIAG] after_full_game_init: begin"); // TEMP breadcrumb
        multi_spectate_after_full_game_init();
#if !defined(NDEBUG) && defined(HAS_EXPERIMENTAL)
        experimental_init_after_game();
#endif
        xlog::warn("[AF-DIAG] after_full_game_init: pre console_init"); // TEMP breadcrumb
        console_init();
        xlog::warn("[AF-DIAG] after_full_game_init: pre multi_after_full_game_init"); // TEMP breadcrumb
        multi_after_full_game_init();
        xlog::warn("[AF-DIAG] after_full_game_init: pre debug_init"); // TEMP breadcrumb
        debug_init();
        xlog::warn("[AF-DIAG] after_full_game_init: pre load_world_hud_assets"); // TEMP breadcrumb
        if (!is_headless_mode()) {
            load_world_hud_assets();
        }
        xlog::warn("[AF-DIAG] after_full_game_init: pre execute_startup_scripts"); // TEMP breadcrumb
        execute_startup_scripts();
        xlog::warn("[AF-DIAG] after_full_game_init: end"); // TEMP breadcrumb
        xlog::LoggerConfig::get().flush_appenders(); // TEMP: flush breadcrumbs

        xlog::info("Game fully initialized");
        xlog::LoggerConfig::get().flush_appenders();
    },
};

CodeInjection cleanup_game_hook{
    0x004B2821,
    []() {
        // Set abort flag so AWP download future exits quickly and doesn't block static destruction
        cancel_awp_download();
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
        client_bot_do_frame();
        koth_do_frame();
        bagman_do_frame();
        alpine_mesh_do_frame();
        atx_do_frame();
        fflink::do_frame();
        int result = rf_do_frame_hook.call_target();
        maybe_autosave();
        debug_do_frame_post();
        multi_level_download_update();
        poll_awp_download();
        waypoints_do_frame();
        return result;
    },
};

CodeInjection after_level_render_hook{
    0x00432375,
    []() {
        if (is_headless_mode()) {
            return;
        }
#if !defined(NDEBUG) && defined(HAS_EXPERIMENTAL)
        experimental_render_in_game();
#endif
        debug_render();
        waypoints_render_debug();
        client_bot_render_debug();
        hud_world_do_frame();
    },
};

CodeInjection after_frame_render_hook{
    0x004B2DC2,
    [] {
        const rf::GameState state = rf::gameseq_get_state();
        if (!rf::is_dedicated_server
            && !is_headless_mode()
            && state != rf::GS_QUITING
            && state != rf::GS_NEW_LEVEL
            && state != rf::GS_MULTI_GETTING_STATE_INFO) {
            // Draw on top (after scene)
            frametime_render_ui();
            achievement_system_do_frame();
            fullscreen_overlay_do_frame();
            gas_region_transition_do_frame();
            spray_picker_render();
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
        waypoints_level_reset();
        atx_level_reset();
        alpine_camera_clear_static_mode();
        if (!save_filename.empty())
            xlog::info("Restoring game from save file: {}", save_filename);

        // attempt to load level_info tbl file
        load_level_info_config(level_filename);

        // evaluate and cache vertex lighting mode for this level (D3D11 only)
        if (is_d3d11()) {
            gr::d3d11::evaluate_mesh_lighting(level_filename);
            if (g_alpine_level_info_config.is_option_loaded(level_filename, AlpineLevelInfoID::UseVertexLighting)
                && get_level_info_value<bool>(AlpineLevelInfoID::UseVertexLighting)) {
                if (g_alpine_game_config.ignore_tbl_vertex_lighting) {
                    rf::console::print("Ignoring vertex lighting override in mapname_info.tbl for {} (cl_ignore_tbl_vertex_lighting is enabled)", level_filename);
                }
                else {
                    rf::console::print("Applying legacy vertex lighting for {} (per override present in mapname_info.tbl)", level_filename);
                }
            }

            gr::d3d11::evaluate_pixel_light_overbright(level_filename);
            if (g_alpine_level_info_config.is_option_loaded(level_filename, AlpineLevelInfoID::PixelLightOverbright)) {
                if (g_alpine_game_config.ignore_tbl_pixel_light_overbright) {
                    rf::console::print("Ignoring pixel light overbright override in mapname_info.tbl for {} (cl_ignore_tbl_pixel_light_overbright is enabled)", level_filename);
                }
                else {
                    rf::console::print("Pixel light overbright set to {:.2f} for {} (per override present in mapname_info.tbl)",
                        gr::d3d11::g_level_pixel_light_overbright, level_filename);
                }
            }

            gr::d3d11::evaluate_alpha_test_threshold(level_filename);
            if (is_stock_alpha_test_level(level_filename)) {
                rf::console::print("Applying stock alpha test threshold to known affected level {}", level_filename);
            }
        }

        // Notify about lightmap clamping TBL overrides
        if (g_alpine_level_info_config.is_option_loaded(level_filename, AlpineLevelInfoID::LightmapClampFloor)
            || g_alpine_level_info_config.is_option_loaded(level_filename, AlpineLevelInfoID::LightmapClampCeiling)) {
            if (g_alpine_game_config.ignore_tbl_lightmap_clamping) {
                rf::console::print("Ignoring lightmap clamping override in mapname_info.tbl for {} (cl_ignore_tbl_lightmap_clamping is enabled)", level_filename);
            }
            else {
                rf::console::print("Applying lightmap clamping for {} (per override present in mapname_info.tbl)", level_filename);
            }
        }

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

        // Cancel any in-flight AWP download from a previous map
        cancel_awp_download();

        // Flow 2A: Bot clients — start async AWP download before waypoints_level_init
        // so it sees the pending flag and defers load_waypoints until download resolves
        if (rf::is_multi && client_bot_launch_enabled()) {
            waypoints_set_awp_download_pending(true);
            if (!start_awp_download_for_installed_map(std::string{rf::level.filename.c_str()}, 3)) {
                waypoints_set_awp_download_pending(false);
            }
        }

        waypoints_level_init();

        // Flow 2B: Normal clients with autodl_download_awps — fire-and-forget AWP download
        if (rf::is_multi && !client_bot_launch_enabled() && !rf::is_dedicated_server
            && g_alpine_game_config.autodl_download_awps) {
            start_awp_download_for_installed_map(std::string{rf::level.filename.c_str()}, 1);
        }

        // Create corona objects (clutter + glare pairs) now that geometry is loaded
        alpine_corona_create_all();

        apply_maximum_fps(); // set maximum FPS based on game state
        process_queued_spawn_points_from_items();
        populate_world_hud_sprite_events();
        populate_fullscreen_overlay_events();
        reset_achievement_state_info();
        multi_level_init_post_gametypes();
        apply_geoable_flags();
        apply_breakable_materials();

        if (!rf::is_dedicated_server && !is_headless_mode()) {
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
    g_process_startup_time = std::time(nullptr);
    const uint64_t startup_ticks = GetTickCount64();

    // Init logging and crash dump support first
    init_logging();
    init_crash_handler();
    install_crash_diagnostics(); // TEMP: first-chance fault logging for the MinGW dedi crash

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
    fflink::do_patch();
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

    xlog::info("Installing hooks took {} ms", GetTickCount64() - startup_ticks);

    return 1; // success
}

BOOL WINAPI DllMain(
    const HINSTANCE instance_handle,
    [[maybe_unused]] const DWORD fdw_reason,
    [[maybe_unused]] const LPVOID lpv_reserved
) {
    g_hmodule = instance_handle;
    DisableThreadLibraryCalls(instance_handle);
    return TRUE;
}
