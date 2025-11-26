#include <xlog/xlog.h>
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include <patch_common/AsmWriter.h>
#include <algorithm>
#include <unordered_set>
#include "hud_internal.h"
#include "hud_world.h"
#include "multi_spectate.h"
#include "../graphics/gr.h"
#include "../object/event_alpine.h"
#include "../multi/server.h"
#include "../multi/gametype.h"
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
#include "../rf/os/timer.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/localize.h"
#include "../os/console.h"

WorldHUDAssets g_world_hud_assets;
static KothHudTuning g_koth_hud_tuning{};
static std::unordered_map<int, NameLabelTex> g_koth_name_labels;
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
    g_world_hud_assets.koth_neutral = rf::bm::load("af_wh_koth_base_neutral.tga", -1, true);
    g_world_hud_assets.koth_neutral_atk = rf::bm::load("af_wh_koth_atk_neutral.tga", -1, true);
    g_world_hud_assets.koth_neutral_def = rf::bm::load("af_wh_koth_def_neutral.tga", -1, true);
    g_world_hud_assets.koth_atk_red = rf::bm::load("af_wh_koth_atk_red.tga", -1, true);
    g_world_hud_assets.koth_atk_blue = rf::bm::load("af_wh_koth_atk_blue.tga", -1, true);
    g_world_hud_assets.koth_def_red = rf::bm::load("af_wh_koth_def_red.tga", -1, true);
    g_world_hud_assets.koth_def_blue = rf::bm::load("af_wh_koth_def_blue.tga", -1, true);
    g_world_hud_assets.koth_red = rf::bm::load("af_wh_koth_base_red.tga", -1, true);
    g_world_hud_assets.koth_blue = rf::bm::load("af_wh_koth_base_blue.tga", -1, true);
    g_world_hud_assets.koth_neutral_c = rf::bm::load("af_wh_koth_cont_neutral.tga", -1, true);
    g_world_hud_assets.koth_red_c = rf::bm::load("af_wh_koth_cont_red.tga", -1, true);
    g_world_hud_assets.koth_blue_c = rf::bm::load("af_wh_koth_cont_blue.tga", -1, true);
    g_world_hud_assets.koth_neutral_l = rf::bm::load("af_wh_koth_lock_neutral.tga", -1, true);
    g_world_hud_assets.koth_red_l = rf::bm::load("af_wh_koth_lock_red.tga", -1, true);
    g_world_hud_assets.koth_blue_l = rf::bm::load("af_wh_koth_lock_blue.tga", -1, true);
    g_world_hud_assets.koth_fill_red = rf::bm::load("af_wh_koth_fill_red.tga", -1, true);
    g_world_hud_assets.koth_fill_blue = rf::bm::load("af_wh_koth_fill_blue.tga", -1, true);
    g_world_hud_assets.koth_ring_fade = rf::bm::load("af_wh_koth_ring_fade.tga", -1, true);
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
    auto all_respawn_points = get_alpine_respawn_points();

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

static rf::Vector3 koth_hill_icon_pos(const HillInfo& h)
{
    rf::Vector3 p{0.f, 0.f, 0.f};

    // prefer handler position
    if (h.handler) {
        p = h.handler->pos;
    }
    else if (h.trigger) {
        p = h.trigger->pos;
    }
    else if (h.trigger_uid >= 0) {
        if (rf::Object* o = rf::obj_lookup_from_uid(h.trigger_uid)) {
            p = o->pos;
        }
    }

    p.y += WorldHUDRender::koth_hill_offset;
    return p;
}

static float koth_fill_scale_from_progress(uint8_t progress01_100, float base_icon_scale)
{
    // area-linear growth: r ∝ sqrt(p)
    const float t = std::clamp(progress01_100, (uint8_t)0, (uint8_t)100) / 100.0f;
    const float r = std::sqrt(t);
    return base_icon_scale * g_koth_hud_tuning.fill_vs_ring_scale * r;
}

