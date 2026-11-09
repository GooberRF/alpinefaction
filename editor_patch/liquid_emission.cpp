#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <common/rfl_chunk_reader.h>
#include <xlog/xlog.h>
#include "liquid_emission.h"
#include "alpine_obj.h"
#include "alpine_spinner.h"
#include "level.h"
#include "resources.h"
#include "vtypes.h"

namespace
{

constexpr std::uint32_t max_records = 65536;
constexpr int chunk_record_size = 16;

// Keyed by object: stock copies, renumbers and keeps deleted room effects alive for undo, but every copy goes
// through the copy hook below and every new one through the constructor hook.
std::unordered_map<const DedRoomEffect*, LiquidEmission> g_emissions;

std::unordered_map<std::int32_t, LiquidEmission> g_level_loaded;
std::vector<const DedRoomEffect*> g_group_saved;
std::vector<DedRoomEffect*> g_group_loaded;

LiquidEmission sanitized(LiquidEmission e)
{
    if (static_cast<std::uint8_t>(e.mode) > static_cast<std::uint8_t>(LiquidEmitMode::up_and_down)) {
        e.mode = LiquidEmitMode::off;
    }
    const LiquidEmission defaults;
    e.intensity = std::isfinite(e.intensity) ? std::clamp(e.intensity, 0.0f, liquid_emission_max_intensity)
                                             : defaults.intensity;
    e.distance = std::isfinite(e.distance) ? std::clamp(e.distance, 0.0f, liquid_emission_max_distance)
                                           : defaults.distance;
    return e;
}

void set_emission(const DedRoomEffect& effect, const LiquidEmission& e)
{
    if (e == LiquidEmission{}) {
        g_emissions.erase(&effect);
    }
    else {
        g_emissions[&effect] = e;
    }
}

DedRoomEffect* __fastcall room_effect_ctor(DedRoomEffect* self);
FunHook<decltype(room_effect_ctor)> room_effect_ctor_hook{0x004548b0, room_effect_ctor};
DedRoomEffect* __fastcall room_effect_ctor(DedRoomEffect* self)
{
    DedRoomEffect* result = room_effect_ctor_hook.call_target(self);
    // a new object at a freed one's address
    g_emissions.erase(self);
    return result;
}

// FUN_00454a30, the field copy of every room effect clone (FUN_00413e30)
void __fastcall room_effect_copy(DedRoomEffect* self, int edx, const DedRoomEffect* src);
FunHook<decltype(room_effect_copy)> room_effect_copy_hook{0x00454a30, room_effect_copy};
void __fastcall room_effect_copy(DedRoomEffect* self, int edx, const DedRoomEffect* src)
{
    room_effect_copy_hook.call_target(self, edx, src);
    if (src) {
        set_emission(*self, liquid_emission_of(*src));
    }
}

// The room effect serializer FUN_00454b20 as the group writer (FUN_00435630) and reader (FUN_004365c0) call it.
void __fastcall room_effect_group_write(DedRoomEffect* self, int edx, void* file);
CallHook<decltype(room_effect_group_write)> room_effect_group_write_hook{0x00436096, room_effect_group_write};
void __fastcall room_effect_group_write(DedRoomEffect* self, int edx, void* file)
{
    room_effect_group_write_hook.call_target(self, edx, file);
    g_group_saved.push_back(self);
}

void __fastcall room_effect_group_read(DedRoomEffect* self, int edx, void* file);
CallHook<decltype(room_effect_group_read)> room_effect_group_read_hook{0x00437e24, room_effect_group_read};
void __fastcall room_effect_group_read(DedRoomEffect* self, int edx, void* file)
{
    room_effect_group_read_hook.call_target(self, edx, file);
    g_group_loaded.push_back(self);
}

struct Record
{
    std::int32_t key;
    LiquidEmission emission;
};

void write_chunk_body(rf::File& file, const std::vector<Record>& records)
{
    file.write<std::uint32_t>(static_cast<std::uint32_t>(records.size()));
    for (const Record& r : records) {
        file.write<std::int32_t>(r.key);
        file.write<std::uint8_t>(static_cast<std::uint8_t>(r.emission.mode));
        file.write<std::uint8_t>(r.emission.color_r);
        file.write<std::uint8_t>(r.emission.color_g);
        file.write<std::uint8_t>(r.emission.color_b);
        file.write<float>(r.emission.intensity);
        file.write<float>(r.emission.distance);
    }
}

// False on a damaged chunk; the records before the damage were already handed on.
template<typename OnRecord>
bool read_chunk(rf::File& file, std::size_t chunk_len, OnRecord&& on_record)
{
    std::size_t remaining = chunk_len;
    rf::File::ChunkGuard chunk_guard{file, remaining};
    RflChunkReader<rf::File> r{file, remaining};
    std::uint32_t count = 0;
    if (!r.read(count) || count > max_records) {
        return false;
    }
    for (std::uint32_t i = 0; i < count; i++) {
        Record rec{};
        std::uint8_t mode = 0;
        if (!r.read(rec.key) || !r.read(mode) || !r.read(rec.emission.color_r) || !r.read(rec.emission.color_g)
            || !r.read(rec.emission.color_b) || !r.read(rec.emission.intensity) || !r.read(rec.emission.distance)) {
            return false;
        }
        rec.emission.mode = static_cast<LiquidEmitMode>(mode);
        on_record(rec.key, sanitized(rec.emission));
    }
    return true;
}

// ─── Room Effect Properties dialog ──────────────────────────────────────────

constexpr int room_effect_type_combo_id = 1457;

constexpr UINT_PTR dialog_subclass_id = 1;

std::uint8_t g_dialog_color_r = 0;
std::uint8_t g_dialog_color_g = 0;
std::uint8_t g_dialog_color_b = 0;

// What OK leaves set; a field the selection disagreed on and nobody filled in stays each effect's own.
struct DialogResult
{
    std::optional<LiquidEmitMode> mode;
    std::optional<std::array<std::uint8_t, 3>> color;
    std::optional<float> intensity;
    std::optional<float> distance;
};
std::optional<DialogResult> g_dialog_result;

// The room effects the dialog edits: stock's selection, which the modal dialog leaves as it is.
std::vector<DedRoomEffect*> selected_room_effects()
{
    std::vector<DedRoomEffect*> effects;
    if (auto* level = CDedLevel::Get()) {
        for (int i = 0; i < level->selection.size; i++) {
            DedObject* obj = level->selection.data_ptr[i];
            if (obj && obj->type == DedObjectType::DED_ROOM_EFFECT) {
                effects.push_back(static_cast<DedRoomEffect*>(obj));
            }
        }
    }
    return effects;
}

bool dialog_effect_is_liquid(HWND hdlg)
{
    HWND combo = GetDlgItem(hdlg, room_effect_type_combo_id);
    if (!combo) {
        return true;
    }
    char text[64] = {};
    const LRESULT sel = SendMessageA(combo, CB_GETCURSEL, 0, 0);
    if (sel != CB_ERR && SendMessageA(combo, CB_GETLBTEXTLEN, sel, 0) < static_cast<LRESULT>(sizeof(text))) {
        SendMessageA(combo, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(text));
    }
    else {
        GetWindowTextA(combo, text, sizeof(text));
    }
    // blank for a selection of mixed types
    return text[0] == '\0' || std::strcmp(text, "Liquid Room") == 0;
}

void dialog_update_enabled(HWND hdlg)
{
    const bool liquid = dialog_effect_is_liquid(hdlg);
    for (int id : {IDC_LIQUID_EMIT_GROUP, IDC_LIQUID_EMIT_MODE_LABEL, IDC_LIQUID_EMIT_MODE}) {
        EnableWindow(GetDlgItem(hdlg, id), liquid);
    }
    const auto mode = alpine_dlg_combo_data(hdlg, IDC_LIQUID_EMIT_MODE, -1);
    const bool emits = liquid && mode != static_cast<LRESULT>(LiquidEmitMode::off);
    for (int id : {IDC_LIQUID_EMIT_COLOR_LABEL, IDC_LIQUID_EMIT_COLOR_SWATCH, IDC_LIQUID_EMIT_COLOR_CHANGE,
                   IDC_LIQUID_EMIT_COLOR_VALUE, IDC_LIQUID_EMIT_INTENSITY_LABEL, IDC_LIQUID_EMIT_INTENSITY,
                   IDC_LIQUID_EMIT_INTENSITY_SPIN, IDC_LIQUID_EMIT_DISTANCE_LABEL, IDC_LIQUID_EMIT_DISTANCE,
                   IDC_LIQUID_EMIT_DISTANCE_SPIN, IDC_LIQUID_EMIT_DISTANCE_UNITS}) {
        EnableWindow(GetDlgItem(hdlg, id), emits);
    }
}

void dialog_capture(HWND hdlg)
{
    const std::vector<DedRoomEffect*> effects = selected_room_effects();
    if (effects.empty()) {
        return;
    }
    const LiquidEmission first = liquid_emission_of(*effects[0]);
    DialogResult result;
    const auto mode = alpine_dlg_combo_data(hdlg, IDC_LIQUID_EMIT_MODE, -1);
    if (mode >= 0) {
        result.mode = static_cast<LiquidEmitMode>(mode);
    }
    std::uint8_t r = g_dialog_color_r, g = g_dialog_color_g, b = g_dialog_color_b;
    if (alpine_dlg_parse_color_text(hdlg, IDC_LIQUID_EMIT_COLOR_VALUE, r, g, b)) {
        result.color = {r, g, b};
    }
    if (alpine_dlg_get_optional_float_field(hdlg, IDC_LIQUID_EMIT_INTENSITY)) {
        result.intensity = alpine_dlg_get_float_field_exact(hdlg, IDC_LIQUID_EMIT_INTENSITY, first.intensity);
    }
    if (alpine_dlg_get_optional_float_field(hdlg, IDC_LIQUID_EMIT_DISTANCE)) {
        result.distance = alpine_dlg_get_float_field_exact(hdlg, IDC_LIQUID_EMIT_DISTANCE, first.distance);
    }
    g_dialog_result = result;
}

void dialog_apply(const DialogResult& result)
{
    for (DedRoomEffect* effect : selected_room_effects()) {
        LiquidEmission e = liquid_emission_of(*effect);
        if (result.mode) {
            e.mode = *result.mode;
        }
        if (result.color) {
            e.color_r = (*result.color)[0];
            e.color_g = (*result.color)[1];
            e.color_b = (*result.color)[2];
        }
        if (result.intensity) {
            e.intensity = *result.intensity;
        }
        if (result.distance) {
            e.distance = *result.distance;
        }
        set_emission(*effect, sanitized(e));
    }
}

LRESULT CALLBACK dialog_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
            if (HIWORD(wp) == BN_CLICKED) {
                dialog_capture(hwnd);
            }
            break;
        case IDC_LIQUID_EMIT_MODE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                dialog_update_enabled(hwnd);
            }
            break;
        case room_effect_type_combo_id:
            if (HIWORD(wp) == CBN_SELCHANGE || HIWORD(wp) == CBN_EDITCHANGE) {
                const LRESULT handled = DefSubclassProc(hwnd, msg, wp, lp);
                dialog_update_enabled(hwnd);
                return handled;
            }
            break;
        case IDC_LIQUID_EMIT_COLOR_VALUE:
            if (HIWORD(wp) == EN_KILLFOCUS
                && alpine_dlg_parse_color_text(hwnd, IDC_LIQUID_EMIT_COLOR_VALUE, g_dialog_color_r, g_dialog_color_g,
                                               g_dialog_color_b)) {
                alpine_dlg_set_color_controls(hwnd, IDC_LIQUID_EMIT_COLOR_SWATCH, IDC_LIQUID_EMIT_COLOR_VALUE,
                                              g_dialog_color_r, g_dialog_color_g, g_dialog_color_b);
            }
            break;
        case IDC_LIQUID_EMIT_COLOR_CHANGE:
            if (HIWORD(wp) == BN_CLICKED) {
                alpine_dlg_pick_color(hwnd, IDC_LIQUID_EMIT_COLOR_SWATCH, IDC_LIQUID_EMIT_COLOR_VALUE,
                                      g_dialog_color_r, g_dialog_color_g, g_dialog_color_b);
                return 0;
            }
            break;
        }
        break;
    case WM_NOTIFY:
        if (alpine_spinner_handle_notify(hwnd, lp)) {
            return TRUE;
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, dialog_subclass_proc, id);
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void dialog_init(HWND hdlg)
{
    g_dialog_result.reset();
    const std::vector<DedRoomEffect*> effects = selected_room_effects();

    alpine_dlg_combo_add(hdlg, IDC_LIQUID_EMIT_MODE, "Off", static_cast<LPARAM>(LiquidEmitMode::off));
    alpine_dlg_combo_add(hdlg, IDC_LIQUID_EMIT_MODE, "Up only", static_cast<LPARAM>(LiquidEmitMode::up));
    alpine_dlg_combo_add(hdlg, IDC_LIQUID_EMIT_MODE, "Up and down",
                         static_cast<LPARAM>(LiquidEmitMode::up_and_down));

    const LiquidEmission first = effects.empty() ? LiquidEmission{} : liquid_emission_of(*effects[0]);
    bool same_mode = true, same_color = true, same_intensity = true, same_distance = true;
    for (const DedRoomEffect* effect : effects) {
        const LiquidEmission e = liquid_emission_of(*effect);
        same_mode = same_mode && e.mode == first.mode;
        same_color = same_color && e.color_r == first.color_r && e.color_g == first.color_g
                     && e.color_b == first.color_b;
        same_intensity = same_intensity && e.intensity == first.intensity;
        same_distance = same_distance && e.distance == first.distance;
    }

    if (same_mode) {
        alpine_dlg_combo_select(hdlg, IDC_LIQUID_EMIT_MODE, static_cast<LPARAM>(first.mode));
    }
    // the swatch cannot show a mixed colour; the blank value leaves each effect's own on OK
    g_dialog_color_r = same_color ? first.color_r : 255;
    g_dialog_color_g = same_color ? first.color_g : 255;
    g_dialog_color_b = same_color ? first.color_b : 255;
    alpine_dlg_set_color_controls(hdlg, IDC_LIQUID_EMIT_COLOR_SWATCH, IDC_LIQUID_EMIT_COLOR_VALUE,
                                  g_dialog_color_r, g_dialog_color_g, g_dialog_color_b);
    if (!same_color) {
        SetDlgItemTextA(hdlg, IDC_LIQUID_EMIT_COLOR_VALUE, "");
    }
    if (same_intensity) {
        alpine_dlg_set_float_field_exact(hdlg, IDC_LIQUID_EMIT_INTENSITY, first.intensity);
    }
    if (same_distance) {
        alpine_dlg_set_float_field_exact(hdlg, IDC_LIQUID_EMIT_DISTANCE, first.distance);
    }
    alpine_spinner_init(hdlg, IDC_LIQUID_EMIT_INTENSITY, IDC_LIQUID_EMIT_INTENSITY_SPIN, 0.05f, 0.0f,
                        liquid_emission_max_intensity, 2);
    alpine_spinner_init(hdlg, IDC_LIQUID_EMIT_DISTANCE, IDC_LIQUID_EMIT_DISTANCE_SPIN, 0.5f, 0.0f,
                        liquid_emission_max_distance, 2);

    dialog_update_enabled(hdlg);
    SetWindowSubclass(hdlg, dialog_subclass_proc, dialog_subclass_id, 0);
}

