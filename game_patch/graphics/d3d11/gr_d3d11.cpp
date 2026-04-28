#include <cassert>
#include <dxgi1_4.h>
#include <dxgi1_5.h>
#include <xlog/xlog.h>
#include "../../rf/gr/gr.h"
#include "../../rf/v3d.h"
#include "../../rf/os/frametime.h"
#include "../../rf/os/os.h"
#include "../../bmpman/bmpman.h"
#include "../../main/main.h"
#include "../../misc/alpine_settings.h"
#include "../gr.h"
#include "gr_d3d11.h"
#include "gr_d3d11_context.h"
#include "gr_d3d11_shader.h"
#include "gr_d3d11_texture.h"
#include "gr_d3d11_state.h"
#include "gr_d3d11_dynamic_geometry.h"
#include "gr_d3d11_solid.h"
#include "gr_d3d11_mesh.h"
#include "gr_d3d11_entity_shadow.h"
#include "gr_d3d11_outline.h"
#include "gr_d3d11_gamma.h"

namespace gr::d3d11
{
    constexpr DXGI_FORMAT swap_chain_format = DXGI_FORMAT_B8G8R8A8_UNORM;

    Renderer::Renderer(HWND hwnd) : hwnd_{hwnd}, d3d11_lib_{L"d3d11.dll"}
    {
        if (!d3d11_lib_) {
            RF_DEBUG_ERROR("Failed to load d3d11.dll");
        }
        init_device();
        // Snapshot settings that must stay consistent across the swap chain / back buffer
        // setup and the flip() path. Changes require a restart to take effect, so the
        // user doesn't end up with a half-applied state mid-session.
        low_frame_latency_ = g_alpine_system_config.low_frame_latency;
        skip_gamma_pass_ = g_alpine_system_config.skip_gamma_pass;
        allow_tearing_ = g_alpine_system_config.allow_tearing;
        init_swap_chain(hwnd);
        init_back_buffer(g_game_config.msaa_level);
        init_depth_stencil_buffer(g_game_config.msaa_level);

        state_manager_ = std::make_unique<StateManager>(device_);
        shader_manager_ = std::make_unique<ShaderManager>(device_);
        texture_manager_ = std::make_unique<TextureManager>(device_, context_);
        render_context_ = std::make_unique<RenderContext>(device_, context_, *state_manager_, *shader_manager_, *texture_manager_);
        dyn_geo_renderer_ = std::make_unique<DynamicGeometryRenderer>(device_, *shader_manager_, *render_context_);
        solid_renderer_ = std::make_unique<SolidRenderer>(device_, *shader_manager_, *state_manager_, *dyn_geo_renderer_, *render_context_);
        mesh_renderer_ = std::make_unique<MeshRenderer>(device_, *shader_manager_, *state_manager_, *render_context_);
        entity_shadow_renderer_ = std::make_unique<EntityShadowRenderer>(device_, *shader_manager_, *mesh_renderer_);
        outline_renderer_ = std::make_unique<OutlineRenderer>(device_, *shader_manager_, *state_manager_, *render_context_);
        gamma_pass_ = std::make_unique<GammaPass>(device_, *shader_manager_);

        // Flush pending outlines before each dyn_geo draw. This ensures outlines
        // render behind transparent effects (smoke, particles, explosions) that are
        // batched in the dyn_geo renderer during scene rendering. Without this,
        // the dyn_geo batcher can flush particles to the framebuffer mid-scene
        // (on state changes or at explicit flush points like render_alpha_detail_room),
        // and outlines would later render on top of those already-drawn particles.
        dyn_geo_renderer_->set_pre_flush_callback([this]() {
            outline_renderer_->flush(*mesh_renderer_);
        });

        render_context_->set_render_target(default_render_target_view_, depth_stencil_view_);
        render_context_->set_cull_mode(D3D11_CULL_BACK);

        rf::gr::screen.depthbuffer_type = rf::gr::DEPTHBUFFER_Z;

        // Disable software vertex clipping
        auto& gr_needs_software_clipping = addr_as_ref<bool>(0x005A445A);
        gr_needs_software_clipping = false;
    }

    Renderer::~Renderer()
    {
        if (context_) {
            context_->ClearState();
        }
        if (rf::gr::screen.window_mode == rf::gr::FULLSCREEN) {
            swap_chain_->SetFullscreenState(FALSE, nullptr);
        }
        if (frame_latency_wait_handle_) {
            CloseHandle(frame_latency_wait_handle_);
            frame_latency_wait_handle_ = nullptr;
        }
    }

    void Renderer::window_active()
    {
        if (rf::gr::screen.window_mode == rf::gr::FULLSCREEN) {
            xlog::info("Entering full screen");
            ShowWindow(hwnd_, SW_RESTORE);
            set_fullscreen_state(true);
        }
    }

    void Renderer::window_inactive()
    {
        if (rf::gr::screen.window_mode == rf::gr::FULLSCREEN) {
            xlog::info("Exiting full screen");
            set_fullscreen_state(false);
            ShowWindow(hwnd_, SW_MINIMIZE);
        }
    }

