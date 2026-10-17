#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <xlog/xlog.h>
#include <patch_common/CallHook.h>
#include <common/scope_guard.h>
#include <common/terrain/alpine_terrain.h>
#include <common/utils/string-utils.h>
#include "alpine_obj.h"
#include "level.h"
#include "mesh.h"
#include "mesh_browser.h"
#include "mfc_types.h"
#include "placement_panel.h"
#include "resources.h"
#include "tbl.h"
#include "terrain_build.h"
#include "terrain_preview.h"
#include "vehicle_factory.h"
#include "vtypes.h"

namespace at = alpine_terrain;

namespace
{

// ─── Tabs ───────────────────────────────────────────────────────────────────

enum PanelTab : int
{
    tab_clutter,
    tab_entity,
    tab_item,
    tab_vehicle,
    tab_mesh,
    tab_count,
};

struct TabInfo
{
    const char* label; // for the stock types also the label of their root in the object tree
    DedObjectType object_type;
    float default_height;
    bool height_from_bottom; // else from the object's origin
};

constexpr TabInfo tab_info[tab_count] = {
    {"Clutter", DedObjectType::DED_CLUTTER, 0.0f, true},
    {"Entity", DedObjectType::DED_ENTITY, 0.0f, true},
    {"Item", DedObjectType::DED_ITEM, 1.0f, false},
    {"Vehicle", DedObjectType::DED_VEHICLE_FACTORY, 1.0f, true},
    {"Mesh", DedObjectType::DED_MESH, 0.0f, true},
};

bool is_stock_tab(int tab)
{
    return tab == tab_clutter || tab == tab_entity || tab == tab_item;
}

// Kept for the session only.
struct TabSettings
{
    std::string cls; // the mesh filename on the Mesh tab
    bool override_height = false;
    std::optional<float> height;
};

TabSettings g_tab_settings[tab_count];
int g_tab = tab_clutter;
std::vector<std::string> g_recent_meshes;
constexpr std::size_t recent_meshes_max = 10;
bool g_collapsed = false;

// ─── Constants ──────────────────────────────────────────────────────────────

constexpr UINT_PTR tick_timer_id = 1;
constexpr UINT tick_ms = 33;
constexpr float spin_deg_per_s = 30.0f;
constexpr float preview_pitch_deg = 20.0f;
constexpr float preview_zoom = 2.4f;
constexpr int combo_drop_dlu = 150;
constexpr int max_tab_padding = 8;
constexpr int min_tab_padding = 3;
constexpr int tab_padding_y = 3;
// The selected tab is drawn wider than its item rectangle.
constexpr int tab_strip_slack = 4;

// Surfaces whose normal is closer to horizontal than this are walls and ceilings: the ray passes them.
constexpr float min_up_normal_y = 0.5f;
// Without a floor under the cursor the object goes this far along the ray, short of the first wall.
constexpr float fallback_distance = 10.0f;
constexpr float wall_clearance = 0.25f;
constexpr int max_terrain_wall_skips = 32;
constexpr ULONGLONG other_views_repaint_ms = 100;
constexpr float marker_half_size = 0.25f;

// Tree drags of the types the panel has no tab for.
constexpr float tree_default_height = 1.0f;
constexpr float respawn_point_height = 2.0f;
// Their orientation is a direction (a spot light's cone, a decal's projection...), so a drag aims them along the
// camera as a double-click does.
constexpr const char* camera_aimed_types[] = {
    "Light",       "Decal",           "Particle Emitter",  "Bolt Emitter",
    "Push Region", "Cutscene Camera", "Directional Light", "Projection Camera",
};

constexpr int layout_ids[] = {
    IDC_PLACEMENT_COLLAPSE, IDC_PLACEMENT_TABS,         IDC_PLACEMENT_CLASS,  IDC_PLACEMENT_BROWSE,
    IDC_PLACEMENT_PREVIEW,  IDC_PLACEMENT_HEIGHT_LABEL, IDC_PLACEMENT_HEIGHT, IDC_PLACEMENT_HEIGHT_OVERRIDE,
};
constexpr std::size_t layout_count = std::size(layout_ids);

std::size_t layout_index(int id)
{
    const auto* it = std::find(std::begin(layout_ids), std::end(layout_ids), id);
    return static_cast<std::size_t>(it - std::begin(layout_ids));
}

// ─── State ──────────────────────────────────────────────────────────────────

// A mesh shown for a class (the preview, a tree drag's ghost); placements rest its bounding box bottom.
struct PlacementMesh
{
    EditorVMesh* vmesh = nullptr;
    void* owned_character = nullptr;
    Vector3 bound_center;
    float bound_radius = 1.0f;
    Vector3 bbox_min;
    Vector3 bbox_max;
};

struct DropRule
{
    float height = 0.0f;
    bool from_bottom = false;  // of the mesh's bounding box, else the object's origin
    float footprint = 0.0f;    // a terrain's square, centred on the drop point
    bool camera_aimed = false; // a type whose orientation is a direction takes the camera's, else upright
};

struct PanelState
{
    EditorObjectPanel* object_panel = nullptr;
    HWND form = nullptr;
    WNDPROC form_proc = nullptr;
    WNDPROC tree_proc = nullptr;
    HTREEITEM tree_press_item = nullptr; // pressed with the left button, not yet dragged
    POINT tree_press{};
    HWND hwnd = nullptr;
    WNDPROC preview_proc = nullptr;
    HHOOK msg_hook = nullptr;
    HFONT glyph_font = nullptr;

    int template_width = 0;
    int template_height = 0;
    int combo_drop_height = 0;
    RECT template_rects[layout_count] = {};
    int tab_fit_width = -1;

    std::vector<std::string> classes; // the current tab's choices
    bool updating = false;