BOOL __fastcall room_effect_dialog_init(CWnd* self, int edx);
FunHook<decltype(room_effect_dialog_init)> room_effect_dialog_init_hook{0x0046C870, room_effect_dialog_init};
BOOL __fastcall room_effect_dialog_init(CWnd* self, int edx)
{
    const BOOL result = room_effect_dialog_init_hook.call_target(self, edx);
    if (HWND hdlg = WndToHandle(self)) {
        dialog_init(hdlg);
    }
    return result;
}

// FUN_0040ab80 tests the dialog's DoModal result: "CMP EAX,1; JNZ 0x0040b358" (9 bytes). Stock applies its own
// fields here only on OK, to the selection the dialog was opened for.
CodeInjection room_effect_dialog_result_injection{
    0x0040b00e,
    [](auto& regs) {
        const bool ok = static_cast<int>(regs.eax) == IDOK;
        if (ok && g_dialog_result) {
            dialog_apply(*g_dialog_result);
        }
        g_dialog_result.reset();
        regs.eip = ok ? 0x0040b017 : 0x0040b358;
    },
    false, // no trampoline: the injection fully replaces the test and its branch
};

} // namespace

LiquidEmission liquid_emission_of(const DedRoomEffect& effect)
{
    const auto it = g_emissions.find(&effect);
    return it != g_emissions.end() ? it->second : LiquidEmission{};
}

