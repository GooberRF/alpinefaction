#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <common/ComPtr.h>
#include <common/DynamicLinkLibrary.h>
#include <common/gr_d3d11/gr_d3d11_shader.h>
#include <common/gr_d3d11/gr_d3d11_transform.h>
#include <gr_d3d11_engine.h>
#include "../editor_gr.h"

namespace alpine_lightmap
{
    struct ReadResult;
}

namespace gr::d3d11
{
    class StateManager;
    class ShaderManager;
    class TextureManager;
    class DynamicGeometryRenderer;
    class RenderContext;
    class AfLightmapRenderer;
    class SolidRenderer;
    class MeshRenderer;
    class ParticleRenderer;
    class EditorFaceRenderer;
    struct EditorLightmapVertex;

    // The selection RED outlines itself in the perspective view: the level faces built from a selected brush that still
    // lie on it, by face id, and the selected brushes' solids the per-face draw shows no edges of; both sorted
    struct EditorSelectionEdges
    {
        std::vector<int> level_face_ids;
        std::vector<const rf::GSolid*> solids;
    };

    // RED's Direct3D 11 backend. Like RED's Direct3D 8 one, every window draws into the top left of one scene
    // target as big as the largest monitor, which flip copies into the swap chain of the window
    // gr_set_viewport_wnd named last.
    class EditorRenderer
    {
    public:
        explicit EditorRenderer(bool headless);
        ~EditorRenderer();

        void set_viewport_wnd(HWND hwnd);
        void flip();
        void clear();
        void zbuffer_clear();
        void set_clip();
        void fog_set();
        void setup_3d();
        rf::ubyte project_vertex(rf::gr::Vertex* v);
        void bitmap(int bm_handle, int x, int y, int w, int h, int sx, int sy, int sw, int sh, bool flip_x,
                    bool flip_y, rf::gr::Mode mode);
        void line_2d(float x1, float y1, float x2, float y2, rf::gr::Mode mode);
        void line_3d(const rf::gr::Vertex& v0, const rf::gr::Vertex& v1, rf::gr::Mode mode);
        void tmapper(int nv, const rf::gr::Vertex** vertices, int vertex_attributes, rf::gr::Mode mode);
        bool poly(int nv, rf::gr::Vertex** vertices, int vertex_attributes, rf::gr::Mode mode, bool constant_sw,
                  float sw);
        bool face(rf::GSolid* solid, rf::GFace* face, rf::gr::Mode mode, int bm_handle);
        bool lightmapped_face(rf::GSolid* solid, rf::GFace* face, rf::gr::Mode mode, int bm_handle, int lm_handle);
        // particle_render's stand-in, as the game's: false when stock has to draw it
        bool add_particle(const rf::Particle& particle);
        void flush_batches();
        bool batch_pending() const;
        // The level fog preview from the view's object pass on: grid, brushes, icons and links stay clear of the fog
        void set_fog_overlay_pass(bool overlay_pass)
        {
            fog_overlay_pass_ = overlay_pass;
        }
        // What the paint of the perspective view in progress draws of the level, and of the solids the per-face draw
        // fills. Every other draw has the defaults. The edges leave out the selection RED outlines itself.
        void set_level_view_style(const EditorLevelStyle& style, EditorSelectionEdges selection_edges = {})
        {
            level_view_style_ = style;
            selection_edges_ = std::move(selection_edges);
        }
        const EditorLevelStyle& level_view_style() const
        {
            return level_view_style_;
        }
        // Around the per-face draw of a solid (0x004E85A0), whose colour GSolid::draw has set: begin is false when its
        // fills are left out, and end draws the edges and portal planes the style asks for
        bool begin_face_draw(const rf::GSolid* solid);
        void end_face_draw(rf::GSolid* solid);
        // Whether the per-face draw in progress draws edges alone (Show Wireframe)
        bool face_draw_edges_only() const
        {
            return face_draw_ && !face_draw_->style.fills;
        }

