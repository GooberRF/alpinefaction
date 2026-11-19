#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <vector>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <toml++/toml.hpp>
#include <xlog/xlog.h>
#include "menu_scene.h"
#include "../graphics/gr.h"
#include "../graphics/d3d11/gr_d3d11_hooks.h"
#include "../main/main.h"
#include "../misc/alpine_settings.h"
#include "../misc/misc.h"
#include "../misc/settings_registry.h"
#include "../misc/vpackfile.h"
#include "../multi/demo/demo.h"
#include "../object/object.h"
#include "../os/console.h"
#include "../rf/ai.h"
#include "../rf/clutter.h"
#include "../rf/entity.h"
#include "../rf/event.h"
#include "../rf/file/file.h"
#include "../rf/geometry.h"
#include "../rf/glare.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_light.h"
#include "../rf/level.h"
#include "../rf/misc.h"
#include "../rf/multi.h"
#include "../rf/object.h"
#include "../rf/os/frametime.h"
#include "../rf/player/camera.h"
#include "../rf/player/player.h"
#include "../rf/sound/sound.h"
#include "../rf/ui.h"
#include "../rf/vmesh.h"
#include "../rf/weapon.h"

namespace
{

constexpr const char* config_filename = "af_main_menu.toml";
constexpr int background_live_level = 1;
constexpr int dim_alpha = 96;
// Far beyond any level, yet small enough for the room search and the view setup.
constexpr float max_coord = 100000.0f;
// A shorter shot would only flicker.
constexpr float min_shot_seconds = 1.0f;

struct Shot
{
    rf::Vector3 pos;
    rf::Matrix3 orient;
};

struct Config
{
    std::string level;
    // Entities go berserk as the level loads, as dbg_berserk does.
    bool berserk = false;
    float shot_seconds = 0.0f;
    float fade_seconds = 0.0f;
    std::vector<Shot> shots;
};

// The view of the last game frame, with the camera it was taken from
struct GameView
{
    rf::Vector3 pos;
    rf::Matrix3 orient;
    float fov = 0.0f;
    rf::Vector3 camera_pos;
    rf::Matrix3 camera_orient;
};

std::optional<Config> g_normal_config;
// Run instead while the menus' seasonal effect is on.
std::optional<Config> g_seasonal_config;
// The one the scene runs, picked as it starts.
const Config* g_config = nullptr;
// A load that failed is not retried until the setting is changed
bool g_load_failed = false;
// Also set while the scene's own level loads, for the hooks below.
bool g_active = false;
std::vector<Shot> g_shots;
std::size_t g_shot_index = 0;
float g_shot_time = 0.0f;
std::optional<GameView> g_game_view;

std::optional<float> finite(const std::optional<double> value)
{
    // Also rejects NaN and a finite double out of a float's range.
    if (!value || !(std::abs(*value) <= std::numeric_limits<float>::max())) {
        return std::nullopt;
    }
    return static_cast<float>(*value);
}

std::optional<Shot> read_shot(const toml::node& node)
{
    const toml::table* const table = node.as_table();
    const toml::array* const pos = table ? (*table)["pos"].as_array() : nullptr;
    if (!pos || pos->size() != 3) {
        return std::nullopt;
    }
    float coords[3];
    for (std::size_t i = 0; i < 3; ++i) {
        const std::optional<float> coord = finite((*pos)[i].value<double>());
        if (!coord || std::abs(*coord) > max_coord) {
            return std::nullopt;
        }
        coords[i] = *coord;
    }
    const std::optional<float> pitch = finite((*table)["pitch"].value<double>());
    const std::optional<float> heading = finite((*table)["heading"].value<double>());
    if (!pitch || !heading) {
        return std::nullopt;
    }
    // set_from_angles uses the x87 sine and cosine, which give garbage for huge angles.
    constexpr float deg_to_rad = std::numbers::pi_v<float> / 180.0f;
    Shot shot;
    shot.pos = {coords[0], coords[1], coords[2]};
    shot.orient.set_from_angles(std::fmod(*pitch, 360.0f) * deg_to_rad, 0.0f,
                                std::fmod(*heading, 360.0f) * deg_to_rad);
    return shot;
}

std::optional<Config> parse_level(const toml::table& live)
{
    Config config;
    config.level = live["level"].value_or(std::string{});
    config.berserk = live["berserk"].value_or(false);
    if (config.level.empty()) {
        return std::nullopt;
    }
    if (!rf::File{}.find(config.level.c_str())) {
        xlog::warn("[MenuScene] {} is not installed", config.level);
        return std::nullopt;
    }
    const std::optional<float> shot_seconds = finite(live["shot_seconds"].value<double>());
    if (!shot_seconds || *shot_seconds < min_shot_seconds) {
        xlog::warn("[MenuScene] {}: shot_seconds must be at least {}", config_filename, min_shot_seconds);
        return std::nullopt;
    }
    config.shot_seconds = *shot_seconds;
    config.fade_seconds =
        std::clamp(finite(live["fade_seconds"].value<double>()).value_or(0.0f), 0.0f, *shot_seconds / 2.0f);

    if (const toml::array* const shots = live["shot"].as_array()) {
        for (std::size_t i = 0; i < shots->size(); ++i) {
            if (std::optional<Shot> shot = read_shot((*shots)[i])) {
                config.shots.push_back(*shot);
            }
            else {
                xlog::warn("[MenuScene] {}: {} shot {} needs pos = [x, y, z], pitch and heading", config_filename,
                           config.level, i + 1);
            }
        }
    }
    if (config.shots.empty()) {
        xlog::warn("[MenuScene] {}: {} has no usable shot", config_filename, config.level);
        return std::nullopt;
    }
    return config;
}

std::optional<Config> parse_level(const toml::table& root, const char* name)
{
    const toml::table* const live = root[name].as_table();
    return live ? parse_level(*live) : std::nullopt;
}

// The seasonal level while the menus' seasonal effect is on, else (or without a usable one) the normal one.
const Config* pick_config()
{
    if (g_seasonal_config && main_menu_seasonal_effect_active()) {
        return &*g_seasonal_config;
    }
    return g_normal_config ? &*g_normal_config : nullptr;
}

// Legacy menus always show the stock Mars background.
bool background_wanted()
{
    return g_alpine_game_config.main_menu_background == background_live_level && !legacy_menus_enabled();
}

// The credits keep the scene loaded but draw their own screen.
bool scene_shown(rf::GameState state)
{
    return g_active && !g_shots.empty() && state != rf::GS_CREDITS && menu_scene_state(state);
}

void place_camera(const Shot& shot)
{
    rf::Camera* const camera = rf::local_player ? rf::local_player->cam : nullptr;
    rf::Entity* const camera_entity = camera ? camera->camera_entity : nullptr;
    if (!camera_entity) {
        return;
    }
    // Weather and the monitors' culling follow the local camera.
    camera_entity->pos = shot.pos;
    camera_entity->orient = shot.orient;
    camera_entity->eye_pos = shot.pos;
    camera_entity->eye_orient = shot.orient;
    camera_entity->set_room(nullptr);
    camera_entity->update_room();
}

// The world part of gameplay_render_frame (0x00431A00), with the Alpine patches on its call sites.
void render_world(const rf::Vector3 pos, const rf::Matrix3 orient, const float fov, rf::GRoom* const eye_room)
{
    rf::GSolid* const solid = rf::g_level_solid;

    solid->clear_rooms_visited_this_frame();
    rf::monitor_render_all(rf::local_player);
    // The monitor patch leaves the last monitor's bitmap bound; gameplay restores it in render_to_dynamic_textures.
    gr_set_render_target(-1);
    rf::gr::reset_clip();

    rf::render_player = rf::local_player;
    rf::gr_set_far_clip(level_default_far_clip());
    rf::gr::set_clip(0, 0, rf::gr::screen_width(), rf::gr::screen_height());
    rf::gr::gameplay_fov = fov;
    rf::gr::setup_3d(&orient, &pos, rf::gr::gameplay_fov, 1, 1);

    if (rf::level.distance_fog_far_clip > 0.0f) {
        const rf::Color& fog = rf::level.distance_fog_color;
        if (!rf::level.has_skyroom) {
            const float camera_far_clip = level_camera_far_clip();
            rf::gr_set_far_clip(camera_far_clip > 0.0f ? camera_far_clip : rf::level.distance_fog_far_clip);
            rf::gr::fog_set(1, fog.red, fog.green, fog.blue, rf::level.distance_fog_near_clip,
                            rf::level.distance_fog_far_clip);
        }
        rf::gr::set_color(fog.red, fog.green, fog.blue, 255);
    }
    else {
        rf::gr::set_color(0, 0, 0, 255);
    }
    rf::gr::clear();

    rf::gr::light_frame_begin();
    rf::g_decal_do_frame();
    rf::gr::light_filter_for_camera();
    rf::obj_reset_render_flags();
    const rf::Matrix3* const sky_rotation = rf::sky_room_rotation_rate > 0.0f ? &rf::sky_room_rotation : nullptr;
    rf::g_solid_portal_render(solid, eye_room, 0, sky_rotation);
    rf::gr::set_color(255, 255, 255, 255);
    // The Direct3D 11 shadow maps replace the blob shadows above quality 0.
    if (g_alpine_game_config.shadow_quality <= 0) {
        const MovingLiquidFacePlanes liquid_planes;
        rf::obj_shadow_render_all();
    }
    rf::clutter_render_all_clocks();
    rf::glare_render_all_flares(rf::level.has_mirrored_faces);
    rf::rail_beam_render_all();
    render_level_world_effects();
    rf::gr::flush();
    rf::gr::fog_set(0, 0, 0, 0, -1.0f, -1.0f);
    rf::render_player = nullptr;

    // The menus' 2D rects take the lit path, which would add the lights uploaded for the level.
    rf::gr::num_relevant_lights = 0;
    gr::d3d11::clear_mesh_lights();
    // Drawn now, under the menu. Left for flip(), they would draw over it, after a stock popup's choice (Quit:
    // multi_stop unloads the multiplayer characters) may have freed the meshes they point at.
    gr::d3d11::flush_scene_outlines();
    rf::gr::reset_clip();
}

void fill_screen(int alpha)
{
    rf::gr::set_color(0, 0, 0, static_cast<rf::ubyte>(alpha));
    rf::gr::rect(0, 0, rf::gr::screen_width(), rf::gr::screen_height());
}

// A direction held fixed to a camera that turned from `from` to `to`
rf::Vector3 carry(const rf::Vector3& dir, const rf::Matrix3& from, const rf::Matrix3& to)
{
    return to.transform_vector({dir.dot_prod(from.rvec), dir.dot_prod(from.uvec), dir.dot_prod(from.fvec)});
}

// The game a menu was opened over, seen as its last frame was. In multiplayer the level runs on beneath the menus,
// so the view follows the camera.
bool draw_game_beneath()
{
    if (!g_game_view || !background_wanted() || !is_d3d11()
        || !(rf::level.flags & rf::LEVEL_LOADED) || !gameseq_is_deep_stacked(rf::GS_GAMEPLAY)) {
        return false;
    }
    rf::Camera* const camera = rf::local_player ? rf::local_player->cam : nullptr;
    if (!camera || !camera->camera_entity) {
        return false;
    }
    GameView view = *g_game_view;
    const rf::Vector3 camera_pos = rf::camera_get_pos(camera);
    const rf::Matrix3 camera_orient = rf::camera_get_orient(camera);
    if (camera_pos != view.camera_pos || camera_orient != view.camera_orient) {
        view.pos = camera_pos + carry(view.pos - view.camera_pos, view.camera_orient, camera_orient);
        view.orient.rvec = carry(view.orient.rvec, view.camera_orient, camera_orient);
        view.orient.uvec = carry(view.orient.uvec, view.camera_orient, camera_orient);
        view.orient.fvec = carry(view.orient.fvec, view.camera_orient, camera_orient);
    }
    // render_world has no underwater fog or overlay.
    rf::GRoom* const eye_room = rf::camera_get_room(camera);
    if (!eye_room || rf::point_in_liquid(eye_room, &view.pos)) {
        return false;
    }
    {
        // Single player never simulates the level beneath the menus, nor does a paused demo, so the frame must not age
        // as it is drawn.
        std::optional<FrametimeGuard> frozen;
        if (!rf::is_multi || demo_playback_sim_frozen()) {
            frozen.emplace();
        }
        render_world(view.pos, view.orient, view.fov, eye_room);
    }
    fill_screen(dim_alpha);
    return true;
}

// gameplay_render_frame's world pass, after its gr_setup_3d, which left the view in rf::gr::eye_*.
CallHook<void(rf::GSolid*, rf::GRoom*, int, const rf::Matrix3*)> gameplay_render_frame_portal_render_hook{
    0x00431FF8,
    [](rf::GSolid* solid, rf::GRoom* eye_room, int flags, const rf::Matrix3* sky_rotation) {
        rf::Camera* const camera = rf::local_player ? rf::local_player->cam : nullptr;
        // Flag 2 is the rail scanner's view, set up with its own FOV; the view before it stands in.
        if (camera && rf::render_player == rf::local_player && !(flags & 2)) {
            g_game_view = GameView{rf::gr::eye_pos, rf::gr::eye_matrix, rf::gr::gameplay_fov,
                                   rf::camera_get_pos(camera), rf::camera_get_orient(camera)};
        }
        gameplay_render_frame_portal_render_hook.call_target(solid, eye_room, flags, sky_rotation);
    },
};

CallHook<char*(const char*)> level_load_intro_movie_hook{
    0x0045C862,
    [](const char* filename) {
        if (g_active) {
            return static_cast<char*>(nullptr);
        }
        return level_load_intro_movie_hook.call_target(filename);
    },
};

CallHook<void(rf::GameState, bool)> level_init_post_set_gameplay_state_hook{
    0x00436081,
    [](rf::GameState state, bool force) {
        if (!g_active) {
            level_init_post_set_gameplay_state_hook.call_target(state, force);
        }
    },
};

CallHook<int(const char*)> level_init_post_levelstart_script_hook{
    0x0043611A,
    [](const char* filename) {
        if (g_active) {
            return 0;
        }
        return level_init_post_levelstart_script_hook.call_target(filename);
    },
};

void __fastcall event_music_start_turn_on(rf::Event* event);
FunHook<decltype(event_music_start_turn_on)> event_music_start_turn_on_hook{0x004BCD70, event_music_start_turn_on};
void __fastcall event_music_start_turn_on(rf::Event* event)
{
    if (!g_active) {
        event_music_start_turn_on_hook.call_target(event);
    }
}

void __fastcall event_music_stop_turn_on(rf::Event* event);
FunHook<decltype(event_music_stop_turn_on)> event_music_stop_turn_on_hook{0x004BCD90, event_music_stop_turn_on};
void __fastcall event_music_stop_turn_on(rf::Event* event)
{
    if (!g_active) {
        event_music_stop_turn_on_hook.call_target(event);
    }
}

// The scene has no player entity, which finding the player reads unchecked.
void ai_set_mode(rf::AiInfo* ai_info, rf::AiMode mode, int mode_param_0, int mode_param_1);
FunHook<decltype(ai_set_mode)> ai_set_mode_hook{0x00407E20, ai_set_mode};
void ai_set_mode(rf::AiInfo* ai_info, rf::AiMode mode, int mode_param_0, int mode_param_1)
{
    if (g_active && mode == rf::AI_MODE_FIND_PLAYER) {
        mode = rf::AI_MODE_WAITING;
    }
    ai_set_mode_hook.call_target(ai_info, mode, mode_param_0, mode_param_1);
}

// Airlock triggers read the player entity unchecked too; the scene runs them as plain triggers.
CodeInjection trigger_do_frame_airlock_injection{
    0x004BF804,
    [](auto& regs) {
        if (g_active) {
            regs.eip = 0x004BF880;
        }
    },
};

CodeInjection trigger_activate_airlock_injection{
    0x004BFDC4,
    [](auto& regs) {
        if (g_active) {
            regs.eip = 0x004C0016;
        }
    },
};

ConsoleCommand2 menu_background_cmd{
    "ui_menu_background",
    [](std::optional<int> style) {
        if (style) {
            setting_console_set("main_menu_background", std::to_string(style.value()));
        }
        rf::console::print("Menu background: {} [0 = Mars, 1 = live level]",
                           g_alpine_game_config.main_menu_background);
    },
    "Set the menu background",
    "ui_menu_background [0-1]",
};

} // namespace

