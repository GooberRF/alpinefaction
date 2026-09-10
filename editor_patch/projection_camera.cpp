#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>
#include <vector>
#include <xlog/xlog.h>
#include "projection_camera.h"
#include "level.h"
#include "vtypes.h"
#include "alpine_obj.h"

static int g_projection_camera_icon_handle = -1;
static std::vector<DedProjectionCamera*> g_projection_camera_clipboard;

static void projection_camera_load_icon()
{
    if (g_projection_camera_icon_handle < 0) {
        g_projection_camera_icon_handle = bm_load("Icon_AFProjection.tga", -1, 1);
    }
}

void DestroyDedProjectionCamera(DedProjectionCamera* camera)
{
    if (!camera) return;
    camera->field_4.free();
    camera->script_name.free();
    camera->class_name.free();
    delete camera;
}

void projection_camera_serialize_chunk(CDedLevel& level, rf::File& file)
{
    auto& cameras = level.GetAlpineLevelProperties().projection_camera_objects;
    if (cameras.empty()) return;

    auto start_pos = level.BeginRflSection(file, alpine_projection_camera_chunk_id);

    uint32_t count = static_cast<uint32_t>(cameras.size());
    file.write<uint32_t>(count);

    for (auto* camera : cameras) {
        file.write<int32_t>(camera->uid);
        file.write<float>(camera->pos.x);
        file.write<float>(camera->pos.y);
        file.write<float>(camera->pos.z);
        file.write<float>(camera->orient.rvec.x);
        file.write<float>(camera->orient.rvec.y);
        file.write<float>(camera->orient.rvec.z);
        file.write<float>(camera->orient.uvec.x);
        file.write<float>(camera->orient.uvec.y);
        file.write<float>(camera->orient.uvec.z);
        file.write<float>(camera->orient.fvec.x);
        file.write<float>(camera->orient.fvec.y);
        file.write<float>(camera->orient.fvec.z);
        write_rfl_string(file, camera->script_name);
    }

    level.EndRflSection(file, start_pos);
}

void projection_camera_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len)
{
    auto& cameras = level.GetAlpineLevelProperties().projection_camera_objects;
    std::size_t remaining = chunk_len;

    rf::File::ChunkGuard chunk_guard{file, remaining};

    auto read_bytes = [&](void* dst, std::size_t n) -> bool {
        if (remaining < n) return false;
        int got = file.read(dst, n);
        if (got != static_cast<int>(n) || file.error()) {
            if (got > 0) remaining -= got;
            return false;
        }
        remaining -= n;
        return true;
    };

    uint32_t count = 0;
    if (!read_bytes(&count, sizeof(count))) return;
    if (count > 10000) count = 10000;

    for (uint32_t i = 0; i < count; i++) {
        auto* camera = new DedProjectionCamera();
        memset(static_cast<DedObject*>(camera), 0, sizeof(DedObject));
        camera->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
        camera->type = DedObjectType::DED_PROJECTION_CAMERA;

        if (!read_bytes(&camera->uid, sizeof(camera->uid))) { DestroyDedProjectionCamera(camera); return; }
        if (!read_bytes(&camera->pos.x, sizeof(float))) { DestroyDedProjectionCamera(camera); return; }
        if (!read_bytes(&camera->pos.y, sizeof(float))) { DestroyDedProjectionCamera(camera); return; }
        if (!read_bytes(&camera->pos.z, sizeof(float))) { DestroyDedProjectionCamera(camera); return; }
        if (!read_bytes(&camera->orient.rvec.x, sizeof(float))) { DestroyDedProjectionCamera(camera); return; }
        if (!read_bytes(&camera->orient.rvec.y, sizeof(float))) { DestroyDedProjectionCamera(camera); return; }
        if (!read_bytes(&camera->orient.rvec.z, sizeof(float))) { DestroyDedProjectionCamera(camera); return; }
        if (!read_bytes(&camera->orient.uvec.x, sizeof(float))) { DestroyDedProjectionCamera(camera); return; }
        if (!read_bytes(&camera->orient.uvec.y, sizeof(float))) { DestroyDedProjectionCamera(camera); return; }
        if (!read_bytes(&camera->orient.uvec.z, sizeof(float))) { DestroyDedProjectionCamera(camera); return; }
        if (!read_bytes(&camera->orient.fvec.x, sizeof(float))) { DestroyDedProjectionCamera(camera); return; }
        if (!read_bytes(&camera->orient.fvec.y, sizeof(float))) { DestroyDedProjectionCamera(camera); return; }
        if (!read_bytes(&camera->orient.fvec.z, sizeof(float))) { DestroyDedProjectionCamera(camera); return; }

        std::string sname = read_rfl_string(file, remaining);
        camera->script_name.assign_0(sname.empty() ? "Projection Camera" : sname.c_str());

        cameras.push_back(camera);
        level.master_objects.add(static_cast<DedObject*>(camera));
    }

    xlog::info("[ProjectionCamera] Loaded {} projection camera object(s)", cameras.size());
}

