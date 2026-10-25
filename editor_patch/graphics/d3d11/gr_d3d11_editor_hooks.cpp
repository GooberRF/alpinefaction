#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <source_location>
#include <string>
#include <vector>
#include <patch_common/AsmWriter.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include <xlog/xlog.h>
#include <common/gr_d3d11/gr_d3d11_mesh.h>
#include <common/scope_guard.h>
#include "../../brush_preview.h"
#include "../../editor_settings.h"
#include "../../headless_bake.h"
#include "../editor_gr.h"
#include "gr_d3d11_editor.h"
#include "gr_d3d11_editor_hooks.h"
#include "gr_d3d11_editor_lightmaps.h"

namespace gr::d3d11
{
    namespace
    {
        std::optional<EditorRenderer> g_renderer;

        // Null before gr_init, after gr_close, and once the device is lost: nothing more reaches the GPU
        EditorRenderer* live_renderer()
        {
            return g_renderer && !g_renderer->stopped() ? &*g_renderer : nullptr;
        }

        // The point lights of a GPU-lit mesh, left in relevant_lights and ordered as in the game. RED's light search
        // reads the editor flag (0x010A7810) where the game reads its static-light switch, so one pass finds the
        // static and the dynamic lights.
        bool gather_mesh_lights(const rf::Vector3& pos, float mesh_radius)
        {
            rf::GSolid* const solid = engine::level_solid();
            if (!solid) {
                return false;
            }
            rf::gr::light_filter_set_solid(solid, true, true);
            const int found = std::clamp(rf::gr::num_relevant_lights, 0, max_scene_lights);
            rf::gr::num_relevant_lights = prioritize_mesh_lights(rf::gr::relevant_lights, found, pos, mesh_radius);
            return true;
        }

        // The lights of the mesh about to draw, true when they have to be released after it. View > Fullbright Meshes
        // leaves none, and the core then draws it unlit (static_meshes_fullbright, character_meshes_fullbright).
        bool begin_mesh_lights(EditorRenderer& r, const rf::Vector3& pos, const rf::Vector3& center, float radius)
        {
            if (g_editor_settings.fullbright_meshes) {
                rf::gr::light_filter_reset();
                return false;
            }
            if (!gather_mesh_lights(pos, radius)) {
                return false;
            }
            r.set_mesh_bounds(center, radius);
            return true;
        }

        void flip()
        {
            if (EditorRenderer* r = live_renderer()) {
                r->flip();
            }
        }

        void close()
        {
            xlog::info("[D3D11] Cleaning up");
            g_renderer.reset();
        }

        void init([[maybe_unused]] HWND hwnd)
        {
            xlog::info("[D3D11] Initializing");
            g_renderer.emplace(headless_bake_active());
            // What the stock device setup (0x004EC8B0) would have set
            red::gr_screen.max_tex_width = static_cast<int>(g_renderer->max_texture_size());
            red::gr_screen.max_tex_height = static_cast<int>(g_renderer->max_texture_size());
            rf::gr::screen.depthbuffer_type = rf::gr::DEPTHBUFFER_Z;
            gr_needs_software_clipping = false;
            // Direct3D 8 set single precision on the creating thread, and the lightmap bake runs on it
            editor_gr_set_bake_precision();
        }

        void set_viewport_wnd(HWND hwnd)
        {
            if (EditorRenderer* r = live_renderer()) {
                r->set_viewport_wnd(hwnd);
            }
        }

        rf::bm::Format read_back_buffer(int x, int y, int w, int h, rf::ubyte* data)
        {
            EditorRenderer* r = live_renderer();
            return r ? r->read_back_buffer(x, y, w, h, data) : rf::bm::FORMAT_NONE;
        }

        void fog_set()
        {
            if (EditorRenderer* r = live_renderer()) {
                r->fog_set();
            }
        }

        rf::ubyte project_vertex(rf::gr::Vertex* v)
        {
            return g_renderer ? g_renderer->project_vertex(v) : v->flags;
        }

        void page_in(int bm_handle)
        {
            if (EditorRenderer* r = live_renderer()) {
                r->page_in(bm_handle);
            }
        }

