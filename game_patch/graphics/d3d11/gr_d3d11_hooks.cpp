#include <algorithm>
#include <cmath>
#include <cstring>
#include <patch_common/AsmWriter.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <common/utils/list-utils.h>
#include <float.h>
#include "../../rf/gr/gr.h"
#include "../../rf/gr/gr_light.h"
#include "../../rf/os/os.h"
#include "../../rf/v3d.h"
#include "../../rf/character.h"
#include "../../rf/level.h"
#include "../../rf/geometry.h"
#include "../../rf/mover.h"
#include "../../bmpman/bmpman.h"
#include "../../main/main.h"
#include "../../misc/misc.h"
#include "../../misc/alpine_settings.h"
#include "gr_d3d11.h"
#include "gr_d3d11_mesh.h"

void gr_light_use_static(bool use_static);

namespace df::gr::d3d11
{
    // Gather both dynamic and static lights for GPU-lit meshes.
    // is_find_static_lights controls which linked list the internal light search
    // functions traverse, so we must call light_filter_set_solid twice to get both.
    // Check if a light's sphere overlaps a mesh's bounding sphere.
    // More accurate than point-to-point distance for large meshes where
    // the origin may be far from the nearest surface to the light.
    static bool light_overlaps_mesh(const rf::gr::Light* light, const rf::Vector3& mesh_pos, float mesh_radius)
    {
        float combined_radius = light->rad_2 + mesh_radius;
        float dist_sq = (light->vec - mesh_pos).len_sq();
        return dist_sq <= combined_radius * combined_radius;
    }

    // Local working buffer for gathered lights. Preserves dynamic-light results
    // while running the static-light pass, then overwrites the globals for GPU upload.
    static rf::gr::Light* gathered_lights[rf::gr::max_relevant_lights];

    static void gather_mesh_lights(const rf::Vector3& pos, float mesh_radius = 0.0f)
    {
        // Pass 1: dynamic lights (default is_find_static_lights=false)
        rf::gr::light_filter_set_solid(rf::level.geometry, true, false);
        int dynamic_count = std::min(rf::gr::num_relevant_lights, rf::gr::max_relevant_lights);

        // Copy dynamic lights to local buffer
        int total = 0;
        for (int i = 0; i < dynamic_count && total < rf::gr::max_relevant_lights; ++i) {
            gathered_lights[total++] = rf::gr::relevant_lights[i];
        }

        // Pass 2: static lights (requires is_find_static_lights=true)
        if (g_alpine_game_config.mesh_static_lighting) {
            rf::gr::light_filter_reset();
            gr_light_use_static(true);
            rf::gr::light_filter_set_solid(rf::level.geometry, true, true);
            gr_light_use_static(false);

            // Append static lights
            int static_count = std::min(rf::gr::num_relevant_lights, rf::gr::max_relevant_lights);
            // Insert static lights before dynamic so static don't push out dynamic
            // Shift dynamic lights right, insert static at front
            if (static_count > 0 && total > 0) {
                int space = std::min(static_count, rf::gr::max_relevant_lights - total);
                std::memmove(gathered_lights + space, gathered_lights, total * sizeof(rf::gr::Light*));
                for (int i = 0; i < space; ++i) {
                    gathered_lights[i] = rf::gr::relevant_lights[i];
                }
                total += space;
            } else {
                for (int i = 0; i < static_count && total < rf::gr::max_relevant_lights; ++i) {
                    gathered_lights[total++] = rf::gr::relevant_lights[i];
                }
            }
        }

        // Filter out negative-intensity (subtractive) lights. These are a lightmap
        // baking trick to darken areas and have no physical meaning for real-time
        // point lighting. They waste GPU slots and cause color artifacts.
        {
            int write = 0;
            for (int read = 0; read < total; ++read) {
                const auto* light = gathered_lights[read];
                if (light->r >= 0.0f && light->g >= 0.0f && light->b >= 0.0f) {
                    gathered_lights[write++] = gathered_lights[read];
                }
            }
            total = write;
        }

        // Priority sorting for the 32-light GPU limit.
        // Three tiers, each stable-partitioned to the front:
        //   1. Dynamic lights whose sphere overlaps the mesh (muzzle flashes, explosions, fire)
        //   2. Large spotlights (radius >= 30m) whose sphere overlaps the mesh
        //   3. Everything else, sorted by distance to mesh center
        // All overlap tests use sphere-sphere intersection (light radius + mesh radius)
        // to correctly handle large meshes where the origin is far from the nearest surface.
        constexpr int gpu_max = 32;
        constexpr float large_spot_radius = 30.0f;
        if (total > 1) {
            auto* begin = gathered_lights;
            auto* end = begin + total;

            // Tier 1: dynamic lights overlapping the mesh
            auto* t1_end = std::stable_partition(begin, end,
                [&pos, mesh_radius](const rf::gr::Light* light) {
                    if (!light->dynamic) return false;
                    return light_overlaps_mesh(light, pos, mesh_radius);
                }
            );

            // Tier 2: large spotlights overlapping the mesh (not already in tier 1)
            auto* t2_end = std::stable_partition(t1_end, end,
                [&pos, mesh_radius](const rf::gr::Light* light) {
                    if (light->type != rf::gr::LT_SPOT) return false;
                    if (light->rad_2 < large_spot_radius) return false;
                    return light_overlaps_mesh(light, pos, mesh_radius);
                }
            );
            int priority_count = static_cast<int>(t2_end - begin);

            // Sort the remaining (non-priority) lights by distance to mesh center
            int remaining = total - priority_count;
            if (remaining > 1) {
                int slots_left = std::max(gpu_max - priority_count, 0);
                std::partial_sort(
                    t2_end,
                    t2_end + std::min(remaining, slots_left),
                    end,
                    [&pos](const rf::gr::Light* a, const rf::gr::Light* b) {
                        float da = (a->vec - pos).len_sq();
                        float db = (b->vec - pos).len_sq();
                        return da < db;
                    }
                );
            }
        }

        // Write results back to the global arrays for the GPU upload to read
        std::memcpy(rf::gr::relevant_lights, gathered_lights, total * sizeof(rf::gr::Light*));
        rf::gr::num_relevant_lights = total;
    }