    PlacementMesh preview;
    float yaw = 30.0f;
    ULONGLONG last_tick = 0;
};

PanelState g_panel;

struct DragState
{
    bool pressed = false;
    bool active = false; // past the drag threshold
    POINT press{};
    HWND capture = nullptr;
    HTREEITEM tree_item = nullptr; // a drag out of the object tree, else out of the preview
    PlacementMesh tree_mesh;
    DropRule rule;
    void* view = nullptr;
    bool valid = false; // pos and orient are where a release places the object
    Vector3 pos;
    Matrix3 orient;
    float top_y = NAN; // the top view casts down from here
    ULONGLONG others_repainted = 0;
    bool others_pending = false;
};

DragState g_drag;

// While set, the tree double-click handler's hit test returns this item instead of the one under the cursor.
// A flag because the panel calls that handler (0x004431C0) itself, with no click to tell the calls apart.
HTREEITEM g_forced_tree_item = nullptr;

// ─── Class lists and meshes ─────────────────────────────────────────────────

bool string_iless(const std::string& a, const std::string& b)
{
    return _stricmp(a.c_str(), b.c_str()) < 0;
}

// The classes the object tree lists for a stock type (0x00443610 lists the templates with listed_in_tree, by
// class_name).
std::vector<std::string> stock_classes(DedObjectType type)
{
    std::vector<std::string> names;
    const CDedLevel* level = CDedLevel::Get();
    if (!level) return names;
    const auto& classes = level->object_classes;
    for (int i = 0; i < classes.size; i++) {
        const DedObjectClass* entry = classes.data_ptr[i];
        const DedObject* tmpl = entry ? entry->tmpl : nullptr;
        if (tmpl && tmpl->type == type && tmpl->listed_in_tree && !tmpl->class_name.empty()) {
            names.emplace_back(tmpl->class_name.c_str());
        }
    }
    std::sort(names.begin(), names.end(), string_iless);
    names.erase(std::unique(names.begin(), names.end(),
                            [](const std::string& a, const std::string& b) { return string_iequals(a, b); }),
                names.end());
    return names;
}

std::vector<std::string> tab_classes(int tab)
{
    if (is_stock_tab(tab)) return stock_classes(tab_info[tab].object_type);
    if (tab == tab_vehicle) {
        std::vector<std::string> names = vehicle_factory_class_choices();
        std::sort(names.begin(), names.end(), string_iless);
        return names;
    }
    return g_recent_meshes;
}

void normalize_mesh_name(std::string& name)
{
    replace_ext_if(name, "v3d", "v3m");
    replace_ext_if(name, "vcm", "v3c");
}

// The mesh files a class may be drawn from, best first. A stock class's template carries the file its new
// objects load (0x00452680 copies it, 0x00414C20 loads it); the tables cover a template without one.
std::vector<std::string> class_meshes(int tab, const std::string& cls)
{
    std::vector<std::string> names;
    if (cls.empty()) return names;
    if (tab == tab_mesh) {
        names.push_back(cls);
        return names;
    }
    if (tab == tab_vehicle) {
        names.push_back(vehicle_factory_mesh_for_class(cls));
        return names;
    }
    if (const CDedLevel* level = CDedLevel::Get()) {
        const auto& classes = level->object_classes;
        for (int i = 0; i < classes.size; i++) {
            const DedObjectClass* entry = classes.data_ptr[i];
            const DedObject* tmpl = entry ? entry->tmpl : nullptr;
            if (tmpl && tmpl->type == tab_info[tab].object_type && string_iequals(tmpl->class_name.c_str(), cls)) {
                names.emplace_back(tmpl->class_mesh_filename.c_str());
                break;
            }
        }
    }
    if (tab == tab_clutter) {
        if (const ClutterClassInfo* info = clutter_tbl_find(cls.c_str())) names.push_back(info->v3d_filename);
    }
    else if (tab == tab_entity) {
        if (const EntityClassInfo* info = entity_tbl_find(cls.c_str())) names.push_back(info->v3d_filename);
    }
    for (std::string& name : names) normalize_mesh_name(name);
    return names;
}

// RED's loader copies the name into VMesh::filename[65] unbounded (0x004BE3A0).
bool mesh_name_fits(const std::string& name)
{
    return !name.empty() && name.size() <= rfl_mesh_name_max_len && !rfl_ext_over_long(name);
}

bool mesh_name_usable(const std::string& name)
{
    if (!mesh_name_fits(name)) return false;
    const std::string_view ext = get_ext_from_filename(name);
    if (!string_iequals(ext, "v3m") && !string_iequals(ext, "v3c") && !string_iequals(ext, "vfx")) return false;
    rf::File file;
    return file.open(name.c_str());
}

void remember_mesh(const std::string& name)
{
    std::erase_if(g_recent_meshes, [&](const std::string& m) { return string_iequals(m, name); });
    g_recent_meshes.insert(g_recent_meshes.begin(), name);
    if (g_recent_meshes.size() > recent_meshes_max) g_recent_meshes.resize(recent_meshes_max);
}

// ─── Preview ────────────────────────────────────────────────────────────────

HWND preview_hwnd()
{
    return g_panel.hwnd ? GetDlgItem(g_panel.hwnd, IDC_PLACEMENT_PREVIEW) : nullptr;
}

void free_mesh(PlacementMesh& m)
{
    mesh_preview_free(m.vmesh, m.owned_character);
    m = PlacementMesh{};
}

// The first of `names` that loads.
void load_mesh(PlacementMesh& m, const std::vector<std::string>& names)
{
    free_mesh(m);
    for (const std::string& name : names) {
        if (!mesh_name_fits(name)) continue;
        m.vmesh = mesh_preview_load(name.c_str(), m.owned_character);
        if (m.vmesh) break;
    }
    if (!m.vmesh) return;
    vmesh_get_bound_sphere(m.vmesh, &m.bound_center, &m.bound_radius);
    if (!(m.bound_radius > 0.01f)) m.bound_radius = 0.5f;
    vmesh_get_bbox(m.vmesh, &m.bbox_min, &m.bbox_max);
    if (!std::isfinite(m.bbox_min.y)) m.bbox_min = {};
    if (!std::isfinite(m.bbox_max.y)) m.bbox_max = {};
}

void load_preview()
{
    load_mesh(g_panel.preview, class_meshes(g_tab, g_tab_settings[g_tab].cls));
    if (HWND preview = preview_hwnd()) InvalidateRect(preview, nullptr, FALSE);
}

// Not inside a viewport frame, and not while a modal window (the mesh browser among them) owns gr.
bool preview_can_draw()
{
    const HWND frame = GetMainFrameHandle();
    return !gr_batch_open && frame && IsWindowEnabled(frame) && !IsIconic(frame);
}

// Worth animating: RED (or a window it owns) is in front and the preview is not clipped away inside RED.
bool preview_shown(HWND preview)
{
    const HWND frame = GetMainFrameHandle();
    const HWND foreground = GetForegroundWindow();
    if (!frame || !foreground || (foreground != frame && GetAncestor(foreground, GA_ROOTOWNER) != frame)) {
        return false;
    }
    if (!IsWindowVisible(preview)) return false;
    // Not GetDC: the static class is CS_PARENTDC, whose DC clips to the panel instead of the preview.
    HDC dc = GetDCEx(preview, nullptr, DCX_CACHE | DCX_CLIPSIBLINGS);
    if (!dc) return false;
    RECT clip{};
    const int region = GetClipBox(dc, &clip);
    ReleaseDC(preview, dc);
    return region != NULLREGION && region != ERROR;
}

void draw_preview(const DRAWITEMSTRUCT& dis)
{
    const Color background = mesh_preview_editor_background();
    if (!preview_can_draw()) {
        if (HBRUSH brush = CreateSolidBrush(RGB(background.r, background.g, background.b))) {
            FillRect(dis.hDC, &dis.rcItem, brush);
            DeleteObject(brush);
        }
        return;
    }
    const PlacementMesh& m = g_panel.preview;
    mesh_preview_draw(dis.hwndItem, m.vmesh,
                      {m.bound_center, m.bound_radius, g_panel.yaw, preview_pitch_deg, preview_zoom}, background);
    // Stock code that reads the gr camera between frames finds the first perspective view's again, not the preview's.
    if (EditorViewport* view = get_active_viewport()) view->setup_gr(0);
}

void draw_collapse_toggle(const DRAWITEMSTRUCT& dis)
{
    FillRect(dis.hDC, &dis.rcItem, GetSysColorBrush(COLOR_BTNFACE));
    RECT r = dis.rcItem;
    if (dis.itemState & ODS_SELECTED) OffsetRect(&r, 1, 1);
    if (g_panel.glyph_font) {
        const HGDIOBJ old_font = SelectObject(dis.hDC, g_panel.glyph_font);
        const int old_mode = SetBkMode(dis.hDC, TRANSPARENT);
        const COLORREF old_color = SetTextColor(dis.hDC, GetSysColor(COLOR_BTNTEXT));
        // Marlett: 4 is a right-pointing triangle, 6 a down-pointing one.
        DrawTextA(dis.hDC, g_collapsed ? "4" : "6", 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SetTextColor(dis.hDC, old_color);
        SetBkMode(dis.hDC, old_mode);
        SelectObject(dis.hDC, old_font);
    }
    if ((dis.itemState & ODS_FOCUS) && !(dis.itemState & ODS_NOFOCUSRECT)) {
        RECT focus = dis.rcItem;
        InflateRect(&focus, -1, -1);
        DrawFocusRect(dis.hDC, &focus);
    }
}

// ─── Placement ──────────────────────────────────────────────────────────────

float tab_height(int tab)
{
    const TabSettings& s = g_tab_settings[tab];
    return s.override_height && s.height ? *s.height : tab_info[tab].default_height;
}

DropRule tab_default_rule(int tab)
{
    return {tab_info[tab].default_height, tab_info[tab].height_from_bottom};
}

DropRule tab_rule(int tab)
{
    return {tab_height(tab), tab_info[tab].height_from_bottom};
}

const PlacementMesh& drag_mesh()
{
    return g_drag.tree_item ? g_drag.tree_mesh : g_panel.preview;
}

// The object's origin for a surface point under it.
Vector3 origin_on_surface(const Vector3& hit, const DropRule& rule, const PlacementMesh& mesh)
{
    float offset = rule.height;
    // Upright with any yaw, so the box bottom stays at its object-space height.
    if (rule.from_bottom && mesh.vmesh) offset -= mesh.bbox_min.y;
    return {hit.x, hit.y + offset, hit.z};
}

EditorViewport* perspective_view()
{
    EditorViewport* active = get_active_viewport();
    if (active && active->view_type == editor_view_type_perspective) return active;
    for (int i = 0; i < editor_num_views; i++) {
        auto* view = static_cast<EditorViewport*>(editor_view_at(i));
        if (view && view->view_type == editor_view_type_perspective) return view;
    }
    return nullptr;
}

// Upright, facing the way the perspective camera looks.
Matrix3 upright_orient(const EditorViewport* view)
{
    if (!view || view->view_type != editor_view_type_perspective) view = perspective_view();
    Vector3 forward{0.0f, 0.0f, 1.0f};
    if (view && view->view_data) {
        const Matrix3& camera = view->view_data->camera_orient;
        float x = camera.fvec.x, z = camera.fvec.z;
        if (x * x + z * z < 1e-6f) {
            // Looking straight down: the screen's up is the way it faces.
            x = camera.uvec.x;
            z = camera.uvec.z;
        }
        const float len = std::sqrt(x * x + z * z);
        if (len > 1e-6f) forward = {x / len, 0.0f, z / len};
    }
    return {{forward.z, 0.0f, -forward.x}, {0.0f, 1.0f, 0.0f}, forward};
}

// The perspective camera's own orientation, which a double-click gives every new object.
Matrix3 camera_orient(const EditorViewport* view)
{
    if (!view || view->view_type != editor_view_type_perspective) view = perspective_view();
    return view && view->view_data ? view->view_data->camera_orient : identity_orient;
}

struct SurfaceCast
{
    bool hit = false;
    float t = 0.0f;      // to the nearest upward facing surface
    float wall_t = 0.0f; // to the nearest surface the ray passed
};

// Built level geometry and terrain only; walls and ceilings do not stop the ray.
SurfaceCast cast_surface(CDedLevel& level, const float (&o)[3], const float (&d)[3])
{
    float wall_t = terrain_pick_reach;
    float best = terrain_build_level_ray_hit(level, o, d, terrain_pick_reach, min_up_normal_y, wall_t);
    for (const DedTerrain* t : level.GetAlpineLevelProperties().terrain_objects) {
        if (!t || t->hidden_in_editor || !t->data.grid || t->data.layers.empty()) continue;
        const at::GridView view = terrain_grid_view(t->pos, t->data, *t->data.grid);
        float t_min = 0.0f;
        for (int skips = 0; skips < max_terrain_wall_skips; skips++) {
            float hit = 0.0f;
            if (!at::raycast(view, o, d, t_min, best, hit)) break;
            float n[3];
            at::heightmap_normal(view, o[0] + d[0] * hit, o[2] + d[2] * hit, n);
            if (n[1] >= min_up_normal_y) {
                best = hit;
                break;
            }
            wall_t = std::min(wall_t, hit);
            t_min = hit + 1e-3f;
        }
    }
    return {best < terrain_pick_reach, best, wall_t};
}

// The highest point of the built geometry and the terrains; NaN without either.
float level_top_y(CDedLevel& level)
{
    float top = -INFINITY;
    if (const GSolid* solid = level.solid) {
        for (int i = 0; i < solid->vertices.size; i++) {
            if (const GVertex* v = solid->vertices.data_ptr[i]) top = std::max(top, v->pos.y);
        }
    }
    for (const DedTerrain* t : level.GetAlpineLevelProperties().terrain_objects) {
        if (t && t->data.grid) {
            top = std::max(top, t->pos.y + t->data.height_min + std::max(t->data.height_range, 0.0f));
        }
    }
    return std::isfinite(top) ? top : NAN;
}

// Where releasing over `view` puts the object: perspective views follow the cursor ray, the top view casts
// straight down at the cursor, the front and side views take nothing.
bool drop_transform(void* view, POINT cursor, Vector3& pos, Matrix3& orient)
{
    CDedLevel* level = CDedLevel::Get();
    auto* viewport = static_cast<EditorViewport*>(view);
    if (!level || !viewport || !viewport->view_data) return false;
    const bool perspective = viewport->view_type == editor_view_type_perspective;
    const bool top = !perspective && viewport->view_data->camera_orient.fvec.y < -0.9f;
    if (!perspective && !top) return false;
    const HWND hwnd = editor_view_hwnd(view);
    POINT client = cursor;
    if (!hwnd || !ScreenToClient(hwnd, &client)) return false;
    viewport->setup_gr(0);
    const TerrainRay ray = terrain_screen_ray(static_cast<float>(client.x), static_cast<float>(client.y));
    float o[3] = {ray.o[0], ray.o[1], ray.o[2]};
    float d[3] = {ray.d[0], ray.d[1], ray.d[2]};
    if (top) {
        if (std::isfinite(g_drag.top_y)) o[1] = g_drag.top_y;
        d[0] = 0.0f;
        d[1] = -1.0f;
        d[2] = 0.0f;
    }
    const SurfaceCast cast = cast_surface(*level, o, d);
    if (cast.hit) {
        pos = origin_on_surface({o[0] + d[0] * cast.t, o[1] + d[1] * cast.t, o[2] + d[2] * cast.t}, g_drag.rule,
                                drag_mesh());
    }
    else if (top) {
        return false;
    }
    else {
        const float t = std::min(fallback_distance, std::max(cast.wall_t - wall_clearance, 0.0f));
        pos = {o[0] + d[0] * t, o[1] + d[1] * t, o[2] + d[2] * t};
    }
    const EditorViewport* camera_view = perspective ? viewport : nullptr;
    orient = g_drag.rule.camera_aimed ? camera_orient(camera_view) : upright_orient(camera_view);
    return true;
}

std::string tree_item_text(HWND tree, HTREEITEM item)
{
    char text[260] = {};
    TVITEMA tvi{};
    tvi.mask = TVIF_TEXT;
    tvi.hItem = item;
    tvi.pszText = text;
    tvi.cchTextMax = sizeof(text);
    return SendMessageA(tree, TVM_GETITEMA, 0, reinterpret_cast<LPARAM>(&tvi)) ? text : "";
}

HTREEITEM find_tree_leaf(HWND tree, HTREEITEM parent, const char* name)
{
    for (HTREEITEM item = TreeView_GetChild(tree, parent); item; item = TreeView_GetNextSibling(tree, item)) {
        if (TreeView_GetChild(tree, item)) {
            if (HTREEITEM found = find_tree_leaf(tree, item, name)) return found;
            continue;
        }
        if (string_iequals(tree_item_text(tree, item), name)) return item;
    }
    return nullptr;
}

HTREEITEM find_class_item(HWND tree, const char* root_label, const char* cls)
{
    for (HTREEITEM root = TreeView_GetRoot(tree); root; root = TreeView_GetNextSibling(tree, root)) {
        if (tree_item_text(tree, root) == root_label) return find_tree_leaf(tree, root, cls);
    }
    return nullptr;
}

struct TreeDrag
{
    DropRule rule;
    std::vector<std::string> meshes; // for the ghost and the bounding box bottom, best first
};

// What dragging a tree item places. Not a folder (the double-click handler 0x004431C0 refuses those too) and not
// the Player Start, which that handler moves instead of creating. The stock factory 0x00442A40 picks the type by
// the root's text, the Alpine one by the leaf's; Alpine types are leaves at the root.
std::optional<TreeDrag> tree_drag_info(HWND tree, HTREEITEM item)
{
    if (!item || TreeView_GetChild(tree, item)) return std::nullopt;
    HTREEITEM root = item;
    while (HTREEITEM parent = TreeView_GetParent(tree, root)) root = parent;
    const std::string leaf = tree_item_text(tree, item);
    const std::string type = root == item ? leaf : tree_item_text(tree, root);
    if (leaf == "Player Start") return std::nullopt;
    for (int tab : {tab_clutter, tab_entity, tab_item}) {
        if (type == tab_info[tab].label) {
            // A class folder with no classes in it.
            if (root == item) return std::nullopt;
            return TreeDrag{tab_default_rule(tab), class_meshes(tab, leaf)};
        }
    }
    if (type == "Mesh") {
        return TreeDrag{tab_default_rule(tab_mesh), class_meshes(tab_mesh, mesh_object_default_filename)};
    }
    if (type == "Vehicle Factory") {
        return TreeDrag{tab_default_rule(tab_vehicle), class_meshes(tab_vehicle, vehicle_factory_default_class)};
    }
    if (type == "Multiplayer Respawn Point") return TreeDrag{{respawn_point_height, false}, {}};
    if (type == "Terrain") {
        return TreeDrag{{tree_default_height, false, at::extent(at::default_verts, at::default_cell_size)}, {}};
    }
    const bool camera_aimed = std::any_of(std::begin(camera_aimed_types), std::end(camera_aimed_types),
                                          [&](const char* name) { return string_iequals(type, name); });
    return TreeDrag{{tree_default_height, false, 0.0f, camera_aimed}, {}};
}

// Runs what the move tool runs after moving an object. A new terrain centres on the drop point, as a double-click
// centres it under the camera.
void move_new_object(DedObject& obj, const Vector3& pos, const Matrix3& orient)
{
    obj.pos = pos;
    obj.orient = orient;
    if (obj.type == DedObjectType::DED_TERRAIN) {
        const float half = at::extent(at::default_verts, static_cast<DedTerrain&>(obj).data.cell_size) * 0.5f;
        obj.pos.x -= half;
        obj.pos.z -= half;
        obj.orient = identity_orient;
    }
    if (!ded_object_updaters_safe(obj)) return;
    ded_object_pos_changed(&obj);
    ded_object_orient_changed(&obj);
}

// Through the tree's own double-click handler, so construction (the Alpine types' included), the clutter's
// attached light and undo stay as they are. It puts the object at the camera, and add_object rests an entity on
// the floor below (0x004917A0/0x004169B0), so the final transform goes on afterwards.
bool create_through_tree(CDedLevel& level, HTREEITEM item, const Vector3& pos, const Matrix3& orient)
{
    EditorObjectPanel* panel = g_panel.object_panel;
    if (!panel || !item) return false;
    const UndoEntry* before = undo_stack_top(level.undo_stack);
    // The Alpine types record no creation; their new objects are the ones that join master_objects.
    const VArray<DedObject*>& master = level.master_objects;
    std::vector<DedObject*> existing(master.data_ptr, master.data_ptr + master.size);
    std::sort(existing.begin(), existing.end());
    {
        g_forced_tree_item = item;
        ScopeGuard unforce{[] { g_forced_tree_item = nullptr; }};
        panel->create_object_under_cursor();
    }
    std::vector<DedObject*> created;
    const UndoEntry* entry = undo_stack_top(level.undo_stack);
    if (entry && entry != before) {
        if (entry->type != undo_create_objects) {
            editor_report(EditorReportLevel::warn, "Placement",
                          "The new object was not recorded as a creation and was left at the camera", true);
            return true;
        }
        // The object, then the light 0x00414B10 adds at its transform for a clutter class that has one.
        created.assign(entry->objects.data_ptr, entry->objects.data_ptr + entry->objects.size);
    }
    else {
        for (int i = 0; i < master.size; i++) {
            if (!std::binary_search(existing.begin(), existing.end(), master.data_ptr[i])) {
                created.push_back(master.data_ptr[i]);
            }
        }
    }
    bool moved = false;
    for (DedObject* obj : created) {
        if (!obj) continue;
        move_new_object(*obj, pos, orient);
        moved = true;
    }
    return moved;
}

bool place_stock(CDedLevel& level, const std::string& cls, const Vector3& pos, const Matrix3& orient)
{
    EditorObjectPanel* panel = g_panel.object_panel;
    const HWND tree = panel ? WndToHandle(&panel->tree) : nullptr;
    const HTREEITEM item = tree ? find_class_item(tree, tab_info[g_tab].label, cls.c_str()) : nullptr;
    if (!item) {
        editor_report(EditorReportLevel::warn, "Placement", "Class '" + cls + "' is not in the object tree", true);
        return false;
    }
    return create_through_tree(level, item, pos, orient);
}

void refresh_class_combo();

bool place(const Vector3& pos, const Matrix3& orient)
{
    CDedLevel* level = CDedLevel::Get();
    const std::string cls = g_tab_settings[g_tab].cls;
    if (!level || cls.empty()) return false;
    bool placed = false;
    if (is_stock_tab(g_tab)) {
        placed = place_stock(*level, cls, pos, orient);
    }
    else if (g_tab == tab_vehicle) {
        placed = PlaceNewVehicleFactoryObject(cls, pos, orient) != nullptr;
    }
    else {
        placed = PlaceNewMeshObject(cls.c_str(), pos, orient) != nullptr;
        if (placed) {
            remember_mesh(cls);
            refresh_class_combo();
        }
    }
    if (placed) redraw_all_viewports();
    return placed;
}

void place_tree_item(HTREEITEM item, const Vector3& pos, const Matrix3& orient)
{
    CDedLevel* level = CDedLevel::Get();
    if (level && create_through_tree(*level, item, pos, orient)) redraw_all_viewports();
}

// Under the active viewport's camera, as the tree places, then down onto the floor below it.
void place_at_camera()
{
    CDedLevel* level = CDedLevel::Get();
    const EditorViewport* viewport = get_active_viewport();
    if (!level || !viewport || !viewport->view_data) return;
    const Vector3 camera = viewport->view_data->camera_pos;
    const float o[3] = {camera.x, camera.y, camera.z};
    const float d[3] = {0.0f, -1.0f, 0.0f};
    const SurfaceCast cast = cast_surface(*level, o, d);
    const Vector3 hit{camera.x, camera.y - cast.t, camera.z};
    const Vector3 pos = cast.hit ? origin_on_surface(hit, tab_rule(g_tab), g_panel.preview) : camera;
    place(pos, upright_orient(viewport));
}

// ─── Drag ───────────────────────────────────────────────────────────────────

void drag_repaint_views(void* view, void* old_view)
{
    const ULONGLONG now = GetTickCount64();
    if (now - g_drag.others_repainted >= other_views_repaint_ms) {
        editor_views_mark_repaint_all();
        g_drag.others_repainted = now;
        g_drag.others_pending = false;
        return;
    }
    editor_view_mark_repaint(view);
    editor_view_mark_repaint(old_view);
    g_drag.others_pending = true;
}

void drag_update()
{
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    void* view = editor_view_under_cursor(cursor);
    Vector3 pos;
    Matrix3 orient;
    const bool valid = view && drop_transform(view, cursor, pos, orient);
    void* old_view = g_drag.view;
    g_drag.view = view;
    g_drag.valid = valid;
    if (valid) {
        g_drag.pos = pos;
        g_drag.orient = orient;
    }
    SetCursor(LoadCursorA(nullptr, valid ? IDC_CROSS : IDC_NO));
    drag_repaint_views(view, old_view);
}

void update_timer();

void drag_reset()
{
    const bool shown = g_drag.active;
    free_mesh(g_drag.tree_mesh);
    g_drag = DragState{};
    if (shown) editor_views_mark_repaint_all();
    update_timer();
}

bool dragging_from(HWND hwnd)
{
    return g_drag.pressed && g_drag.capture == hwnd;
}

void drag_cancel()
{
    const HWND capture = g_drag.capture;
    const bool captured = capture && GetCapture() == capture;
    // Reset first: the WM_CAPTURECHANGED ReleaseCapture sends finds nothing left to cancel.
    drag_reset();
    if (captured) ReleaseCapture();
}

void drag_activate(CDedLevel& level)
{
    g_drag.active = true;
    const float top = level_top_y(level);
    g_drag.top_y = std::isfinite(top) ? top + 1.0f : NAN;
}

void drag_press(HWND preview, POINT pt)
{
    drag_cancel();
    if (!CDedLevel::Get()) return;
    g_drag.pressed = true;
    g_drag.press = pt;
    g_drag.capture = preview;
    g_drag.rule = tab_rule(g_tab);
    SetCapture(preview);
    update_timer();
}

// The press has already moved past the drag threshold, so the drag starts active.
void tree_drag_begin(HWND tree, HTREEITEM item)
{
    std::optional<TreeDrag> info = tree_drag_info(tree, item);
    const HWND form = g_panel.form;
    if (!info || !form) return;
    drag_cancel();
    CDedLevel* level = CDedLevel::Get();
    if (!level) return;
    // Loads before the drag counts as pressed, so a throw leaves no drag without capture.
    load_mesh(g_drag.tree_mesh, info->meshes);
    SetCapture(form);
    g_drag.pressed = true;
    g_drag.capture = form;
    g_drag.tree_item = item;
    g_drag.rule = info->rule;
    update_timer();
    drag_activate(*level);
    drag_update();
}

void drag_move(POINT pt)
{
    if (!g_drag.active) {
        if (std::abs(pt.x - g_drag.press.x) <= GetSystemMetrics(SM_CXDRAG) &&
            std::abs(pt.y - g_drag.press.y) <= GetSystemMetrics(SM_CYDRAG)) {
            return;
        }
        CDedLevel* level = CDedLevel::Get();
        if (!level) {
            drag_cancel();
            return;
        }
        drag_activate(*level);
    }
    drag_update();
}

void drag_release()
{
    const bool drop = g_drag.active && g_drag.valid;
    const HTREEITEM tree_item = g_drag.tree_item;
    const Vector3 pos = g_drag.pos;
    const Matrix3 orient = g_drag.orient;
    drag_cancel();
    if (!drop) return;
    if (tree_item) {
        place_tree_item(tree_item, pos, orient);
    }
    else {
        place(pos, orient);
    }
}

// ─── Controls ───────────────────────────────────────────────────────────────

// The full list, with the current class shown.
void refresh_class_combo()
{
    if (!g_panel.hwnd) return;
    g_panel.classes = tab_classes(g_tab);
    HWND combo = GetDlgItem(g_panel.hwnd, IDC_PLACEMENT_CLASS);
    g_panel.updating = true;
    SendMessageA(combo, CB_RESETCONTENT, 0, 0);
    for (const std::string& name : g_panel.classes) {
        SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    }
    const std::string& cls = g_tab_settings[g_tab].cls;
    const LRESULT index = cls.empty() ? CB_ERR
                                      : SendMessageA(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                                     reinterpret_cast<LPARAM>(cls.c_str()));
    SendMessageA(combo, CB_SETCURSEL, index == CB_ERR ? static_cast<WPARAM>(-1) : static_cast<WPARAM>(index), 0);
    SetWindowTextA(combo, cls.c_str());
    // The combo's edit is ES_NOHIDESEL, so the select-all from setting its text would stay highlighted.
    SendMessageA(combo, CB_SETEDITSEL, 0, MAKELPARAM(-1, 0));
    g_panel.updating = false;
}

// Typing narrows the list to the classes holding the text, keeping what was typed.
void filter_class_combo()
{
    HWND combo = GetDlgItem(g_panel.hwnd, IDC_PLACEMENT_CLASS);
    char text[128] = {};
    GetWindowTextA(combo, text, sizeof(text));
    const DWORD sel = static_cast<DWORD>(SendMessageA(combo, CB_GETEDITSEL, 0, 0));
    g_panel.updating = true;
    SendMessageA(combo, CB_RESETCONTENT, 0, 0);
    for (const std::string& name : g_panel.classes) {
        if (string_icontains(name, text)) {
            SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        }
    }
    // Dropping the list selects its closest item into the edit, so the text goes back after.
    if (!SendMessageA(combo, CB_GETDROPPEDSTATE, 0, 0)) {
        SendMessageA(combo, CB_SHOWDROPDOWN, TRUE, 0);
        SetCursor(LoadCursorA(nullptr, IDC_ARROW));
    }
    SetWindowTextA(combo, text);
    SendMessageA(combo, CB_SETEDITSEL, 0, MAKELPARAM(LOWORD(sel), HIWORD(sel)));
    g_panel.updating = false;
}

// The text of list item `index`; empty for none or one too long for a class name.
std::string combo_item_text(HWND combo, LRESULT index)
{
    char item[128] = {};
    if (index == CB_ERR ||
        SendMessageA(combo, CB_GETLBTEXTLEN, static_cast<WPARAM>(index), 0) >= static_cast<LRESULT>(sizeof(item))) {
        return {};
    }
    SendMessageA(combo, CB_GETLBTEXT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(item));
    return item;
}

void set_class(const std::string& name)
{
    g_tab_settings[g_tab].cls = name;
    if (g_tab == tab_mesh) remember_mesh(name);
    refresh_class_combo();
    load_preview();
}

std::string class_box_text()
{
    char text[128] = {};
    GetWindowTextA(GetDlgItem(g_panel.hwnd, IDC_PLACEMENT_CLASS), text, sizeof(text));
    return text;
}

// Enter in the class box: an exact class name (any case) selects it, anything else is put back.
void commit_typed_class(std::string_view text)
{
    const std::string typed{trim(text)};
    if (g_tab == tab_mesh) {
        if (mesh_name_usable(typed)) {
            set_class(typed);
            return;
        }
    }
    else {
        auto it = std::find_if(g_panel.classes.begin(), g_panel.classes.end(),
                               [&](const std::string& name) { return string_iequals(name, typed); });
        if (it != g_panel.classes.end()) {
            set_class(*it);
            return;
        }
    }
    refresh_class_combo();
}

void update_height_controls()
{
    const HWND hdlg = g_panel.hwnd;
    const TabSettings& s = g_tab_settings[g_tab];
    g_panel.updating = true;
    CheckDlgButton(hdlg, IDC_PLACEMENT_HEIGHT_OVERRIDE, s.override_height ? BST_CHECKED : BST_UNCHECKED);
    alpine_dlg_set_float_field(hdlg, IDC_PLACEMENT_HEIGHT, tab_height(g_tab));
    EnableWindow(GetDlgItem(hdlg, IDC_PLACEMENT_HEIGHT), s.override_height);
    g_panel.updating = false;
}

void read_height_field()
{
    TabSettings& s = g_tab_settings[g_tab];
    if (g_panel.updating || !s.override_height) return;
    char text[32] = {};
    GetDlgItemTextA(g_panel.hwnd, IDC_PLACEMENT_HEIGHT, text, sizeof(text));
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    if (end != text && std::isfinite(value)) s.height = value;
}

// The widest padding at which every tab still fits the strip; narrower strips keep the scroll arrows.
void fit_tab_padding(HWND tabs, int strip_width)
{
    PanelState& p = g_panel;
    if (strip_width == p.tab_fit_width) return;
    p.tab_fit_width = strip_width;
    for (int pad = max_tab_padding; pad >= min_tab_padding; pad--) {
        SendMessageA(tabs, TCM_SETPADDING, 0, MAKELPARAM(pad, tab_padding_y));
        // Setting the padding alone need not measure the tabs again; changing one of them does.
        TCITEMA item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<char*>(tab_info[0].label);
        SendMessageA(tabs, TCM_SETITEMA, 0, reinterpret_cast<LPARAM>(&item));
        RECT first{};
        RECT last{};
        if (!SendMessageA(tabs, TCM_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&first)) ||
            !SendMessageA(tabs, TCM_GETITEMRECT, tab_count - 1, reinterpret_cast<LPARAM>(&last))) {
            break;
        }
        if (last.right - first.left <= strip_width - tab_strip_slack) break;
    }
    InvalidateRect(tabs, nullptr, TRUE);
}

// Lays the panel out across `width` pixels and returns its height.
int panel_layout(int width)
{
    const PanelState& p = g_panel;
    const int dx = std::max(width - p.template_width, 0);
    const UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    auto rect = [&](int id) { return p.template_rects[layout_index(id)]; };

    HWND tabs = GetDlgItem(p.hwnd, IDC_PLACEMENT_TABS);
    RECT r = rect(IDC_PLACEMENT_TABS);
    SetWindowPos(tabs, nullptr, r.left, r.top, r.right - r.left + dx, r.bottom - r.top, flags);
    fit_tab_padding(tabs, r.right - r.left + dx);

    const RECT browse = rect(IDC_PLACEMENT_BROWSE);
    r = rect(IDC_PLACEMENT_CLASS);
    const int combo_right = g_tab == tab_mesh ? browse.left + dx - 2 : r.right + dx;
    HWND combo = GetDlgItem(p.hwnd, IDC_PLACEMENT_CLASS);
    // Resizing a combo selects all of its edit text.
    const DWORD sel = static_cast<DWORD>(SendMessageA(combo, CB_GETEDITSEL, 0, 0));
    SetWindowPos(combo, nullptr, r.left, r.top, combo_right - r.left, p.combo_drop_height, flags);
    SendMessageA(combo, CB_SETEDITSEL, 0, MAKELPARAM(LOWORD(sel), HIWORD(sel)));
    SetWindowPos(GetDlgItem(p.hwnd, IDC_PLACEMENT_BROWSE), nullptr, browse.left + dx, browse.top, 0, 0,
                 flags | SWP_NOSIZE);
    r = rect(IDC_PLACEMENT_PREVIEW);
    SetWindowPos(GetDlgItem(p.hwnd, IDC_PLACEMENT_PREVIEW), nullptr, r.left, r.top, r.right - r.left + dx,
                 r.bottom - r.top, flags);
    for (int id : {IDC_PLACEMENT_HEIGHT_LABEL, IDC_PLACEMENT_HEIGHT, IDC_PLACEMENT_HEIGHT_OVERRIDE}) {
        r = rect(id);
        SetWindowPos(GetDlgItem(p.hwnd, id), nullptr, r.left, r.top, 0, 0, flags | SWP_NOSIZE);
    }
    if (g_collapsed) {
        // The header row alone, with the toggle's margins.
        const RECT toggle = rect(IDC_PLACEMENT_COLLAPSE);
        return toggle.bottom + toggle.top;
    }
    return p.template_height;
}

// The stock WM_SIZE (0x00444430) stretches the tree over the whole form; the panel takes the top of it.
void form_layout()
{
    const PanelState& p = g_panel;
    if (!p.form || !p.hwnd) return;
    RECT rc{};
    GetClientRect(p.form, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int panel_height = panel_layout(width);
    // No bit copies: after the stock stretch they would carry the tree's pixels from over the panel back down.
    const UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS;
    SetWindowPos(p.hwnd, nullptr, 0, 0, width, panel_height, flags);
    if (HWND tree = GetDlgItem(p.form, editor_object_tree_id)) {
        SetWindowPos(tree, nullptr, 0, panel_height, width, std::max(height - panel_height, 0), flags);
    }
}

// The preview spins and a drag repaints the other views on the timer; a collapsed panel keeps it only for a drag.
void update_timer()
{
    const HWND hdlg = g_panel.hwnd;
    if (!hdlg) return;
    if (!g_collapsed || g_drag.pressed) {
        SetTimer(hdlg, tick_timer_id, tick_ms, nullptr);
    }
    else {
        KillTimer(hdlg, tick_timer_id);
    }
}

void show_panel_controls()
{
    const HWND hdlg = g_panel.hwnd;
    const int content = g_collapsed ? SW_HIDE : SW_SHOW;
    for (int id : {IDC_PLACEMENT_TABS, IDC_PLACEMENT_CLASS, IDC_PLACEMENT_PREVIEW, IDC_PLACEMENT_HEIGHT_LABEL,
                   IDC_PLACEMENT_HEIGHT, IDC_PLACEMENT_HEIGHT_OVERRIDE}) {
        ShowWindow(GetDlgItem(hdlg, id), content);
    }
    ShowWindow(GetDlgItem(hdlg, IDC_PLACEMENT_BROWSE), !g_collapsed && g_tab == tab_mesh ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hdlg, IDC_PLACEMENT_TITLE), g_collapsed ? SW_SHOW : SW_HIDE);
}

void set_collapsed(bool collapsed)
{
    if (collapsed == g_collapsed) return;
    if (dragging_from(preview_hwnd())) drag_cancel();
    g_collapsed = collapsed;
    show_panel_controls();
    form_layout();
    InvalidateRect(GetDlgItem(g_panel.hwnd, IDC_PLACEMENT_COLLAPSE), nullptr, FALSE);
    update_timer();
}

void select_tab(int tab)
{
    g_tab = std::clamp(tab, 0, tab_count - 1);
    std::string& cls = g_tab_settings[g_tab].cls;
    if (g_tab == tab_mesh && cls.empty()) {
        cls = mesh_object_default_filename;
        remember_mesh(cls);
    }
    std::vector<std::string> classes = tab_classes(g_tab);
    if (g_tab != tab_mesh && !classes.empty() &&
        std::none_of(classes.begin(), classes.end(), [&](const std::string& name) { return name == cls; })) {
        cls = classes.front();
    }
    show_panel_controls();
    refresh_class_combo();
    update_height_controls();
    form_layout();
    load_preview();
}

void browse_mesh()
{
    std::string name = g_tab_settings[tab_mesh].cls;
    if (alpine_browse_mesh(g_panel.hwnd, name, ALPINE_MESH_ANY) && mesh_name_fits(name)) {
        set_class(name);
    }
}

void tick()
{
    PanelState& p = g_panel;
    const ULONGLONG now = GetTickCount64();
    const float dt = p.last_tick ? std::min(static_cast<float>(now - p.last_tick) / 1000.0f, 0.2f) : 0.0f;
    p.last_tick = now;
    if (g_drag.others_pending && now - g_drag.others_repainted >= other_views_repaint_ms) {
        editor_views_mark_repaint_all();
        g_drag.others_repainted = now;
        g_drag.others_pending = false;
    }
    // No gr work in the middle of a click or drag anywhere: the views and the tree are busy with it.
    const bool button_held = ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) |
                               GetAsyncKeyState(VK_MBUTTON)) & 0x8000) != 0;
    EditorVMesh* vmesh = p.preview.vmesh;
    const HWND preview = preview_hwnd();
    if (g_collapsed || button_held || !vmesh || !preview || !preview_shown(preview) || !preview_can_draw()) return;
    p.yaw = std::fmod(p.yaw + spin_deg_per_s * dt, 360.0f);
    if (vmesh_get_type(vmesh) == VMESH_TYPE_ANIM_FX) {
        const Vector3 origin{};
        vmesh_process(vmesh, dt, 0, &origin, &identity_orient, 1);
    }
    InvalidateRect(preview, nullptr, FALSE);
}