void render_string_3d_pos_new(const rf::Vector3& pos, const std::string& text, int offset_x, int offset_y,
    int font, rf::ubyte r, rf::ubyte g, rf::ubyte b, rf::ubyte a)
{
    rf::gr::Vertex dest;

    // Transform the position to screen space
    if (!rf::gr::rotate_vertex(&dest, &pos))
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

static WorldHUDView make_world_hud_view(rf::Vector3 pos, bool stay_inside_fog = true)
{
    WorldHUDView v{pos, 1.0f};

    rf::Camera* camera = rf::local_player ? rf::local_player->cam : nullptr;
    const rf::Vector3 cam_pos = camera ? rf::camera_get_pos(camera) : rf::Vector3{0, 0, 0};
    float distance = (camera ? pos.distance_to(cam_pos) : WorldHUDRender::reference_distance);

    if (stay_inside_fog) {
        const float fog_far_clip = rf::level.distance_fog_far_clip;
        const float max_distance = (fog_far_clip > WorldHUDRender::fog_dist_min)
                                       ? std::min(fog_far_clip, WorldHUDRender::fog_dist_max)
                                       : WorldHUDRender::fog_dist_max;
        const float limit = max_distance * WorldHUDRender::fog_dist_multi;
        if (distance > limit) {
            rf::Vector3 dir = pos - cam_pos;
            dir.normalize_safe();
            v.pos = cam_pos + (dir * limit);
            distance = limit;
        }
    }

    v.dist_factor = std::max(distance, 1.0f) / WorldHUDRender::reference_distance;
    return v;
}

static rf::gr::Mode bitmap_mode_from(WorldHUDRenderMode render_mode)
{
    switch (render_mode) {
    case WorldHUDRenderMode::no_overdraw_glow:
        return rf::gr::glow_3d_bitmap_mode;
    case WorldHUDRenderMode::overdraw:
        return rf::gr::bitmap_3d_mode;
    default:
        return rf::gr::bitmap_3d_mode_no_z;
    }
}

static inline void koth_owner_color(HillOwner owner, HillLockStatus lock_status, rf::ubyte& r, rf::ubyte& g, rf::ubyte& b, rf::ubyte& a)
{
    bool locked = (lock_status != HillLockStatus::HLS_Available);
    switch (owner) {
    case HillOwner::HO_Red:
        r = 167;
        g = 0;
        b = 0;
        a = 200;
        return;
    case HillOwner::HO_Blue:
        r = 52;
        g = 78;
        b = 167;
        a = 200;
        return;
    default:
        r = locked ? 100 : 200;
        g = locked ? 100 : 200;
        b = locked ? 100 : 200;
        a = locked ? 50 : 200;
        return;
    }
}

static inline int hill_key(const HillInfo& h)
{
    if (h.trigger)
        return h.trigger->uid;
    return h.trigger_uid; // fallback if trigger is broken somehow (should never happen)
}

static inline rf::Vector3 camera_right()
{
    if (auto* cam = rf::local_player ? rf::local_player->cam : nullptr)
        return rf::camera_get_orient(cam).rvec;
    
    return rf::Vector3{1.f, 0.f, 0.f};
}

static inline rf::Vector3 camera_up()
{
    if (auto* cam = rf::local_player ? rf::local_player->cam : nullptr)
        return rf::camera_get_orient(cam).uvec;
    return rf::Vector3{0.f, 1.f, 0.f};
}

static NameLabelTex& ensure_hill_name_tex(const HillInfo& h, int font)
{
    const int key = hill_key(h);
    auto& slot = g_koth_name_labels[key];

    if (slot.bm == -1 || slot.text != h.name || slot.font != font) {
        const auto [tw, th] = rf::gr::get_string_size(h.name, font);

        const int pad = 2;
        const int bw = std::max(1, tw + pad * 2);
        const int bh = std::max(1, th + pad * 2);

        if (slot.bm != -1) {
            rf::bm::release(slot.bm);
            slot.bm = -1;
        }

        slot.bm = rf::bm::create(rf::bm::FORMAT_4444_ARGB, bw, bh);

        // keep resident
        rf::bm::texture_add_ref(slot.bm);

        // clear on GPU path
        rf::bm::clear_user_bitmap(slot.bm);

        // render name text
        rf::gr::set_color(255, 255, 255, 255);
        rf::gr::gr_render_string_into_bitmap(pad, pad, slot.bm, h.name.c_str(), font);

        slot.w_px = bw;
        slot.h_px = bh;
        slot.text = h.name;
        slot.font = font;
    }

    return slot;
}

void clear_koth_name_textures()
{
    for (auto& kv : g_koth_name_labels) {
        if (kv.second.bm != -1)
            rf::bm::release(kv.second.bm);
    }
    g_koth_name_labels.clear();
}

bool hill_vis_contested(HillInfo& h)
{
    const int now = rf::timer_get(1000);
    const int enter = 500;
    const int exit = 200;

    // desired
    const bool desired = (h.steal_dir != HillOwner::HO_Neutral) && (h.capture_milli >= (h.vis_contested ? exit : enter));

    if (desired != h.vis_contested) {
        if (now - h.vis_last_flip_ms >= 120) {
            h.vis_contested = desired;
            h.vis_last_flip_ms = now;
        }
        // ignore this transient flip
    }
    return h.vis_contested;
}

static void render_koth_icon_for_hill(const HillInfo& h, WorldHUDRenderMode rm)
{
    if (!h.trigger)
        return;

    const rf::Vector3 world_pos = koth_hill_icon_pos(h);
    const WorldHUDView view = make_world_hud_view(world_pos, true);

    const float ring_base = g_koth_hud_tuning.icon_base_scale;
    float ring_scale = std::clamp(ring_base * view.dist_factor, WorldHUDRender::min_scale, WorldHUDRender::max_scale);
    const bool contested = hill_vis_contested(const_cast<HillInfo&>(h));
    const bool locked = (h.lock_status != HillLockStatus::HLS_Available);

    int ring_bmp = 0;
    bool esc_show_role_icon = false;

    // neutral base ring
    if (contested) {
        ring_bmp = g_world_hud_assets.koth_neutral_c;
    }
    else if (locked) {
        ring_bmp = g_world_hud_assets.koth_neutral_l;
    }
    else if (gt_is_esc() && !multi_spectate_is_spectating() && rf::local_player) {
        const HillOwner local_team = (rf::local_player->team == 0) ? HillOwner::HO_Red : HillOwner::HO_Blue;
        const HillOwner owner = h.ownership;
        const bool local_can_attack = esc_team_can_attack_hill(h, local_team);
        const bool hill_is_neutral = (h.ownership == HillOwner::HO_Neutral);

        const int neutral_atk_bmp = g_world_hud_assets.koth_neutral_atk;
        const int atk_bmp = (owner == HillOwner::HO_Red)
            ? g_world_hud_assets.koth_atk_red
            : (owner == HillOwner::HO_Blue) ? g_world_hud_assets.koth_atk_blue
            : neutral_atk_bmp;
        const int def_bmp = (owner == HillOwner::HO_Red)
            ? g_world_hud_assets.koth_def_red
            : (owner == HillOwner::HO_Blue) ? g_world_hud_assets.koth_def_blue
            : g_world_hud_assets.koth_neutral_def;

        ring_bmp = hill_is_neutral ? neutral_atk_bmp : (local_can_attack ? atk_bmp : def_bmp);
        esc_show_role_icon = true;
    }
    else if (gt_is_rev() && !multi_spectate_is_spectating()) {
        const bool local_is_red = (rf::local_player && rf::local_player->team == 0);
        ring_bmp = local_is_red ? g_world_hud_assets.koth_neutral_atk : g_world_hud_assets.koth_neutral_def;
    }
    else {
        ring_bmp = g_world_hud_assets.koth_neutral;
    }

    // owned base ring
    if (!esc_show_role_icon) {
        if (h.ownership == HillOwner::HO_Red) {
            ring_bmp = contested ? g_world_hud_assets.koth_red_c
                       : locked  ? g_world_hud_assets.koth_red_l
                                 : g_world_hud_assets.koth_red;
        }
        else if (h.ownership == HillOwner::HO_Blue) {
            ring_bmp = contested ? g_world_hud_assets.koth_blue_c
                       : locked  ? g_world_hud_assets.koth_blue_l
                                 : g_world_hud_assets.koth_blue;
        }
    }

    // capture progress bar
    if (contested) {
        const float t = std::clamp(h.capture_progress, (uint8_t)0, (uint8_t)100) / 100.0f;

        const float track_w = (2.0f * ring_scale) * g_koth_hud_tuning.fill_vs_ring_scale;
        const float bar_h = ring_scale * 0.44f;

        const float cur_w = std::max(track_w * t, 1e-4f);
        if (cur_w > 1e-4f && bar_h > 1e-4f) {
            const rf::Vector3 right = camera_right();
            const rf::Vector3 up = camera_up();

            const float bar_y_offset = -0.7f * ring_scale;
            rf::Vector3 bar_pos = view.pos + up * bar_y_offset + right * (-0.5f * track_w + 0.5f * cur_w);

            const int fill_bmp = (h.steal_dir == HillOwner::HO_Red) ? g_world_hud_assets.koth_fill_red : g_world_hud_assets.koth_fill_blue;
            rf::gr::set_texture(fill_bmp, -1);

            rf::gr::gr_3d_bitmap_angle_wh(&bar_pos, 0.0f, cur_w, bar_h, bitmap_mode_from(rm));
        }
    }

    // hill name label
    const int font = !g_alpine_game_config.world_hud_big_text;
    NameLabelTex& lbl = ensure_hill_name_tex(h, font);

    const float text_h_world = ring_scale * 0.55f;
    const float aspect = (lbl.w_px > 0 && lbl.h_px > 0) ? float(lbl.w_px) / float(lbl.h_px) : 1.0f;
    const float text_w_world = text_h_world * aspect;

    const rf::Vector3 up = camera_up();
    const float margin = ring_scale * -0.4f;
    const rf::Vector3 text_pos = view.pos + up * (ring_scale + margin + 0.5f * text_h_world);

    rf::gr::set_color(255, 255, 255, 255);
    rf::gr::set_texture(lbl.bm, -1);
    rf::gr::gr_3d_bitmap_angle_wh(&const_cast<rf::Vector3&>(text_pos), 0.0f, text_w_world, text_h_world, bitmap_mode_from(rm));

    // icon ring
    rf::gr::set_texture(ring_bmp, -1);
    rf::gr::gr_3d_bitmap_angle(&const_cast<rf::Vector3&>(view.pos), 0.0f, ring_scale, bitmap_mode_from(rm));
}

static void build_koth_hill_icons()
{
    if (!rf::is_multi || !multi_is_game_type_with_hills())
        return;

    const auto gt = rf::multi_get_game_type();
    const bool overdraw_enabled = g_alpine_game_config.world_hud_overdraw;
    const bool is_rev_or_esc = gt == rf::NetGameType::NG_TYPE_REV || gt == rf::NetGameType::NG_TYPE_ESC;

    for (const auto& h : g_koth_info.hills) {
        auto render_mode = WorldHUDRenderMode::no_overdraw;

        if (overdraw_enabled) {
            // REV/ESC: only overdraw for active point
            // other modes: overdraw for all points
            const bool allow_overdraw_for_hill = !is_rev_or_esc || (h.lock_status == HillLockStatus::HLS_Available);

            if (allow_overdraw_for_hill) {
                render_mode = WorldHUDRenderMode::overdraw;
            }
        }

        render_koth_icon_for_hill(h, render_mode);
    }
}

void build_player_labels() {
    bool is_spectating = multi_spectate_is_spectating();
    bool show_all = is_spectating && g_alpine_game_config.world_hud_spectate_player_labels;
    bool is_team_mode = multi_is_team_game_type();
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
        const auto [text_width, text_height] = rf::gr::get_string_size(label, font);
        int half_text_width = text_width / 2;

        render_string_3d_pos_new(string_pos, label.c_str(), -half_text_width, -25, font, 200, 200, 200, 223);
    }
}

