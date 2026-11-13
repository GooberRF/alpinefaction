#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <xlog/xlog.h>
#include <gr_d3d11_engine.h>
#include <common/gr_d3d11/gr_d3d11_util.h>
#include <common/gr_d3d11/gr_d3d11_context.h>
#include <common/gr_d3d11/gr_d3d11_particle.h>
#include <common/gr_d3d11/gr_d3d11_shader.h>

namespace
{
    // Mirror of ParticleViewBuffer in particle_vs.hlsl (b5)
    struct alignas(16) ParticleViewBufferData
    {
        std::array<float, 3> view_rvec;
        float far_clip_z;
        std::array<float, 3> view_uvec;
        float far_clip_on;
        std::array<float, 3> view_fvec;
        float pad0;
        std::array<float, 3> view_pos;
        float pad1;
        std::array<float, 3> matrix_scale;
        float pad2;
        std::array<float, 4> proj;
    };
    static_assert(offsetof(ParticleViewBufferData, far_clip_z) == 12);
    static_assert(offsetof(ParticleViewBufferData, view_uvec) == 16);
    static_assert(offsetof(ParticleViewBufferData, far_clip_on) == 28);
    static_assert(offsetof(ParticleViewBufferData, view_fvec) == 32);
    static_assert(offsetof(ParticleViewBufferData, view_pos) == 48);
    static_assert(offsetof(ParticleViewBufferData, matrix_scale) == 64);
    static_assert(offsetof(ParticleViewBufferData, proj) == 80);
    static_assert(sizeof(ParticleViewBufferData) == 96);

    // Mirror of WeatherBuffer in particle_vs.hlsl (b6)
    struct alignas(16) WeatherBufferData
    {
        std::array<float, 3> box_min;
        std::uint32_t wrap_base;
        std::array<float, 3> box_size;
        std::uint32_t salt;
        std::array<float, 3> box_shift;
        float sim_time;
        std::array<float, 3> camera_pos;
        float half_width;
        std::array<float, 3> camera_rvec;
        float streak_half_len;
        std::array<float, 3> camera_uvec;
        float sway_amplitude;
        std::array<float, 3> fall_dir;
        float sway_speed;
        std::array<float, 3> region_center;
        float sway_phase;
        std::array<float, 3> region_rvec;
        float region_radius;
        std::array<float, 3> region_uvec;
        float surface_y;
        std::array<float, 3> region_fvec;
        float column_cell;
        std::array<float, 3> region_half_extents;
        std::uint32_t flags;
        std::array<std::int32_t, 2> column_cols;
        std::array<std::int32_t, 2> column_origin;
        std::array<float, 4> color;
    };
    static_assert(offsetof(WeatherBufferData, wrap_base) == 12);
    static_assert(offsetof(WeatherBufferData, box_size) == 16);
    static_assert(offsetof(WeatherBufferData, box_shift) == 32);
    static_assert(offsetof(WeatherBufferData, camera_pos) == 48);
    static_assert(offsetof(WeatherBufferData, camera_rvec) == 64);
    static_assert(offsetof(WeatherBufferData, camera_uvec) == 80);
    static_assert(offsetof(WeatherBufferData, fall_dir) == 96);
    static_assert(offsetof(WeatherBufferData, region_center) == 112);
    static_assert(offsetof(WeatherBufferData, region_rvec) == 128);
    static_assert(offsetof(WeatherBufferData, region_uvec) == 144);
    static_assert(offsetof(WeatherBufferData, region_fvec) == 160);
    static_assert(offsetof(WeatherBufferData, region_half_extents) == 176);
    static_assert(offsetof(WeatherBufferData, flags) == 188);
    static_assert(offsetof(WeatherBufferData, column_cols) == 192);
    static_assert(offsetof(WeatherBufferData, column_origin) == 200);
    static_assert(offsetof(WeatherBufferData, color) == 208);
    static_assert(sizeof(WeatherBufferData) == 224);

    // weather_flags bits in particle_vs.hlsl
    constexpr std::uint32_t weather_flag_snow = 1;
    constexpr std::uint32_t weather_flag_sphere = 2;
    constexpr std::uint32_t weather_flag_liquid_surface = 4;
    constexpr std::uint32_t weather_flag_ceiling = 8;

