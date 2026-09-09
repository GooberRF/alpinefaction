#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>
#include <patch_common/FunHook.h>
#include <xlog/xlog.h>
#include "scene_capture.h"
#include "gr.h"
#include "../bmpman/atx.h"
#include "../os/os.h"
#include "../rf/bmpman.h"
#include "../rf/clutter.h"
#include "../rf/geometry.h"
#include "../rf/gr/gr.h"
#include "../rf/math/matrix.h"
#include "../rf/math/vector.h"
#include "../rf/multi.h"
#include "../rf/object.h"
#include "../rf/particle_emitter.h"
#include "../rf/player/player.h"

namespace
{

struct Projector
{
    int event_uid;
    int camera_handle;
    int target_bm;
    std::string atx_handle;
    float fov;
    int64_t interval_ms;
    int64_t next_capture_ms;
    int w;
    int h;
    bool warned_missing_camera;
    bool warned_no_room;
    bool warned_oversized;
    bool warned_bind_failed;
    int rt_generation;
};

std::vector<Projector> g_projectors;
std::unordered_set<int> g_warned_activate_failed;

Projector* find_projector(int event_uid)
{
    for (auto& p : g_projectors) {
        if (p.event_uid == event_uid) {
            return &p;
        }
    }
    return nullptr;
}

void release_target(int bm_handle)
{
    if (bm_handle < 0) {
        return;
    }
    // Drop the renderer's cached texture before the cache slot can be recycled by another
    // bitmap, otherwise a later load could inherit this render target.
    rf::gr::mark_texture_dirty(bm_handle);
    rf::bm::release(bm_handle);
}

void projector_do_frame(rf::Player* player)
{
    if (g_projectors.empty() || rf::is_dedicated_server || !rf::g_level_solid) {
        return;
    }

    rf::Player* const prev_render_player = rf::render_player;
    rf::render_player = player;

    const int64_t now_ms = timer::get_i64(1000);
    // Anti-aliasing or resolution changes drop every render target, and cache eviction can
    // take one too; nothing re-renders them, so a throttled projector would show the cleared
    // target until its interval came round.
    const int rt_generation = gr_render_target_generation();

    for (auto& p : g_projectors) {
        if (p.rt_generation != rt_generation) {
            p.rt_generation = rt_generation;
            p.next_capture_ms = 0;
        }
        if (now_ms < p.next_capture_ms) {
            continue;
        }

        rf::Object* camera = rf::obj_from_handle(p.camera_handle);
        if (!camera) {
            if (!p.warned_missing_camera) {
                p.warned_missing_camera = true;
                xlog::warn("[Display_Projection] uid={} camera object is gone, feed frozen", p.event_uid);
            }
            continue;
        }
        p.warned_missing_camera = false;

        // The engine keeps this current: obj_create resolves it at creation and refuses to
        // create a clutter outside geometry, and Object::move sets OF_WAS_TELEPORTED, which
        // makes obj_process call update_room for a mover-carried camera every frame.
        rf::GRoom* const eye_room = camera->room;
        if (!eye_room) {
            if (!p.warned_no_room) {
                p.warned_no_room = true;
                xlog::warn("[Display_Projection] uid={} camera has no room, skipping captures",
                           p.event_uid);
            }
            continue;
        }
        p.warned_no_room = false;

        // The shared depth buffer is screen sized, so a target that outgrew the screen after a
        // resolution change can no longer depth test over its whole surface. Re-firing the
        // event re-clamps it.
        if (p.w > rf::gr::screen.max_w || p.h > rf::gr::screen.max_h) {
            if (!p.warned_oversized) {
                p.warned_oversized = true;
                xlog::warn("[Display_Projection] uid={} {}x{} target exceeds the {}x{} screen, "
                           "skipping captures", p.event_uid, p.w, p.h,
                           rf::gr::screen.max_w, rf::gr::screen.max_h);
            }
            continue;
        }
        p.warned_oversized = false;

        if (scene_capture_render(camera->pos, camera->orient, p.fov, p.target_bm, p.w, p.h, eye_room)) {
            p.warned_bind_failed = false;
        }
        else if (!p.warned_bind_failed) {
            p.warned_bind_failed = true;
            xlog::warn("[Display_Projection] uid={} could not bind its render target, "
                       "retrying on the configured interval", p.event_uid);
        }
        // Advanced either way: a failing bind still costs a dyn-geo flush per attempt.
        p.next_capture_ms = now_ms + p.interval_ms;
    }

    rf::render_player = prev_render_player;
}

// monitor_render_all(player) — render_to_dynamic_textures (0x00431820) runs this per local
// player at 0x00431857, which puts projectors in exactly the frame slot the stock monitor
// cameras render from.
FunHook<void(rf::Player*)> monitor_render_all_hook{
    0x004121D0,
    [](rf::Player* player) {
        monitor_render_all_hook.call_target(player);
        // One capture per frame, not one per local player.
        if (player == rf::local_player) {
            projector_do_frame(player);
        }
    },
};
}