        void clear()
        {
            if (EditorRenderer* r = live_renderer()) {
                r->clear();
            }
        }

        void zbuffer_clear()
        {
            if (EditorRenderer* r = live_renderer()) {
                r->zbuffer_clear();
            }
        }

        void set_clip()
        {
            if (EditorRenderer* r = live_renderer()) {
                r->set_clip();
            }
        }

        void bitmap(int bm_handle, int x, int y, int w, int h, int sx, int sy, int sw, int sh, bool flip_x, bool flip_y,
                    rf::gr::Mode mode)
        {
            if (EditorRenderer* r = live_renderer()) {
                r->bitmap(bm_handle, x, y, w, h, sx, sy, sw, sh, flip_x, flip_y, mode);
            }
        }

        void line(float x1, float y1, float x2, float y2, rf::gr::Mode mode)
        {
            if (EditorRenderer* r = live_renderer()) {
                r->line_2d(x1, y1, x2, y2, mode);
            }
        }

        void line_3d(const rf::gr::Vertex& v0, const rf::gr::Vertex& v1, rf::gr::Mode mode)
        {
            if (EditorRenderer* r = live_renderer()) {
                r->line_3d(v0, v1, mode);
            }
        }

        void tmapper(int nv, const rf::gr::Vertex** vertices, int vertex_attributes, rf::gr::Mode mode)
        {
            if (EditorRenderer* r = live_renderer()) {
                r->tmapper(nv, vertices, vertex_attributes, mode);
            }
        }

        bool poly(int nv, rf::gr::Vertex** vertices, int vertex_attributes, rf::gr::Mode mode, bool constant_sw,
                  float sw)
        {
            EditorRenderer* r = live_renderer();
            return r && r->poly(nv, vertices, vertex_attributes, mode, constant_sw, sw);
        }

        // 0x004E1880: a face in one texture stage, its vertices rotated by the caller (0x004E94B0, 0x004E9300)
        bool render_face(rf::GSolid* solid, rf::GFace* face, rf::gr::Mode mode, int bm_handle,
                         [[maybe_unused]] int clip_or)
        {
            EditorRenderer* r = live_renderer();
            return r && r->face(solid, face, mode, bm_handle);
        }

        // 0x004E1C90: a face with its lightmap in the second stage, then its decals. Its dynamic-light pass needs face
        // flag 0x40000, which RED never sets while the editor flag 0x010A7810 is on.
        bool render_lightmapped_face(rf::GSolid* solid, rf::GFace* face, rf::gr::Mode mode, int bm_handle,
                                     int lm_handle, [[maybe_unused]] int clip_or)
        {
            EditorRenderer* r = live_renderer();
            return r && r->lightmapped_face(solid, face, mode, bm_handle, lm_handle);
        }

        // 0x004F5D10, which binds a stage's texture: draws take their textures with them, so only its result and the
        // section scale it reports remain
        int set_stage_texture([[maybe_unused]] int stage, int bm_handle, [[maybe_unused]] int section, float* u_scale,
                              float* v_scale, [[maybe_unused]] char bind)
        {
            if (bm_handle >= 0) {
                if (u_scale) {
                    *u_scale = 1.0f;
                }
                if (v_scale) {
                    *v_scale = 1.0f;
                }
            }
            return 1;
        }

        void flush_batches()
        {
            if (EditorRenderer* r = live_renderer()) {
                r->flush_batches();
            }
        }

        // bm_release's free of a bitmap's entry (cdecl, the entry index). The next bitmap the slot holds must not find
        // this one's texture. bm_init's frees (0x004BB6C4) run before any texture exists.
        CallHook<void(int)> bm_free_entry_hook{
            0x004BDEFE,
            [](int index) {
                if (EditorRenderer* r = live_renderer()) {
                    r->texture_free(rf::bm::bitmaps[index].handle);
                }
                bm_free_entry_hook.call_target(index);
            },
        };

        void texture_save_cache()
        {
            if (EditorRenderer* r = live_renderer()) {
                r->texture_save_cache();
            }
        }