    // VertexLayout::weather_seed reads a seed as two float4s, and weather_columns is a Buffer<uint4>
    static_assert(sizeof(gr::d3d11::WeatherSeed) == 32);
    static_assert(sizeof(gr::d3d11::WeatherColumn) == 16);

    constexpr int batch_max_instance = 8192;
    constexpr UINT instance_slot = 1;
    constexpr UINT view_cbuffer_slot = 5;
    constexpr UINT weather_cbuffer_slot = 6;
    constexpr UINT weather_columns_srv_slot = 0;
    // Same 0-1-2 / 0-2-3 fan Renderer::poly's add_poly builds on the stock quads, so the winding is unchanged
    constexpr rf::ushort quad_indices[6] = {0, 1, 2, 0, 2, 3};
    // gr_d3d_3d_bitmap_stretched_square 0x00558E30 draws a shorter streak as an unrotated billboard
    constexpr double min_stretch_len_sq = 0.001;
}

namespace gr::d3d11
{
    ParticleRenderer::ParticleRenderer(ComPtr<ID3D11Device> device, ShaderManager& shader_manager,
        RenderContext& render_context) :
        device_{device}, render_context_(render_context),
        instance_ring_buffer_{batch_max_instance, D3D11_BIND_VERTEX_BUFFER, device_, render_context.device_context()}
    {
        billboard_vertex_shader_ = shader_manager.get_vertex_shader(VertexShaderId::particle);
        stretched_vertex_shader_ = shader_manager.get_vertex_shader(VertexShaderId::particle_stretched);
        weather_vertex_shader_ = shader_manager.get_vertex_shader(VertexShaderId::weather);
        std_pixel_shader_ = shader_manager.get_pixel_shader(PixelShaderId::standard);
        std_pixel_shader_no_gas_ = shader_manager.get_pixel_shader(PixelShaderId::standard_no_gas);

        CD3D11_BUFFER_DESC index_desc{sizeof(quad_indices), D3D11_BIND_INDEX_BUFFER, D3D11_USAGE_IMMUTABLE};
        D3D11_SUBRESOURCE_DATA index_data{quad_indices, 0, 0};
        CD3D11_BUFFER_DESC cbuffer_desc{
            sizeof(ParticleViewBufferData),
            D3D11_BIND_CONSTANT_BUFFER,
            D3D11_USAGE_DYNAMIC,
            D3D11_CPU_ACCESS_WRITE,
        };
        instancing_ok_ = billboard_vertex_shader_.vertex_shader && billboard_vertex_shader_.input_layout
            && stretched_vertex_shader_.vertex_shader && stretched_vertex_shader_.input_layout
            && SUCCEEDED(device_->CreateBuffer(&index_desc, &index_data, &quad_index_buffer_))
            && SUCCEEDED(device_->CreateBuffer(&cbuffer_desc, nullptr, &view_cbuffer_));
        if (!instancing_ok_) {
            xlog::warn("[Particles] Particles are drawn by the stock path: their shaders could not be loaded");
        }

        CD3D11_BUFFER_DESC weather_cbuffer_desc{
            sizeof(WeatherBufferData),
            D3D11_BIND_CONSTANT_BUFFER,
            D3D11_USAGE_DYNAMIC,
            D3D11_CPU_ACCESS_WRITE,
        };
        weather_ok_ = instancing_ok_ && weather_vertex_shader_.vertex_shader && weather_vertex_shader_.input_layout
            && SUCCEEDED(device_->CreateBuffer(&weather_cbuffer_desc, nullptr, &weather_cbuffer_));
        if (instancing_ok_ && !weather_ok_) {
            xlog::warn("[Particles] Weather is simulated on the CPU: its shader could not be loaded");
        }
    }

    void ParticleRenderer::set_pre_batch_callback(std::function<void()> callback)
    {
        pre_batch_callback_ = std::move(callback);
    }

    void ParticleRenderer::begin_run(const State& state, bool full)
    {
        if (!batch_pending_ && pre_batch_callback_) {
            pre_batch_callback_();
        }
        if (!batch_pending_ || state_ != state || full) {
            flush();
            state_ = state;
            capture_view();
        }
        batch_pending_ = true;
    }