void init_panel(HWND hdlg)
{
    PanelState& p = g_panel;
    p.hwnd = hdlg;
    RECT client{};
    GetClientRect(hdlg, &client);
    p.template_width = client.right;
    p.template_height = client.bottom;
    for (std::size_t i = 0; i < layout_count; i++) {
        RECT rc{};
        if (HWND ctl = GetDlgItem(hdlg, layout_ids[i])) {
            GetWindowRect(ctl, &rc);
            MapWindowPoints(nullptr, hdlg, reinterpret_cast<POINT*>(&rc), 2);
        }
        p.template_rects[i] = rc;
    }
    RECT drop{0, 0, 0, combo_drop_dlu};
    MapDialogRect(hdlg, &drop);
    p.combo_drop_height = drop.bottom;
    p.tab_fit_width = -1;

    const RECT toggle = p.template_rects[layout_index(IDC_PLACEMENT_COLLAPSE)];
    const int glyph_height = std::min(toggle.right - toggle.left, toggle.bottom - toggle.top);
    p.glyph_font = CreateFontA(glyph_height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, SYMBOL_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, "Marlett");

    HWND tabs = GetDlgItem(hdlg, IDC_PLACEMENT_TABS);
    // Single row: the default minimum tab width alone would push the fifth tab past the default pane width.
    SendMessageA(tabs, TCM_SETMINTABWIDTH, 0, 0);
    SendMessageA(tabs, TCM_SETPADDING, 0, MAKELPARAM(min_tab_padding, tab_padding_y));
    for (int i = 0; i < tab_count; i++) {
        TCITEMA item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<char*>(tab_info[i].label);
        SendMessageA(tabs, TCM_INSERTITEMA, i, reinterpret_cast<LPARAM>(&item));
    }
    SendMessageA(tabs, TCM_SETCURSEL, g_tab, 0);
    SendDlgItemMessageA(hdlg, IDC_PLACEMENT_CLASS, CB_LIMITTEXT, rfl_mesh_name_max_len, 0);
    SendDlgItemMessageA(hdlg, IDC_PLACEMENT_HEIGHT, EM_SETLIMITTEXT, 16, 0);
    update_timer();
}