    static std::optional<Renderer> renderer;

    void update_window_mode();

    void msg_handler(UINT msg, WPARAM w_param, LPARAM l_param)
    {
        if (!renderer) {
            return;
        }
        switch (msg) {
        case WM_ACTIVATEAPP:
            if (w_param) {
                xlog::trace("active {:x} {:x}", w_param, l_param);
                renderer->window_active();
            }
            else {
                xlog::trace("inactive {:x} {:x}", w_param, l_param);
                renderer->window_inactive();
            }
            break;
        case WM_SYSKEYDOWN:
            // Handle Alt+Enter to toggle between fullscreen and windowed mode.
            // DXGI's built-in Alt+Enter handling is disabled (MakeWindowAssociation with
            // DXGI_MWA_NO_ALT_ENTER) to prevent it from calling SetFullscreenState internally,
            // which would put the swap chain in an inconsistent state and crash on the next Present().
            if (w_param == VK_RETURN) {
                auto new_mode = (rf::gr::screen.window_mode == rf::gr::FULLSCREEN)
                    ? rf::gr::WINDOWED
                    : rf::gr::FULLSCREEN;
                rf::gr::screen.window_mode = new_mode;
                update_window_mode();
            }
            break;
        }
    }

    void flip()
    {
        renderer->flip();
    }

    void close()
    {
        xlog::info("Cleaning up D3D11");
        renderer.reset();
    }

    void init(HWND hwnd)
    {
        xlog::info("Initializing D3D11");
        renderer.emplace(hwnd);
        rf::os_add_msg_handler(msg_handler);

        // set these here to prevent a crash during realtime creation or clearing of bitmaps
        // stock game sets these in gr_d3d_init_device
        rf::bm::max_d3d_texture_resolution_h = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
        rf::bm::max_d3d_texture_resolution_w = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;

        // Switch FPU to single-precision mode for backward compatibility
        // Direct3D 8/9 does it automatically unless D3DCREATE_FPU_PRESERVE flag is used,
        // but Direct3D 11 no longer does it.
        // It is needed to keep old checksums in AC
        _controlfp(_PC_24, _MCW_PC);
    }

    rf::bm::Format read_back_buffer(int x, int y, int w, int h, rf::ubyte *data)
    {
        return renderer->read_back_buffer(x, y, w, h, data);
    }

    void page_in(int bm_handle)
    {
        renderer->page_in(bm_handle);
    }

    void clear()
    {
        renderer->clear();
    }

