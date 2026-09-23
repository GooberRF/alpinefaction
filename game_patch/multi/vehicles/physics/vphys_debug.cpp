#include <algorithm>
#include <cmath>
#include <vector>
#include <common/utils/list-utils.h>
#include "vphys_internal.h"
#include "../vehicle_physics.h"
#include "../vehicle.h"
#include "../../../graphics/gr.h"
#include "../../../rf/ai.h"
#include "../../../rf/entity.h"
#include "../../../rf/gameseq.h"
#include "../../../rf/multi.h"
#include "../../../rf/physics.h"
#include "../../../rf/player/camera.h"

bool g_vphys_dbg = false;

namespace
{
    rf::Vector3 hull_local_to_world(const rf::Entity* ep, const btVector3& local)
    {
        return ep->pos + ep->orient.rvec * local.x() + ep->orient.uvec * local.y()
             + ep->orient.fvec * local.z();
    }

    constexpr int box_edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                      {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};

    void vphys_dbg_draw_box(const rf::Entity* ep, const HullBox& box)
    {
        rf::Vector3 corner[8];
        for (int i = 0; i < 8; ++i) {
            const btVector3 local(box.center.x() + ((i & 1) ? box.half.x() : -box.half.x()),
                                  box.center.y() + ((i & 2) ? box.half.y() : -box.half.y()),
                                  box.center.z() + ((i & 4) ? box.half.z() : -box.half.z()));
            corner[i] = hull_local_to_world(ep, local);
        }
        for (const auto& e : box_edges) {
            rf::gr::line_vec(corner[e[0]], corner[e[1]], no_overdraw_2d_line);
        }
    }