    void Renderer::set_fullscreen_state(bool fullscreen)
    {
        DF_GR_D3D11_CHECK_HR(
            swap_chain_->SetFullscreenState(fullscreen, nullptr)
        );
        // unref swapchain resources before calling ResizeBuffers
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        back_buffer_.release();
        back_buffer_rtv_.release();
        scene_texture_.release();
        scene_texture_srv_.release();
        default_render_target_.release();
        default_render_target_view_.release();
        DF_GR_D3D11_CHECK_HR(
            swap_chain_->ResizeBuffers(0, rf::gr::screen.max_w, rf::gr::screen.max_h, DXGI_FORMAT_UNKNOWN, swap_chain_flags_)
        );
        // get back buffer from the swap chain after it has been resized
        init_back_buffer(g_game_config.msaa_level);
        render_context_->set_render_target(default_render_target_view_, depth_stencil_view_);
    }

    void Renderer::init_device()
    {
        auto pD3D11CreateDevice = d3d11_lib_.get_proc_address<PFN_D3D11_CREATE_DEVICE>("D3D11CreateDevice");
        if (!pD3D11CreateDevice) {
            RF_DEBUG_ERROR("Cannot find D3D11CreateDevice procedure");
        }

        // D3D_FEATURE_LEVEL feature_levels[] = {
        //     D3D_FEATURE_LEVEL_9_1,
        //     D3D_FEATURE_LEVEL_9_2,
        //     D3D_FEATURE_LEVEL_9_3,
        //     D3D_FEATURE_LEVEL_10_0,
        //     D3D_FEATURE_LEVEL_10_1,
        //     D3D_FEATURE_LEVEL_11_0,
        //     D3D_FEATURE_LEVEL_11_1
        // };

        DWORD flags = 0;
    //#ifndef NDEBUG
        // Requires Windows 10 SDK
        //flags |= D3D11_CREATE_DEVICE_DEBUG;
    //#endif
        D3D_FEATURE_LEVEL feature_level_supported;
        DF_GR_D3D11_CHECK_HR(
            pD3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                flags,
                // feature_levels,
                // std::size(feature_levels),
                nullptr,
                0,
                D3D11_SDK_VERSION,
                &device_,
                &feature_level_supported,
                &context_
            )
        );

        init_error(device_);