void menu_scene_load_config()
{
    const std::optional<std::string> content = read_game_file(config_filename);
    if (!content) {
        xlog::warn("[MenuScene] Could not read {}", config_filename);
        return;
    }
    try {
        const toml::table root = toml::parse(*content, std::string{config_filename});
        g_normal_config = parse_level(root, "live_level_normal");
        g_seasonal_config = parse_level(root, "live_level_seasonal");
        if (!g_normal_config) {
            xlog::warn("[MenuScene] {} has no usable [live_level_normal]", config_filename);
        }
    }
    catch (const toml::parse_error& err) {
        xlog::warn("[MenuScene] Failed to parse {}: {}", config_filename, err.description());
    }
}

bool menu_scene_active()
{
    return g_active;
}

bool menu_scene_state(rf::GameState state)
{
    switch (state) {
        case rf::GS_MAIN_MENU:
        case rf::GS_EXTRAS_MENU:
        case rf::GS_OPTIONS_MENU:
        case rf::GS_LOAD_GAME_MENU:
        case rf::GS_SAVE_GAME_MENU:
        case rf::GS_MESSAGE_LOG:
        case rf::GS_CREDITS:
        case rf::GS_MULTI_MENU:
        case rf::GS_MULTI_SERVER_LIST:
        case rf::GS_MULTI_SPLITSCREEN:
        case rf::GS_MULTI_CREATE_GAME:
            return true;
        default:
            return false;
    }
}

