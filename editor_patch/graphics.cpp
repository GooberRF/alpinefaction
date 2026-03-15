#include <common/config/BuildConfig.h>
#include <common/utils/os-utils.h>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/MemUtils.h>
#include <xlog/xlog.h>
#include <d3d8.h>
#include <algorithm>
#include <cmath>

HWND GetMainFrameHandle();

// After geometry rebuild, rooms allocated from recycled heap memory may have stale
// non-NULL geo_cache pointers left over from the previous cycle. The D3D8 renderer
// checks geo_cache != NULL to decide whether to use an existing cache or rebuild.
// Stale pointers cause the renderer to use invalid cache data, producing artifacts.
//
// Fix: validate geo_cache pointers in the render function by checking whether they
// fall within the valid render cache pool allocation range [pool_base, pool_cursor_snapshot).
// The snapshot is taken at the start of each render pass (loop index 0), BEFORE any rooms
// are rebuilt in that frame. This prevents the advancing pool_cursor from causing later
// rooms' stale pointers to pass the bounds check during the same render pass.
static char* pool_cursor_snapshot = nullptr;

// Detail rooms array: replaces the stock 256-entry array at 0x010cee5c with count at 0x010cf25c.
// Levels with many brushes can exceed 256 detail rooms, overflowing the array and corrupting the
// adjacent count variable, causing the BSP traversal to read garbage pointers and crash.
static constexpr int max_detail_rooms = 8192;
static void* detail_room_list[max_detail_rooms];
static int detail_room_count;

namespace red
{
    struct GrScreen
    {
        int signature;
        int max_width;
        int max_height;
        int mode;
        int window_mode;
        int field_14;
        float aspect;
        int field_1c;
        int bits_per_pixel;
        int bytes_ber_pixel;
        int field_28;
        int offset_x;
        int offset_y;
        int clip_width;
        int clip_height;
        int max_tex_width;
        int max_tex_height;
        int clip_left;
        int clip_right;
        int clip_top;
        int clip_bottom;
        int current_color;
        int current_bitmap;
        int current_bitmap2;
        int fog_mode;
        int fog_color;
        float fog_near;
        float fog_far;
        float fog_far_scaled;
        bool recolor_enabled;
        float recolor_red;
        float recolor_green;
        float recolor_blue;
        int field_84;
        int field_88;
        int zbuffer_mode;
    };
    static_assert(sizeof(GrScreen) == 0x90);

    struct Vector3;
    struct Matrix3;

    auto& gr_d3d_buffers_locked = addr_as_ref<bool>(0x0183930D);
    auto& gr_d3d_primitive_type = addr_as_ref<D3DPRIMITIVETYPE>(0x0175A2C8);
    auto& gr_d3d_max_hw_vertex = addr_as_ref<int>(0x01621FA8);
    auto& gr_d3d_max_hw_index = addr_as_ref<int>(0x01621FAC);
    auto& gr_d3d_num_vertices = addr_as_ref<int>(0x01839310);
    auto& gr_d3d_num_indices = addr_as_ref<int>(0x01839314);
    auto& gr_screen = addr_as_ref<GrScreen>(0x014CF748);

}

CallHook<void()> frametime_calculate_hook{
    0x00483047,
    []() {
        HWND hwnd = GetMainFrameHandle();
        if (!IsWindowVisible(hwnd)) {
            // when minimized limit to 1 FPS
            Sleep(1000);
        }
        else {
            // Check if editor is the foreground window
            HWND foreground_wnd = GetForegroundWindow();
            bool editor_is_in_foreground = GetWindowThreadProcessId(foreground_wnd, nullptr) == GetCurrentThreadId();
            // when editor is in background limit to 4 FPS
            if (!editor_is_in_foreground) {
                Sleep(250);
            }
        }
        frametime_calculate_hook.call_target();
    },
};

CodeInjection after_gr_init_hook{
    0x004B8D4D,
    []() {
        // Fix performance issues caused by this field being initialized to inf
        red::gr_screen.fog_far_scaled = 255.0f / red::gr_screen.fog_far;
    },
};

