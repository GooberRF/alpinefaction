#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <string>
#include <vector>
#include <xlog/xlog.h>
#include <common/utils/list-utils.h>
#include "minimap.h"
#include "hud.h"
#include "hud_internal.h"
#include "hud_world.h"
#include "multi_scoreboard.h"
#include "multi_spectate.h"
#include "remote_server_cfg_ui.h"
#include "../graphics/gr.h"
#include "../input/input.h"
#include "../misc/alpine_settings.h"
#include "../misc/level.h"
#include "../misc/vote_panel.h"
#include "../multi/alpine_packets.h"
#include "../multi/bagman.h"
#include "../multi/gametype.h"
#include "../multi/jetpack.h"
#include "../multi/multi.h"
#include "../multi/salvage.h"
#include "../multi/vehicles/vehicle.h"
#include "../multi/vehicles/vehicle_markers.h"
#include "../object/event_alpine.h"
#include "../os/console.h"
#include "../os/os.h"
#include "../rf/entity.h"
#include "../rf/file/file.h"
#include "../rf/gameseq.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/hud.h"
#include "../rf/multi.h"
#include "../rf/player/camera.h"
#include "../rf/player/player.h"
#include "../rf/trigger.h"

namespace
{
    struct Crater
    {
        float x;
        float z;
        float radius;
    };

    struct MinimapState
    {
        int bitmap = -1;
        std::vector<Crater> craters;
    };
    MinimapState g_minimap;

    constexpr std::size_t max_craters = 1024;

    constexpr rf::gr::Mode image_mode{
        rf::gr::TEXTURE_SOURCE_CLAMP,
        rf::gr::COLOR_SOURCE_VERTEX_TIMES_TEXTURE,
        rf::gr::ALPHA_SOURCE_VERTEX_TIMES_TEXTURE,
        rf::gr::ALPHA_BLEND_ALPHA,
        rf::gr::ZBUFFER_TYPE_NONE,
        rf::gr::FOG_NOT_ALLOWED,
    };

    constexpr rf::gr::Mode fill_mode{
        rf::gr::TEXTURE_SOURCE_NONE,
        rf::gr::COLOR_SOURCE_VERTEX,
        rf::gr::ALPHA_SOURCE_VERTEX,
        rf::gr::ALPHA_BLEND_ALPHA,
        rf::gr::ZBUFFER_TYPE_NONE,
        rf::gr::FOG_NOT_ALLOWED,
    };

    struct ClipVert
    {
        float x;
        float y;
        float u;
        float v;
    };
    constexpr int max_clip_verts = 40;

    struct Panel
    {
        float x0;
        float y0;
        float x1;
        float y1;
        float cx;
        float cy;
    };

    struct View
    {
        Panel panel;
        float center_x;
        float center_z;
        float ppu;
        float yaw;
        float cos_yaw;
        float sin_yaw;
        float scale;
        rf::ubyte image_alpha;
        bool labels;

        void to_screen(float wx, float wz, float& sx, float& sy) const
        {
            const float dx = wx - center_x;
            const float dz = wz - center_z;
            sx = panel.cx + (dx * cos_yaw - dz * sin_yaw) * ppu;
            sy = panel.cy - (dx * sin_yaw + dz * cos_yaw) * ppu;
        }

        bool contains(float sx, float sy, float margin) const
        {
            return sx >= panel.x0 - margin && sx <= panel.x1 + margin && sy >= panel.y0 - margin &&
                   sy <= panel.y1 + margin;
        }
    };

    struct Viewer
    {
        rf::Player* player;
        rf::Entity* entity;
        int team;
        bool team_mode;
    };

