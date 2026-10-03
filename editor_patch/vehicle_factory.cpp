#include <windows.h>
#include <commctrl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <algorithm>
#include <vector>
#include <xlog/xlog.h>
#include <common/rfl_chunk_reader.h>
#include <common/utils/string-utils.h>
#include <common/vehicle_meshes.h>
#include <common/vehicle_orient.h>
#include "vehicle_factory.h"
#include "level.h"
#include "resources.h"
#include "vtypes.h"
#include "alpine_obj.h"
#include "alpine_spinner.h"
#include "tbl.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{

// ─── Constants ───────────────────────────────────────────────────────────────

// Drop-list fallback for an install whose entity.tbl could not be read. An off-list class already
// on a level is kept by the dialog's append path.
const char* const g_stock_vehicle_classes[] = {
    // $Use: "vehicle"
    "Jeep01", "APC", "Fighter01", "sub", "Driller01",
    // $Use: "turret"
    "Stationary Turret_Plain",
};

// Offered by entity.tbl but not supported by factories; kept off the drop-list only.
const char* const g_unsupported_vehicle_classes[] = {
    "masako_fighter", "Stationary Turret", "Shuttle",
};

bool vehicle_factory_class_is_unsupported(const std::string& class_name)
{
    return std::any_of(std::begin(g_unsupported_vehicle_classes), std::end(g_unsupported_vehicle_classes),
        [&](const char* name) { return string_iequals(class_name, name); });
}

// Preview meshes for the stock classes, used only when entity.tbl is unavailable.
const AlpineVehicleClassMesh g_fallback_meshes[] = {
    {"Jeep01", "jeep01.v3c"},
    {"APC", "apc01.v3c"},
    {"Fighter01", "fighter01.v3c"},
    {"masako_fighter", "masakofighter.v3c"},
    {"sub", "sub01.v3c"},
    {"Driller01", "driller01.v3c"},
    {"Shuttle", "shuttle_3rd.V3D"},
    {"Stationary Turret", "sturret_head.v3m"},
    {"Stationary Turret_Plain", "sentry_turret_plain.v3m"},
};

// Selection/culling sphere used until a preview mesh supplies a real one.
constexpr float vehicle_factory_fallback_radius = 0.75f;

// DedVehicleFactory's own default, used wherever a delay arrives non-finite.
constexpr float vehicle_factory_default_respawn_delay_s = 30.0f;
constexpr float vehicle_factory_max_respawn_delay_s = 3600.0f;

// ─── Globals ─────────────────────────────────────────────────────────────────

int g_vehicle_factory_icon_handle = -1;
std::vector<DedVehicleFactory*> g_vehicle_factory_clipboard;

void vehicle_factory_load_icon()
{
    if (g_vehicle_factory_icon_handle < 0) {
        // No factory icon ships yet; the bag sprite stands in, drawn in the factory's own colour.
        g_vehicle_factory_icon_handle = bm_load("Icon_AFBag.tga", -1, 1);
    }
}

// ─── Mesh preview ────────────────────────────────────────────────────────────

EditorVMesh* get_preview_vmesh(DedVehicleFactory* factory)
{
    return static_cast<EditorVMesh*>(factory->preview_vmesh);
}

void vehicle_factory_free_preview(DedVehicleFactory* factory)
{
    if (auto* v = get_preview_vmesh(factory)) {
        vmesh_free(v);
    }
    factory->preview_vmesh = nullptr;
    factory->preview_class.clear();
    factory->preview_load_failed = false;
    factory->preview_bound_center[0] = 0.0f;
    factory->preview_bound_center[1] = 0.0f;
    factory->preview_bound_center[2] = 0.0f;
    factory->preview_bound_radius = 0.0f;
}