CodeInjection gr_d3d_line_3d_patch_1{
    0x004E133E,
    [](auto& regs) {
        bool flush_needed = !red::gr_d3d_buffers_locked
                         || red::gr_d3d_primitive_type != D3DPT_LINELIST
                         || red::gr_d3d_max_hw_vertex + 2 > 6000
                         || red::gr_d3d_max_hw_index + red::gr_d3d_num_indices + 2 > 10000;
        if (!flush_needed) {
            xlog::trace("Skipping gr_d3d_prepare_buffers");
            regs.eip = 0x004E1343;
        }
        else {
            xlog::trace("Line drawing requires gr_d3d_prepare_buffers {} {} {} {}",
                 red::gr_d3d_buffers_locked, red::gr_d3d_primitive_type, red::gr_d3d_max_hw_vertex,
                 red::gr_d3d_max_hw_index + red::gr_d3d_num_indices);
        }
    },
};

CallHook<void()> gr_d3d_line_3d_patch_2{
    0x004E1528,
    []() {
        red::gr_d3d_num_vertices += 2;
    },
};

CodeInjection gr_d3d_line_2d_patch_1{
    0x004E10BD,
    [](auto& regs) {
        bool flush_needed = !red::gr_d3d_buffers_locked
                         || red::gr_d3d_primitive_type != D3DPT_LINELIST
                         || red::gr_d3d_max_hw_vertex + 2 > 6000
                         || red::gr_d3d_max_hw_index + red::gr_d3d_num_indices + 2 > 10000;
        if (!flush_needed) {
            xlog::trace("Skipping gr_d3d_prepare_buffers");
            regs.eip = 0x004E10C2;
        }
        else {
            xlog::trace("Line drawing requires gr_d3d_prepare_buffers {} {} {} {}",
                 red::gr_d3d_buffers_locked, red::gr_d3d_primitive_type, red::gr_d3d_max_hw_vertex,
                 red::gr_d3d_max_hw_index + red::gr_d3d_num_indices);
        }
    },
};

CallHook<void()> gr_d3d_line_2d_patch_2{
    0x004E11F2,
    []() {
        red::gr_d3d_num_vertices += 2;
    },
};

CodeInjection gr_d3d_poly_patch{
    0x004E1573,
    [](auto& regs) {
        if (red::gr_d3d_primitive_type != D3DPT_TRIANGLELIST) {
            regs.eip = 0x004E1598;
        }
    },
};

CodeInjection gr_d3d_bitmap_patch_1{
    0x004E090E,
    [](auto& regs) {
        if (red::gr_d3d_primitive_type != D3DPT_TRIANGLELIST) {
            regs.eip = 0x004E092C;
        }
    },
};

CodeInjection gr_d3d_bitmap_patch_2{
    0x004E0C97,
    [](auto& regs) {
        if (red::gr_d3d_primitive_type != D3DPT_TRIANGLELIST) {
            regs.eip = 0x004E0CBB;
        }
    },
};

CodeInjection gr_d3d_render_geometry_face_patch_1{
    0x004E18F1,
    [](auto& regs) {
        if (red::gr_d3d_primitive_type != D3DPT_TRIANGLELIST) {
            regs.eip = 0x004E1916;
        }
    },
};

CodeInjection gr_d3d_render_geometry_face_patch_2{
    0x004E1B2D,
    [](auto& regs) {
        if (red::gr_d3d_primitive_type != D3DPT_TRIANGLELIST) {
            regs.eip = 0x004E1B53;
        }
    },
};

CallHook<void(int, int, int, int, int, HWND, float, bool, int, D3DFORMAT)> gr_init_hook{
    0x00482B78,
    [](int max_w, int max_h, int bit_depth, int mode, int window_mode, HWND hwnd, float far_zvalue, bool sync_blit, int video_card, D3DFORMAT backbuffer_format) {
        max_w = GetSystemMetrics(SM_CXSCREEN);
        max_h = GetSystemMetrics(SM_CYSCREEN);
        gr_init_hook.call_target(max_w, max_h, bit_depth, mode, window_mode, hwnd, far_zvalue, sync_blit, video_card, backbuffer_format);
    },
};

CodeInjection gr_init_widescreen_patch{
    0x004B8CD1,
    []() {
        red::gr_screen.aspect = 1.0f;
    },
};

