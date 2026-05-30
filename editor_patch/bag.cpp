#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>
#include <vector>
#include <xlog/xlog.h>
#include "bag.h"
#include "level.h"
#include "vtypes.h"
#include "alpine_obj.h"

static int g_bag_icon_handle = -1;
static std::vector<DedBag*> g_bag_clipboard;

static void bag_load_icon()
{
    if (g_bag_icon_handle < 0) {
        g_bag_icon_handle = bm_load("Icon_AFBag.tga", -1, 1);
    }
}

static void destroy_ded_bag(DedBag* bag)
{
    if (!bag) return;
    bag->field_4.free();
    bag->script_name.free();
    bag->class_name.free();
    delete bag;
}

void bag_serialize_chunk(CDedLevel& level, rf::File& file)
{
    auto& bags = level.GetAlpineLevelProperties().bag_objects;
    if (bags.empty()) return;

    auto start_pos = level.BeginRflSection(file, alpine_bag_chunk_id);

    uint32_t count = static_cast<uint32_t>(bags.size());
    file.write<uint32_t>(count);

    for (auto* bag : bags) {
        file.write<int32_t>(bag->uid);
        file.write<float>(bag->pos.x);
        file.write<float>(bag->pos.y);
        file.write<float>(bag->pos.z);
        file.write<float>(bag->orient.rvec.x);
        file.write<float>(bag->orient.rvec.y);
        file.write<float>(bag->orient.rvec.z);
        file.write<float>(bag->orient.uvec.x);
        file.write<float>(bag->orient.uvec.y);
        file.write<float>(bag->orient.uvec.z);
        file.write<float>(bag->orient.fvec.x);
        file.write<float>(bag->orient.fvec.y);
        file.write<float>(bag->orient.fvec.z);
    }

    level.EndRflSection(file, start_pos);
}

void bag_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len)
{
    auto& bags = level.GetAlpineLevelProperties().bag_objects;
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
    if (count > 100) count = 100;

    for (uint32_t i = 0; i < count; i++) {
        auto* bag = new DedBag();
        memset(static_cast<DedObject*>(bag), 0, sizeof(DedObject));
        bag->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
        bag->type = DedObjectType::DED_BAG;

        if (!read_bytes(&bag->uid, sizeof(bag->uid))) { destroy_ded_bag(bag); return; }
        if (!read_bytes(&bag->pos.x, sizeof(float))) { destroy_ded_bag(bag); return; }
        if (!read_bytes(&bag->pos.y, sizeof(float))) { destroy_ded_bag(bag); return; }
        if (!read_bytes(&bag->pos.z, sizeof(float))) { destroy_ded_bag(bag); return; }
        if (!read_bytes(&bag->orient.rvec.x, sizeof(float))) { destroy_ded_bag(bag); return; }
        if (!read_bytes(&bag->orient.rvec.y, sizeof(float))) { destroy_ded_bag(bag); return; }
        if (!read_bytes(&bag->orient.rvec.z, sizeof(float))) { destroy_ded_bag(bag); return; }
        if (!read_bytes(&bag->orient.uvec.x, sizeof(float))) { destroy_ded_bag(bag); return; }
        if (!read_bytes(&bag->orient.uvec.y, sizeof(float))) { destroy_ded_bag(bag); return; }
        if (!read_bytes(&bag->orient.uvec.z, sizeof(float))) { destroy_ded_bag(bag); return; }
        if (!read_bytes(&bag->orient.fvec.x, sizeof(float))) { destroy_ded_bag(bag); return; }
        if (!read_bytes(&bag->orient.fvec.y, sizeof(float))) { destroy_ded_bag(bag); return; }
        if (!read_bytes(&bag->orient.fvec.z, sizeof(float))) { destroy_ded_bag(bag); return; }

        bag->script_name.assign_0("Bag");

        bags.push_back(bag);
        level.master_objects.add(static_cast<DedObject*>(bag));
    }

    xlog::info("[Bag] Loaded {} bag object(s)", bags.size());
}

void PlaceNewBagObject()
{
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto* bag = new DedBag();
    memset(static_cast<DedObject*>(bag), 0, sizeof(DedObject));
    bag->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
    bag->type = DedObjectType::DED_BAG;

    bag->script_name.assign_0("Bag");

    auto* viewport = get_active_viewport();
    if (viewport && viewport->view_data) {
        bag->pos = viewport->view_data->camera_pos;
    }

    bag->orient.rvec = {1.0f, 0.0f, 0.0f};
    bag->orient.uvec = {0.0f, 1.0f, 0.0f};
    bag->orient.fvec = {0.0f, 0.0f, 1.0f};

    bag->uid = generate_uid();

    level->GetAlpineLevelProperties().bag_objects.push_back(bag);
    level->master_objects.add(static_cast<DedObject*>(bag));

    level->clear_selection();
    level->add_to_selection(static_cast<DedObject*>(bag));
    level->update_console_display();
}

DedBag* CloneBagObject(DedBag* source, bool add_to_level)
{
    if (!source) return nullptr;

    auto* bag = new DedBag();
    memset(static_cast<DedObject*>(bag), 0, sizeof(DedObject));
    bag->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
    bag->type = DedObjectType::DED_BAG;

    bag->pos = source->pos;
    bag->orient = source->orient;
    bag->script_name.assign_0(source->script_name.c_str());

    bag->uid = generate_uid();

    if (add_to_level) {
        auto* level = CDedLevel::Get();
        if (level) {
            level->GetAlpineLevelProperties().bag_objects.push_back(bag);
            level->master_objects.add(static_cast<DedObject*>(bag));
        }
    }

    return bag;
}