        void texture_flush_cache(bool force)
        {
            if (EditorRenderer* r = live_renderer()) {
                r->texture_flush_cache(force);
            }
        }

        void texture_mark_dirty(int bm_handle)
        {
            if (EditorRenderer* r = live_renderer()) {
                r->texture_mark_dirty(bm_handle);
            }
        }

        bool lock(int bm_handle, int section, rf::gr::LockInfo* lock)
        {
            EditorRenderer* r = live_renderer();
            return r && r->lock(bm_handle, section, lock);
        }

        void unlock(rf::gr::LockInfo* lock)
        {
            if (EditorRenderer* r = live_renderer()) {
                r->unlock(lock);
            }
        }

        void get_texel(int bm_handle, float u, float v, rf::gr::Color* clr)
        {
            if (EditorRenderer* r = live_renderer()) {
                r->get_texel(bm_handle, u, v, clr);
            }
        }

        void texture_add_ref(int bm_handle)
        {
            if (EditorRenderer* r = live_renderer()) {
                r->texture_add_ref(bm_handle);
            }
        }

        void texture_remove_ref(int bm_handle)
        {
            if (EditorRenderer* r = live_renderer()) {
                r->texture_remove_ref(bm_handle);
            }
        }

        void render_solid(rf::GSolid* solid, rf::GRoom** rooms, int num_rooms)
        {
            EditorRenderer* r = live_renderer();
            if (r && solid && rooms && num_rooms > 0) {
                r->render_solid(solid, rooms, num_rooms);
            }
        }

        void render_sky_room(rf::GRoom* room)
        {
            EditorRenderer* r = live_renderer();
            if (r && room && engine::level_solid()) {
                r->render_sky_room(room);
            }
        }

        void render_alpha_detail_room(rf::GRoom* room, rf::GSolid* solid)
        {
            EditorRenderer* r = live_renderer();
            if (r && room && solid) {
                r->render_alpha_detail_room(room, solid);
            }
        }

        void clear_solid_render_cache()
        {
            if (g_renderer) {
                g_renderer->clear_solid_cache();
            }
        }

        void reset_solid_render_cache_after_boolean()
        {
            if (g_renderer) {
                g_renderer->reset_solid_cache_after_boolean();
            }
        }

        // The game's detection of self-illuminated chunks: stock lighting writes exactly white for them
        void mark_fullbright_batches(rf::VifLodMesh* lod_mesh, int lod_index, const rf::MeshRenderParams& params)
        {
            const rf::VifMesh* mesh = lod_mesh->meshes[lod_index];
            if (params.vertex_colors && lod_mesh->render_cache && mesh) {
                static_cast<BaseMeshRenderCache*>(lod_mesh->render_cache)
                    ->mark_fullbright_batches(lod_index, *mesh, params.vertex_colors);
            }
        }

        void render_v3d_vif(rf::VifLodMesh* lod_mesh, [[maybe_unused]] rf::VifMesh* mesh, const rf::Vector3& pos,
                            const rf::Matrix3& orient, int lod_index, const rf::MeshRenderParams& params)
        {
            EditorRenderer* r = live_renderer();
            if (!r || !lod_mesh || lod_index < 0 || lod_index >= lod_mesh->num_levels) {
                return;
            }
            // A scaled orient scales the mesh's extent too
            const float radius = lod_mesh->radius * orient.rvec.len();
            const bool lights_gathered =
                begin_mesh_lights(*r, pos, pos + orient.transform_vector(lod_mesh->center), radius);
            r->page_in_v3d_mesh(lod_mesh, nullptr, 0);
            mark_fullbright_batches(lod_mesh, lod_index, params);
            // The pixel shader lights the mesh instead of RED's vertex colours
            rf::MeshRenderParams gpu_params = params;
            gpu_params.vertex_colors = nullptr;
            r->render_v3d_vif(lod_mesh, lod_index, pos, orient, gpu_params, true);
            if (lights_gathered) {
                rf::gr::light_filter_reset();
                r->clear_mesh_lights();
            }
        }