FunHook<void(red::Matrix3*, red::Vector3*, float, bool, bool)> gr_setup_3d_hook{
    0x004C5980,
    [](red::Matrix3* viewer_orient, red::Vector3* viewer_pos, float horizontal_fov, bool zbuffer_flag, bool z_scale) {
        // check if this is a perspective view
        if (z_scale) {
            // Note: RED uses h_fov value of 90 degrees in the perspective view
            // Use Hor+ scaling method to improve user experience when displayed on a widescreen
            // Assume provided FOV makes sense on a 4:3 screen
            float s = static_cast<float>(red::gr_screen.clip_width) / red::gr_screen.clip_height * 0.75f;
            constexpr float pi = 3.141592f;
            float h_fov_rad = horizontal_fov / 180.0f * pi;
            float x = std::tan(h_fov_rad / 2.0f);
            float y = x * s;
            h_fov_rad = 2.0f * std::atan(y);
            horizontal_fov = h_fov_rad / pi * 180.0f;
            // Clamp the value to avoid artifacts when view is very stretched
            horizontal_fov = std::clamp(horizontal_fov, 60.0f, 120.0f);
            //xlog::info("fov {}", horizontal_fov);
        }
        gr_setup_3d_hook.call_target(viewer_orient, viewer_pos, horizontal_fov, zbuffer_flag, z_scale);
    },
};

CodeInjection geo_cache_validate_in_render{
    0x005021a6, // MOV EAX, [ESI+0x04] — load geo_cache in render function's room loop
    [](auto& regs) {
        // Snapshot pool_cursor at the start of each render pass (first room, index 0).
        // After a pool reset, this captures pool_base, making the valid range empty
        // so ALL stale pointers are detected for the entire frame.
        auto original_esp = static_cast<uintptr_t>(regs.esp);
        auto loop_index = *reinterpret_cast<int*>(original_esp + 0x80);
        if (loop_index == 0) {
            pool_cursor_snapshot = addr_as_ref<char*>(0x01128364);
        }

        // Read geo_cache pointer from room struct
        auto* room = reinterpret_cast<char*>(static_cast<uintptr_t>(regs.esi));
        auto* geo_cache = *reinterpret_cast<char**>(room + 0x4);

        if (geo_cache) {
            auto& pool_base = addr_as_ref<char*>(0x010e8f60);
            auto gc_addr = reinterpret_cast<uintptr_t>(geo_cache);
            if (gc_addr < reinterpret_cast<uintptr_t>(pool_base) ||
                gc_addr >= reinterpret_cast<uintptr_t>(pool_cursor_snapshot)) {
                // Cache pointer is outside the valid pool range — it's stale
                *reinterpret_cast<void**>(room + 0x4) = nullptr;
                geo_cache = nullptr;
            }
        }

        // Replicate overwritten instructions:
        //   5021a6: MOV EAX, [ESI+0x04]  (3 bytes)
        //   5021a9: TEST EAX, EAX         (2 bytes)
        regs.eax = reinterpret_cast<uintptr_t>(geo_cache);
        regs.eflags.cmp(static_cast<int>(regs.eax), 0);
        regs.eip = 0x5021ab; // Resume at JZ instruction after overwritten bytes
    },
    false, // needs_trampoline = false — SubHook LDE can't parse instructions here
};

// After the post-build room loop, reset the render cache pool so that stale geo_cache
// pointers from the old world are detected by geo_cache_validate_in_render.
// This hook fires at the end of FUN_004399b0 when the world has been rebuilt.
//
// Note: We do NOT reset idx_7c/idx_80 (brush/portal processing indices). The engine's
// own virtual method call at vtable+0x58 (right after this hook) clears the brush/portal
// containers. On the next build, renumbering repopulates them with the same N items, and
// idx_7c == N == count means no reprocessing is needed — the CSG tree data is reused as-is.
CodeInjection geo_build_reset_render_cache{
    0x0043a6cd, // MOV ECX, dword ptr [0x006f9e68] — right after post-build room loop
    [](auto& regs) {
        auto& pool_cursor = addr_as_ref<void*>(0x01128364);
        auto& pool_base = addr_as_ref<void*>(0x010e8f60);
        auto& tracked_room_count = addr_as_ref<int>(0x010ac748);

        pool_cursor = pool_base;
        tracked_room_count = 0;
    },
};

CodeInjection detail_room_overflow_check{
    0x0049b757, // MOV [EAX*4+array], ESI — unbounded detail room array write
    [](auto& regs) {
        if (static_cast<int>(regs.eax) >= max_detail_rooms) {
            WARN_ONCE("Detail rooms limit reached ({}), additional detail rooms will not be rendered", max_detail_rooms);
            regs.eip = 0x0049b764; // skip write + inc + store
        }
    },
};

CodeInjection gr_d3d_init_load_library_injection{
    0x004EC50E,
    [](auto& regs) {
        extern HMODULE g_module;
        auto d3d8to9_path = get_module_dir(g_module) + "d3d8to9.dll";
        xlog::info("Loading d3d8to9.dll: {}", d3d8to9_path);
        HMODULE d3d8to9_module = LoadLibraryA(d3d8to9_path.c_str());
        if (d3d8to9_module) {
            regs.eax = d3d8to9_module;
            regs.eip = 0x004EC519;
        }
    },
};