    // The view instances are expanded with, as rotate_vertex would have read it
    void ParticleRenderer::capture_view()
    {
        const Projection& proj = render_context_.projection();
        run_view_.matrix = rf::gr::view_matrix;
        run_view_.pos = rf::gr::view_pos;
        run_view_.scale = rf::gr::matrix_scale;
        run_view_.far_clip_on = rf::gr_use_far_clip;
        run_view_.far_clip_z = rf::gr_far_clip_dist_scaled;
        run_view_.proj = {proj.scale_x(), proj.scale_y(), proj.scale_z(), proj.translate_z()};
    }

    GpuParticleInstance* ParticleRenderer::setup_instance(const State& state)
    {
        begin_run(state, instance_ring_buffer_.is_full(1));
        return instance_ring_buffer_.alloc(1);
    }

    void ParticleRenderer::flush_batch()
    {
        if (!batch_pending_) {
            return;
        }
        flush();
        cached_bm_handle_ = -1; // bitmap handles can be recycled between levels
    }

    void ParticleRenderer::upload_view()
    {
        if (view_uploaded_ && run_view_ == uploaded_view_) {
            return;
        }
        uploaded_view_ = run_view_;
        view_uploaded_ = true;

        const View& v = run_view_;
        ParticleViewBufferData data{};
        data.view_rvec = {v.matrix.rvec.x, v.matrix.rvec.y, v.matrix.rvec.z};
        data.view_uvec = {v.matrix.uvec.x, v.matrix.uvec.y, v.matrix.uvec.z};
        data.view_fvec = {v.matrix.fvec.x, v.matrix.fvec.y, v.matrix.fvec.z};
        data.view_pos = {v.pos.x, v.pos.y, v.pos.z};
        data.matrix_scale = {v.scale.x, v.scale.y, v.scale.z};
        data.far_clip_z = v.far_clip_z;
        data.far_clip_on = v.far_clip_on ? 1.0f : 0.0f;
        data.proj = v.proj;
        D3D11_MAPPED_SUBRESOURCE mapped;
        DF_GR_D3D11_CHECK_HR(
            render_context_.device_context()->Map(view_cbuffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        std::memcpy(mapped.pData, &data, sizeof(data));
        render_context_.device_context()->Unmap(view_cbuffer_, 0);
    }

    void ParticleRenderer::flush()
    {
        batch_pending_ = false;
        auto [start_instance, num_instance] = instance_ring_buffer_.submit();
        if (num_instance == 0) {
            return;
        }
        upload_view();
        render_context_.set_vertex_buffer(instance_ring_buffer_.get_buffer(), sizeof(GpuParticleInstance),
            instance_slot);
        render_context_.set_index_buffer(quad_index_buffer_);
        render_context_.set_vertex_shader(
            state_.primitive == Primitive::stretched ? stretched_vertex_shader_ : billboard_vertex_shader_);
        render_context_.bind_vs_cbuffer(view_cbuffer_slot, view_cbuffer_);
        render_context_.set_pixel_shader(state_.pixel_shader);
        render_context_.set_primitive_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        render_context_.set_mode(state_.mode);
        render_context_.set_textures(state_.textures[0], state_.textures[1]);
        render_context_.set_cull_mode(D3D11_CULL_NONE);
        render_context_.draw_indexed_instanced(static_cast<int>(std::size(quad_indices)), num_instance, 0, 0,
            start_instance);
        ++draws_;
    }

    bool ParticleRenderer::lookup_bitmap_size(int bm_handle, int& w, int& h)
    {
        if (bm_handle != cached_bm_handle_) {
            rf::bm::get_dimensions(bm_handle, &cached_bm_w_, &cached_bm_h_);
            cached_bm_handle_ = bm_handle;
        }
        w = cached_bm_w_;
        h = cached_bm_h_;
        return w != 0;
    }

    bool ParticleRenderer::add_particle_instance(const rf::Particle& particle, const rf::Vector3& pos,
        const rf::Vector3& last_pos)
    {
        // Frame selection, particle_render 0x00494B90. Stock rounds with floor(x + 0.5).
        int bm_handle = particle.first_frame_bitmap;
        const int num_frames = particle.num_frames;
        if (num_frames >= 2) {
            const float n = static_cast<float>(num_frames);
            int frame;
            if (particle.flags & rf::PTF_LOOP) {
                const float frac = particle.age - static_cast<float>(static_cast<int>(std::floor(particle.age)));
                frame = static_cast<int>(std::floor(frac * 15.0f / n + 0.5f));
            }
            else if (particle.flags2 & rf::PTF2_HOLD_LAST_FRAME) {
                frame = static_cast<int>(std::floor(
                    particle.age / (particle.age_pct_to_finish_vbm * particle.max_life_seconds) * n + 0.5f));
                if (frame >= num_frames) {
                    frame = num_frames - 1;
                }
            }
            else {
                frame = static_cast<int>(std::floor(particle.age / particle.max_life_seconds * n + 0.5f));
            }
            bm_handle += std::clamp(frame, 0, num_frames - 1);
        }

        rf::gr::Mode mode = (particle.flags & rf::PTF_GLOW) ? rf::gr::glow_3d_bitmap_mode : rf::gr::bitmap_3d_mode_no_z;
        if (particle.flags & rf::PTF_NO_Z_CHECK) {
            mode.set_zbuffer_type(rf::gr::ZBUFFER_TYPE_NONE);
        }

        // Stock feeds the colour through gr_set_color, so the quad picks it up from the screen
        // colour exactly where the dynamic geometry batcher would have.
        const rf::Color color = vertex_color_for_mode(mode, particle.clr_current);

        State state{
            Primitive::billboard,
            normalize_texture_handles_for_mode(mode, {bm_handle, -1}),
            mode,
            render_context_.has_gas_regions() ? std_pixel_shader_ : std_pixel_shader_no_gas_,
        };
        float angle = particle.bitmap_orient;
        if (particle.flags & rf::PTF_VEL_STRETCH) {
            // Stock squares the streak in x87 precision before comparing it with a double
            const rf::Vector3 dir = last_pos - pos;
            const double len_sq = static_cast<double>(dir.x) * dir.x + static_cast<double>(dir.y) * dir.y
                + static_cast<double>(dir.z) * dir.z;
            if (len_sq >= min_stretch_len_sq) {
                state.primitive = Primitive::stretched;
                GpuParticleInstance& inst = *setup_instance(state);
                inst.pos = {pos.x, pos.y, pos.z};
                inst.radius = particle.radius;
                inst.axes = {last_pos.x, last_pos.y, last_pos.z, 0.0f};
                inst.color = pack_color(color);
                return true;
            }
            angle = 0.0f;
        }
        return add_billboard_instance(pos, angle, particle.radius, bm_handle, color, state);
    }

    bool ParticleRenderer::add_billboard_instance(const rf::Vector3& pos, float angle, float radius, int bm_handle,
        rf::Color color, const State& state)
    {
        // Billboard build, gr_d3d_3d_bitmap_angle 0x00555AC0 -> 0x00555230: half extents stretched to the
        // bitmap's aspect, rotated by the angle. particle_vs.hlsl places the corners around the centre.
        float fx = radius;
        float fy = radius;
        if (bm_handle >= 0) {
            int bm_w, bm_h;
            if (!lookup_bitmap_size(bm_handle, bm_w, bm_h)) {
                return false; // stock draws nothing for a zero width bitmap
            }
            if (bm_w < bm_h) {
                fy = (static_cast<float>(bm_h) * radius) / static_cast<float>(bm_w);
            }
            else if (bm_h < bm_w) {
                fx = (static_cast<float>(bm_w) * radius) / static_cast<float>(bm_h);
            }
        }
        const float sin_a = std::sin(angle);
        const float cos_a = std::cos(angle);

        GpuParticleInstance& inst = *setup_instance(state);
        inst.pos = {pos.x, pos.y, pos.z};
        inst.radius = radius;
        inst.axes = {fx * cos_a, fy * sin_a, fx * sin_a, fy * cos_a};
        inst.color = pack_color(color);
        return true;
    }

    bool ParticleRenderer::add_billboard(const rf::Vector3& pos, float angle, float radius, rf::gr::Mode mode)
    {
        if (!instancing_ok_ || !rf::gr::perspective_projection) {
            return false;
        }
        const rf::Color color = vertex_color_for_mode(mode, rf::gr::screen.current_color);
        const int bm_handle = rf::gr::screen.current_texture_1;
        const State state{
            Primitive::billboard,
            normalize_texture_handles_for_mode(mode, {bm_handle, -1}),
            mode,
            render_context_.has_gas_regions() ? std_pixel_shader_ : std_pixel_shader_no_gas_,
        };
        add_billboard_instance(pos, angle, radius, bm_handle, color, state);
        return true; // a zero width bitmap is still handled, stock draws nothing either
    }

    bool ParticleRenderer::add_particle(const rf::Particle& particle)
    {
        // The debug draw adds a bounding box per particle that this path does not reproduce.
        if (rf::particle_draw_bbox || !instancing_ok_ || !rf::gr::perspective_projection) {
            return false;
        }
        ++rf::num_particles_rendered_this_frame; // stock counts every particle it visits
        add_particle_instance(particle, particle.pos, particle.last_pos);
        return true;
    }

    bool ParticleRenderer::render_emitter(rf::ParticleEmitter* emitter, float interp_dt, int* out_drawn, int* out_draws)
    {
        if (rf::particle_draw_bbox || !instancing_ok_ || !rf::gr::perspective_projection) {
            return false;
        }

        drawn_ = 0;
        draws_ = 0;
        cached_bm_handle_ = -1; // bitmap handles can be recycled between levels

        for (rf::Particle* p = emitter->particle_list.next; p != &emitter->particle_list; p = p->next) {
            rf::Vector3 pos = p->pos;
            rf::Vector3 last_pos = p->last_pos;
            if (interp_dt > 0.0f) {
                // The streak is drawn last_pos to pos, so both move to keep its length
                const rf::Vector3 delta = p->vel * interp_dt;
                pos += delta;
                last_pos += delta;
            }
            ++rf::num_particles_rendered_this_frame; // stock counts every particle it visits
            if (add_particle_instance(*p, pos, last_pos)) {
                ++drawn_;
            }
        }

        flush();
        if (out_drawn) {
            *out_drawn = drawn_;
        }
        if (out_draws) {
            *out_draws = draws_;
        }
        return true;
    }

    bool ParticleRenderer::upload_weather_seeds(const std::vector<WeatherSeed>& seeds)
    {
        weather_seed_buffer_.release();
        weather_seed_count_ = 0;
        if (!weather_ok_ || seeds.empty()) {
            return false;
        }
        CD3D11_BUFFER_DESC desc{
            static_cast<UINT>(seeds.size() * sizeof(WeatherSeed)),
            D3D11_BIND_VERTEX_BUFFER,
            D3D11_USAGE_IMMUTABLE,
        };
        D3D11_SUBRESOURCE_DATA data{seeds.data(), 0, 0};
        if (FAILED(device_->CreateBuffer(&desc, &data, &weather_seed_buffer_))) {
            return false;
        }
        weather_seed_count_ = static_cast<int>(seeds.size());
        return true;
    }

    bool ParticleRenderer::update_weather_columns(int slot, int num_cells, int first,
        const std::vector<WeatherColumn>& cells)
    {
        const int count = static_cast<int>(cells.size());
        if (!weather_ok_ || slot < 0 || num_cells <= 0 || first < 0 || count > num_cells - first) {
            return false;
        }
        if (static_cast<std::size_t>(slot) >= weather_columns_.size()) {
            weather_columns_.resize(slot + 1);
        }
        WeatherColumnBuffer& columns = weather_columns_[slot];
        if (columns.num_cells != num_cells) {
            columns = {};
            if (first != 0 || count != num_cells) {
                return false;
            }
            CD3D11_BUFFER_DESC desc{
                static_cast<UINT>(num_cells * sizeof(WeatherColumn)),
                D3D11_BIND_SHADER_RESOURCE,
                D3D11_USAGE_DEFAULT,
            };
            D3D11_SUBRESOURCE_DATA data{cells.data(), 0, 0};
            D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
            srv_desc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
            srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            srv_desc.Buffer.NumElements = static_cast<UINT>(num_cells);
            if (FAILED(device_->CreateBuffer(&desc, &data, &columns.buffer))
                || FAILED(device_->CreateShaderResourceView(columns.buffer, &srv_desc, &columns.srv))) {
                columns = {};
                return false;
            }
            columns.num_cells = num_cells;
            return true;
        }
        if (count > 0) {
            const D3D11_BOX box{
                static_cast<UINT>(first * sizeof(WeatherColumn)), 0, 0,
                static_cast<UINT>((first + count) * sizeof(WeatherColumn)), 1, 1,
            };
            render_context_.device_context()->UpdateSubresource(columns.buffer, 0, &box, cells.data(), 0, 0);
        }
        return true;
    }

    void ParticleRenderer::release_weather_columns()
    {
        weather_columns_.clear();
    }

    bool ParticleRenderer::draw_weather(const WeatherDraw& draw)
    {
        if (!weather_ok_ || !rf::gr::perspective_projection || draw.count > weather_seed_count_) {
            return false;
        }
        ID3D11ShaderResourceView* columns_srv = nullptr;
        if (draw.columns_slot >= 0) {
            if (static_cast<std::size_t>(draw.columns_slot) >= weather_columns_.size()
                || weather_columns_[draw.columns_slot].num_cells != draw.cols_x * draw.cols_z) {
                return false;
            }
            columns_srv = weather_columns_[draw.columns_slot].srv;
        }
        if (draw.count <= 0) {
            return true;
        }

        // Exactly where the CPU path's quads would have reached the framebuffer: after the open batch,
        // or after whatever starting a batch drains.
        if (batch_pending_) {
            flush();
        }
        else if (pre_batch_callback_) {
            pre_batch_callback_();
        }
        capture_view();
        upload_view();

        // Colour from the mode, exactly as add_billboard takes it from the screen state
        const rf::Color color = vertex_color_for_mode(draw.mode, draw.color);

        auto vec = [](const rf::Vector3& v) {
            return std::array<float, 3>{v.x, v.y, v.z};
        };
        WeatherBufferData data{};
        data.box_min = vec(draw.box_min);
        data.wrap_base = draw.wrap_base;
        data.box_size = vec(draw.box_size);
        data.salt = draw.salt;
        data.box_shift = vec(draw.box_shift);
        data.sim_time = draw.time;
        data.camera_pos = vec(draw.camera_pos);
        data.half_width = draw.half_width;
        data.camera_rvec = vec(draw.camera_orient.rvec);
        data.streak_half_len = draw.streak_half_len;
        data.camera_uvec = vec(draw.camera_orient.uvec);
        data.sway_amplitude = draw.sway_amplitude;
        data.fall_dir = vec(draw.fall_dir);
        data.sway_speed = draw.sway_speed;
        data.region_center = vec(draw.region_center);
        data.sway_phase = draw.sway_phase;
        data.region_rvec = vec(draw.region_orient.rvec);
        data.region_radius = draw.region_radius;
        data.region_uvec = vec(draw.region_orient.uvec);
        data.surface_y = draw.surface_y;
        data.region_fvec = vec(draw.region_orient.fvec);
        data.column_cell = draw.column_cell;
        data.region_half_extents = vec(draw.region_half_extents);
        data.flags = (draw.snow ? weather_flag_snow : 0) | (draw.sphere ? weather_flag_sphere : 0)
            | (draw.has_surface ? weather_flag_liquid_surface : 0) | (columns_srv ? weather_flag_ceiling : 0);
        data.column_cols = {draw.cols_x, draw.cols_z};
        data.column_origin = {draw.column_origin_x, draw.column_origin_z};
        data.color = {color.red / 255.0f, color.green / 255.0f, color.blue / 255.0f, color.alpha / 255.0f};
        D3D11_MAPPED_SUBRESOURCE mapped;
        DF_GR_D3D11_CHECK_HR(
            render_context_.device_context()->Map(weather_cbuffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        std::memcpy(mapped.pData, &data, sizeof(data));
        render_context_.device_context()->Unmap(weather_cbuffer_, 0);

        render_context_.set_vertex_buffer(weather_seed_buffer_, sizeof(WeatherSeed), instance_slot);
        render_context_.set_index_buffer(quad_index_buffer_);
        render_context_.set_vertex_shader(weather_vertex_shader_);
        render_context_.bind_vs_cbuffer(view_cbuffer_slot, view_cbuffer_);
        render_context_.bind_vs_cbuffer(weather_cbuffer_slot, weather_cbuffer_);
        render_context_.device_context()->VSSetShaderResources(weather_columns_srv_slot, 1, &columns_srv);
        render_context_.set_pixel_shader(
            render_context_.has_gas_regions() ? std_pixel_shader_ : std_pixel_shader_no_gas_);
        render_context_.set_primitive_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        render_context_.set_mode(draw.mode);
        const std::array<int, 2> textures = normalize_texture_handles_for_mode(draw.mode, {draw.bm_handle, -1});
        render_context_.set_textures(textures[0], textures[1]);
        render_context_.set_cull_mode(D3D11_CULL_NONE);
        render_context_.draw_indexed_instanced(static_cast<int>(std::size(quad_indices)), draw.count, 0, 0, 0);
        return true;
    }
}