        void render_character_vif(rf::VifLodMesh* lod_mesh, [[maybe_unused]] rf::VifMesh* mesh,
                                  const rf::Vector3& pos, const rf::Matrix3& orient, const rf::CharacterInstance* ci,
                                  int lod_index, const rf::MeshRenderParams& params)
        {
            EditorRenderer* r = live_renderer();
            if (!r || !lod_mesh || !ci || lod_index < 0 || lod_index >= lod_mesh->num_levels
                || !lod_mesh->meshes[lod_index]) {
                return;
            }
            const float radius = lod_mesh->radius * orient.rvec.len();
            const bool lights_gathered =
                begin_mesh_lights(*r, pos, pos + orient.transform_vector(lod_mesh->center), radius);
            // White vertex colours: the pixel shader does all the lighting, as in the game
            const rf::VifMesh* lod = lod_mesh->meshes[lod_index];
            std::size_t total_vertices = 0;
            for (int i = 0; i < lod->num_chunks; ++i) {
                total_vertices += lod->chunks[i].num_vecs;
            }
            static std::vector<rf::ubyte> white_vertex_colors;
            if (white_vertex_colors.size() < total_vertices * 3) {
                white_vertex_colors.resize(total_vertices * 3, 255);
            }
            rf::MeshRenderParams gpu_params = params;
            gpu_params.vertex_colors = white_vertex_colors.data();
            r->render_character_vif(lod_mesh, lod_index, pos, orient, ci, gpu_params, true);
            if (lights_gathered) {
                rf::gr::light_filter_reset();
                r->clear_mesh_lights();
            }
        }

        // VifLodMesh constructor: the render cache pointer past the stock structure starts empty
        CodeInjection vif_lod_mesh_ctor_injection{
            0x0050DA20,
            [](auto& regs) {
                rf::VifLodMesh* lod_mesh = regs.ecx;
                lod_mesh->render_cache = nullptr;
            },
        };

        CodeInjection vif_lod_mesh_destroy_injection{
            0x0050D2F0,
            [](auto& regs) {
                auto* const lod_mesh = addr_as_ref<rf::VifLodMesh*>(regs.esp + 4);
                if (g_renderer && lod_mesh) {
                    g_renderer->clear_vif_cache(lod_mesh);
                }
            },
        };

        // V3d page-in (an empty stub), reached from VMesh::page_in 0x004BEE30 with the V3d in ECX
        CodeInjection v3d_page_in_injection{
            0x004EFDA9,
            [](auto& regs) {
                rf::V3d* v3d = regs.ecx;
                EditorRenderer* r = live_renderer();
                if (r && v3d && v3d->num_meshes > 0 && v3d->meshes[0].vu) {
                    const int num_materials = v3d->meshes[0].num_materials;
                    auto* materials = reinterpret_cast<rf::MeshMaterial*>(v3d->meshes[0].materials);
                    r->page_in_v3d_mesh(v3d->meshes[0].vu, num_materials > 0 ? materials : nullptr, num_materials);
                }
            },
        };

        // CharacterInstance page-in (a RET stub), reached from VMesh::page_in with the instance in ECX
        CodeInjection character_instance_page_in_injection{
            0x004DC390,
            [](auto& regs) {
                rf::CharacterInstance* ci = regs.ecx;
                EditorRenderer* r = live_renderer();
                if (!r || !ci || !ci->base_character || ci->base_character->num_character_meshes < 1) {
                    return;
                }
                rf::V3dMesh* mesh = ci->base_character->character_meshes[0].mesh;
                if (mesh && mesh->vu) {
                    r->page_in_character_mesh(mesh->vu);
                }
            },
        };

        // The room objects pass (0x00424F50, the portal render modes) draws a room's liquid surfaces through
        // render_face_list, which draws nothing here: they come from the room's cache instead, as in the game
        // (RF 0x004D4106). EDI is the room, EBX the solid.
        CodeInjection room_objects_liquid_injection{
            0x0042545A,
            [](auto& regs) {
                rf::GRoom* room = regs.edi;
                rf::GSolid* solid = regs.ebx;
                EditorRenderer* r = live_renderer();
                if (r && room && solid) {
                    r->render_room_liquid_surface(solid, room);
                }
                regs.eip = 0x004254A3;
            },
        };