    int clip_edge(const ClipVert* in, int n, ClipVert* out, bool y_axis, float bound, bool keep_greater)
    {
        auto coord = [y_axis](const ClipVert& p) { return y_axis ? p.y : p.x; };
        auto inside = [&](const ClipVert& p) { return keep_greater ? coord(p) >= bound : coord(p) <= bound; };
        int m = 0;
        for (int i = 0; i < n; ++i) {
            const ClipVert& a = in[i];
            const ClipVert& b = in[(i + 1) % n];
            const bool a_in = inside(a);
            if (a_in && m < max_clip_verts) {
                out[m++] = a;
            }
            if (a_in != inside(b) && m < max_clip_verts) {
                const float t = (bound - coord(a)) / (coord(b) - coord(a));
                out[m++] = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.u + (b.u - a.u) * t,
                            a.v + (b.v - a.v) * t};
            }
        }
        return m;
    }

    void submit_poly(int bmh, const ClipVert* poly, int count, rf::gr::Mode mode)
    {
        static std::array<rf::gr::Vertex, max_clip_verts> verts;
        for (int i = 0; i < count; ++i) {
            rf::gr::Vertex& v = verts[i];
            v = rf::gr::Vertex{};
            v.sx = poly[i].x;
            v.sy = poly[i].y;
            v.u1 = poly[i].u;
            v.v1 = poly[i].v;
        }
        gr_poly_2d(bmh, count, verts.data(), mode);
    }

    void draw_clipped(int bmh, const ClipVert* poly, int n, const Panel& p, rf::gr::Mode mode)
    {
        const int count = std::min(n, max_clip_verts);
        if (count < 3) {
            return;
        }
        float min_x = poly[0].x;
        float max_x = poly[0].x;
        float min_y = poly[0].y;
        float max_y = poly[0].y;
        for (int i = 1; i < count; ++i) {
            min_x = std::min(min_x, poly[i].x);
            max_x = std::max(max_x, poly[i].x);
            min_y = std::min(min_y, poly[i].y);
            max_y = std::max(max_y, poly[i].y);
        }
        if (min_x >= p.x0 && max_x <= p.x1 && min_y >= p.y0 && max_y <= p.y1) {
            submit_poly(bmh, poly, count, mode);
            return;
        }
        if (max_x < p.x0 || min_x > p.x1 || max_y < p.y0 || min_y > p.y1) {
            return;
        }
        std::array<ClipVert, max_clip_verts> a;
        std::array<ClipVert, max_clip_verts> b;
        int m = clip_edge(poly, count, a.data(), false, p.x0, true);
        m = clip_edge(a.data(), m, b.data(), false, p.x1, false);
        m = clip_edge(b.data(), m, a.data(), true, p.y0, true);
        m = clip_edge(a.data(), m, b.data(), true, p.y1, false);
        if (m >= 3) {
            submit_poly(bmh, b.data(), m, mode);
        }
    }

    template<int N>
    const std::array<ClipVert, N>& unit_circle()
    {
        static const std::array<ClipVert, N> table = [] {
            std::array<ClipVert, N> t{};
            for (int i = 0; i < N; ++i) {
                const float a = 2.0f * std::numbers::pi_v<float> * i / N;
                t[i] = {std::cos(a), std::sin(a), 0.0f, 0.0f};
            }
            return t;
        }();
        return table;
    }

    template<int N>
    void fill_disc(const View& view, float sx, float sy, float r, const rf::Color& color)
    {
        static_assert(N >= 3 && N <= max_clip_verts - 4);
        const std::array<ClipVert, N>& unit = unit_circle<N>();
        ClipVert poly[N];
        for (int i = 0; i < N; ++i) {
            poly[i] = {sx + r * unit[i].x, sy + r * unit[i].y, 0.0f, 0.0f};
        }
        rf::gr::set_color(color);
        draw_clipped(-1, poly, N, view.panel, fill_mode);
    }

    void fill_square(const View& view, float sx, float sy, float half, const rf::Color& color)
    {
        const ClipVert poly[4] = {
            {sx - half, sy - half, 0.0f, 0.0f},
            {sx + half, sy - half, 0.0f, 0.0f},
            {sx + half, sy + half, 0.0f, 0.0f},
            {sx - half, sy + half, 0.0f, 0.0f},
        };
        rf::gr::set_color(color);
        draw_clipped(-1, poly, 4, view.panel, fill_mode);
    }

    void fill_diamond(const View& view, float sx, float sy, float half, const rf::Color& color)
    {
        const ClipVert poly[4] = {
            {sx, sy - half, 0.0f, 0.0f},
            {sx + half, sy, 0.0f, 0.0f},
            {sx, sy + half, 0.0f, 0.0f},
            {sx - half, sy, 0.0f, 0.0f},
        };
        rf::gr::set_color(color);
        draw_clipped(-1, poly, 4, view.panel, fill_mode);
    }

    rf::Color color_from_packed(uint32_t packed, rf::ubyte alpha = 255)
    {
        const auto [r, g, b, a] = extract_color_components(packed);
        return {static_cast<rf::ubyte>(r), static_cast<rf::ubyte>(g), static_cast<rf::ubyte>(b), alpha};
    }

    rf::Color teammate_color(const Viewer& viewer)
    {
        const auto& override_color = g_alpine_game_config.outlines_color_team;
        return override_color ? color_from_packed(*override_color) : hud_team_color(viewer.team);
    }

    const rf::Color outline_color{0, 0, 0, 200};

    void draw_objective(const View& view, const rf::Vector3& pos, const rf::Color& color)
    {
        const float half = 4.0f * view.scale;
        const float edge = half + view.scale;
        float sx, sy;
        view.to_screen(pos.x, pos.z, sx, sy);
        sx = std::clamp(sx, view.panel.x0 + edge, view.panel.x1 - edge);
        sy = std::clamp(sy, view.panel.y0 + edge, view.panel.y1 - edge);
        fill_square(view, sx, sy, edge, outline_color);
        fill_square(view, sx, sy, half, color);
    }

    void draw_player_dot(const View& view, const rf::Vector3& pos, const rf::Color& color)
    {
        const float r = 3.0f * view.scale;
        float sx, sy;
        view.to_screen(pos.x, pos.z, sx, sy);
        if (!view.contains(sx, sy, 0.0f)) {
            return;
        }
        fill_disc<10>(view, sx, sy, r + view.scale, outline_color);
        fill_disc<10>(view, sx, sy, r, color);
    }

    // Enemies never appear on the minimap, visible or not.
    bool is_enemy(const Viewer& viewer, const rf::Player* player)
    {
        return player && player != viewer.player && (!viewer.team_mode || player->team != viewer.team);
    }

    void draw_image(const View& view, const AlpineLevelProperties& props)
    {
        if (g_minimap.bitmap < 0) {
            return;
        }
        const rf::Vector3& mn = props.minimap_world_min;
        const rf::Vector3& mx = props.minimap_world_max;
        const float corners[4][4] = {
            {mn.x, mx.z, 0.0f, 0.0f},
            {mx.x, mx.z, 1.0f, 0.0f},
            {mx.x, mn.z, 1.0f, 1.0f},
            {mn.x, mn.z, 0.0f, 1.0f},
        };
        ClipVert poly[4];
        for (int i = 0; i < 4; ++i) {
            view.to_screen(corners[i][0], corners[i][1], poly[i].x, poly[i].y);
            poly[i].u = corners[i][2];
            poly[i].v = corners[i][3];
        }
        rf::gr::set_color(255, 255, 255, view.image_alpha);
        draw_clipped(g_minimap.bitmap, poly, 4, view.panel, image_mode);
    }

    void draw_craters(const View& view)
    {
        const rf::Color color{0, 0, 0, 120};
        for (const Crater& c : g_minimap.craters) {
            const float r = c.radius * view.ppu;
            float sx, sy;
            view.to_screen(c.x, c.z, sx, sy);
            if (r < 0.5f || !view.contains(sx, sy, r)) {
                continue;
            }
            fill_disc<16>(view, sx, sy, r, color);
        }
    }

    rf::Color hill_color(HillOwner owner, rf::ubyte alpha)
    {
        switch (owner) {
        case HillOwner::HO_Red:
            return hud_team_color(rf::TEAM_RED, alpha);
        case HillOwner::HO_Blue:
            return hud_team_color(rf::TEAM_BLUE, alpha);
        default:
            return {200, 200, 200, alpha};
        }
    }

    // The capture area as the hill tests it: a box trigger is a cylinder only when its handler says so.
    void draw_hill_area(const View& view, const HillInfo& h, const rf::Color& color)
    {
        const rf::Trigger* t = h.trigger;
        if (!t) {
            return;
        }
        const float min_px = 4.0f * view.scale;
        if (t->type == 1 && !(h.handler && h.handler->sphere_to_cylinder)) {
            const float hx = 0.5f * std::fabs(t->box_size.x);
            const float hz = 0.5f * std::fabs(t->box_size.z);
            if (std::max(hx, hz) * view.ppu < min_px) {
                return;
            }
            const rf::Vector3& r = t->orient.rvec;
            const rf::Vector3& f = t->orient.fvec;
            const float signs[4][2] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};
            ClipVert poly[4];
            for (int i = 0; i < 4; ++i) {
                const float wx = t->pos.x + signs[i][0] * hx * r.x + signs[i][1] * hz * f.x;
                const float wz = t->pos.z + signs[i][0] * hx * r.z + signs[i][1] * hz * f.z;
                view.to_screen(wx, wz, poly[i].x, poly[i].y);
                poly[i].u = 0.0f;
                poly[i].v = 0.0f;
            }
            rf::gr::set_color(color);
            draw_clipped(-1, poly, 4, view.panel, fill_mode);
            return;
        }
        const float radius = t->type == 1 ? koth_box_cylinder_radius(t) : t->radius;
        const float r = radius * view.ppu;
        float sx, sy;
        view.to_screen(t->pos.x, t->pos.z, sx, sy);
        if (r >= min_px && view.contains(sx, sy, r)) {
            fill_disc<20>(view, sx, sy, r, color);
        }
    }

    void draw_hills(const View& view)
    {
        if (!multi_is_game_type_with_hills()) {
            return;
        }
        for (const HillInfo& h : g_koth_info.hills) {
            if (!h.trigger && !h.handler && h.trigger_uid < 0) {
                continue;
            }
            draw_hill_area(view, h, hill_color(h.ownership, 90));
            draw_objective(view, koth_hill_icon_pos(h), hill_color(h.ownership, 255));
        }
    }

    void draw_ctf_flags(const View& view, const Viewer& viewer)
    {
        if (rf::multi_get_game_type() != rf::NetGameType::NG_TYPE_CTF || !rf::ctf_red_flag_item ||
            !rf::ctf_blue_flag_item) {
            return;
        }
        for (const bool blue : {false, true}) {
            const int team = blue ? rf::TEAM_BLUE : rf::TEAM_RED;
            const rf::Vector3& home = blue ? rf::ctf_blue_flag_pos : rf::ctf_red_flag_pos;
            float hx, hy;
            view.to_screen(home.x, home.z, hx, hy);
            if (view.contains(hx, hy, 0.0f)) {
                fill_disc<12>(view, hx, hy, 5.0f * view.scale, hud_team_color(team, 110));
            }

            rf::Player* carrier = blue ? rf::multi_ctf_get_blue_flag_player() : rf::multi_ctf_get_red_flag_player();
            const bool in_base = blue ? rf::multi_ctf_is_blue_flag_in_base() : rf::multi_ctf_is_red_flag_in_base();
            rf::Vector3 pos = home;
            if (carrier) {
                rf::Entity* carrier_ep = rf::entity_from_handle(carrier->entity_handle);
                if (!carrier_ep || is_enemy(viewer, carrier)) {
                    continue;
                }
                pos = carrier_ep->pos;
            }
            else if (!in_base) {
                pos = (blue ? rf::ctf_blue_flag_item : rf::ctf_red_flag_item)->pos;
            }
            draw_objective(view, pos, hud_team_color(team));
        }
    }

    void draw_bag(const View& view)
    {
        if (!gt_is_bagman_any() || g_bagman_info.state == BagState::BS_Delayed) {
            return;
        }
        rf::Vector3 pos{};
        if (g_bagman_info.state == BagState::BS_Carried) {
            rf::Entity* ep = g_bagman_info.carrier ? rf::entity_from_handle(g_bagman_info.carrier->entity_handle) : nullptr;
            if (!ep) {
                return;
            }
            pos = ep->pos;
        }
        else if (!bagman_get_client_pickup_pos(&pos)) {
            return;
        }
        draw_objective(view, pos, hud_amber_color);
    }

    void draw_salvage_flag(const View& view)
    {
        if (!gt_is_salvage()) {
            return;
        }
        const SalFlagState state = salvage_get_state();
        rf::Vector3 pos{};
        if (state == SalFlagState::Carried) {
            rf::Player* carrier = salvage_get_carrier();
            rf::Entity* ep = carrier ? rf::entity_from_handle(carrier->entity_handle) : nullptr;
            if (!ep) {
                return;
            }
            pos = ep->pos;
        }
        else if (state == SalFlagState::AtSpawn || state == SalFlagState::Dropped) {
            if (!salvage_get_client_flag_pos(&pos)) {
                return;
            }
        }
        else {
            return;
        }
        draw_objective(view, pos, {0, 255, 0, 255});
    }

    template<typename F>
    void for_each_factory(F&& fn)
    {
        if (!vehicle_level_has_factories()) {
            return;
        }
        for (int i = 0; i < vehicle_factory_ui_count(); ++i) {
            const AlpineVehicleFactoryInfo* info = vehicle_factory(i);
            const VehicleFactoryUi* ui = vehicle_factory_ui(i);
            if (info && ui) {
                fn(*info, *ui);
            }
        }
    }

    void draw_vehicles(const View& view, const Viewer& viewer)
    {
        const float half = 4.0f * view.scale;
        for_each_factory([&](const AlpineVehicleFactoryInfo& info, const VehicleFactoryUi& ui) {
            if (ui.state == AF_VEHICLE_FACTORY_GIVEN_UP) {
                return;
            }
            float sx, sy;
            view.to_screen(info.pos.x, info.pos.z, sx, sy);
            if (!view.contains(sx, sy, 0.0f)) {
                return;
            }
            fill_square(view, sx, sy, half + view.scale, hud_team_color(info.team, 200));
            fill_square(view, sx, sy, half - view.scale, {0, 0, 0, 200});
        });

        if (!vehicle_level_has_factories()) {
            return;
        }
        rf::Entity* viewer_hull = viewer.entity ? vehicle_ridden_hull(viewer.entity) : nullptr;
        for (rf::Entity& hull : DoublyLinkedList{rf::entity_list}) {
            if (!vehicle_is_synced_entity_type(&hull) || rf::entity_is_dying(&hull) || &hull == viewer_hull) {
                continue;
            }
            if (vehicle_occupied_by_enemy(viewer.player, &hull)) {
                continue;
            }
            float sx, sy;
            view.to_screen(hull.pos.x, hull.pos.z, sx, sy);
            if (!view.contains(sx, sy, 0.0f)) {
                continue;
            }
            fill_diamond(view, sx, sy, half + 2.0f * view.scale, outline_color);
            fill_diamond(view, sx, sy, half + view.scale, hud_team_color(vehicle_hull_team(hull.handle)));
            if (vehicle_outline_occupant(&hull)) {
                fill_diamond(view, sx, sy, 0.4f * half, outline_color);
            }
        }
    }

    void draw_players(const View& view, const Viewer& viewer)
    {
        for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
            if (&player == viewer.player || is_enemy(viewer, &player)) {
                continue;
            }
            rf::Entity* ep = rf::entity_from_handle(player.entity_handle);
            if (!ep || rf::entity_is_dying(ep)) {
                continue;
            }
            draw_player_dot(view, ep->pos, teammate_color(viewer));
        }
    }

    // Concave, so drawn as two triangles: a fan would fill the notch once clipping moves its first vertex.
    void draw_self_arrow(const View& view, const rf::Vector3& pos, float heading)
    {
        constexpr float shape[4][2] = {{0.0f, -7.0f}, {5.0f, 6.0f}, {0.0f, 3.0f}, {-5.0f, 6.0f}};
        constexpr int tris[2][3] = {{0, 1, 2}, {0, 2, 3}};
        const float s = view.scale;
        const float c = std::cos(heading);
        const float sn = std::sin(heading);
        float cx, cy;
        view.to_screen(pos.x, pos.z, cx, cy);
        auto draw = [&](float grow) {
            ClipVert pts[4];
            for (int i = 0; i < 4; ++i) {
                const float px = shape[i][0] * s * grow;
                const float py = shape[i][1] * s * grow;
                pts[i] = {cx + px * c - py * sn, cy + px * sn + py * c, 0.0f, 0.0f};
            }
            for (const auto& tri : tris) {
                const ClipVert poly[3] = {pts[tri[0]], pts[tri[1]], pts[tri[2]]};
                draw_clipped(-1, poly, 3, view.panel, fill_mode);
            }
        };
        rf::gr::set_color(outline_color);
        draw(1.35f);
        rf::gr::set_color(255, 255, 255, 255);
        draw(1.0f);
    }

    // align_x / align_y: 0 anchors the text's left / top edge, 0.5 centres it.
    void draw_label(const View& view, float ax, float ay, const std::string& text, const rf::Color& color,
                    float align_x, float align_y)
    {
        if (text.empty()) {
            return;
        }
        const int font = hud_get_small_font();
        // gr::string does not clip, so a label wider than the view is shortened to fit.
        std::string shown = text;
        const int max_w = static_cast<int>(view.panel.x1 - view.panel.x0 - 2.0f);
        const float w = static_cast<float>(gr_fit_string(shown, max_w, font, "..."));
        const float h = static_cast<float>(rf::gr::get_font_height(font));
        const float max_x = std::max(view.panel.x0, view.panel.x1 - 1.0f - w);
        const float max_y = std::max(view.panel.y0, view.panel.y1 - 1.0f - h);
        const int x = static_cast<int>(std::lround(std::clamp(ax - align_x * w, view.panel.x0, max_x)));
        const int y = static_cast<int>(std::lround(std::clamp(ay - align_y * h, view.panel.y0, max_y)));
        rf::gr::set_color(0, 0, 0, 200);
        rf::gr::string(x + 1, y + 1, shown.c_str(), font);
        rf::gr::set_color(color);
        rf::gr::string(x, y, shown.c_str(), font);
    }

    void draw_labels(const View& view)
    {
        // Outer half-size of the objective and factory squares.
        const float marker_half = 5.0f * view.scale;
        const float gap = 2.0f * view.scale;
        if (multi_is_game_type_with_hills()) {
            for (const HillInfo& h : g_koth_info.hills) {
                if (h.name.empty() || (!h.trigger && !h.handler && h.trigger_uid < 0)) {
                    continue;
                }
                const rf::Vector3 pos = koth_hill_icon_pos(h);
                float sx, sy;
                view.to_screen(pos.x, pos.z, sx, sy);
                draw_label(view, sx, sy + marker_half + gap, h.name, hill_color(h.ownership, 255), 0.5f, 0.0f);
            }
        }
        const int64_t now = timer::get_i64(1000);
        for_each_factory([&](const AlpineVehicleFactoryInfo& info, const VehicleFactoryUi& ui) {
            if (ui.state != AF_VEHICLE_FACTORY_PENDING) {
                return;
            }
            float sx, sy;
            view.to_screen(info.pos.x, info.pos.z, sx, sy);
            if (!view.contains(sx, sy, 0.0f)) {
                return;
            }
            draw_label(view, sx + marker_half + gap, sy, vehicle_marker_countdown_text(ui.deadline_ms - now),
                       hud_amber_color, 0.0f, 0.5f);
        });
    }

    void draw_view(const View& view, const Viewer& viewer, const AlpineLevelProperties& props,
                   const rf::Vector3& self_pos, float heading)
    {
        draw_image(view, props);
        draw_craters(view);
        draw_hills(view);
        draw_vehicles(view, viewer);
        draw_players(view, viewer);
        draw_ctf_flags(view, viewer);
        draw_bag(view);
        draw_salvage_flag(view);
        draw_self_arrow(view, self_pos, heading - view.yaw);
        if (view.labels) {
            draw_labels(view);
        }
    }

    Panel make_panel(int x, int y, int size, int border)
    {
        Panel p{};
        p.x0 = static_cast<float>(x + border);
        p.y0 = static_cast<float>(y + border);
        p.x1 = static_cast<float>(x + size - border);
        p.y1 = static_cast<float>(y + size - border);
        p.cx = 0.5f * (p.x0 + p.x1);
        p.cy = 0.5f * (p.y0 + p.y1);
        return p;
    }

    Viewer make_viewer()
    {
        const bool follows = multi_spectate_is_spectating() && !multi_spectate_is_freelook();
        rf::Player* viewed = follows ? multi_spectate_get_target_player() : nullptr;
        Viewer viewer{};
        viewer.player = viewed ? viewed : rf::local_player;
        viewer.entity = rf::entity_from_handle(viewer.player->entity_handle);
        viewer.team = viewer.player->team;
        viewer.team_mode = multi_is_team_game_type();
        return viewer;
    }

    // Where "you" are on both views: the viewed entity, not a chase or orbit camera behind it.
    rf::Vector3 viewer_world_pos(const Viewer& viewer, rf::Camera* camera)
    {
        // Spectating yourself means a static camera: your (possibly still dying) body is not you.
        const bool at_camera = multi_spectate_is_freelook() || !viewer.entity || rf::entity_is_dying(viewer.entity)
            || (multi_spectate_is_spectating() && viewer.player == rf::local_player);
        if (at_camera) {
            return rf::camera_get_pos(camera);
        }
        const rf::Entity* hull = vehicle_ridden_hull(viewer.entity);
        return hull ? hull->pos : viewer.entity->pos;
    }

    // From the right vector, which stays horizontal through pitch; fvec degenerates looking straight down.
    float camera_heading(rf::Camera* camera)
    {
        const rf::Matrix3 orient = rf::camera_get_orient(camera);
        return std::atan2(-orient.rvec.z, orient.rvec.x);
    }

    // Stable for the level and HUD state, so the reserved panel space does not come and go.
    bool minimap_level_active()
    {
        if (!rf::is_multi || rf::is_dedicated_server || is_headless_mode()) {
            return false;
        }
        if (!AlpineLevelProperties::instance().minimap_enabled || rf::gameseq_get_state() != rf::GS_GAMEPLAY) {
            return false;
        }
        const rf::Player* const lp = rf::local_player;
        return lp && lp->settings.show_hud && !is_hud_effectively_hidden();
    }

    // Overlays that hide both the panel and the big map.
    bool minimap_overlay_active()
    {
        return multi_scoreboard_is_visible() || g_remote_server_cfg_popup.is_active() ||
               vote_panel_is_gameplay_overlay_active();
    }

    // Transient views that hide only the corner panel; its space stays reserved.
    bool corner_panel_suppressed()
    {
        return rf::local_player->fpgun_data.scanning_for_target || multi_spectate_is_freelook() ||
               rf::hud_render_weapon_cycle;
    }

    // control_is_control_down reads raw key state, so a key typed into the chat box would count.
    bool big_map_held()
    {
        if (rf::console::console_is_visible() || rf::multi_chat_is_say_visible()) {
            return false;
        }
        return rf::control_is_control_down(&rf::local_player->settings.controls,
                                           get_af_control(rf::AlpineControlConfigAction::AF_ACTION_BIG_MAP));
    }

    struct PanelStyle
    {
        rf::ubyte bg_alpha;
        rf::ubyte image_alpha;
        float marker_scale;
        bool follow_viewer; // centred on the viewer with cl_minimap_zoom/rotate, else the whole level north-up
        bool labels;
    };

    void render_panel(int x, int y, int size, const PanelStyle& style)
    {
        const auto& props = AlpineLevelProperties::instance();
        const float s = g_hud_ammo_scale;
        const int border = std::max(1, static_cast<int>(std::lround(s)));
        if (size <= 2 * border) {
            return;
        }

        rf::gr::set_color(0, 0, 0, style.bg_alpha);
        rf::gr::rect(x, y, size, size);

        if (rf::Camera* camera = rf::local_player->cam) {
            const rf::Vector3& mn = props.minimap_world_min;
            const rf::Vector3& mx = props.minimap_world_max;
            const Viewer viewer = make_viewer();
            const rf::Vector3 self_pos = viewer_world_pos(viewer, camera);
            const float heading = camera_heading(camera);
            const float extent = std::max(mx.x - mn.x, mx.z - mn.z);

            View view{};
            view.panel = make_panel(x, y, size, border);
            const float width = view.panel.x1 - view.panel.x0;
            if (style.follow_viewer) {
                view.center_x = self_pos.x;
                view.center_z = self_pos.z;
                view.ppu = width / (extent * g_alpine_game_config.minimap_zoom);
                view.yaw = g_alpine_game_config.minimap_rotate ? heading : 0.0f;
            }
            else {
                view.center_x = 0.5f * (mn.x + mx.x);
                view.center_z = 0.5f * (mn.z + mx.z);
                view.ppu = width / extent;
                view.yaw = 0.0f;
            }
            view.cos_yaw = std::cos(view.yaw);
            view.sin_yaw = std::sin(view.yaw);
            view.scale = style.marker_scale * s;
            view.image_alpha = style.image_alpha;
            view.labels = style.labels;

            draw_view(view, viewer, props, self_pos, heading);
        }

        rf::gr::set_color(rf::hud_full_color);
        hud_rect_border(x, y, size, size, border);
    }

    void render_big_map()
    {
        const int clip_w = rf::gr::clip_width();
        const int clip_h = rf::gr::clip_height();
        const int size = static_cast<int>(std::min(0.8f * clip_h, 0.9f * clip_w));
        render_panel((clip_w - size) / 2, (clip_h - size) / 2, size, {110, 200, 1.5f, false, true});
    }
}