    void vphys_dbg_draw_aabb(const rf::Vector3& lo, const rf::Vector3& hi)
    {
        rf::Vector3 corner[8];
        for (int i = 0; i < 8; ++i) {
            corner[i] = rf::Vector3{(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y,
                                    (i & 4) ? hi.z : lo.z};
        }
        for (const auto& e : box_edges) {
            rf::gr::line_vec(corner[e[0]], corner[e[1]], no_overdraw_2d_line);
        }
    }

    constexpr float vphys_dbg_chunk_radius = 40.0f;

    void vphys_dbg_draw_level_mesh(const rf::Vector3& view)
    {
        static std::vector<LevelMeshDebugChunk> chunks;
        level_mesh_debug_collect(view, vphys_dbg_chunk_radius, chunks);
        for (const LevelMeshDebugChunk& c : chunks) {
            if (c.recently_rebuilt) {
                rf::gr::set_color(255, 140, 20, 255);
            }
            else {
                rf::gr::set_color(70, 105, 80, 255);
            }
            vphys_dbg_draw_aabb(c.aabb_min, c.aabb_max);
        }
        const float cell = level_mesh_debug_cell_size();
        const rf::Vector3 base = level_mesh_debug_cell_origin(view);
        rf::gr::set_color(60, 70, 110, 255);
        for (int x = -1; x <= 1; ++x) {
            for (int y = -1; y <= 1; ++y) {
                for (int z = -1; z <= 1; ++z) {
                    const rf::Vector3 lo{base.x + static_cast<float>(x) * cell,
                                         base.y + static_cast<float>(y) * cell,
                                         base.z + static_cast<float>(z) * cell};
                    vphys_dbg_draw_aabb(lo, rf::Vector3{lo.x + cell, lo.y + cell, lo.z + cell});
                }
            }
        }
    }

    HullBox dbg_inflate(const HullBox& box, float m)
    {
        HullBox out;
        out.half = btVector3(box.half.x() + m, box.half.y() + m, box.half.z() + m);
        out.center = box.center;
        return out;
    }

    void vphys_dbg_draw_cross(const rf::Vector3& p, float size)
    {
        rf::gr::line_vec(rf::Vector3{p.x - size, p.y, p.z}, rf::Vector3{p.x + size, p.y, p.z},
                         no_overdraw_2d_line);
        rf::gr::line_vec(rf::Vector3{p.x, p.y - size, p.z}, rf::Vector3{p.x, p.y + size, p.z},
                         no_overdraw_2d_line);
        rf::gr::line_vec(rf::Vector3{p.x, p.y, p.z - size}, rf::Vector3{p.x, p.y, p.z + size},
                         no_overdraw_2d_line);
    }

    void vphys_dbg_draw_wheel_circle(const rf::Entity* ep, const rf::Vector3& centre, float radius)
    {
        constexpr int segments = 16;
        rf::Vector3 prev{};
        for (int i = 0; i <= segments; ++i) {
            const float a = vehicle_two_pi * static_cast<float>(i) / static_cast<float>(segments);
            const rf::Vector3 point =
                centre + ep->orient.fvec * (std::cos(a) * radius) + ep->orient.uvec * (std::sin(a) * radius);
            if (i > 0) {
                rf::gr::line_vec(prev, point, no_overdraw_2d_line);
            }
            prev = point;
        }
    }

    void vphys_dbg_draw_driven_car(const VehicleSimBody& b, rf::Entity* ep,
                                   const VehiclePhysicsParams& p)
    {
        btRaycastVehicle* veh = b.raycast_vehicle;
        if (!veh) {
            return;
        }
        for (int i = 0; i < veh->getNumWheels(); ++i) {
            const btWheelInfo& w = veh->getWheelInfo(i);
            const rf::Vector3 hard = from_bt(w.m_raycastInfo.m_hardPointWS);
            const rf::Vector3 down = from_bt(w.m_raycastInfo.m_wheelDirectionWS);
            const float ray_len = w.getSuspensionRestLength() + w.m_wheelsRadius;
            const bool contact = w.m_raycastInfo.m_isInContact;

            rf::gr::set_color(255, 220, 0, 255);
            vphys_dbg_draw_cross(hard, 0.12f);

            rf::gr::set_color(90, 90, 120, 255);
            rf::gr::line_vec(hard, hard + down * ray_len, no_overdraw_2d_line);

            if (contact) {
                rf::gr::set_color(40, 230, 60, 255);
            }
            else {
                rf::gr::set_color(230, 50, 50, 255);
            }
            const rf::Vector3 centre = hard + down * w.m_raycastInfo.m_suspensionLength;
            rf::gr::line_vec(hard, centre, no_overdraw_2d_line);
            vphys_dbg_draw_wheel_circle(ep, centre, w.m_wheelsRadius);
            if (contact) {
                vphys_dbg_draw_cross(from_bt(w.m_raycastInfo.m_contactPointWS), 0.2f);
            }
        }

        {
            const HullBox& cb = b.car_box;
            const float wr = b.wheel_reach;
            rf::gr::set_color(90, 200, 90, 255);
            rf::Vector3 c[4];
            for (int i = 0; i < 4; ++i) {
                c[i] = hull_local_to_world(
                    ep, btVector3(cb.center.x() + ((i & 1) ? cb.half.x() : -cb.half.x()), -wr,
                                  cb.center.z() + ((i & 2) ? cb.half.z() : -cb.half.z())));
            }
            rf::gr::line_vec(c[0], c[1], no_overdraw_2d_line);
            rf::gr::line_vec(c[2], c[3], no_overdraw_2d_line);
            rf::gr::line_vec(c[0], c[2], no_overdraw_2d_line);
            rf::gr::line_vec(c[1], c[3], no_overdraw_2d_line);
        }

        const float arrow = 2.0f + b.car_box.half.y();
        rf::gr::set_color(235, 235, 235, 255);
        rf::gr::line_vec(ep->pos, ep->pos + ep->orient.uvec * arrow, no_overdraw_2d_line);
        if (b.upright_recovering) {
            rf::gr::set_color(255, 40, 40, 255);
        }
        else {
            rf::gr::set_color(255, 150, 20, 255);
        }
        const rf::Vector3 ref_tip = ep->pos + b.upright_ref * arrow;
        rf::gr::line_vec(ep->pos, ref_tip, no_overdraw_2d_line);
        vphys_dbg_draw_cross(ref_tip, 0.25f);

        if (p.drill_probe_reach > 0.0f) {
            const HullBox& box = b.car_box;
            const float front = box.center.z() + box.half.z();
            const rf::Vector3 nose = hull_local_to_world(ep, btVector3(box.center.x(), box.center.y(), 0.0f));
            rf::gr::set_color(0, 220, 255, 255);
            rf::gr::line_vec(nose, nose + ep->orient.fvec * (front + std::max(p.chassis_clearance, 0.0f)),
                             no_overdraw_2d_line);
            rf::gr::set_color(255, 60, 255, 255);
            const rf::Vector3 tip = nose + ep->orient.fvec * p.drill_probe_reach;
            rf::gr::line_vec(nose + ep->orient.fvec * front, tip, no_overdraw_2d_line);
            vphys_dbg_draw_cross(tip, 0.3f);
        }
    }
} // namespace

void vphys_render_debug()
{
    if (!g_vphys_dbg || rf::gameseq_get_state() != rf::GS_GAMEPLAY) {
        return;
    }
    const rf::Entity* view_ep =
        g_vphys.driven ? rf::entity_from_handle(g_vphys.driven->vehicle_handle) : nullptr;
    if (!view_ep) {
        view_ep = rf::local_player_entity;
    }
    if (view_ep) {
        vphys_dbg_draw_level_mesh(view_ep->pos);
    }
    for (rf::Entity& entity : DoublyLinkedList{rf::entity_list}) {
        if (!vehicle_is_synced_entity_type(&entity) || rf::entity_is_dying(&entity)) {
            continue;
        }
        const VehicleSimBody* sim = sim_body_from_handle(entity.handle);
        const bool driven = sim != nullptr && sim->body != nullptr;

        const HullBox base = driven ? sim->car_box_base : hull_local_box(&entity);
        rf::gr::set_color(110, 110, 120, 255);
        vphys_dbg_draw_box(&entity, base);
        rf::gr::set_color(230, 60, 230, 255);
        vphys_dbg_draw_box(&entity, dbg_inflate(base, vehicle_ram_contact_margin));

        const int cls = vphys_class_for(&entity);
        if (cls < 0) {
            continue; // a turret: the base and ram boxes above are all it has
        }
        const VehiclePhysicsParams& p = params_for_class(cls);
        if (vphys_class_is_automobile(cls)) {
            const HullBox box = driven ? sim->car_box
                                       : hull_contact_box(base, p, hull_bottom_raise(p, &entity, sim));
            rf::gr::set_color(driven ? 0 : 90, driven ? 220 : 170, driven ? 255 : 190, 255);
            vphys_dbg_draw_box(&entity, box);
            rf::gr::set_color(240, 220, 60, 255);
            vphys_dbg_draw_box(&entity, dbg_inflate(box, std::max(p.chassis_clearance, 0.0f)));
            if (driven) {
                vphys_dbg_draw_driven_car(*sim, &entity, p);
            }
            continue;
        }
        rf::gr::set_color(driven ? 0 : 90, driven ? 220 : 170, driven ? 255 : 190, 255);
        rf::gr::sphere(entity.pos, hull_radius_for(p, &entity), no_overdraw_2d_line);
        rf::gr::set_color(240, 220, 60, 255);
        rf::gr::sphere(entity.pos, hull_standoff(p, &entity), no_overdraw_2d_line);
    }
}
