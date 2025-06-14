#include <xlog/xlog.h>
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include <patch_common/AsmWriter.h>
#include <algorithm>
#include <unordered_set>
#include "hud_internal.h"
#include "hud.h"
#include "hud_world.h"
#include "multi_spectate.h"
#include "../object/event_alpine.h"
#include "../multi/server.h"
#include "../misc/alpine_settings.h"
#include "../sound/sound.h"
#include "../rf/hud.h"
#include "../rf/player/player.h"
#include "../rf/player/camera.h"
#include "../rf/entity.h"
#include "../rf/bmpman.h"
#include "../rf/multi.h"
#include "../rf/gameseq.h"
#include "../rf/level.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/localize.h"
#include "../os/console.h"
#include <patch_common/CallHook.h>
#include "../rf/weapon.h"
#include "../misc/alpine_options.h"

WorldHUDAssets g_world_hud_assets;
bool draw_mp_spawn_world_hud = false;
std::unordered_set<rf::EventWorldHUDSprite*> world_hud_sprite_events;
std::vector<EphemeralWorldHUDSprite> ephemeral_world_hud_sprites;
std::vector<EphemeralWorldHUDString> ephemeral_world_hud_strings;

void load_world_hud_assets() {
    g_world_hud_assets.flag_red_d = rf::bm::load("af_wh_ctf_red_d.tga", -1, true);
    g_world_hud_assets.flag_blue_d = rf::bm::load("af_wh_ctf_blue_d.tga", -1, true);
    g_world_hud_assets.flag_red_a = rf::bm::load("af_wh_ctf_red_a.tga", -1, true);
    g_world_hud_assets.flag_blue_a = rf::bm::load("af_wh_ctf_blue_a.tga", -1, true);
    g_world_hud_assets.flag_red_s = rf::bm::load("af_wh_ctf_red_s.tga", -1, true);
    g_world_hud_assets.flag_blue_s = rf::bm::load("af_wh_ctf_blue_s.tga", -1, true);
    g_world_hud_assets.mp_respawn = rf::bm::load("af_wh_mp_spawn.tga", -1, true);
}

void do_render_world_hud_sprite(rf::Vector3 pos, float base_scale, int bitmap_handle,
    WorldHUDRenderMode render_mode, bool stay_inside_fog, bool distance_scaling, bool only_draw_during_gameplay) {

    if (only_draw_during_gameplay && rf::gameseq_get_state() != rf::GameState::GS_GAMEPLAY) {
        return;
    }

    auto vec = pos;
    auto scale = base_scale;
    auto bitmap_mode = rf::gr::bitmap_3d_mode_no_z; // default (no_overdraw)

    // handle render mode
    switch (render_mode) {
        case WorldHUDRenderMode::no_overdraw_glow:
            bitmap_mode = rf::gr::glow_3d_bitmap_mode;
            break;
        case WorldHUDRenderMode::overdraw:
            bitmap_mode = rf::gr::bitmap_3d_mode;
            break;
        case WorldHUDRenderMode::no_overdraw:
        default:
            bitmap_mode = rf::gr::bitmap_3d_mode_no_z;
            break;
    }

    // handle distance scaling and fog distance adjustment
    if (stay_inside_fog || distance_scaling) {
        rf::Camera* camera = rf::local_player->cam;
        rf::Vector3 camera_pos = rf::camera_get_pos(camera);
        float distance = vec.distance_to(camera_pos);

        // If the icon would be clipped due to being beyond the fog far clip, draw it just inside instead
        if (stay_inside_fog) {
            float fog_far_clip = rf::level.distance_fog_far_clip;

            // enforce min and max effective fog distance
            float max_distance = fog_far_clip > WorldHUDRender::fog_dist_min
                                     ? std::min(fog_far_clip, WorldHUDRender::fog_dist_max)
                                     : WorldHUDRender::fog_dist_max;

            // adjust sprite position
            if (distance > max_distance * WorldHUDRender::fog_dist_multi) {
                rf::Vector3 direction = vec - camera_pos;
                direction.normalize_safe();

                vec = camera_pos + (direction * (max_distance * WorldHUDRender::fog_dist_multi));
                distance = max_distance * WorldHUDRender::fog_dist_multi;
            }
        }

        // Scale icon based on distance from camera
        if (distance_scaling) {
            float scale_factor = std::max(distance, 1.0f) / WorldHUDRender::reference_distance;
            scale = std::clamp(base_scale * scale_factor, WorldHUDRender::min_scale, WorldHUDRender::max_scale);
        }
    }

    // draw sprite
    rf::gr::set_texture(bitmap_handle, -1);
    rf::gr::gr_3d_bitmap_angle(&vec, 0.0f, scale, bitmap_mode);
}