void liquid_emission_serialize_chunk(CDedLevel& level, rf::File& file)
{
    std::vector<Record> records;
    const auto& effects = level.room_effects;
    for (int i = 0; i < effects.size; i++) {
        const auto* effect = static_cast<const DedRoomEffect*>(effects.data_ptr[i]);
        if (!effect) continue;
        const LiquidEmission e = liquid_emission_of(*effect);
        if (e != LiquidEmission{}) {
            records.push_back({effect->uid, e});
        }
    }
    if (records.empty()) return;
    auto start_pos = level.BeginRflSection(file, alpine_liquid_emission_chunk_id);
    write_chunk_body(file, records);
    level.EndRflSection(file, start_pos);
}

void liquid_emission_level_load_begin()
{
    g_level_loaded.clear();
}

void liquid_emission_deserialize_chunk(rf::File& file, std::size_t chunk_len)
{
    const bool complete = read_chunk(file, chunk_len, [&](std::int32_t uid, const LiquidEmission& e) {
        g_level_loaded[uid] = e;
    });
    if (!complete) {
        xlog::warn("[LiquidEmission] Liquid emission chunk is damaged (len={})", chunk_len);
    }
}

void liquid_emission_level_load_end(const CDedLevel& level)
{
    const auto& effects = level.room_effects;
    for (int i = 0; i < effects.size && !g_level_loaded.empty(); i++) {
        const auto* effect = static_cast<const DedRoomEffect*>(effects.data_ptr[i]);
        if (!effect) continue;
        if (const auto it = g_level_loaded.find(effect->uid); it != g_level_loaded.end()) {
            set_emission(*effect, it->second);
        }
    }
    g_level_loaded.clear();
}