void destroy_panel(HWND hdlg)
{
    PanelState& p = g_panel;
    drag_cancel();
    KillTimer(hdlg, tick_timer_id);
    free_mesh(p.preview);
    if (p.msg_hook) {
        UnhookWindowsHookEx(p.msg_hook);
        p.msg_hook = nullptr;
    }
    if (p.glyph_font) {
        DeleteObject(p.glyph_font);
        p.glyph_font = nullptr;
    }
    p.classes.clear();
    p.hwnd = nullptr;
}

void on_command(WORD id, WORD code)
{
    switch (id) {
    case IDC_PLACEMENT_CLASS:
        if (g_panel.updating) break;
        if (code == CBN_EDITCHANGE && g_tab != tab_mesh) {
            filter_class_combo();
        }
        else if (code == CBN_SELENDOK) {
            HWND combo = GetDlgItem(g_panel.hwnd, IDC_PLACEMENT_CLASS);
            const std::string item = combo_item_text(combo, SendMessageA(combo, CB_GETCURSEL, 0, 0));
            if (!item.empty()) {
                g_tab_settings[g_tab].cls = item;
                load_preview();
            }
        }
        else if (code == CBN_KILLFOCUS) {
            refresh_class_combo();
        }
        break;
    case IDOK:
        if (IsChild(GetDlgItem(g_panel.hwnd, IDC_PLACEMENT_CLASS), GetFocus())) commit_typed_class(class_box_text());
        break;
    case IDCANCEL:
        if (g_drag.pressed) drag_cancel();
        break;
    case IDC_PLACEMENT_BROWSE:
        if (code == BN_CLICKED) browse_mesh();
        break;
    case IDC_PLACEMENT_COLLAPSE:
        // An owner-drawn button reports a quick second click as a double-click.
        if (code == BN_CLICKED || code == BN_DOUBLECLICKED) set_collapsed(!g_collapsed);
        break;
    case IDC_PLACEMENT_TITLE:
        if (code == STN_CLICKED) set_collapsed(false);
        break;
    case IDC_PLACEMENT_HEIGHT_OVERRIDE:
        if (code == BN_CLICKED && !g_panel.updating) {
            TabSettings& s = g_tab_settings[g_tab];
            s.override_height = IsDlgButtonChecked(g_panel.hwnd, IDC_PLACEMENT_HEIGHT_OVERRIDE) == BST_CHECKED;
            if (s.override_height && !s.height) s.height = tab_info[g_tab].default_height;
            update_height_controls();
        }
        break;
    case IDC_PLACEMENT_HEIGHT:
        if (code == EN_CHANGE) read_height_field();
        break;
    default:
        break;
    }
}