    void bitmap(int bitmap_handle, int x, int y, int w, int h, int sx, int sy, int sw, int sh, bool flip_x, bool flip_y, rf::gr::Mode mode)
    {
        renderer->bitmap(bitmap_handle, x, y, w, h, sx, sy, sw, sh, flip_x, flip_y, mode);
    }

    void bitmap_float(int bitmap_handle, float x, float y, float w, float h, float sx, float sy, float sw, float sh, bool flip_x, bool flip_y, rf::gr::Mode mode)
    {
        renderer->bitmap(bitmap_handle, x, y, w, h, sx, sy, sw, sh, flip_x, flip_y, mode);
    }

    void set_clip()
    {
        renderer->set_clip();
    }

    void zbuffer_clear()
    {
        renderer->zbuffer_clear();
    }

    void tmapper(int nv, const rf::gr::Vertex **vertices, int vertex_attributes, rf::gr::Mode mode)
    {
        renderer->tmapper(nv, vertices, vertex_attributes, mode);
    }

    void line(float x1, float y1, float x2, float y2, rf::gr::Mode mode)
    {
        renderer->line_2d(x1, y1, x2, y2, mode);
    }

    void line_3d(const rf::gr::Vertex& v0, const rf::gr::Vertex& v1, rf::gr::Mode mode)
    {
        renderer->line_3d(v0, v1, mode);
    }

    void texture_save_cache()
    {
        renderer->texture_save_cache();
    }

    void texture_flush_cache(bool force)
    {
        renderer->texture_flush_cache(force);
    }

    void texture_mark_dirty(int bm_handle)
    {
        renderer->texture_mark_dirty(bm_handle);
    }

    bool lock(int bm_handle, int section, rf::gr::LockInfo *lock)
    {
        return renderer->lock(bm_handle, section, lock);
    }

    void unlock(rf::gr::LockInfo *lock)
    {
        renderer->unlock(lock);
    }

    void get_texel(int bm_handle, float u, float v, rf::gr::Color *clr)
    {
        renderer->get_texel(bm_handle, u, v, clr);
    }

    void texture_add_ref(int bm_handle)
    {
        renderer->texture_add_ref(bm_handle);
    }

    void texture_remove_ref(int bm_handle)
    {
        renderer->texture_remove_ref(bm_handle);
    }

    void update_texture_filtering()
    {
        if (renderer) {
            renderer->reset_sampler_states();
        }
    }

    void render_solid(rf::GSolid* solid, rf::GRoom** rooms, int num_rooms)
    {
        renderer->render_solid(solid, rooms, num_rooms);
    }

    void render_movable_solid(rf::GSolid* solid, const rf::Vector3& pos, const rf::Matrix3& orient)
    {
        // Stock gr_d3d_render_movable_solid (0x00553C60) gathers only dynamic lights
        // using the mover's own GSolid (not level.geometry). Static lights are already
        // baked into the mover's lightmap, so we must not add them as point lights.
        // The stock engine also temporarily transforms the solid's bbox to world space
        // before calling light_filter_set_solid, since it uses bbox for sphere overlap tests.
        bool lights_gathered = false;
        if (solid) {
            // Save local-space bbox
            rf::Vector3 saved_min = solid->bbox_min;
            rf::Vector3 saved_max = solid->bbox_max;

            // Transform local bbox to world-space AABB (replicates stock FUN_00539a40)
            rf::Vector3 local_center = (saved_min + saved_max) * 0.5f;
            rf::Vector3 half = (saved_max - saved_min) * 0.5f;
            rf::Vector3 world_center = pos + orient.transform_vector(local_center);
            // Compute world-space half-extents from oriented local half-extents
            rf::Vector3 world_half{
                std::abs(orient.rvec.x) * half.x + std::abs(orient.uvec.x) * half.y + std::abs(orient.fvec.x) * half.z,
                std::abs(orient.rvec.y) * half.x + std::abs(orient.uvec.y) * half.y + std::abs(orient.fvec.y) * half.z,
                std::abs(orient.rvec.z) * half.x + std::abs(orient.uvec.z) * half.y + std::abs(orient.fvec.z) * half.z,
            };
            solid->bbox_min = world_center - world_half;
            solid->bbox_max = world_center + world_half;

            rf::gr::light_filter_set_solid(solid, true, false);
            lights_gathered = true;

            // Restore local-space bbox
            solid->bbox_min = saved_min;
            solid->bbox_max = saved_max;
        }

        renderer->render_movable_solid(solid, pos, orient);

        if (lights_gathered) {
            rf::gr::light_filter_reset();
            renderer->clear_mesh_lights();
        }
    }

