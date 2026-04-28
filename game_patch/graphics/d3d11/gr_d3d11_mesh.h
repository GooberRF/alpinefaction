#pragma once

#include <cstddef>
#include <unordered_map>
#include <vector>
#include <d3d11.h>
#include <common/ComPtr.h>
#include "gr_d3d11_shader.h"
#include "../../misc/alpine_options.h"
#include "../../misc/alpine_settings.h"
#include "../../rf/level.h"

namespace rf
{
    struct VifLodMesh;
    struct MeshMaterial;
    struct MeshRenderParams;
    struct CharacterInstance;
}

namespace gr::d3d11
{
    class StateManager;
    class RenderContext;

    // Cached vertex lighting state for the current level, updated at level load.
    // Avoids per-frame string comparisons and hash lookups in hot render paths.
    extern bool g_level_vertex_lighting;
    void evaluate_mesh_lighting(const std::string& level_filename);

    inline bool level_uses_vertex_lighting()
    {
        return g_level_vertex_lighting;
    }

    // Cached pixel light overbright for the current level, updated at level load.
    // Per-level TBL override takes precedence over the global setting.
    extern float g_level_pixel_light_overbright;
    void evaluate_pixel_light_overbright(const std::string& level_filename);

    // Alpha test threshold for ZBUFFER_TYPE_FULL_ALPHA_TEST, updated at level load.
    // Stock D3D8 value is 16/255; default is 1/255 for better gradient rendering.
    extern float g_alpha_test_threshold;
    void evaluate_alpha_test_threshold(const std::string& level_filename);

    void on_character_fullbright_state_changed();
    void on_static_vertex_color_state_changed(rf::VifLodMesh* changed_lod_mesh = nullptr);

    void clear_entity_ambient_cache();

    // RAII guard: while one of these is alive, the mesh drawn by the stock render
    // function it wraps opts out of r_picmip
    class [[nodiscard]] ScopedPicmipSkipObject
    {
    public:
        ScopedPicmipSkipObject() noexcept { ++depth_; }
        ~ScopedPicmipSkipObject() noexcept { --depth_; }
        ScopedPicmipSkipObject(const ScopedPicmipSkipObject&) = delete;
        ScopedPicmipSkipObject& operator=(const ScopedPicmipSkipObject&) = delete;

        static bool active() noexcept { return depth_ > 0; }

    private:
        static inline int depth_ = 0;
    };

    class BaseMeshRenderCache
    {
    public:
        struct Batch
        {
            Batch(int start_index, int num_indices, int base_vertex, int texture_index, rf::gr::Mode mode, bool double_sided, float self_illumination = 0.0f) :
                start_index{start_index}, num_indices{num_indices}, base_vertex{base_vertex},
                texture_index{texture_index}, mode{mode}, double_sided{double_sided},
                self_illumination{self_illumination}
            {}

            int start_index;
            int num_indices;
            int base_vertex = 0;
            int texture_index;
            rf::gr::Mode mode;
            bool double_sided = false;
            float self_illumination = 0.0f;
        };

        struct Mesh
        {
            std::vector<Batch> batches;
            std::size_t vertex_offset = 0;
            std::size_t vertex_count = 0;
        };

        virtual ~BaseMeshRenderCache() {}
        BaseMeshRenderCache(rf::VifLodMesh* lod_mesh) :
            lod_mesh_(lod_mesh)
        {}

        const std::vector<Batch>& get_batches(int lod_level) const
        {
            return meshes_[lod_level].batches;
        }

        const Mesh& get_mesh(int lod_level) const
        {
            return meshes_[lod_level];
        };

        std::size_t base_vertex_offset() const
        {
            return base_vertex_offset_;
        }

    protected:
        rf::VifLodMesh* lod_mesh_;
        std::vector<Mesh> meshes_;
        std::size_t base_vertex_offset_ = 0;
    };


    class BufferWrapper
    {
    public:
        BufferWrapper(unsigned initial_capacity, unsigned el_size, UINT bind_flag, ID3D11Device* device);
        void write(void* data, unsigned n, RenderContext& render_context);
        void reserve(unsigned n, RenderContext& render_context);