INT_PTR panel_message(HWND hdlg, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_INITDIALOG:
        init_panel(hdlg);
        return FALSE;
    case WM_TIMER:
        if (wparam == tick_timer_id) {
            tick();
            return TRUE;
        }
        break;
    case WM_DRAWITEM: {
        const auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (dis && dis->CtlID == IDC_PLACEMENT_PREVIEW) {
            draw_preview(*dis);
            return TRUE;
        }
        if (dis && dis->CtlID == IDC_PLACEMENT_COLLAPSE) {
            draw_collapse_toggle(*dis);
            return TRUE;
        }
        break;
    }
    case WM_NOTIFY: {
        const auto* nm = reinterpret_cast<const NMHDR*>(lparam);
        if (nm && nm->idFrom == IDC_PLACEMENT_TABS && nm->code == TCN_SELCHANGE) {
            drag_cancel();
            select_tab(static_cast<int>(SendMessageA(nm->hwndFrom, TCM_GETCURSEL, 0, 0)));
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        on_command(LOWORD(wparam), HIWORD(wparam));
        return TRUE;
    case WM_DESTROY:
        destroy_panel(hdlg);
        break;
    default:
        break;
    }
    return FALSE;
}

// Nothing the panel allocates may unwind into USER32's frames.
INT_PTR CALLBACK PlacementPanelProc(HWND hdlg, UINT msg, WPARAM wparam, LPARAM lparam)
{
    try {
        return panel_message(hdlg, msg, wparam, lparam);
    }
    catch (...) {
        xlog::error("[Placement] panel message {:#x} failed", msg);
        return FALSE;
    }
}

LRESULT preview_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        if (!g_drag.active) {
            SetCursor(LoadCursorA(nullptr, IDC_SIZEALL));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        drag_press(hwnd, pt);
        return 0;
    case WM_MOUSEMOVE:
        if (dragging_from(hwnd)) {
            drag_move(pt);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (dragging_from(hwnd)) {
            drag_release();
            return 0;
        }
        break;
    case WM_LBUTTONDBLCLK:
        drag_cancel();
        place_at_camera();
        return 0;
    case WM_RBUTTONDOWN:
        if (dragging_from(hwnd)) {
            drag_cancel();
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (dragging_from(hwnd)) drag_reset();
        break;
    case WM_NCDESTROY:
        if (g_panel.preview_proc) {
            SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_panel.preview_proc));
        }
        break;
    default:
        break;
    }
    return CallWindowProcA(g_panel.preview_proc, hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    try {
        return preview_message(hwnd, msg, wparam, lparam);
    }
    catch (...) {
        xlog::error("[Placement] preview message {:#x} failed", msg);
        return 0;
    }
}

// A tree drag holds the form's capture.
bool form_drag_message(HWND hwnd, UINT msg, LPARAM lparam)
{
    if (!dragging_from(hwnd)) return false;
    switch (msg) {
    case WM_MOUSEMOVE:
        drag_move({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        return true;
    case WM_LBUTTONUP:
        drag_release();
        return true;
    case WM_RBUTTONDOWN:
        drag_cancel();
        return true;
    case WM_CAPTURECHANGED:
        drag_reset();
        return false;
    default:
        return false;
    }
}

LRESULT form_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    const WNDPROC orig = g_panel.form_proc;
    if (msg == WM_NCDESTROY) {
        // Put back before MFC's own WM_NCDESTROY, which unsubclasses only while its proc is the current one.
        SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig));
        g_panel.form = nullptr;
        g_panel.form_proc = nullptr;
        g_panel.object_panel = nullptr;
        return CallWindowProcA(orig, hwnd, msg, wparam, lparam);
    }
    if (form_drag_message(hwnd, msg, lparam)) return 0;
    const LRESULT result = CallWindowProcA(orig, hwnd, msg, wparam, lparam);
    if (msg == WM_SIZE) form_layout();
    return result;
}