        // ~CDedLevel deletes the level solid (the GSolid deleting destructor 0x00419AE0, thiscall) without clearing its
        // pointer, which every later render cache flush (~GSolid's first call) would walk
        void __fastcall level_delete_solid(::GSolid* solid, int edx, int flags);
        CallHook<decltype(level_delete_solid)> level_delete_solid_hook{0x004184E0, level_delete_solid};

        void __fastcall level_delete_solid(::GSolid* solid, int edx, int flags)
        {
            level_delete_solid_hook.call_target(solid, edx, flags);
            if (CDedLevel* level = CDedLevel::Get(); level && level->solid == solid) {
                level->solid = nullptr;
            }
        }

        // ~GSolid (0x004956B0) right after its render cache flush, ESI the solid: the Alpine lightmap charts are
        // positional over the level solid's surfaces, so they go with it
        CodeInjection solid_destroy_injection{
            0x004956DF,
            [](auto& regs) {
                const auto* solid = reinterpret_cast<const ::GSolid*>(static_cast<std::uintptr_t>(regs.esi));
                editor_af_lightmap_solid_destroyed(solid);
            },
        };

        // The placeholder bm_load makes for a missing file (0x004BC9C0), after it converted its 32x32 888 pixels
        // (0x004F6F60) into the locked texture, the lock info at ESP+0x24. That converter writes an 888 source only
        // into the 16-bit formats Direct3D 8 locked; this texture locks as 8888, which it leaves unwritten (black).
        CodeInjection missing_bitmap_placeholder_injection{
            0x004BCA17,
            [](auto& regs) {
                constexpr int placeholder_edge = 32;
                const auto& lock = addr_as_ref<rf::gr::LockInfo>(regs.esp + 0x24);
                const rf::ubyte* pixels = rf::bm::missing_placeholder_pixels;
                if (pixels && lock.data && lock.w == placeholder_edge && lock.h == placeholder_edge) {
                    bm_convert_format(lock.data, lock.format, pixels, rf::bm::FORMAT_888_RGB, placeholder_edge,
                                      placeholder_edge, lock.stride_in_bytes, placeholder_edge * 3);
                }
            },
        };

        // particle_render (cdecl), from the perspective view's particle pass (0x00480AA0) and the portal modes' room
        // objects pass (0x00480A60, 0x00480A70): as the game's instanced quads, stock where the core declines
        void particle_render(rf::Particle* particle);
        FunHook<decltype(particle_render)> particle_render_hook{0x0047F790, particle_render};

        void particle_render(rf::Particle* particle)
        {
            EditorRenderer* r = live_renderer();
            if (!r || !particle || !r->add_particle(*particle)) {
                particle_render_hook.call_target(particle);
            }
        }

        // The perspective view's level as View > Show Wireframe, Wireframe Overlay and Draw See-thru Portal Faces ask.
        // The paint reads the portal option (0x0047AE3F) after this. The overlay is left to Brush and Object modes, as
        // the other modes draw their brushes' own wireframes.
        EditorLevelStyle perspective_level_view_style()
        {
            const bool wireframe = g_editor_settings.show_wireframe;
            const CDedLevel* level = CDedLevel::Get();
            const bool overlay = g_editor_settings.wireframe_overlay && level
                && (level->edit_mode == DedEditMode::Brush || level->edit_mode == DedEditMode::Object);
            return {
                .fills = !wireframe,
                .edges = wireframe || overlay,
                .portal_planes = view_portal_option == VIEW_PORTAL_FACES_SEE_THRU,
            };
        }