    void render_alpha_detail_room(rf::GRoom *room, rf::GSolid *solid)
    {
        if (renderer) {
            renderer->render_alpha_detail_room(room, solid);
        }
    }

    void render_sky_room(rf::GRoom *room)
    {
        renderer->render_sky_room(room);
    }

    void render_v3d_vif(rf::VifLodMesh *lod_mesh, [[maybe_unused]] rf::VifMesh *mesh, const rf::Vector3& pos, const rf::Matrix3& orient, int lod_index, const rf::MeshRenderParams& params)
    {
        if (lod_mesh && lod_index >= 0 && lod_index < lod_mesh->num_levels && !level_uses_vertex_lighting()) {
            bool lights_gathered = false;
            if (rf::level.geometry) {
                gather_mesh_lights(pos, lod_mesh->radius);
                lights_gathered = true;
            }

            // Ensure render cache exists before setting self-illumination.
            renderer->page_in_v3d_mesh(lod_mesh);

            // Detect fullbright batches from CPU vertex colors.
            // The stock CPU code sets vertex colors to exactly (255,255,255) for self-illuminated
            // materials via max(computed_lighting, self_illumination * 255).
            // Only update batches that don't already have self_illumination set (from v3d_page_in
            // materials path) to avoid overwriting correct values with false positives.
            if (params.vertex_colors && lod_mesh->render_cache) {
                auto* cache = reinterpret_cast<BaseMeshRenderCache*>(lod_mesh->render_cache);
                auto& batches = const_cast<std::vector<BaseMeshRenderCache::Batch>&>(cache->get_batches(lod_index));
                rf::VifMesh* vif_mesh_lod = lod_mesh->meshes[lod_index];
                if (vif_mesh_lod) {
                    int vertex_offset = 0;
                    for (int ci = 0; ci < vif_mesh_lod->num_chunks && ci < static_cast<int>(batches.size()); ++ci) {
                        if (batches[ci].self_illumination == 0.0f) {
                            const rf::ubyte* vc = params.vertex_colors + vertex_offset * 3;
                            if (vc[0] == 255 && vc[1] == 255 && vc[2] == 255) {
                                batches[ci].self_illumination = 1.0f;
                            }
                        }
                        vertex_offset += vif_mesh_lod->chunks[ci].num_vecs;
                    }
                }
            }

            // Clear CPU vertex colors so the GPU shader handles all lighting.
            rf::MeshRenderParams params_no_cpu_lighting = params;
            params_no_cpu_lighting.vertex_colors = nullptr;

            renderer->render_v3d_vif(lod_mesh, lod_index, pos, orient, params_no_cpu_lighting, true);

            if (lights_gathered) {
                rf::gr::light_filter_reset();
                renderer->clear_mesh_lights();
            }
            return;
        }

        renderer->render_v3d_vif(lod_mesh, lod_index, pos, orient, params);
    }

