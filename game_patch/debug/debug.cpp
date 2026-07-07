#include <cmath>
#include <algorithm>
#include "debug_internal.h"
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include <xlog/xlog.h>
#include "../os/console.h"
#include "../rf/entity.h"
#include "../rf/vmesh.h"
#include "../rf/player/player.h"
#include "../multi/multi.h"

#define DEBUG_PERF 1

#ifdef NDEBUG
#define MEMORY_TRACKING 0
#define VARRAY_OOB_CHECK 0
#define EMULATE_PACKET_LOSS 0
#else // NDEBUG
#define MEMORY_TRACKING 1
#define VARRAY_OOB_CHECK 0
#define EMULATE_PACKET_LOSS 0
#define PACKET_LOSS_RATE 10 // every n packet is lost
#endif // NDEBUG

#if MEMORY_TRACKING
constexpr uint32_t BOUND_MARKER = 0xDEADBEEF;
#endif

int g_num_heap_allocs = 0;
size_t g_current_heap_usage = 0;
size_t g_max_heap_usage = 0;

FunHook<int(size_t)> callnewh_hook{
    0x0057A212,
    [](size_t size) {
        xlog::error("Failed to allocate {} bytes", size);
        return callnewh_hook.call_target(size);
    },
};

#if MEMORY_TRACKING

FunHook<void*(size_t, bool)> nh_malloc_hook{
    0x00573B49,
    [](size_t size, bool unk) -> void* {
        g_num_heap_allocs++;
        g_current_heap_usage += size;
        g_max_heap_usage = std::max(g_max_heap_usage, g_current_heap_usage);
        void* ptr = nh_malloc_hook.call_target(size + 12, unk);
        if (!ptr) {
            xlog::trace("Allocation of {} bytes failed!", size);
            return nullptr;
        }
        auto bytes = static_cast<std::byte*>(ptr);
        *reinterpret_cast<uint32_t*>(bytes) = size;
        *reinterpret_cast<uint32_t*>(bytes + 4) = BOUND_MARKER;
        *reinterpret_cast<uint32_t*>(bytes + 8 + size) = BOUND_MARKER;
        // Overwrite old data to make detecting use-after-free errors easier
        std::memset(bytes + 8, 0xCC, size);
        auto result = bytes + 8;
        xlog::trace("nh_malloc {:x} -> {}", size, result);
        return result;
    },
};

FunHook<void(void*)> free_hook{
    0x00573C71,
    [](void* ptr) {
        xlog::trace("free {}", ptr);
        if (!ptr) {
            return;
        }
        auto bytes = static_cast<std::byte*>(ptr);
        bytes -= 8;
        auto size = *reinterpret_cast<uint32_t*>(bytes);
        auto front_marker = *reinterpret_cast<uint32_t*>(bytes + 4);
        auto tail_marker = *reinterpret_cast<uint32_t*>(bytes + 8 + size);
        g_current_heap_usage -= size;
        if (front_marker != BOUND_MARKER) {
            xlog::warn("Memory corruption detected: front marker {:x}", front_marker);
        }
        if (tail_marker != BOUND_MARKER) {
            xlog::warn("Memory corruption detected: tail marker {:x}", tail_marker);
        }
        // Overwrite old data to make detecting use-after-free errors easier
        std::memset(bytes + 8, 0xCC, size);
        free_hook.call_target(bytes);
    },
};

FunHook<void*(void*, size_t)> realloc_hook{
    0x00578873,
    [](void* ptr, size_t size) {
        auto bytes = static_cast<std::byte*>(ptr);
        bytes -= 8;
        auto old_size = *reinterpret_cast<size_t*>(bytes);
        g_current_heap_usage -= old_size;
        g_current_heap_usage += size;
        g_max_heap_usage = std::max(g_max_heap_usage, g_current_heap_usage);
        auto out_ptr = realloc_hook.call_target(bytes, size + 12);
        auto out_bytes = reinterpret_cast<std::byte*>(out_ptr);
        out_ptr = out_bytes + 8;
        xlog::trace("realloc {} {:x} -> {}", ptr, size, out_ptr);
        return out_ptr;
    },
};