// World-space bounding sphere of the preview mesh; falls back to the fixed radius around the
// factory's own origin while no preview is loaded.
void vehicle_factory_bound_sphere(const DedVehicleFactory* factory, float* out_center,
    float* out_radius)
{
    const float* c = factory->preview_bound_center;
    const Vector3 center = factory->pos + factory->orient * Vector3{c[0], c[1], c[2]};
    out_center[0] = center.x;
    out_center[1] = center.y;
    out_center[2] = center.z;
    *out_radius = factory->preview_bound_radius > 0.0f ? factory->preview_bound_radius
                                                       : vehicle_factory_fallback_radius;
}

// The AF override the game will apply wins; otherwise entity.tbl is authoritative (it carries the
// extension) and the fallback table covers the stock classes.
std::string vehicle_factory_mesh_for_class(const std::string& class_name)
{
    std::string filename;
    // The game rewrites $V3D Filename for these classes at level init, so the preview has to show
    // the hull that will actually spawn.
    for (const auto& entry : alpine_vehicle_class_meshes) {
        if (!string_iequals(entry.class_name, class_name)) {
            continue;
        }
        // Probed, so an install without the AF assets still previews the stock hull.
        rf::File file;
        if (file.open(entry.vmesh_filename)) {
            filename = entry.vmesh_filename;
        }
        break;
    }
    if (filename.empty()) {
        if (const auto* ei = entity_tbl_find(class_name.c_str())) {
            filename = ei->v3d_filename;
        }
    }
    if (filename.empty()) {
        for (const auto& entry : g_fallback_meshes) {
            if (string_iequals(entry.class_name, class_name)) {
                filename = entry.vmesh_filename;
                break;
            }
        }
    }
    if (filename.empty()) {
        return filename;
    }
    replace_ext_if(filename, "v3d", "v3m");
    replace_ext_if(filename, "vcm", "v3c");
    return filename;
}

void vehicle_factory_load_preview(DedVehicleFactory* factory)
{
    vehicle_factory_free_preview(factory);
    factory->preview_class = factory->vehicle_class;

    const std::string filename = vehicle_factory_mesh_for_class(factory->vehicle_class);
    if (filename.empty()) {
        factory->preview_load_failed = true;
        return;
    }
    // entity.tbl bounds nothing and RED's loader copies the name into VMesh::filename[65] with an
    // unbounded inline strcpy (0x004BE3A0).
    if (filename.size() > rfl_mesh_name_max_len || rfl_ext_over_long(filename)) {
        xlog::warn("[VehicleFactory] mesh name '{}' for class '{}' is too long to load", filename,
            factory->vehicle_class);
        factory->preview_load_failed = true;
        return;
    }

    auto ext = get_ext_from_filename(filename);
    EditorVMesh* vmesh = nullptr;
    if (string_iequals(ext, "v3m")) {
        vmesh = vmesh_load_v3m(filename.c_str(), 1, -1);
    }
    else if (string_iequals(ext, "v3c")) {
        // The v3c loader raises a fatal error for a missing file, so probe first.
        rf::File file;
        if (file.open(filename.c_str())) {
            vmesh = vmesh_load_v3c(filename.c_str(), 0, 0);
        }
        else {
            xlog::warn("[VehicleFactory] mesh '{}' not found for class '{}'", filename,
                factory->vehicle_class);
        }
    }

    factory->preview_vmesh = vmesh;
    factory->preview_load_failed = (vmesh == nullptr);
    if (vmesh) {
        vmesh->replacement_materials = nullptr;
        vmesh->use_replacement_materials = false;
        vmesh_get_bound_sphere(vmesh, factory->preview_bound_center, &factory->preview_bound_radius);
        if (factory->preview_bound_radius < 0.25f) {
            factory->preview_bound_radius = 0.25f;
        }
    }
}

// ─── Properties Dialog ──────────────────────────────────────────────────────

std::vector<DedVehicleFactory*> g_selected_factories;