        // RED outlines selected brushes itself; one in a moving group drawn as edges alone keeps its edges as outline
        EditorSelectionEdges selection_edges(bool fills)
        {
            CDedLevel* level = CDedLevel::Get();
            if (!level) {
                return {};
            }
            SelectedBrushGeometry selected = selected_brush_geometry(*level);
            EditorSelectionEdges edges{std::move(selected.built_face_ids), {}};
            for (BrushNode* brush : selected.brushes) {
                if (fills || !level->brush_in_moving_group(brush)) {
                    edges.solids.push_back(static_cast<const rf::GSolid*>(brush->geometry));
                }
            }
            std::sort(edges.solids.begin(), edges.solids.end());
            return edges;
        }

        // The 3D view paint. In the perspective view it sets the level's View style, and View > Preview Level Fog
        // draws its level, meshes and particles in the level's distance fog, as the game draws them whether or not
        // the level has a sky room (the sky's modes forbid fog). RED never sets fog after gr_init, and gr_screen's fog
        // holds for every window, so it is on for this paint only.
        void __fastcall view_paint(EditorViewport* view, int edx, void* dc);
        FunHook<decltype(view_paint)> view_paint_hook{0x0047AD30, view_paint};

        void __fastcall view_paint(EditorViewport* view, int edx, void* dc)
        {
            EditorRenderer* r = live_renderer();
            if (!r || !view || view->view_type != editor_view_type_perspective) {
                view_paint_hook.call_target(view, edx, dc);
                return;
            }
            const EditorLevelStyle style = perspective_level_view_style();
            r->set_level_view_style(style, style.edges ? selection_edges(style.fills) : EditorSelectionEdges{});
            const ScopeGuard style_reset{[] {
                if (EditorRenderer* live = live_renderer()) {
                    live->set_level_view_style({});
                }
            }};
            const CDedLevel* level = CDedLevel::Get();
            if (!g_editor_settings.preview_level_fog || !level || !(level->fog_far_clip > 0.0f)) {
                view_paint_hook.call_target(view, edx, dc);
                return;
            }
            const float far_clip = level->fog_far_clip;
            // gr_fog_set would keep the last pair for a near clip past the far one
            const float near_clip = std::clamp(level->fog_near_clip, 0.0f, far_clip);
            gr_fog_set(true, level->fog_color.r, level->fog_color.g, level->fog_color.b, near_clip, far_clip);
            const ScopeGuard fog_off{[] {
                gr_fog_set(false, 0, 0, 0, -1.0f, -1.0f);
                if (EditorRenderer* r = live_renderer()) {
                    r->set_fog_overlay_pass(false);
                }
            }};
            view_paint_hook.call_target(view, edx, dc);
        }

        // The paint's object pass (thiscall, the view index): grid, brushes, objects, icons and links, after the level
        CallHook<void __fastcall(CDedLevel*, int, int)> view_paint_objects_hook{
            0x0047B73B,
            [](CDedLevel* level, int edx, int view_index) FASTCALL_LAMBDA {
                if (EditorRenderer* r = live_renderer(); r && rf::gr::screen.fog_mode) {
                    r->set_fog_overlay_pass(true);
                }
                view_paint_objects_hook.call_target(level, edx, view_index);
            },
        };

        // The back-face test (0x004EE4C0, cdecl, the face) of gr_d3d_render_geometry_in_editor (0x004E85A0), which
        // draws Render Everything, selected brushes and the brush preview: View > Show Back Faces keeps what it
        // rejects, and so does Show Wireframe, whose edges are not culled in the level caches either
        CallHook<bool(::GFace*)> geometry_face_facing_hook{
            0x004E8654,
            [](::GFace* face) {
                const EditorRenderer* r = live_renderer();
                return g_editor_settings.show_back_faces || (r && r->face_draw_edges_only())
                    || geometry_face_facing_hook.call_target(face);
            },
        };

        // GSolid::draw's filled branch (0x004CC9A0) calling gr_d3d_render_geometry_in_editor (0x004E85A0, cdecl: solid,
        // pos, orient, outline): Render Everything, filled brushes and the brush preview. Its stock outline is left out
        // where the faces already draw as edges.
        CallHook<void(rf::GSolid*, const void*, const void*, char)> geometry_in_editor_hook{
            0x004CC9BD,
            [](rf::GSolid* solid, const void* pos, const void* orient, char outline) {
                EditorRenderer* r = live_renderer();
                const bool fills = !r || r->begin_face_draw(solid);
                geometry_in_editor_hook.call_target(solid, pos, orient, fills ? outline : 0);
                if (EditorRenderer* live = live_renderer()) {
                    live->end_face_draw(solid);
                }
            },
        };

