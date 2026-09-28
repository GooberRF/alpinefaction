#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <format>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include <patch_common/CallHook.h>
#include <patch_common/MemUtils.h>
#include <common/terrain/alpine_terrain.h>
#include <xlog/xlog.h>
#include "alpine_obj.h"
#include "alpine_spinner.h"
#include "level.h"
#include "mfc_types.h"
#include "resources.h"
#include "terrain.h"
#include "terrain_build.h"
#include "terrain_decorations.h"
#include "terrain_paint.h"
#include "terrain_paint_math.h"
#include "terrain_preview.h"
#include "vtypes.h"

namespace at = alpine_terrain;
namespace tp = terrain_paint;

namespace
{

// ─── RED viewports ──────────────────────────────────────────────────────────
// Mouse handlers only latch state (+0x58 cursor, +0x60..+0x62 buttons) that RED polls from its idle loop.

// The view message map (0x0055C170): AFX_MSGMAP_ENTRY pfn slots of WM_LBUTTONDOWN, WM_LBUTTONUP and
// WM_LBUTTONDBLCLK, each a thiscall (UINT flags, CPoint point) handler that returns 0xC bytes.
constexpr uintptr_t msgmap_lbutton_down_pfn = 0x0055C19C;
constexpr uintptr_t msgmap_lbutton_up_pfn = 0x0055C1B4;
constexpr uintptr_t msgmap_lbutton_dblclk_pfn = 0x0055C274;

HWND view_hwnd(void* view)
{
    return view ? WndToHandle(static_cast<CWnd*>(view)) : nullptr;
}

bool is_view(void* view)
{
    for (int i = 0; view && i < editor_num_views; i++) {
        if (editor_view_at(i) == view) return true;
    }
    return false;
}

// ─── State ──────────────────────────────────────────────────────────────────

struct Settings
{
    tp::Tool tool = tp::Tool::paint_layer;
    int layer = 0;
    float radius = 8.0f;   // world units
    float strength = 0.5f; // 0..1
    tp::Falloff falloff = tp::Falloff::smooth;
    float spacing = 0.25f; // of the radius
    float set_height = 0.0f; // world y
    float ramp_angle = 15.0f; // degrees, rising along the drag
};
Settings g_settings;

// The Layer list as the selection last saw it, so edits in Properties keep the selection on its entry.
struct LayerListState
{
    const DedTerrain* owner = nullptr;
    tp::LayerListShape shape;
};
LayerListState g_layer_list;

constexpr float min_radius = 0.1f;
constexpr float max_radius = 4096.0f;
constexpr float max_set_height = at::max_coord + at::max_height_range;

// While a sculpt stroke is held still, it dabs this often; the panel's timer keeps the idle loop running.
constexpr DWORD repeat_ms = 40;
constexpr UINT_PTR repeat_timer_id = 1;

constexpr int tool_ids[tp::tool_count] = {
    IDC_TTOOLS_PAINT,  IDC_TTOOLS_ERASE,          IDC_TTOOLS_SMOOTH,  IDC_TTOOLS_HOLES,
    IDC_TTOOLS_CLEAR_HOLES, IDC_TTOOLS_RAISE, IDC_TTOOLS_LOWER, IDC_TTOOLS_SMOOTH_HEIGHTS,
    IDC_TTOOLS_FLATTEN, IDC_TTOOLS_SET_HEIGHT, IDC_TTOOLS_NOISE, IDC_TTOOLS_GEO_CHUNKS,
    IDC_TTOOLS_RAMP, IDC_TTOOLS_RAMP_BETWEEN,
};

const char* const tool_hints[tp::tool_count] = {
    "Blends toward the selected layer, or raises the selected overlay's or decoration's coverage. Strength: how "
    "far per dab.",
    "Blends toward layer 1, the base, or lowers the selected overlay's or decoration's coverage. Strength: how far "
    "per dab.",
    "Blends layer weights, or the selected overlay's or decoration's coverage, toward their neighbours'. "
    "Strength: how far per dab.",
    "Cuts holes in the cells under the brush. Strength and falloff do not apply.",
    "Fills holes in the cells under the brush. Strength and falloff do not apply.",
    "Raises by up to strength x radius / 20 per dab at the centre; hold still to keep raising. The height "
    "range grows as needed.",
    "Lowers by up to strength x radius / 20 per dab at the centre; hold still to keep lowering. The height "
    "range grows as needed; a geoable terrain's bottom only moves if the surface is lowered through it.",
    "Blends heights toward their local average. Strength: the blend per dab; hold still to keep smoothing.",
    "Blends toward the surface height where the stroke started. Strength: the blend per dab.",
    "Blends toward the Height field (world Y). Strength: the blend per dab. Ctrl+click samples the height "
    "under the cursor.",
    "Adds noise of up to +/- strength x radius / 4, a new pattern each stroke; going over a spot again in the "
    "same stroke does not add more.",
    "Click or drag over chunks to set whether craters carve them (orange = geoable); the first chunk decides "
    "on or off. No rebuild needed, just save.",
    "Slopes at the Angle field along the drag (negative goes down) from the height where the stroke began; "
    "the direction locks after a short drag. Strength: the blend per dab.",
    "Press at one end and release at the other: blends toward a straight ramp between the heights there, as "
    "wide as the brush. Strength: the blend, applied once on release.",
};

// Chunk outlines while Geoable Chunks is the tool
constexpr uint8_t geo_chunk_on_rgb[3] = {0xff, 0x80, 0x00};
constexpr uint8_t geo_chunk_off_rgb[3] = {0x80, 0x80, 0x80};
// Swatch of the decoration entries in the Layer list
constexpr COLORREF decoration_swatch_color = RGB(80, 160, 60);

struct Panel
{
    HWND hwnd = nullptr;
    DedTerrain* target = nullptr;
    HHOOK msg_hook = nullptr;
    bool updating = false;
    std::vector<std::string> layer_labels;
    std::vector<COLORREF> layer_colors;
    // Geoable Chunks is enabled: the target is geoable.
    bool geo_allowed = false;
    std::string status;
};
Panel g_panel;

struct Hover
{
    bool valid = false;
    float pos[3] = {};
    void* view = nullptr;
    POINT cursor{-1, -1};
    const TerrainGrid* grid = nullptr;
    Vector3 terrain_pos{};
    float radius = 0.0f;
    uint32_t height_serial = 0;
};
Hover g_hover;
DWORD g_other_views_repainted = 0;
bool g_other_views_pending = false;
// Bumped by every height edit, so the cursor follows a surface that moved under it.
uint32_t g_height_serial = 0;
// A one-off message on the panel's status line (range grown, stitch result, ...).
std::string g_note;

struct Stroke
{
    bool active = false;
    void* view = nullptr;
    HWND hwnd = nullptr;
    DedTerrain* terrain = nullptr;
    // Between the first dab that changed something and the stroke's end: the grid and height mapping
    // before the stroke (for the undo diff) and the terrain's own copy the dabs edit in place.
    std::shared_ptr<const TerrainGrid> before;
    std::shared_ptr<TerrainGrid> grid;
    float height_min_before = 0.0f, height_range_before = 0.0f, thickness_before = 0.0f;
    tp::Rect texels, cells, verts;
    bool requantized = false;
    bool has_last = false;
    float last[2] = {};
    DWORD last_dab = 0;
    // Sculpting: the flatten target (offset above origin.y), and the noise pattern and coverage.
    bool has_target = false;
    float target = 0.0f;
    uint32_t seed = 0;
    float noise_feature = 1.0f;
    std::vector<float> coverage;
    // Ramp tools: where the stroke started (world x, z) and the surface offset there; a ramp's
    // direction once locked; a Bridge Points stroke's far end.
    bool has_anchor = false;
    float anchor[2] = {};
    float anchor_offset = 0.0f;
    bool ramp_locked = false;
    float ramp_dir[2] = {};
    bool has_end = false;
    float end[2] = {};
    float end_offset = 0.0f;
    // Sculpting: each vertex's exact offset so far (tp::SculptDab::exact).
    std::vector<double> exact;
    tp::SculptScratch scratch;
    // Geoable Chunks: the state the stroke's first chunk decided, and from its first change until the
    // commit, the terrain's stored mask before it and how many chunks changed.
    bool geo_decided = false;
    bool geo_state = false;
    bool geo_changed = false;
    std::vector<uint8_t> geo_before;
    at::ChunkLayout geo_before_layout{};
    uint32_t geo_count = 0;
};
Stroke g_stroke;
uint32_t g_stroke_count = 0;
// The view whose WM_LBUTTONDOWN paint mode took, so its matching WM_LBUTTONUP never reaches RED's
// click-select code either.
void* g_swallow_up_view = nullptr;

struct KeyRepeat
{
    bool down = false;
    DWORD next = 0;
};
KeyRepeat g_keys[2];

// ─── Undo ───────────────────────────────────────────────────────────────────

// A Geoable Chunks stroke: the terrain's stored chunk geo mask and its layout.
struct GeoChunks
{
    std::vector<uint8_t> mask;
    at::ChunkLayout layout{};
};

struct UndoEntry
{
    tp::StrokeDiff diff;
    uint64_t seq = 0;
    bool geometry = false; // holes or heights
    bool geo = false;      // geo_before / geo_after instead of the diff
    GeoChunks geo_before, geo_after;
    std::size_t bytes = 0;
};

struct UndoStack
{
    const DedTerrain* terrain = nullptr;
    // The grid and height mapping the top of the stack describes; anything else means a non-paint
    // edit replaced them.
    std::weak_ptr<const TerrainGrid> current;
    float height_min = 0.0f, height_range = 0.0f, thickness = 0.0f;
    std::deque<UndoEntry> undo, redo;
};

constexpr std::size_t undo_budget = 64u << 20;
std::vector<std::unique_ptr<UndoStack>> g_stacks;
std::size_t g_undo_bytes = 0;
uint64_t g_undo_seq = 0;

void clear_entries(std::deque<UndoEntry>& q)
{
    for (const UndoEntry& e : q) g_undo_bytes -= e.bytes;
    q.clear();
}

void clear_stack(UndoStack& s)
{
    clear_entries(s.undo);
    clear_entries(s.redo);
}

UndoStack* find_stack(const DedTerrain* t)
{
    for (auto& s : g_stacks) {
        if (s->terrain == t) return s.get();
    }
    return nullptr;
}

// The terrain's stack, emptied first when its grid is no longer the one the stack ends on.
UndoStack* valid_stack(const DedTerrain* t)
{
    UndoStack* s = find_stack(t);
    if (s && (s->current.lock().get() != t->data.grid.get() || s->height_min != t->data.height_min ||
              s->height_range != t->data.height_range || s->thickness != t->data.thickness)) {
        clear_stack(*s);
    }
    return s;
}

// The terrain's grid and mapping are what its stack now ends on.
void track_stack(const DedTerrain* t)
{
    if (UndoStack* s = find_stack(t)) {
        s->current = t->data.grid;
        s->height_min = t->data.height_min;
        s->height_range = t->data.height_range;
        s->thickness = t->data.thickness;
    }
}

void drop_stack(const DedTerrain* t)
{
    for (auto it = g_stacks.begin(); it != g_stacks.end(); ++it) {
        if ((*it)->terrain == t) {
            clear_stack(**it);
            g_stacks.erase(it);
            return;
        }
    }
}

// Oldest undo steps go first, across every terrain; redo steps only once no undo step is left.
void enforce_budget()
{
    while (g_undo_bytes > undo_budget) {
        UndoStack* oldest = nullptr;
        for (auto& s : g_stacks) {
            if (!s->undo.empty() && (!oldest || s->undo.front().seq < oldest->undo.front().seq)) oldest = s.get();
        }
        if (oldest) {
            g_undo_bytes -= oldest->undo.front().bytes;
            oldest->undo.pop_front();
            continue;
        }
        for (auto& s : g_stacks) clear_entries(s->redo);
        break;
    }
}

// ─── Helpers ────────────────────────────────────────────────────────────────

bool target_in_level(CDedLevel* level, const DedTerrain* t)
{
    if (!level || !t) return false;
    const auto& list = level->GetAlpineLevelProperties().terrain_objects;
    return std::find(list.begin(), list.end(), t) != list.end();
}

bool terrain_paintable(const DedTerrain* t)
{
    return t && !t->hidden_in_editor && t->data.grid && !t->data.layers.empty();
}

bool terrain_geoable(const DedTerrain* t)
{
    return t && (t->data.flags & at::flag_geoable);
}

TerrainCellRect texels_to_cells(const tp::Rect& r, uint32_t mul)
{
    return {r.x0 / mul, r.z0 / mul, (r.x1 + mul - 1) / mul, (r.z1 + mul - 1) / mul};
}

// The surface height at world (x, z), as an offset above origin.y.
float surface_offset(const DedTerrain& t, float x, float z)
{
    return at::height_at(terrain_grid_view(t.pos, t.data, *t.data.grid), x, z) - t.pos.y;
}

// Mid-drag, what a ramp tool will do.
std::string ramp_status()
{
    const DedTerrain* t = g_stroke.terrain;
    if (!g_stroke.active || !g_stroke.has_anchor || !terrain_paintable(t)) return {};
    if (g_settings.tool == tp::Tool::ramp) {
        if (!g_stroke.ramp_locked) return "Ramp: drag to set its direction";
        return std::format("Ramp {:.3g} deg toward x {:.2f}, z {:.2f}", g_settings.ramp_angle, g_stroke.ramp_dir[0],
                           g_stroke.ramp_dir[1]);
    }
    if (g_settings.tool == tp::Tool::ramp_between && g_stroke.has_end) {
        const float dx = g_stroke.end[0] - g_stroke.anchor[0], dz = g_stroke.end[1] - g_stroke.anchor[1];
        const float length = std::sqrt(dx * dx + dz * dz);
        const float rise = surface_offset(*t, g_stroke.end[0], g_stroke.end[1]) - g_stroke.anchor_offset;
        return std::format("Length {:.4g}, rise {:.4g} ({:.3g} deg)", length, rise,
                           std::atan2(rise, length) * (180.0f / 3.14159265f));
    }
    return {};
}

void update_status()
{
    if (!g_panel.hwnd) return;
    const UndoStack* s = g_panel.target ? valid_stack(g_panel.target) : nullptr;
    const std::size_t undo = s ? s->undo.size() : 0, redo = s ? s->redo.size() : 0;
    std::string text = std::format("Undo {}, redo {} ({:.1f} MB)", undo, redo,
                                   static_cast<double>(g_undo_bytes) / (1024.0 * 1024.0));
    const DedTerrain* t = g_panel.target;
    if (g_settings.tool == tp::Tool::geo_chunks && terrain_geoable(t) && t->data.grid) {
        const uint32_t n = at::layout_chunk_count(terrain_geo_chunk_layout(t->data));
        uint32_t on = 0;
        for (uint32_t k = 0; k < n; k++) on += terrain_chunk_geoable(t->data, k) ? 1 : 0;
        text += std::format(". Geoable chunks {}/{}", on, n);
    }
    const std::string ramp = ramp_status();
    if (!ramp.empty()) text += ". " + ramp;
    if (!g_note.empty()) text += ". " + g_note;
    // Set only on a change: it is refreshed on every mouse move while a ramp is dragged.
    if (text != g_panel.status) {
        SetDlgItemTextA(g_panel.hwnd, IDC_TTOOLS_STATUS, text.c_str());
        g_panel.status = std::move(text);
    }
    EnableWindow(GetDlgItem(g_panel.hwnd, IDC_TTOOLS_UNDO), undo > 0);
    EnableWindow(GetDlgItem(g_panel.hwnd, IDC_TTOOLS_REDO), redo > 0);
}

// ─── Strokes ────────────────────────────────────────────────────────────────

void invalidate(DedTerrain* t, const tp::Rect& texels, const tp::Rect& cells, uint32_t mul)
{
    if (!texels.empty()) {
        const TerrainCellRect r = texels_to_cells(texels, mul);
        terrain_preview_invalidate(t, &r, false);
        // Coverage and linked weights place the decorations whose shadows are baked.
        if (terrain_decorations_cast(t->data)) terrain_preview_lighting_changed(t);
    }
    if (!cells.empty()) {
        const TerrainCellRect r{cells.x0, cells.z0, cells.x1, cells.z1};
        terrain_preview_invalidate(t, &r, false);
        terrain_preview_lighting_changed(t);
    }
}

// The terrain's stack, made if needed, now ending on the terrain as it is, with its redo steps dropped.
UndoStack& stack_for_commit(const DedTerrain* t)
{
    UndoStack* s = valid_stack(t);
    if (!s) {
        g_stacks.push_back(std::make_unique<UndoStack>());
        s = g_stacks.back().get();
        s->terrain = t;
    }
    s->current = t->data.grid;
    s->height_min = t->data.height_min;
    s->height_range = t->data.height_range;
    s->thickness = t->data.thickness;
    clear_entries(s->redo);
    return *s;
}

void push_entry(UndoStack& s, UndoEntry&& e)
{
    if (e.bytes > undo_budget) {
        clear_stack(s);
        xlog::warn("[Terrain] a paint stroke of {} bytes is over the undo budget; the undo history was cleared",
                   e.bytes);
        return;
    }
    g_undo_bytes += e.bytes;
    s.undo.push_back(std::move(e));
    enforce_budget();
}

// Records a Geoable Chunks stroke's changes so far as one undo step.
void geo_stroke_commit()
{
    DedTerrain* t = g_stroke.terrain;
    if (!g_stroke.geo_changed) return;
    g_stroke.geo_changed = false;
    GeoChunks before{std::move(g_stroke.geo_before), g_stroke.geo_before_layout};
    const uint32_t count = g_stroke.geo_count;
    g_stroke.geo_count = 0;
    if (!t) return;
    mark_level_modified();
    try {
        g_note = std::format("{} chunk(s) now {}", count, g_stroke.geo_state ? "geoable" : "not geoable");
        UndoStack& s = stack_for_commit(t);
        UndoEntry e;
        e.seq = ++g_undo_seq;
        e.geo = true;
        e.geo_before = std::move(before);
        e.geo_after = {t->data.geo_chunks, t->data.geo_chunks_layout};
        e.bytes = sizeof(UndoEntry) + e.geo_before.mask.capacity() + e.geo_after.mask.capacity();
        push_entry(s, std::move(e));
    }
    catch (const std::bad_alloc&) {
        if (UndoStack* s = find_stack(t)) clear_stack(*s);
        xlog::error("[Terrain] out of memory recording a Geoable Chunks stroke; the undo history was cleared");
    }
    update_status();
}

// Records the stroke so far as one undo step and lets the next dab start a fresh copy.
void stroke_commit()
{
    geo_stroke_commit();
    DedTerrain* t = g_stroke.terrain;
    std::shared_ptr<const TerrainGrid> before = std::move(g_stroke.before);
    std::shared_ptr<TerrainGrid> grid = std::move(g_stroke.grid);
    const tp::Rect texels = g_stroke.texels, cells = g_stroke.cells;
    const tp::Rect verts = g_stroke.requantized && grid ? tp::Rect{0, 0, grid->nx, grid->nz} : g_stroke.verts;
    g_stroke.texels = g_stroke.cells = g_stroke.verts = {};
    g_stroke.requantized = false;
    if (!t || !grid || !before || t->data.grid.get() != grid.get()) return;
    if (texels.empty() && cells.empty() && verts.empty()) return;

    CDedLevel* level = CDedLevel::Get();
    if ((!cells.empty() || !verts.empty()) && level) level->mark_geometry_dirty();
    mark_level_modified();

    try {
        UndoStack& s = stack_for_commit(t);
        UndoEntry e;
        e.seq = ++g_undo_seq;
        e.geometry = !cells.empty() || !verts.empty();
        e.diff.texels = texels;
        e.diff.cells = cells;
        e.diff.verts = verts;
        e.diff.height_min_before = g_stroke.height_min_before;
        e.diff.height_range_before = g_stroke.height_range_before;
        e.diff.height_min_after = t->data.height_min;
        e.diff.height_range_after = t->data.height_range;
        e.diff.thickness_before = g_stroke.thickness_before;
        e.diff.thickness_after = t->data.thickness;
        if (!verts.empty()) {
            tp::capture_heights(before->heights.data(), grid->nx, verts, e.diff.heights_before);
            tp::capture_heights(grid->heights.data(), grid->nx, verts, e.diff.heights_after);
        }
        const uint32_t ww = at::weight_width(grid->nx, grid->weight_res_mul);
        const uint32_t wh = at::weight_height(grid->nz, grid->weight_res_mul);
        const uint32_t cx = at::cells(grid->nx);
        e.diff.deco_planes = static_cast<uint32_t>(terrain_decoration_plane_count(*grid));
        if (!texels.empty()) {
            e.diff.overlay = !before->overlay.empty() && before->overlay.size() == grid->overlay.size();
            const bool planes = before->decoration.size() == grid->decoration.size();
            tp::capture_weights(before->weights.data(), ww, wh, texels, e.diff.weights_before,
                                e.diff.overlay ? before->overlay.data() : nullptr,
                                planes ? before->decoration.data() : nullptr, e.diff.deco_planes);
            tp::capture_weights(grid->weights.data(), ww, wh, texels, e.diff.weights_after,
                                e.diff.overlay ? grid->overlay.data() : nullptr,
                                planes ? grid->decoration.data() : nullptr, e.diff.deco_planes);
            if (!planes) e.diff.deco_planes = 0;
        }
        if (!cells.empty()) {
            tp::capture_holes(before->holes.data(), cx, cells, e.diff.holes_before);
            tp::capture_holes(grid->holes.data(), cx, cells, e.diff.holes_after);
        }
        e.bytes = e.diff.bytes();
        push_entry(s, std::move(e));
    }
    catch (const std::bad_alloc&) {
        if (UndoStack* s = find_stack(t)) clear_stack(*s);
        xlog::error("[Terrain] out of memory recording a paint stroke; the undo history was cleared");
    }
    update_status();
}

void stop_repeat_timer()
{
    if (g_panel.hwnd) KillTimer(g_panel.hwnd, repeat_timer_id);
}

void stroke_end()
{
    if (!g_stroke.active) return;
    stroke_commit();
    const HWND hwnd = g_stroke.hwnd;
    g_stroke = Stroke{};
    stop_repeat_timer();
    if (hwnd && GetCapture() == hwnd) ReleaseCapture();
    update_status();
    editor_views_mark_repaint_all();
}

void stroke_abandon()
{
    const HWND hwnd = g_stroke.hwnd;
    g_stroke = Stroke{};
    stop_repeat_timer();
    if (hwnd && GetCapture() == hwnd) ReleaseCapture();
}

// Out of memory mid-edit: the stroke stops, and a terrain it had started editing loses its undo
// history, since it may now hold heights or a grown mapping that no undo step records.
void paint_out_of_memory()
{
    DedTerrain* t = g_stroke.grid || g_stroke.geo_changed ? g_stroke.terrain : nullptr;
    stroke_abandon();
    if (UndoStack* s = t ? find_stack(t) : nullptr) clear_stack(*s);
    try {
        // Dabs already written stay in the terrain, so the level still needs saving and rebuilding.
        if (t) {
            if (CDedLevel* level = CDedLevel::Get()) level->mark_geometry_dirty();
            mark_level_modified();
        }
        xlog::error("[Terrain] out of memory painting; the stroke was stopped and its undo history cleared");
        g_note = "Out of memory: the stroke was stopped";
        g_hover.grid = nullptr;
        update_status();
        editor_views_mark_repaint_all();
    }
    catch (const std::bad_alloc&) {
    }
}

// Gives the stroke its own copy of the terrain's grid (once per stroke, and again after a commit).
void stroke_own_grid(DedTerrain* t)
{
    if (g_stroke.grid && t->data.grid.get() != g_stroke.grid.get()) {
        // Something replaced the grid mid-stroke: what was painted is lost to undo with it.
        g_stroke.before.reset();
        g_stroke.grid.reset();
        g_stroke.texels = g_stroke.cells = g_stroke.verts = {};
        g_stroke.requantized = false;
    }
    if (g_stroke.grid) return;
    valid_stack(t);
    g_stroke.before = t->data.grid;
    g_stroke.height_min_before = t->data.height_min;
    g_stroke.height_range_before = t->data.height_range;
    g_stroke.thickness_before = t->data.thickness;
    g_stroke.coverage.clear();
    g_stroke.exact.clear();
    auto copy = std::make_shared<TerrainGrid>(*g_stroke.before);
    g_stroke.grid = copy;
    t->data.grid = copy;
    terrain_preview_rebind_grid(t, g_stroke.before.get(), t->data.grid);
    track_stack(t);
}

void note_height_write(DedTerrain* t, const tp::HeightGrid& hg, const tp::HeightWrite& w)
{
    if (w.requantized) {
        t->data.thickness = at::thickness_after_growth(t->data.flags, t->data.thickness, t->data.height_min,
                                                       hg.height_min);
        t->data.height_min = hg.height_min;
        t->data.height_range = hg.height_range;
        g_stroke.requantized = true;
        track_stack(t);
        g_note = std::format("Height range grown to {:.6g} .. {:.6g} above the origin", hg.height_min,
                             static_cast<double>(hg.height_min) + hg.height_range);
    }
    if (w.clamped) {
        g_note = std::format("Heights clamped: the height range is at its limit ({:.6g} span, min {:.6g})",
                             at::max_height_range, -at::max_coord);
    }
    if (w.verts.empty()) return;
    g_stroke.verts = tp::rect_union(g_stroke.verts, w.verts);
    g_height_serial++;
    const TerrainCellRect r{w.verts.x0, w.verts.z0, w.verts.x1, w.verts.z1};
    terrain_preview_heights_changed(t, w.requantized ? nullptr : &r);
}

bool sculpt_dab(DedTerrain* t, TerrainGrid& g, const tp::Dab& dab, float x, float z)
{
    DedTerrainData& d = t->data;
    tp::SculptDab s;
    s.tool = g_settings.tool;
    s.dab = dab;
    switch (s.tool) {
    case tp::Tool::raise:
    case tp::Tool::lower:
        s.amount = tp::raise_step(g_settings.radius, g_settings.strength);
        break;
    case tp::Tool::flatten:
        if (!g_stroke.has_target) {
            g_stroke.has_target = true;
            g_stroke.target = surface_offset(*t, x, z);
        }
        s.target = g_stroke.target;
        break;
    case tp::Tool::set_height:
        s.target = g_settings.set_height - t->pos.y;
        break;
    case tp::Tool::ramp:
        if (!g_stroke.ramp_locked) return false;
        s.target = g_stroke.anchor_offset;
        s.plane_x = (g_stroke.anchor[0] - t->pos.x) / d.cell_size;
        s.plane_z = (g_stroke.anchor[1] - t->pos.z) / d.cell_size;
        tp::ramp_slope(g_settings.ramp_angle, g_stroke.ramp_dir[0], g_stroke.ramp_dir[1], d.cell_size, s.slope_x,
                       s.slope_z);
        break;
    case tp::Tool::ramp_between:
        if (!g_stroke.has_anchor || !g_stroke.has_end) return false;
        s.target = g_stroke.anchor_offset;
        s.target_end = g_stroke.end_offset;
        s.seg_x = (g_stroke.end[0] - g_stroke.anchor[0]) / d.cell_size;
        s.seg_z = (g_stroke.end[1] - g_stroke.anchor[1]) / d.cell_size;
        break;
    case tp::Tool::noise:
        if (!g_stroke.before || g_stroke.before->heights.size() != g.heights.size()) return false;
        if (g_stroke.coverage.size() != g.heights.size()) g_stroke.coverage.assign(g.heights.size(), 0.0f);
        if (!g_stroke.seed) {
            g_stroke.seed = static_cast<uint32_t>(at::splitmix64(++g_stroke_count ^ GetTickCount())) | 1u;
            g_stroke.noise_feature = tp::noise_feature_cells(dab.radius);
        }
        s.amount = tp::noise_amplitude(g_settings.radius, g_settings.strength);
        s.base = g_stroke.before->heights.data();
        s.base_min = g_stroke.height_min_before;
        s.base_range = g_stroke.height_range_before;
        s.coverage = g_stroke.coverage.data();
        s.seed = g_stroke.seed;
        s.feature_cells = g_stroke.noise_feature;
        break;
    default:
        break;
    }
    if (g_stroke.exact.size() != g.heights.size()) g_stroke.exact.assign(g.heights.size(), NAN);
    s.exact = g_stroke.exact.data();
    tp::HeightGrid hg{g.heights.data(), g.nx, g.nz, d.height_min, d.height_range};
    hg.headroom_floor = at::growth_headroom_floor(d.flags, d.height_min, d.thickness);
    const tp::HeightWrite w = tp::sculpt_dab(hg, s, g_stroke.scratch);
    note_height_write(t, hg, w);
    return !w.verts.empty();
}

// One dab at world (x, z) on the stroke's terrain. True when it changed something.
bool apply_dab(float x, float z)
{
    DedTerrain* t = g_stroke.terrain;
    g_stroke.last_dab = GetTickCount();
    if (!terrain_paintable(t)) return false;
    stroke_own_grid(t);
    TerrainGrid& g = *g_stroke.grid;
    const DedTerrainData& d = t->data;
    tp::Dab dab;
    dab.cx = (x - t->pos.x) / d.cell_size;
    dab.cz = (z - t->pos.z) / d.cell_size;
    dab.radius = std::max(g_settings.radius / d.cell_size, 0.01f);
    dab.strength = g_settings.strength;
    dab.falloff = g_settings.falloff;
    if (tp::tool_edits_heights(g_settings.tool)) return sculpt_dab(t, g, dab, x, z);

    tp::Rect texels, cells;
    const uint32_t layer_count = static_cast<uint32_t>(std::min<std::size_t>(d.layers.size(), at::max_layers));
    const uint32_t ww = at::weight_width(g.nx, g.weight_res_mul), wh = at::weight_height(g.nz, g.weight_res_mul);
    tp::WeightMaps maps{g.weights.data(), ww, wh, g.weight_res_mul, layer_count};
    // Entries past the base layers in the panel's list are the overlays, then the decorations.
    const int overlay = g_settings.layer - static_cast<int>(layer_count);
    const int overlay_count = static_cast<int>(std::min<std::size_t>(d.overlays.size(), at::max_overlays));
    const bool on_overlay = overlay >= 0 && overlay < overlay_count &&
                            g.overlay.size() == at::overlay_map_bytes(g.nx, g.nz, g.weight_res_mul);
    const int deco = overlay - overlay_count;
    const bool on_deco = deco >= 0 && deco < static_cast<int>(d.decorations.size()) &&
                         static_cast<std::size_t>(deco) < terrain_decoration_plane_count(g);
    uint8_t* deco_plane = on_deco ? terrain_decoration_plane(g, static_cast<std::size_t>(deco)).data() : nullptr;
    tp::CoverageMap cov = on_deco ? tp::CoverageMap{deco_plane, ww, wh, g.weight_res_mul, 0, 1}
                                  : tp::CoverageMap{g.overlay.data(), ww, wh, g.weight_res_mul,
                                                    static_cast<uint32_t>(overlay)};
    const bool on_coverage = on_overlay || on_deco;
    switch (g_settings.tool) {
    case tp::Tool::paint_layer:
        if (on_coverage) texels = tp::paint_coverage(cov, dab, true);
        else if (overlay < 0) texels = tp::paint_layer(maps, dab, static_cast<uint32_t>(std::max(g_settings.layer, 0)));
        break;
    case tp::Tool::erase:
        texels = on_coverage ? tp::paint_coverage(cov, dab, false) : tp::paint_layer(maps, dab, 0);
        break;
    case tp::Tool::smooth:
        texels = on_coverage ? tp::smooth_coverage(cov, dab) : tp::smooth_weights(maps, dab);
        break;
    case tp::Tool::paint_holes:
    case tp::Tool::clear_holes:
        cells = tp::paint_holes(g.holes.data(), at::cells(g.nx), at::cells(g.nz), dab,
                                g_settings.tool == tp::Tool::paint_holes);
        break;
    default:
        break;
    }
    if (texels.empty() && cells.empty()) return false;
    g_stroke.texels = tp::rect_union(g_stroke.texels, texels);
    g_stroke.cells = tp::rect_union(g_stroke.cells, cells);
    invalidate(t, texels, cells, g.weight_res_mul);
    return true;
}

// Geoable Chunks: every chunk the cursor crossed since the last point takes the state the stroke's first
// chunk decided (the opposite of what that chunk was).
bool geo_stroke_to(float x, float z)
{
    DedTerrain* t = g_stroke.terrain;
    if (!terrain_paintable(t) || !terrain_geoable(t)) return false;
    DedTerrainData& d = t->data;
    const at::ChunkLayout layout = terrain_geo_chunk_layout(d);
    const float cx = (x - t->pos.x) / d.cell_size, cz = (z - t->pos.z) / d.cell_size;
    const float px = g_stroke.has_last ? (g_stroke.last[0] - t->pos.x) / d.cell_size : cx;
    const float pz = g_stroke.has_last ? (g_stroke.last[1] - t->pos.z) / d.cell_size : cz;
    g_stroke.has_last = true;
    g_stroke.last[0] = x;
    g_stroke.last[1] = z;
    if (!g_stroke.geo_decided) {
        g_stroke.geo_decided = true;
        g_stroke.geo_state = !terrain_chunk_geoable(d, tp::chunk_at(layout, cx, cz));
    }
    bool changed = false;
    tp::chunks_on_segment(layout, px, pz, cx, cz, [&](uint32_t k) {
        if (terrain_chunk_geoable(d, k) == g_stroke.geo_state) return;
        if (!g_stroke.geo_changed) {
            std::vector<uint8_t> now = terrain_geo_chunks(d);
            g_stroke.geo_before = d.geo_chunks;
            g_stroke.geo_before_layout = d.geo_chunks_layout;
            d.geo_chunks = std::move(now);
            d.geo_chunks_layout = layout;
            g_stroke.geo_changed = true;
        }
        at::set_bit(d.geo_chunks.data(), k, g_stroke.geo_state);
        g_stroke.geo_count++;
        changed = true;
    });
    return changed;
}

// Dabs from the last one toward (x, z), one per spacing step.
bool stroke_to(float x, float z)
{
    const DedTerrain* t = g_stroke.terrain;
    if (!t) return false;
    if (g_settings.tool == tp::Tool::geo_chunks) return geo_stroke_to(x, z);
    const bool ramp = g_settings.tool == tp::Tool::ramp;
    if ((ramp || g_settings.tool == tp::Tool::ramp_between) && !g_stroke.has_anchor) {
        if (!terrain_paintable(t)) return false;
        g_stroke.has_anchor = true;
        g_stroke.anchor[0] = x;
        g_stroke.anchor[1] = z;
        g_stroke.anchor_offset = surface_offset(*t, x, z);
    }
    if (g_settings.tool == tp::Tool::ramp_between) {
        // Applied on release (ramp_between_apply)
        g_stroke.has_end = true;
        g_stroke.end[0] = x;
        g_stroke.end[1] = z;
        return false;
    }
    bool changed = false;
    if (ramp && !g_stroke.ramp_locked) {
        // Nothing until the drag is long enough to give the direction; then the stroke dabs from the
        // anchor on.
        const float dx = x - g_stroke.anchor[0], dz = z - g_stroke.anchor[1];
        const float dist = std::sqrt(dx * dx + dz * dz);
        const float cs = t->data.cell_size;
        if (!(dist >= tp::ramp_lock_cells(g_settings.radius / cs) * cs)) return false;
        g_stroke.ramp_locked = true;
        g_stroke.ramp_dir[0] = dx / dist;
        g_stroke.ramp_dir[1] = dz / dist;
        g_stroke.has_last = true;
        g_stroke.last[0] = g_stroke.anchor[0];
        g_stroke.last[1] = g_stroke.anchor[1];
        changed = apply_dab(g_stroke.anchor[0], g_stroke.anchor[1]);
    }
    if (!g_stroke.has_last) {
        g_stroke.has_last = true;
        g_stroke.last[0] = x;
        g_stroke.last[1] = z;
        return apply_dab(x, z);
    }
    const float step = std::max(g_settings.spacing * g_settings.radius, t->data.cell_size * 0.05f);
    float dx = x - g_stroke.last[0], dz = z - g_stroke.last[1];
    float dist = std::sqrt(dx * dx + dz * dz);
    if (dist < step) return changed;
    dx /= dist;
    dz /= dist;
    constexpr int max_dabs_per_tick = 256;
    for (int n = 0; dist >= step && n < max_dabs_per_tick; n++) {
        g_stroke.last[0] += dx * step;
        g_stroke.last[1] += dz * step;
        dist -= step;
        changed = apply_dab(g_stroke.last[0], g_stroke.last[1]) || changed;
    }
    return changed;
}

// Bridge Points, on release: one pass over the segment from where the stroke started to where
// it ends, whose height is sampled now.
bool ramp_between_apply()
{
    DedTerrain* t = g_stroke.terrain;
    if (!g_stroke.has_anchor || !g_stroke.has_end || !terrain_paintable(t)) return false;
    const float cs = t->data.cell_size;
    const float dx = (g_stroke.end[0] - g_stroke.anchor[0]) / cs, dz = (g_stroke.end[1] - g_stroke.anchor[1]) / cs;
    if (!(std::sqrt(dx * dx + dz * dz) >= tp::min_segment_cells)) {
        g_note = "Bridge Points: drag from one end to the other";
        return false;
    }
    g_stroke.end_offset = surface_offset(*t, g_stroke.end[0], g_stroke.end[1]);
    return apply_dab(g_stroke.anchor[0], g_stroke.anchor[1]);
}

// ─── Cursor tracking ────────────────────────────────────────────────────────

void* view_under_cursor(POINT cursor)
{
    const HWND over = WindowFromPoint(cursor);
    for (int i = 0; over && i < editor_num_views; i++) {
        void* view = editor_view_at(i);
        if (view && view_hwnd(view) == over && IsWindowVisible(over)) return view;
    }
    return nullptr;
}

// Hole cells count as surface, so the brush keeps working over them.
bool cast_at_terrain(void* view, POINT cursor, const DedTerrain& t, float (&hit)[3])
{
    const HWND hwnd = view_hwnd(view);
    POINT client = cursor;
    if (!hwnd || !ScreenToClient(hwnd, &client)) return false;
    static_cast<EditorViewport*>(view)->setup_gr(0);
    const TerrainRay ray = terrain_screen_ray(static_cast<float>(client.x), static_cast<float>(client.y));
    float th = 0.0f;
    if (!terrain_ray_hit(t, ray, terrain_pick_reach, th, /* ignore_holes */ true)) return false;
    for (int i = 0; i < 3; i++) hit[i] = ray.o[i] + ray.d[i] * th;
    return true;
}

// Follows the mouse and paints when a stroke is on. Views repaint only when something they show moved.
void paint_tick()
{
    DedTerrain* t = g_panel.target;
    POINT cursor{};
    GetCursorPos(&cursor);
    void* view = g_stroke.active ? g_stroke.view : view_under_cursor(cursor);
    if (!terrain_paintable(t)) view = nullptr;
    const DWORD now = GetTickCount();

    // Held still, a sculpt stroke keeps dabbing where it last dabbed.
    bool changed = false;
    if (g_stroke.active && g_stroke.has_last && tp::tool_repeats_in_place(g_settings.tool) &&
        now - g_stroke.last_dab >= repeat_ms) {
        changed = apply_dab(g_stroke.last[0], g_stroke.last[1]);
    }

    const bool cursor_moved = view != g_hover.view || cursor.x != g_hover.cursor.x || cursor.y != g_hover.cursor.y;
    const bool moved = cursor_moved ||
                       (t && (t->data.grid.get() != g_hover.grid || t->pos.x != g_hover.terrain_pos.x ||
                              t->pos.y != g_hover.terrain_pos.y || t->pos.z != g_hover.terrain_pos.z)) ||
                       g_settings.radius != g_hover.radius || g_hover.height_serial != g_height_serial;
    if (!moved) {
        if (g_other_views_pending && now - g_other_views_repainted >= 100) {
            g_other_views_pending = false;
            g_other_views_repainted = now;
            editor_views_mark_repaint_all();
        }
        return;
    }

    void* old_view = g_hover.view;
    const bool was_valid = g_hover.valid;
    g_hover.view = view;
    g_hover.cursor = cursor;
    g_hover.radius = g_settings.radius;
    g_hover.height_serial = g_height_serial;
    g_hover.valid = false;
    if (view && t) {
        g_hover.valid = cast_at_terrain(view, cursor, *t, g_hover.pos);
    }

    // Only the mouse moves a stroke on: a surface rising under a still cursor would otherwise walk the
    // brush along the view ray.
    if (g_stroke.active && cursor_moved) {
        if (g_hover.valid) {
            changed = stroke_to(g_hover.pos[0], g_hover.pos[2]) || changed;
        }
        else {
            g_stroke.has_last = false;
        }
        if (g_stroke.has_anchor) update_status();
    }
    if (t) {
        g_hover.grid = t->data.grid.get();
        g_hover.terrain_pos = t->pos;
    }

    // The view under the cursor follows at once, the others a few times a second.
    if (!g_hover.valid && !was_valid && !changed) return;
    if (changed || now - g_other_views_repainted >= 100) {
        editor_views_mark_repaint_all();
        g_other_views_repainted = now;
        g_other_views_pending = false;
    }
    else {
        editor_view_mark_repaint(view);
        editor_view_mark_repaint(old_view);
        g_other_views_pending = true;
    }
}

// ─── Keys ───────────────────────────────────────────────────────────────────

void set_radius(float r)
{
    g_settings.radius = std::clamp(r, min_radius, max_radius);
    if (g_panel.hwnd) {
        g_panel.updating = true;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4g", g_settings.radius);
        SetDlgItemTextA(g_panel.hwnd, IDC_TTOOLS_RADIUS, buf);
        g_panel.updating = false;
    }
}

// [ and ] scale the radius, repeating while held. RED's key state only moves while its window has
// the keyboard, so these never fire while typing in the panel.
void poll_keys()
{
    const bool focus = GetForegroundWindow() == GetMainFrameHandle();
    const DWORD now = GetTickCount();
    const uint8_t codes[2] = {DIK_LBRACKET, DIK_RBRACKET};
    for (int i = 0; i < 2; i++) {
        KeyRepeat& k = g_keys[i];
        const bool down = focus && g_dinput_keys[codes[i]] != 0;
        if (down && (!k.down || static_cast<int32_t>(now - k.next) >= 0)) {
            set_radius(i == 0 ? g_settings.radius / 1.15f : g_settings.radius * 1.15f);
            k.next = now + (k.down ? 60 : 400);
        }
        k.down = down;
    }
}

// ─── Undo / redo ────────────────────────────────────────────────────────────

void undo_step(bool undo)
{
    DedTerrain* t = g_panel.target;
    if (!t || !t->data.grid || !IsWindowEnabled(GetMainFrameHandle())) return;
    if (g_stroke.active) stroke_commit();
    UndoStack* s = valid_stack(t);
    std::deque<UndoEntry>* from = s ? (undo ? &s->undo : &s->redo) : nullptr;
    if (!from || from->empty()) {
        update_status();
        return;
    }
    try {
        if (from->back().geo) {
            const GeoChunks& side = undo ? from->back().geo_before : from->back().geo_after;
            std::vector<uint8_t> mask = side.mask;
            UndoEntry e = std::move(from->back());
            from->pop_back();
            t->data.geo_chunks = std::move(mask);
            t->data.geo_chunks_layout = (undo ? e.geo_before : e.geo_after).layout;
            mark_level_modified();
            (undo ? s->redo : s->undo).push_back(std::move(e));
            g_note.clear();
            update_status();
            editor_views_mark_repaint_all();
            return;
        }
        // Backstop: adding or removing a decoration replaces the grid, which already empties the stack.
        if (from->back().diff.deco_planes != terrain_decoration_plane_count(*t->data.grid)) {
            clear_stack(*s);
            update_status();
            return;
        }
        auto g = std::make_shared<TerrainGrid>(*t->data.grid);
        UndoEntry e = std::move(from->back());
        from->pop_back();
        const uint32_t ww = at::weight_width(g->nx, g->weight_res_mul);
        const uint32_t wh = at::weight_height(g->nz, g->weight_res_mul);
        tp::apply_diff(e.diff, !undo, g->weights.data(), ww, wh, g->holes.data(), at::cells(g->nx),
                       g->overlay.empty() ? nullptr : g->overlay.data(),
                       g->decoration.empty() ? nullptr : g->decoration.data());
        tp::apply_height_diff(e.diff, !undo, g->heights.data(), g->nx, t->data.height_min, t->data.height_range,
                              t->data.thickness);
        const TerrainGrid* old = t->data.grid.get();
        t->data.grid = g;
        terrain_preview_rebind_grid(t, old, t->data.grid);
        invalidate(t, e.diff.texels, e.diff.cells, g->weight_res_mul);
        if (!e.diff.verts.empty()) {
            g_height_serial++;
            const TerrainCellRect r{e.diff.verts.x0, e.diff.verts.z0, e.diff.verts.x1, e.diff.verts.z1};
            terrain_preview_heights_changed(t, e.diff.mapping_changed() ? nullptr : &r);
        }
        track_stack(t);
        if (e.geometry) {
            if (CDedLevel* level = CDedLevel::Get()) level->mark_geometry_dirty();
        }
        mark_level_modified();
        (undo ? s->redo : s->undo).push_back(std::move(e));
    }
    catch (const std::bad_alloc&) {
        clear_stack(*s);
        xlog::error("[Terrain] out of memory applying paint undo; the undo history was cleared");
    }
    g_hover.grid = nullptr;
    update_status();
    editor_views_mark_repaint_all();
}

bool can_step(bool undo)
{
    const DedTerrain* t = g_panel.target;
    if (!g_panel.hwnd || !t || !t->data.grid) return false;
    // undo_step commits the stroke first, which clears the redo steps
    const bool pending = g_stroke.active && g_stroke.terrain == t && (g_stroke.grid || g_stroke.geo_changed);
    const UndoStack* s = valid_stack(t);
    return undo ? pending || (s && !s->undo.empty()) : !pending && s && !s->redo.empty();
}

// ─── Brush cursor ───────────────────────────────────────────────────────────

// A polyline laid on the surface `lift` above it; a piece with an end off the grid is skipped.
class DrapedPath
{
public:
    DrapedPath(const at::GridView& v, float lift) : v_{v}, lift_{lift} {}