void liquid_emission_group_save_begin()
{
    g_group_saved.clear();
}

void liquid_emission_group_serialize_chunk(rf::File& file)
{
    std::vector<Record> records;
    for (std::size_t i = 0; i < g_group_saved.size(); i++) {
        const LiquidEmission e = liquid_emission_of(*g_group_saved[i]);
        if (e != LiquidEmission{}) {
            records.push_back({static_cast<std::int32_t>(i), e});
        }
    }
    g_group_saved.clear();
    if (records.empty()) return;
    file.write<std::int32_t>(alpine_liquid_emission_chunk_id);
    file.write<std::int32_t>(static_cast<std::int32_t>(sizeof(std::uint32_t) + records.size() * chunk_record_size));
    write_chunk_body(file, records);
}

void liquid_emission_group_load_begin()
{
    g_group_loaded.clear();
}

void liquid_emission_group_deserialize_chunk(rf::File& file, std::size_t chunk_len)
{
    const bool complete = read_chunk(file, chunk_len, [&](std::int32_t index, const LiquidEmission& e) {
        if (index >= 0 && index < static_cast<std::int32_t>(g_group_loaded.size())) {
            set_emission(*g_group_loaded[index], e);
        }
    });
    if (!complete) {
        xlog::warn("[LiquidEmission] The group's liquid emission chunk is damaged (len={})", chunk_len);
    }
}

