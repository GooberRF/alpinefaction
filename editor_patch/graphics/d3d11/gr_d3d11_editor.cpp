#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>
#include <xlog/xlog.h>
#include <common/gr_d3d11/gr_d3d11_af_lightmap.h>
#include <common/gr_d3d11/gr_d3d11_context.h>
#include <common/gr_d3d11/gr_d3d11_dynamic_geometry.h>
#include <common/gr_d3d11/gr_d3d11_mesh.h>
#include <common/gr_d3d11/gr_d3d11_particle.h>
#include <common/gr_d3d11/gr_d3d11_shader.h>
#include <common/gr_d3d11/gr_d3d11_solid.h>
#include <common/gr_d3d11/gr_d3d11_state.h>
#include <common/gr_d3d11/gr_d3d11_texture.h>
#include <common/gr_d3d11/gr_d3d11_util.h>
#include "../../brush_preview.h"
#include "../../command_line.h"
#include "../../editor_settings.h"
#include "../../headless_bake.h"
#include "../../terrain_preview.h"
#include "gr_d3d11_editor.h"
#include "gr_d3d11_editor_faces.h"

namespace gr::d3d11
{
    namespace
    {
        constexpr DXGI_FORMAT scene_format = DXGI_FORMAT_B8G8R8A8_UNORM;
        // The near plane the game uses. RED's own frustum culls; the depth range only orders what it keeps.
        constexpr float perspective_z_near = 0.1f;
        // RED clamps its far clip to 1000 for the projection scale (gr_d3d_setup_3d); never cut nearer than that
        constexpr float perspective_min_z_far = 1000.0f;
        // An orthographic view sees in front of and behind its eye. Depth clipping is off, so geometry past the range
        // still draws, clamped to its end; within it a 24-bit depth step is 2^16 / 2^24 = 1/256 of a metre.
        constexpr float ortho_depth_range = 32768.0f;
        // The stock projection's second flag bit: no screen position (behind the eye with software clipping)
        constexpr rf::ubyte vertex_projection_done = rf::gr::VF_PROJECTED | rf::gr::VF_BEHIND_EYE;
        // The per-face draw's (0x004E94B0) modes of a face without a lightmap, and of a lightmapped face
        constexpr rf::gr::Mode unlit_face_mode{rf::gr::TEXTURE_SOURCE_WRAP, rf::gr::COLOR_SOURCE_TEXTURE,
                                               rf::gr::ALPHA_SOURCE_TEXTURE, rf::gr::ALPHA_BLEND_ALPHA,
                                               rf::gr::ZBUFFER_TYPE_FULL, rf::gr::FOG_ALLOWED};
        constexpr rf::gr::Mode lightmapped_face_mode{rf::gr::TEXTURE_SOURCE_CLAMP_1_WRAP_0_MOD2X,
                                                     rf::gr::COLOR_SOURCE_TEXTURE, rf::gr::ALPHA_SOURCE_TEXTURE,
                                                     rf::gr::ALPHA_BLEND_ALPHA, rf::gr::ZBUFFER_TYPE_FULL,
                                                     rf::gr::FOG_ALLOWED};

        // View > Show Wireframe and Wireframe Overlay: depth tested and written, pulled this fraction of their
        // distance toward the eye to win over their faces
        constexpr rf::gr::Mode face_edge_mode{rf::gr::TEXTURE_SOURCE_NONE, rf::gr::COLOR_SOURCE_VERTEX,
                                              rf::gr::ALPHA_SOURCE_VERTEX, rf::gr::ALPHA_BLEND_NONE,
                                              rf::gr::ZBUFFER_TYPE_FULL, rf::gr::FOG_NOT_ALLOWED};
        constexpr float face_edge_pull = 1.0f / 256.0f;
        // The colour the level draw (0x0047AF51) gives the per-face draw
        constexpr rf::Color level_edge_color{255, 255, 255, 255};
        // View > Draw See-thru Portal Faces: the Terrain Tools mirror planes' mode_vertex_alpha and opacity
        constexpr rf::gr::Mode portal_plane_mode{rf::gr::TEXTURE_SOURCE_NONE, rf::gr::COLOR_SOURCE_VERTEX,
                                                 rf::gr::ALPHA_SOURCE_VERTEX, rf::gr::ALPHA_BLEND_ALPHA,
                                                 rf::gr::ZBUFFER_TYPE_READ, rf::gr::FOG_ALLOWED};

        // The Preferences' portal brush colour (the stock portal wireframes' colour)
        rf::Color portal_plane_color()
        {
            if (const EditorColorPrefs* prefs = editor_color_prefs()) {
                const Color& c = prefs->color(EditorColorPrefIndex::portal_brush);
                return {c.r, c.g, c.b, guide_plane_alpha};
            }
            return {0xFF, 0xFF, 0x00, guide_plane_alpha};
        }

        // outline_standard_vs's OutlineParams and outline_ps's OutlineColor
        constexpr UINT outline_params_vs_slot = 4;
        constexpr UINT outline_color_ps_slot = 2;

        // The projection with every depth face_edge_pull nearer and x and y unchanged: scaling both clip distances
        // keeps sz and scales tz. Only the perspective view draws edges.
        Projection pulled_toward_eye(const Projection& proj)
        {
            if (proj.is_orthographic()) {
                return proj;
            }
            constexpr float scale = 1.0f / (1.0f - face_edge_pull);
            return Projection{proj.scale_x(), proj.scale_y(), proj.z_near() * scale, proj.z_far() * scale};
        }

        // View > Show Decals, in the display modes that draw textures: with lightmaps, and textures only
        bool level_decals_drawn()
        {
            return g_editor_settings.show_decals && !view_room_colors && !view_see_through && !view_lightmaps_only;
        }

        // Direct3D 8 hardware snaps screen positions to 1/16 pixel before rasterising, Direct3D 11 to 1/256: snapped
        // here, two positions Direct3D 8 rasterised the same stay together
        float snap_to_d3d8_subpixel(float coord)
        {
            constexpr float subpixel_steps = 16.0f;
            return std::nearbyint(coord * subpixel_steps) / subpixel_steps;
        }

        // The level faces the textured brush preview draws over, left out of the level caches as RED's own leave them
        bool brush_preview_hides_face(const rf::GFace& face)
        {
            return brush_preview_hides(reinterpret_cast<const ::GFace&>(face));
        }

        // headless_bake.h lists them with the bake's other exit codes
        constexpr UINT renderer_failure_exit_code = 8;
        constexpr UINT device_lost_exit_code = 9;
        constexpr UINT renderer_error_exit_code = 10;

        constexpr const char* use_d3d9_advice =
            "To use the Direct3D 9 renderer instead, set Renderer=D3D9 in the "
            "[Alpine] section of alpine_editor.ini, or start the editor with -d3d9.";