void build_ephemeral_world_hud_sprite_icons() {
    std::erase_if(ephemeral_world_hud_sprites, [](const EphemeralWorldHUDSprite& es) {
        return !es.timestamp.valid() || es.timestamp.elapsed();
    });

    for (const auto& es : ephemeral_world_hud_sprites) {
        int font = !g_alpine_game_config.world_hud_big_text;

        if (es.bitmap != -1) {
            do_render_world_hud_sprite(es.pos, 1.0f, es.bitmap, es.render_mode, true, true, true);
        }

        // determine label width
        const auto [text_width, text_height] = rf::gr::get_string_size(es.label, font);
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
        int font = !g_alpine_game_config.world_hud_big_text;
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
        const auto [text_width, text_height] = rf::gr::get_string_size(label, font);
        int half_text_width = text_width / 2;

        render_string_3d_pos_new(string_pos, label.c_str(), -half_text_width, -25,
            font, 255, 255, 255, 223);
    }
}

static inline void make_onb_edge_with_up(const rf::Vector3& dir_norm, const rf::Vector3& up_exact, rf::Matrix3& M)
{
    rf::Vector3 r = dir_norm;
    rf::Vector3 u = up_exact;
    u.normalize_safe();

    u = u - r * r.dot_prod(u);
    u.normalize_safe();

    rf::Vector3 f = r.cross(u);
    f.normalize_safe();

    // avoid drift
    u = f.cross(r);
    u.normalize_safe();

    M.rvec = r;
    M.uvec = u;
    M.fvec = f;
}