LRESULT CALLBACK FormProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    try {
        return form_message(hwnd, msg, wparam, lparam);
    }
    catch (...) {
        xlog::error("[Placement] object panel message {:#x} failed", msg);
        return 0;
    }
}

// Watches the tree's own clicks for a press on an item that then moves past the drag threshold; every message
// still reaches the tree unchanged. Not the tree's own drag detection: with it enabled, the double-click that
// places objects stopped working.
void tree_watch_drag(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    PanelState& p = g_panel;
    const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    switch (msg) {
    case WM_LBUTTONDOWN: {
        TVHITTESTINFO hit{};
        hit.pt = pt;
        const HTREEITEM item = TreeView_HitTest(hwnd, &hit);
        // The tree's click tracking may already have swallowed the button-up.
        const bool held = GetKeyState(VK_LBUTTON) < 0;
        p.tree_press_item = held && item && (hit.flags & TVHT_ONITEM) ? item : nullptr;
        p.tree_press = pt;
        break;
    }
    case WM_MOUSEMOVE: {
        const HTREEITEM item = p.tree_press_item;
        if (!item) break;
        if (!(wparam & MK_LBUTTON)) {
            p.tree_press_item = nullptr;
            break;
        }
        if (std::abs(pt.x - p.tree_press.x) <= GetSystemMetrics(SM_CXDRAG) &&
            std::abs(pt.y - p.tree_press.y) <= GetSystemMetrics(SM_CYDRAG)) {
            break;
        }
        p.tree_press_item = nullptr;
        tree_drag_begin(hwnd, item);
        break;
    }
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_CAPTURECHANGED:
        p.tree_press_item = nullptr;
        break;
    default:
        break;
    }
}