        // Logs `msg`, then `advice`, as one line (the bake's log in a headless bake) and shows them, a paragraph each,
        // in a box the system draws: unlike editor_report_blocking, which holds its box back while a bake runs, no
        // modal loop dispatches paints back into the failing renderer. A failure while closing exits at once.
        [[noreturn]] void close_editor(const char* caption, const std::string& msg, UINT exit_code,
                                       const char* advice = nullptr)
        {
            static bool closing = false;
            if (std::exchange(closing, true)) {
                ExitProcess(exit_code);
            }
            editor_report(EditorReportLevel::error, "D3D11", advice ? std::format("{} {}", msg, advice) : msg, true);
            if (!headless_bake_active()) {
                const std::string text = advice ? std::format("{}\n\n{}", msg, advice) : msg;
                MessageBoxA(nullptr, text.c_str(), caption, MB_OK | MB_ICONERROR | MB_SERVICE_NOTIFICATION);
            }
            ExitProcess(exit_code);
        }

        // The editor cannot draw at all without the device, so it says how to switch renderers and exits
        [[noreturn]] void renderer_failure(const std::string& what, HRESULT hr)
        {
            close_editor("Direct3D 11", std::format("{} (error 0x{:08x}).", what, static_cast<unsigned>(hr)),
                         renderer_failure_exit_code, use_d3d9_advice);
        }

        constexpr const char* level_not_saved =
            "\n\nThe level could not be saved: changes since it was last saved are lost.";

        // The closing box's last paragraph, after RED's autosave of the level. The autosave is not run inside a level
        // load or save, whose own file is what the box then has to tell about.
        std::string autosave_level()
        {
            switch (level_file_op()) {
                case LevelFileOp::none:
                    break;
                case LevelFileOp::load:
                    return "\n\nThe level was being loaded, so nothing was saved. The level file being loaded was not "
                           "modified.";
                case LevelFileOp::save:
                    return "\n\nThe level was being saved, so it could not be saved again: the level file being saved "
                           "may be incomplete or damaged. Check it before using it. The last good copy, if any, is the "
                           "level's autosave (\"~\" before its extension), a backup or, after Save As, the file it "
                           "was saved from.";
                case LevelFileOp::autosave:
                    return level_not_saved;
            }
            if (const std::optional<std::string> saved_to = level_autosave_now()) {
                return std::format("\n\nThe level was saved to:\n{}\n\nRestart the level editor and open that file to "
                                   "continue.",
                                   *saved_to);
            }
            return level_not_saved;
        }

        // `save` is false when the device was lost during the autosave itself
        [[noreturn]] void close_after_device_loss(HRESULT reason, bool save)
        {
            std::string msg = std::format("The graphics device stopped working (error 0x{:08x}), so the level editor "
                                          "has to close.",
                                          static_cast<unsigned>(reason));
            if (!headless_bake_active()) {
                msg += save ? autosave_level() : level_not_saved;
            }
            close_editor("Graphics Device Lost", msg, device_lost_exit_code);
        }

        // The clip-space position the dynamic geometry gives a projected vertex
        void to_clip_space(const rf::gr::Vertex& v, const Projection& proj, EditorLightmapVertex& out)
        {
            const float ndc_x = (v.sx - rf::gr::screen.offset_x) / rf::gr::screen.clip_width * 2.0f - 1.0f;
            const float ndc_y = (v.sy - rf::gr::screen.offset_y) / rf::gr::screen.clip_height * -2.0f + 1.0f;
            const float w = proj.unproject_z(v.sw);
            out.x = ndc_x * w;
            out.y = ndc_y * w;
            out.z = v.sw * w;
            out.w = w;
        }

        // A face's vertices as the per-face draw (0x004E94B0) rotated them, or false for a broken edge loop
        bool gather_face_vertices(const rf::GFace* face, std::vector<rf::gr::Vertex>& verts,
                                  std::vector<const rf::GFaceVertex*>& face_verts)
        {
            verts.clear();
            face_verts.clear();
            const rf::GFaceVertex* fv = face->edge_loop;
            while (fv) {
                if (!fv->vertex || static_cast<int>(verts.size()) >= rf::max_face_vertices) {
                    return false;
                }
                rf::gr::Vertex& v = verts.emplace_back();
                v.world_pos = fv->vertex->rotated_pos;
                v.codes = static_cast<rf::ubyte>(fv->vertex->clip_codes);
                v.flags = 0;
                v.u1 = fv->texture_u;
                v.v1 = fv->texture_v;
                face_verts.push_back(fv);
                fv = fv->next;
                if (fv == face->edge_loop) {
                    return true;
                }
            }
            return false;
        }

        rf::ubyte clip_codes(const rf::gr::Vertex& v)
        {
            return v.codes;
        }

        rf::ubyte clip_codes(const rf::gr::Vertex* v)
        {
            return v->codes;
        }

        // Every vertex outside the same frustum plane: nothing of the polygon is in view
        template<typename T>
        bool outside_view(std::span<T> verts)
        {
            rf::ubyte and_code = 0xFF;
            for (const auto& v : verts) {
                and_code &= clip_codes(v);
            }
            return and_code != 0;
        }
    }

    [[noreturn]] void close_after_startup_error(const std::string& msg)
    {
        close_editor("Direct3D 11", std::format("The Direct3D 11 renderer could not start: {}.", msg),
                     renderer_failure_exit_code, use_d3d9_advice);
    }

    // The terrain preview draws a terrain's compiled chunk rooms, so while this lives their face lists are empty: the
    // solid renderer then leaves them out of their parents' caches and draws, as it does a destroyed breakable detail
    // room. `active` names it until it restores them.
    class EditorRenderer::TerrainChunkRoomsHidden
    {
    public:
        TerrainChunkRoomsHidden(rf::GRoom** rooms, int num_rooms, TerrainChunkRoomsHidden*& active) : active_{active}
        {
            // One slot: a nested hider's restore would orphan the outer one
            assert(!active_);
            for (int i = 0; i < num_rooms; ++i) {
                for (rf::GRoom* detail_room : rooms[i]->detail_rooms) {
                    hide(reinterpret_cast<::GRoom*>(detail_room));
                }
            }
            active_ = this;
        }

        ~TerrainChunkRoomsHidden()
        {
            restore();
        }

        TerrainChunkRoomsHidden(const TerrainChunkRoomsHidden&) = delete;
        TerrainChunkRoomsHidden& operator=(const TerrainChunkRoomsHidden&) = delete;

        void restore()
        {
            for (const Hidden& h : hidden_) {
                h.room->face_list_head = h.face_list_head;
                h.room->face_list_count = h.face_list_count;
            }
            hidden_.clear();
            active_ = nullptr;
        }

    private:
        struct Hidden
        {
            ::GRoom* room;
            ::GFace* face_list_head;
            int face_list_count;
        };

        void hide(::GRoom* room)
        {
            // A detail room can sit in several rooms' lists; only its first visit holds its faces
            const bool hidden = std::any_of(hidden_.begin(), hidden_.end(), [room](const Hidden& h) {
                return h.room == room;
            });
            if (!hidden && terrain_preview_hides_room(room)) {
                hidden_.push_back({room, room->face_list_head, room->face_list_count});
                room->face_list_head = nullptr;
                room->face_list_count = 0;
            }
        }