static inline void face_camera(const rf::Vector3& cam_pos, const rf::Vector3& quad_pos, rf::Matrix3& M)
{
    rf::Vector3 view = cam_pos - quad_pos;
    view.normalize_safe();
    if (M.fvec.dot_prod(view) < 0.0f) {
        M.rvec *= -1.0f;
        M.fvec *= -1.0f;
    }
}

static void draw_edge_oriented_single_bottom(const rf::Vector3& a, const rf::Vector3& b, float thickness, const rf::Vector3& up_exact, WorldHUDRenderMode mode)
{
    rf::Vector3 x = b - a;
    const float len = x.len();
    if (len <= 1e-4f || thickness <= 1e-5f)
        return;

    x *= (1.0f / len);

    rf::Camera* cam = rf::local_player ? rf::local_player->cam : nullptr;
    const rf::Vector3 cam_pos = cam ? rf::camera_get_pos(cam) : rf::Vector3{0, 0, 0};

    rf::Matrix3 M{};
    make_onb_edge_with_up(x, up_exact, M);

    const float height_scale = std::max(0.0f, g_alpine_game_config.control_point_outline_height_scale);
    const float half_h = 0.5f * thickness * height_scale;
    const float half_w = 0.5f * len;

    // align quad with bottom edge
    rf::Vector3 center = (a + b) * 0.5f + M.uvec * half_h;

    face_camera(cam_pos, center, M);

    rf::gr::set_texture(g_world_hud_assets.koth_ring_fade, -1);
    gr_3d_bitmap_oriented_wh(&center, &M, half_w, half_h, bitmap_mode_from(mode));
}