INT_PTR CALLBACK VehicleFactoryDialogProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        if (g_selected_factories.empty()) {
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        auto* factory = g_selected_factories[0];

        SetDlgItemTextA(hdlg, IDC_VEHICLE_FACTORY_SCRIPT_NAME, factory->script_name.c_str());

        // The classes this install's entity.tbl offers the game, less the unsupported ones; a class
        // already on the object (unsupported or not) is appended rather than dropped.
        HWND cls = GetDlgItem(hdlg, IDC_VEHICLE_FACTORY_CLASS);
        const std::vector<std::string> tbl_classes =
            entity_tbl_class_names_with_use({ENTITY_USE_VEHICLE, ENTITY_USE_TURRET});
        if (tbl_classes.empty()) {
            // entity.tbl is missing or its .vpp is not mounted, so nothing was parsed.
            for (const char* name : g_stock_vehicle_classes) {
                SendMessageA(cls, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
            }
        }
        else {
            for (const std::string& name : tbl_classes) {
                if (!vehicle_factory_class_is_unsupported(name)) {
                    SendMessageA(cls, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
                }
            }
        }
        int cls_sel = static_cast<int>(SendMessageA(cls, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
            reinterpret_cast<LPARAM>(factory->vehicle_class.c_str())));
        if (cls_sel == CB_ERR) {
            cls_sel = factory->vehicle_class.empty()
                ? 0
                : static_cast<int>(SendMessageA(cls, CB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(factory->vehicle_class.c_str())));
        }
        SendMessage(cls, CB_SETCURSEL, cls_sel == CB_ERR ? 0 : cls_sel, 0);

        alpine_dlg_set_float_field(hdlg, IDC_VEHICLE_FACTORY_RESPAWN_DELAY, factory->respawn_delay_s);
        alpine_spinner_init(hdlg, IDC_VEHICLE_FACTORY_RESPAWN_DELAY, IDC_VEHICLE_FACTORY_RESPAWN_DELAY_SPIN,
                            1.0f, 0.0f, vehicle_factory_max_respawn_delay_s, 1);

        HWND team = GetDlgItem(hdlg, IDC_VEHICLE_FACTORY_TEAM);
        SendMessageA(team, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("None"));
        SendMessageA(team, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Red"));
        SendMessageA(team, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Blue"));
        SendMessage(team, CB_SETCURSEL, static_cast<int>(factory->team) + 1, 0);

        CheckDlgButton(hdlg, IDC_VEHICLE_FACTORY_LOCK_TEAM,
            factory->lock_to_team ? BST_CHECKED : BST_UNCHECKED);

        CheckDlgButton(hdlg, IDC_VEHICLE_FACTORY_ACTIVE,
            factory->active_by_default ? BST_CHECKED : BST_UNCHECKED);

        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            char name_buf[256] = {};
            GetDlgItemTextA(hdlg, IDC_VEHICLE_FACTORY_SCRIPT_NAME, name_buf, sizeof(name_buf));
            // Read the selection, not the window text: a drop-list has no edit control.
            char class_buf[64] = {};
            HWND cls_ok = GetDlgItem(hdlg, IDC_VEHICLE_FACTORY_CLASS);
            const int cls_cur = static_cast<int>(SendMessage(cls_ok, CB_GETCURSEL, 0, 0));
            const bool have_class = cls_cur != CB_ERR
                && SendMessage(cls_ok, CB_GETLBTEXTLEN, cls_cur, 0) < static_cast<LRESULT>(sizeof(class_buf));
            if (have_class) {
                SendMessageA(cls_ok, CB_GETLBTEXT, cls_cur, reinterpret_cast<LPARAM>(class_buf));
            }
            float delay = alpine_dlg_get_float_field(hdlg, IDC_VEHICLE_FACTORY_RESPAWN_DELAY);
            // "nan"/"inf" parse, and std::clamp propagates them rather than bounding them.
            if (!std::isfinite(delay)) {
                delay = vehicle_factory_default_respawn_delay_s;
            }
            delay = std::clamp(delay, 0.0f, vehicle_factory_max_respawn_delay_s);

            int team_sel = static_cast<int>(SendDlgItemMessage(hdlg, IDC_VEHICLE_FACTORY_TEAM,
                CB_GETCURSEL, 0, 0));
            auto team = (team_sel >= 1 && team_sel <= 2) ? static_cast<VehicleFactoryTeam>(team_sel - 1)
                                                         : VehicleFactoryTeam::none;
            bool lock_to_team = IsDlgButtonChecked(hdlg, IDC_VEHICLE_FACTORY_LOCK_TEAM) == BST_CHECKED;
            bool active = IsDlgButtonChecked(hdlg, IDC_VEHICLE_FACTORY_ACTIVE) == BST_CHECKED;

            // The script name is the factory's identity, so it alone does not bulk-apply.
            const bool single = g_selected_factories.size() == 1;

            for (auto* f : g_selected_factories) {
                if (single) {
                    f->script_name.assign_0(name_buf);
                }
                // A failed combo read is no answer at all, not an empty class name.
                if (have_class) {
                    // The render pass reloads at most one preview per frame, so drop the stale
                    // bounding sphere here rather than let picking use it until then.
                    if (f->vehicle_class != class_buf) {
                        vehicle_factory_free_preview(f);
                    }
                    f->vehicle_class = class_buf;
                }
                f->respawn_delay_s = delay;
                f->team = team;
                f->lock_to_team = lock_to_team;
                f->active_by_default = active;
            }
            EndDialog(hdlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_NOTIFY:
        if (alpine_spinner_handle_notify(hdlg, lp)) return TRUE;
        break;
    }
    return FALSE;
}

} // namespace

void ShowVehicleFactoryPropertiesDialog(CDedLevel* level)
{
    auto& sel = level->selection;
    g_selected_factories.clear();
    for (int i = 0; i < sel.get_size(); i++) {
        DedObject* obj = sel[i];
        if (obj && obj->type == DedObjectType::DED_VEHICLE_FACTORY) {
            g_selected_factories.push_back(static_cast<DedVehicleFactory*>(obj));
        }
    }

    if (!g_selected_factories.empty()) {
        DialogBoxParam(
            reinterpret_cast<HINSTANCE>(&__ImageBase),
            MAKEINTRESOURCE(IDD_ALPINE_VEHICLE_FACTORY_PROPERTIES),
            GetActiveWindow(),
            VehicleFactoryDialogProc,
            0
        );
    }

    g_selected_factories.clear();
}

// ─── Cleanup ─────────────────────────────────────────────────────────────────

void DestroyDedVehicleFactory(DedVehicleFactory* factory)
{
    if (!factory) return;
    vehicle_factory_free_preview(factory);
    factory->field_4.free();
    factory->script_name.free();
    factory->class_name.free();
    delete factory;
}

// ─── Serialization ──────────────────────────────────────────────────────────

// Vehicle Factory chunk (id 0x0AFBAE07, RFL 306+), no per-chunk version per the Alpine convention.
// vehicle_factory_load_chunk in game_patch/multi/vehicles/vehicle_spawn.cpp must stay in step.
void vehicle_factory_serialize_chunk(CDedLevel& level, rf::File& file)
{
    auto& factories = level.GetAlpineLevelProperties().vehicle_factory_objects;
    if (factories.empty()) return;

    auto start_pos = level.BeginRflSection(file, alpine_vehicle_factory_chunk_id);

    file.write<uint32_t>(static_cast<uint32_t>(factories.size()));

    for (auto* factory : factories) {
        file.write<int32_t>(factory->uid);
        file.write<float>(factory->pos.x);
        file.write<float>(factory->pos.y);
        file.write<float>(factory->pos.z);
        file.write<float>(factory->orient.rvec.x);
        file.write<float>(factory->orient.rvec.y);
        file.write<float>(factory->orient.rvec.z);
        file.write<float>(factory->orient.uvec.x);
        file.write<float>(factory->orient.uvec.y);
        file.write<float>(factory->orient.uvec.z);
        file.write<float>(factory->orient.fvec.x);
        file.write<float>(factory->orient.fvec.y);
        file.write<float>(factory->orient.fvec.z);
        write_rfl_string(file, factory->script_name);
        write_rfl_string(file, factory->vehicle_class);
        file.write<float>(factory->respawn_delay_s);
        // 5 bytes: team (0xFF none), two reserved, lock_to_team, active_by_default.
        file.write<uint8_t>(factory->team == VehicleFactoryTeam::none
            ? 0xFFu : static_cast<uint8_t>(factory->team));
        file.write<uint8_t>(0u);
        file.write<uint8_t>(0u);
        file.write<uint8_t>(factory->lock_to_team ? 1u : 0u);
        file.write<uint8_t>(factory->active_by_default ? 1u : 0u);
    }

    level.EndRflSection(file, start_pos);
}

void vehicle_factory_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len)
{
    auto& factories = level.GetAlpineLevelProperties().vehicle_factory_objects;
    std::size_t remaining = chunk_len;

    rf::File::ChunkGuard chunk_guard{file, remaining};
    RflChunkReader<rf::File> reader{file, remaining};

    uint32_t count = 0;
    if (!reader.read_bytes(&count, sizeof(count))) return;
    // The cap is level-wide, as the game applies it across every chunk.
    const uint32_t loaded = static_cast<uint32_t>(std::min<std::size_t>(factories.size(), vehicle_factory_max_records));
    count = std::min(count, vehicle_factory_max_records - loaded);

    for (uint32_t i = 0; i < count; i++) {
        auto* factory = new DedVehicleFactory();
        memset(static_cast<DedObject*>(factory), 0, sizeof(DedObject));
        factory->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
        factory->type = DedObjectType::DED_VEHICLE_FACTORY;

        if (!reader.read_bytes(&factory->uid, sizeof(factory->uid))) { DestroyDedVehicleFactory(factory); return; }
        if (!reader.read_bytes(&factory->pos.x, sizeof(float))) { DestroyDedVehicleFactory(factory); return; }
        if (!reader.read_bytes(&factory->pos.y, sizeof(float))) { DestroyDedVehicleFactory(factory); return; }
        if (!reader.read_bytes(&factory->pos.z, sizeof(float))) { DestroyDedVehicleFactory(factory); return; }
        if (!reader.read_bytes(&factory->orient.rvec.x, sizeof(float))) { DestroyDedVehicleFactory(factory); return; }
        if (!reader.read_bytes(&factory->orient.rvec.y, sizeof(float))) { DestroyDedVehicleFactory(factory); return; }
        if (!reader.read_bytes(&factory->orient.rvec.z, sizeof(float))) { DestroyDedVehicleFactory(factory); return; }
        if (!reader.read_bytes(&factory->orient.uvec.x, sizeof(float))) { DestroyDedVehicleFactory(factory); return; }
        if (!reader.read_bytes(&factory->orient.uvec.y, sizeof(float))) { DestroyDedVehicleFactory(factory); return; }
        if (!reader.read_bytes(&factory->orient.uvec.z, sizeof(float))) { DestroyDedVehicleFactory(factory); return; }
        if (!reader.read_bytes(&factory->orient.fvec.x, sizeof(float))) { DestroyDedVehicleFactory(factory); return; }
        if (!reader.read_bytes(&factory->orient.fvec.y, sizeof(float))) { DestroyDedVehicleFactory(factory); return; }
        if (!reader.read_bytes(&factory->orient.fvec.z, sizeof(float))) { DestroyDedVehicleFactory(factory); return; }

        if (!std::isfinite(factory->pos.x) || !std::isfinite(factory->pos.y)
            || !std::isfinite(factory->pos.z)) {
            factory->pos = {};
        }
        // The game drops a factory whose basis fails this test.
        if (!vehicle_orient_is_rotation(factory->orient)) {
            factory->orient = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
        }

        std::string sname;
        if (!reader.read_string(sname)) { DestroyDedVehicleFactory(factory); return; }
        factory->script_name.assign_0(sname.c_str());

        if (!reader.read_string(factory->vehicle_class)) { DestroyDedVehicleFactory(factory); return; }
        if (rfl_name_over_long(factory->vehicle_class)) factory->vehicle_class.clear();

        if (!reader.read_bytes(&factory->respawn_delay_s, sizeof(float))) { DestroyDedVehicleFactory(factory); return; }
        if (!std::isfinite(factory->respawn_delay_s)) {
            factory->respawn_delay_s = vehicle_factory_default_respawn_delay_s;
        }
        factory->respawn_delay_s =
            std::clamp(factory->respawn_delay_s, 0.0f, vehicle_factory_max_respawn_delay_s);
        uint8_t team = 0xFF;
        if (!reader.read_bytes(&team, sizeof(team))) { DestroyDedVehicleFactory(factory); return; }
        factory->team = (team == 0 || team == 1) ? static_cast<VehicleFactoryTeam>(team)
                                                 : VehicleFactoryTeam::none;
        uint8_t reserved[2] = {};
        if (!reader.read_bytes(reserved, sizeof(reserved))) { DestroyDedVehicleFactory(factory); return; }
        uint8_t lock_to_team = 0;
        if (!reader.read_bytes(&lock_to_team, sizeof(lock_to_team))) { DestroyDedVehicleFactory(factory); return; }
        // A team-none factory keeps its lock: a control point can hand it a team later.
        factory->lock_to_team = (lock_to_team != 0);
        uint8_t active = 1;
        if (!reader.read_bytes(&active, sizeof(active))) { DestroyDedVehicleFactory(factory); return; }
        factory->active_by_default = (active != 0);

        factories.push_back(factory);
        level.master_objects.add(static_cast<DedObject*>(factory));
    }

    xlog::info("[VehicleFactory] Loaded {} vehicle factory object(s)", factories.size());
}

// ─── Object Lifecycle ───────────────────────────────────────────────────────

void PlaceNewVehicleFactoryObject()
{
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto* factory = new DedVehicleFactory();
    memset(static_cast<DedObject*>(factory), 0, sizeof(DedObject));
    factory->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
    factory->type = DedObjectType::DED_VEHICLE_FACTORY;

    factory->script_name.assign_0("Vehicle Factory");

    auto* viewport = get_active_viewport();
    if (viewport && viewport->view_data) {
        factory->pos = viewport->view_data->camera_pos;
        factory->orient = viewport->view_data->camera_orient;
    }
    else {
        factory->orient.rvec = {1.0f, 0.0f, 0.0f};
        factory->orient.uvec = {0.0f, 1.0f, 0.0f};
        factory->orient.fvec = {0.0f, 0.0f, 1.0f};
    }

    factory->uid = generate_uid();

    level->GetAlpineLevelProperties().vehicle_factory_objects.push_back(factory);
    level->master_objects.add(static_cast<DedObject*>(factory));

    level->clear_selection();
    level->add_to_selection(static_cast<DedObject*>(factory));
    level->update_console_display();
}

DedVehicleFactory* CloneVehicleFactoryObject(DedVehicleFactory* source, bool add_to_level)
{
    if (!source) return nullptr;

    auto* factory = new DedVehicleFactory();
    memset(static_cast<DedObject*>(factory), 0, sizeof(DedObject));
    factory->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
    factory->type = DedObjectType::DED_VEHICLE_FACTORY;

    factory->pos = source->pos;
    factory->orient = source->orient;
    factory->script_name.assign_0(source->script_name.c_str());

    factory->vehicle_class = source->vehicle_class;
    factory->respawn_delay_s = source->respawn_delay_s;
    factory->team = source->team;
    factory->lock_to_team = source->lock_to_team;
    factory->active_by_default = source->active_by_default;

    factory->uid = generate_uid();

    if (add_to_level) {
        auto* level = CDedLevel::Get();
        if (level) {
            level->GetAlpineLevelProperties().vehicle_factory_objects.push_back(factory);
            level->master_objects.add(static_cast<DedObject*>(factory));
        }
    }

    return factory;
}

void DeleteVehicleFactoryObject(DedVehicleFactory* factory)
{
    if (!factory) return;
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto& factories = level->GetAlpineLevelProperties().vehicle_factory_objects;
    auto it = std::find(factories.begin(), factories.end(), factory);
    if (it != factories.end()) {
        factories.erase(it);
    }
    alpine_remove_from_groups(level, static_cast<DedObject*>(factory));
    level->master_objects.remove_by_value(static_cast<DedObject*>(factory));
    DestroyDedVehicleFactory(factory);
}

// ─── Rendering ──────────────────────────────────────────────────────────────

void vehicle_factory_render(CDedLevel* level)
{
    auto& factories = level->GetAlpineLevelProperties().vehicle_factory_objects;
    if (factories.empty()) return;

    float cam_param = gr_cam_param;
    bool did_lazy_load = false;

    for (auto* factory : factories) {
        if (factory->hidden_in_editor) continue;

        const bool selected = is_object_selected(level, factory);

        // One preview load per frame, matching the mesh object pass.
        if (factory->preview_class != factory->vehicle_class) {
            vehicle_factory_free_preview(factory);
        }
        if (!get_preview_vmesh(factory) && !factory->preview_load_failed && !did_lazy_load) {
            vehicle_factory_load_preview(factory);
            did_lazy_load = true;
        }

        float sphere_center[3] = {};
        float sphere_radius = 0.0f;
        vehicle_factory_bound_sphere(factory, sphere_center, &sphere_radius);

        auto* vm = get_preview_vmesh(factory);
        if (vm) {
            set_draw_color(0xff, 0xff, 0xff, 0xff);

            EditorRenderParams render_params;
            if (editor_textures_enabled != 0) {
                render_params.flags |= ERF_TEXTURED;
                render_params.diffuse_color = {0xff, 0xff, 0xff, 0xff};
            }
            if (selected) {
                render_params.flags |= ERF_SELECTION_HIGHLIGHT;
                render_params.selection_color = {0xff, 0x00, 0x00, 0xff};
            }

            room_setup(nullptr, &factory->pos, sphere_radius, 1, 1);
            vmesh_render(vm, &factory->pos, &factory->orient, &render_params);
            room_cleanup();
        }
        else {
            vehicle_factory_load_icon();
            if (selected) {
                set_draw_color(0xff, 0x00, 0x00, 0xff);
            }
            else {
                set_draw_color(0xff, 0x8c, 0x1a, 0xff); // orange
            }
            if (g_vehicle_factory_icon_handle >= 0) {
                gr_set_bitmap(g_vehicle_factory_icon_handle, -1);
            }
            gr_render_billboard(&factory->pos, 0, 0.25f, cam_param);
        }

        // Facing arrow: the direction the spawned vehicle points.
        draw_3d_arrow(
            factory->pos.x, factory->pos.y, factory->pos.z,
            factory->pos.x + factory->orient.fvec.x * 2.0f,
            factory->pos.y + factory->orient.fvec.y * 2.0f,
            factory->pos.z + factory->orient.fvec.z * 2.0f,
            selected ? 255 : 0, selected ? 0 : 255, 255);

        if (selected) {
            draw_wireframe_sphere(sphere_center[0], sphere_center[1], sphere_center[2],
                sphere_radius, 255, 0, 0);
        }
    }
}

void vehicle_factory_pick(CDedLevel* level, int param1, int param2)
{
    auto& factories = level->GetAlpineLevelProperties().vehicle_factory_objects;
    for (auto* factory : factories) {
        if (factory->hidden_in_editor) continue;
        if (level->hit_test_point(param1, param2, &factory->pos)) {
            level->select_object(static_cast<DedObject*>(factory));
        }
    }
}

DedVehicleFactory* vehicle_factory_click_pick(CDedLevel* level, float click_x, float click_y,
    float* out_dist_sq)
{
    auto& factories = level->GetAlpineLevelProperties().vehicle_factory_objects;
    float best_dist_sq = 1e30f;
    DedVehicleFactory* best = nullptr;

    for (auto* factory : factories) {
        if (factory->hidden_in_editor) continue;

        float center_pos[3] = {};
        float bound_radius = 0.0f;
        vehicle_factory_bound_sphere(factory, center_pos, &bound_radius);

        float screen_cx = 0.0f, screen_cy = 0.0f;
        if (!project_to_screen_2d(center_pos, &screen_cx, &screen_cy))
            continue;

        float edge_pos[3] = {center_pos[0], center_pos[1] + bound_radius, center_pos[2]};
        float screen_ex = 0.0f, screen_ey = 0.0f;
        float screen_radius_sq;
        if (project_to_screen_2d(edge_pos, &screen_ex, &screen_ey)) {
            float rdx = screen_ex - screen_cx;
            float rdy = screen_ey - screen_cy;
            screen_radius_sq = rdx * rdx + rdy * rdy;
        }
        else {
            screen_radius_sq = 400.0f; // fallback 20px
        }
        if (screen_radius_sq < 100.0f) screen_radius_sq = 100.0f; // floor at 10px

        float dx = screen_cx - click_x;
        float dy = screen_cy - click_y;
        float dist_sq = dx * dx + dy * dy;
        if (dist_sq <= screen_radius_sq && dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            best = factory;
        }
    }

    if (out_dist_sq) *out_dist_sq = best_dist_sq;
    return best;
}

void vehicle_factory_tree_populate(EditorTreeCtrl* tree, int master_groups, CDedLevel* level)
{
    auto& factories = level->GetAlpineLevelProperties().vehicle_factory_objects;

    char buf[64];
    snprintf(buf, sizeof(buf), "Vehicle Factories (%d)", static_cast<int>(factories.size()));
    int parent = tree->insert_item(buf, master_groups, 0xffff0002);

    for (auto* factory : factories) {
        const char* name = factory->script_name.c_str();
        if (!name || name[0] == '\0') {
            name = "(unnamed vehicle factory)";
        }
        int child = tree->insert_item(name, parent, 0xffff0002);
        tree->set_item_data(child, factory->uid);
    }
}

void vehicle_factory_tree_add_object_type(EditorTreeCtrl* tree)
{
    tree->insert_item("Vehicle Factory", 0xffff0000, 0xffff0002);
}

bool vehicle_factory_copy_object(DedObject* source)
{
    if (!source || source->type != DedObjectType::DED_VEHICLE_FACTORY) return false;
    auto* staged = CloneVehicleFactoryObject(static_cast<DedVehicleFactory*>(source), false);
    if (staged) {
        g_vehicle_factory_clipboard.push_back(staged);
        return true;
    }
    return false;
}

void vehicle_factory_paste_objects(CDedLevel* level)
{
    for (auto* staged : g_vehicle_factory_clipboard) {
        auto* clone = CloneVehicleFactoryObject(staged, true);
        if (clone) {
            level->add_to_selection(static_cast<DedObject*>(clone));
        }
    }
}

void vehicle_factory_clear_clipboard()
{
    for (auto* factory : g_vehicle_factory_clipboard) {
        DestroyDedVehicleFactory(factory);
    }
    g_vehicle_factory_clipboard.clear();
}

void vehicle_factory_handle_delete_or_cut(DedObject* obj)
{
    if (!obj || obj->type != DedObjectType::DED_VEHICLE_FACTORY) return;
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto& factories = level->GetAlpineLevelProperties().vehicle_factory_objects;
    auto it = std::find(factories.begin(), factories.end(), static_cast<DedVehicleFactory*>(obj));
    if (it != factories.end()) {
        factories.erase(it);
    }
}

void vehicle_factory_handle_delete_selection(CDedLevel* level)
{
    alpine_compact_selection<DedVehicleFactory>(level, DedObjectType::DED_VEHICLE_FACTORY,
                                                DeleteVehicleFactoryObject);
}

void vehicle_factory_ensure_uid(int& uid)
{
    auto* level = CDedLevel::Get();
    if (!level) return;
    alpine_ensure_uid(level->GetAlpineLevelProperties().vehicle_factory_objects, uid);
}