void ApplyLiquidEmissionPatches()
{
    room_effect_ctor_hook.install();
    room_effect_copy_hook.install();
    room_effect_group_write_hook.install();
    room_effect_group_read_hook.install();
    room_effect_dialog_init_hook.install();
    room_effect_dialog_result_injection.install();
}

// ─── Liquid surface geometry ────────────────────────────────────────────────

namespace
{

// Lambert's polygon irradiance, one edge at a time: the angle the edge subtends from p along the unit normal of
// the plane through p and the edge.
void add_edge_circulation(double (&sum)[3], const Vector3& p, const Vector3& a, const Vector3& b)
{
    const Vector3 va = normalized(a - p);
    const Vector3 vb = normalized(b - p);
    const Vector3 c = cross(va, vb);
    const double s = length(c);
    if (!(s > 1e-9)) {
        return;
    }
    const double theta = std::atan2(s, static_cast<double>(dot(va, vb)));
    sum[0] += c.x * theta / s;
    sum[1] += c.y * theta / s;
    sum[2] += c.z * theta / s;
}

float point_segment_distance_sq(float px, float pz, float ax, float az, float bx, float bz, float& qx, float& qz)
{
    const float dx = bx - ax;
    const float dz = bz - az;
    const float len_sq = dx * dx + dz * dz;
    const float t = len_sq > 0.0f ? std::clamp(((px - ax) * dx + (pz - az) * dz) / len_sq, 0.0f, 1.0f) : 0.0f;
    qx = ax + dx * t;
    qz = az + dz * t;
    return (px - qx) * (px - qx) + (pz - qz) * (pz - qz);
}

} // namespace