    void move_to(float x, float z)
    {
        prev_in_ = point(x, z, prev_);
    }

    void line_to(float x, float z)
    {
        Vector3 cur;
        const bool in = point(x, z, cur);
        if (in && prev_in_) gr_line_3d(&prev_, &cur, editor_line_mode());
        prev_ = cur;
        prev_in_ = in;
    }

private:
    bool point(float x, float z, Vector3& p) const
    {
        p.x = x;
        p.z = z;
        p.y = at::height_at(v_, x, z) + lift_;
        return x >= v_.origin[0] && x <= v_.origin[0] + at::extent(v_.nx, v_.cell_size) && z >= v_.origin[2] &&
               z <= v_.origin[2] + at::extent(v_.nz, v_.cell_size);
    }

    const at::GridView& v_;
    float lift_;
    Vector3 prev_{};
    bool prev_in_ = false;
};

void draw_ring(const at::GridView& v, float cx, float cz, float radius, float lift)
{
    const int segments =
        std::clamp(static_cast<int>(std::ceil(6.2831853f * radius / (v.cell_size * 0.5f))), 24, 256);
    DrapedPath path{v, lift};
    path.move_to(cx + radius, cz);
    for (int i = 1; i <= segments; i++) {
        const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(segments);
        path.line_to(cx + std::cos(a) * radius, cz + std::sin(a) * radius);
    }
}

// The outline of every point within `radius` of the segment (ax, az)-(bx, bz).
void draw_capsule(const at::GridView& v, float ax, float az, float bx, float bz, float radius, float lift)
{
    const float dx = bx - ax, dz = bz - az;
    const float length = std::sqrt(dx * dx + dz * dz);
    if (!(length > 0.0f)) {
        draw_ring(v, ax, az, radius, lift);
        return;
    }
    const float ux = dx / length, uz = dz / length; // along the segment
    const float nx = -uz, nz = ux;                  // to its side
    const float piece = v.cell_size * 0.5f;
    const int arc = std::clamp(static_cast<int>(std::ceil(3.14159265f * radius / piece)), 12, 128);
    const int side = std::clamp(static_cast<int>(std::ceil(length / piece)), 1, 256);
    DrapedPath path{v, lift};
    // One side, the far cap, the other side, the near cap.
    path.move_to(ax + nx * radius, az + nz * radius);
    for (int end = 0; end < 2; end++) {
        const float sign = end == 0 ? 1.0f : -1.0f;
        const float sx = end == 0 ? ax : bx, sz = end == 0 ? az : bz;
        const float cx = end == 0 ? bx : ax, cz = end == 0 ? bz : az;
        for (int i = 1; i <= side; i++) {
            const float f = sign * static_cast<float>(i) / static_cast<float>(side);
            path.line_to(sx + sign * nx * radius + dx * f, sz + sign * nz * radius + dz * f);
        }
        for (int i = 1; i <= arc; i++) {
            const float a = 3.14159265f * static_cast<float>(i) / static_cast<float>(arc);
            const float c = sign * std::cos(a) * radius, s = sign * std::sin(a) * radius;
            path.line_to(cx + c * nx + s * ux, cz + c * nz + s * uz);
        }
    }
}

// A chunk's outline on the surface, `inset` cells inside its rect, each side in at most 16 steps.
void draw_chunk_outline(const at::GridView& v, const at::ChunkRect& r, float inset, float lift)
{
    const float x0 = static_cast<float>(r.x0) + inset, x1 = static_cast<float>(r.x1) - inset;
    const float z0 = static_cast<float>(r.z0) + inset, z1 = static_cast<float>(r.z1) - inset;
    if (!(x0 < x1 && z0 < z1)) return;
    const float corners[5][2] = {{x0, z0}, {x1, z0}, {x1, z1}, {x0, z1}, {x0, z0}};
    auto point = [&](float cx, float cz) {
        Vector3 p;
        p.x = v.origin[0] + cx * v.cell_size;
        p.z = v.origin[2] + cz * v.cell_size;
        p.y = at::height_at(v, p.x, p.z) + lift;
        return p;
    };
    for (int s = 0; s < 4; s++) {
        const float dx = corners[s + 1][0] - corners[s][0], dz = corners[s + 1][1] - corners[s][1];
        const int steps = std::clamp(static_cast<int>(std::ceil(std::max(std::fabs(dx), std::fabs(dz)))), 1, 16);
        Vector3 prev = point(corners[s][0], corners[s][1]);
        for (int i = 1; i <= steps; i++) {
            const float f = static_cast<float>(i) / static_cast<float>(steps);
            const Vector3 cur = point(corners[s][0] + dx * f, corners[s][1] + dz * f);
            gr_line_3d(&prev, &cur, editor_line_mode());
            prev = cur;
        }
    }
}

// Geoable Chunks: every chunk outlined by its state, the one under the cursor inset again in the cursor's colour.
void draw_geo_chunks(const DedTerrain& t, const at::GridView& v, float lift)
{
    const at::ChunkLayout layout = terrain_geo_chunk_layout(t.data);
    const uint32_t n = at::layout_chunk_count(layout);
    constexpr float inset = 0.2f;
    for (bool on : {false, true}) {
        const uint8_t(&rgb)[3] = on ? geo_chunk_on_rgb : geo_chunk_off_rgb;
        set_draw_color(rgb[0], rgb[1], rgb[2], 0xff);
        for (uint32_t k = 0; k < n; k++) {
            if (terrain_chunk_geoable(t.data, k) != on) continue;
            draw_chunk_outline(v, at::chunk_rect(layout.cells_x, layout.cells_z, layout.edge, k), inset, lift);
        }
    }
    if (!g_hover.valid) return;
    const uint32_t k = tp::chunk_at(layout, (g_hover.pos[0] - v.origin[0]) / v.cell_size,
                                    (g_hover.pos[2] - v.origin[2]) / v.cell_size);
    if (g_stroke.active) {
        set_draw_color(0xff, 0x00, 0x00, 0xff);
    }
    else {
        set_draw_color(0xff, 0xff, 0x00, 0xff);
    }
    draw_chunk_outline(v, at::chunk_rect(layout.cells_x, layout.cells_z, layout.edge, k), inset * 3.0f, lift);
}

// ─── Panel ──────────────────────────────────────────────────────────────────

void panel_update_state();

// The base layers, then the overlays, then the decorations.
void panel_refresh_layers(bool force)
{
    if (!g_panel.hwnd || !g_panel.target) return;
    const DedTerrainData& d = g_panel.target->data;
    std::vector<std::string> labels, names;
    tp::LayerListShape shape;
    shape.layers = static_cast<int>(d.layers.size());
    for (std::size_t i = 0; i < d.layers.size(); i++) {
        const std::string& name = d.layers[i].texture;
        labels.push_back(std::format("{}: {}", i + 1, name.empty() ? "(none)" : name));
        names.push_back(name);
    }
    for (std::size_t i = 0; i < d.overlays.size(); i++) {
        const std::string& name = d.overlays[i].texture;
        labels.push_back(std::format("Overlay {}: {}", i + 1, name.empty() ? "(none)" : name));
        names.push_back(name);
        shape.overlays.push_back(name);
    }
    for (std::size_t i = 0; i < d.decorations.size(); i++) {
        const std::string& name = d.decorations[i].mesh;
        labels.push_back(std::format("Deco {}: {}", i + 1, name.empty() ? "(none)" : name));
        shape.decorations.push_back(name);
    }
    if (!force && labels == g_panel.layer_labels) return;
    if (g_layer_list.owner == g_panel.target) {
        g_settings.layer = tp::remap_layer_selection(g_settings.layer, g_layer_list.shape, shape);
    }
    g_layer_list = {g_panel.target, std::move(shape)};
    g_panel.layer_labels = labels;
    g_panel.layer_colors.clear();
    for (const std::string& name : names) {
        uint8_t rgb[3] = {128, 128, 128};
        terrain_preview_layer_color(name, rgb);
        g_panel.layer_colors.push_back(RGB(rgb[0], rgb[1], rgb[2]));
    }
    g_panel.layer_colors.insert(g_panel.layer_colors.end(), d.decorations.size(), decoration_swatch_color);
    HWND list = GetDlgItem(g_panel.hwnd, IDC_TTOOLS_LAYER_LIST);
    SendMessageA(list, LB_RESETCONTENT, 0, 0);
    for (const std::string& label : labels) {
        SendMessageA(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    g_settings.layer = std::clamp(g_settings.layer, 0, std::max(0, static_cast<int>(labels.size()) - 1));
    SendMessageA(list, LB_SETCURSEL, g_settings.layer, 0);
    panel_update_state();
}

void panel_update_state()
{
    if (!g_panel.hwnd) return;
    // Geoable Chunks needs a geoable target; it falls back to the first tool when Geoable is turned off.
    g_panel.geo_allowed = terrain_geoable(g_panel.target);
    EnableWindow(GetDlgItem(g_panel.hwnd, IDC_TTOOLS_GEO_CHUNKS), g_panel.geo_allowed);
    if (!g_panel.geo_allowed && g_settings.tool == tp::Tool::geo_chunks) {
        if (g_stroke.active) stroke_end();
        g_settings.tool = tp::Tool::paint_layer;
        for (int i = 0; i < tp::tool_count; i++) {
            CheckDlgButton(g_panel.hwnd, tool_ids[i],
                           i == static_cast<int>(g_settings.tool) ? BST_CHECKED : BST_UNCHECKED);
        }
        editor_views_mark_repaint_all();
    }
    const bool geo = g_settings.tool == tp::Tool::geo_chunks;
    // Erase and Smooth act on the selected overlay or decoration, if any.
    const bool coverage =
        g_panel.target && (!g_panel.target->data.overlays.empty() || !g_panel.target->data.decorations.empty());
    const bool list = g_settings.tool == tp::Tool::paint_layer ||
                      (coverage && (g_settings.tool == tp::Tool::erase || g_settings.tool == tp::Tool::smooth));
    EnableWindow(GetDlgItem(g_panel.hwnd, IDC_TTOOLS_LAYER_LIST), list);
    const bool weights = !tp::tool_edits_holes(g_settings.tool) && !geo;
    for (int id : {IDC_TTOOLS_STRENGTH, IDC_TTOOLS_STRENGTH_SPIN, IDC_TTOOLS_FALLOFF}) {
        EnableWindow(GetDlgItem(g_panel.hwnd, id), weights);
    }
    for (int id : {IDC_TTOOLS_RADIUS, IDC_TTOOLS_RADIUS_SPIN}) EnableWindow(GetDlgItem(g_panel.hwnd, id), !geo);
    const bool spacing = !geo && g_settings.tool != tp::Tool::ramp_between;
    for (int id : {IDC_TTOOLS_SPACING, IDC_TTOOLS_SPACING_SPIN}) EnableWindow(GetDlgItem(g_panel.hwnd, id), spacing);
    const bool set_height = g_settings.tool == tp::Tool::set_height;
    for (int id : {IDC_TTOOLS_HEIGHT, IDC_TTOOLS_HEIGHT_SPIN}) EnableWindow(GetDlgItem(g_panel.hwnd, id), set_height);
    const bool ramp = g_settings.tool == tp::Tool::ramp;
    for (int id : {IDC_TTOOLS_ANGLE, IDC_TTOOLS_ANGLE_SPIN}) EnableWindow(GetDlgItem(g_panel.hwnd, id), ramp);
    SetDlgItemTextA(g_panel.hwnd, IDC_TTOOLS_HINT, tool_hints[static_cast<int>(g_settings.tool)]);
}

void set_height_field(float y)
{
    g_settings.set_height = std::clamp(y, -max_set_height, max_set_height);
    if (!g_panel.hwnd) return;
    g_panel.updating = true;
    alpine_dlg_set_float_field_exact(g_panel.hwnd, IDC_TTOOLS_HEIGHT, g_settings.set_height);
    g_panel.updating = false;
}

// Ctrl+click with Set Height: the Height field takes the surface height under the cursor.
void sample_set_height(void* view)
{
    const DedTerrain* t = g_panel.target;
    POINT cursor{};
    float hit[3];
    if (terrain_paintable(t) && GetCursorPos(&cursor) && cast_at_terrain(view, cursor, *t, hit)) {
        set_height_field(hit[1]);
        g_note.clear();
    }
    else {
        g_note = "Ctrl+click on the terrain to sample its height";
    }
    update_status();
}

// Snaps this terrain's border vertices to the heights of any other terrain with the same cell size
// whose grid is aligned with it and has a vertex at the same x/z, as one undo step.
void stitch_edges()
{
    DedTerrain* t = g_panel.target;
    CDedLevel* level = CDedLevel::Get();
    if (!terrain_paintable(t) || !target_in_level(level, t) || !IsWindowEnabled(GetMainFrameHandle())) return;
    if (g_stroke.active) stroke_end();

    struct Neighbour
    {
        const DedTerrain* terrain;
        at::GridView view;
        int64_t dx, dz; // its vertex (0, 0) is this terrain's vertex (dx, dz)
        bool used = false;
    };
    std::vector<Neighbour> neighbours;
    const float cs = t->data.cell_size;
    for (const DedTerrain* o : level->GetAlpineLevelProperties().terrain_objects) {
        if (!o || o == t || o->hidden_in_editor || !o->data.grid) continue;
        if (std::fabs(o->data.cell_size - cs) > cs * 1e-4f) continue;
        const float fx = (o->pos.x - t->pos.x) / cs, fz = (o->pos.z - t->pos.z) / cs;
        const float rx = std::round(fx), rz = std::round(fz);
        if (std::fabs(fx - rx) > 1e-3f || std::fabs(fz - rz) > 1e-3f) continue;
        neighbours.push_back({o, terrain_grid_view(o->pos, o->data, *o->data.grid), static_cast<int64_t>(rx),
                              static_cast<int64_t>(rz)});
    }

    const TerrainGrid& src = *t->data.grid;
    std::vector<tp::HeightTarget> targets;
    auto match = [&](uint32_t i, uint32_t j) {
        for (Neighbour& n : neighbours) {
            const int64_t oi = static_cast<int64_t>(i) - n.dx, oj = static_cast<int64_t>(j) - n.dz;
            if (oi < 0 || oj < 0 || oi >= n.view.nx || oj >= n.view.nz) continue;
            const float y = at::top_y(n.view, static_cast<uint32_t>(oi), static_cast<uint32_t>(oj));
            targets.push_back({j * src.nx + i, static_cast<double>(y) - t->pos.y});
            n.used = true;
            return;
        }
    };
    for (uint32_t i = 0; i < src.nx; i++) {
        match(i, 0);
        match(i, src.nz - 1);
    }
    for (uint32_t j = 1; j + 1 < src.nz; j++) {
        match(0, j);
        match(src.nx - 1, j);
    }
    const auto used = std::count_if(neighbours.begin(), neighbours.end(), [](const Neighbour& n) { return n.used; });
    if (targets.empty()) {
        g_note = "Stitch: no grid-aligned terrain with the same cell size touches this one's border";
        update_status();
        return;
    }

    g_stroke = Stroke{};
    g_stroke.terrain = t;
    stroke_own_grid(t);
    TerrainGrid& g = *g_stroke.grid;
    tp::HeightGrid hg{g.heights.data(), g.nx, g.nz, t->data.height_min, t->data.height_range};
    hg.headroom_floor = at::growth_headroom_floor(t->data.flags, t->data.height_min, t->data.thickness);
    const tp::HeightWrite w = tp::write_heights(hg, targets);
    g_note.clear();
    note_height_write(t, hg, w);
    const tp::Rect changed = g_stroke.verts;
    stroke_commit();
    g_stroke = Stroke{};
    const std::string range_note = std::move(g_note);
    g_note = changed.empty() ? std::format("Stitch: the border already matches {} terrain(s)", used)
                             : std::format("Stitched {} border vertices to {} terrain(s)", targets.size(), used);
    if (!range_note.empty()) g_note += "; " + range_note;
    g_hover.grid = nullptr;
    update_status();
    editor_views_mark_repaint_all();
}

void panel_set_target(DedTerrain* t)
{
    if (g_stroke.active) stroke_end();
    g_panel.target = t;
    g_hover = Hover{};
    if (!g_panel.hwnd) return;
    SetDlgItemTextA(g_panel.hwnd, IDC_TTOOLS_TARGET, t ? terrain_label(*t).c_str() : "");
    panel_refresh_layers(true);
    update_status();
}

void panel_destroyed()
{
    if (g_stroke.active) stroke_end();
    if (g_panel.msg_hook) UnhookWindowsHookEx(g_panel.msg_hook);
    g_panel = Panel{};
    g_hover = Hover{};
    editor_views_mark_repaint_all();
}

// The panel's keyboard (tab, arrows, typing) goes through IsDialogMessage before RED's accelerators
// see it: MFC's pump would otherwise hand Tab and Ctrl+Z to the main frame.
LRESULT CALLBACK panel_msg_hook(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && wp == PM_REMOVE && g_panel.hwnd) {
        auto* msg = reinterpret_cast<MSG*>(lp);
        if (msg->message >= WM_KEYFIRST && msg->message <= WM_KEYLAST &&
            (msg->hwnd == g_panel.hwnd || IsChild(g_panel.hwnd, msg->hwnd)) && IsDialogMessageA(g_panel.hwnd, msg)) {
            msg->message = WM_NULL;
            msg->wParam = 0;
            msg->lParam = 0;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void panel_read_fields(int changed)
{
    if (g_panel.updating || !g_panel.hwnd) return;
    const HWND h = g_panel.hwnd;
    if (changed == IDC_TTOOLS_RADIUS) {
        const float r = alpine_dlg_get_float_field(h, IDC_TTOOLS_RADIUS);
        if (std::isfinite(r) && r > 0.0f) g_settings.radius = std::clamp(r, min_radius, max_radius);
    }
    else if (changed == IDC_TTOOLS_STRENGTH) {
        const int v = alpine_dlg_get_int_field(h, IDC_TTOOLS_STRENGTH);
        if (v > 0) g_settings.strength = static_cast<float>(std::clamp(v, 1, 100)) / 100.0f;
    }
    else if (changed == IDC_TTOOLS_SPACING) {
        const int v = alpine_dlg_get_int_field(h, IDC_TTOOLS_SPACING);
        if (v > 0) g_settings.spacing = static_cast<float>(std::clamp(v, 5, 200)) / 100.0f;
    }
    else if (changed == IDC_TTOOLS_HEIGHT) {
        const float y = alpine_dlg_get_float_field(h, IDC_TTOOLS_HEIGHT);
        if (std::isfinite(y)) g_settings.set_height = std::clamp(y, -max_set_height, max_set_height);
    }
    else if (changed == IDC_TTOOLS_ANGLE) {
        const float a = alpine_dlg_get_float_field(h, IDC_TTOOLS_ANGLE);
        if (std::isfinite(a)) g_settings.ramp_angle = std::clamp(a, -tp::max_ramp_angle, tp::max_ramp_angle);
    }
}

INT_PTR CALLBACK TerrainToolsProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        g_panel.hwnd = hdlg;
        g_panel.updating = true;
        for (int i = 0; i < tp::tool_count; i++) {
            CheckDlgButton(hdlg, tool_ids[i], i == static_cast<int>(g_settings.tool) ? BST_CHECKED : BST_UNCHECKED);
        }
        HWND falloff = GetDlgItem(hdlg, IDC_TTOOLS_FALLOFF);
        for (const char* name : {"Smooth", "Linear", "Constant"}) {
            SendMessageA(falloff, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
        }
        SendMessageA(falloff, CB_SETCURSEL, static_cast<int>(g_settings.falloff), 0);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4g", g_settings.radius);
        SetDlgItemTextA(hdlg, IDC_TTOOLS_RADIUS, buf);
        SetDlgItemInt(hdlg, IDC_TTOOLS_STRENGTH, static_cast<UINT>(std::lround(g_settings.strength * 100.0f)), FALSE);
        SetDlgItemInt(hdlg, IDC_TTOOLS_SPACING, static_cast<UINT>(std::lround(g_settings.spacing * 100.0f)), FALSE);
        alpine_spinner_init(hdlg, IDC_TTOOLS_RADIUS, IDC_TTOOLS_RADIUS_SPIN, 0.5f, min_radius, max_radius, 2);
        alpine_spinner_init_int(hdlg, IDC_TTOOLS_STRENGTH, IDC_TTOOLS_STRENGTH_SPIN, 5, 1, 100);
        alpine_spinner_init_int(hdlg, IDC_TTOOLS_SPACING, IDC_TTOOLS_SPACING_SPIN, 5, 5, 200);
        alpine_dlg_set_float_field_exact(hdlg, IDC_TTOOLS_HEIGHT, g_settings.set_height);
        alpine_spinner_init(hdlg, IDC_TTOOLS_HEIGHT, IDC_TTOOLS_HEIGHT_SPIN, 1.0f, -max_set_height, max_set_height, 2);
        std::snprintf(buf, sizeof(buf), "%.4g", g_settings.ramp_angle);
        SetDlgItemTextA(hdlg, IDC_TTOOLS_ANGLE, buf);
        alpine_spinner_init(hdlg, IDC_TTOOLS_ANGLE, IDC_TTOOLS_ANGLE_SPIN, 1.0f, -tp::max_ramp_angle,
                            tp::max_ramp_angle, 1);
        CheckDlgButton(hdlg, IDC_TTOOLS_SHOW_DECORATIONS, terrain_decorations_visible() ? BST_CHECKED : BST_UNCHECKED);
        g_panel.updating = false;

        // Top right of RED's window, clear of the side panel's top, kept on the monitor's work area.
        RECT frame{}, self{};
        if (GetWindowRect(GetMainFrameHandle(), &frame) && GetWindowRect(hdlg, &self)) {
            const LONG w = self.right - self.left, h = self.bottom - self.top;
            LONG x = frame.right - w - 24, y = frame.top + 96;
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoA(MonitorFromWindow(GetMainFrameHandle(), MONITOR_DEFAULTTONEAREST), &mi)) {
                x = std::max(std::min(x, mi.rcWork.right - w), mi.rcWork.left);
                y = std::max(std::min(y, mi.rcWork.bottom - h), mi.rcWork.top);
            }
            SetWindowPos(hdlg, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_TTOOLS_PAINT:
        case IDC_TTOOLS_ERASE:
        case IDC_TTOOLS_SMOOTH:
        case IDC_TTOOLS_HOLES:
        case IDC_TTOOLS_CLEAR_HOLES:
        case IDC_TTOOLS_RAISE:
        case IDC_TTOOLS_LOWER:
        case IDC_TTOOLS_SMOOTH_HEIGHTS:
        case IDC_TTOOLS_FLATTEN:
        case IDC_TTOOLS_SET_HEIGHT:
        case IDC_TTOOLS_NOISE:
        case IDC_TTOOLS_GEO_CHUNKS:
        case IDC_TTOOLS_RAMP:
        case IDC_TTOOLS_RAMP_BETWEEN:
            if (IsDlgButtonChecked(hdlg, LOWORD(wp)) == BST_CHECKED) {
                if (g_stroke.active) stroke_commit();
                const int* id = std::find(std::begin(tool_ids), std::end(tool_ids), static_cast<int>(LOWORD(wp)));
                g_settings.tool = static_cast<tp::Tool>(id - std::begin(tool_ids));
                g_note.clear();
                panel_update_state();
                update_status();
                editor_views_mark_repaint_all();
            }
            return TRUE;
        case IDC_TTOOLS_HEIGHT:
        case IDC_TTOOLS_ANGLE:
            if (HIWORD(wp) == EN_CHANGE) panel_read_fields(LOWORD(wp));
            return TRUE;
        case IDC_TTOOLS_STITCH:
            try {
                stitch_edges();
            }
            catch (const std::bad_alloc&) {
                paint_out_of_memory();
            }
            return TRUE;
        case IDC_TTOOLS_LAYER_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE) {
                const LRESULT sel = SendDlgItemMessageA(hdlg, IDC_TTOOLS_LAYER_LIST, LB_GETCURSEL, 0, 0);
                if (sel != LB_ERR) g_settings.layer = static_cast<int>(sel);
            }
            return TRUE;
        case IDC_TTOOLS_RADIUS:
        case IDC_TTOOLS_STRENGTH:
        case IDC_TTOOLS_SPACING:
            if (HIWORD(wp) == EN_CHANGE) panel_read_fields(LOWORD(wp));
            return TRUE;
        case IDC_TTOOLS_FALLOFF:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                const LRESULT sel = SendDlgItemMessageA(hdlg, IDC_TTOOLS_FALLOFF, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel <= 2) g_settings.falloff = static_cast<tp::Falloff>(sel);
                editor_views_mark_repaint_all();
            }
            return TRUE;
        case IDC_TTOOLS_SHOW_DECORATIONS:
            terrain_decorations_set_visible(IsDlgButtonChecked(hdlg, IDC_TTOOLS_SHOW_DECORATIONS) == BST_CHECKED);
            editor_views_mark_repaint_all();
            return TRUE;
        case IDC_TTOOLS_UNDO:
            undo_step(true);
            return TRUE;
        case IDC_TTOOLS_REDO:
            undo_step(false);
            return TRUE;
        case IDC_TTOOLS_PROPERTIES:
            // Deferred so the panel is destroyed outside its own proc before the modal dialog opens.
            PostMessageA(GetMainFrameHandle(), WM_COMMAND, ID_TERRAIN_TOOLS_PROPERTIES, 0);
            return TRUE;
        case IDCANCEL:
            DestroyWindow(hdlg);
            return TRUE;
        }
        break;
    case WM_TIMER:
        // Only wakes RED's idle loop, which dabs a held sculpt stroke.
        if (wp == repeat_timer_id) return TRUE;
        break;
    case WM_CLOSE:
        DestroyWindow(hdlg);
        return TRUE;
    case WM_DESTROY:
        panel_destroyed();
        return TRUE;
    case WM_NOTIFY:
        if (alpine_spinner_handle_notify(hdlg, lp)) return TRUE;
        break;
    case WM_MEASUREITEM: {
        auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (mis && mis->CtlID == IDC_TTOOLS_LAYER_LIST) {
            mis->itemHeight = 18;
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (!dis || dis->CtlID != IDC_TTOOLS_LAYER_LIST) break;
        if (dis->itemID == static_cast<UINT>(-1)) return TRUE;
        const bool selected = (dis->itemState & ODS_SELECTED) != 0;
        const bool enabled = IsWindowEnabled(dis->hwndItem) != FALSE;
        FillRect(dis->hDC, &dis->rcItem, GetSysColorBrush(selected && enabled ? COLOR_HIGHLIGHT : COLOR_WINDOW));
        RECT swatch = dis->rcItem;
        swatch.left += 2;
        swatch.top += 2;
        swatch.bottom -= 2;
        swatch.right = swatch.left + (swatch.bottom - swatch.top);
        const COLORREF color =
            dis->itemID < g_panel.layer_colors.size() ? g_panel.layer_colors[dis->itemID] : RGB(128, 128, 128);
        if (HBRUSH brush = CreateSolidBrush(color)) {
            FillRect(dis->hDC, &swatch, brush);
            DeleteObject(brush);
        }
        FrameRect(dis->hDC, &swatch, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        char text[128] = {};
        SendMessageA(dis->hwndItem, LB_GETTEXT, dis->itemID, reinterpret_cast<LPARAM>(text));
        RECT tr = dis->rcItem;
        tr.left = swatch.right + 4;
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC,
                     GetSysColor(!enabled ? COLOR_GRAYTEXT : selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
        DrawTextA(dis->hDC, text, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (dis->itemState & ODS_FOCUS) DrawFocusRect(dis->hDC, &dis->rcItem);
        return TRUE;
    }
    }
    return FALSE;
}

// ─── Viewport mouse handlers ────────────────────────────────────────────────

bool paint_takes_clicks(void* view)
{
    CDedLevel* level = CDedLevel::Get();
    return g_panel.hwnd && is_view(view) && target_in_level(level, g_panel.target) &&
           IsWindowEnabled(GetMainFrameHandle());
}

void stroke_begin(void* view)
{
    if (g_stroke.active) stroke_end();
    g_stroke = Stroke{};
    g_stroke.active = true;
    g_stroke.view = view;
    g_stroke.hwnd = view_hwnd(view);
    g_stroke.terrain = g_panel.target;
    g_swallow_up_view = view;
    if (g_stroke.hwnd) SetCapture(g_stroke.hwnd);
    if (tp::tool_repeats_in_place(g_settings.tool)) SetTimer(g_panel.hwnd, repeat_timer_id, repeat_ms, nullptr);
    g_note.clear();
    g_hover.cursor = {-1, -1};
    paint_tick();
}

void panel_click(void* view, UINT flags)
{
    try {
        if ((flags & MK_CONTROL) && g_settings.tool == tp::Tool::set_height) {
            g_swallow_up_view = view;
            sample_set_height(view);
            return;
        }
        stroke_begin(view);
    }
    catch (const std::bad_alloc&) {
        paint_out_of_memory();
    }
}

// The button came up on the stroke's view. Ending the stroke any other way applies no bridge points.
void stroke_release()
{
    if (g_settings.tool != tp::Tool::ramp_between || !IsWindowEnabled(GetMainFrameHandle())) return;
    try {
        paint_tick(); // takes the cursor where the button came up
        ramp_between_apply();
    }
    catch (const std::bad_alloc&) {
        paint_out_of_memory();
    }
}

void __fastcall view_lbutton_down(void* view, void* /*edx*/, UINT flags, int x, int y)
{
    if (paint_takes_clicks(view)) {
        panel_click(view, flags);
        return;
    }
    g_swallow_up_view = nullptr;
    static_cast<EditorViewport*>(view)->on_lbutton_down(flags, x, y);
}

void __fastcall view_lbutton_dblclk(void* view, void* /*edx*/, UINT flags, int x, int y)
{
    // The second press of a double click arrives as this instead of WM_LBUTTONDOWN.
    if (paint_takes_clicks(view)) {
        panel_click(view, flags);
        return;
    }
    g_swallow_up_view = nullptr;
    static_cast<EditorViewport*>(view)->on_lbutton_dblclk(flags, x, y);
}

void __fastcall view_lbutton_up(void* view, void* /*edx*/, UINT flags, int x, int y)
{
    if (view && view == g_swallow_up_view) {
        g_swallow_up_view = nullptr;
        if (g_stroke.active && g_stroke.view == view) {
            stroke_release();
            stroke_end();
        }
        return;
    }
    static_cast<EditorViewport*>(view)->on_lbutton_up(flags, x, y);
}

// RED's idle loop focuses whichever view's rect holds the cursor, even under another window such as
// this panel, which cancels that window's button clicks mid-press. Stock behaviour while it is closed.
void* __fastcall view_hover_focus(void* view, void* edx);
CallHook<void* __fastcall(void*, void*)> view_hover_focus_hook{0x004834E1, view_hover_focus};
void* __fastcall view_hover_focus(void* view, void* edx)
{
    const HWND hwnd = view_hwnd(view);
    POINT cursor{};
    if (g_panel.hwnd && hwnd && GetCursorPos(&cursor)) {
        const HWND over = WindowFromPoint(cursor);
        if (over && over != hwnd && !IsChild(hwnd, over)) return nullptr;
    }
    return view_hover_focus_hook.call_target(view, edx);
}

} // namespace

void terrain_paint_open(CDedLevel* level, DedTerrain* terrain)
{
    if (!level || !target_in_level(level, terrain)) return;
    terrain_prepare(*terrain);
    if (!g_panel.hwnd) {
        HWND hwnd = CreateDialogParamA(reinterpret_cast<HINSTANCE>(&__ImageBase),
                                       MAKEINTRESOURCEA(IDD_ALPINE_TERRAIN_TOOLS), GetMainFrameHandle(),
                                       TerrainToolsProc, 0);
        if (!hwnd) {
            xlog::error("[Terrain] the Terrain Tools panel could not be created ({})", GetLastError());
            return;
        }
        g_panel.msg_hook = SetWindowsHookExA(WH_GETMESSAGE, panel_msg_hook, nullptr, GetCurrentThreadId());
    }
    panel_set_target(terrain);
    panel_update_state();
    ShowWindow(g_panel.hwnd, SW_SHOW);
    SetActiveWindow(g_panel.hwnd);
    editor_views_mark_repaint_all();
}

void terrain_paint_open_for_selection(CDedLevel* level)
{
    if (!level) return;
    auto& sel = level->selection;
    for (int i = 0; i < sel.get_size(); i++) {
        DedObject* obj = sel[i];
        if (obj && obj->type == DedObjectType::DED_TERRAIN) {
            terrain_paint_open(level, static_cast<DedTerrain*>(obj));
            return;
        }
    }
    MessageBoxA(GetMainFrameHandle(), "Select a terrain first, then open Terrain Tools.", "Terrain Tools",
                MB_OK | MB_ICONINFORMATION);
}

void terrain_paint_show_properties(CDedLevel* level)
{
    DedTerrain* terrain = g_panel.target;
    if (!g_panel.hwnd || !target_in_level(level, terrain) || !IsWindowEnabled(GetMainFrameHandle())) return;
    DestroyWindow(g_panel.hwnd);
    terrain_show_properties(level, terrain);
}

bool terrain_paint_active()
{
    return g_panel.hwnd != nullptr && g_panel.target != nullptr;
}

void terrain_paint_idle()
{
    if (!g_panel.hwnd) return;
    try {
        CDedLevel* level = CDedLevel::Get();
        if (!target_in_level(level, g_panel.target)) {
            DestroyWindow(g_panel.hwnd);
            return;
        }
        // A modal dialog is up: its own loop runs, and nothing here may touch the terrain.
        if (!IsWindowEnabled(GetMainFrameHandle())) return;
        if (g_stroke.active && (!g_stroke.hwnd || GetCapture() != g_stroke.hwnd)) stroke_end();
        if (terrain_geoable(g_panel.target) != g_panel.geo_allowed) {
            panel_update_state();
            update_status();
        }
        poll_keys();
        panel_refresh_layers(false);
        paint_tick();
    }
    catch (const std::bad_alloc&) {
        paint_out_of_memory();
    }
}

void terrain_paint_textures_reloaded()
{
    try {
        panel_refresh_layers(true);
    }
    catch (const std::bad_alloc&) {
        xlog::error("[Terrain] out of memory refreshing the Terrain Tools layer list");
    }
}

void terrain_paint_draw_cursor(CDedLevel& level)
{
    const DedTerrain* t = g_panel.target;
    if (!g_panel.hwnd || !terrain_paintable(t) || !target_in_level(&level, t)) return;
    const bool geo = g_settings.tool == tp::Tool::geo_chunks;
    const bool segment = g_stroke.active && g_stroke.terrain == t && g_settings.tool == tp::Tool::ramp_between &&
                         g_stroke.has_anchor && g_stroke.has_end;
    if (!geo && !segment && !g_hover.valid) return;
    const at::GridView v = terrain_grid_view(t->pos, t->data, *t->data.grid);
    const float lift = std::max(0.05f, v.cell_size * 0.05f);
    if (geo) {
        if (terrain_geoable(t)) draw_geo_chunks(*t, v, lift);
        return;
    }
    if (g_stroke.active) {
        set_draw_color(0xff, 0x00, 0x00, 0xff);
    }
    else {
        set_draw_color(0xff, 0xff, 0x00, 0xff);
    }
    if (segment) {
        // The ramp's reach, then its centre line between the two end heights.
        const float ax = g_stroke.anchor[0], az = g_stroke.anchor[1], bx = g_stroke.end[0], bz = g_stroke.end[1];
        draw_capsule(v, ax, az, bx, bz, g_settings.radius, lift);
        if (g_settings.falloff != tp::Falloff::constant) {
            draw_capsule(v, ax, az, bx, bz, g_settings.radius * 0.5f, lift);
        }
        set_draw_color(0xff, 0xff, 0x00, 0xff);
        const Vector3 a{ax, t->pos.y + g_stroke.anchor_offset + lift, az};
        const Vector3 b{bx, at::height_at(v, bx, bz) + lift, bz};
        gr_line_3d(&a, &b, editor_line_mode());
        return;
    }
    draw_ring(v, g_hover.pos[0], g_hover.pos[2], g_settings.radius, lift);
    if (!tp::tool_edits_holes(g_settings.tool) && g_settings.falloff != tp::Falloff::constant) {
        draw_ring(v, g_hover.pos[0], g_hover.pos[2], g_settings.radius * 0.5f, lift);
    }
    const float tick = std::max(g_settings.radius * 0.08f, v.cell_size * 0.25f);
    const Vector3 a{g_hover.pos[0], g_hover.pos[1] + lift, g_hover.pos[2]};
    const Vector3 b{g_hover.pos[0], g_hover.pos[1] + lift + tick, g_hover.pos[2]};
    gr_line_3d(&a, &b, editor_line_mode());
}

void terrain_paint_undo()
{
    undo_step(true);
}

void terrain_paint_redo()
{
    undo_step(false);
}

bool terrain_paint_can_undo()
{
    return can_step(true);
}

bool terrain_paint_can_redo()
{
    return can_step(false);
}

bool terrain_paint_stroke_active()
{
    return g_stroke.active;
}

void terrain_paint_end_stroke(const DedTerrain* terrain)
{
    if (g_stroke.active && (!terrain || g_stroke.terrain == terrain)) stroke_commit();
}

void terrain_paint_forget(const DedTerrain* terrain)
{
    if (!terrain) return;
    if (g_stroke.terrain == terrain) stroke_abandon();
    drop_stack(terrain);
    if (g_layer_list.owner == terrain) g_layer_list = LayerListState{};
    if (g_panel.hwnd && g_panel.target == terrain) {
        g_panel.target = nullptr;
        DestroyWindow(g_panel.hwnd);
    }
}

void ApplyTerrainPaintPatches()
{
    write_mem_ptr(msgmap_lbutton_down_pfn, &view_lbutton_down);
    write_mem_ptr(msgmap_lbutton_up_pfn, &view_lbutton_up);
    write_mem_ptr(msgmap_lbutton_dblclk_pfn, &view_lbutton_dblclk);
    view_hover_focus_hook.install();
}