FunHook<size_t(void*)> msize_hook{
    0x00578BA2,
    [](void* ptr) {
        xlog::trace("msize {}", ptr);
        ptr = static_cast<std::byte*>(ptr) - 8;
        return msize_hook.call_target(ptr) - 12;
    },
};

ConsoleCommand2 mem_stats_cmd{
    "d_mem_stats",
    []() {
        constexpr float mb_float = 1024.0f * 1024.0f;
        rf::console::print("Number of heap allocations: {}", g_num_heap_allocs);
        rf::console::print("Current heap usage: {:.2f} MB", g_current_heap_usage / mb_float);
        rf::console::print("Max heap usage: {:.2f} MB", g_max_heap_usage / mb_float);
    },
};

#endif // MEMORY_TRACKING

#if VARRAY_OOB_CHECK
CodeInjection VArray_Ptr__get_out_of_bounds_check{
    0x0040A480,
    [](auto& regs) {
        int size = *static_cast<int*>(regs.ecx);
        int index = *reinterpret_cast<int*>(regs.esp + 4);
        if (index < 0 || index >= size) {
            xlog::error("VArray out of bounds access! index {} size {}", index, size);
        }
    },
};
#endif // VARRAY_OOB_CHECK

#if EMULATE_PACKET_LOSS

FunHook<int(const void*, unsigned, int, const rf::NetAddr*, int)> net_send_hook{
    0x00528820,
    [](const void* packet, unsigned packet_len, int flags, const rf::NetAddr* addr, int packet_kind) {
        if (rand() % PACKET_LOSS_RATE == 0)
            return 0;
        return net_send_hook.call_target(packet, packet_len, flags, addr, packet_kind);
    },
};

FunHook<void(void*, const void*, unsigned, rf::NetAddr*)> net_buffer_packet_hook{
    0x00528950,
    [](void* buffer, const void* data, unsigned data_len, rf::NetAddr* addr) {
        if (rand() % PACKET_LOSS_RATE == 0)
            return;
        return net_buffer_packet_hook.call_target(buffer, data, data_len, addr);
    },
};

#endif // EMULATE_PACKET_LOSS

void debug_multi_init()
{
    debug_cmd_multi_init();
    profiler_multi_init();
}

void debug_apply_patches()
{
    // Log error when memory allocation fails
    callnewh_hook.install();

#if MEMORY_TRACKING
    nh_malloc_hook.install();
    free_hook.install();
    realloc_hook.install();
    msize_hook.install();
#endif // MEMORY_TRACKING

#if VARRAY_OOB_CHECK
    VArray_Ptr__get_out_of_bounds_check.install();
#endif

#if EMULATE_PACKET_LOSS
    net_send_hook.install();
    net_buffer_packet_hook.install();
#endif

    debug_unresponsive_apply_patches();
#if DEBUG_PERF
    profiler_init();
#endif
}

void debug_init()
{
#if MEMORY_TRACKING
    mem_stats_cmd.register_cmd();
#endif

    debug_cmd_init();
    debug_unresponsive_init();
#ifndef NDEBUG
    register_obj_debug_commands();
#endif
}