bool LiquidSurface::build(const GSolid& solid, const GRoom& room)
{
    polygons_.clear();
    outline_.clear();
    cells_.clear();

    std::vector<std::vector<Point>> sides[2];
    double height_sum[2] = {};
    int height_count[2] = {};
    for (const GFace* face = solid.face_list_head; face; face = face->next_solid) {
        if (!(face->flags & FACE_LIQUID) || face->which_room != &room || std::abs(face->plane.normal.y) < 0.999f) {
            continue;
        }
        const int side = face->plane.normal.y > 0.0f ? 0 : 1;
        std::vector<Point> poly;
        double y_sum = 0.0;
        for_each_face_corner(*face, [&](const GFaceVertex& fv) {
            if (fv.vertex) {
                poly.push_back({fv.vertex->pos.x, fv.vertex->pos.z});
                y_sum += fv.vertex->pos.y;
            }
        });
        if (poly.size() < 3) {
            continue;
        }
        height_sum[side] += y_sum;
        height_count[side] += static_cast<int>(poly.size());
        sides[side].push_back(std::move(poly));
    }
    // a surface built two sided has a face each way; the upward ones are the surface
    const int side = sides[0].empty() ? 1 : 0;
    if (sides[side].empty()) {
        return false;
    }
    height_ = static_cast<float>(height_sum[side] / height_count[side]);

    min_x_ = min_z_ = std::numeric_limits<float>::max();
    max_x_ = max_z_ = std::numeric_limits<float>::lowest();
    for (auto& poly : sides[side]) {
        double area = 0.0;
        for (std::size_t i = 0; i < poly.size(); i++) {
            const Point& a = poly[i];
            const Point& b = poly[(i + 1) % poly.size()];
            area += static_cast<double>(a.x) * b.z - static_cast<double>(b.x) * a.z;
        }
        if (std::abs(area) < 1e-8) {
            continue;
        }
        if (area < 0.0) {
            std::reverse(poly.begin(), poly.end());
        }
        for (const Point& p : poly) {
            min_x_ = std::min(min_x_, p.x);
            max_x_ = std::max(max_x_, p.x);
            min_z_ = std::min(min_z_, p.z);
            max_z_ = std::max(max_z_, p.z);
        }
        polygons_.push_back(std::move(poly));
    }
    if (polygons_.empty()) {
        return false;
    }

    // Neighbouring polygons walk their shared edge in opposite directions, so it cancels out of the sum.
    std::map<std::array<float, 4>, int> net;
    for (const auto& poly : polygons_) {
        for (std::size_t i = 0; i < poly.size(); i++) {
            const Point& a = poly[i];
            const Point& b = poly[(i + 1) % poly.size()];
            if (std::tie(a.x, a.z) < std::tie(b.x, b.z)) {
                net[{a.x, a.z, b.x, b.z}]++;
            }
            else {
                net[{b.x, b.z, a.x, a.z}]--;
            }
        }
    }
    for (const auto& [key, count] : net) {
        const Point lo{key[0], key[1]};
        const Point hi{key[2], key[3]};
        for (int k = 0; k < std::abs(count); k++) {
            outline_.push_back(count > 0 ? Edge{lo, hi} : Edge{hi, lo});
        }
    }

    // Seen from straight above a polygon the circulation points along the receiver's normal or against it.
    const auto& first = polygons_.front();
    Vector3 centre{0.0f, height_ + 1.0f, 0.0f};
    for (const Point& p : first) {
        centre.x += p.x / static_cast<float>(first.size());
        centre.z += p.z / static_cast<float>(first.size());
    }
    double sum[3] = {};
    for (std::size_t i = 0; i < first.size(); i++) {
        const Point& a = first[i];
        const Point& b = first[(i + 1) % first.size()];
        add_edge_circulation(sum, centre, {a.x, height_, a.z}, {b.x, height_, b.z});
    }
    winding_sign_ = sum[1] < 0.0 ? 1.0f : -1.0f;

    build_grid();
    return true;
}

