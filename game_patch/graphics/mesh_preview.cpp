#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <xlog/xlog.h>
#include <common/gr_d3d11/gr_d3d11_mesh.h>
#include <common/utils/string-utils.h>
#include "mesh_preview.h"
#include "gr.h"
#include "d3d11/gr_d3d11_hooks.h"
#include "../misc/misc.h"
#include "../rf/bmpman.h"
#include "../rf/character.h"
#include "../rf/file/file.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_light.h"
#include "../rf/level.h"
#include "../rf/math/matrix.h"
#include "../rf/math/vector.h"
#include "../rf/os/frametime.h"
#include "../rf/v3d.h"
#include "../rf/vmesh.h"

namespace
{

constexpr float turn_rate = 0.5f; // radians per second, about one turn in 12.5 s
constexpr float fov = 40.0f;
constexpr float camera_pitch = 0.32f; // radians the camera looks down by
constexpr float frame_margin = 1.08f;
constexpr std::array<float, 3> ambient{0.30f, 0.30f, 0.33f};

struct PreviewLight
{
    rf::Vector3 dir; // from the mesh, in camera distances
    float r;
    float g;
    float b;
};

constexpr std::size_t preview_light_count = 3;
const std::array<PreviewLight, preview_light_count> preview_lights{{
    {{-0.65f, 0.75f, -0.65f}, 1.00f, 0.96f, 0.90f},
    {{0.85f, 0.15f, -0.45f}, 0.42f, 0.46f, 0.55f},
    {{0.10f, 0.55f, 0.95f}, 0.55f, 0.55f, 0.60f},
}};

// The lighting state a preview borrows, from a loaded level or whatever the last one left. It sets its own for
// the draw and puts them back after.
class PreviewLightingScope
{
public:
    explicit PreviewLightingScope(float eye_dist)
    {
        rf::gr::light_get_ambient(&saved_ambient_[0], &saved_ambient_[1], &saved_ambient_[2]);
        rf::gr::light_set_ambient(ambient[0], ambient[1], ambient[2]);
        saved_vertex_lighting_ = gr::d3d11::g_level_vertex_lighting;
        gr::d3d11::g_level_vertex_lighting = false;
        saved_fullbright_ = g_character_meshes_are_fullbright;
        g_character_meshes_are_fullbright = false;
        // The far plane comes from the level's fog; a stale short one would cut a big mesh off.
        saved_fog_far_clip_ = rf::level.distance_fog_far_clip;
        rf::level.distance_fog_far_clip = 0.0f;

        // The mesh hooks would gather a loaded level's lights at the mesh instead of using this list.
        saved_skip_gather_ = gr::d3d11::set_skip_mesh_light_gather(true);
        for (std::size_t i = 0; i < preview_light_count; ++i) {
            const PreviewLight& def = preview_lights[i];
            rf::gr::Light& light = lights_[i];
            light = {};
            light.type = rf::gr::LT_POINT;
            light.vec = def.dir * eye_dist;
            light.vec2 = light.vec;
            light.rad_2 = eye_dist * 4.0f;
            light.r = def.r;
            light.g = def.g;
            light.b = def.b;
            light.on = true;
            rf::gr::relevant_lights[i] = &light;
        }
        rf::gr::num_relevant_lights = static_cast<int>(preview_light_count);
    }

    ~PreviewLightingScope()
    {
        gr::d3d11::set_skip_mesh_light_gather(saved_skip_gather_);
        rf::level.distance_fog_far_clip = saved_fog_far_clip_;
        g_character_meshes_are_fullbright = saved_fullbright_;
        gr::d3d11::g_level_vertex_lighting = saved_vertex_lighting_;
        rf::gr::light_set_ambient(saved_ambient_[0], saved_ambient_[1], saved_ambient_[2]);
        // Emptied, not restored: the saved list points at a gone level's lights and the GPU copy tints later draws.
        rf::gr::num_relevant_lights = 0;
        gr::d3d11::clear_mesh_lights();
    }