    void render_character_vif(rf::VifLodMesh *lod_mesh, [[maybe_unused]] rf::VifMesh *mesh, const rf::Vector3& pos, const rf::Matrix3& orient, const rf::CharacterInstance *ci, int lod_index, const rf::MeshRenderParams& params)
    {
        bool use_vertex_lighting = level_uses_vertex_lighting();

        if (lod_mesh && lod_index >= 0 && lod_index < lod_mesh->num_levels) {
            // Gather nearby lights (both static and dynamic) so the pixel shader can
            // compute per-pixel N·L lighting for this character mesh.
            // Skip when using vertex lighting — the old path doesn't need gathered lights.
            bool is_first_person = (params.flags & rf::MeshRenderFlags::MRF_FIRST_PERSON) != 0;
            bool lights_gathered = false;
            if (!use_vertex_lighting && rf::level.geometry) {
                gather_mesh_lights(pos, lod_mesh->radius);
                lights_gathered = true;
            }

            bool fullbright_character = g_character_meshes_are_fullbright && !is_first_person;
            bool synthesize_colors = params.vertex_colors == nullptr || fullbright_character;

            if (synthesize_colors) {
                rf::MeshRenderParams params_with_vertex_colors = params;

                rf::VifMesh* lod_mesh_level = lod_mesh->meshes[lod_index];
                int total_vertices = 0;
                for (int chunk_index = 0; chunk_index < lod_mesh_level->num_chunks; ++chunk_index) {
                    total_vertices += lod_mesh_level->chunks[chunk_index].num_vecs;
                }

                struct ScratchVertexColors
                {
                    std::vector<rf::ubyte> data;
                    rf::Color last_color{0, 0, 0, 255};
                    std::size_t last_vertex_count = 0;
                };

                static thread_local ScratchVertexColors scratch_vertex_colors;
                scratch_vertex_colors.data.resize(static_cast<std::size_t>(total_vertices) * 3);

                rf::Color base_color{255, 255, 255, 255};

                if (use_vertex_lighting && !fullbright_character) {
                    // Old (master) CPU ambient calculation: scale + bias the entity's
                    // lightmap-sampled ambient color into vertex colors.
                    float ambient_r = static_cast<float>(params.ambient_color.red);
                    float ambient_g = static_cast<float>(params.ambient_color.green);
                    float ambient_b = static_cast<float>(params.ambient_color.blue);

                    // White ambient means no lightmap data — guess from level ambient
                    if (ambient_r == 255 && ambient_g == 255 && ambient_b == 255) {
                        ambient_r = static_cast<float>(rf::level.ambient_light.red) + 64.0f;
                        ambient_g = static_cast<float>(rf::level.ambient_light.green) + 64.0f;
                        ambient_b = static_cast<float>(rf::level.ambient_light.blue) + 64.0f;
                    }

                    constexpr float scale = 1.5f;
                    constexpr float bias = 32.0f;
                    base_color.red = static_cast<rf::ubyte>(std::clamp(ambient_r * scale + bias, 0.0f, 255.0f));
                    base_color.green = static_cast<rf::ubyte>(std::clamp(ambient_g * scale + bias, 0.0f, 255.0f));
                    base_color.blue = static_cast<rf::ubyte>(std::clamp(ambient_b * scale + bias, 0.0f, 255.0f));
                }
                // else: enhanced lighting uses neutral white — shader handles all lighting

                bool color_changed =
                    scratch_vertex_colors.last_color.red != base_color.red ||
                    scratch_vertex_colors.last_color.green != base_color.green ||
                    scratch_vertex_colors.last_color.blue != base_color.blue ||
                    scratch_vertex_colors.last_vertex_count != static_cast<std::size_t>(total_vertices);

                if (color_changed) {
                    for (std::size_t i = 0; i < scratch_vertex_colors.data.size(); i += 3) {
                        scratch_vertex_colors.data[i] = base_color.red;
                        scratch_vertex_colors.data[i + 1] = base_color.green;
                        scratch_vertex_colors.data[i + 2] = base_color.blue;
                    }
                    scratch_vertex_colors.last_color = base_color;
                    scratch_vertex_colors.last_vertex_count = static_cast<std::size_t>(total_vertices);
                }

                params_with_vertex_colors.vertex_colors = scratch_vertex_colors.data.data();
                renderer->render_character_vif(lod_mesh, lod_index, pos, orient, ci, params_with_vertex_colors, !use_vertex_lighting);

                if (lights_gathered) {
                    rf::gr::light_filter_reset();
                    renderer->clear_mesh_lights();
                }
                return;
            }

            renderer->render_character_vif(lod_mesh, lod_index, pos, orient, ci, params);

            if (lights_gathered) {
                rf::gr::light_filter_reset();
                renderer->clear_mesh_lights();
            }
            return;
        }

        renderer->render_character_vif(lod_mesh, lod_index, pos, orient, ci, params);
    }

    void fog_set()
    {
        renderer->fog_set();
    }

    bool set_render_target(int bm_handle)
    {
        return renderer->set_render_target(bm_handle);
    }

    void clear_solid_render_cache()
    {
        if (renderer) {
            renderer->clear_solid_cache();
        }
    }

    void reset_solid_render_cache_after_boolean()
    {
        if (renderer) {
            renderer->reset_solid_cache_after_boolean();
        }
    }

    void delete_texture(int bm_handle)
    {
        renderer->texture_mark_dirty(bm_handle);
    }

    void update_window_mode()
    {
        renderer->set_fullscreen_state(rf::gr::screen.window_mode == rf::gr::FULLSCREEN);
    }

    rf::ubyte project_vertex_new(Vertex* v)
    {
        renderer->project_vertex(v);
        return v->flags;
    }

    bool poly(int nv, rf::gr::Vertex** vertices, int vertex_attributes, rf::gr::Mode mode, bool constant_sw, float sw)
    {
        return renderer->poly(nv, vertices, vertex_attributes, mode, constant_sw, sw);
    }