void PlaceNewProjectionCameraObject()
{
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto* camera = new DedProjectionCamera();
    memset(static_cast<DedObject*>(camera), 0, sizeof(DedObject));
    camera->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
    camera->type = DedObjectType::DED_PROJECTION_CAMERA;

    camera->script_name.assign_0("Projection Camera");

    // Match stock placement (FUN_004431c0): take both position and orientation from the
    // active viewport, so the camera starts out looking where the mapper is looking.
    auto* viewport = get_active_viewport();
    if (viewport && viewport->view_data) {
        camera->pos = viewport->view_data->camera_pos;
        camera->orient = viewport->view_data->camera_orient;
    }
    else {
        camera->orient.rvec = {1.0f, 0.0f, 0.0f};
        camera->orient.uvec = {0.0f, 1.0f, 0.0f};
        camera->orient.fvec = {0.0f, 0.0f, 1.0f};
    }

    camera->uid = generate_uid();

    level->GetAlpineLevelProperties().projection_camera_objects.push_back(camera);
    level->master_objects.add(static_cast<DedObject*>(camera));

    level->clear_selection();
    level->add_to_selection(static_cast<DedObject*>(camera));
    level->update_console_display();
}

DedProjectionCamera* CloneProjectionCameraObject(DedProjectionCamera* source, bool add_to_level)
{
    if (!source) return nullptr;

    auto* camera = new DedProjectionCamera();
    memset(static_cast<DedObject*>(camera), 0, sizeof(DedObject));
    camera->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
    camera->type = DedObjectType::DED_PROJECTION_CAMERA;

    camera->pos = source->pos;
    camera->orient = source->orient;
    camera->script_name.assign_0(source->script_name.c_str());

    camera->uid = generate_uid();

    if (add_to_level) {
        auto* level = CDedLevel::Get();
        if (level) {
            level->GetAlpineLevelProperties().projection_camera_objects.push_back(camera);
            level->master_objects.add(static_cast<DedObject*>(camera));
        }
    }

    return camera;
}

void DeleteProjectionCameraObject(DedProjectionCamera* camera)
{
    if (!camera) return;
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto& cameras = level->GetAlpineLevelProperties().projection_camera_objects;
    auto it = std::find(cameras.begin(), cameras.end(), camera);
    if (it != cameras.end()) {
        cameras.erase(it);
    }
    alpine_remove_from_groups(level, static_cast<DedObject*>(camera));
    level->master_objects.remove_by_value(static_cast<DedObject*>(camera));
    DestroyDedProjectionCamera(camera);
}

static void draw_facing_arrow(const DedProjectionCamera* camera)
{
    const Vector3& p = camera->pos;
    const Vector3& f = camera->orient.fvec;
    draw_3d_arrow(p.x, p.y, p.z, p.x + f.x, p.y + f.y, p.z + f.z, 255, 0, 0);
}

void projection_camera_render(CDedLevel* level)
{
    auto& cameras = level->GetAlpineLevelProperties().projection_camera_objects;
    if (cameras.empty()) return;

    projection_camera_load_icon();

    const float cam_param = gr_cam_param;

    for (auto* camera : cameras) {
        if (camera->hidden_in_editor) continue;

        const bool selected = is_object_selected(level, camera);
        const int r = 0xff;
        const int g = selected ? 0x00 : 0xc0;
        const int b = selected ? 0x00 : 0x20;

        draw_facing_arrow(camera);

        set_draw_color(r, g, b, 0xff);
        if (g_projection_camera_icon_handle >= 0) {
            gr_set_bitmap(g_projection_camera_icon_handle, -1);
        }
        gr_render_billboard(&camera->pos, 0, 0.25f, cam_param);
    }
}