FunHook<void(HWND)> gr_d3d_set_viewport_wnd_hook{
    0x004EB840,
    [](HWND hwnd) {
        // Original code:
        // * sets broken offset and clip size in gr_screen
        // * configures viewport using off by one window size
        // * reconfigures D3D matrices for no reason (they are unchanged)
        // * resets clipping rect and viewport short after (0x004B8E2B)
        // Rewrite it keeping only the parts that works properly and makes sense
        // Note: In all places where this code is called clip rect is manually changed after the call
        auto& gr_d3d_hwnd = addr_as_ref<HWND>(0x0183B950);
        auto& gr_d3d_wnd_client_rect = addr_as_ref<RECT>(0x0183B798);
        auto& gr_d3d_flush_buffer = addr_as_ref<void()>(0x004E99D0);
        gr_d3d_flush_buffer();
        gr_d3d_hwnd = hwnd;
        GetClientRect(hwnd, &gr_d3d_wnd_client_rect);
    },
};

void ApplyGraphicsPatches()
{
#if D3D_HW_VERTEX_PROCESSING
    // Use hardware vertex processing instead of software processing
    write_mem<u8>(0x004EC73E + 1, D3DCREATE_HARDWARE_VERTEXPROCESSING);
    write_mem<u32>(0x004EBC3D + 1, D3DUSAGE_DYNAMIC|D3DUSAGE_DONOTCLIP|D3DUSAGE_WRITEONLY);
    write_mem<u32>(0x004EBC77 + 1, D3DUSAGE_DYNAMIC|D3DUSAGE_DONOTCLIP|D3DUSAGE_WRITEONLY);
#endif

    // Avoid flushing D3D buffers in GrSetColorRgba
    AsmWriter(0x004B976D).nop(5);

    // Add Sleep if window is inactive
    frametime_calculate_hook.install();

    // Improve performance
    after_gr_init_hook.install();

    // Reduce number of draw-calls for line rendering
    AsmWriter(0x004E1335).nop(5);
    gr_d3d_line_3d_patch_1.install();
    gr_d3d_line_3d_patch_2.install();
    AsmWriter(0x004E10B4).nop(5);
    gr_d3d_line_2d_patch_1.install();
    gr_d3d_line_2d_patch_2.install();
    gr_d3d_poly_patch.install();
    gr_d3d_bitmap_patch_1.install();
    gr_d3d_bitmap_patch_2.install();
    gr_d3d_render_geometry_face_patch_1.install();
    gr_d3d_render_geometry_face_patch_2.install();

    // Fix editor not using all space for rendering when used with a big monitor
    gr_init_hook.install();

    // Fix aspect ratio on wide screens
    gr_init_widescreen_patch.install();

    // Use Hor+ FOV scaling
    gr_setup_3d_hook.install();

    // Use d3d8to9 instead of d3d8
    gr_d3d_init_load_library_injection.install();

    // Fix setting viewport window
    gr_d3d_set_viewport_wnd_hook.install();

    // Validate geo_cache pointers in the render function against pool bounds
    geo_cache_validate_in_render.install();

    // Reset render cache pool after geometry rebuild so stale geo_cache pointers are detected
    geo_build_reset_render_cache.install();

    // Expand detail rooms array from 256 entries (0x010cee5c)
    // Array base references
    write_mem_ptr(0x0049b046, detail_room_list);  // FUN_0049afb0: read loop base
    write_mem_ptr(0x0049b75a, detail_room_list);  // FUN_0049b550: array write
    // Count variable references (0x010cf25c -> &detail_room_count)
    write_mem_ptr(0x0049b037, &detail_room_count);  // FUN_0049afb0: count read
    write_mem_ptr(0x0049b0f5, &detail_room_count);  // FUN_0049afb0: loop count read
    write_mem_ptr(0x0049b717, &detail_room_count);  // FUN_0049b550: count reset
    write_mem_ptr(0x0049b74e, &detail_room_count);  // FUN_0049b550: count read before write
    write_mem_ptr(0x0049b760, &detail_room_count);  // FUN_0049b550: count store after inc
    // Bounds check (must install after write_mem_ptr so saved bytes have new addresses)
    detail_room_overflow_check.install();

    // Expand geo_cache limits for large room geometry rendering

    // Increase render cache memory pool from 8 MB to 32 MB
    write_mem<u32>(0x00482c41 + 1, 0x2000000);

    // Increase face count limit from 16384 to 65536
    write_mem<u32>(0x0049b689 + 1, 0x10000);
    write_mem<u32>(0x0049b7bd + 1, 0x10000);

    // Increase batch count limit from 512 to 1024
    write_mem<u32>(0x0049bbe5 + 2, 0x400); // in geo_cache_prepare_room
    write_mem<u32>(0x0049acb5 + 1, 0x400); // in batch data clear function

    // Redirect face list array from 16384 to 65536 entries
    static void* editor_geo_face_list[0x10000];
    write_mem_ptr(0x0049b435, editor_geo_face_list);
    write_mem_ptr(0x0049b693, editor_geo_face_list);
    write_mem_ptr(0x0049b7c7, editor_geo_face_list);
    write_mem_ptr(0x0049b85d, editor_geo_face_list);
    write_mem_ptr(0x0049b902, editor_geo_face_list);
    write_mem_ptr(0x0049b929, editor_geo_face_list);
    write_mem_ptr(0x0049ba26, editor_geo_face_list);
    write_mem_ptr(0x0049ba2c, editor_geo_face_list);
    write_mem_ptr(0x0049ba3a, editor_geo_face_list);
    write_mem_ptr(0x0049ba41, editor_geo_face_list);
    write_mem_ptr(0x0049ba55, editor_geo_face_list);
    write_mem_ptr(0x0049ba62, editor_geo_face_list);
    write_mem_ptr(0x0049baa0, editor_geo_face_list);
    write_mem_ptr(0x0049bad7, editor_geo_face_list);
    write_mem_ptr(0x0049bb39, editor_geo_face_list);
    write_mem_ptr(0x0049bc05, editor_geo_face_list);
    write_mem_ptr(0x0049bc2a, editor_geo_face_list);
    write_mem_ptr(0x0049bf29, editor_geo_face_list);
    write_mem_ptr(0x0049bf76, editor_geo_face_list);
    write_mem_ptr(0x0049c1b0, editor_geo_face_list);

    // Redirect sort key array from 16384 to 65536 entries
    static void* editor_geo_sort_keys[0x10000];
    write_mem_ptr(0x0049b43c, editor_geo_sort_keys);
    write_mem_ptr(0x0049b6c3, editor_geo_sort_keys);
    write_mem_ptr(0x0049b7f5, editor_geo_sort_keys);
    write_mem_ptr(0x0049b910, editor_geo_sort_keys);
    write_mem_ptr(0x0049ba13, editor_geo_sort_keys);
    write_mem_ptr(0x0049ba19, editor_geo_sort_keys);
    write_mem_ptr(0x0049ba48, editor_geo_sort_keys);
    write_mem_ptr(0x0049ba4e, editor_geo_sort_keys);
    write_mem_ptr(0x0049bbfe, editor_geo_sort_keys);
    write_mem_ptr(0x0049bc3a, editor_geo_sort_keys);

    // Redirect batch data array from 512 to 1024 entries (0x50 bytes each)
    static char editor_geo_batch_data[0x400 * 0x50];
    write_mem_ptr(0x0049acbd, editor_geo_batch_data);
    write_mem_ptr(0x0049bc1b, editor_geo_batch_data);
    write_mem_ptr(0x0049c2f6, editor_geo_batch_data);

    // Redirect unique vertex pointer array (expanded to 32768 entries)
    static void* editor_geo_unique_verts[0x8000];
    write_mem_ptr(0x0049bfaf, editor_geo_unique_verts);
    write_mem_ptr(0x0049bfdc, editor_geo_unique_verts);
    write_mem_ptr(0x0049c126, editor_geo_unique_verts);
    write_mem_ptr(0x0049c163, editor_geo_unique_verts);
    write_mem_ptr(0x0049c1ea, editor_geo_unique_verts);

    // Redirect vertex chain array (expanded to 32768 entries)
    static short editor_geo_vertex_chain[0x8000];
    write_mem_ptr(0x0049bfa8, editor_geo_vertex_chain);
    write_mem_ptr(0x0049c01a, editor_geo_vertex_chain);
    write_mem_ptr(0x0049c026, editor_geo_vertex_chain);
    write_mem_ptr(0x0049c034, editor_geo_vertex_chain);
    write_mem_ptr(0x0049c226, editor_geo_vertex_chain);
}