        // gr_d3d_setup_3d's call to gr_d3d_setup_frustum, once the scale and the projection kind are set
        CodeInjection setup_3d_injection{
            0x004EDF44,
            []() {
                if (EditorRenderer* r = live_renderer()) {
                    r->setup_3d();
                }
            },
        };
    }

    bool upload_af_lightmap_atlas(const alpine_lightmap::ReadResult& section, const std::vector<std::uint8_t>& blocks)
    {
        EditorRenderer* r = live_renderer();
        return r && r->upload_af_lightmap_atlas(section, blocks);
    }

    void release_af_lightmap_atlas()
    {
        if (EditorRenderer* r = live_renderer()) {
            r->release_af_lightmap_atlas();
        }
    }

    bool af_lightmap_atlas_live()
    {
        return g_renderer && g_renderer->af_lightmap_atlas_live();
    }

    void flush()
    {
        flush_batches();
    }

    bool batch_pending()
    {
        EditorRenderer* r = live_renderer();
        return r && r->batch_pending();
    }

    EditorLevelStyle level_style()
    {
        const EditorRenderer* r = live_renderer();
        return r ? r->level_view_style() : EditorLevelStyle{};
    }
}

[[noreturn]] void editor_d3d11_fatal_error(const std::string& text, const std::source_location& loc)
{
    using namespace gr::d3d11;
    // A device removed under the core shows first as whatever call failed, before Present sees it
    if (g_renderer && g_renderer->device_removed()) {
        g_renderer->handle_device_lost(DXGI_ERROR_DEVICE_REMOVED);
    }
    const std::string msg = std::format("{} ({}:{})", text, loc.file_name(), loc.line());
    // g_renderer is set once its constructor returns
    if (!g_renderer) {
        close_after_startup_error(msg);
    }
    g_renderer->close_after_error(msg);
}