// draw CTF flag sprites
void build_ctf_flag_icons()
{
    if (!rf::ctf_red_flag_item || !rf::ctf_blue_flag_item) {
        return; // don't render unless map has both flags
    }

    bool team = rf::local_player->team;

    auto build_and_render_flag_icon = [&](bool player_team, bool flag_team) {

        rf::Vector3 vec = flag_team ? rf::ctf_blue_flag_pos : rf::ctf_red_flag_pos;
        vec.y += WorldHUDRender::ctf_flag_offset; // position icon above flag origin

        // Choose texture based on team and flag type
        int bitmap_handle = -1;
        if (flag_team) {
            if (rf::multi_ctf_is_blue_flag_in_base()) {
                bitmap_handle = player_team ? g_world_hud_assets.flag_blue_d : g_world_hud_assets.flag_blue_a;
            }
            else {
                bitmap_handle = g_world_hud_assets.flag_blue_s;
            }
        }
        else {
            if (rf::multi_ctf_is_red_flag_in_base()) {
                bitmap_handle = player_team ? g_world_hud_assets.flag_red_a : g_world_hud_assets.flag_red_d;
            }
            else {
                bitmap_handle = g_world_hud_assets.flag_red_s;
            }
        }

        auto render_mode = g_alpine_game_config.world_hud_overdraw ? WorldHUDRenderMode::overdraw : WorldHUDRenderMode::no_overdraw;

        do_render_world_hud_sprite(vec, 0.6f, bitmap_handle, render_mode, true, true, true);
    };

    // render flag sprites
    build_and_render_flag_icon(team, false); // red
    build_and_render_flag_icon(team, true);  // blue
}

void build_mp_respawn_icons() {
    auto all_respawn_points = get_new_multi_respawn_points();

    for (auto& point : all_respawn_points) {
        // build colour for icon and arrow
        int r = 200;
        int g = 200;
        int b = 200;

        if (point.red_team && !point.blue_team) {
            r = 167;
            g = 0;
            b = 0;
        }
        else if (point.blue_team && !point.red_team) {
            r = 52;
            g = 78;
            b = 167;
        }

        // draw an arrow in the direction of the spawn point
        rf::Vector3 arrow_end = point.position + (point.orientation.fvec * 1.5f);
        rf::gr::gr_line_arrow(
            point.position.x, point.position.y, point.position.z,
            arrow_end.x, arrow_end.y, arrow_end.z,
            r, g, b);

        rf::gr::set_color(r, g, b);
        do_render_world_hud_sprite(point.position, 1.0, g_world_hud_assets.mp_respawn,
                                   WorldHUDRenderMode::no_overdraw_glow, false, false, true);
    }
}

void build_world_hud_sprite_icons() {
    bool team = rf::local_player->team;

    for (auto& event : world_hud_sprite_events) {
        if (event->enabled) {
            if (team && event->sprite_filename_blue_int.has_value()) {
                do_render_world_hud_sprite(event->pos, event->scale, event->sprite_filename_blue_int.value_or(-1),
                    event->render_mode, false, false, true);
            }
            else if (event->sprite_filename_int.has_value()) {
                do_render_world_hud_sprite(event->pos, event->scale, event->sprite_filename_int.value_or(-1),
                    event->render_mode, false, false, true);
            }
        }
    }
}

void render_string_3d_pos_new(const rf::Vector3& pos, const std::string& text, int offset_x, int offset_y,
    int font, rf::ubyte r, rf::ubyte g, rf::ubyte b, rf::ubyte a)
{
    rf::gr::Vertex dest;

    // Transform the position to screen space
    if (!rf::gr::rotate_vertex(&dest, pos))
    {
        rf::gr::project_vertex(&dest);

        // Check if projection was successful
        if (dest.flags & 1)
        {
            int screen_x = static_cast<int>(dest.sx) + offset_x;
            int screen_y = static_cast<int>(dest.sy) + offset_y;
            // text_3d_mode and text_2d_mode both have issues with fog
            auto render_mode = rf::level.distance_fog_far_clip == 0.0f ? rf::gr::text_2d_mode : rf::gr::bitmap_3d_mode;
            rf::gr::set_color(r, g, b, a);
            rf::gr::string(screen_x, screen_y, text.c_str(), font, render_mode);
        }
    }
}