    static CodeInjection g_render_room_objects_render_liquid_injection{
        0x004D4106,
        [](auto& regs) {
            rf::GRoom* room = regs.edi;
            rf::GSolid* solid = regs.ebx;
            renderer->render_room_liquid_surface(solid, room);
            regs.eip = 0x004D414F;
        },
    };

    static CodeInjection gr_d3d_setup_3d_injection{
        0x005473E4,
        []() {
            float sx = matrix_scale.x / matrix_scale.z;
            float sy = matrix_scale.y / matrix_scale.z;
            static auto& zm = addr_as_ref<float>(0x005A7DD8);
            float zn = 0.1f; // static near plane (RF uses: zm / matrix_scale.z)
            zm = 1.0f; // let's not use zm at all to simplify software projections
            float zf = rf::level.distance_fog_far_clip > 0.0f ? rf::level.distance_fog_far_clip : 1700.0f;
            renderer->setup_3d(Projection{sx, sy, zn, zf});
        },
    };

    static CodeInjection gr_d3d_setup_fustrum_injection{
        0x00546A40,
        []() {
            // Glares and volumetric lights use doubled far clip plane
            // To avoid using "user clip planes" disable depth clipping
            // Frustum culling should be enough
            static auto& use_far_clip = addr_as_ref<bool>(0x01818B65);
            static auto& far_clip_dist = addr_as_ref<float>(0x01818B68);
            float z_far = renderer->z_far();
            bool depth_clip_enable = use_far_clip && far_clip_dist < z_far * 1.1f;
            renderer->set_far_clip(depth_clip_enable);
        },
    };

    static CodeInjection vif_lod_mesh_ctor_injection{
        0x00569D00,
        [](auto& regs) {
            rf::VifLodMesh* lod_mesh = regs.ecx;
            lod_mesh->render_cache = nullptr;
        },
    };

    static CodeInjection vif_lod_mesh_destroy_injection{
        0x005695D0,
        [](auto& regs) {
            const auto lod_mesh = addr_as_ref<rf::VifLodMesh*>(regs.esp + 4);
            if (renderer) {
                renderer->clear_vif_cache(lod_mesh);
            }
        },
    };


    static CodeInjection v3d_page_in_injection{
        0x0053C1B9,
        [](auto& regs) {
            rf::V3d* v3d = regs.ecx;
            if (renderer && v3d->num_meshes > 0 && v3d->meshes[0].vu) {
                auto* materials = reinterpret_cast<rf::MeshMaterial*>(v3d->meshes[0].materials);
                int num_materials = v3d->meshes[0].num_materials;
                renderer->page_in_v3d_mesh(v3d->meshes[0].vu,
                    (num_materials > 0) ? materials : nullptr, num_materials);
            }
        },
    };

    static CodeInjection character_instance_page_in_injection{
        0x0051C4C0,
        [](auto& regs) {
            rf::CharacterInstance* ci = regs.ecx;
            if (renderer) {
                renderer->page_in_character_mesh(ci->base_character->character_meshes[0].mesh->vu);
            }
        },
    };

    void set_pow2_tex_active(bool active)
    {
        if (renderer) {
            renderer->set_pow2_tex_active(active);
        }
    }

    static CodeInjection level_page_in_injection{
        0x0045CC20,
        []() {
            if (renderer) {
                renderer->page_in_solid(rf::level.geometry);
                for (rf::MoverBrush& mb : DoublyLinkedList{rf::mover_brush_list}) {
                    renderer->page_in_movable_solid(mb.geometry);
                }
                renderer->reset_static_vertex_color_tracking();
            }
        },
    };

    static CodeInjection level_page_out_injection{
        0x0045CB83,
        []() {
            if (renderer) {
                renderer->flush_caches();
            }
            clear_entity_ambient_cache();
        },
    };

    // Hook blob shadow rendering: allow blob shadows only when D3D11 shadow quality is 0
    static CallHook<void()> obj_shadow_render_all_hook{
        0x00432021,
        []() {
            if (g_alpine_game_config.shadow_quality > 0) {
                return; // D3D11 shadow mapping handles shadows at quality > 0
            }
            obj_shadow_render_all_hook.call_target();
        },
    };
}

