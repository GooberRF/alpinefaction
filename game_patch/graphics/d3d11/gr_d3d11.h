#pragma once

#include <cstdint>
#include <vector>
#include <d3d11.h>
#include <common/ComPtr.h>
#include <common/DynamicLinkLibrary.h>
#include <xlog/xlog.h>
#include "../../rf/gr/gr.h"
#include "../../rf/os/frametime.h"
#include <common/gr_d3d11/gr_d3d11_util.h>
#include <common/gr_d3d11/gr_d3d11_transform.h>
#include <common/gr_d3d11/gr_d3d11_liquid.h>
#include "gr_d3d11_scenefx.h"

namespace rf
{
    struct GSolid;
    struct GRoom;
    struct VifMesh;
    struct VifLodMesh;
    struct MeshMaterial;
    struct MeshRenderParams;
    struct CharacterInstance;
    struct Player;
    struct VfxSfxoRenderObj;
    struct Particle;
    struct ParticleEmitter;
}

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
    class ParticleRenderer;
    struct WeatherSeed;
    struct WeatherColumn;
    struct WeatherDraw;
    class RenderContext;
    class AfLightmapRenderer;
    class SolidRenderer;
    class MeshRenderer;
    class DecorationRenderer;
    class EntityShadowRenderer;
    class OutlineRenderer;
    class VfxMeshRenderer;
    class GammaPass;

    class Renderer
    {
    public:
        Renderer(HWND hwnd);
        ~Renderer();
        void window_active();
        void window_inactive();
        void set_fullscreen_state(bool fullscreen);
        void bitmap(int bm_handle, int x, int y, int w, int h, int sx, int sy, int sw, int sh, bool flip_x, bool flip_y, rf::gr::Mode mode);
        void bitmap(int bm_handle, float x, float y, float w, float h, float sx, float sy, float sw, float sh, bool flip_x, bool flip_y, rf::gr::Mode mode);
        void poly_2d(int bm_handle, int nv, const rf::gr::Vertex* vertices, rf::gr::Mode mode);
        void page_in(int bm_handle);
        void clear();
        void zbuffer_clear();
        void set_clip();
        void set_far_clip(bool enabled);
        void flip();
        void flush_outlines_before_fpgun();
        void texture_save_cache();
        void texture_flush_cache(bool force);
        void texture_flush_non_user_cache();
        void texture_mark_dirty(int bm_handle);
        void texture_add_ref(int bm_handle);
        void texture_remove_ref(int bm_handle);
        void reset_sampler_states();
        bool lock(int bm_handle, int section, rf::gr::LockInfo *lock);
        void unlock(rf::gr::LockInfo *lock);
        void get_texel(int bm_handle, float u, float v, rf::gr::Color *clr);
        bool set_render_target(int bm_handle);
        void invalidate_texture_cache();
        int render_target_generation();
        rf::bm::Format read_back_buffer(int x, int y, int w, int h, rf::ubyte *data);
        void tmapper(int nv, const rf::gr::Vertex **vertices, int vertex_attributes, rf::gr::Mode mode);
        void line_3d(const rf::gr::Vertex& v0, const rf::gr::Vertex& v1, rf::gr::Mode mode);
        void line_2d(float x1, float y1, float x2, float y2, rf::gr::Mode mode);
        bool poly(int nv, rf::gr::Vertex** vertices, int vertex_attributes, rf::gr::Mode mode, bool constant_sw, float sw);
        void project_vertex(rf::gr::Vertex* v);
        void setup_3d(Projection proj);
        void render_solid(rf::GSolid* solid, rf::GRoom** rooms, int num_rooms);
        void render_movable_solid(rf::GSolid* solid, const rf::Vector3& pos, const rf::Matrix3& orient, bool include_alpha);
        bool movable_solid_has_alpha(rf::GSolid* solid);
        void render_movable_solid_alpha(rf::GSolid* solid, const rf::Vector3& pos, const rf::Matrix3& orient);
        void render_alpha_detail_room(rf::GRoom *room, rf::GSolid *solid);
        bool render_particle_emitter(rf::ParticleEmitter* emitter, float interp_dt, int* out_drawn, int* out_draws);
        bool batched_billboard(const rf::Vector3& pos, float angle, float radius, rf::gr::Mode mode);
        bool batched_particle(const rf::Particle& particle);
        void flush_batched_quads();
        bool upload_weather_seeds(const std::vector<WeatherSeed>& seeds);
        bool update_weather_columns(int slot, int num_cells, int first, const std::vector<WeatherColumn>& cells);
        void release_weather_columns();
        bool draw_weather(const WeatherDraw& draw);
        void render_sky_room(rf::GRoom *room, rf::Vector3& out_sky_transform_pos, rf::Matrix3& out_sky_transform_orient);
        void render_room_liquid_surface(rf::GSolid* solid, rf::GRoom* room);
        void render_room_decoration_edges(rf::GSolid* solid, rf::GRoom* room);
        void clear_solid_cache();
        void release_detail_room_cache(rf::GRoom* room);
        void reset_solid_cache_after_boolean();
        void release_terrain_gpu();
        bool upload_af_lightmap_atlas(const alpine_lightmap::ReadResult& section,
                                      const std::vector<std::uint8_t>& blocks);
        void release_af_lightmap_atlas();
        bool af_lightmap_atlas_live() const;
        void render_v3d_vif(rf::VifLodMesh *lod_mesh, int lod_index, const rf::Vector3& pos, const rf::Matrix3& orient, const rf::MeshRenderParams& params, bool skip_ambient_cache = false);
        void render_character_vif(rf::VifLodMesh *lod_mesh, int lod_index, const rf::Vector3& pos, const rf::Matrix3& orient, const rf::CharacterInstance *ci, const rf::MeshRenderParams& params, bool skip_ambient_cache = false);
        void clear_vif_cache(rf::VifLodMesh *lod_mesh);
        void render_vfx(rf::VfxSfxoRenderObj* obj, float frame);
        void fog_set();
        void page_in_v3d_mesh(rf::VifLodMesh* lod_mesh, rf::MeshMaterial* materials = nullptr, int num_materials = 0);
        void page_in_character_mesh(rf::VifLodMesh* lod_mesh);
        void page_in_solid(rf::GSolid* solid);
        void page_in_movable_solid(rf::GSolid* solid);
        void flush_caches();
        void reset_static_vertex_color_tracking();
        void clear_mesh_lights();
        void set_mesh_bounds(const rf::Vector3& center, float radius);
        void set_pow2_tex_active(bool active);
        float z_far() const;
        bool supports_sample_count(uint32_t sample_count);
        uint32_t get_sample_count() const;
        void flush_frame_buffers();
        bool supports_exclusive_fullscreen() const;
        void run_scene_post_pass();
        // Same predicate run_scene_post_pass uses, so the reticle deferral cannot disagree with it
        bool liquid_post_pass_pending() const;
        void run_scope_glass_pass();

        void defer_reticle(rf::Player* pp)
        {
            deferred_reticle_player_ = pp;
        }

        rf::Player* take_deferred_reticle()
        {
            rf::Player* pp = deferred_reticle_player_;
            deferred_reticle_player_ = nullptr;
            return pp;
        }

        void run_damage_vignette_pass();
        void trigger_damage_vignette(unsigned dir_mask);
        bool liquid_background_color(rf::Vector3& out) const;
        void set_sky_room(bool sky_room);
        void set_draw_room_uid(int room_uid);

        // Room whose object dispatch is running, -1 outside it (fpgun, sky objects)
        void set_object_room_uid(int room_uid)
        {
            object_room_uid_ = room_uid;
        }

        // The tint hook at 0x004328FD runs after the post pass in the same frame
        bool liquid_tint_drawn_this_frame() const
        {
            return liquid_tint_drawn_frame_ == rf::frame_count;
        }

        int render_target_bm_handle() const
        {
            return render_target_bm_handle_;
        }

    private:
        void init_device();
        void init_swap_chain(HWND hwnd);
        void init_back_buffer(const uint32_t sample_count);
        void resize_swap_chain_buffers();
        // The MSAA setting, or 1 when the device can't do it
        uint32_t usable_sample_count();
        void init_scene_texture();
        void init_depth_stencil_buffer(const uint32_t sample_count);
        void flush_outlines_before_2d();
        bool ensure_postfx_source();
        void copy_scene_to_postfx_source();

        HWND hwnd_;
        DynamicLinkLibrary d3d11_lib_;
        ComPtr<ID3D11Device> device_;
        ComPtr<IDXGISwapChain> swap_chain_;
        ComPtr<ID3D11DeviceContext> context_;
        ComPtr<ID3D11Texture2D> back_buffer_;
        ComPtr<ID3D11RenderTargetView> back_buffer_rtv_;
        ComPtr<ID3D11Texture2D> scene_texture_;
        ComPtr<ID3D11ShaderResourceView> scene_texture_srv_;
        ComPtr<ID3D11Texture2D> postfx_source_;
        ComPtr<ID3D11ShaderResourceView> postfx_source_srv_;
        ComPtr<ID3D11Texture2D> msaa_render_target_;
        ComPtr<ID3D11Texture2D> default_render_target_;
        ComPtr<ID3D11RenderTargetView> default_render_target_view_;
        ComPtr<ID3D11DepthStencilView> depth_stencil_view_;
        SceneDepthCapture scene_depth_;
        std::unique_ptr<StateManager> state_manager_;
        std::unique_ptr<ShaderManager> shader_manager_;
        std::unique_ptr<TextureManager> texture_manager_;
        std::unique_ptr<DynamicGeometryRenderer> dyn_geo_renderer_;
        std::unique_ptr<ParticleRenderer> particle_renderer_;
        std::unique_ptr<RenderContext> render_context_;
        std::unique_ptr<AfLightmapRenderer> af_lightmap_renderer_;
        std::unique_ptr<SolidRenderer> solid_renderer_;
        std::unique_ptr<MeshRenderer> mesh_renderer_;
        std::unique_ptr<DecorationRenderer> decoration_renderer_;
        std::unique_ptr<VfxMeshRenderer> vfx_renderer_;
        std::unique_ptr<EntityShadowRenderer> entity_shadow_renderer_;
        std::unique_ptr<OutlineRenderer> outline_renderer_;
        std::unique_ptr<GammaPass> gamma_pass_;
        std::unique_ptr<ScenePostPass> scene_post_pass_;
        DamageVignetteState damage_vignette_;
        UINT rt_width_ = 0;
        UINT rt_height_ = 0;
        int liquid_tint_drawn_frame_ = -1;
        int damage_vignette_decay_frame_ = -1;
        int object_room_uid_ = -1;
        int liquid_update_frame_ = -1;
        int scene_depth_frame_ = -1;
        bool scene_depth_wanted_ = false;
        rf::Player* deferred_reticle_player_ = nullptr;
        int render_target_bm_handle_ = -1;
        bool skip_gamma_pass_ = false;
        bool low_frame_latency_ = false;
        bool allow_tearing_ = false;
        bool frame_latency_stall_logged_ = false;
        // Set after a rejected Present was recovered, so a second rejection in a row stays fatal
        bool present_recovered_ = false;
        // What the depth buffer was built with; a resized back buffer has to match it
        uint32_t frame_buffer_sample_count_ = 1;
        HANDLE frame_latency_wait_handle_ = nullptr;
        UINT swap_chain_flags_ = 0;
    };
}