        rf::bm::Format read_back_buffer(int x, int y, int w, int h, rf::ubyte* data);
        void page_in(int bm_handle);
        void texture_save_cache();
        void texture_flush_cache(bool force);
        void texture_mark_dirty(int bm_handle);
        // Before the bitmap's entry is freed: the texture cache is keyed by bitmap slot, which the next bitmap reuses
        void texture_free(int bm_handle);
        void texture_add_ref(int bm_handle);
        void texture_remove_ref(int bm_handle);
        bool lock(int bm_handle, int section, rf::gr::LockInfo* lock);
        void unlock(rf::gr::LockInfo* lock);
        void get_texel(int bm_handle, float u, float v, rf::gr::Color* clr);

        void render_solid(rf::GSolid* solid, rf::GRoom** rooms, int num_rooms);
        void render_sky_room(rf::GRoom* room);
        void render_alpha_detail_room(rf::GRoom* room, rf::GSolid* solid);
        void render_room_liquid_surface(rf::GSolid* solid, rf::GRoom* room);
        void clear_solid_cache();
        void reset_solid_cache_after_boolean();

        bool upload_af_lightmap_atlas(const alpine_lightmap::ReadResult& section,
                                      const std::vector<std::uint8_t>& blocks);
        void release_af_lightmap_atlas();
        bool af_lightmap_atlas_live() const;

        void render_v3d_vif(rf::VifLodMesh* lod_mesh, int lod_index, const rf::Vector3& pos, const rf::Matrix3& orient,
                            const rf::MeshRenderParams& params, bool skip_ambient_cache);
        void render_character_vif(rf::VifLodMesh* lod_mesh, int lod_index, const rf::Vector3& pos,
                                  const rf::Matrix3& orient, const rf::CharacterInstance* ci,
                                  const rf::MeshRenderParams& params, bool skip_ambient_cache);
        void clear_vif_cache(rf::VifLodMesh* lod_mesh);
        void page_in_v3d_mesh(rf::VifLodMesh* lod_mesh, rf::MeshMaterial* materials, int num_materials);
        void page_in_character_mesh(rf::VifLodMesh* lod_mesh);
        void set_mesh_bounds(const rf::Vector3& center, float radius);
        void clear_mesh_lights();

        // Once the device is lost or a call failed: nothing more reaches the GPU
        bool stopped() const
        {
            return stopped_;
        }

        // A driver reset (TDR) or removed device: the level is saved as RED's autosave saves it, the user is told, and
        // RED closes. Any other result returns.
        void handle_device_lost(HRESULT hr);
        // A core call failed for a reason other than a lost device: the level is saved, the user told and RED closed
        // the same way
        [[noreturn]] void close_after_error(const std::string& msg);
        // The largest texture side the device's feature level allows
        UINT max_texture_size() const;
        // The core's fatal errors: whether the device is gone, so the failure is a device loss
        bool device_removed() const;

    private:
        class TerrainChunkRoomsHidden;

        struct ViewTarget
        {
            ComPtr<IDXGISwapChain> swap_chain;
            UINT w = 0;
            UINT h = 0;
        };

        void init_device();
        // Before RED closes; false when the renderer already was stopped
        bool stop();
        void init_scene_target();
        void present();
        ViewTarget* view_target(HWND hwnd, UINT w, UINT h);
        void forget_stale_room_caches(rf::GRoom* room);
        void remember_room_caches(rf::GRoom* room);
        void sync_solid_options();
        // The level caches' edges and portal planes the style asks for, each drawn by draw(SolidOverlay) in one colour
        template<typename F>
        void render_solid_overlays(F&& draw);
        bool fill_face(rf::GSolid* solid, rf::GFace* face, rf::gr::Mode mode, int bm_handle);
        bool fill_lightmapped_face(rf::GSolid* solid, rf::GFace* face, rf::gr::Mode mode, int bm_handle,
                                   int lm_handle);
        // A face of the per-face draw as its style asks: fill(), its edges, or both
        template<typename F>
        bool styled_face(rf::GFace* face, F&& fill);
        bool queue_face_edges(rf::GFace* face);
        // Gathers `face` into face_verts_ and face_vertex_refs_ as the per-face draw rotated it and projects it; false
        // for a broken edge loop, fewer than 3 or more than max_vertices vertices, or a face out of view
        bool gather_visible_face(const rf::GFace* face, std::size_t max_vertices);
        void queue_portal_planes(rf::GSolid* solid);
        // Before anything but a particle is queued: the particles queued before it draw first, and while the level
        // fog preview is on, RED's lines and 2D bitmaps (`overlay`) and, in the overlay pass, all but a .vfx mesh's
        // polygons are drawn as if their mode forbade fog
        rf::gr::Mode prepare_draw(rf::gr::Mode mode, bool overlay);
        // Queues `face` (at most max_vertices of them) with both texture stages, its vertices white with the face's
        // texture UVs until fill(face vertex, index, out) sets the lightmap UV and chart and whatever else differs
        template<typename F>
        bool queue_lightmapped_face(rf::GFace* face, rf::gr::Mode mode, int bm_handle, int lm_handle,
                                    std::size_t max_vertices, F&& fill);
        // Its decal polygons over a face drawn with lm_handle; lightmap_uv(face vertex, out) sets a lightmap UV
        template<typename F>
        void queue_face_decals(rf::GFace* face, int lm_handle, F&& lightmap_uv);