std::optional<MinimapRect> minimap_panel_rect()
{
    if (!g_alpine_game_config.minimap || !minimap_level_active()) {
        return std::nullopt;
    }

    const float s = g_hud_ammo_scale;
    const int clip_w = rf::gr::clip_width();
    const int gap = static_cast<int>(std::lround(6.0f * s));
    const int right = std::min(hud_ammo_counter_right_x(), clip_w - 2);
    const int top = hud_ammo_counter_bottom_y() + gap;

    // Stay in the column right of the centred chat box, and above the jetpack gauge's label.
    const int max_from_chat = right - (multi_hud_chat_box_right_x() + gap);
    const int max_from_jet = jetpacks_are_active() ? jetpack_fuel_gauge_label_top_y() - gap - top : max_from_chat;

    const int wanted = static_cast<int>(std::lround(g_alpine_game_config.minimap_size * s));
    const int size = std::min({wanted, max_from_chat, max_from_jet});
    if (size < static_cast<int>(std::lround(48.0f * s))) {
        return std::nullopt;
    }
    return MinimapRect{right - size, top, size};
}

void minimap_render()
{
    if (!minimap_level_active() || minimap_overlay_active()) {
        return;
    }
    if (big_map_held()) {
        render_big_map();
    }
    else if (!corner_panel_suppressed()) {
        if (const auto rect = minimap_panel_rect()) {
            render_panel(rect->x, rect->y, rect->size, {150, 230, 1.0f, true, false});
        }
    }
}