static void draw_3d_capsule_general(const rf::Vector3& cap_a, const rf::Vector3& cap_b, float radius)
{
    auto mode = rf::gr::Mode{
        rf::gr::TEXTURE_SOURCE_NONE,
        rf::gr::COLOR_SOURCE_VERTEX,
        rf::gr::ALPHA_SOURCE_VERTEX,
        rf::gr::ALPHA_BLEND_NONE,
        rf::gr::ZBUFFER_TYPE_FULL,
        rf::gr::FOG_NOT_ALLOWED,
    };

    rf::Vector3 axis = cap_b - cap_a;
    float axis_len = axis.len();

    constexpr int num_segments = 24;
    constexpr int half_arc = num_segments / 4;
    constexpr float two_pi = 6.2831853f;
    constexpr float half_pi = 1.5707963f;

    rf::Vector3 v_axis;
    if (axis_len > 1e-6f) {
        v_axis = axis * (1.0f / axis_len);
    }
    else {
        v_axis = {0.0f, 1.0f, 0.0f};
    }

    rf::Vector3 helper = (std::abs(v_axis.y) < 0.9f)
        ? rf::Vector3{0.0f, 1.0f, 0.0f}
        : rf::Vector3{1.0f, 0.0f, 0.0f};
    rf::Vector3 u_axis = v_axis.cross(helper);
    u_axis.normalize();
    rf::Vector3 w_axis = v_axis.cross(u_axis);

    rf::Vector3 a_pts[num_segments];
    rf::Vector3 b_pts[num_segments];

    for (int i = 0; i < num_segments; i++) {
        float angle = two_pi * static_cast<float>(i) / static_cast<float>(num_segments);
        rf::Vector3 offset = u_axis * (radius * std::cos(angle)) + w_axis * (radius * std::sin(angle));
        a_pts[i] = cap_a + offset;
        b_pts[i] = cap_b + offset;
    }

    for (int i = 0; i < num_segments; i++) {
        int next = (i + 1) % num_segments;
        rf::gr::line_vec(a_pts[i], a_pts[next], mode);
        rf::gr::line_vec(b_pts[i], b_pts[next], mode);

        if (i % (num_segments / 4) == 0) {
            rf::gr::line_vec(a_pts[i], b_pts[i], mode);
        }
    }

    auto draw_hemisphere_arc = [&](const rf::Vector3& center, float sign, const rf::Vector3& arc_dir) {
        rf::Vector3 prev;
        for (int i = 0; i <= half_arc; i++) {
            float phi = half_pi * static_cast<float>(i) / static_cast<float>(half_arc);
            float r_eq = radius * std::cos(phi);
            float r_ax = radius * std::sin(phi) * sign;
            rf::Vector3 pt = center + arc_dir * r_eq + v_axis * r_ax;
            if (i > 0)
                rf::gr::line_vec(prev, pt, mode);
            prev = pt;
        }
    };

    draw_hemisphere_arc(cap_a, -1.0f, u_axis);
    draw_hemisphere_arc(cap_a, -1.0f, w_axis);
    draw_hemisphere_arc(cap_a, -1.0f, -u_axis);
    draw_hemisphere_arc(cap_a, -1.0f, -w_axis);

    draw_hemisphere_arc(cap_b, 1.0f, u_axis);
    draw_hemisphere_arc(cap_b, 1.0f, w_axis);
    draw_hemisphere_arc(cap_b, 1.0f, -u_axis);
    draw_hemisphere_arc(cap_b, 1.0f, -w_axis);
}

static void draw_3d_cylinder_general(const rf::Vector3& cyl_a, const rf::Vector3& cyl_b, float radius)
{
    auto mode = rf::gr::Mode{
        rf::gr::TEXTURE_SOURCE_NONE,
        rf::gr::COLOR_SOURCE_VERTEX,
        rf::gr::ALPHA_SOURCE_VERTEX,
        rf::gr::ALPHA_BLEND_NONE,
        rf::gr::ZBUFFER_TYPE_FULL,
        rf::gr::FOG_NOT_ALLOWED,
    };

    rf::Vector3 axis = cyl_b - cyl_a;
    float axis_len = axis.len();

    constexpr int num_segments = 24;
    constexpr float two_pi = 6.2831853f;

    rf::Vector3 v_axis;
    if (axis_len > 1e-6f) {
        v_axis = axis * (1.0f / axis_len);
    }
    else {
        v_axis = {0.0f, 1.0f, 0.0f};
    }

    rf::Vector3 helper = (std::abs(v_axis.y) < 0.9f)
        ? rf::Vector3{0.0f, 1.0f, 0.0f}
        : rf::Vector3{1.0f, 0.0f, 0.0f};
    rf::Vector3 u_axis = v_axis.cross(helper);
    u_axis.normalize();
    rf::Vector3 w_axis = v_axis.cross(u_axis);

    rf::Vector3 a_pts[num_segments];
    rf::Vector3 b_pts[num_segments];

    for (int i = 0; i < num_segments; i++) {
        float angle = two_pi * static_cast<float>(i) / static_cast<float>(num_segments);
        rf::Vector3 offset = u_axis * (radius * std::cos(angle)) + w_axis * (radius * std::sin(angle));
        a_pts[i] = cyl_a + offset;
        b_pts[i] = cyl_b + offset;
    }

    for (int i = 0; i < num_segments; i++) {
        int next = (i + 1) % num_segments;
        // Rings at each end
        rf::gr::line_vec(a_pts[i], a_pts[next], mode);
        rf::gr::line_vec(b_pts[i], b_pts[next], mode);

        // Connecting lines at cardinal points
        if (i % (num_segments / 4) == 0) {
            rf::gr::line_vec(a_pts[i], b_pts[i], mode);
        }
    }

    // Cross-lines on disc caps
    rf::gr::line_vec(a_pts[0], a_pts[num_segments / 2], mode);
    rf::gr::line_vec(a_pts[num_segments / 4], a_pts[3 * num_segments / 4], mode);
    rf::gr::line_vec(b_pts[0], b_pts[num_segments / 2], mode);
    rf::gr::line_vec(b_pts[num_segments / 4], b_pts[3 * num_segments / 4], mode);
}