void DeleteBagObject(DedBag* bag)
{
    if (!bag) return;
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto& bags = level->GetAlpineLevelProperties().bag_objects;
    auto it = std::find(bags.begin(), bags.end(), bag);
    if (it != bags.end()) {
        bags.erase(it);
    }
    level->master_objects.remove_by_value(static_cast<DedObject*>(bag));
    destroy_ded_bag(bag);
}

void bag_render(CDedLevel* level)
{
    auto& bags = level->GetAlpineLevelProperties().bag_objects;
    if (bags.empty()) return;

    bag_load_icon();

    float cam_param = gr_cam_param;

    for (auto* bag : bags) {
        if (bag->hidden_in_editor) continue;

        bool selected = is_object_selected(level, bag);

        if (selected) {
            set_draw_color(0xff, 0x00, 0x00, 0xff);
        }
        else {
            set_draw_color(0x4f, 0xcf, 0x16, 0xff); // green
        }

        if (g_bag_icon_handle >= 0) {
            gr_set_bitmap(g_bag_icon_handle, -1);
        }

        gr_render_billboard(&bag->pos, 0, 0.25f, cam_param);
    }
}

void bag_pick(CDedLevel* level, int param1, int param2)
{
    auto& bags = level->GetAlpineLevelProperties().bag_objects;
    for (auto* bag : bags) {
        if (bag->hidden_in_editor) continue;
        bool hit = level->hit_test_point(param1, param2, &bag->pos);
        if (hit) {
            level->select_object(static_cast<DedObject*>(bag));
        }
    }
}

DedBag* bag_click_pick(CDedLevel* level, float click_x, float click_y)
{
    auto& bags = level->GetAlpineLevelProperties().bag_objects;
    float best_dist_sq = 1e30f;
    DedBag* best_bag = nullptr;

    for (auto* bag : bags) {
        if (bag->hidden_in_editor) continue;

        float center_pos[3] = {bag->pos.x, bag->pos.y, bag->pos.z};
        float screen_cx = 0.0f, screen_cy = 0.0f;
        if (!project_to_screen_2d(center_pos, &screen_cx, &screen_cy))
            continue;

        constexpr float screen_radius_sq = 400.0f; // 20px

        float dx = screen_cx - click_x;
        float dy = screen_cy - click_y;
        float dist_sq = dx * dx + dy * dy;
        if (dist_sq <= screen_radius_sq && dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            best_bag = bag;
        }
    }

    return best_bag;
}

void bag_tree_populate(EditorTreeCtrl* tree, int master_groups, CDedLevel* level)
{
    auto& bags = level->GetAlpineLevelProperties().bag_objects;

    char buf[64];
    snprintf(buf, sizeof(buf), "Bags (%d)", static_cast<int>(bags.size()));
    int parent = tree->insert_item(buf, master_groups, 0xffff0002);

    for (auto* bag : bags) {
        const char* name = bag->script_name.c_str();
        if (!name || name[0] == '\0') {
            name = "(unnamed bag)";
        }
        int child = tree->insert_item(name, parent, 0xffff0002);
        tree->set_item_data(child, bag->uid);
    }
}

void bag_tree_add_object_type(EditorTreeCtrl* tree)
{
    tree->insert_item("Bag", 0xffff0000, 0xffff0002);
}

bool bag_copy_object(DedObject* source)
{
    if (!source || source->type != DedObjectType::DED_BAG) return false;
    auto* staged = CloneBagObject(static_cast<DedBag*>(source), false);
    if (staged) {
        g_bag_clipboard.push_back(staged);
        return true;
    }
    return false;
}

void bag_paste_objects(CDedLevel* level)
{
    for (auto* staged : g_bag_clipboard) {
        auto* clone = CloneBagObject(staged, true);
        if (clone) {
            level->add_to_selection(static_cast<DedObject*>(clone));
        }
    }
}

void bag_clear_clipboard()
{
    for (auto* bag : g_bag_clipboard) {
        destroy_ded_bag(bag);
    }
    g_bag_clipboard.clear();
}

void bag_handle_delete_or_cut(DedObject* obj)
{
    if (!obj || obj->type != DedObjectType::DED_BAG) return;
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto& bag_objects = level->GetAlpineLevelProperties().bag_objects;
    auto it = std::find(bag_objects.begin(), bag_objects.end(), static_cast<DedBag*>(obj));
    if (it != bag_objects.end()) {
        bag_objects.erase(it);
    }
}

void bag_handle_delete_selection(CDedLevel* level)
{
    auto& sel = level->selection;
    for (int i = sel.size - 1; i >= 0; i--) {
        DedObject* obj = sel.data_ptr[i];
        if (obj && obj->type == DedObjectType::DED_BAG) {
            for (int j = i; j < sel.size - 1; j++) {
                sel.data_ptr[j] = sel.data_ptr[j + 1];
            }
            sel.size--;
            DeleteBagObject(static_cast<DedBag*>(obj));
        }
    }
}

void bag_ensure_uid(int& uid)
{
    auto* level = CDedLevel::Get();
    if (!level) return;
    for (auto* b : level->GetAlpineLevelProperties().bag_objects) {
        if (b->uid >= uid) uid = b->uid + 1;
    }
}