void minimap_notify_geomod(const rf::Vector3& pos, float radius)
{
    if (!rf::is_multi || rf::is_dedicated_server || is_headless_mode() ||
        !AlpineLevelProperties::instance().minimap_enabled || !std::isfinite(radius) || radius <= 0.0f) {
        return;
    }
    for (const Crater& c : g_minimap.craters) {
        if (std::fabs(c.x - pos.x) < 0.01f && std::fabs(c.z - pos.z) < 0.01f && std::fabs(c.radius - radius) < 0.01f) {
            return;
        }
    }
    if (g_minimap.craters.size() >= max_craters) {
        g_minimap.craters.erase(g_minimap.craters.begin());
    }
    g_minimap.craters.push_back({pos.x, pos.z, radius});
}

void minimap_level_init_post()
{
    const auto& props = AlpineLevelProperties::instance();
    if (!rf::is_multi || rf::is_dedicated_server || is_headless_mode() || !props.minimap_enabled) {
        return;
    }
    if (props.minimap_bitmap.empty() || !rf::File{}.find(props.minimap_bitmap.c_str())) {
        xlog::warn("[Minimap] Bitmap '{}' not found, drawing markers only", props.minimap_bitmap);
        return;
    }
    g_minimap.bitmap = rf::bm::load(props.minimap_bitmap.c_str(), -1, true);
}