static void draw_3d_aabb(const rf::Vector3& bbox_min, const rf::Vector3& bbox_max)
{
    auto mode = rf::gr::Mode{
        rf::gr::TEXTURE_SOURCE_NONE,
        rf::gr::COLOR_SOURCE_VERTEX,
        rf::gr::ALPHA_SOURCE_VERTEX,
        rf::gr::ALPHA_BLEND_NONE,
        rf::gr::ZBUFFER_TYPE_FULL,
        rf::gr::FOG_NOT_ALLOWED,
    };

    // 8 corners of the box
    rf::Vector3 c[8] = {
        {bbox_min.x, bbox_min.y, bbox_min.z},
        {bbox_max.x, bbox_min.y, bbox_min.z},
        {bbox_max.x, bbox_min.y, bbox_max.z},
        {bbox_min.x, bbox_min.y, bbox_max.z},
        {bbox_min.x, bbox_max.y, bbox_min.z},
        {bbox_max.x, bbox_max.y, bbox_min.z},
        {bbox_max.x, bbox_max.y, bbox_max.z},
        {bbox_min.x, bbox_max.y, bbox_max.z},
    };

    // Bottom face
    rf::gr::line_vec(c[0], c[1], mode);
    rf::gr::line_vec(c[1], c[2], mode);
    rf::gr::line_vec(c[2], c[3], mode);
    rf::gr::line_vec(c[3], c[0], mode);
    // Top face
    rf::gr::line_vec(c[4], c[5], mode);
    rf::gr::line_vec(c[5], c[6], mode);
    rf::gr::line_vec(c[6], c[7], mode);
    rf::gr::line_vec(c[7], c[4], mode);
    // Vertical edges
    rf::gr::line_vec(c[0], c[4], mode);
    rf::gr::line_vec(c[1], c[5], mode);
    rf::gr::line_vec(c[2], c[6], mode);
    rf::gr::line_vec(c[3], c[7], mode);
}

// Fraction of the bbox height by which the engine lowers a crouched entity's bbox_max.y (matches
// the FMUL constant at 0x005893C0 in the stock collision path). Used to reconstruct the same bbox
// the server hit-tests against before handing it to compute_hitbox_geometry().
static constexpr float k_crouch_bbox_top_fraction = 0.5f;