        TerrainChunkRoomsHidden*& active_;
        std::vector<Hidden> hidden_;
    };

    EditorRenderer::EditorRenderer(bool headless) : headless_{headless}, d3d11_lib_{L"d3d11.dll"}
    {
        if (!d3d11_lib_) {
            renderer_failure("d3d11.dll could not be loaded", HRESULT_FROM_WIN32(GetLastError()));
        }
        init_device();
        init_scene_target();

        state_manager_ = std::make_unique<StateManager>(device_);
        shader_manager_ = std::make_unique<ShaderManager>(device_);
        texture_manager_ = std::make_unique<TextureManager>(device_, context_);
        render_context_ =
            std::make_unique<RenderContext>(device_, context_, *state_manager_, *shader_manager_, *texture_manager_);
        dyn_geo_renderer_ = std::make_unique<DynamicGeometryRenderer>(device_, *shader_manager_, *render_context_);
        af_lightmap_renderer_ = std::make_unique<AfLightmapRenderer>(device_, context_);
        face_renderer_ = std::make_unique<EditorFaceRenderer>(device_, *shader_manager_, *render_context_,
                                                              *dyn_geo_renderer_, *af_lightmap_renderer_);
        solid_renderer_ = std::make_unique<SolidRenderer>(device_, *shader_manager_, *state_manager_,
                                                          *dyn_geo_renderer_, *render_context_, *af_lightmap_renderer_);
        mesh_renderer_ = std::make_unique<MeshRenderer>(device_, *shader_manager_, *state_manager_, *render_context_);
        particle_renderer_ = std::make_unique<ParticleRenderer>(device_, *shader_manager_, *render_context_);

        dyn_geo_renderer_->set_pre_flush_callback([this]() {
            face_renderer_->flush();
        });
        particle_renderer_->set_pre_batch_callback([this]() {
            face_renderer_->flush();
            dyn_geo_renderer_->flush();
        });

        render_context_->set_render_target(scene_rtv_, depth_stencil_view_);
        // The CPU frustum is the only cut, as with RED's Direct3D 8 renderer
        render_context_->set_depth_clip_enabled(false);
        render_context_->set_cull_mode(D3D11_CULL_NONE);
        render_context_->update_lights(true);

        overlay_vertex_shader_ = shader_manager_->get_vertex_shader(VertexShaderId::outline_standard);
        overlay_pixel_shader_ = shader_manager_->get_pixel_shader(PixelShaderId::outline);
        // No thickness: the vertex shader only transforms
        const std::array<float, 4> outline_params{1.0f, 1.0f, 0.0f, 0.0f};
        CD3D11_BUFFER_DESC params_desc{sizeof(outline_params), D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_IMMUTABLE};
        const D3D11_SUBRESOURCE_DATA params_data{outline_params.data(), 0, 0};
        DF_GR_D3D11_CHECK_HR(device_->CreateBuffer(&params_desc, &params_data, &overlay_vs_params_));
        CD3D11_BUFFER_DESC color_desc{sizeof(std::array<float, 4>), D3D11_BIND_CONSTANT_BUFFER};
        DF_GR_D3D11_CHECK_HR(device_->CreateBuffer(&color_desc, nullptr, &overlay_color_));
    }

    EditorRenderer::~EditorRenderer()
    {
        if (context_) {
            context_->ClearState();
        }
    }