        xlog::info("D3D11 feature level: 0x{:x}", static_cast<int>(feature_level_supported));
    }

    void Renderer::init_swap_chain(HWND hwnd)
    {
        ComPtr<IDXGIDevice> dxgi_device;
        DF_GR_D3D11_CHECK_HR(
            device_->QueryInterface(&dxgi_device)
        );

        ComPtr<IDXGIAdapter> dxgi_adapter;
        DF_GR_D3D11_CHECK_HR(
            dxgi_device->GetAdapter(&dxgi_adapter)
        );

        ComPtr<IDXGIFactory> dxgi_factory;
        DF_GR_D3D11_CHECK_HR(
            dxgi_adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&dxgi_factory))
        );

        ComPtr<IDXGIFactory2> dxgi_factory2;
        ComPtr<IDXGIFactory3> dxgi_factory3;
        ComPtr<IDXGIFactory4> dxgi_factory4;
        dxgi_factory->QueryInterface(&dxgi_factory2);
        dxgi_factory->QueryInterface(&dxgi_factory3);
        dxgi_factory->QueryInterface(&dxgi_factory4);

        swap_chain_flags_ = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        // Both features require IDXGIFactory2 (flip-model swap chains). Without it
        // we're on the legacy CreateSwapChain path; clear the snapshots so flip()
        // doesn't later pass DXGI_PRESENT_ALLOW_TEARING to a non-tearing swap chain.
        if (!dxgi_factory2) {
            if (low_frame_latency_ || allow_tearing_) {
                xlog::warn("LowFrameLatency and AllowTearing require IDXGIFactory2; disabled");
            }
            low_frame_latency_ = false;
            allow_tearing_ = false;
        }
        if (low_frame_latency_) {
            if (rf::gr::screen.window_mode == rf::gr::FULLSCREEN) {
                low_frame_latency_ = false;
                xlog::warn("LowFrameLatency requires windowed or stretched window mode; disabled in fullscreen");
            }
            else {
                swap_chain_flags_ |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            }
        }
        if (allow_tearing_) {
            if (rf::gr::screen.window_mode == rf::gr::FULLSCREEN) {
                allow_tearing_ = false;
                xlog::warn("AllowTearing requires windowed or stretched window mode; disabled in fullscreen");
            }
            else {
                ComPtr<IDXGIFactory5> dxgi_factory5;
                if (SUCCEEDED(dxgi_factory->QueryInterface(&dxgi_factory5))) {
                    BOOL tearing_supported = FALSE;
                    if (SUCCEEDED(dxgi_factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                            &tearing_supported, sizeof(tearing_supported))) && tearing_supported) {
                        swap_chain_flags_ |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
                        xlog::info("D3D11 ALLOW_TEARING enabled");
                    }
                    else {
                        allow_tearing_ = false;
                        xlog::warn("ALLOW_TEARING requested but the system does not support it; disabled");
                    }
                }
                else {
                    allow_tearing_ = false;
                    xlog::warn("ALLOW_TEARING requires IDXGIFactory5; disabled");
                }
            }
        }

        if (dxgi_factory2) {
            DXGI_SWAP_CHAIN_DESC1 sc_desc1;
            ZeroMemory(&sc_desc1, sizeof(sc_desc1));
            sc_desc1.Width = rf::gr::screen.max_w;
            sc_desc1.Height = rf::gr::screen.max_h;
            sc_desc1.Format = swap_chain_format;
            // Note: flip modes do not support multi-sampling so always use one sample for backbuffer
            // When MSAA is enabled we will create a separate render target with multi-sampling enabled
            sc_desc1.SampleDesc.Count = 1;
            sc_desc1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sc_desc1.BufferCount = 2;
            if (dxgi_factory4) {
                sc_desc1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            }
            else if (dxgi_factory3) {
                sc_desc1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            }
            else {
                sc_desc1.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            }
            xlog::info("D3D11 swap effect: {}", static_cast<int>(sc_desc1.SwapEffect));
            sc_desc1.Flags = swap_chain_flags_;

            DXGI_SWAP_CHAIN_FULLSCREEN_DESC sc_fs_desc;
            ZeroMemory(&sc_fs_desc, sizeof(sc_fs_desc));
            sc_fs_desc.Windowed = rf::gr::screen.window_mode == rf::gr::WINDOWED;

            ComPtr<IDXGISwapChain1> swap_chain1;
            DF_GR_D3D11_CHECK_HR(
                dxgi_factory2->CreateSwapChainForHwnd(device_, hwnd, &sc_desc1, &sc_fs_desc, nullptr, &swap_chain1)
            );
            DF_GR_D3D11_CHECK_HR(swap_chain1->QueryInterface(&swap_chain_));

            // Configure the low-latency waitable-object pattern. This is the flip-model-correct
            // way to cap queued frames; the device-level SetMaximumFrameLatency API has known
            // pacing problems on flip-model swap chains with some drivers.
            if (swap_chain_flags_ & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
                ComPtr<IDXGISwapChain2> swap_chain2;
                if (SUCCEEDED(swap_chain_->QueryInterface(&swap_chain2))) {
                    HRESULT max_latency_hr = swap_chain2->SetMaximumFrameLatency(1);
                    if (FAILED(max_latency_hr)) {
                        xlog::warn("SetMaximumFrameLatency failed (hr=0x{:x}); low frame latency inactive", static_cast<unsigned>(max_latency_hr));
                    }
                    else {
                        frame_latency_wait_handle_ = swap_chain2->GetFrameLatencyWaitableObject();
                        if (frame_latency_wait_handle_) {
                            xlog::info("D3D11 low-latency waitable object acquired");
                        }
                        else {
                            xlog::warn("GetFrameLatencyWaitableObject returned null; low frame latency inactive");
                        }
                    }
                }
                else {
                    xlog::warn("IDXGISwapChain2 not available; low frame latency inactive");
                }
                // The FRAME_LATENCY_WAITABLE_OBJECT flag is baked into the swap chain at
                // creation; we can't strip it after the fact. If we didn't end up with a
                // usable wait handle, fullscreen stays blocked until the user disables
                // D3D11_LowFrameLatency and restarts.
                if (!frame_latency_wait_handle_) {
                    xlog::warn("Exclusive fullscreen will remain unavailable this session; "
                               "set D3D11_LowFrameLatency=0 in alpine_system.ini and restart to restore");
                }
            }
        }
        else {
            DXGI_SWAP_CHAIN_DESC sd;
            ZeroMemory(&sd, sizeof(sd));
            sd.BufferCount = rf::gr::screen.window_mode == rf::gr::FULLSCREEN ? 2 : 1;
            sd.BufferDesc.Width = rf::gr::screen.max_w;
            sd.BufferDesc.Height = rf::gr::screen.max_h;
            sd.BufferDesc.Format = swap_chain_format;
            sd.BufferDesc.RefreshRate.Numerator = 0;
            sd.BufferDesc.RefreshRate.Denominator = 1;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.OutputWindow = hwnd;
            sd.SampleDesc.Count = 1;
            sd.SampleDesc.Quality = 0;
            sd.Windowed = rf::gr::screen.window_mode == rf::gr::WINDOWED;
            sd.Flags = swap_chain_flags_;

            DF_GR_D3D11_CHECK_HR(
                dxgi_factory->CreateSwapChain(device_, &sd, &swap_chain_)
            );
        }

        // Alpine uses DXGI's flip presentation model, which requires ResizeBuffers
        // after mode transitions
        // Disable DXGI's built-in Alt+Enter fullscreen toggle. DXGI's auto-handling
        // calls SetFullscreenState internally without the app's knowledge, which causes
        // the next Present() call to return DXGI_ERROR_INVALID_CALL and crash. We handle
        // Alt+Enter ourselves via WM_SYSKEYDOWN in the message handler.
        dxgi_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    }

    void Renderer::init_back_buffer(const uint32_t msaa_level)
    {
        // Get a pointer to the back buffer
        DF_GR_D3D11_CHECK_HR(
            swap_chain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<LPVOID*>(&back_buffer_))
        );

        msaa_render_target_.release();

        // Always create back buffer RTV (used as gamma pass output, or as the main
        // render target when the gamma pass is skipped)
        DF_GR_D3D11_CHECK_HR(
            device_->CreateRenderTargetView(back_buffer_, nullptr, &back_buffer_rtv_)
        );

        // Create intermediate scene texture for gamma post-processing
        init_scene_texture();

        // Create a render-target view for the main rendering pass
        if (g_antialiasing && msaa_level >= 2 && msaa_level <= 8) {
            D3D11_TEXTURE2D_DESC desc;
            back_buffer_->GetDesc(&desc);
            desc.SampleDesc.Count = msaa_level;
            DF_GR_D3D11_CHECK_HR(
                device_->CreateTexture2D(&desc, nullptr, &msaa_render_target_)
            );
            default_render_target_ = msaa_render_target_;

            CD3D11_RENDER_TARGET_VIEW_DESC view_desc{D3D11_RTV_DIMENSION_TEXTURE2DMS};
            DF_GR_D3D11_CHECK_HR(
                device_->CreateRenderTargetView(
                    default_render_target_,
                    &view_desc,
                    &default_render_target_view_
                )
            );
        } else {
            // Without MSAA, render directly to the scene texture. Final output to
            // back_buffer_ happens in flip() — either via the gamma pass, or via
            // a CopyResource when the gamma pass is skipped.
            default_render_target_ = scene_texture_;
            DF_GR_D3D11_CHECK_HR(
                device_->CreateRenderTargetView(
                    default_render_target_,
                    nullptr,
                    &default_render_target_view_
                )
            );
        }
    }

    void Renderer::init_scene_texture()
    {
        D3D11_TEXTURE2D_DESC desc;
        back_buffer_->GetDesc(&desc);
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        DF_GR_D3D11_CHECK_HR(
            device_->CreateTexture2D(&desc, nullptr, &scene_texture_)
        );
        DF_GR_D3D11_CHECK_HR(
            device_->CreateShaderResourceView(scene_texture_, nullptr, &scene_texture_srv_)
        );
    }

    void Renderer::init_depth_stencil_buffer(const uint32_t msaa_level)
    {
        D3D11_TEXTURE2D_DESC depth_stencil_desc;
        ZeroMemory(&depth_stencil_desc, sizeof(depth_stencil_desc));
        depth_stencil_desc.Width = rf::gr::screen.max_w;
        depth_stencil_desc.Height = rf::gr::screen.max_h;
        depth_stencil_desc.MipLevels = 1;
        depth_stencil_desc.ArraySize = 1;
        depth_stencil_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        const bool use_msaa = g_antialiasing && msaa_level >= 2 && msaa_level <= 8;
        if (use_msaa) {
             depth_stencil_desc.SampleDesc.Count = msaa_level;
        } else {
             depth_stencil_desc.SampleDesc.Count = 1;
        }
        depth_stencil_desc.SampleDesc.Quality = 0;
        depth_stencil_desc.Usage = D3D11_USAGE_DEFAULT;
        depth_stencil_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

        ComPtr<ID3D11Texture2D> depth_stencil;
        DF_GR_D3D11_CHECK_HR(
            device_->CreateTexture2D(&depth_stencil_desc, nullptr, &depth_stencil)
        );

        D3D11_DEPTH_STENCIL_VIEW_DESC view_desc{};
        view_desc.ViewDimension = use_msaa
            ? D3D11_DSV_DIMENSION_TEXTURE2DMS
            : D3D11_DSV_DIMENSION_TEXTURE2D;

        DF_GR_D3D11_CHECK_HR(
            device_->CreateDepthStencilView(depth_stencil, &view_desc, &depth_stencil_view_)
        );
    }

    bool Renderer::is_sample_count_valid(const uint32_t sample_count) {
        switch (sample_count) {
            case 0:
                return false;
            case 1:
                return true;
            case 2:
            case 4:
            case 8: {
                UINT num_quality_levels = 0;
                const HRESULT hr = device_->CheckMultisampleQualityLevels(
                    swap_chain_format,
                    sample_count,
                    &num_quality_levels
                );
                return SUCCEEDED(hr) && num_quality_levels > 0;
            }
            default:
                return false;
        }
    }

    void Renderer::flush_render_targets() {
        texture_manager_->flush_render_targets();
        init_back_buffer(g_game_config.msaa_level);
        init_depth_stencil_buffer(g_game_config.msaa_level);
        render_context_
            ->set_render_target(default_render_target_view_, depth_stencil_view_);
    }

    void Renderer::bitmap(int bm_handle, int x, int y, int w, int h, int sx, int sy, int sw, int sh, bool flip_x, bool flip_y, rf::gr::Mode mode)
    {
        // Flush pending outlines before any 2D bitmap is committed to the framebuffer.
        // RF draws the scope overlay as a bitmap during HUD rendering. Because the
        // dyn_geo batcher flushes each batch when the texture/mode changes (see setup()),
        // the scope can end up in the framebuffer before zbuffer_clear fires and flushes
        // outlines — which would place outlines on top of the scope. By flushing outlines
        // here (on the first bitmap after 3D scene rendering), outlines land in the
        // framebuffer first, and subsequent bitmaps (scope etc.) commit on top of them.
        // If outlines were already flushed in zbuffer_clear this is a no-op.
        outline_renderer_->flush(*mesh_renderer_);
        dyn_geo_renderer_->bitmap(bm_handle,
            static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h),
            static_cast<float>(sx), static_cast<float>(sy), static_cast<float>(sw), static_cast<float>(sh),
            flip_x, flip_y, mode);
    }

    void Renderer::bitmap(int bm_handle, float x, float y, float w, float h, float sx, float sy, float sw, float sh, bool flip_x, bool flip_y, rf::gr::Mode mode)
    {
        outline_renderer_->flush(*mesh_renderer_);
        dyn_geo_renderer_->bitmap(bm_handle, x, y, w, h, sx, sy, sw, sh, flip_x, flip_y, mode);
    }

    void Renderer::page_in(int bm_handle)
    {
        texture_manager_->page_in(bm_handle);
    }

    void Renderer::clear()
    {
        dyn_geo_renderer_->flush();
        render_context_->clear();
    }

    void Renderer::zbuffer_clear()
    {
        // Flush queued outlines before clearing the depth buffer.
        // The fpgun rendering clears zbuffer before its setup_3d call,
        // so outlines must be rendered while scene depth data is still available.
        // Outlines are flushed first so that subsequently flushed dyn_geo draws
        // (player labels, crosshair, world HUD) always render on top of outlines.
        // The outline silhouette pass writes depth (DepthWriteMask = ALL) to prevent
        // liquid surfaces from alpha-blending over outline pixels. This is safe here
        // because the zbuffer is about to be cleared for the fpgun pass, and the
        // written depth values are geometrically correct (at the character's depth).
        // Note: the scope overlay (a bitmap) is drawn
        // BEFORE zbuffer_clear, causing the scope to reach the framebuffer before
        // this flush via the dyn_geo batcher's automatic state-change flush. The
        // bitmap() override handles that case by flushing outlines on the first
        // bitmap call, so this flush here is then a no-op and that path is also safe.
        outline_renderer_->flush(*mesh_renderer_);
        dyn_geo_renderer_->flush();
        render_context_->zbuffer_clear();
    }

    void Renderer::set_clip()
    {
        dyn_geo_renderer_->flush();
        render_context_->set_clip();
    }

    void Renderer::flip()
    {
        // Pace the CPU against the GPU's presentation queue before we do any
        // end-of-frame work. This is the canonical placement for the frame-latency
        // waitable object (MSFT samples wait at frame-start); it gates Present()
        // on the swap chain being ready to accept the next frame. The handle is
        // auto-reset and starts signaled, so the first wait returns immediately.
        if (frame_latency_wait_handle_) {
            DWORD wait_result = WaitForSingleObject(frame_latency_wait_handle_, 1000);
            if (wait_result == WAIT_TIMEOUT) {
                if (!frame_latency_stall_logged_) {
                    xlog::warn("Frame-latency wait timed out after 1000 ms; GPU may be stalled");
                    frame_latency_stall_logged_ = true;
                }
            }
            else if (wait_result == WAIT_FAILED) {
                xlog::warn("Frame-latency wait failed (GetLastError={}); disabling", GetLastError());
                CloseHandle(frame_latency_wait_handle_);
                frame_latency_wait_handle_ = nullptr;
            }
            else {
                frame_latency_stall_logged_ = false;
            }
        }
        outline_renderer_->flush_forced_xray(*mesh_renderer_);
        dyn_geo_renderer_->flush();
        entity_shadow_renderer_->render_debug_overlay(context_);
        entity_shadow_renderer_->unbind_shadow_resources(context_);
        if (msaa_render_target_) {
            // Resolve MSAA into scene_texture_ unconditionally. The final copy to
            // back_buffer_ happens below — either via the gamma pass, or via a
            // CopyResource when the gamma pass is skipped.
            context_->ResolveSubresource(scene_texture_, 0, msaa_render_target_, 0, swap_chain_format);
        }
        if (skip_gamma_pass_) {
            // Straight copy into the back buffer; cheaper than the gamma pixel shader
            // and avoids the state invalidate, while keeping scene_texture_ stable
            // for read_back_buffer. CopyResource forbids src/dst being bound as RTV,
            // and in the no-MSAA path scene_texture_ is bound via default_render_target_view_,
            // so unbind before the copy. set_render_target at end of flip() rebinds.
            context_->OMSetRenderTargets(0, nullptr, nullptr);
            context_->CopyResource(back_buffer_, scene_texture_);
        }
        else {
            // Gamma-corrects scene_texture_ into the back buffer. Runs unconditionally
            // even at gamma == 1.0 (where the shader degenerates to a copy) because
            // the per-frame cost is negligible and skipping the float comparison keeps
            // the hot path branchless.
            gamma_pass_->render(context_, scene_texture_srv_, back_buffer_rtv_, rf::gr::gamma);
            // Restore render context state after gamma pass overwrote shaders/layout/blend/etc.
            render_context_->invalidate_cached_state();
        }
        xlog::trace("Presenting frame {}", rf::frame_count);
        UINT sync_interval = g_alpine_system_config.vsync ? 1 : 0;
        // DXGI_PRESENT_ALLOW_TEARING can only be combined with sync_interval 0, and only
        // works when the swap chain was created with DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING.
        UINT present_flags = 0;
        if (allow_tearing_ && sync_interval == 0) {
            present_flags |= DXGI_PRESENT_ALLOW_TEARING;
        }
        DF_GR_D3D11_CHECK_HR(
            swap_chain_->Present(sync_interval, present_flags)
        );
        // Flip swap effect clears render target after Present call
        render_context_->set_render_target(default_render_target_view_, depth_stencil_view_);
        // Note: it would be better to call update_per_frame_constants after frametime_calculate
        render_context_->update_per_frame_constants();
    }

    void Renderer::texture_save_cache()
    {
        texture_manager_->save_cache();
    }

    void Renderer::texture_flush_cache(bool force)
    {
        texture_manager_->flush_cache(force);
    }

    void Renderer::texture_flush_non_user_cache()
    {
        texture_manager_->flush_non_user_cache();
    }

    void Renderer::texture_mark_dirty(int bm_handle)
    {
        texture_manager_->mark_dirty(bm_handle);
    }

    void Renderer::texture_add_ref(int bm_handle)
    {
        texture_manager_->add_ref(bm_handle);
    }

    void Renderer::texture_remove_ref(int bm_handle)
    {
        texture_manager_->remove_ref(bm_handle);
    }

    void Renderer::reset_sampler_states()
    {
        state_manager_->clear_sampler_state_cache();
    }

    bool Renderer::lock(int bm_handle, int section, rf::gr::LockInfo *lock)
    {
        return texture_manager_->lock(bm_handle, section, lock);
    }

    void Renderer::unlock(rf::gr::LockInfo *lock)
    {
        texture_manager_->unlock(lock);
    }

    void Renderer::get_texel(int bm_handle, float u, float v, rf::gr::Color *clr)
    {
        texture_manager_->get_texel(bm_handle, u, v, clr);
    }

    void Renderer::tmapper(int nv, const rf::gr::Vertex **vertices, int vertex_attributes, rf::gr::Mode mode)
    {
        std::array<int, 2> tex_handles{rf::gr::screen.current_texture_1, rf::gr::screen.current_texture_2};
        dyn_geo_renderer_->add_poly(nv, vertices, vertex_attributes, tex_handles, mode);
    }

    void Renderer::line_3d(const rf::gr::Vertex& v0, const rf::gr::Vertex& v1, rf::gr::Mode mode)
    {
        dyn_geo_renderer_->line_3d(v0, v1, mode);
    }

    void Renderer::line_2d(float x1, float y1, float x2, float y2, rf::gr::Mode mode)
    {
        dyn_geo_renderer_->line_2d(x1, y1, x2, y2, mode);
    }

    bool Renderer::poly(int nv, rf::gr::Vertex** vertices, int vertex_attributes, rf::gr::Mode mode, bool constant_sw, float sw)
    {
        rf::ubyte and_code = 0xFF;
        for (int i = 0; i < nv; ++i) {
            and_code &= vertices[i]->codes;
        }
        if (and_code) {
            return false;
        }
        for (int i = 0; i < nv; ++i) {
            auto v = vertices[i];
            if (!(v->flags & rf::gr::VF_PROJECTED)) {
                project_vertex(v);
            }
            if (constant_sw) {
                float unscaled_z = std::max(sw / rf::gr::matrix_scale.z, 0.1f);
                v->sw = render_context_->projection().project_z(unscaled_z);
            }
        }
        tmapper(nv, const_cast<const rf::gr::Vertex**>(vertices), vertex_attributes, mode);
        return true;
    }

    void Renderer::project_vertex(rf::gr::Vertex* v)
    {
        if (v->flags & rf::gr::VF_PROJECTED) {
            return;
        }

        rf::Vector3 unscaled_world_pos = v->world_pos / rf::gr::matrix_scale;
        auto& proj = render_context_->projection();
        auto proj_pos = proj.project(unscaled_world_pos);
        v->sx = rf::gr::screen.clip_width / 2.0f * (proj_pos.x + 1.0f) + rf::gr::screen.offset_x;
        v->sy = rf::gr::screen.clip_height / 2.0f * (1.0f - proj_pos.y) + rf::gr::screen.offset_y;
        v->sw = proj_pos.z;

        v->flags |= rf::gr::VF_PROJECTED;
    }

    bool Renderer::set_render_target(int bm_handle)
    {
        dyn_geo_renderer_->flush();
        if (bm_handle != -1) {
            ID3D11RenderTargetView* const render_target_view =
                texture_manager_->lookup_render_target(bm_handle);
            if (!render_target_view) {
                return false;
            }
            render_context_->set_render_target(render_target_view, depth_stencil_view_);
        } else {
            render_context_->set_render_target(default_render_target_view_, depth_stencil_view_);
        }
        if (render_target_bm_handle_ != -1) {
            texture_manager_->finish_render_target(render_target_bm_handle_);
        }
        render_target_bm_handle_ = bm_handle;
        return true;
    }

    rf::bm::Format Renderer::read_back_buffer([[maybe_unused]] int x, [[maybe_unused]] int y, int w, int h, rf::ubyte *data)
    {
        dyn_geo_renderer_->flush();
        // Resolve the current scene content into a readable texture.
        // With MSAA the scene lives in msaa_render_target_; without MSAA it is
        // already in scene_texture_ (which is also a non-MSAA texture).
        if (msaa_render_target_) {
            context_->ResolveSubresource(back_buffer_, 0, msaa_render_target_, 0, swap_chain_format);
            return texture_manager_->read_back_buffer(back_buffer_, x, y, w, h, data);
        }
        return texture_manager_->read_back_buffer(scene_texture_, x, y, w, h, data);
    }

    void Renderer::setup_3d(Projection proj)
    {
        render_context_->update_view_proj_transform(proj);
        // Only initialize outlines when rendering to the back buffer.
        // The rail gun scanner calls setup_3d while rendering to a small texture;
        // running begin_frame there would save the wrong projection and cause
        // outlines to be queued (and potentially flushed) into the scanner texture.
        if (render_target_bm_handle_ == -1) {
            outline_renderer_->begin_frame();
        }
    }

    void Renderer::set_far_clip(bool enabled)
    {
        render_context_->set_depth_clip_enabled(enabled);
    }

    void Renderer::render_solid(rf::GSolid* solid, rf::GRoom** rooms, int num_rooms)
    {
        dyn_geo_renderer_->flush();

        // Generate entity shadow map before rendering the scene
        entity_shadow_renderer_->generate_shadow_map(context_, *render_context_, rf::gr::eye_pos);

        // Restore render target and viewport after shadow pass
        // Use the current render target (may be a security camera/mirror texture, not the backbuffer)
        ID3D11RenderTargetView* current_rtv = default_render_target_view_;
        if (render_target_bm_handle_ != -1) {
            ID3D11RenderTargetView* rt = texture_manager_->lookup_render_target(render_target_bm_handle_);
            if (rt) current_rtv = rt;
        }
        render_context_->set_render_target(current_rtv, depth_stencil_view_);
        render_context_->set_clip();
        render_context_->set_cull_mode(D3D11_CULL_BACK);

        // Bind shadow resources for the scene pixel shader
        entity_shadow_renderer_->bind_shadow_resources(context_);

        solid_renderer_->render_solid(solid, rooms, num_rooms);
    }

    void Renderer::render_movable_solid(rf::GSolid* solid, const rf::Vector3& pos, const rf::Matrix3& orient)
    {
        dyn_geo_renderer_->flush();
        solid_renderer_->render_movable_solid(solid, pos, orient);
    }

    void Renderer::render_alpha_detail_room(rf::GRoom *room, rf::GSolid *solid)
    {
        dyn_geo_renderer_->flush();
        solid_renderer_->render_alpha_detail(room, solid);
    }

    void Renderer::render_sky_room(rf::GRoom *room, rf::Vector3& out_sky_transform_pos, rf::Matrix3& out_sky_transform_orient)
    {
        dyn_geo_renderer_->flush();
        solid_renderer_->render_sky_room(room, out_sky_transform_pos, out_sky_transform_orient);
    }

    void Renderer::render_room_liquid_surface(rf::GSolid* solid, rf::GRoom* room)
    {
        // Flush outlines before liquid renders. The outline silhouette pass writes
        // depth (DepthWriteMask=ALL), so outline pixels claim depth ownership.
        // When the liquid subsequently depth-tests at those pixels, it fails
        // (outline is closer than liquid), preventing liquid alpha-blend from
        // contaminating outline colors.
        outline_renderer_->flush(*mesh_renderer_);
        dyn_geo_renderer_->flush();
        // Disable shadows for liquid surfaces — shadows pass through water/lava
        // and land on the solid geometry below
        entity_shadow_renderer_->disable_shadow_rendering(context_);
        solid_renderer_->render_room_liquid_surface(solid, room);
        entity_shadow_renderer_->bind_shadow_resources(context_);
    }

    void Renderer::clear_solid_cache()
    {
        solid_renderer_->clear_cache();
    }

    void Renderer::reset_solid_cache_after_boolean()
    {
        solid_renderer_->reset_cache_after_boolean();
    }

    void Renderer::render_v3d_vif(rf::VifLodMesh *lod_mesh, int lod_index, const rf::Vector3& pos, const rf::Matrix3& orient, const rf::MeshRenderParams& params, bool skip_ambient_cache)
    {
        dyn_geo_renderer_->flush();
        mesh_renderer_->render_v3d_vif(lod_mesh, lod_index, pos, orient, params, skip_ambient_cache);

        // Skip outline queuing when rendering to a texture (e.g. rail gun scanner).
        if (render_target_bm_handle_ != -1) {
            return;
        }

        // Queue outline for third-person weapon meshes.
        // The game renders weapon meshes (MRF_CUSTOM_AMBIENT_COLOR) immediately after
        // the owning character mesh, so current_character_outline tracks the association.
        if ((params.flags & rf::MeshRenderFlags::MRF_CUSTOM_AMBIENT_COLOR) &&
            !(params.flags & rf::MeshRenderFlags::MRF_FIRST_PERSON)) {
            if (auto* info = outline_renderer_->current_character_outline()) {
                QueuedV3dOutline entry{};
                entry.lod_mesh = lod_mesh;
                entry.lod_index = lod_index;
                entry.pos = pos;
                entry.orient = orient;
                entry.info = *info;
                outline_renderer_->queue_v3d(std::move(entry));
            }
        }
    }

    void Renderer::render_character_vif(rf::VifLodMesh *lod_mesh, int lod_index, const rf::Vector3& pos, const rf::Matrix3& orient, const rf::CharacterInstance *ci, const rf::MeshRenderParams& params, bool skip_ambient_cache)
    {
        dyn_geo_renderer_->flush();
        mesh_renderer_->render_character_vif(lod_mesh, lod_index, pos, orient, ci, params, skip_ambient_cache);

        // Skip outline queuing when rendering to a texture (e.g. rail gun scanner).
        // Outlines use the main depth/stencil buffer and should only render to the back buffer.
        if (render_target_bm_handle_ != -1) {
            return;
        }

        // Queue outline if this character needs one, and track for weapon meshes
        if (auto* info = outline_renderer_->lookup(ci)) {
            outline_renderer_->set_current_character_outline(info);

            QueuedOutline entry{};
            entry.lod_mesh = lod_mesh;
            entry.lod_index = lod_index;
            entry.pos = pos;
            entry.orient = orient;
            entry.ci = ci;
            entry.info = *info;
            outline_renderer_->queue(std::move(entry));
        }
        else {
            outline_renderer_->set_current_character_outline(nullptr);
        }
    }

    void Renderer::clear_vif_cache(rf::VifLodMesh *lod_mesh)
    {
        mesh_renderer_->clear_vif_cache(lod_mesh);
    }

    void Renderer::fog_set()
    {
        dyn_geo_renderer_->flush();
        render_context_->fog_set();
    }

    void Renderer::page_in_v3d_mesh(rf::VifLodMesh* lod_mesh, rf::MeshMaterial* materials, int num_materials)
    {
        mesh_renderer_->page_in_v3d_mesh(lod_mesh, materials, num_materials);
    }

    void Renderer::page_in_character_mesh(rf::VifLodMesh* lod_mesh)
    {
        mesh_renderer_->page_in_character_mesh(lod_mesh);
    }

    void Renderer::page_in_solid(rf::GSolid* solid)
    {
        solid_renderer_->page_in_solid(solid);
    }

    void Renderer::page_in_movable_solid(rf::GSolid* solid)
    {
        solid_renderer_->page_in_movable_solid(solid);
    }

    void Renderer::flush_caches()
    {
        mesh_renderer_->flush_caches();
    }

    void Renderer::reset_static_vertex_color_tracking()
    {
        mesh_renderer_->reset_static_vertex_color_tracking();
    }

    void Renderer::clear_mesh_lights()
    {
        render_context_->update_lights();
    }

    float Renderer::z_far() const
    {
        return render_context_->projection().z_far();
    }

    bool Renderer::supports_exclusive_fullscreen() const
    {
        // ALLOW_TEARING and FRAME_LATENCY_WAITABLE_OBJECT are both incompatible with
        // exclusive fullscreen swap chains. If either flag is set, SetFullscreenState(TRUE)
        // may fail. Callers should check this before initiating a fullscreen transition.
        constexpr UINT incompatible = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING |
                                      DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        return (swap_chain_flags_ & incompatible) == 0;
    }

    void Renderer::set_pow2_tex_active(bool active)
    {
        bool was_active = render_context_->is_pow2_tex_active();
        render_context_->set_pow2_tex_active(active);

        // When transitioning from p2t to non-p2t, evict only pow2-padded textures
        // so they are reloaded at native dimensions. Non-padded textures (fonts,
        // user bitmaps, already-pow2 textures) are preserved.
        if (was_active && !active) {
            texture_manager_->flush_pow2_padded_textures();
        }
    }
}