static void render_hitboxes()
{
    if (!g_dbg_hitboxes && !g_dbg_cspheres)
        return;

    auto& multi_entity_bbox_size = addr_as_ref<rf::Vector3>(0x007C6A70);
    auto& server_info = get_af_server_info();
    bool legacy = server_info ? server_info->legacy_hitboxes : g_alpine_server_config.legacy_hitboxes;

    rf::Entity* ep = rf::entity_list.next;
    while (ep != &rf::entity_list) {
        rf::Entity* entity = ep;
        ep = ep->next;

        if (!entity->vmesh)
            continue;

        if (rf::local_player && entity->handle == rf::local_player->entity_handle)
            continue;

        // Hybrid (or legacy) hitbox volume — independent toggle: `debug hitbox`
        if (g_dbg_hitboxes) {
            // Match engine bbox computation: entity->pos ± half_size,
            // then for crouching only bbox_max.y is lowered (feet stay on floor)
            rf::Vector3 half_size = multi_entity_bbox_size;
            rf::Vector3 bbox_min = entity->pos - half_size;
            rf::Vector3 bbox_max = entity->pos + half_size;
            bool crouching = rf::entity_is_crouching(entity);
            if (crouching) {
                bbox_max.y -= multi_entity_bbox_size.y * k_crouch_bbox_top_fraction;
            }

            if (legacy) {
                // Legacy mode: draw the AABB that the engine actually uses for collision
                rf::gr::set_color(255, 128, 0, 255);
                draw_3d_aabb(bbox_min, bbox_max);
            }
            else {
                // Compute the exact same volume the server hit-tests (shared with the collision path).
                HitboxGeometry geo = compute_hitbox_geometry(entity, bbox_min, bbox_max);

                // Lower capsule (green)
                rf::gr::set_color(0, 255, 0, 255);
                draw_3d_capsule_general(geo.lower_bot, geo.lower_top, geo.radius);

                if (geo.split) {
                    // Torso cylinder (cyan)
                    rf::gr::set_color(0, 255, 255, 255);
                    draw_3d_cylinder_general(geo.upper_a, geo.upper_b, geo.radius);

                    // Head sphere (magenta)
                    if (geo.has_head) {
                        rf::gr::set_color(255, 0, 255, 255);
                        auto mode = rf::gr::Mode{
                            rf::gr::TEXTURE_SOURCE_NONE,
                            rf::gr::COLOR_SOURCE_VERTEX,
                            rf::gr::ALPHA_SOURCE_VERTEX,
                            rf::gr::ALPHA_BLEND_NONE,
                            rf::gr::ZBUFFER_TYPE_FULL,
                            rf::gr::FOG_NOT_ALLOWED,
                        };
                        rf::gr::sphere(geo.head_pos, geo.head_radius, mode);
                    }
                }
            }
        }

        // Engine collision spheres (yellow) — independent toggle: `debug cspheres`
        if (g_dbg_cspheres) {
            int num_cspheres = rf::vmesh_get_num_cspheres(entity->vmesh);
            for (int i = 0; i < num_cspheres; i++) {
                rf::Vector3 sphere_pos;
                if (rf::vmesh_get_csphere_pos(entity->vmesh, i, &sphere_pos, &entity->pos, &entity->orient)) {
                    rf::Vector3 local_pos;
                    float sphere_radius;
                    if (rf::vmesh_get_csphere(entity->vmesh, i, &local_pos, &sphere_radius)) {
                        rf::gr::set_color(255, 255, 0, 180);
                        auto mode = rf::gr::Mode{
                            rf::gr::TEXTURE_SOURCE_NONE,
                            rf::gr::COLOR_SOURCE_VERTEX,
                            rf::gr::ALPHA_SOURCE_VERTEX,
                            rf::gr::ALPHA_BLEND_ALPHA,
                            rf::gr::ZBUFFER_TYPE_FULL,
                            rf::gr::FOG_NOT_ALLOWED,
                        };
                        rf::gr::sphere(sphere_pos, sphere_radius, mode);
                    }
                }
            }
        }
    }
}

void debug_render()
{
    debug_cmd_render();
    render_hitboxes();
}

void debug_render_ui()
{
    debug_cmd_render_ui();
#if DEBUG_PERF
    profiler_draw_ui();
#endif
#ifndef NDEBUG
    render_obj_debug_ui();
#endif
}

void debug_cleanup()
{
    debug_unresponsive_cleanup();
}

void debug_do_frame_pre()
{
    debug_unresponsive_do_update();
}

void debug_do_frame_post()
{
    profiler_do_frame_post();
}