static void draw_box_trigger_bottom_outline_colored(const rf::Trigger* t, float thickness, WorldHUDRenderMode mode,
    rf::ubyte r, rf::ubyte g, rf::ubyte b, rf::ubyte a, float outline_offset)
{
    if (!t)
        return;

    const rf::Vector3 c = t->pos;
    const auto& o = t->orient;
    const rf::Vector3 he = t->box_size * 0.5f;

    const rf::Vector3 base00 = c + o.rvec * (-he.x) + o.uvec * (-he.y) + o.fvec * (-he.z);
    const rf::Vector3 base01 = c + o.rvec * (-he.x) + o.uvec * (-he.y) + o.fvec * (+he.z);
    const rf::Vector3 base11 = c + o.rvec * (+he.x) + o.uvec * (-he.y) + o.fvec * (+he.z);
    const rf::Vector3 base10 = c + o.rvec * (+he.x) + o.uvec * (-he.y) + o.fvec * (-he.z);

    const rf::Vector3 offset = o.uvec * outline_offset;

    const rf::Vector3 p00 = base00 + offset;
    const rf::Vector3 p01 = base01 + offset;
    const rf::Vector3 p11 = base11 + offset;
    const rf::Vector3 p10 = base10 + offset;

    rf::gr::set_color(r, g, b, a);
    const rf::Vector3 up_exact = o.uvec;

    draw_edge_oriented_single_bottom(p00, p01, thickness, up_exact, mode);
    draw_edge_oriented_single_bottom(p01, p11, thickness, up_exact, mode);
    draw_edge_oriented_single_bottom(p11, p10, thickness, up_exact, mode);
    draw_edge_oriented_single_bottom(p10, p00, thickness, up_exact, mode);
}