void LiquidSurface::build_grid()
{
    grid_ = std::clamp(static_cast<int>(std::sqrt(static_cast<float>(polygons_.size())) * 2.0f), 1, 64);
    cell_x_ = std::max((max_x_ - min_x_) / static_cast<float>(grid_), 1e-6f);
    cell_z_ = std::max((max_z_ - min_z_) / static_cast<float>(grid_), 1e-6f);
    cells_.assign(static_cast<std::size_t>(grid_) * grid_, {});
    auto cell = [&](float v, float lo, float size) {
        return std::clamp(static_cast<int>((v - lo) / size), 0, grid_ - 1);
    };
    for (int i = 0; i < static_cast<int>(polygons_.size()); i++) {
        float x0 = std::numeric_limits<float>::max(), z0 = x0;
        float x1 = std::numeric_limits<float>::lowest(), z1 = x1;
        for (const Point& p : polygons_[i]) {
            x0 = std::min(x0, p.x);
            x1 = std::max(x1, p.x);
            z0 = std::min(z0, p.z);
            z1 = std::max(z1, p.z);
        }
        for (int cz = cell(z0, min_z_, cell_z_); cz <= cell(z1, min_z_, cell_z_); cz++) {
            for (int cx = cell(x0, min_x_, cell_x_); cx <= cell(x1, min_x_, cell_x_); cx++) {
                cells_[static_cast<std::size_t>(cz) * grid_ + cx].push_back(i);
            }
        }
    }
}