    void EditorRenderer::init_device()
    {
        const auto create_device = d3d11_lib_.get_proc_address<PFN_D3D11_CREATE_DEVICE>("D3D11CreateDevice");
        if (!create_device) {
            renderer_failure("D3D11CreateDevice is missing from d3d11.dll", E_NOINTERFACE);
        }
        const D3D_FEATURE_LEVEL feature_levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            // The lowest level that runs the shaders (shader model 4.0)
            D3D_FEATURE_LEVEL_10_0,
        };
        UINT flags = editor_command_line_has(L"-d3d11-debug-layer") ? D3D11_CREATE_DEVICE_DEBUG : 0;
        D3D_FEATURE_LEVEL feature_level{};
        auto create = [&](D3D_DRIVER_TYPE driver_type) {
            HRESULT hr = create_device(nullptr, driver_type, nullptr, flags, feature_levels,
                                       static_cast<UINT>(std::size(feature_levels)), D3D11_SDK_VERSION, &device_,
                                       &feature_level, &context_);
            if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
                xlog::warn("[D3D11] The debug layer is not available");
                flags &= ~D3D11_CREATE_DEVICE_DEBUG;
                hr = create_device(nullptr, driver_type, nullptr, flags, feature_levels,
                                   static_cast<UINT>(std::size(feature_levels)), D3D11_SDK_VERSION, &device_,
                                   &feature_level, &context_);
            }
            return hr;
        };
        HRESULT hr = create(D3D_DRIVER_TYPE_HARDWARE);
        // A headless bake draws nothing anyone sees, so a machine without a GPU can still run one
        if (FAILED(hr) && headless_) {
            xlog::warn("[D3D11] No hardware device (0x{:08x}), using WARP", static_cast<unsigned>(hr));
            hr = create(D3D_DRIVER_TYPE_WARP);
        }
        if (FAILED(hr)) {
            renderer_failure("The Direct3D 11 device could not be created", hr);
        }
        init_error(device_);
        xlog::info("[D3D11] Feature level 0x{:x}", static_cast<int>(feature_level));

        ComPtr<IDXGIDevice1> dxgi_device;
        ComPtr<IDXGIAdapter> dxgi_adapter;
        hr = device_->QueryInterface(__uuidof(IDXGIDevice1), dxgi_device.put_void());
        if (SUCCEEDED(hr)) {
            dxgi_device->SetMaximumFrameLatency(1);
            hr = dxgi_device->GetAdapter(&dxgi_adapter);
        }
        if (SUCCEEDED(hr)) {
            hr = dxgi_adapter->GetParent(__uuidof(IDXGIFactory), dxgi_factory_.put_void());
        }
        if (FAILED(hr)) {
            renderer_failure("The DXGI factory of the Direct3D 11 device could not be found", hr);
        }
    }

    void EditorRenderer::init_scene_target()
    {
        scene_w_ = static_cast<UINT>(std::max(rf::gr::screen.max_w, 1));
        scene_h_ = static_cast<UINT>(std::max(rf::gr::screen.max_h, 1));

        CD3D11_TEXTURE2D_DESC scene_desc{scene_format, scene_w_, scene_h_, 1, 1,
                                         D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
        HRESULT hr = device_->CreateTexture2D(&scene_desc, nullptr, &scene_texture_);
        if (SUCCEEDED(hr)) {
            hr = device_->CreateRenderTargetView(scene_texture_, nullptr, &scene_rtv_);
        }
        ComPtr<ID3D11Texture2D> depth_stencil;
        CD3D11_TEXTURE2D_DESC depth_desc{DXGI_FORMAT_D24_UNORM_S8_UINT, scene_w_, scene_h_, 1, 1,
                                         D3D11_BIND_DEPTH_STENCIL};
        if (SUCCEEDED(hr)) {
            hr = device_->CreateTexture2D(&depth_desc, nullptr, &depth_stencil);
        }
        if (SUCCEEDED(hr)) {
            D3D11_DEPTH_STENCIL_VIEW_DESC view_desc{};
            view_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            view_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            hr = device_->CreateDepthStencilView(depth_stencil, &view_desc, &depth_stencil_view_);
        }
        if (FAILED(hr)) {
            renderer_failure(std::format("The {}x{} scene target could not be created", scene_w_, scene_h_), hr);
        }
    }

    void EditorRenderer::set_viewport_wnd(HWND hwnd)
    {
        flush_batches();
        hwnd_ = hwnd;
        std::erase_if(view_targets_, [](const auto& entry) {
            return !IsWindow(entry.first);
        });
    }

    EditorRenderer::ViewTarget* EditorRenderer::view_target(HWND hwnd, UINT w, UINT h)
    {
        auto it = view_targets_.find(hwnd);
        if (it == view_targets_.end()) {
            DXGI_SWAP_CHAIN_DESC desc{};
            desc.BufferDesc.Width = w;
            desc.BufferDesc.Height = h;
            desc.BufferDesc.Format = scene_format;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 1;
            desc.OutputWindow = hwnd;
            desc.Windowed = TRUE;
            desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            ViewTarget target;
            const HRESULT hr = dxgi_factory_->CreateSwapChain(device_, &desc, &target.swap_chain);
            if (FAILED(hr)) {
                handle_device_lost(hr);
                if (!presented_) {
                    renderer_failure("A Direct3D 11 swap chain could not be created", hr);
                }
                if (!std::exchange(swap_chain_failure_logged_, true)) {
                    xlog::error("[D3D11] A window's swap chain could not be created (0x{:08x}); it stays blank",
                                static_cast<unsigned>(hr));
                }
                return nullptr;
            }
            dxgi_factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
            target.w = w;
            target.h = h;
            it = view_targets_.emplace(hwnd, std::move(target)).first;
        }
        else if (it->second.w != w || it->second.h != h) {
            const HRESULT hr = it->second.swap_chain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
            if (FAILED(hr)) {
                handle_device_lost(hr);
                xlog::error("[D3D11] ResizeBuffers failed: 0x{:08x}", static_cast<unsigned>(hr));
                view_targets_.erase(it);
                return nullptr;
            }
            it->second.w = w;
            it->second.h = h;
        }
        return &it->second;
    }

    bool EditorRenderer::stop()
    {
        // From here on nothing reaches the GPU, the save that follows included
        if (std::exchange(stopped_, true)) {
            return false;
        }
        if (hidden_terrain_rooms_) {
            hidden_terrain_rooms_->restore();
        }
        return true;
    }

    void EditorRenderer::handle_device_lost(HRESULT hr)
    {
        if (stopped_ || (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET)) {
            return;
        }
        stop();
        const HRESULT reason = device_->GetDeviceRemovedReason();
        xlog::error("[D3D11] The device was lost (0x{:08x}, reason 0x{:08x})", static_cast<unsigned>(hr),
                    static_cast<unsigned>(reason));
        close_after_device_loss(FAILED(reason) ? reason : hr, true);
    }

    void EditorRenderer::close_after_error(const std::string& msg)
    {
        // A call failing inside the save below finds the renderer already stopped, so the save is not retried
        const bool first = stop();
        if (!first && device_removed()) {
            close_after_device_loss(device_->GetDeviceRemovedReason(), false);
        }
        std::string text = msg + ". The level editor has to close.";
        if (!headless_bake_active()) {
            text += first ? autosave_level() : level_not_saved;
        }
        close_editor("Direct3D 11 renderer", text, renderer_error_exit_code);
    }

    UINT EditorRenderer::max_texture_size() const
    {
        // D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION, for feature levels 10_0 and 10_1
        constexpr UINT feature_level_10_max_texture_size = 8192;
        return device_->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 ? D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
                                                                     : feature_level_10_max_texture_size;
    }

    bool EditorRenderer::device_removed() const
    {
        return FAILED(device_->GetDeviceRemovedReason());
    }

    void EditorRenderer::present()
    {
        RECT rect{};
        if (headless_ || stopped_ || !hwnd_ || !IsWindow(hwnd_) || !GetClientRect(hwnd_, &rect)) {
            return;
        }
        const LONG client_w = rect.right - rect.left;
        const LONG client_h = rect.bottom - rect.top;
        if (client_w <= 0 || client_h <= 0) {
            return;
        }
        const auto w = static_cast<UINT>(client_w);
        const auto h = static_cast<UINT>(client_h);
        ViewTarget* target = view_target(hwnd_, w, h);
        if (!target) {
            return;
        }
        ComPtr<ID3D11Texture2D> back_buffer;
        if (FAILED(target->swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), back_buffer.put_void()))) {
            return;
        }
        // A copy must not read a bound render target
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        const D3D11_BOX box{0, 0, 0, std::min(w, scene_w_), std::min(h, scene_h_), 1};
        context_->CopySubresourceRegion(back_buffer, 0, 0, 0, 0, scene_texture_, 0, &box);
        render_context_->set_render_target(scene_rtv_, depth_stencil_view_);
        const HRESULT hr = target->swap_chain->Present(0, 0);
        handle_device_lost(hr);
        presented_ = presented_ || SUCCEEDED(hr);
    }

    void EditorRenderer::flip()
    {
        flush_batches();
        present();
        render_context_->update_per_frame_constants();
    }

    void EditorRenderer::flush_batches()
    {
        particle_renderer_->flush_batch();
        face_renderer_->flush();
        dyn_geo_renderer_->flush();
    }

    rf::gr::Mode EditorRenderer::prepare_draw(rf::gr::Mode mode, bool overlay)
    {
        particle_renderer_->flush_batch();
        if (rf::gr::screen.fog_mode && (overlay || (fog_overlay_pass_ && !rf::sky_room_pass_active))) {
            mode.set_fog_type(rf::gr::FOG_NOT_ALLOWED);
        }
        return mode;
    }

    bool EditorRenderer::add_particle(const rf::Particle& particle)
    {
        return particle_renderer_->add_particle(particle);
    }

    bool EditorRenderer::batch_pending() const
    {
        return face_renderer_->has_pending() || dyn_geo_renderer_->has_pending();
    }

    void EditorRenderer::clear()
    {
        flush_batches();
        render_context_->clear();
    }

    void EditorRenderer::zbuffer_clear()
    {
        flush_batches();
        render_context_->zbuffer_clear();
    }

    void EditorRenderer::set_clip()
    {
        flush_batches();
        render_context_->set_clip();
    }

    void EditorRenderer::fog_set()
    {
        flush_batches();
        render_context_->fog_set();
    }

    void EditorRenderer::setup_3d()
    {
        // A particle run keeps the view it started with
        particle_renderer_->flush_batch();
        // Called from gr_d3d_setup_3d once it has set the scale and, for an orthographic view, the rotated z
        const rf::Vector3& scale = rf::gr::matrix_scale;
        if (rf::gr::perspective_projection) {
            const float z_far = std::isfinite(gr_far_clip_dist) ? std::max(gr_far_clip_dist, perspective_min_z_far)
                                                                : perspective_min_z_far;
            render_context_->update_view_proj_transform(
                Projection{scale.x / scale.z, scale.y / scale.z, perspective_z_near, z_far});
        }
        else {
            // gr_d3d_project_vertex divides the scaled rotated position by the constant rotated z
            render_context_->update_view_proj_transform(Projection::orthographic(
                scale.x / gr_ortho_rotated_z, scale.y / gr_ortho_rotated_z, -ortho_depth_range, ortho_depth_range));
        }
    }

    rf::ubyte EditorRenderer::project_vertex(rf::gr::Vertex* v)
    {
        if (v->flags & vertex_projection_done) {
            return v->flags;
        }
        const Projection& proj = render_context_->projection();
        const rf::Vector3 view_pos = v->world_pos / rf::gr::matrix_scale;
        rf::Vector3 proj_pos;
        if (proj.is_orthographic()) {
            // An orthographic view rotates every vertex to the same z, so what RED projects sits on the near plane,
            // over the meshes, and draws in order among itself as with Direct3D 8
            proj_pos = {view_pos.x * proj.scale_x(), view_pos.y * proj.scale_y(), 1.0f};
        }
        else {
            proj_pos = proj.project(view_pos);
        }
        v->sx = rf::gr::screen.clip_width / 2.0f * (proj_pos.x + 1.0f) + rf::gr::screen.offset_x;
        v->sy = rf::gr::screen.clip_height / 2.0f * (1.0f - proj_pos.y) + rf::gr::screen.offset_y;
        v->sw = proj_pos.z;
        v->flags |= rf::gr::VF_PROJECTED;
        return v->flags;
    }

    void EditorRenderer::bitmap(int bm_handle, int x, int y, int w, int h, int sx, int sy, int sw, int sh, bool flip_x,
                                bool flip_y, rf::gr::Mode mode)
    {
        dyn_geo_renderer_->bitmap(bm_handle, static_cast<float>(x), static_cast<float>(y), static_cast<float>(w),
                                  static_cast<float>(h), static_cast<float>(sx), static_cast<float>(sy),
                                  static_cast<float>(sw), static_cast<float>(sh), flip_x, flip_y,
                                  prepare_draw(mode, true));
    }

    void EditorRenderer::line_2d(float x1, float y1, float x2, float y2, rf::gr::Mode mode)
    {
        // As gr_d3d_line (0x004E1200): whole pixels, and a horizontal or vertical line reaches half a pixel past
        // its far end so that its last pixel is drawn
        x1 = std::trunc(x1);
        y1 = std::trunc(y1);
        x2 = std::trunc(x2);
        y2 = std::trunc(y2);
        if (x1 == x2) {
            (y1 < y2 ? y2 : y1) += 0.5f;
        }
        else if (y1 == y2) {
            (x1 < x2 ? x2 : x1) += 0.5f;
        }
        dyn_geo_renderer_->line_2d(x1, y1, x2, y2, prepare_draw(mode, true));
    }

    void EditorRenderer::line_3d(const rf::gr::Vertex& v0, const rf::gr::Vertex& v1, rf::gr::Mode mode)
    {
        // Direct3D 8 puts pixel centres at whole screen coordinates, Direct3D 11 half a pixel further on, so a
        // line drawn twice at the same x would cover two pixel columns
        rf::gr::Vertex centred[2] = {v0, v1};
        for (rf::gr::Vertex& v : centred) {
            v.sx = snap_to_d3d8_subpixel(v.sx) + 0.5f;
            v.sy = snap_to_d3d8_subpixel(v.sy) + 0.5f;
        }
        dyn_geo_renderer_->line_3d(centred[0], centred[1], prepare_draw(mode, true));
    }

    void EditorRenderer::tmapper(int nv, const rf::gr::Vertex** vertices, int vertex_attributes, rf::gr::Mode mode)
    {
        const std::array<int, 2> tex_handles{rf::gr::screen.current_texture_1, rf::gr::screen.current_texture_2};
        dyn_geo_renderer_->add_poly(nv, vertices, vertex_attributes, tex_handles, prepare_draw(mode, false));
    }

    bool EditorRenderer::poly(int nv, rf::gr::Vertex** vertices, int vertex_attributes, rf::gr::Mode mode,
                              bool constant_sw, float sw)
    {
        if (nv < 3 || outside_view(std::span{vertices, static_cast<std::size_t>(nv)})) {
            return false;
        }
        const Projection& proj = render_context_->projection();
        for (int i = 0; i < nv; ++i) {
            rf::gr::Vertex* v = vertices[i];
            project_vertex(v);
            if (constant_sw) {
                v->sw = proj.is_orthographic() ? 1.0f
                                               : proj.project_z(std::max(sw / rf::gr::matrix_scale.z, 0.1f));
            }
        }
        tmapper(nv, const_cast<const rf::gr::Vertex**>(vertices), vertex_attributes, mode);
        return true;
    }

    bool EditorRenderer::face(rf::GSolid* solid, rf::GFace* face, rf::gr::Mode mode, int bm_handle)
    {
        return styled_face(face, [&] {
            return fill_face(solid, face, mode, bm_handle);
        });
    }

    bool EditorRenderer::lightmapped_face(rf::GSolid* solid, rf::GFace* face, rf::gr::Mode mode, int bm_handle,
                                          int lm_handle)
    {
        return styled_face(face, [&] {
            return fill_lightmapped_face(solid, face, mode, bm_handle, lm_handle);
        });
    }

    template<typename F>
    bool EditorRenderer::styled_face(rf::GFace* face, F&& fill)
    {
        if (!face_draw_) {
            return fill();
        }
        const std::span<const int> hidden_ids = face_draw_->hidden_edge_face_ids;
        const bool edges = face_draw_->style.edges
            && !(face && std::binary_search(hidden_ids.begin(), hidden_ids.end(), face->attributes.face_id));
        if (!face_draw_->style.fills) {
            return edges ? queue_face_edges(face) : gather_visible_face(face, rf::max_face_vertices);
        }
        const bool drawn = fill();
        if (drawn && edges) {
            queue_face_edges(face);
        }
        return drawn;
    }

    bool EditorRenderer::fill_face(rf::GSolid* solid, rf::GFace* face, rf::gr::Mode mode, int bm_handle)
    {
        // A face past RED's surfaces that an Alpine overflow chart lights draws lightmapped, as in the game
        AfOverflowFace overflow;
        if (mode == unlit_face_mode && !editor_textures_enabled && face && face->attributes.surface_index < 0
            && !face->attributes.is_portal() && engine::af_lightmap_overflow_face(solid, face, overflow)) {
            const auto lightmap_uv = [&overflow](const rf::GFaceVertex& fv, EditorLightmapVertex& out) {
                const bool charted = engine::af_lightmap_overflow_texel(overflow, fv.vertex->pos, out.u1, out.v1);
                out.lm_chart = charted ? static_cast<float>(overflow.chart) : -1.0f;
            };
            const bool drawn = queue_lightmapped_face(
                face, lightmapped_face_mode, bm_handle, -1, rf::max_face_vertices,
                [&](const rf::GFaceVertex& fv, std::size_t, EditorLightmapVertex& out) { lightmap_uv(fv, out); });
            if (drawn && level_decals_drawn()) {
                queue_face_decals(face, -1, lightmap_uv);
            }
            return drawn;
        }
        if (!gather_visible_face(face, rf::max_face_vertices)) {
            return false;
        }
        face_vert_ptrs_.clear();
        for (const rf::gr::Vertex& v : face_verts_) {
            face_vert_ptrs_.push_back(&v);
        }
        dyn_geo_renderer_->add_poly(static_cast<int>(face_verts_.size()), face_vert_ptrs_.data(), 0, {bm_handle, -1},
                                    prepare_draw(mode, false));
        // Textures only draws every face here, unlit
        if (level_decals_drawn()) {
            queue_face_decals(face, -1, [](const rf::GFaceVertex&, EditorLightmapVertex&) {});
        }
        return true;
    }

    bool EditorRenderer::fill_lightmapped_face(rf::GSolid* solid, rf::GFace* face, rf::gr::Mode mode, int bm_handle,
                                               int lm_handle)
    {
        if (!face || face->attributes.is_portal()) {
            return false;
        }
        AfLightmapFace chart;
        const bool charted = face->attributes.surface_index >= 0
            && engine::af_lightmap_face_setup(solid, face->attributes.surface_index, chart);
        // The page a level without stock lightmaps synthesises is never written: neutral, as in the solid caches
        if (!charted && lm_handle == engine::af_lightmap_synthesized_page_bm()) {
            lm_handle = -1;
        }
        const auto lightmap_uv = [&](const rf::GFaceVertex& fv, EditorLightmapVertex& out) {
            if (charted) {
                engine::af_lightmap_face_texel(chart, fv.vertex->pos, out.u1, out.v1);
                out.lm_chart = static_cast<float>(chart.chart);
            }
            else {
                out.u1 = fv.lightmap_u;
                out.v1 = fv.lightmap_v;
                out.lm_chart = -1.0f;
            }
        };
        const bool drawn = queue_lightmapped_face(
            face, mode, bm_handle, lm_handle, rf::max_face_vertices,
            [&](const rf::GFaceVertex& fv, std::size_t, EditorLightmapVertex& out) { lightmap_uv(fv, out); });
        if (drawn && level_decals_drawn()) {
            queue_face_decals(face, lm_handle, lightmap_uv);
        }
        return drawn;
    }

    template<typename F>
    void EditorRenderer::queue_face_decals(rf::GFace* face, int lm_handle, F&& lightmap_uv)
    {
        // Lit by the face's lightmap when their mode takes one, as in the solid caches. As stock, only a face flagged
        // with a decal draws its list.
        if (!(face->attributes.flags & (rf::FACE_HAS_DECAL | rf::FACE_HAS_LEVEL_DECAL))) {
            return;
        }
        for (const rf::DecalPoly* dp = face->decal_list; dp; dp = dp->next_for_face) {
            const rf::GDecal* decal = dp->my_decal;
            if (!decal) {
                continue;
            }
            const rf::gr::Mode decal_mode = determine_decal_mode(decal);
            const std::array<int, 2> textures = normalize_texture_handles_for_mode(decal_mode,
                                                                                   {decal->bitmap_id, lm_handle});
            // An overflow-charted face is lit by its chart with no lightmap handle
            const bool takes_lightmap = normalize_texture_handles_for_mode(decal_mode, {decal->bitmap_id, 0})[1] != -1;
            const int diffuse = pack_color(rf::Color{255, 255, 255, decal->alpha});
            queue_lightmapped_face(face, decal_mode, textures[0], textures[1], std::size(dp->uvs),
                                   [&](const rf::GFaceVertex& fv, std::size_t i, EditorLightmapVertex& out) {
                                       out.diffuse = diffuse;
                                       out.u0 = dp->uvs[i].x;
                                       out.v0 = dp->uvs[i].y;
                                       if (takes_lightmap) {
                                           lightmap_uv(fv, out);
                                       }
                                   });
        }
    }

    bool EditorRenderer::queue_face_edges(rf::GFace* face)
    {
        if (!gather_visible_face(face, rf::max_face_vertices)) {
            return false;
        }
        const Projection& proj = render_context_->projection();
        const Projection pulled = pulled_toward_eye(proj);
        for (rf::gr::Vertex& v : face_verts_) {
            v.sw = pulled.project_z(proj.unproject_z(v.sw));
        }
        for (std::size_t i = 0; i < face_verts_.size(); ++i) {
            const rf::gr::Vertex& v0 = face_verts_[i];
            const rf::gr::Vertex& v1 = face_verts_[(i + 1) % face_verts_.size()];
            face_edges_.push_back({v0.sx, v0.sy, v0.sw});
            face_edges_.push_back({v1.sx, v1.sy, v1.sw});
        }
        return true;
    }

    bool EditorRenderer::gather_visible_face(const rf::GFace* face, std::size_t max_vertices)
    {
        if (!face || !gather_face_vertices(face, face_verts_, face_vertex_refs_) || face_verts_.size() < 3
            || face_verts_.size() > max_vertices || outside_view(std::span{face_verts_})) {
            return false;
        }
        for (rf::gr::Vertex& v : face_verts_) {
            project_vertex(&v);
        }
        return true;
    }

    bool EditorRenderer::begin_face_draw(const rf::GSolid* solid)
    {
        // GSolid::draw has set the brush's colour, or the level draw's white
        const rf::Color& color = rf::gr::screen.current_color;
        FaceDraw draw{level_view_style_, rf::Color{color.red, color.green, color.blue, 255}};
        if (solid && solid == engine::level_solid()) {
            draw.hidden_edge_face_ids = selection_edges_.level_face_ids;
        }
        else if (std::binary_search(selection_edges_.solids.begin(), selection_edges_.solids.end(), solid)) {
            draw.style.edges = false;
        }
        face_draw_ = draw;
        return level_view_style_.fills;
    }

    void EditorRenderer::end_face_draw(rf::GSolid* solid)
    {
        if (!face_draw_) {
            return;
        }
        const FaceDraw draw = *face_draw_;
        face_draw_.reset();
        if (!face_edges_.empty()) {
            // A line takes its vertex colour from gr_screen
            const rf::Color saved_color = rf::gr::screen.current_color;
            rf::gr::screen.current_color = draw.edge_color;
            const rf::gr::Mode mode = prepare_draw(face_edge_mode, true);
            for (std::size_t i = 0; i + 1 < face_edges_.size(); i += 2) {
                rf::gr::Vertex v0{};
                rf::gr::Vertex v1{};
                v0.sx = face_edges_[i].sx;
                v0.sy = face_edges_[i].sy;
                v0.sw = face_edges_[i].sw;
                v1.sx = face_edges_[i + 1].sx;
                v1.sy = face_edges_[i + 1].sy;
                v1.sw = face_edges_[i + 1].sw;
                dyn_geo_renderer_->line_3d(v0, v1, mode);
            }
            rf::gr::screen.current_color = saved_color;
            face_edges_.clear();
        }
        if (draw.style.portal_planes && solid) {
            queue_portal_planes(solid);
        }
    }

    // The portal faces the per-face draw (0x004E85A0) left out, from both sides
    void EditorRenderer::queue_portal_planes(rf::GSolid* solid)
    {
        const rf::gr::Mode mode = prepare_draw(portal_plane_mode, true);
        const rf::Color color = portal_plane_color();
        for (rf::GFace& face : solid->face_list) {
            if (!face.attributes.is_portal() || face.attributes.is_show_sky()) {
                continue;
            }
            face_verts_.clear();
            bool closed = false;
            const rf::GFaceVertex* fv = face.edge_loop;
            // A loop gather_face_vertices rejects is left out
            while (fv && fv->vertex && static_cast<int>(face_verts_.size()) < rf::max_face_vertices) {
                rf::gr::Vertex& v = face_verts_.emplace_back();
                // Rotated into the view of the instance GSolid::draw started, as the per-face draw rotates
                project_to_screen(&v, &fv->vertex->pos);
                fv = fv->next;
                if (fv == face.edge_loop) {
                    closed = true;
                    break;
                }
            }
            if (!closed || face_verts_.size() < 3 || outside_view(std::span{face_verts_})) {
                continue;
            }
            face_vert_ptrs_.clear();
            for (rf::gr::Vertex& v : face_verts_) {
                project_vertex(&v);
                v.r = color.red;
                v.g = color.green;
                v.b = color.blue;
                v.a = color.alpha;
                face_vert_ptrs_.push_back(&v);
            }
            dyn_geo_renderer_->add_poly(static_cast<int>(face_verts_.size()), face_vert_ptrs_.data(),
                                        rf::gr::TMAP_FLAG_RGB | rf::gr::TMAP_FLAG_ALPHA, {-1, -1}, mode);
        }
    }

    template<typename F>
    bool EditorRenderer::queue_lightmapped_face(rf::GFace* face, rf::gr::Mode mode, int bm_handle, int lm_handle,
                                                std::size_t max_vertices, F&& fill)
    {
        static std::vector<EditorLightmapVertex> gpu_verts;
        if (!gather_visible_face(face, max_vertices)) {
            return false;
        }
        const Projection& proj = render_context_->projection();
        gpu_verts.resize(face_verts_.size());
        for (std::size_t i = 0; i < face_verts_.size(); ++i) {
            EditorLightmapVertex& out = gpu_verts[i];
            to_clip_space(face_verts_[i], proj, out);
            out.diffuse = -1;
            out.u0 = face_vertex_refs_[i]->texture_u;
            out.v0 = face_vertex_refs_[i]->texture_v;
            out.u1 = 0.0f;
            out.v1 = 0.0f;
            out.lm_chart = -1.0f;
            fill(*face_vertex_refs_[i], i, out);
        }
        face_renderer_->add_face(static_cast<int>(gpu_verts.size()), gpu_verts.data(), bm_handle, lm_handle,
                                 prepare_draw(mode, false));
        return true;
    }

    rf::bm::Format EditorRenderer::read_back_buffer(int x, int y, int w, int h, rf::ubyte* data)
    {
        flush_batches();
        return texture_manager_->read_back_buffer(scene_texture_, x, y, w, h, data);
    }

    void EditorRenderer::page_in(int bm_handle)
    {
        texture_manager_->page_in(bm_handle);
    }

    void EditorRenderer::texture_save_cache()
    {
        texture_manager_->save_cache();
    }

    void EditorRenderer::texture_flush_cache(bool force)
    {
        texture_manager_->flush_cache(force);
    }

    void EditorRenderer::texture_mark_dirty(int bm_handle)
    {
        texture_manager_->mark_dirty(bm_handle);
    }

    void EditorRenderer::texture_free(int bm_handle)
    {
        // A queued draw may still sample it
        flush_batches();
        texture_manager_->mark_dirty(bm_handle);
    }

    void EditorRenderer::texture_add_ref(int bm_handle)
    {
        texture_manager_->add_ref(bm_handle);
    }

    void EditorRenderer::texture_remove_ref(int bm_handle)
    {
        texture_manager_->remove_ref(bm_handle);
    }

    bool EditorRenderer::lock(int bm_handle, int section, rf::gr::LockInfo* lock)
    {
        return texture_manager_->lock(bm_handle, section, lock);
    }

    void EditorRenderer::unlock(rf::gr::LockInfo* lock)
    {
        texture_manager_->unlock(lock);
    }

    void EditorRenderer::get_texel(int bm_handle, float u, float v, rf::gr::Color* clr)
    {
        texture_manager_->get_texel(bm_handle, u, v, clr);
    }

    void EditorRenderer::forget_stale_room_caches(rf::GRoom* room)
    {
        auto forget = [this](rf::GRoom* r) {
            if (r->geo_cache && !room_caches_.contains(r->geo_cache)) {
                r->geo_cache = nullptr;
            }
        };
        forget(room);
        if (!room->is_detail) {
            for (rf::GRoom* detail_room : room->detail_rooms) {
                forget(detail_room);
            }
        }
    }

    void EditorRenderer::remember_room_caches(rf::GRoom* room)
    {
        auto remember = [this](const rf::GRoom* r) {
            if (r->geo_cache) {
                room_caches_.insert(r->geo_cache);
            }
        };
        remember(room);
        if (!room->is_detail) {
            for (rf::GRoom* detail_room : room->detail_rooms) {
                remember(detail_room);
            }
        }
    }

    // RED's View display mode and portal face option and the Alpine View options for the level caches. RED flushes them
    // on a display mode change (0x0047ADCD), so a display mode change found here only happens if something set the flags
    // without one. It does not flush on a portal face option change, leaving its built caches as they were.
    void EditorRenderer::sync_solid_options()
    {
        SolidDisplayMode display_mode = SolidDisplayMode::normal;
        if (view_room_colors) {
            display_mode = SolidDisplayMode::room_colors;
        }
        else if (view_see_through) {
            display_mode = SolidDisplayMode::see_through;
        }
        else if (editor_textures_enabled) {
            display_mode = SolidDisplayMode::textures_only;
        }
        const bool decals_drawn = level_decals_drawn();
        const SolidCacheOptions options{
            .display_mode = display_mode,
            .invisible_faces_drawn = g_editor_settings.show_invisible_faces,
            .portal_faces_drawn = view_portal_faces != 0,
            .hides_face = brush_preview_hides_face,
            .level_decals_hidden = !decals_drawn,
            .edges_built = g_editor_settings.show_wireframe || g_editor_settings.wireframe_overlay,
            .portal_planes_built = view_portal_option == VIEW_PORTAL_FACES_SEE_THRU,
        };
        if (solid_renderer_->cache_options() != options) {
            clear_solid_cache();
            solid_renderer_->set_cache_options(options);
        }
        solid_renderer_->set_two_sided(g_editor_settings.show_back_faces);
        // The other decals, drawn over the caches each frame
        rf::gr::decals_enabled = decals_drawn;
    }

    template<typename F>
    void EditorRenderer::render_solid_overlays(F&& draw)
    {
        if (!level_view_style_.edges && !level_view_style_.portal_planes) {
            return;
        }
        render_context_->set_vertex_shader(overlay_vertex_shader_);
        render_context_->set_pixel_shader(overlay_pixel_shader_);
        render_context_->bind_vs_cbuffer(outline_params_vs_slot, overlay_vs_params_);
        render_context_->bind_ps_cbuffer(outline_color_ps_slot, overlay_color_);
        render_context_->set_cull_mode(D3D11_CULL_NONE);
        const auto pass = [&](SolidOverlay what, rf::gr::Mode mode, const rf::Color& color) {
            const std::array<float, 4> rgba{color.red / 255.0f, color.green / 255.0f, color.blue / 255.0f,
                                            color.alpha / 255.0f};
            context_->UpdateSubresource(overlay_color_, 0, nullptr, rgba.data(), 0, 0);
            render_context_->set_mode(mode);
            draw(what);
        };
        if (level_view_style_.edges) {
            const Projection proj = render_context_->projection();
            render_context_->update_view_proj_transform(pulled_toward_eye(proj));
            pass(SolidOverlay::edges, face_edge_mode, level_edge_color);
            render_context_->update_view_proj_transform(proj);
        }
        if (level_view_style_.portal_planes) {
            pass(SolidOverlay::portal_planes, portal_plane_mode, portal_plane_color());
        }
        // Puts the context's own buffers back in the outline shaders' slots, as after the game's outlines
        render_context_->invalidate_mode();
    }

    void EditorRenderer::render_solid(rf::GSolid* solid, rf::GRoom** rooms, int num_rooms)
    {
        flush_batches();
        sync_solid_options();
        for (int i = 0; i < num_rooms; ++i) {
            forget_stale_room_caches(rooms[i]);
        }
        {
            TerrainChunkRoomsHidden hidden{rooms, num_rooms, hidden_terrain_rooms_};
            if (level_view_style_.fills) {
                solid_renderer_->render_solid(solid, rooms, num_rooms);
            }
            render_solid_overlays([&](SolidOverlay what) {
                solid_renderer_->render_overlay(what, solid, rooms, num_rooms, selection_edges_.level_face_ids);
            });
        }
        for (int i = 0; i < num_rooms; ++i) {
            remember_room_caches(rooms[i]);
        }
    }

    void EditorRenderer::render_sky_room(rf::GRoom* room)
    {
        flush_batches();
        sync_solid_options();
        forget_stale_room_caches(room);
        if (level_view_style_.fills) {
            rf::Vector3 sky_pos;
            rf::Matrix3 sky_orient;
            solid_renderer_->render_sky_room(room, sky_pos, sky_orient);
        }
        render_solid_overlays([&](SolidOverlay what) {
            solid_renderer_->render_sky_room_overlay(what, room);
        });
        remember_room_caches(room);
        render_context_->set_sky_room(false);
    }

    void EditorRenderer::render_alpha_detail_room(rf::GRoom* room, rf::GSolid* solid)
    {
        flush_batches();
        // Their edges come with render_solid's
        if (!level_view_style_.fills || terrain_preview_hides_room(reinterpret_cast<const ::GRoom*>(room))) {
            return;
        }
        sync_solid_options();
        forget_stale_room_caches(room);
        solid_renderer_->render_alpha_detail(room, solid);
        remember_room_caches(room);
    }

    void EditorRenderer::render_room_liquid_surface(rf::GSolid* solid, rf::GRoom* room)
    {
        flush_batches();
        if (!level_view_style_.fills) {
            return;
        }
        sync_solid_options();
        forget_stale_room_caches(room);
        {
            TerrainChunkRoomsHidden hidden{&room, 1, hidden_terrain_rooms_};
            solid_renderer_->render_room_liquid_surface(solid, room);
        }
        remember_room_caches(room);
    }

    void EditorRenderer::clear_solid_cache()
    {
        solid_renderer_->clear_cache();
        room_caches_.clear();
    }

    void EditorRenderer::reset_solid_cache_after_boolean()
    {
        solid_renderer_->reset_cache_after_boolean();
    }

    bool EditorRenderer::upload_af_lightmap_atlas(const alpine_lightmap::ReadResult& section,
                                                  const std::vector<std::uint8_t>& blocks)
    {
        flush_batches();
        const bool uploaded = af_lightmap_renderer_->upload(section, blocks);
        // The lights buffer carries the atlas constants
        render_context_->update_lights(true);
        return uploaded;
    }

    void EditorRenderer::release_af_lightmap_atlas()
    {
        flush_batches();
        af_lightmap_renderer_->release();
        render_context_->update_lights(true);
    }

    bool EditorRenderer::af_lightmap_atlas_live() const
    {
        return af_lightmap_renderer_->live();
    }

    void EditorRenderer::render_v3d_vif(rf::VifLodMesh* lod_mesh, int lod_index, const rf::Vector3& pos,
                                        const rf::Matrix3& orient, const rf::MeshRenderParams& params,
                                        bool skip_ambient_cache)
    {
        flush_batches();
        mesh_renderer_->render_v3d_vif(lod_mesh, lod_index, pos, orient, params, skip_ambient_cache);
    }

    void EditorRenderer::render_character_vif(rf::VifLodMesh* lod_mesh, int lod_index, const rf::Vector3& pos,
                                              const rf::Matrix3& orient, const rf::CharacterInstance* ci,
                                              const rf::MeshRenderParams& params, bool skip_ambient_cache)
    {
        flush_batches();
        mesh_renderer_->render_character_vif(lod_mesh, lod_index, pos, orient, ci, params, skip_ambient_cache);
    }

    void EditorRenderer::clear_vif_cache(rf::VifLodMesh* lod_mesh)
    {
        mesh_renderer_->clear_vif_cache(lod_mesh);
    }

    void EditorRenderer::page_in_v3d_mesh(rf::VifLodMesh* lod_mesh, rf::MeshMaterial* materials, int num_materials)
    {
        mesh_renderer_->page_in_v3d_mesh(lod_mesh, materials, num_materials);
    }

    void EditorRenderer::page_in_character_mesh(rf::VifLodMesh* lod_mesh)
    {
        mesh_renderer_->page_in_character_mesh(lod_mesh);
    }

    void EditorRenderer::set_mesh_bounds(const rf::Vector3& center, float radius)
    {
        render_context_->set_mesh_bounds(center, radius);
    }

    void EditorRenderer::clear_mesh_lights()
    {
        render_context_->update_lights();
        render_context_->clear_mesh_bounds();
    }
}