void gr_d3d11_apply_patch()
{
    using namespace df::gr::d3d11;

    g_render_room_objects_render_liquid_injection.install();
    gr_d3d_setup_3d_injection.install();
    gr_d3d_setup_fustrum_injection.install();
    vif_lod_mesh_ctor_injection.install();
    vif_lod_mesh_destroy_injection.install();
    v3d_page_in_injection.install();
    character_instance_page_in_injection.install();
    level_page_in_injection.install();
    level_page_out_injection.install();

    // Do not use built-in render cache
    AsmWriter{0x004F0B90}.jmp(clear_solid_render_cache); // g_render_cache_clear
    AsmWriter{0x004F0B20}.ret(); // g_render_cache_init
    AsmWriter{0x004F0BD0}.jmp(reset_solid_render_cache_after_boolean); // g_render_cache_reset_after_boolean

    using namespace asm_regs;
    AsmWriter{0x00544FC0}.jmp(flip); // gr_d3d_flip
    AsmWriter{0x00545230}.jmp(close); // gr_d3d_close
    AsmWriter{0x00545960}.jmp(init); // gr_d3d_init
    AsmWriter{0x00546730}.jmp(read_back_buffer); // gr_d3d_read_backbuffer
    AsmWriter{0x005468C0}.jmp(fog_set); // gr_d3d_fog_set
    AsmWriter{0x00546A00}.mov(al, 1).ret(); // gr_d3d_is_mode_supported
    // AsmWriter{0x00546A40}.ret(); // gr_d3d_setup_frustum
    // AsmWriter{0x00546F60}.ret(); // gr_d3d_change_frustum
    // AsmWriter{0x00547150}.ret(); // gr_d3d_setup_3d
    // AsmWriter{0x005473F0}.ret(); // gr_d3d_start_instance
    // AsmWriter{0x00547540}.ret(); // gr_d3d_stop_instance
    AsmWriter{0x005477A0}.jmp(project_vertex_new); // gr_d3d_project_vertex
    // AsmWriter{0x005478F0}.ret(); // gr_d3d_is_normal_facing
    // AsmWriter{0x00547960}.ret(); // gr_d3d_is_normal_facing_plane
    // AsmWriter{0x005479B0}.ret(); // gr_d3d_get_apparent_distance_from_camera
    // AsmWriter{0x005479D0}.ret(); // gr_d3d_screen_coords_from_world_coords
    AsmWriter{0x00547A60}.ret(); // gr_d3d_update_gamma_ramp
    AsmWriter{0x00547AC0}.ret(); // gr_d3d_set_texture_mip_filter
    AsmWriter{0x00550820}.jmp(page_in); // gr_d3d_page_in
    AsmWriter{0x005508C0}.jmp(clear); // gr_d3d_clear
    AsmWriter{0x00550980}.jmp(zbuffer_clear); // gr_d3d_zbuffer_clear
    AsmWriter{0x00550A30}.jmp(set_clip); // gr_d3d_set_clip
    AsmWriter{0x00550AA0}.jmp(bitmap); // gr_d3d_bitmap
    AsmWriter{0x00551450}.ret(); // gr_d3d_flush_after_color_change
    AsmWriter{0x00551460}.jmp(line); // gr_d3d_line
    AsmWriter{0x00551900}.jmp(tmapper); // gr_d3d_tmapper
    AsmWriter{0x005536C0}.jmp(render_sky_room);
    AsmWriter{0x00553C60}.jmp(render_movable_solid); // gr_d3d_render_movable_solid - uses gr_d3d_render_face_list
    // AsmWriter{0x00553EE0}.ret(); // gr_d3d_vfx - uses gr_poly
    // AsmWriter{0x00554BF0}.ret(); // gr_d3d_vfx_facing - uses gr_d3d_3d_bitmap_angle, gr_d3d_render_volumetric_light
    // AsmWriter{0x00555080}.ret(); // gr_d3d_vfx_glow - uses gr_d3d_3d_bitmap_angle
    // AsmWriter{0x00555100}.ret(); // gr_d3d_line_vertex
    AsmWriter{0x005516E0}.jmp(line_3d); // gr_d3d_line_vertex_internal
    // AsmWriter{0x005551E0}.ret(); // gr_d3d_line_vec - uses gr_d3d_line_vertex
    // AsmWriter{0x00555790}.ret(); // gr_d3d_3d_bitmap - uses gr_poly
    // AsmWriter{0x00555AC0}.ret(); // gr_d3d_3d_bitmap_angle - uses gr_poly
    // AsmWriter{0x00555B20}.ret(); // gr_d3d_3d_bitmap_angle_wh - uses gr_poly
    // AsmWriter{0x00555B80}.ret(); // gr_d3d_render_volumetric_light - uses gr_poly
    // AsmWriter{0x00555DC0}.ret(); // gr_d3d_laser - uses gr_tmapper
    // AsmWriter{0x005563F0}.ret(); // gr_d3d_cylinder - uses gr_line
    // AsmWriter{0x005565D0}.ret(); // gr_d3d_cone - uses gr_line
    // AsmWriter{0x005566E0}.ret(); // gr_d3d_sphere - uses gr_line
    // AsmWriter{0x00556AB0}.ret(); // gr_d3d_chain - uses gr_poly
    // AsmWriter{0x00556F50}.ret(); // gr_d3d_line_directed - uses gr_line_vertex
    // AsmWriter{0x005571F0}.ret(); // gr_d3d_line_arrow - uses gr_line_vertex
    // AsmWriter{0x00557460}.ret(); // gr_d3d_render_particle_sys_particle - uses gr_poly, gr_3d_bitmap_angle
    // AsmWriter{0x00557D40}.ret(); // gr_d3d_render_bolts - uses gr_poly, gr_line
    // AsmWriter{0x00558320}.ret(); // gr_d3d_render_geomod_debris - uses gr_poly
    // AsmWriter{0x00558450}.ret(); // gr_d3d_render_glass_shard - uses gr_poly
    AsmWriter{0x00558550}.ret(); // gr_d3d_render_face_wireframe
    // AsmWriter{0x005585F0}.ret(); // gr_d3d_render_weapon_tracer - uses gr_poly
    AsmWriter{0x005587C0}.jmp(poly); // gr_d3d_poly
    AsmWriter{0x00558920}.ret(); // gr_d3d_render_geometry_wireframe
    AsmWriter{0x00558960}.ret(); // gr_d3d_render_geometry_in_editor
    AsmWriter{0x00558C40}.ret(); // gr_d3d_render_sel_face_in_editor
    // AsmWriter{0x00558D40}.ret(); // gr_d3d_world_poly - uses gr_d3d_poly
    // AsmWriter{0x00558E30}.ret(); // gr_d3d_3d_bitmap_stretched_square - uses gr_d3d_world_poly
    // AsmWriter{0x005590F0}.ret(); // gr_d3d_rod - uses gr_d3d_world_poly
    AsmWriter{0x005596C0}.ret(); // gr_d3d_render_face_list_colored
    AsmWriter{0x0055B520}.jmp(texture_save_cache); // gr_d3d_texture_save_cache
    AsmWriter{0x0055B550}.jmp(texture_flush_cache); // gr_d3d_texture_flush_cache
    AsmWriter{0x0055CDC0}.jmp(texture_mark_dirty); // gr_d3d_texture_mark_dirty
    AsmWriter{0x0055CE00}.jmp(lock); // gr_d3d_lock
    AsmWriter{0x0055CF60}.jmp(unlock); // gr_d3d_unlock
    AsmWriter{0x0055CFA0}.jmp(get_texel); // gr_d3d_get_texel
    AsmWriter{0x0055D160}.jmp(texture_add_ref); // gr_d3d_texture_add_ref
    AsmWriter{0x0055D190}.jmp(texture_remove_ref); // gr_d3d_texture_remove_ref
    AsmWriter{0x0055F5E0}.jmp(render_solid); // gr_d3d_render_static_solid
    AsmWriter{0x00561650}.ret(); // gr_d3d_render_face_list
    // AsmWriter{0x0052FA40}.jmp(render_lod_vif); // gr_d3d_render_vif_mesh
    AsmWriter{0x0052DE10}.jmp(render_v3d_vif); // gr_d3d_render_v3d_vif
    AsmWriter{0x0052E9E0}.jmp(render_character_vif); // gr_d3d_render_character_vif
    AsmWriter{0x004D34D0}.jmp(render_alpha_detail_room); // room_render_alpha_detail
    AsmWriter{0x0054F160}.ret(); // gr_d3d_set_state

    // Hook blob shadow rendering: skip when D3D11 shadow mapping is active (quality > 0)
    obj_shadow_render_all_hook.install();

    // Change size of standard structures
    write_mem<int8_t>(0x00569884 + 1, sizeof(rf::VifMesh));
    write_mem<int8_t>(0x00569732 + 1, sizeof(rf::VifLodMesh));
}