bool LiquidSurface::contains(float x, float z) const
{
    if (!(x >= min_x_ && x <= max_x_ && z >= min_z_ && z <= max_z_) || cells_.empty()) {
        return false;
    }
    const int cx = std::clamp(static_cast<int>((x - min_x_) / cell_x_), 0, grid_ - 1);
    const int cz = std::clamp(static_cast<int>((z - min_z_) / cell_z_), 0, grid_ - 1);
    for (int index : cells_[static_cast<std::size_t>(cz) * grid_ + cx]) {
        const auto& poly = polygons_[index];
        bool inside = false;
        for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
            const Point& a = poly[i];
            const Point& b = poly[j];
            if ((a.z > z) != (b.z > z) && x < (b.x - a.x) * (z - a.z) / (b.z - a.z) + a.x) {
                inside = !inside;
            }
        }
        if (inside) {
            return true;
        }
    }
    return false;
}

float LiquidSurface::nearest_edge_distance_xz(float x, float z, Point* nearest) const
{
    float best = std::numeric_limits<float>::max();
    for (const Edge& e : outline_) {
        float qx, qz;
        const float d = point_segment_distance_sq(x, z, e.a.x, e.a.z, e.b.x, e.b.z, qx, qz);
        if (d < best) {
            best = d;
            if (nearest) {
                *nearest = {qx, qz};
            }
        }
    }
    return std::sqrt(best);
}

Vector3 LiquidSurface::nearest_point(const Vector3& p) const
{
    if (contains(p.x, p.z)) {
        return {p.x, height_, p.z};
    }
    Point q{p.x, p.z};
    nearest_edge_distance_xz(p.x, p.z, &q);
    return {q.x, height_, q.z};
}

float LiquidSurface::distance(const Vector3& p) const
{
    const float dy = p.y - height_;
    if (contains(p.x, p.z)) {
        return std::abs(dy);
    }
    const float dxz = nearest_edge_distance_xz(p.x, p.z, nullptr);
    return std::sqrt(dy * dy + dxz * dxz);
}

float LiquidSurface::form_factor(const Vector3& p, const Vector3& n) const
{
    const float dy = p.y - height_;
    if (std::abs(dy) < 1e-4f || polygons_.empty()) {
        return 0.0f;
    }
    double sum[3] = {};
    auto in_front = [&](const Vector3& q) { return dot(q - p, n); };
    const bool whole = in_front({min_x_, height_, min_z_}) >= 0.0f && in_front({max_x_, height_, min_z_}) >= 0.0f
                       && in_front({min_x_, height_, max_z_}) >= 0.0f && in_front({max_x_, height_, max_z_}) >= 0.0f;
    if (whole) {
        for (const Edge& e : outline_) {
            add_edge_circulation(sum, p, {e.a.x, height_, e.a.z}, {e.b.x, height_, e.b.z});
        }
    }
    else {
        // Only the part in front of the receiver lights it: each polygon is clipped to its tangent plane.
        thread_local std::vector<Vector3> clipped;
        for (const auto& poly : polygons_) {
            clipped.clear();
            for (std::size_t i = 0; i < poly.size(); i++) {
                const Vector3 a{poly[i].x, height_, poly[i].z};
                const Vector3 b{poly[(i + 1) % poly.size()].x, height_, poly[(i + 1) % poly.size()].z};
                const float fa = in_front(a);
                const float fb = in_front(b);
                if (fa >= 0.0f) {
                    clipped.push_back(a);
                }
                if ((fa >= 0.0f) != (fb >= 0.0f)) {
                    clipped.push_back(a + (b - a) * (fa / (fa - fb)));
                }
            }
            for (std::size_t i = 0; clipped.size() >= 3 && i < clipped.size(); i++) {
                add_edge_circulation(sum, p, clipped[i], clipped[(i + 1) % clipped.size()]);
            }
        }
    }
    const double side = dy > 0.0f ? 1.0 : -1.0;
    const double ff =
        side * winding_sign_ * (sum[0] * n.x + sum[1] * n.y + sum[2] * n.z) / (2.0 * std::numbers::pi);
    return static_cast<float>(std::clamp(ff, 0.0, 1.0));
}