void menu_scene_maybe_start()
{
    // The config is only loaded outside a dedicated server and headless mode.
    if (g_active || g_load_failed || !background_wanted() || !is_d3d11() || rf::is_multi || !rf::local_player
        || (rf::level.flags & rf::LEVEL_LOADED)) {
        return;
    }
    g_config = pick_config();
    if (!g_config) {
        return;
    }

    rf::vmesh_static_cache_purge();
    rf::game_level_init_pre(false);
    g_active = true;
    rf::String filename{g_config->level.c_str()};
    rf::String state_filename;
    char error[1024] = {};
    const bool loaded = rf::level_load(filename, state_filename, error) == 0;
    if (loaded) {
        rf::game_level_init_post(false);
    }
    // Left set, the Multiplayer button and a new game would close the scene through game_shutdown.
    rf::game_initialized = false;
    if (!loaded) {
        xlog::warn("[MenuScene] Could not load {}: {}", g_config->level, error);
        g_active = false;
        g_load_failed = true;
        return;
    }

    g_shots.clear();
    for (const Shot& shot : g_config->shots) {
        if (rf::find_room(rf::g_level_solid, &shot.pos)) {
            g_shots.push_back(shot);
        }
        else {
            xlog::warn("[MenuScene] Dropping the shot at {} {} {}: it is outside {}", shot.pos.x, shot.pos.y,
                       shot.pos.z, g_config->level);
        }
    }
    if (g_shots.empty()) {
        menu_scene_stop();
        g_load_failed = true;
        return;
    }
    if (g_config->berserk) {
        entities_go_berserk();
    }
    g_shot_index = 0;
    g_shot_time = 0.0f;
    place_camera(g_shots.front());
}