    PreviewLightingScope(const PreviewLightingScope&) = delete;
    PreviewLightingScope& operator=(const PreviewLightingScope&) = delete;

private:
    std::array<rf::gr::Light, preview_light_count> lights_{};
    float saved_ambient_[3]{};
    bool saved_vertex_lighting_ = false;
    bool saved_fullbright_ = false;
    bool saved_skip_gather_ = false;
    float saved_fog_far_clip_ = 0.0f;
};

rf::Matrix3 yaw_orient(float yaw)
{
    rf::Matrix3 m;
    m.rvec = {std::cos(yaw), 0.0f, -std::sin(yaw)};
    m.uvec = {0.0f, 1.0f, 0.0f};
    m.fvec = {std::sin(yaw), 0.0f, std::cos(yaw)};
    return m;
}

// vmesh_create_character treats a missing <name>.v3c, and a base character past the 64 slots, as fatal errors
// (0x0051D780, 0x0051D7E0). A preview leaves slots spare for the characters a level loads later.
bool character_can_load(const std::string& name)
{
    constexpr int spare_slots = 8;
    if (!rf::File{}.find((name + ".v3c").c_str())) {
        return false;
    }
    int used = 0;
    for (const rf::Character& slot : rf::base_characters) {
        if (slot.flags & rf::CHARACTER_FLAG_LOADED) {
            if (string_iequals(slot.name, name)) {
                return true;
            }
            ++used;
        }
    }
    return used + spare_slots < static_cast<int>(std::size(rf::base_characters));
}

rf::VMesh* load_preview_mesh(const std::string& filename)
{
    rf::VMesh* mesh = nullptr;
    const std::string_view ext = get_ext_from_filename(filename);
    if (string_iequals(ext, "vfx")) {
        mesh = rf::vmesh_create_anim_fx(filename.c_str(), -1);
        // An effect shows nothing until its animation has weight, which obj_create_mesh (0x00489FE0) gives it.
        if (mesh) {
            rf::vmesh_set_action_weight(mesh, 0, 1.0f);
        }
    }
    else if (string_iequals(ext, "v3d") || string_iequals(ext, "v3m")) {
        mesh = rf::vmesh_load(filename.c_str(), rf::MESH_TYPE_STATIC, -1);
    }
    else if (string_iequals(ext, "v3c") || string_iequals(ext, "vcm")) {
        const std::string name{get_filename_without_ext(filename)};
        if (character_can_load(name)) {
            mesh = rf::vmesh_create_character(name.c_str(), 0, 0);
        }
    }
    if (mesh && !mesh->instance) {
        rf::vmesh_free(mesh);
        mesh = nullptr;
    }
    if (!mesh) {
        xlog::warn("[Preview] No preview for '{}': the mesh did not load", filename);
    }
    return mesh;
}

// A character's own box is far roomier than the figure, so its collision spheres frame it.
void get_mesh_bounds(rf::VMesh* mesh, rf::Vector3& bbox_min, rf::Vector3& bbox_max)
{
    const int num_spheres = mesh->type == rf::MESH_TYPE_CHARACTER ? rf::vmesh_get_num_cspheres(mesh) : 0;
    if (num_spheres <= 0) {
        rf::vmesh_get_bbox(mesh, &bbox_min, &bbox_max);
        return;
    }
    for (int i = 0; i < num_spheres; ++i) {
        rf::Vector3 center;
        float radius = 0.0f;
        rf::vmesh_get_csphere(mesh, i, &center, &radius);
        const rf::Vector3 lo{center.x - radius, center.y - radius, center.z - radius};
        const rf::Vector3 hi{center.x + radius, center.y + radius, center.z + radius};
        if (i == 0) {
            bbox_min = lo;
            bbox_max = hi;
            continue;
        }
        bbox_min = {std::min(bbox_min.x, lo.x), std::min(bbox_min.y, lo.y), std::min(bbox_min.z, lo.z)};
        bbox_max = {std::max(bbox_max.x, hi.x), std::max(bbox_max.y, hi.y), std::max(bbox_max.z, hi.z)};
    }
}

void grow_bbox(rf::VMesh* mesh, const rf::Vector3& pos, const rf::Matrix3& orient, rf::Vector3& bbox_min,
               rf::Vector3& bbox_max)
{
    rf::Vector3 lo;
    rf::Vector3 hi;
    rf::vmesh_get_bbox(mesh, &lo, &hi);
    for (int i = 0; i < 8; ++i) {
        const rf::Vector3 corner{(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z};
        const rf::Vector3 p = pos + orient.transform_vector(corner);
        bbox_min = {std::min(bbox_min.x, p.x), std::min(bbox_min.y, p.y), std::min(bbox_min.z, p.z)};
        bbox_max = {std::max(bbox_max.x, p.x), std::max(bbox_max.y, p.y), std::max(bbox_max.z, p.z)};
    }
}

} // namespace

void MeshPreview::set_mesh(const std::string& filename, const PartsFn& parts, float zoom)
{
    release_mesh();
    yaw_ = 0.0f;
    zoom_ = zoom;
    if (filename.empty()) {
        return;
    }
    mesh_ = load_preview_mesh(filename);
    if (!mesh_ || !parts) {
        return;
    }
    // A file repeated across parts, as a hull's four tires, is loaded once and drawn at each.
    std::vector<std::pair<std::string, rf::VMesh*>> loaded;
    for (const MeshPreviewPart& part : parts(mesh_)) {
        auto it = std::ranges::find_if(loaded, [&](const auto& l) { return string_iequals(l.first, part.filename); });
        if (it == loaded.end()) {
            rf::VMesh* part_mesh = load_preview_mesh(part.filename);
            if (part_mesh) {
                part_meshes_.push_back(part_mesh);
            }
            loaded.emplace_back(part.filename, part_mesh);
            it = std::prev(loaded.end());
        }
        if (it->second) {
            const int grip = part.grip.empty() ? -1 : rf::vmesh_lookup_prop_point(it->second, part.grip.c_str());
            parts_.push_back(LoadedPart{it->second, part.pos, part.orient, part.follow_prop, grip});
        }
    }
}

void MeshPreview::set_character(const std::string& filename, const std::string& state_anim,
                                const std::function<void(rf::VMesh*)>& dress, const PartsFn& parts)
{
    set_mesh(filename, parts);
    if (!mesh_ || mesh_->type != rf::MESH_TYPE_CHARACTER) {
        return;
    }
    // A character faces down its +z, away from the camera.
    yaw_ = std::numbers::pi_v<float>;
    if (dress) {
        dress(mesh_);
    }
    // The animation loads from <name>.rfa whatever extension the table gives it (0x0053A980).
    const std::string anim_file = std::string{get_filename_without_ext(state_anim)} + ".rfa";
    if (!state_anim.empty() && rf::File{}.find(anim_file.c_str())) {
        action_ = rf::character_mesh_load_action(mesh_->mesh, state_anim.c_str(), 1, 0);
    }
}

void MeshPreview::render(int w, int h)
{
    rendered_ = false;
    if (!mesh_ || w <= 0 || h <= 0) {
        return;
    }
    // set_render_target binds the shared depth buffer, which is screen sized.
    if (rf::gr::screen.max_w > 0 && rf::gr::screen.max_h > 0) {
        w = std::min(w, rf::gr::screen.max_w);
        h = std::min(h, rf::gr::screen.max_h);
    }
    if (target_bm_ < 0 || w != target_w_ || h != target_h_) {
        release_target();
        target_bm_ = rf::bm::create(rf::bm::FORMAT_RENDER_TARGET, w, h);
        if (target_bm_ < 0) {
            return;
        }
        target_w_ = w;
        target_h_ = h;
    }

    constexpr float two_pi = 2.0f * std::numbers::pi_v<float>;
    yaw_ = std::fmod(yaw_ + turn_rate * rf::frametime, two_pi);

    const bool animated = mesh_->type == rf::MESH_TYPE_ANIM_FX || mesh_->type == rf::MESH_TYPE_CHARACTER;
    const auto pose = [&](rf::Vector3& at, rf::Matrix3& facing, float dt) {
        if (action_ >= 0) {
            // As an entity holds its state: the weights set afresh each frame, which leaves its time running.
            rf::vmesh_reset_actions(mesh_);
            rf::vmesh_set_action_weight(mesh_, action_, 1.0f);
        }
        if (animated) {
            rf::vmesh_process(mesh_, dt, 0, &at, &facing, 0);
        }
    };
    const auto held_frame = [&](const LoadedPart& part, rf::Vector3 at, rf::Matrix3 facing) {
        rf::Matrix3 prop_orient;
        rf::Vector3 prop_pos;
        rf::vmesh_get_prop_point_transform(mesh_, part.follow_prop, &facing, &at, &prop_orient, &prop_pos);
        rf::Vector3 part_pos = prop_pos;
        if (part.grip >= 0) {
            rf::Matrix3 grip_orient;
            rf::Vector3 grip_pos;
            rf::vmesh_get_prop_point_transform(part.mesh, part.grip, &prop_orient, &prop_pos, &grip_orient, &grip_pos);
            part_pos = prop_pos - (grip_pos - prop_pos);
        }
        return std::pair{part_pos, prop_orient};
    };

    // Measured once, in the first pose: an animation moves the collision spheres a character is framed by, and a
    // frame measured each time would breathe with it.
    if (!bounds_) {
        rf::Vector3 origin{};
        rf::Matrix3 facing = rf::identity_matrix;
        pose(origin, facing, 0.0f);
        rf::Vector3 lo;
        rf::Vector3 hi;
        get_mesh_bounds(mesh_, lo, hi);
        for (const LoadedPart& part : parts_) {
            if (part.follow_prop < 0) {
                grow_bbox(part.mesh, part.pos, part.orient, lo, hi);
            }
            else {
                const auto [part_pos, part_orient] = held_frame(part, origin, facing);
                grow_bbox(part.mesh, part_pos, part_orient, lo, hi);
            }
        }
        bounds_ = {lo, hi};
    }
    const auto& [bbox_min, bbox_max] = *bounds_;
    const rf::Vector3 center = (bbox_min + bbox_max) * 0.5f;
    const float radius = std::max((bbox_max - bbox_min).len() * 0.5f, 0.01f);

    rf::Matrix3 orient = yaw_orient(yaw_);
    rf::Vector3 pos = orient.transform_vector(center) * -1.0f;

    // The sphere has to fit the narrower of the two view angles.
    const float half_h = fov * 0.5f * std::numbers::pi_v<float> / 180.0f;
    const float half_v = std::atan(std::tan(half_h) * static_cast<float>(h) / static_cast<float>(w));
    float dist = radius * frame_margin / std::sin(std::min(half_h, half_v)) / zoom_;
    if (mesh_->type == rf::MESH_TYPE_CHARACTER) {
        // A standing figure: its height fills the view, and its width at any turn fits across it.
        const rf::Vector3 half = (bbox_max - bbox_min) * 0.5f;
        const float across = std::hypot(half.x, half.z);
        dist = std::max(half.y / std::tan(half_v), across / std::tan(half_h)) * frame_margin + across;
    }
    rf::Matrix3 eye_orient;
    eye_orient.fvec = {0.0f, -std::sin(camera_pitch), std::cos(camera_pitch)};
    eye_orient.uvec = {0.0f, std::cos(camera_pitch), std::sin(camera_pitch)};
    eye_orient.rvec = {1.0f, 0.0f, 0.0f};
    const rf::Vector3 eye_pos = eye_orient.fvec * -dist;

    pose(pos, orient, rf::frametime);

    std::vector<std::pair<rf::Vector3, rf::Matrix3>> part_frames;
    part_frames.reserve(parts_.size());
    for (const LoadedPart& part : parts_) {
        if (part.follow_prop >= 0) {
            part_frames.push_back(held_frame(part, pos, orient));
            continue;
        }
        rf::Matrix3 part_orient = orient;
        part_orient.mul(part.orient);
        part_frames.emplace_back(pos + orient.transform_vector(part.pos), part_orient);
    }
    // An effect advances once a frame however many parts share it.
    for (rf::VMesh* part_mesh : part_meshes_) {
        const auto it = std::ranges::find(parts_, part_mesh, &LoadedPart::mesh);
        if (part_mesh->type == rf::MESH_TYPE_ANIM_FX && it != parts_.end()) {
            const std::size_t i = static_cast<std::size_t>(it - parts_.begin());
            rf::vmesh_process(part_mesh, rf::frametime, 0, &part_frames[i].first, &part_frames[i].second, 0);
        }
    }

    // Anything the UI queued so far belongs to the screen, not to the target.
    rf::gr::flush();
    if (!gr_set_render_target(target_bm_)) {
        return;
    }

    int clip_x = 0;
    int clip_y = 0;
    int clip_w = 0;
    int clip_h = 0;
    rf::gr::get_clip(&clip_x, &clip_y, &clip_w, &clip_h);
    rf::gr::set_clip(0, 0, w, h);
    rf::gr::set_color(0, 0, 0, 255);
    rf::gr::clear();
    {
        const PreviewLightingScope lighting{dist};
        rf::gr::setup_3d(&eye_orient, &eye_pos, fov, 1, 1);
        rf::MeshRenderParams params{};
        params.init_defaults();
        params.lod_level = 0;
        params.orient = orient;
        const gr::d3d11::ScopedPicmipSkipObject full_res_textures;
        rf::vmesh_render(mesh_, &pos, &orient, &params);
        for (std::size_t i = 0; i < parts_.size(); ++i) {
            params.orient = part_frames[i].second;
            rf::vmesh_render(parts_[i].mesh, &part_frames[i].first, &part_frames[i].second, &params);
        }
        rf::gr::flush();
    }
    gr_set_render_target(-1);
    rf::gr::set_clip(clip_x, clip_y, clip_w, clip_h);
    rendered_ = true;
}

void MeshPreview::render_in_well(int w, int h)
{
    if (w > 2 && h > 2) {
        render(w - 2, h - 2);
    }
    else {
        rendered_ = false;
    }
}

void MeshPreview::release()
{
    release_mesh();
    release_target();
}

void MeshPreview::release_mesh()
{
    parts_.clear();
    for (rf::VMesh* part_mesh : part_meshes_) {
        rf::vmesh_free(part_mesh);
    }
    part_meshes_.clear();
    action_ = -1;
    bounds_.reset();
    if (mesh_) {
        rf::vmesh_free(mesh_);
        mesh_ = nullptr;
    }
    rendered_ = false;
}

void MeshPreview::release_target()
{
    if (target_bm_ >= 0) {
        // Drop the renderer's cached texture first, or a later bitmap given this slot inherits the target.
        rf::gr::mark_texture_dirty(target_bm_);
        rf::bm::release(target_bm_);
        target_bm_ = -1;
    }
    target_w_ = 0;
    target_h_ = 0;
    rendered_ = false;
}