        bool headless_;
        DynamicLinkLibrary d3d11_lib_;
        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11DeviceContext> context_;
        ComPtr<IDXGIFactory> dxgi_factory_;
        ComPtr<ID3D11Texture2D> scene_texture_;
        ComPtr<ID3D11RenderTargetView> scene_rtv_;
        ComPtr<ID3D11DepthStencilView> depth_stencil_view_;
        UINT scene_w_ = 0;
        UINT scene_h_ = 0;
        std::unique_ptr<StateManager> state_manager_;
        std::unique_ptr<ShaderManager> shader_manager_;
        std::unique_ptr<TextureManager> texture_manager_;
        std::unique_ptr<RenderContext> render_context_;
        std::unique_ptr<DynamicGeometryRenderer> dyn_geo_renderer_;
        std::unique_ptr<EditorFaceRenderer> face_renderer_;
        std::unique_ptr<AfLightmapRenderer> af_lightmap_renderer_;
        std::unique_ptr<SolidRenderer> solid_renderer_;
        std::unique_ptr<MeshRenderer> mesh_renderer_;
        std::unique_ptr<ParticleRenderer> particle_renderer_;
        HWND hwnd_ = nullptr;
        bool fog_overlay_pass_ = false;
        EditorLevelStyle level_view_style_;
        EditorSelectionEdges selection_edges_;
        struct FaceDraw
        {
            EditorLevelStyle style;
            rf::Color edge_color;
            // Sorted; into selection_edges_, so valid for this per-face draw only
            std::span<const int> hidden_edge_face_ids;
        };
        std::optional<FaceDraw> face_draw_;
        // The edges the per-face draw in progress queued, as line end pairs: screen position and projected depth
        struct EdgeVertex
        {
            float sx;
            float sy;
            float sw;
        };
        std::vector<EdgeVertex> face_edges_;
        // Scratch of the face draws, one at a time
        std::vector<rf::gr::Vertex> face_verts_;
        std::vector<const rf::GFaceVertex*> face_vertex_refs_;
        std::vector<const rf::gr::Vertex*> face_vert_ptrs_;
        // outline_standard_vs at no thickness and outline_ps: the level cache overlays in one colour
        VertexShaderAndLayout overlay_vertex_shader_;
        ComPtr<ID3D11PixelShader> overlay_pixel_shader_;
        ComPtr<ID3D11Buffer> overlay_vs_params_;
        ComPtr<ID3D11Buffer> overlay_color_;
        std::unordered_map<HWND, ViewTarget> view_targets_;
        bool stopped_ = false;
        // A frame reached a window: from then on a window whose swap chain cannot be made stays blank
        bool presented_ = false;
        bool swap_chain_failure_logged_ = false;
        // geo_cache values the solid renderer handed out since its last clear. A room RED reallocates can keep a
        // stale value, which the renderer would take for its own cache.
        std::unordered_set<const void*> room_caches_;
        // While a solid draw has the terrain preview's chunk rooms emptied, so a save on device loss restores them
        TerrainChunkRoomsHidden* hidden_terrain_rooms_ = nullptr;
    };

    // A core call failed while the renderer was being built: closes as a device that could not be created does
    [[noreturn]] void close_after_startup_error(const std::string& msg);
}