void minimap_level_reset()
{
    g_minimap = MinimapState{};
}

ConsoleCommand2 minimap_cmd{
    "cl_minimap",
    [](std::optional<bool> enabled) {
        g_alpine_game_config.minimap = enabled.value_or(!g_alpine_game_config.minimap);
        rf::console::print("Minimap is {}", g_alpine_game_config.minimap ? "enabled" : "disabled");
    },
    "Show the multiplayer minimap on levels that provide one",
    "cl_minimap [bool]",
};

ConsoleCommand2 minimap_rotate_cmd{
    "cl_minimap_rotate",
    [](std::optional<bool> enabled) {
        g_alpine_game_config.minimap_rotate = enabled.value_or(!g_alpine_game_config.minimap_rotate);
        rf::console::print("Minimap rotation is {}",
                           g_alpine_game_config.minimap_rotate ? "enabled (player heading up)" : "disabled (north up)");
    },
    "Rotate the minimap with the player's heading instead of keeping north up",
    "cl_minimap_rotate [bool]",
};

ConsoleCommand2 minimap_size_cmd{
    "cl_minimap_size",
    [](std::optional<float> size) {
        if (size) {
            g_alpine_game_config.set_minimap_size(*size);
        }
        rf::console::print("Minimap size is {:.0f} ({:.0f}-{:.0f}, scaled with the big HUD)",
                           g_alpine_game_config.minimap_size, AlpineGameSettings::min_minimap_size,
                           AlpineGameSettings::max_minimap_size);
    },
    "Set the minimap panel size in small-HUD pixels",
    "cl_minimap_size [80-240]",
};

ConsoleCommand2 minimap_zoom_cmd{
    "cl_minimap_zoom",
    [](std::optional<float> zoom) {
        if (zoom) {
            g_alpine_game_config.set_minimap_zoom(*zoom);
        }
        rf::console::print("Minimap zoom is {:.2f} (share of the level's width shown across the panel)",
                           g_alpine_game_config.minimap_zoom);
    },
    "Set how much of the level the minimap shows, as a share of the level's larger extent",
    "cl_minimap_zoom [0.1-1.0]",
};

void minimap_apply_patches()
{
    minimap_cmd.register_cmd();
    minimap_rotate_cmd.register_cmd();
    minimap_size_cmd.register_cmd();
    minimap_zoom_cmd.register_cmd();
}