void build_player_labels() {
    bool is_spectating = multi_spectate_is_spectating();
    bool show_all = is_spectating && g_alpine_game_config.world_hud_spectate_player_labels;
    bool is_team_mode = rf::multi_get_game_type() != rf::NetGameType::NG_TYPE_DM;
    bool show_teammates = g_alpine_game_config.world_hud_team_player_labels && is_team_mode && !is_spectating;
    auto spectate_target = multi_spectate_get_target_player();

    int font = !g_alpine_game_config.world_hud_big_text;
    auto player_list = SinglyLinkedList{rf::player_list};

    for (auto& player : player_list) {
        rf::Entity* player_entity = rf::entity_from_handle(player.entity_handle);

        if (!player_entity) {
            continue; // not spawned
        }

        if (rf::entity_is_dying(player_entity)) {
            continue; // dying
        }

        if (player_entity == rf::local_player_entity) {
            continue; // myself
        }

        // Determine if this player's label should be shown
        if (!(show_all || (show_teammates && player.team == rf::local_player->team))) {
            continue; // Don't show non-teammates if not spectating
        }

        if (is_spectating && spectate_target && &player == spectate_target) {
            continue; // Don't show spectated player label
        }

        rf::Vector3 string_pos = player_entity->pos;
        string_pos.y += 0.85f;
        std::string label = player.name;

        // determine label width
        int text_width = 0, text_height = 0;
        rf::gr::gr_get_string_size(&text_width, &text_height, label.c_str(), label.size(), font);
        int half_text_width = text_width / 2;

        render_string_3d_pos_new(string_pos, label.c_str(), -half_text_width, -25, font, 200, 200, 200, 223);
    }
}

void build_ephemeral_world_hud_sprite_icons() {
    std::erase_if(ephemeral_world_hud_sprites, [](const EphemeralWorldHUDSprite& es) {
        return !es.timestamp.valid() || es.timestamp.elapsed();
    });

    for (const auto& es : ephemeral_world_hud_sprites) {
        int font = hud_get_messages_font();

        if (es.bitmap != -1) {
            do_render_world_hud_sprite(es.pos, 1.0f, es.bitmap, es.render_mode, true, true, true);
        }

        // determine label width
        int text_width = 0, text_height = 0;
        rf::gr::gr_get_string_size(&text_width, &text_height, es.label.c_str(), es.label.size(), font);
        int half_text_width = text_width / 2;

        auto text_pos = es.pos;
        render_string_3d_pos_new(text_pos, es.label.c_str(), -half_text_width, -25,
            font, 255, 255, 255, 223);
    }
}

void build_ephemeral_world_hud_strings() {
    std::erase_if(ephemeral_world_hud_strings, [](const EphemeralWorldHUDString& es) {
        return !es.timestamp.valid() || es.timestamp.elapsed();
    });

    for (const auto& es : ephemeral_world_hud_strings) {
        int label_y_offset = 0;
        int font = hud_get_messages_font();
        rf::Vector3 string_pos = es.pos;
        string_pos.y += 0.85f;

        if (es.float_away) {
            // Calculate the progress of the fade effect
            float progress = es.duration > 0
                ? static_cast<float>(es.timestamp.time_until()) / static_cast<float>(es.duration)
                : 0.0f;
            progress = std::clamp(progress, 0.0f, 1.0f); // confirm within bounds
            string_pos.y += (1.0f - progress) * 3.0f;

            // Apply wind effect
            float elapsed_time = static_cast<float>(es.duration) * (1.0f - progress);
            float wind_amplitude = 0.15f;
            float wind_frequency_x = 12.0f;
            float wind_frequency_z = 9.0f;

            string_pos.x += wind_amplitude * std::sin((elapsed_time * 0.002f) + es.wind_phase_offset);
            string_pos.z += wind_amplitude * std::cos((elapsed_time * 0.002f) + es.wind_phase_offset * 0.8f);
        }

        std::string label = std::to_string(es.damage);

        // determine label width
        int text_width = 0, text_height = 0;
        rf::gr::gr_get_string_size(&text_width, &text_height, label.c_str(), label.size(), font);
        int half_text_width = text_width / 2;

        render_string_3d_pos_new(string_pos, label.c_str(), -half_text_width, -25,
            font, 255, 255, 255, 223);
    }
}