bool scene_capture_render(const rf::Vector3& pos, const rf::Matrix3& orient, float fov,
                          int target_bm, int w, int h, rf::GRoom* eye_room)
{
    rf::GSolid* const solid = rf::g_level_solid;
    if (!solid || target_bm < 0) {
        return false;
    }
    if (!gr_set_render_target(target_bm)) {
        return false;
    }

    int clip_x = 0, clip_y = 0, clip_w = 0, clip_h = 0;
    rf::gr::get_clip(&clip_x, &clip_y, &clip_w, &clip_h);

    rf::gr::set_clip(0, 0, w, h);
    rf::gr::set_color(0, 0, 0, 255);
    rf::gr::clear();
    rf::gr::setup_3d(&orient, &pos, fov, 1, 1);
    rf::gr::light_frame_begin();
    rf::g_decal_pass_list_a.clear();
    rf::g_decal_pass_list_b.clear();
    rf::gr::light_filter_for_camera();
    rf::obj_reset_render_flags();

    rf::monitor_render_in_progress = true;
    // Stock hands the portal renderer the rotating sky room matrix only while the rate is > 0
    const rf::Matrix3* const sky_rotation =
        rf::sky_room_rotation_rate > 0.0f ? &rf::sky_room_rotation : nullptr;
    rf::g_solid_portal_render(solid, eye_room, 0, sky_rotation);
    rf::monitor_render_in_progress = false;

    for (int i = 0; i < rf::bolt_emitter_list.size(); ++i) {
        rf::BoltEmitter* const emitter = rf::bolt_emitter_list.at_unchecked(i);
        if (emitter && emitter->should_render()) {
            emitter->render(&pos);
        }
    }

    rf::gr::flush();
    solid->reset_room_render_frames();

    gr_set_render_target(-1);
    rf::gr::set_clip(clip_x, clip_y, clip_w, clip_h);
    return true;
}

bool projector_activate(int event_uid, int camera_handle, const std::string& atx_handle,
                        float fov, float interval_s, int w, int h)
{
    // set_render_target binds the shared screen-sized depth buffer (init_depth_stencil_buffer
    // sizes it from screen.max_w/max_h), so a target bigger than the screen would only get depth
    // testing over the overlapping corner.
    if (rf::gr::screen.max_w > 0 && rf::gr::screen.max_h > 0) {
        w = std::min(w, rf::gr::screen.max_w);
        h = std::min(h, rf::gr::screen.max_h);
    }

    // Same canonical form the controller registry keys on, so re-triggering with a different
    // spelling compares equal instead of clearing the feed it just set.
    const std::string handle_key = atx_canonical_handle(atx_handle);

    // Probe before allocating: a repeating trigger on a texture the level never references would
    // otherwise create and release a render target on every fire.
    if (!atx_has_controller(handle_key)) {
        if (g_warned_activate_failed.insert(event_uid).second) {
            xlog::warn("[Display_Projection] uid={} ATX '{}' not loaded — the texture must be "
                       "referenced by the level before this event fires", event_uid, handle_key);
            if (find_projector(event_uid)) {
                xlog::warn("[Display_Projection] uid={} keeps its running projector and its "
                           "previous parameters", event_uid);
            }
        }
        return false;
    }
    g_warned_activate_failed.erase(event_uid);

    Projector* existing = find_projector(event_uid);
    const bool reuse_target = existing && existing->w == w && existing->h == h;

    // Resolved before anything is torn down, so a failure here leaves every projector as it was.
    const int target_bm = reuse_target ? existing->target_bm
                                       : rf::bm::create(rf::bm::FORMAT_RENDER_TARGET, w, h);
    if (target_bm < 0) {
        xlog::warn("[Display_Projection] uid={} could not create a {}x{} render target", event_uid, w, h);
        return false;
    }

    // A second event pointing at the same ATX takes the feed over; the one that had it would
    // otherwise keep rendering into a target nothing samples any more.
    for (auto it = g_projectors.begin(); it != g_projectors.end(); ++it) {
        if (it->event_uid == event_uid || it->atx_handle != handle_key) {
            continue;
        }
        xlog::warn("[Display_Projection] uid={} took ATX '{}' from uid={}, which is now off",
                   event_uid, handle_key, it->event_uid);
        atx_clear_live_feed(it->atx_handle, it->target_bm);
        release_target(it->target_bm);
        g_projectors.erase(it);
        // The erase can move the vector's storage.
        existing = find_projector(event_uid);
        break;
    }

    if (!atx_set_live_feed(handle_key, target_bm)) {
        if (!reuse_target) {
            release_target(target_bm);
        }
        if (existing) {
            xlog::warn("[Display_Projection] uid={} keeping the running projector ({}x{} on '{}') — "
                       "the new parameters were not applied", event_uid, existing->w, existing->h,
                       existing->atx_handle);
        }
        return false;
    }

    Projector updated{event_uid, camera_handle, target_bm, handle_key, fov,
                      static_cast<int64_t>(std::max(0.0f, interval_s) * 1000.0f), 0, w, h,
                      false, false, false, false, gr_render_target_generation()};

    if (existing) {
        if (existing->atx_handle != handle_key) {
            atx_clear_live_feed(existing->atx_handle, existing->target_bm);
        }
        if (!reuse_target) {
            release_target(existing->target_bm);
        }
        *existing = std::move(updated);
        return true;
    }

    g_projectors.push_back(std::move(updated));
    return true;
}

void projector_deactivate(int event_uid)
{
    for (auto it = g_projectors.begin(); it != g_projectors.end(); ++it) {
        if (it->event_uid != event_uid) {
            continue;
        }
        atx_clear_live_feed(it->atx_handle, it->target_bm);
        release_target(it->target_bm);
        g_projectors.erase(it);
        return;
    }
}

void projector_clear_all()
{
    gr_set_render_target(-1);
    for (auto& p : g_projectors) {
        atx_clear_live_feed(p.atx_handle, p.target_bm);
        release_target(p.target_bm);
    }
    g_projectors.clear();
    g_warned_activate_failed.clear();
}

void scene_capture_apply_patch()
{
    monitor_render_all_hook.install();
}