void editor_d3d11_apply_patch()
{
    using namespace gr::d3d11;
    using namespace asm_regs;
    xlog::info("[D3D11] Installing the Direct3D 11 renderer");

    // VifMesh and VifLodMesh allocations carry the render cache pointer the shared mesh renderer keeps past the stock
    // structures. Both PUSH imm8 sizes are the allocators' first argument (0x0050D5A4, 0x0050D452).
    static_assert(sizeof(rf::VifMesh) < 0x80 && sizeof(rf::VifLodMesh) < 0x80);
    write_mem<int8_t>(0x0050D5A4 + 1, static_cast<int8_t>(sizeof(rf::VifMesh)));
    write_mem<int8_t>(0x0050D452 + 1, static_cast<int8_t>(sizeof(rf::VifLodMesh)));
    vif_lod_mesh_ctor_injection.install();
    vif_lod_mesh_destroy_injection.install();
    v3d_page_in_injection.install();
    character_instance_page_in_injection.install();
    setup_3d_injection.install();
    solid_destroy_injection.install();
    room_objects_liquid_injection.install();
    missing_bitmap_placeholder_injection.install();
    bm_free_entry_hook.install();
    level_delete_solid_hook.install();
    particle_render_hook.install();
    view_paint_hook.install();
    view_paint_objects_hook.install();
    geometry_face_facing_hook.install();
    geometry_in_editor_hook.install();

    // The solid renderer keeps its own room caches
    AsmWriter{0x0049B4E0}.jmp(clear_solid_render_cache); // geo_cache_flush_all
    AsmWriter{0x0049B470}.ret(); // render cache init (allocates the stock pool)
    AsmWriter{0x0049B520}.jmp(reset_solid_render_cache_after_boolean);

    AsmWriter{0x004EB840}.jmp(set_viewport_wnd); // gr_d3d_set_viewport_wnd
    AsmWriter{0x004EBB20}.jmp(flip); // gr_d3d_flip
    AsmWriter{0x004EBD90}.jmp(close); // gr_d3d_close
    AsmWriter{0x004EC4C0}.jmp(init); // gr_d3d_init
    AsmWriter{0x004ED290}.jmp(read_back_buffer); // gr_d3d_read_backbuffer
    AsmWriter{0x004ED420}.jmp(fog_set); // gr_d3d_fog_set
    AsmWriter{0x004ED560}.mov(al, 1).ret(); // gr_d3d_is_mode_supported
    AsmWriter{0x004EE300}.jmp(project_vertex); // gr_d3d_project_vertex
    AsmWriter{0x004EE5C0}.ret(); // gr_d3d_update_gamma_ramp
    AsmWriter{0x004EE620}.ret(); // gr_d3d_set_texture_mip_filter
    AsmWriter{0x004DEDA0}.ret(); // gr_d3d_set_state
    AsmWriter{0x004E0460}.jmp(page_in); // gr_d3d_page_in
    AsmWriter{0x004E0500}.jmp(clear); // gr_d3d_clear
    AsmWriter{0x004E05C0}.jmp(zbuffer_clear); // gr_d3d_zbuffer_clear
    AsmWriter{0x004E0670}.jmp(set_clip); // gr_d3d_set_clip
    AsmWriter{0x004E06E0}.jmp(bitmap); // gr_d3d_bitmap
    AsmWriter{0x004E1090}.ret(); // gr_d3d_flush_after_color_change
    AsmWriter{0x004E10A0}.jmp(line); // gr_d3d_line
    AsmWriter{0x004E1320}.jmp(line_3d); // gr_d3d_line_vertex_internal
    AsmWriter{0x004E1540}.jmp(tmapper); // gr_d3d_tmapper
    AsmWriter{0x004E8400}.jmp(poly); // gr_d3d_poly
    AsmWriter{0x004F4760}.jmp(texture_save_cache); // gr_d3d_texture_save_cache
    AsmWriter{0x004F4790}.jmp(texture_flush_cache); // gr_d3d_texture_flush_cache
    AsmWriter{0x004F6000}.jmp(texture_mark_dirty); // gr_d3d_texture_mark_dirty
    AsmWriter{0x004F6040}.jmp(lock); // gr_d3d_lock
    AsmWriter{0x004F61A0}.jmp(unlock); // gr_d3d_unlock
    AsmWriter{0x004F61E0}.jmp(get_texel); // gr_d3d_get_texel
    AsmWriter{0x004F63A0}.jmp(texture_add_ref); // gr_d3d_texture_add_ref
    AsmWriter{0x004F63D0}.jmp(texture_remove_ref); // gr_d3d_texture_remove_ref
    AsmWriter{0x004E3300}.jmp(render_sky_room); // render_sky_room
    AsmWriter{0x004E38A0}.ret(); // gr_d3d_render_movable_solid, which RED never calls (no movers)
    AsmWriter{0x00502040}.jmp(render_solid); // gr_d3d_render_static_solid
    AsmWriter{0x005040B0}.ret(); // gr_d3d_render_face_list
    AsmWriter{0x00505C60}.jmp(render_v3d_vif); // gr_d3d_render_v3d_vif
    AsmWriter{0x00506830}.jmp(render_character_vif); // gr_d3d_render_character_vif
    AsmWriter{0x004247B0}.jmp(render_alpha_detail_room); // room_render_alpha_detail

    // The editor's per-face draws end in these two, and the texture binds behind them reach the device
    AsmWriter{0x004E1880}.jmp(render_face);
    AsmWriter{0x004E1C90}.jmp(render_lightmapped_face);
    AsmWriter{0x004F5D10}.jmp(set_stage_texture);
    AsmWriter{0x004E0490}.ret(); // set both stages' textures and the state
    // Alpine code brackets its own draws with the batch pair while a batch is open, which never happens here
    AsmWriter{0x004E98E0}.ret(); // gr_begin_batch
    AsmWriter{0x004E99D0}.jmp(flush_batches); // gr_flush_batch
}