LRESULT CALLBACK TreeProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    const WNDPROC orig = g_panel.tree_proc;
    if (msg == WM_NCDESTROY) {
        // Put back only while ours is current: MFC may have subclassed the tree on top of it.
        if (reinterpret_cast<WNDPROC>(GetWindowLongPtrA(hwnd, GWLP_WNDPROC)) == TreeProc) {
            SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig));
        }
        g_panel.tree_proc = nullptr;
        g_panel.tree_press_item = nullptr;
        return CallWindowProcA(orig, hwnd, msg, wparam, lparam);
    }
    const LRESULT result = CallWindowProcA(orig, hwnd, msg, wparam, lparam);
    try {
        tree_watch_drag(hwnd, msg, wparam, lparam);
    }
    catch (...) {
        xlog::error("[Placement] tree drag from message {:#x} failed", msg);
    }
    return result;
}

// Whether keys going to `hwnd` are text typed into the class box or the height field.
bool panel_takes_keys(HWND hwnd)
{
    return hwnd && (IsChild(GetDlgItem(g_panel.hwnd, IDC_PLACEMENT_CLASS), hwnd) ||
                    hwnd == GetDlgItem(g_panel.hwnd, IDC_PLACEMENT_HEIGHT));
}

// With the filtered list down, Enter in the class box goes to the combo, which closes the list and, with no item in it
// picked, clears the text. So the typed name is committed here first.
bool commit_from_open_list(const MSG& msg)
{
    const HWND combo = GetDlgItem(g_panel.hwnd, IDC_PLACEMENT_CLASS);
    if (msg.message != WM_KEYDOWN || msg.wParam != VK_RETURN || !IsChild(combo, msg.hwnd) ||
        !SendMessageA(combo, CB_GETDROPPEDSTATE, 0, 0)) {
        return false;
    }
    const std::string text = class_box_text();
    // An item picked with the arrows is written into the box. Wine also highlights a mere prefix match.
    const LRESULT sel = SendMessageA(combo, CB_GETCURSEL, 0, 0);
    if (sel != CB_ERR && string_iequals(combo_item_text(combo, sel), trim(text))) return false;
    SendMessageA(combo, CB_SHOWDROPDOWN, FALSE, 0);
    commit_typed_class(text);
    return true;
}