void menu_scene_stop()
{
    if (!g_active) {
        return;
    }
    g_active = false;
    g_shots.clear();
    rf::level_close();
}

void menu_scene_on_level_closed()
{
    g_active = false;
    g_shots.clear();
    g_game_view.reset();
}

void menu_scene_do_frame(rf::GameState state)
{
    if (!scene_shown(state)) {
        return;
    }
    rf::gameplay_sim_frame();
    g_shot_time += rf::frametime;
    const float shot_seconds = g_config->shot_seconds;
    if (g_shot_time >= shot_seconds) {
        g_shot_time = std::fmod(g_shot_time, shot_seconds);
        g_shot_index = (g_shot_index + 1) % g_shots.size();
    }
    place_camera(g_shots[g_shot_index]);
}

bool menu_scene_draw_background()
{
    if (!g_alpine_game_config.rendering_enabled) {
        return false;
    }
    if (!scene_shown(rf::gameseq_get_state())) {
        return draw_game_beneath();
    }
    const Shot& shot = g_shots[g_shot_index];
    // Looked up each frame, as the engine does for its own camera.
    rf::GRoom* const eye_room = rf::find_room(rf::g_level_solid, &shot.pos);
    if (!eye_room) {
        return false;
    }
    render_world(shot.pos, shot.orient, gr_scale_world_fov(90.0f), eye_room);
    fill_screen(dim_alpha);
    if (g_config->fade_seconds > 0.0f) {
        const float edge = std::min(g_shot_time, g_config->shot_seconds - g_shot_time);
        const float fade = 1.0f - std::clamp(edge / g_config->fade_seconds, 0.0f, 1.0f);
        if (fade > 0.0f) {
            fill_screen(static_cast<int>(std::lround(fade * 255.0f)));
        }
    }
    return true;
}

void menu_scene_on_setting_changed()
{
    g_load_failed = false;
    if (!background_wanted() && g_active) {
        menu_scene_stop();
        // Closing the level stopped the menu music; restarted as the main menu's init (0x00443270) does.
        rf::snd_eax_room_off();
        rf::ui::main_menu_music_play(rf::ui::main_menu_music_filename.c_str());
        rf::ui::main_menu_music_stopped = 0;
    }
}

void menu_scene_apply_patch()
{
    level_load_intro_movie_hook.install();
    level_init_post_set_gameplay_state_hook.install();
    level_init_post_levelstart_script_hook.install();
    event_music_start_turn_on_hook.install();
    event_music_stop_turn_on_hook.install();
    ai_set_mode_hook.install();
    trigger_do_frame_airlock_injection.install();
    trigger_activate_airlock_injection.install();
    gameplay_render_frame_portal_render_hook.install();
    menu_background_cmd.register_cmd();
}