static void draw_ring_outline_on_plane_colored(const rf::Vector3& center_on_plane, const rf::Vector3& axis_u_in, float radius, float thickness,
    WorldHUDRenderMode mode, rf::ubyte r, rf::ubyte g, rf::ubyte b, rf::ubyte a, float lift_along_u)
{
    if (radius <= 0.f || thickness <= 0.f)
        return;

    rf::gr::set_color(r, g, b, a);
    rf::gr::set_texture(g_world_hud_assets.koth_ring_fade, -1);

    rf::Vector3 axis_u = axis_u_in;
    axis_u.normalize_safe();

    // Orthonormal basis on the plane of the ring
    rf::Vector3 r0 = std::fabs(axis_u.dot_prod({0, 1, 0})) < 0.95f ? rf::Vector3{0, 1, 0}.cross(axis_u) : rf::Vector3{1, 0, 0}.cross(axis_u);
    r0.normalize_safe();
    rf::Vector3 f0 = axis_u.cross(r0);
    f0.normalize_safe();

    // Use configured segment count
    int segs = g_alpine_game_config.control_point_outline_segments;

    const float dth = (2.0f * 3.14159265f) / float(segs);
    const float th0 = 0.5f * dth;

    auto circle_pt = [&](float th) {
        return center_on_plane + r0 * (radius * std::cos(th)) + f0 * (radius * std::sin(th));
    };

    // Height scaling
    const float height_scale = std::max(0.0f, g_alpine_game_config.control_point_outline_height_scale);
    const float half_h = 0.5f * thickness * height_scale;
    if (half_h <= 1e-5f)
        return;

    rf::Vector3 prev = circle_pt(th0);

    for (int i = 1; i <= segs; ++i) {
        const float th = th0 + i * dth;
        rf::Vector3 cur = circle_pt(th);

        rf::Matrix3 M{};
        M.rvec = (cur - prev);
        const float chord_len = M.rvec.len();
        if (chord_len > 1e-5f) {
            M.rvec *= (1.0f / chord_len);
            M.uvec = axis_u;
            M.fvec = M.rvec.cross(M.uvec);
            M.fvec.normalize_safe();
            M.uvec = M.fvec.cross(M.rvec);
            M.uvec.normalize_safe();

            const float half_w = 0.5f * chord_len; // width along the ring segment
            const rf::Vector3 mid = (prev + cur) * 0.5f;

            // place on the plane + any extra lift, then raise by half_h so bottom sits on plane
            rf::Vector3 center = mid + M.uvec * (lift_along_u + half_h);

            if (rf::Camera* cam = rf::local_player ? rf::local_player->cam : nullptr) {
                const rf::Vector3 cam_pos = rf::camera_get_pos(cam);
                face_camera(cam_pos, center, M);
            }

            gr_3d_bitmap_oriented_wh(&center, &M, half_w, half_h, bitmap_mode_from(mode));
        }

        prev = cur;
    }
}

static inline void draw_sphere_ring_outline(const rf::Trigger* t, float thickness, WorldHUDRenderMode mode,
    rf::ubyte r, rf::ubyte g, rf::ubyte b, rf::ubyte a, float outline_offset)
{
    if (!t || t->radius <= 0.f)
        return;
    const rf::Vector3 center_on_plane = t->pos;
    const rf::Vector3 axis_u = {0.f, 1.f, 0.f};
    draw_ring_outline_on_plane_colored(center_on_plane, axis_u, t->radius, thickness, mode, r, g, b, a, outline_offset);
}

static inline void draw_sphere_as_cylinder_base_outline(const rf::Trigger* t, float thickness, WorldHUDRenderMode mode,
    rf::ubyte r, rf::ubyte g, rf::ubyte b, rf::ubyte a, float outline_offset, bool use_trigger_up)
{
    if (!t || t->radius <= 0.f)
        return;
    rf::Vector3 axis = use_trigger_up ? t->orient.uvec : rf::Vector3{0.f, 1.f, 0.f};
    axis.normalize_safe();
    const rf::Vector3 base_center = t->pos - axis * t->radius;
    draw_ring_outline_on_plane_colored(base_center, axis, t->radius, thickness, mode, r, g, b, a, outline_offset);
}

static inline float cylinder_radius_for_box_inscribed(const rf::Trigger* t)
{
    const float hx = 0.5f * std::fabs(t->box_size.x);
    const float hz = 0.5f * std::fabs(t->box_size.z);
    return std::min(hx, hz);
}

static inline void draw_box_as_cylinder_base_outline(const rf::Trigger* t, float thickness, WorldHUDRenderMode mode,
    rf::ubyte r, rf::ubyte g, rf::ubyte b, rf::ubyte a, float outline_offset)
{
    if (!t)
        return;
    const float radius = cylinder_radius_for_box_inscribed(t);
    const rf::Vector3 u = t->orient.uvec;
    const rf::Vector3 he = t->box_size * 0.5f;
    const rf::Vector3 base_center = t->pos + u * (-he.y);
    draw_ring_outline_on_plane_colored(base_center, u, radius, thickness, mode, r, g, b, a, outline_offset);
}

