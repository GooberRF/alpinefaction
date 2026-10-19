#pragma once

#include <array>
#include <functional>
#include <vector>
#include <d3d11.h>
#include <common/ComPtr.h>
#include "../../rf/gr/gr.h"
#include "gr_d3d11_shader.h"
#include "gr_d3d11_buffer.h"
#include "gr_d3d11_hooks.h"

namespace rf
{
    struct Particle;
    struct ParticleEmitter;
}

namespace gr::d3d11
{
    struct GpuParticleInstance;
    class RenderContext;

    // Batched stand-in for the stock per particle render path (particle_render 0x00494B90): every
    // particle becomes one instance record, which particle_vs.hlsl expands to the stock quad, and one
    // instanced draw is issued per texture+mode run. Emitters are drawn from inside their render
    // callback, so the sort order against other transparent geometry is the one stock produced.
    class ParticleRenderer
    {
    public:
        ParticleRenderer(ComPtr<ID3D11Device> device, ShaderManager& shader_manager, RenderContext& render_context);
        // Returns false when the emitter has to go through the stock path instead.
        bool render_emitter(rf::ParticleEmitter* emitter, float interp_dt, int* out_drawn, int* out_draws);
        // Adds one particle no emitter owns to the open batch, which the caller flushes before
        // anything else is drawn. Returns false when it has to go through the stock path instead.
        bool add_particle(const rf::Particle& particle);
        // Batched stand-in for gr_3d_bitmap_angle. Takes colour and texture from the gr screen state,
        // exactly like the dynamic geometry batcher.
        bool add_billboard(const rf::Vector3& pos, float angle, float radius, rf::gr::Mode mode);
        // Ends a batched pass. A no-op when nothing is pending.
        void flush_batch();
        // GPU weather: the shared seeds, each region's ceiling cache, and one instanced draw per region
        // issued in place, after whatever was batched before it.
        bool upload_weather_seeds(const std::vector<WeatherSeed>& seeds);
        bool update_weather_columns(int slot, int num_cells, int first, const std::vector<WeatherColumn>& cells);
        void release_weather_columns();
        bool draw_weather(const WeatherDraw& draw);
        // Runs when the batch goes from empty to pending, so geometry queued elsewhere reaches
        // the framebuffer before the quads that follow it.
        void set_pre_batch_callback(std::function<void()> callback);

    private:
        // How particle_vs.hlsl expands the run's instances: a billboard or a velocity stretched square
        enum class Primitive
        {
            billboard,
            stretched,
        };

        struct State
        {
            Primitive primitive = Primitive::billboard;
            std::array<int, 2> textures = { -1, -1 };
            rf::gr::Mode mode{
                rf::gr::TEXTURE_SOURCE_NONE,
                rf::gr::COLOR_SOURCE_VERTEX,
                rf::gr::ALPHA_SOURCE_VERTEX,
                rf::gr::ALPHA_BLEND_NONE,
                rf::gr::ZBUFFER_TYPE_NONE,
                rf::gr::FOG_ALLOWED,
            };
            ID3D11PixelShader* pixel_shader = nullptr;

            bool operator==(const State& other) const = default;
        };

        // The engine view the instanced quads are expanded with, as rotate_vertex and
        // compute_vertex_codes read it
        struct View
        {
            rf::Matrix3 matrix;
            rf::Vector3 pos;
            rf::Vector3 scale;
            float far_clip_z = 0.0f;
            bool far_clip_on = false;
            std::array<float, 4> proj{};

            bool operator==(const View& other) const = default;
        };

        struct WeatherColumnBuffer
        {
            ComPtr<ID3D11Buffer> buffer;
            ComPtr<ID3D11ShaderResourceView> srv;
            int num_cells = 0;
        };

        void begin_run(const State& state, bool full);
        void capture_view();
        GpuParticleInstance* setup_instance(const State& state);
        void flush();
        void upload_view();
        bool lookup_bitmap_size(int bm_handle, int& w, int& h);
        // particle_render's frame select, mode and colour, then the billboard or stretched record.
        // pos and last_pos are where the particle is drawn, which interpolation may have moved.
        bool add_particle_instance(const rf::Particle& particle, const rf::Vector3& pos, const rf::Vector3& last_pos);
        // Shared billboard record for every gr_3d_bitmap_angle source. Returns false when stock would
        // have drawn nothing.
        bool add_billboard_instance(const rf::Vector3& pos, float angle, float radius, int bm_handle,
            rf::Color color, const State& state);

        ComPtr<ID3D11Device> device_;
        RenderContext& render_context_;
        RingBuffer<GpuParticleInstance> instance_ring_buffer_;
        ComPtr<ID3D11Buffer> quad_index_buffer_;
        ComPtr<ID3D11Buffer> view_cbuffer_;
        VertexShaderAndLayout billboard_vertex_shader_;
        VertexShaderAndLayout stretched_vertex_shader_;
        VertexShaderAndLayout weather_vertex_shader_;
        ComPtr<ID3D11Buffer> weather_cbuffer_;
        ComPtr<ID3D11Buffer> weather_seed_buffer_;
        int weather_seed_count_ = 0;
        std::vector<WeatherColumnBuffer> weather_columns_;
        ComPtr<ID3D11PixelShader> std_pixel_shader_;
        ComPtr<ID3D11PixelShader> std_pixel_shader_no_gas_;
        bool instancing_ok_ = false;
        bool weather_ok_ = false;
        State state_;
        View run_view_;
        View uploaded_view_;
        bool view_uploaded_ = false;
        std::function<void()> pre_batch_callback_;
        bool batch_pending_ = false;
        int drawn_ = 0;
        int draws_ = 0;
        int cached_bm_handle_ = -1;
        int cached_bm_w_ = 0;
        int cached_bm_h_ = 0;
    };
}