        void clear()
        {
            size_ = 0;
        }

        unsigned size() const
        {
            return size_;
        }

        ID3D11Buffer* buffer()
        {
            return buffer_;
        }

    private:
        ComPtr<ID3D11Buffer> buffer_;
        unsigned size_ = 0;
        UINT bind_flag_;
        unsigned capacity_;
        unsigned el_size_;

        void create_buffer(ID3D11Device* device);
    };

    class MeshRenderer
    {
    public:
        MeshRenderer(ComPtr<ID3D11Device> device, ShaderManager& shader_manager, StateManager& state_manager, RenderContext& render_context);
        ~MeshRenderer();
        void render_v3d_vif(rf::VifLodMesh *lod_mesh, int lod_index, const rf::Vector3& pos, const rf::Matrix3& orient, const rf::MeshRenderParams& params, bool skip_ambient_cache = false);
        void render_character_vif(rf::VifLodMesh *lod_mesh, int lod_index, const rf::Vector3& pos, const rf::Matrix3& orient, const rf::CharacterInstance *ci, const rf::MeshRenderParams& params, bool skip_ambient_cache = false);
        void clear_vif_cache(rf::VifLodMesh *lod_mesh);
        void page_in_v3d_mesh(rf::VifLodMesh* lod_mesh, rf::MeshMaterial* materials = nullptr, int num_materials = 0);
        void page_in_character_mesh(rf::VifLodMesh* lod_mesh);
        void flush_caches();
        void reset_static_vertex_color_tracking();

        bool has_cache(const rf::VifLodMesh* lod_mesh) const;
        void handle_static_vertex_color_state_change(rf::VifLodMesh* changed_lod_mesh, uint64_t generation);

        // Shadow rendering: draws mesh geometry with externally-set shaders (depth-only pass)
        void draw_shadow_v3d_mesh(rf::VifLodMesh* lod_mesh, const rf::Vector3& pos, const rf::Matrix3& orient,
                                  const VertexShaderAndLayout& shadow_vs, ID3D11DeviceContext* context);
        void draw_shadow_character_mesh(rf::VifLodMesh* lod_mesh, const rf::Vector3& pos, const rf::Matrix3& orient,
                                        const rf::CharacterInstance* ci, const VertexShaderAndLayout& shadow_vs,
                                        ID3D11DeviceContext* context);

        // Outline support: prepare character mesh for drawing without actually rendering.
        // Sets up model transform, bone transforms, binds vertex/index buffers, and returns batches.
        const std::vector<BaseMeshRenderCache::Batch>* prepare_character_for_draw(
            rf::VifLodMesh* lod_mesh, int lod_index,
            const rf::Vector3& pos, const rf::Matrix3& orient,
            const rf::CharacterInstance* ci);

        // Outline support: prepare static (v3d) mesh for drawing without actually rendering.
        // Sets up model transform, binds vertex/index buffers, and returns batches.
        const std::vector<BaseMeshRenderCache::Batch>* prepare_v3d_for_draw(
            rf::VifLodMesh* lod_mesh, int lod_index,
            const rf::Vector3& pos, const rf::Matrix3& orient);

    private:
        void draw_cached_mesh(rf::VifLodMesh *lod_mesh, BaseMeshRenderCache& render_cache, const rf::MeshRenderParams& params, int lod_index, bool skip_ambient_cache = false);

        ComPtr<ID3D11Device> device_;
        RenderContext& render_context_;
        std::unordered_map<rf::VifLodMesh*, std::unique_ptr<BaseMeshRenderCache>> render_caches_;
        VertexShaderAndLayout standard_vertex_shader_;
        VertexShaderAndLayout character_vertex_shader_;
        ComPtr<ID3D11PixelShader> pixel_shader_;
        ComPtr<ID3D11PixelShader> pixel_shader_no_gas_;
        BufferWrapper v3d_vb_;
        BufferWrapper v3d_ib_;
        uint64_t last_static_vertex_color_generation_ = 0;
    };
}