void hud_world_do_frame() {
    if (rf::is_multi && g_alpine_game_config.world_hud_ctf_icons && rf::multi_get_game_type() == rf::NetGameType::NG_TYPE_CTF) {
        build_ctf_flag_icons();
    }
    if (g_pre_match_active || (draw_mp_spawn_world_hud && (!rf::is_multi || rf::is_server))) {
        build_mp_respawn_icons();
    }
    if (!world_hud_sprite_events.empty()) {
        build_world_hud_sprite_icons();
    }
    if (!ephemeral_world_hud_sprites.empty()) {
        build_ephemeral_world_hud_sprite_icons();
    }
    if (!ephemeral_world_hud_strings.empty()) {
        build_ephemeral_world_hud_strings();
    }
    if (rf::is_multi &&
    ((multi_spectate_is_spectating() && g_alpine_game_config.world_hud_spectate_player_labels) ||
    (!multi_spectate_is_spectating() && g_alpine_game_config.world_hud_team_player_labels && rf::multi_get_game_type() != rf::NetGameType::NG_TYPE_DM))) {
        build_player_labels();
    }
}

void populate_world_hud_sprite_events()
{
    world_hud_sprite_events.clear();

    std::vector<rf::Event*> events = rf::find_all_events_by_type(rf::EventType::World_HUD_Sprite);

    for (rf::Event* event : events) {
        if (auto* hud_sprite_event = dynamic_cast<rf::EventWorldHUDSprite*>(event)) {
            world_hud_sprite_events.insert(hud_sprite_event);
            hud_sprite_event->build_sprite_ints();
        }
    }
}

void add_location_ping_world_hud_sprite(rf::Vector3 pos, std::string player_name, int player_id)
{
    // Remove any existing entry from the same player name
    std::erase_if(ephemeral_world_hud_sprites,
        [&](const EphemeralWorldHUDSprite& es) { return es.player_id == player_id; });

    auto bitmap = rf::bm::load("af_wh_ping1.tga", -1, true);

    EphemeralWorldHUDSprite es;
    es.bitmap = bitmap;
    es.pos = pos;
    es.label = player_name;
    es.player_id = player_id;
    es.timestamp.set(4000);

    play_local_sound_3d(get_custom_sound_id(1), pos, 0, 1.0f);

    ephemeral_world_hud_sprites.push_back(es);
}

void add_damage_notify_world_hud_string(rf::Vector3 pos, uint8_t damaged_player_id, uint16_t damage, bool died)
{
    if (!g_alpine_game_config.world_hud_damage_numbers) {
        return; // turned off
    }

    std::uniform_real_distribution<float> wind_offset_dist(0.0f, 3.14f * 2);

    // Use cumulative damage values for the same player_id unless disabled
    if (!g_alpine_game_config.world_hud_alt_damage_indicators) {
        // Search for an existing entry with the same player_id
        auto it = std::find_if(
            ephemeral_world_hud_strings.begin(), ephemeral_world_hud_strings.end(),
            [damaged_player_id](const EphemeralWorldHUDString& es) { return es.player_id == damaged_player_id; });

        if (it != ephemeral_world_hud_strings.end()) {
            // If found, sum the damage values and remove the old entry
            damage += it->damage;
            ephemeral_world_hud_strings.erase(it);
        }
    }

    EphemeralWorldHUDString es;
    es.pos = pos;
    es.player_id = damaged_player_id;
    es.damage = damage;
    es.duration = 1000;
    es.timestamp.set(es.duration);
    es.float_away = true;
    es.wind_phase_offset = wind_offset_dist(g_rng);

    ephemeral_world_hud_strings.push_back(es);
}

ConsoleCommand2 worldhudaltdmgindicators_cmd{
    "cl_wh_altdmgindicators",
    []() {
        g_alpine_game_config.world_hud_alt_damage_indicators = !g_alpine_game_config.world_hud_alt_damage_indicators;
        rf::console::print("Individual world HUD damage indicators are {}", g_alpine_game_config.world_hud_alt_damage_indicators ? "enabled" : "disabled");
    },
    "Toggle individual vs. cumulative (default) world HUD damage indicator strings",
    "cl_wh_altdmgindicators",
};