// Use trigger to determine base center
static inline bool koth_base_and_axis_for_hill(const HillInfo& h, const rf::Trigger* t, rf::Vector3& base_center, rf::Vector3& axis_u, float offset)
{
    if (!t)
        return false;

    // determine axis
    axis_u = g_koth_info.rules.cyl_use_trigger_up ? t->orient.uvec : rf::Vector3{0.f, 1.f, 0.f};
    axis_u.normalize_safe();
    if (axis_u.len() <= 1e-5f)
        axis_u = {0.f, 1.f, 0.f};

    // decide base center from trigger type
    if (t->type == 1) { // box: bottom face center
        const rf::Vector3 he = t->box_size * 0.5f;
        base_center = t->pos + t->orient.uvec * (-he.y);
    }
    else { // sphere: center plane
        base_center = t->pos - axis_u * t->radius;
    }

    base_center += axis_u * offset;

    return true;
}

// Use handler position to determine column height above base plane
static inline float koth_column_height_for_hill(const HillInfo& h, const rf::Vector3& base_center, const rf::Vector3& axis_u)
{
    if (h.handler) {
        const rf::Vector3 delta = h.handler->pos - base_center;
        const float along = (delta.dot_prod(axis_u) / 3.0f) * g_alpine_game_config.control_point_column_height_scale;
        return std::max(along, 0.0f);
    }
    
    return 0.0f; // should never happen
}

// Draw light column at center of control point
static void draw_ring_light_column_colored(const rf::Vector3& base_center, const rf::Vector3& axis_u_in,
    float radius, float height, WorldHUDRenderMode mode, rf::ubyte r, rf::ubyte g, rf::ubyte b, rf::ubyte a)
{
    if (radius <= 1e-6f || height <= 1e-6f)
        return;

    rf::Vector3 axis_u = axis_u_in;
    axis_u.normalize_safe();

    rf::gr::set_color(r, g, b, a);
    rf::gr::set_texture(g_world_hud_assets.koth_ring_fade, -1);

    // orthonormal basis on the ring plane
    rf::Vector3 r0 = std::fabs(axis_u.dot_prod({0, 1, 0})) < 0.95f ? rf::Vector3{0, 1, 0}.cross(axis_u) : rf::Vector3{1, 0, 0}.cross(axis_u);
    r0.normalize_safe();
    rf::Vector3 f0 = axis_u.cross(r0);
    f0.normalize_safe();

    const int segs = g_alpine_game_config.control_point_column_segments;
    const float dth = (2.0f * 3.14159265f) / float(segs);
    const float th0 = 0.5f * dth;

    auto circle_pt = [&](float th) {
        return base_center + r0 * (radius * std::cos(th)) + f0 * (radius * std::sin(th));
    };

    rf::Vector3 prev = circle_pt(th0);

    const float half_h = 0.5f * height; // center at half height
    const float lift = 0.0f; // start at base

    for (int i = 1; i <= segs; ++i) {
        const float th = th0 + i * dth;
        rf::Vector3 cur = circle_pt(th);

        rf::Matrix3 M{};
        M.rvec = (cur - prev);
        const float chord_len = M.rvec.len();
        if (chord_len > 1e-5f) {
            M.rvec *= (1.0f / chord_len);
            M.uvec = axis_u;
            M.fvec = M.rvec.cross(M.uvec);
            M.fvec.normalize_safe();
            M.uvec = M.fvec.cross(M.rvec);
            M.uvec.normalize_safe();

            const float half_w = 0.5f * chord_len;
            const rf::Vector3 mid = (prev + cur) * 0.5f;

            rf::Vector3 center = mid + M.uvec * (lift + half_h);

            if (rf::Camera* cam = rf::local_player ? rf::local_player->cam : nullptr) {
                const rf::Vector3 cam_pos = rf::camera_get_pos(cam);
                face_camera(cam_pos, center, M);
            }

            // gradient fades vertically
            gr_3d_bitmap_oriented_wh(&center, &M, half_w, half_h, bitmap_mode_from(mode));
        }

        prev = cur;
    }
}