void projection_camera_pick(CDedLevel* level, int param1, int param2)
{
    auto& cameras = level->GetAlpineLevelProperties().projection_camera_objects;
    for (auto* camera : cameras) {
        if (camera->hidden_in_editor) continue;
        bool hit = level->hit_test_point(param1, param2, &camera->pos);
        if (hit) {
            level->select_object(static_cast<DedObject*>(camera));
        }
    }
}

DedProjectionCamera* projection_camera_click_pick(CDedLevel* level, float click_x, float click_y)
{
    auto& cameras = level->GetAlpineLevelProperties().projection_camera_objects;
    float best_dist_sq = 1e30f;
    DedProjectionCamera* best_camera = nullptr;

    for (auto* camera : cameras) {
        if (camera->hidden_in_editor) continue;

        float center_pos[3] = {camera->pos.x, camera->pos.y, camera->pos.z};
        float screen_cx = 0.0f, screen_cy = 0.0f;
        if (!project_to_screen_2d(center_pos, &screen_cx, &screen_cy))
            continue;

        constexpr float screen_radius_sq = 400.0f; // 20px

        float dx = screen_cx - click_x;
        float dy = screen_cy - click_y;
        float dist_sq = dx * dx + dy * dy;
        if (dist_sq <= screen_radius_sq && dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            best_camera = camera;
        }
    }

    return best_camera;
}

void projection_camera_tree_populate(EditorTreeCtrl* tree, int master_groups, CDedLevel* level)
{
    auto& cameras = level->GetAlpineLevelProperties().projection_camera_objects;

    char buf[64];
    snprintf(buf, sizeof(buf), "Projection Cameras (%d)", static_cast<int>(cameras.size()));
    int parent = tree->insert_item(buf, master_groups, 0xffff0002);

    for (auto* camera : cameras) {
        const char* name = camera->script_name.c_str();
        if (!name || name[0] == '\0') {
            name = "(unnamed projection camera)";
        }
        int child = tree->insert_item(name, parent, 0xffff0002);
        tree->set_item_data(child, camera->uid);
    }
}

void projection_camera_tree_add_object_type(EditorTreeCtrl* tree)
{
    tree->insert_item("Projection Camera", 0xffff0000, 0xffff0002);
}

bool projection_camera_copy_object(DedObject* source)
{
    if (!source || source->type != DedObjectType::DED_PROJECTION_CAMERA) return false;
    auto* staged = CloneProjectionCameraObject(static_cast<DedProjectionCamera*>(source), false);
    if (staged) {
        g_projection_camera_clipboard.push_back(staged);
        return true;
    }
    return false;
}

void projection_camera_paste_objects(CDedLevel* level)
{
    for (auto* staged : g_projection_camera_clipboard) {
        auto* clone = CloneProjectionCameraObject(staged, true);
        if (clone) {
            level->add_to_selection(static_cast<DedObject*>(clone));
        }
    }
}

void projection_camera_clear_clipboard()
{
    for (auto* camera : g_projection_camera_clipboard) {
        DestroyDedProjectionCamera(camera);
    }
    g_projection_camera_clipboard.clear();
}

void projection_camera_handle_delete_or_cut(DedObject* obj)
{
    if (!obj || obj->type != DedObjectType::DED_PROJECTION_CAMERA) return;
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto& cameras = level->GetAlpineLevelProperties().projection_camera_objects;
    auto it = std::find(cameras.begin(), cameras.end(), static_cast<DedProjectionCamera*>(obj));
    if (it != cameras.end()) {
        cameras.erase(it);
    }
}

void projection_camera_handle_delete_selection(CDedLevel* level)
{
    auto& sel = level->selection;
    for (int i = sel.size - 1; i >= 0; i--) {
        DedObject* obj = sel.data_ptr[i];
        if (obj && obj->type == DedObjectType::DED_PROJECTION_CAMERA) {
            for (int j = i; j < sel.size - 1; j++) {
                sel.data_ptr[j] = sel.data_ptr[j + 1];
            }
            sel.size--;
            DeleteProjectionCameraObject(static_cast<DedProjectionCamera*>(obj));
        }
    }
}

void projection_camera_ensure_uid(int& uid)
{
    auto* level = CDedLevel::Get();
    if (!level) return;
    for (auto* c : level->GetAlpineLevelProperties().projection_camera_objects) {
        if (c->uid >= uid) uid = c->uid + 1;
    }
}