ConsoleCommand2 worldhudctf_cmd{
    "cl_wh_ctf",
    []() {
        g_alpine_game_config.world_hud_ctf_icons = !g_alpine_game_config.world_hud_ctf_icons;
        rf::console::print("CTF world HUD is {}", g_alpine_game_config.world_hud_ctf_icons ? "enabled" : "disabled");
    },
    "Toggle drawing of world HUD indicators for CTF flags",
    "cl_wh_ctf",
};

ConsoleCommand2 worldhudoverdraw_cmd{
    "cl_wh_objoverdraw",
    []() {
        g_alpine_game_config.world_hud_overdraw = !g_alpine_game_config.world_hud_overdraw;
        rf::console::print("World HUD overdraw is {}", g_alpine_game_config.world_hud_overdraw ? "enabled" : "disabled");
    },
    "Toggle whether world HUD indicators for objectives are drawn on top of everything else",
    "cl_wh_objoverdraw",
};

ConsoleCommand2 worldhudbigtext_cmd{
    "cl_wh_bigtext",
    []() {
        g_alpine_game_config.world_hud_big_text = !g_alpine_game_config.world_hud_big_text;
        rf::console::print("World HUD big text is {}", g_alpine_game_config.world_hud_big_text ? "enabled" : "disabled");
    },
    "Toggle whether world HUD text labels use big or standard text",
    "cl_wh_bigtext",
};

ConsoleCommand2 worldhuddamagenumbers_cmd{
    "cl_wh_hitnumbers",
    []() {
        g_alpine_game_config.world_hud_damage_numbers = !g_alpine_game_config.world_hud_damage_numbers;
        rf::console::print("World HUD damage indicator numbers are {}", g_alpine_game_config.world_hud_damage_numbers ? "enabled" : "disabled");
    },
    "Toggle whether to display numeric damage indicators when you hit players in multiplayer (if enabled by an Alpine Faction server)",
    "cl_wh_hitnumbers",
};

ConsoleCommand2 worldhudspectateplayerlabels_cmd{
    "spectate_playerlabels",
    []() {
        g_alpine_game_config.world_hud_spectate_player_labels = !g_alpine_game_config.world_hud_spectate_player_labels;
        rf::console::print("World HUD spectate mode player labels are {}", g_alpine_game_config.world_hud_spectate_player_labels ? "enabled" : "disabled");
    },
    "Toggle whether to display player name labels in spectate mode",
    "spectate_playerlabels",
};

ConsoleCommand2 worldhudteamplayerlabels_cmd{
    "cl_wh_teamplayerlabels",
    []() {
        g_alpine_game_config.world_hud_team_player_labels = !g_alpine_game_config.world_hud_team_player_labels;
        rf::console::print("World HUD team player labels are {}", g_alpine_game_config.world_hud_team_player_labels ? "enabled" : "disabled");
    },
    "Toggle whether to display player name labels for your teammates",
    "cl_wh_teamplayerlabels",
};

ConsoleCommand2 worldhudmpspawns_cmd{
    "dbg_wh_mpspawns",
    []() {
        draw_mp_spawn_world_hud = !draw_mp_spawn_world_hud;

        rf::console::print("World HUD multiplayer respawn points are {}", draw_mp_spawn_world_hud ? "enabled" : "disabled");

        if (draw_mp_spawn_world_hud && rf::is_multi && !rf::is_server) {
            rf::console::print("World HUD multiplayer respawn points will only be visible in single player or if you are the server host");
        }
    },
    "Toggle whether world HUD indicators for multiplayer respawn points are drawn",
    "dbg_wh_mpspawns",
};

void hud_world_apply_patch()
{
    // register commands
    worldhudaltdmgindicators_cmd.register_cmd();
    worldhudctf_cmd.register_cmd();
    worldhudoverdraw_cmd.register_cmd();
    worldhudbigtext_cmd.register_cmd();
    worldhuddamagenumbers_cmd.register_cmd();
    worldhudspectateplayerlabels_cmd.register_cmd();
    worldhudteamplayerlabels_cmd.register_cmd();
    worldhudmpspawns_cmd.register_cmd();
}