static void build_koth_hill_outlines()
{
    if (!rf::is_multi || !multi_is_game_type_with_hills())
        return;

    const auto mode = WorldHUDRenderMode::no_overdraw_glow;

    for (const auto& h : g_koth_info.hills) {
        rf::Trigger* trig = h.trigger ? h.trigger : koth_resolve_trigger_from_uid(h.trigger_uid);
        if (!trig)
            continue;

        rf::ubyte r, g, b, a;
        koth_owner_color(h.ownership, h.lock_status, r, g, b, a);

        // outline
        if (trig->type == 0 && h.handler && h.handler->sphere_to_cylinder) {
            // sphere to cylinder with base ring
            draw_sphere_as_cylinder_base_outline(trig, 0.08f, mode, r, g, b, a, h.outline_offset, g_koth_info.rules.cyl_use_trigger_up);
        }
        else if (trig->type == 1 && h.handler && h.handler->sphere_to_cylinder) {
            // box to cylinder with base ring
            draw_box_as_cylinder_base_outline(trig, 0.08f, mode, r, g, b, a, h.outline_offset);
        }
        else if (trig->type == 1) {
            // rectangular outline for box
            draw_box_trigger_bottom_outline_colored(trig, 0.08f, mode, r, g, b, a, h.outline_offset);
        }
        else {
            // sphere ring (mid plane)
            draw_sphere_ring_outline(trig, 0.08f, mode, r, g, b, a, h.outline_offset);
        }

        // light column
        rf::Vector3 base_center{}, axis_u{};
        if (koth_base_and_axis_for_hill(h, trig, base_center, axis_u, h.outline_offset)) {
            const float cheight = koth_column_height_for_hill(h, base_center, axis_u);
            const float cradius = 1.0f; // maybe make configurable on handler event?
            if (cheight > 0.02f) {
                draw_ring_light_column_colored(base_center, axis_u, cradius, cheight, mode, r, g, b, a);
            }
        }
    }
}

void hud_world_do_frame() {
    if (rf::is_multi && g_alpine_game_config.world_hud_ctf_icons && rf::multi_get_game_type() == rf::NetGameType::NG_TYPE_CTF) {
        build_ctf_flag_icons();
    }
    if (rf::is_multi && multi_is_game_type_with_hills()) {
        build_koth_hill_icons();
        build_koth_hill_outlines();
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
    (!multi_spectate_is_spectating() && g_alpine_game_config.world_hud_team_player_labels && multi_is_team_game_type()))) {
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
    if (!g_alpine_game_config.show_location_pings) {
        return;
    }

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

ConsoleCommand2 set_cp_outline_height_cmd{
    "cl_outlineheightscale",
    [](std::optional<float> new_height) {
        if (new_height) {
            g_alpine_game_config.set_control_point_outline_height_scale(new_height.value());
        }
        rf::console::print("Control point outline height scale is {:.2f}.", g_alpine_game_config.control_point_outline_height_scale);
    },
    "Set control point outline height scale",
    "cl_outlineheightscale <scale>",
};

ConsoleCommand2 set_cp_outline_segments_cmd{
    "cl_outlinesegments",
    [](std::optional<int> new_segments) {
        if (new_segments) {
            g_alpine_game_config.set_control_point_outline_segments(new_segments.value());
        }
        rf::console::print("Control point outline ring segments is set to {}.", g_alpine_game_config.control_point_outline_segments);
    },
    "Set number of segments for control point outline rings",
    "cl_outlinesegments <segments>",
};

ConsoleCommand2 set_cp_column_segments_cmd{
    "cl_columnsegments",
    [](std::optional<int> new_segments) {
        if (new_segments) {
            g_alpine_game_config.set_control_point_column_segments(new_segments.value());
        }
        rf::console::print("Control point column ring segments is set to {}.", g_alpine_game_config.control_point_column_segments);
    },
    "Set number of segments for control point light columns",
    "cl_columnsegments <segments>",
};

ConsoleCommand2 set_cp_column_height_scale_cmd{
    "cl_columnheightscale",
    [](std::optional<float> new_height) {
        if (new_height) {
            g_alpine_game_config.set_control_point_column_height_scale(new_height.value());
        }
        rf::console::print("Control point light column height scale is {:.2f}.", g_alpine_game_config.control_point_column_height_scale);
    },
    "Set control point light column height scale",
    "cl_columnheightscale <scale>",
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
    set_cp_outline_height_cmd.register_cmd();
    set_cp_outline_segments_cmd.register_cmd();
    set_cp_column_segments_cmd.register_cmd();
    set_cp_column_height_scale_cmd.register_cmd();
}