// The form view's PreTranslateMessage hands keys to the frame's accelerators first, so Ctrl+Z or Ctrl+V
// typed in the panel's text fields would act on the level. Those fields take their keys before they get
// there; everywhere else in the panel the accelerators stay live.
bool panel_key(MSG& msg)
{
    if (msg.message < WM_KEYFIRST || msg.message > WM_KEYLAST) return false;
    // Whichever window has the focus; with none, keys reach the active window as WM_SYSKEYDOWN.
    if ((msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) && msg.wParam == VK_ESCAPE && g_drag.pressed) {
        drag_cancel();
        return true;
    }
    return panel_takes_keys(msg.hwnd) && (commit_from_open_list(msg) || IsDialogMessageA(g_panel.hwnd, &msg));
}

LRESULT CALLBACK panel_msg_hook(int code, WPARAM wparam, LPARAM lparam)
{
    if (code == HC_ACTION && wparam == PM_REMOVE && g_panel.hwnd) {
        auto* msg = reinterpret_cast<MSG*>(lparam);
        bool taken = false;
        try {
            taken = panel_key(*msg);
        }
        catch (...) {
            xlog::error("[Placement] key message {:#x} failed", msg->message);
        }
        if (taken) {
            msg->message = WM_NULL;
            msg->wParam = 0;
            msg->lParam = 0;
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

HTREEITEM __fastcall tree_dblclk_hit_test(void* tree, void* edx, int x, int y, UINT* flags);
CallHook<HTREEITEM __fastcall(void*, void*, int, int, UINT*)> tree_dblclk_hit_test_hook{
    0x0044322E,
    tree_dblclk_hit_test,
};
HTREEITEM __fastcall tree_dblclk_hit_test(void* tree, void* edx, int x, int y, UINT* flags)
{
    if (g_forced_tree_item) {
        if (flags) *flags = TVHT_ONITEMLABEL;
        return g_forced_tree_item;
    }
    // 0x004431C0 takes the cursor into the form's client space (0x00442230 on the form), which stock shares with
    // the tree at (0,0); the panel moves the tree down.
    if (HWND tree_hwnd = g_panel.form ? GetDlgItem(g_panel.form, editor_object_tree_id) : nullptr) {
        POINT pt{x, y};
        MapWindowPoints(g_panel.form, tree_hwnd, &pt, 1);
        x = pt.x;
        y = pt.y;
    }
    return tree_dblclk_hit_test_hook.call_target(tree, edx, x, y, flags);
}

} // namespace

void ApplyPlacementPanelPatches()
{
    tree_dblclk_hit_test_hook.install();
}

void placement_panel_create(EditorObjectPanel* object_panel, HWND form)
{
    if (!object_panel || !form || g_panel.hwnd) return;
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TAB_CLASSES};
    InitCommonControlsEx(&icc);
    g_panel.object_panel = object_panel;
    g_panel.form = form;
    try {
        const HWND hwnd = CreateDialogParamA(reinterpret_cast<HINSTANCE>(&__ImageBase),
                                             MAKEINTRESOURCEA(IDD_ALPINE_PLACEMENT_PANEL), form, PlacementPanelProc, 0);
        if (!hwnd) {
            xlog::error("[Placement] the panel could not be created ({})", GetLastError());
            g_panel.object_panel = nullptr;
            g_panel.form = nullptr;
            return;
        }
        if (HWND preview = preview_hwnd()) {
            g_panel.preview_proc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrA(preview, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(PreviewProc)));
        }
        g_panel.form_proc =
            reinterpret_cast<WNDPROC>(SetWindowLongPtrA(form, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(FormProc)));
        g_panel.msg_hook = SetWindowsHookExA(WH_GETMESSAGE, panel_msg_hook, nullptr, GetCurrentThreadId());
        if (HWND tree = GetDlgItem(form, editor_object_tree_id)) {
            g_panel.tree_proc =
                reinterpret_cast<WNDPROC>(SetWindowLongPtrA(tree, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(TreeProc)));
        }
        select_tab(g_tab);
    }
    catch (...) {
        xlog::error("[Placement] out of memory building the panel");
    }
}

void placement_panel_render_ghost()
{
    if (!g_drag.active || !g_drag.valid) return;
    const PlacementMesh& m = drag_mesh();
    const Vector3& pos = g_drag.pos;
    const Matrix3& orient = g_drag.orient;
    if (g_drag.rule.footprint > 0.0f) {
        const Vector3 dims{g_drag.rule.footprint, 0.0f, g_drag.rule.footprint};
        set_draw_color(0xff, 0xff, 0x00, 0xff);
        draw_wireframe_box_3d(&pos, &identity_orient, &dims, editor_line_mode());
        return;
    }
    Vector3 lo{-marker_half_size, -marker_half_size, -marker_half_size};
    Vector3 hi{marker_half_size, marker_half_size, marker_half_size};
    if (EditorVMesh* vm = m.vmesh) {
        set_draw_color(0xff, 0xff, 0xff, 0xff);
        EditorRenderParams params = editor_mesh_render_params();
        const bool anim_fx = vmesh_get_type(vm) == VMESH_TYPE_ANIM_FX;
        if (anim_fx) vfx_render_transparent = 1;
        const Vector3 bound_center = pos + orient * m.bound_center;
        room_setup(nullptr, &bound_center, m.bound_radius, 1, 1);
        vmesh_render(vm, &pos, &orient, &params);
        room_cleanup();
        if (anim_fx) vfx_render_transparent = 0;
        lo = m.bbox_min;
        hi = m.bbox_max;
    }
    const Vector3 center = pos + orient * Vector3{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
    const Vector3 dims{hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};
    set_draw_color(0xff, 0xff, 0x00, 0xff);
    draw_wireframe_box_3d(&center, &orient, &dims, editor_line_mode());
}

bool placement_panel_dragging()
{
    return g_drag.pressed;
}
