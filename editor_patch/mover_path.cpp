#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <common/rfl_chunk_reader.h>
#include <xlog/xlog.h>
#include "alpine_obj.h"
#include "gizmo.h"
#include "level.h"
#include "mover_path.h"
#include "resources.h"
#include "vtypes.h"

namespace mp = mover_path;

namespace
{

mp::Vec3 to_path(const Vector3& v)
{
    return {v.x, v.y, v.z};
}

Vector3 to_world(const mp::Vec3& v)
{
    return {v.x, v.y, v.z};
}

bool has_path_data(const AlpineLevelProperties& props)
{
    return !props.mover_path_keyframes.empty();
}

bool data_curved(const mp::KeyframeData& data)
{
    return data.segment_mode == static_cast<uint8_t>(mp::SegmentMode::curved);
}

bool group_has_paths(const AlpineLevelProperties& props, const GroupEntry& group)
{
    return !props.legacy_movers && !group.keyframes->rotate_in_place;
}

struct GroupPath
{
    std::vector<DedKeyframe*> keyframes;
    std::vector<mp::Key> keys;
    bool closing_segment = false;

    int count() const
    {
        return static_cast<int>(keyframes.size());
    }

    bool has_segment(int owner) const
    {
        return owner < count() - 1 || closing_segment;
    }

    bool segment_curved(int owner) const
    {
        return has_segment(owner) && data_curved(keys[owner].data);
    }
};

GroupPath group_path(const AlpineLevelProperties& props, const GroupEntry& group)
{
    GroupPath path;
    const auto& objects = group.keyframes->objects;
    for (int i = 0; i < objects.size; i++) {
        auto* kf = static_cast<DedKeyframe*>(objects.data_ptr[i]);
        if (!kf || !kf->active) continue;
        path.keyframes.push_back(kf);
        mp::Key key;
        key.pos = to_path(kf->pos);
        if (const auto it = props.mover_path_keyframes.find(kf->uid); it != props.mover_path_keyframes.end()) {
            key.data = it->second;
        }
        path.keys.push_back(key);
    }
    const auto type = static_cast<mp::MoveType>(group.keyframes->movement_type);
    path.closing_segment = mp::has_closing_segment(type, path.count());
    mp::resolve_handles(path.keys, path.closing_segment);
    return path;
}

int first_active_keyframe_uid(const GroupEntry& group)
{
    const auto& objects = group.keyframes->objects;
    for (int i = 0; i < objects.size; i++) {
        const auto* kf = static_cast<const DedKeyframe*>(objects.data_ptr[i]);
        if (kf && kf->active) {
            return kf->uid;
        }
    }
    return -1;
}

GroupEntry* moving_group_of(const CDedLevel& level, const DedObject* obj)
{
    const auto& groups = level.moving_groups;
    for (int i = 0; i < groups.size; i++) {
        GroupEntry* group = groups.data_ptr[i];
        if (!group || !group->is_moving_group()) continue;
        const auto& members = group->keyframes->objects;
        if (std::find(members.data_ptr, members.data_ptr + members.size, obj) != members.data_ptr + members.size) {
            return group;
        }
    }
    return nullptr;
}

bool group_has_brush(const GroupEntry& group, const BrushNode* brush)
{
    BrushNode* const* begin = group.brushes.data_ptr;
    BrushNode* const* end = begin + group.brushes.size;
    return std::find(begin, end, brush) != end;
}

GroupEntry* moving_group_of_brush(const CDedLevel& level, const BrushNode* brush)
{
    const auto& groups = level.moving_groups;
    for (int i = 0; i < groups.size; i++) {
        GroupEntry* group = groups.data_ptr[i];
        if (group && group->is_moving_group() && group_has_brush(*group, brush)) {
            return group;
        }
    }
    return nullptr;
}

bool path_selected(CDedLevel& level, const GroupPath& path)
{
    return std::any_of(path.keyframes.begin(), path.keyframes.end(),
                       [&](DedKeyframe* kf) { return is_object_selected(&level, kf); });
}

std::vector<DedKeyframe*> selected_keyframes(const CDedLevel& level)
{
    std::vector<DedKeyframe*> keyframes;
    for (int i = 0; i < level.selection.size; i++) {
        DedObject* obj = level.selection.data_ptr[i];
        if (obj && obj->type == DedObjectType::DED_KEYFRAME) {
            keyframes.push_back(static_cast<DedKeyframe*>(obj));
        }
    }
    return keyframes;
}

// ─── Chunk ──────────────────────────────────────────────────────────────────

struct MoverRecord
{
    int32_t first_keyframe;
    uint8_t orient_mode;
};

struct KeyframeRecord
{
    int32_t keyframe;
    mp::KeyframeData data;
};

constexpr int chunk_keyframe_data_size = 28;

int chunk_body_size(const std::vector<MoverRecord>& movers, const std::vector<KeyframeRecord>& keyframes)
{
    return static_cast<int>(sizeof(uint32_t) * 2 + movers.size() * (sizeof(int32_t) + 4)
                            + keyframes.size() * (sizeof(int32_t) + chunk_keyframe_data_size));
}

void write_chunk_body(rf::File& file, const std::vector<MoverRecord>& movers,
                      const std::vector<KeyframeRecord>& keyframes)
{
    file.write<uint32_t>(static_cast<uint32_t>(movers.size()));
    for (const MoverRecord& m : movers) {
        file.write<int32_t>(m.first_keyframe);
        file.write<uint8_t>(m.orient_mode);
        for (int i = 0; i < 3; i++) {
            file.write<uint8_t>(0);
        }
    }
    file.write<uint32_t>(static_cast<uint32_t>(keyframes.size()));
    for (const KeyframeRecord& k : keyframes) {
        file.write<int32_t>(k.keyframe);
        mp::write_keyframe_data(file, k.data);
    }
}

std::vector<const GroupEntry*> g_group_save_groups;

// ─── Viewport drawing ───────────────────────────────────────────────────────

constexpr int curve_draw_steps = 24;
constexpr float handle_radius = 0.2f;
constexpr float tick_seconds = 0.25f;
constexpr float tick_size = 0.08f;
constexpr int max_ticks_per_segment = 64;
const Color handle_color{255, 200, 0};
const Color drag_color{255, 255, 255};
const Color tick_color{120, 220, 255};

void draw_line(const Vector3& a, const Vector3& b, const Color& c)
{
    draw_link_line(a.x, a.y, a.z, b.x, b.y, b.z, c.r, c.g, c.b);
}

void draw_curve(const mp::Curve& curve, const Color& color)
{
    Vector3 prev = to_world(curve.p0);
    for (int i = 1; i <= curve_draw_steps; i++) {
        const float t = static_cast<float>(i) / static_cast<float>(curve_draw_steps);
        const Vector3 p = to_world(mp::sample(curve, t));
        draw_line(prev, p, color);
        prev = p;
    }
}

Vector3 segment_point(const GroupPath& path, int owner, const mp::Curve* curve, float s)
{
    if (curve) {
        return to_world(mp::sample(*curve, mp::param_at_fraction(*curve, s)));
    }
    const mp::Vec3& a = path.keys[owner].pos;
    const mp::Vec3& b = path.keys[(owner + 1) % path.count()].pos;
    return to_world(a + (b - a) * s);
}

// Equal steps of time along the forward run, so bunched ticks show where the mover is slow.
void draw_segment_ticks(const GroupEntry& group, const GroupPath& path, int owner, const mp::Curve* curve)
{
    const DedKeyframe& kf = *path.keyframes[owner];
    const mp::Vec3& a = path.keys[owner].pos;
    const mp::Vec3& b = path.keys[(owner + 1) % path.count()].pos;
    const float dist = curve ? curve->length : mp::length(b - a);
    const bool velocity = group.keyframes->use_travel_time_as_velocity;
    const float travel = mp::travel_seconds(
        mp::effective_travel_param(kf.depart_travel_time, kf.accel_time, kf.decel_time, velocity), dist, velocity);
    float accel = kf.accel_time;
    float decel = kf.decel_time;
    mp::fit_ramps(travel, velocity, accel, decel);

    const int ticks = std::clamp(static_cast<int>(travel / tick_seconds), 1, max_ticks_per_segment);
    const float step = travel / static_cast<float>(ticks);
    for (int i = 1; i < ticks; i++) {
        const float s = mp::segment_progress(step * static_cast<float>(i), travel, accel, decel,
                                             path.keys[owner].data.ease);
        const Vector3 p = segment_point(path, owner, curve, s);
        draw_line({p.x - tick_size, p.y, p.z}, {p.x + tick_size, p.y, p.z}, tick_color);
        draw_line({p.x, p.y - tick_size, p.z}, {p.x, p.y + tick_size, p.z}, tick_color);
        draw_line({p.x, p.y, p.z - tick_size}, {p.x, p.y, p.z + tick_size}, tick_color);
    }
}

// ─── Handle drags ───────────────────────────────────────────────────────────

struct HandleRef
{
    DedKeyframe* keyframe = nullptr;
    bool out = false;
    mp::KeyframeData resolved; // its handles as drawn
};

struct HandleDrag
{
    bool active = false;
    bool started = false;
    HandleRef handle;
    EditorViewport* view = nullptr;
    HWND hwnd = nullptr;
    POINT press{};
    Vector3 plane_point;
    Vector3 plane_normal;
    Vector3 grab_offset; // from the cursor's plane point to the handle
    mp::KeyframeData before;
};
HandleDrag g_handle_drag;

struct HoveredHandle
{
    const DedKeyframe* keyframe = nullptr;
    bool out = false;
    POINT cursor{};
};
HoveredHandle g_hovered_handle;

bool handle_shown(const GroupPath& path, int index, bool out)
{
    if (out) {
        return path.segment_curved(index);
    }
    const int count = path.count();
    const bool has_prev = index > 0 || path.closing_segment;
    return has_prev && path.segment_curved((index + count - 1) % count);
}

Vector3 handle_pos(const DedKeyframe& kf, const mp::KeyframeData& data, bool out)
{
    return kf.pos + to_world(out ? data.out_handle : data.in_handle);
}

void draw_keyframe_handles(const DedKeyframe& kf, const GroupPath& path, int index)
{
    const mp::KeyframeData& data = path.keys[index].data;
    for (bool out : {false, true}) {
        if (!handle_shown(path, index, out)) continue;
        const bool dragged = g_handle_drag.active && g_handle_drag.handle.keyframe == &kf
                          && g_handle_drag.handle.out == out;
        const bool hovered = g_hovered_handle.keyframe == &kf && g_hovered_handle.out == out;
        const Color& color = dragged || hovered ? drag_color : handle_color;
        const Vector3 p = handle_pos(kf, data, out);
        draw_line(kf.pos, p, color);
        set_draw_color(color.r, color.g, color.b, 255);
        draw_wireframe_sphere_3d(&p, handle_radius, editor_line_mode());
    }
}

void draw_group_path(const CDedLevel& level, const GroupEntry& group, const Color& link_color)
{
    const AlpineLevelProperties& props = const_cast<CDedLevel&>(level).GetAlpineLevelProperties();
    if (!group_has_paths(props, group)) return;
    const GroupPath path = group_path(props, group);
    if (path.count() < 2) return;

    const bool selected = path_selected(const_cast<CDedLevel&>(level), path);

    for (int i = 0; i < path.count(); i++) {
        if (!path.has_segment(i)) continue;
        const bool hidden = path.keyframes[i]->hidden_in_editor;
        const bool curved = path.segment_curved(i);
        const bool closing = i == path.count() - 1;
        const mp::Curve curve = curved ? mp::make_segment_curve(path.keys, i) : mp::Curve{};
        if (!hidden && curved) {
            draw_curve(curve, link_color);
        }
        else if (!hidden && closing) {
            draw_line(path.keyframes[i]->pos, path.keyframes[0]->pos, link_color);
        }
        if (selected) {
            draw_segment_ticks(group, path, i, curved ? &curve : nullptr);
        }
    }

    for (int i = 0; i < path.count(); i++) {
        if (selected && !path.keyframes[i]->hidden_in_editor) {
            draw_keyframe_handles(*path.keyframes[i], path, i);
        }
    }
}

// Stock draws no loop's closing link.
void __fastcall draw_keyframes(CDedLevel* level, int edx);
CallHook<void __fastcall(CDedLevel*, int)> draw_keyframes_hook{0x0041F284, draw_keyframes};
void __fastcall draw_keyframes(CDedLevel* level, int edx)
{
    draw_keyframes_hook.call_target(level, edx);
    try {
        const EditorColorPrefs* prefs = editor_color_prefs();
        const Color link_color =
            prefs ? prefs->color(EditorColorPrefIndex::keyframe_link) : Color{255, 0, 0};
        const auto& groups = level->moving_groups;
        for (int i = 0; i < groups.size; i++) {
            const GroupEntry* group = groups.data_ptr[i];
            if (group && group->is_moving_group()) {
                draw_group_path(*level, *group, link_color);
            }
        }
    }
    catch (const std::exception& e) {
        xlog::error("[MoverPath] {}", e.what());
    }
}

// Past stock's check that the keyframe is shown. A curved segment is draw_group_path's.
CodeInjection keyframe_link_injection{
    0x004226C6,
    [](auto& regs) {
        const CDedLevel* level = CDedLevel::Get();
        const auto* group = reinterpret_cast<const GroupEntry*>(static_cast<uintptr_t>(regs.ebp));
        const auto* from = reinterpret_cast<const DedKeyframe*>(static_cast<uintptr_t>(regs.esi));
        const auto& props = const_cast<CDedLevel*>(level)->GetAlpineLevelProperties();
        if (!has_path_data(props) || !group_has_paths(props, *group)) return;
        const auto it = props.mover_path_keyframes.find(from->uid);
        if (it != props.mover_path_keyframes.end() && data_curved(it->second)) {
            regs.eip = 0x004226F5;
        }
    },
};

void view_cursor(HWND hwnd, POINT& cursor)
{
    GetCursorPos(&cursor);
    ScreenToClient(hwnd, &cursor);
}

float world_per_pixels(const EditorRay& ray, const EditorRay& ray_across, const Vector3& p)
{
    const float depth = dot(p - ray.o, ray.d);
    const Vector3 a = ray.o + ray.d * depth;
    const Vector3 b = ray_across.o + ray_across.d * depth;
    return length(b - a);
}

bool find_handle(CDedLevel& level, EditorViewport* view, int x, int y, HandleRef& found)
{
    constexpr float pick_pixels = 8.0f;
    const AlpineLevelProperties& props = level.GetAlpineLevelProperties();
    if (!has_path_data(props) || selected_keyframes(level).empty()) return false;

    view->setup_gr(0);
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    const EditorRay ray = editor_screen_ray(fx, fy);
    const EditorRay across = editor_screen_ray(fx + pick_pixels, fy);

    const bool perspective = view->view_type == editor_view_type_perspective;
    float best_depth = 0.0f;
    bool any = false;
    const auto& groups = level.moving_groups;
    for (int g = 0; g < groups.size; g++) {
        const GroupEntry* group = groups.data_ptr[g];
        if (!group || !group->is_moving_group() || !group_has_paths(props, *group)) continue;
        const GroupPath path = group_path(props, *group);
        if (!path_selected(level, path)) continue;
        for (int i = 0; i < path.count(); i++) {
            DedKeyframe* kf = path.keyframes[i];
            if (kf->hidden_in_editor) continue;
            for (bool out : {false, true}) {
                if (!handle_shown(path, i, out)) continue;
                const Vector3 p = handle_pos(*kf, path.keys[i].data, out);
                const float depth = dot(p - ray.o, ray.d);
                if (perspective && depth <= 0.0f) continue;
                const Vector3 nearest = ray.o + ray.d * depth;
                if (length(p - nearest) > std::max(world_per_pixels(ray, across, p), handle_radius)) continue;
                if (!any || depth < best_depth) {
                    any = true;
                    best_depth = depth;
                    found = {kf, out, path.keys[i].data};
                }
            }
        }
    }
    return any;
}

void hover_tick()
{
    POINT cursor{};
    if (!GetCursorPos(&cursor)
        || (cursor.x == g_hovered_handle.cursor.x && cursor.y == g_hovered_handle.cursor.y)) {
        return;
    }
    auto* view = static_cast<EditorViewport*>(editor_view_under_cursor(cursor));
    CDedLevel* level = CDedLevel::Get();
    const HWND main = GetMainFrameHandle();
    HandleRef handle;
    POINT client = cursor;
    if (view && level && main && IsWindowEnabled(main) && GetForegroundWindow() == main && !view->lbutton_held
        && !view->rbutton_held && !view->mbutton_held && !editor_transform_key_held() && level->transform_mode == 0
        && ScreenToClient(editor_view_hwnd(view), &client)) {
        find_handle(*level, view, client.x, client.y, handle);
    }
    const bool changed = handle.keyframe != g_hovered_handle.keyframe || handle.out != g_hovered_handle.out;
    g_hovered_handle = {handle.keyframe, handle.out, cursor};
    if (changed) {
        editor_views_mark_repaint_all();
    }
}

void apply_handle(HandleDrag& d, const Vector3& offset)
{
    mp::KeyframeData data = d.before;
    data.in_handle = d.handle.resolved.in_handle;
    data.out_handle = d.handle.resolved.out_handle;
    if (data.tangent_mode == static_cast<uint8_t>(mp::TangentMode::automatic)) {
        data.tangent_mode = static_cast<uint8_t>(mp::TangentMode::aligned);
    }
    const mp::Vec3 moved = mp::sanitize_handle(to_path(offset));
    mp::Vec3& own = d.handle.out ? data.out_handle : data.in_handle;
    mp::Vec3& other = d.handle.out ? data.in_handle : data.out_handle;
    own = moved;
    if (data.tangent_mode == static_cast<uint8_t>(mp::TangentMode::aligned)) {
        other = mp::normalized(moved) * -mp::length(other);
    }
    mover_path_set_keyframe_data(*d.handle.keyframe, data);
    editor_views_mark_repaint_all();
}

void update_handle_drag()
{
    HandleDrag& d = g_handle_drag;
    POINT cursor{};
    view_cursor(d.hwnd, cursor);
    if (!d.started) {
        if (std::abs(cursor.x - d.press.x) <= GetSystemMetrics(SM_CXDRAG)
            && std::abs(cursor.y - d.press.y) <= GetSystemMetrics(SM_CYDRAG)) {
            return;
        }
        d.started = true;
    }
    d.view->setup_gr(0);
    const EditorRay ray = editor_screen_ray(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
    const float denom = dot(ray.d, d.plane_normal);
    if (std::abs(denom) < 1.0e-4f) return;
    const float t = dot(d.plane_point - ray.o, d.plane_normal) / denom;
    if (!std::isfinite(t)) return;

    Vector3 offset = ray.o + ray.d * t + d.grab_offset - d.handle.keyframe->pos;
    if (gizmo_snapping()) {
        const float step = CDedLevel::Get()->grid_size;
        if (step > 1.0e-6f && std::isfinite(step)) {
            offset = {std::round(offset.x / step) * step, std::round(offset.y / step) * step,
                      std::round(offset.z / step) * step};
        }
    }
    apply_handle(d, offset);
}

void end_handle_drag()
{
    const HWND hwnd = g_handle_drag.hwnd;
    g_handle_drag.active = false;
    if (hwnd && GetCapture() == hwnd) {
        ReleaseCapture();
    }
    editor_views_mark_repaint_all();
}

void cancel_handle_drag()
{
    if (g_handle_drag.active) {
        mover_path_set_keyframe_data(*g_handle_drag.handle.keyframe, g_handle_drag.before);
        end_handle_drag();
    }
}

// ─── Keyframe Properties ────────────────────────────────────────────────────

void fill_combo(HWND hdlg, int id, std::initializer_list<const char*> items, int selected)
{
    const HWND combo = GetDlgItem(hdlg, id);
    SendMessageA(combo, CB_RESETCONTENT, 0, 0);
    for (const char* item : items) {
        SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    }
    SendMessageA(combo, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
}

// The value every keyframe shares, or -1 for a mixed selection: nothing is picked, and OK leaves each as it was.
template<class Fn>
int shared_value(const std::vector<DedKeyframe*>& keyframes, Fn&& value)
{
    int shared = -1;
    for (size_t i = 0; i < keyframes.size(); i++) {
        const int v = value(*keyframes[i]);
        if (i > 0 && v != shared) return -1;
        shared = v;
    }
    return shared;
}

uint8_t group_orient_mode(const AlpineLevelProperties& props, const GroupEntry& group)
{
    const auto it = props.mover_path_orient_modes.find(first_active_keyframe_uid(group));
    return it != props.mover_path_orient_modes.end() ? it->second : static_cast<uint8_t>(mp::OrientMode::none);
}

void update_dialog_enables(HWND hdlg)
{
    const CDedLevel* level = CDedLevel::Get();
    const bool legacy = !level || const_cast<CDedLevel*>(level)->GetAlpineLevelProperties().legacy_movers;
    const bool rotating = IsDlgButtonChecked(hdlg, IDC_KF_ROTATE_IN_PLACE) != BST_UNCHECKED;
    EnableWindow(GetDlgItem(hdlg, IDC_KF_PATH_SEGMENT), !legacy && !rotating);
    EnableWindow(GetDlgItem(hdlg, IDC_KF_PATH_TANGENT), !legacy && !rotating);
    EnableWindow(GetDlgItem(hdlg, IDC_KF_PATH_EASE), !legacy);
    EnableWindow(GetDlgItem(hdlg, IDC_KF_PATH_ORIENT), !legacy && !rotating);
}

int combo_selection(HWND hdlg, int id)
{
    return static_cast<int>(SendDlgItemMessageA(hdlg, id, CB_GETCURSEL, 0, 0));
}

// ─── Playback preview ───────────────────────────────────────────────────────

struct Sim
{
    int from = 0;
    int to = -1;
    float t = 0.0f;
    float pause = 0.0f;
    bool forward = true;
    bool last_leg = false; // a rotating mover on the way back of a run that stops at the end of it

    bool operator==(const Sim& o) const
    {
        return from == o.from && to == o.to && t == o.t && pause == o.pause && forward == o.forward
            && last_leg == o.last_leg;
    }
};

struct SegmentTiming
{
    int owner = -1;
    mp::Curve curve;
    bool curved = false;
    float dist = 0.0f;
    float travel = 0.0f;
    float accel = 0.0f;
    float decel = 0.0f;
    uint8_t ease = 0;
};

SegmentTiming segment_timing(const GroupEntry& group, const GroupPath& path, const Sim& sim)
{
    SegmentTiming seg;
    const int count = path.count();
    seg.owner = mp::segment_owner(sim.from, sim.to, count, path.closing_segment);
    seg.curved = seg.owner >= 0 && path.segment_curved(seg.owner);
    if (seg.curved) {
        seg.curve = mp::make_segment_curve(path.keys, seg.owner);
        seg.dist = seg.curve.length;
    }
    else {
        seg.dist = mp::length(path.keys[sim.to].pos - path.keys[sim.from].pos);
    }
    const DedKeyframe& departing = *path.keyframes[sim.from];
    const bool velocity = group.keyframes->use_travel_time_as_velocity;
    const float param = sim.forward ? departing.depart_travel_time : departing.return_travel_time;
    const float travel_param = mp::effective_travel_param(param, departing.accel_time, departing.decel_time, velocity);
    seg.travel = mp::travel_seconds(travel_param, seg.dist, velocity);
    seg.accel = departing.accel_time;
    seg.decel = departing.decel_time;
    mp::fit_ramps(seg.travel, velocity, seg.accel, seg.decel);
    seg.ease = path.keys[sim.from].data.ease;
    return seg;
}

SegmentTiming spin_timing(const GroupEntry& group, const GroupPath& path, const Sim& sim)
{
    SegmentTiming seg;
    const DedKeyframe& kf = *path.keyframes[0];
    const bool velocity = group.keyframes->use_travel_time_as_velocity;
    const float param = sim.forward ? kf.depart_travel_time : kf.return_travel_time;
    seg.travel = mp::travel_seconds(mp::effective_travel_param(param, kf.accel_time, kf.decel_time, velocity), 1.0f,
                                    false);
    seg.accel = kf.accel_time;
    seg.decel = kf.decel_time;
    seg.ease = path.keys[0].data.ease;
    return seg;
}

mp::MoveType group_move_type(const GroupEntry& group)
{
    return static_cast<mp::MoveType>(group.keyframes->movement_type);
}

float phase_left(const GroupEntry& group, const GroupPath& path, const Sim& sim)
{
    if (sim.pause > 0.0f) {
        return sim.pause;
    }
    if (sim.to < 0) {
        return 0.0f;
    }
    const float travel = group.keyframes->rotate_in_place ? spin_timing(group, path, sim).travel
                                                          : segment_timing(group, path, sim).travel;
    return std::max(travel - sim.t, 0.0f);
}

void finish_phase(const GroupEntry& group, const GroupPath& path, Sim& sim)
{
    if (sim.pause > 0.0f) {
        sim.pause = 0.0f;
        return;
    }
    if (group.keyframes->rotate_in_place) {
        sim.t = 0.0f;
        const mp::MoveType type = group_move_type(group);
        if (type == mp::MoveType::loop_infinite) {
            return;
        }
        if (sim.forward) {
            sim.pause = path.keyframes[0]->pause_time;
        }
        sim.forward = !sim.forward;
        const bool returns = type == mp::MoveType::ping_pong_once || type == mp::MoveType::loop_once;
        if (type == mp::MoveType::one_way || (returns && sim.last_leg)) {
            sim.to = -1;
            sim.last_leg = false;
        }
        else if (returns) {
            sim.last_leg = true;
        }
        return;
    }
    const int cur = sim.to;
    const mp::MoveType type = group_move_type(group);
    const mp::NextStep step = mp::next_keyframe(type, path.count(), sim.from, cur, sim.forward);
    sim.from = cur;
    sim.to = step.next;
    sim.forward = step.forward;
    sim.t = 0.0f;
    // A one-way run stops at its end without its pause
    sim.pause = type == mp::MoveType::one_way && step.next < 0 ? 0.0f : path.keyframes[cur]->pause_time;
}

constexpr int max_phases = 4096;

void advance_sim(const GroupEntry& group, const GroupPath& path, Sim& sim, float dt)
{
    for (int i = 0; i < max_phases && (sim.to >= 0 || sim.pause > 0.0f); i++) {
        const float left = phase_left(group, path, sim);
        if (dt < left) {
            if (sim.pause > 0.0f) {
                sim.pause -= dt;
            }
            else {
                sim.t += dt;
            }
            return;
        }
        dt -= left;
        finish_phase(group, path, sim);
    }
}

void activate_sim(const GroupEntry& group, const GroupPath& path, Sim& sim)
{
    if (sim.to >= 0) return;
    const int count = path.count();
    if (group.keyframes->rotate_in_place) {
        sim.to = 0;
    }
    else if (sim.forward) {
        sim.to = sim.from + 1 < count ? sim.from + 1 : 0;
    }
    else {
        sim.to = sim.from > 0 ? sim.from - 1 : count - 1;
    }
}

float measure_run(const GroupEntry& group, const GroupPath& path, Sim sim)
{
    const Sim start = sim;
    float total = 0.0f;
    for (int i = 0; i < max_phases && (sim.to >= 0 || sim.pause > 0.0f); i++) {
        total += phase_left(group, path, sim);
        finish_phase(group, path, sim);
        if (sim == start) break;
    }
    return total;
}

struct Preview
{
    GroupEntry* group = nullptr;
    HWND panel = nullptr;
    bool playing = false;
    float speed = 1.0f;
    Sim sim;
    Sim run_start;
    float run_time = 0.0f;
    float run_length = 0.0f;
    LARGE_INTEGER last_tick{};
};
Preview g_preview;

constexpr UINT_PTR preview_timer_id = 1;
constexpr UINT preview_timer_ms = 16;
constexpr int preview_slider_steps = 1000;

// The group may have been deleted, or its keyframes changed, since the last frame.
bool preview_valid(const CDedLevel& level, const GroupPath* path)
{
    const auto& groups = level.moving_groups;
    const auto end = groups.data_ptr + groups.size;
    if (!g_preview.group || std::find(groups.data_ptr, end, g_preview.group) == end
        || !g_preview.group->is_moving_group()) {
        return false;
    }
    if (!path) return true;
    const int count = path->count();
    const int needed = g_preview.group->keyframes->rotate_in_place ? 1 : 2;
    return count >= needed && g_preview.sim.from < count && g_preview.sim.to < count
        && g_preview.run_start.from < count && g_preview.run_start.to < count;
}

void preview_reset_sim(const GroupPath& path)
{
    const MovingGroupKeyframes& kfs = *g_preview.group->keyframes;
    Sim sim;
    sim.from = kfs.starting_keyframe > path.count() ? 0 : std::max(kfs.starting_keyframe - 1, 0);
    // Level load turns the option off for rotating movers
    sim.forward = !kfs.starts_backwards || kfs.rotate_in_place;
    g_preview.sim = sim;
    g_preview.run_start = sim;
    g_preview.run_time = 0.0f;
    g_preview.run_length = 0.0f;
}

void preview_start_run(const GroupPath& path)
{
    activate_sim(*g_preview.group, path, g_preview.sim);
    g_preview.run_start = g_preview.sim;
    g_preview.run_time = 0.0f;
    g_preview.run_length = measure_run(*g_preview.group, path, g_preview.sim);
}

void update_preview_panel()
{
    const HWND panel = g_preview.panel;
    if (!panel) return;
    SetDlgItemTextA(panel, IDC_MOVER_PREVIEW_PLAY, g_preview.playing ? "Pause" : "Play");
    const float fraction =
        g_preview.run_length > 0.0f ? std::clamp(g_preview.run_time / g_preview.run_length, 0.0f, 1.0f) : 0.0f;
    SendDlgItemMessageA(panel, IDC_MOVER_PREVIEW_SLIDER, TBM_SETPOS, TRUE,
                        static_cast<LPARAM>(fraction * static_cast<float>(preview_slider_steps)));
    const char* state = g_preview.sim.to >= 0 ? "Moving" : "Stopped";
    if (g_preview.sim.pause > 0.0f) {
        state = "Paused at keyframe";
    }
    char text[96];
    std::snprintf(text, sizeof(text), "%s: keyframe %d, %.2f / %.2f s", state, g_preview.sim.from + 1,
                  g_preview.run_time, g_preview.run_length);
    SetDlgItemTextA(panel, IDC_MOVER_PREVIEW_STATUS, text);
}

void preview_tick()
{
    CDedLevel* level = CDedLevel::Get();
    if (!level || !preview_valid(*level, nullptr)) {
        mover_path_preview_close();
        return;
    }
    LARGE_INTEGER now{};
    LARGE_INTEGER freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const float real_dt = static_cast<float>(now.QuadPart - g_preview.last_tick.QuadPart)
                        / static_cast<float>(freq.QuadPart);
    g_preview.last_tick = now;

    const GroupPath path = group_path(level->GetAlpineLevelProperties(), *g_preview.group);
    if (!preview_valid(*level, &path)) {
        mover_path_preview_close();
        return;
    }
    if (!g_preview.playing) return;

    const float dt = std::min(real_dt, 0.1f) * g_preview.speed;
    advance_sim(*g_preview.group, path, g_preview.sim, dt);
    g_preview.run_time += dt;
    if (g_preview.sim.to < 0 && g_preview.sim.pause <= 0.0f) {
        g_preview.playing = false;
        g_preview.run_time = g_preview.run_length;
    }
    else if (g_preview.run_length > 0.0f && g_preview.run_time >= g_preview.run_length) {
        g_preview.run_time -= g_preview.run_length;
    }
    update_preview_panel();
    editor_views_mark_repaint_all();
}

void preview_scrub(int position)
{
    CDedLevel* level = CDedLevel::Get();
    if (!level || !preview_valid(*level, nullptr)) return;
    const GroupPath path = group_path(level->GetAlpineLevelProperties(), *g_preview.group);
    if (!preview_valid(*level, &path)) return;
    if (g_preview.sim.to < 0 && g_preview.run_length <= 0.0f) {
        preview_start_run(path);
    }
    g_preview.playing = false;
    g_preview.run_time = g_preview.run_length * static_cast<float>(position) / static_cast<float>(preview_slider_steps);
    g_preview.sim = g_preview.run_start;
    advance_sim(*g_preview.group, path, g_preview.sim, g_preview.run_time);
    update_preview_panel();
    editor_views_mark_repaint_all();
}

void preview_play_pause()
{
    CDedLevel* level = CDedLevel::Get();
    if (!level || !preview_valid(*level, nullptr)) return;
    const GroupPath path = group_path(level->GetAlpineLevelProperties(), *g_preview.group);
    if (!preview_valid(*level, &path)) return;
    g_preview.playing = !g_preview.playing;
    if (g_preview.playing && g_preview.sim.to < 0 && g_preview.sim.pause <= 0.0f) {
        preview_start_run(path);
    }
    QueryPerformanceCounter(&g_preview.last_tick);
    update_preview_panel();
}

void preview_stop()
{
    CDedLevel* level = CDedLevel::Get();
    if (!level || !preview_valid(*level, nullptr)) return;
    const GroupPath path = group_path(level->GetAlpineLevelProperties(), *g_preview.group);
    g_preview.playing = false;
    preview_reset_sim(path);
    update_preview_panel();
    editor_views_mark_repaint_all();
}

INT_PTR CALLBACK MoverPreviewProc(HWND hdlg, UINT msg, WPARAM wparam, LPARAM)
{
    switch (msg) {
        case WM_INITDIALOG:
            fill_combo(hdlg, IDC_MOVER_PREVIEW_SPEED, {"0.25x", "0.5x", "1x", "2x"}, 2);
            g_preview.speed = 1.0f;
            SendDlgItemMessageA(hdlg, IDC_MOVER_PREVIEW_SLIDER, TBM_SETRANGE, TRUE,
                                MAKELPARAM(0, preview_slider_steps));
            SetTimer(hdlg, preview_timer_id, preview_timer_ms, nullptr);
            return TRUE;
        case WM_TIMER:
            if (wparam == preview_timer_id) {
                preview_tick();
            }
            return TRUE;
        case WM_HSCROLL:
            if (LOWORD(wparam) != TB_ENDTRACK) {
                preview_scrub(static_cast<int>(
                    SendDlgItemMessageA(hdlg, IDC_MOVER_PREVIEW_SLIDER, TBM_GETPOS, 0, 0)));
            }
            return TRUE;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case IDC_MOVER_PREVIEW_PLAY:
                    preview_play_pause();
                    return TRUE;
                case IDC_MOVER_PREVIEW_STOP:
                    preview_stop();
                    return TRUE;
                case IDC_MOVER_PREVIEW_SPEED:
                    if (HIWORD(wparam) == CBN_SELCHANGE) {
                        constexpr float speeds[] = {0.25f, 0.5f, 1.0f, 2.0f};
                        const int index = combo_selection(hdlg, IDC_MOVER_PREVIEW_SPEED);
                        if (index >= 0 && index < 4) {
                            g_preview.speed = speeds[index];
                        }
                    }
                    return TRUE;
                case IDCANCEL:
                    mover_path_preview_close();
                    return TRUE;
                default:
                    break;
            }
            break;
        case WM_CLOSE:
            mover_path_preview_close();
            return TRUE;
        default:
            break;
    }
    return FALSE;
}

mp::Mat3 to_mat3(const Matrix3& m)
{
    return {to_path(m.rvec), to_path(m.uvec), to_path(m.fvec)};
}

Matrix3 to_matrix(const mp::Mat3& m)
{
    Matrix3 out = identity_orient;
    out.rvec = to_world(m.rvec);
    out.uvec = to_world(m.uvec);
    out.fvec = to_world(m.fvec);
    return out;
}

mp::Vec3 rotated_about(const mp::Vec3& v, const mp::Vec3& axis, float angle)
{
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return v * c + mp::cross(axis, v) * s + axis * (mp::dot(axis, v) * (1.0f - c));
}

// Stock's orient from a forward vector (0x004FCEA0), which differs from mp::facing straight up or down
mp::Mat3 stock_forward_orient(const mp::Vec3& dir)
{
    constexpr float vertical = 1.0e-4f;
    const mp::Vec3 f = mp::normalized(dir);
    if (std::abs(f.x) < vertical && std::abs(f.z) < vertical) {
        return f.y < 0.0f ? mp::Mat3{{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, -1.0f, 0.0f}}
                          : mp::Mat3{{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}};
    }
    return mp::facing(f);
}

struct GhostPose
{
    Vector3 kf0;
    Vector3 now;
    bool turns = false;
    mp::Mat3 from;
    mp::Mat3 to;
    bool keep_orient = false;
    bool aimed = false;
    mp::Mat3 aim;

    void place(const Vector3& rest_pos, const Matrix3& rest_orient, Vector3& pos, Matrix3& orient) const
    {
        pos = rest_pos + (now - kf0);
        orient = rest_orient;
        if (turns) {
            const mp::Vec3 rel = to_path(rest_pos - kf0);
            pos = now + to_world(mp::carry(from, to, rel));
            if (!keep_orient) {
                orient = to_matrix(mp::carry(from, to, to_mat3(rest_orient)));
            }
        }
        if (aimed) {
            orient = to_matrix(aim);
        }
    }
};

GhostPose ghost_pose(const AlpineLevelProperties& props, const GroupEntry& group, const GroupPath& path)
{
    const Sim& sim = g_preview.sim;
    GhostPose ghost;
    ghost.kf0 = path.keyframes[0]->pos;
    ghost.now = ghost.kf0;

    if (group.keyframes->rotate_in_place) {
        const SegmentTiming seg = spin_timing(group, path, sim);
        const float s = sim.to < 0 ? 0.0f : mp::segment_progress(sim.t, seg.travel, seg.accel, seg.decel, seg.ease);
        constexpr float deg_to_rad = 3.14159265f / 180.0f;
        const float full = path.keyframes[0]->rotation_degrees * deg_to_rad;
        const float angle = (sim.forward ? s : 1.0f - s) * full;
        const mp::Vec3 axis = mp::normalized(to_path(path.keyframes[0]->orient.uvec));
        ghost.turns = true;
        ghost.keep_orient = group.keyframes->force_orient;
        ghost.to = {rotated_about({1.0f, 0.0f, 0.0f}, axis, angle), rotated_about({0.0f, 1.0f, 0.0f}, axis, angle),
                    rotated_about({0.0f, 0.0f, 1.0f}, axis, angle)};
        return ghost;
    }

    float s = 0.0f;
    SegmentTiming seg;
    if (sim.to >= 0) {
        seg = segment_timing(group, path, sim);
        s = mp::segment_progress(sim.t, seg.travel, seg.accel, seg.decel, seg.ease);
        const mp::Vec3 at = seg.curved
            ? mp::sample(seg.curve, mp::param_at_fraction(seg.curve, seg.owner == sim.from ? s : 1.0f - s))
            : path.keys[sim.from].pos + (path.keys[sim.to].pos - path.keys[sim.from].pos) * s;
        ghost.now = to_world(at);
    }
    else {
        ghost.now = path.keyframes[sim.from]->pos;
    }

    const uint8_t mode = group_orient_mode(props, group);
    if (mode == static_cast<uint8_t>(mp::OrientMode::keyframe_rotation)) {
        ghost.turns = true;
        ghost.from = to_mat3(path.keyframes[0]->orient);
        const mp::Mat3 a = to_mat3(path.keyframes[sim.from]->orient);
        ghost.to = sim.to >= 0 ? mp::slerp(a, to_mat3(path.keyframes[sim.to]->orient), s) : a;
    }
    else if (mode == static_cast<uint8_t>(mp::OrientMode::follow_path)) {
        auto direction = [&](const mp::PathSpot& spot) {
            const bool curved = spot.owner >= 0 && path.segment_curved(spot.owner);
            const mp::Curve curve = curved ? mp::make_segment_curve(path.keys, spot.owner) : mp::Curve{};
            return mp::follow_direction(spot, sim.from, sim.to, path.count(), curved ? &curve : nullptr,
                                        [&](int i) { return path.keys[i].pos; });
        };
        ghost.turns = true;
        ghost.from = mp::facing(direction({0, 0.0f}));
        ghost.to = mp::facing(direction(mp::path_spot(sim.from, sim.to, s, path.count(), path.closing_segment)));
    }
    else if (group.keyframes->force_orient && sim.to >= 0) {
        ghost.aimed = true;
        ghost.aim = stock_forward_orient(path.keys[sim.to].pos - path.keys[sim.from].pos);
    }
    return ghost;
}

void draw_ghost_object(DedObject& obj, const GhostPose& ghost)
{
    constexpr float marker_radius = 0.5f;
    Vector3 pos;
    Matrix3 orient;
    ghost.place(obj.pos, obj.orient, pos, orient);
    if (auto* vmesh = static_cast<EditorVMesh*>(obj.vmesh)) {
        Vector3 center;
        float radius = 0.0f;
        vmesh_get_bound_sphere(vmesh, &center, &radius);
        set_draw_color(0xff, 0xff, 0xff, 0xff);
        EditorRenderParams params = editor_mesh_render_params();
        room_setup(nullptr, &pos, radius, 1, 1);
        vmesh_render(vmesh, &pos, &orient, &params);
        room_cleanup();
        return;
    }
    set_draw_color(0xff, 0xff, 0x00, 0xff);
    draw_wireframe_sphere_3d(&pos, marker_radius, editor_line_mode());
}

// The preview draws the mover's brushes at their simulated pose, so the originals would be a second copy.
CallHook<void __fastcall(GSolid*, int, char, int, char, const Vector3*, const Matrix3*, int, int, int)>
    unselected_mover_brush_draw_hook{
    0x0041EC14,
    [](GSolid* solid, int edx, char outline, int face_outlines, char vertex_marks, const Vector3* pos,
       const Matrix3* orient, int r, int g, int b) FASTCALL_LAMBDA {
        const auto* brush =
            reinterpret_cast<const BrushNode*>(reinterpret_cast<const char*>(pos) - offsetof(BrushNode, pos));
        if (!mover_path_preview_hides(*brush)) {
            unselected_mover_brush_draw_hook.call_target(solid, edx, outline, face_outlines, vertex_marks, pos, orient,
                                                         r, g, b);
        }
    },
};

} // namespace

void mover_path_serialize_chunk(CDedLevel& level, rf::File& file)
{
    const AlpineLevelProperties& props = level.GetAlpineLevelProperties();
    std::vector<MoverRecord> movers;
    std::vector<KeyframeRecord> keyframes;
    const auto& groups = level.moving_groups;
    for (int i = 0; i < groups.size; i++) {
        const GroupEntry* group = groups.data_ptr[i];
        if (!group || !group->is_moving_group()) continue;
        const uint8_t orient_mode = group_orient_mode(props, *group);
        if (orient_mode != static_cast<uint8_t>(mp::OrientMode::none)) {
            movers.push_back({first_active_keyframe_uid(*group), orient_mode});
        }
        const auto& objects = group->keyframes->objects;
        for (int j = 0; j < objects.size; j++) {
            const auto* kf = static_cast<const DedKeyframe*>(objects.data_ptr[j]);
            if (!kf || !kf->active) continue;
            const mp::KeyframeData data = mover_path_keyframe_data(*kf);
            if (!mp::is_default(data)) {
                keyframes.push_back({kf->uid, data});
            }
        }
    }
    if (movers.empty() && keyframes.empty()) return;
    auto start_pos = level.BeginRflSection(file, alpine_mover_path_chunk_id);
    write_chunk_body(file, movers, keyframes);
    level.EndRflSection(file, start_pos);
}

void mover_path_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len)
{
    AlpineLevelProperties& props = level.GetAlpineLevelProperties();
    std::size_t remaining = chunk_len;
    rf::File::ChunkGuard chunk_guard{file, remaining};
    RflChunkReader<rf::File> reader{file, remaining};
    const bool complete = mp::read_chunk(
        reader,
        [&](int32_t uid, uint8_t orient_mode) {
            if (orient_mode != static_cast<uint8_t>(mp::OrientMode::none)) {
                props.mover_path_orient_modes[uid] = orient_mode;
            }
        },
        [&](int32_t uid, const mp::KeyframeData& data) { props.mover_path_keyframes[uid] = data; });
    if (!complete) {
        xlog::warn("[MoverPath] Mover path chunk is damaged (len={})", chunk_len);
    }
}

void mover_path_group_save_begin()
{
    g_group_save_groups.clear();
}

void mover_path_group_saved(const GroupEntry* group)
{
    if (group && group->is_moving_group()) {
        g_group_save_groups.push_back(group);
    }
}

void mover_path_group_serialize_chunk(CDedLevel& level, rf::File& file)
{
    const AlpineLevelProperties& props = level.GetAlpineLevelProperties();
    std::vector<MoverRecord> movers;
    std::vector<KeyframeRecord> keyframes;
    int32_t base = 0;
    for (const GroupEntry* group : g_group_save_groups) {
        const auto& objects = group->keyframes->objects;
        bool first_active = true;
        for (int j = 0; j < objects.size; j++) {
            const auto* kf = static_cast<const DedKeyframe*>(objects.data_ptr[j]);
            if (!kf || !kf->active) continue;
            if (first_active) {
                first_active = false;
                const uint8_t orient_mode = group_orient_mode(props, *group);
                if (orient_mode != static_cast<uint8_t>(mp::OrientMode::none)) {
                    movers.push_back({base + j, orient_mode});
                }
            }
            const mp::KeyframeData data = mover_path_keyframe_data(*kf);
            if (!mp::is_default(data)) {
                keyframes.push_back({base + j, data});
            }
        }
        base += objects.size;
    }
    g_group_save_groups.clear();
    if (movers.empty() && keyframes.empty()) return;
    file.write<int32_t>(alpine_mover_path_chunk_id);
    file.write<int32_t>(chunk_body_size(movers, keyframes));
    write_chunk_body(file, movers, keyframes);
}

void mover_path_group_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len, int first_new_group)
{
    AlpineLevelProperties& props = level.GetAlpineLevelProperties();
    std::vector<DedObject*> imported;
    const auto& groups = level.moving_groups;
    for (int i = first_new_group; i < groups.size; i++) {
        const GroupEntry* group = groups.data_ptr[i];
        if (!group || !group->is_moving_group()) continue;
        const auto& objects = group->keyframes->objects;
        imported.insert(imported.end(), objects.data_ptr, objects.data_ptr + objects.size);
    }
    auto uid_at = [&](int32_t index) {
        const bool valid = index >= 0 && index < static_cast<int32_t>(imported.size()) && imported[index];
        return valid ? imported[index]->uid : -1;
    };

    std::size_t remaining = chunk_len;
    rf::File::ChunkGuard chunk_guard{file, remaining};
    RflChunkReader<rf::File> reader{file, remaining};
    const bool complete = mp::read_chunk(
        reader,
        [&](int32_t index, uint8_t orient_mode) {
            if (const int uid = uid_at(index); uid != -1 && orient_mode != 0) {
                props.mover_path_orient_modes[uid] = orient_mode;
            }
        },
        [&](int32_t index, const mp::KeyframeData& data) {
            if (const int uid = uid_at(index); uid != -1) {
                props.mover_path_keyframes[uid] = data;
            }
        });
    if (!complete) {
        xlog::warn("[MoverPath] The group's mover path chunk is damaged (len={})", chunk_len);
    }
}

mp::KeyframeData mover_path_keyframe_data(const DedObject& keyframe)
{
    const auto& keyframes = CDedLevel::Get()->GetAlpineLevelProperties().mover_path_keyframes;
    const auto it = keyframes.find(keyframe.uid);
    return it != keyframes.end() ? it->second : mp::KeyframeData{};
}

void mover_path_set_keyframe_data(const DedObject& keyframe, const mp::KeyframeData& data)
{
    auto& keyframes = CDedLevel::Get()->GetAlpineLevelProperties().mover_path_keyframes;
    if (data == mp::KeyframeData{}) {
        keyframes.erase(keyframe.uid);
    }
    else {
        keyframes[keyframe.uid] = data;
    }
}

void mover_path_keyframe_mirrored(const DedObject& keyframe, int axis)
{
    mp::KeyframeData data = mover_path_keyframe_data(keyframe);
    auto mirror = [axis](mp::Vec3& v) {
        (axis == 0 ? v.x : axis == 1 ? v.y : v.z) *= -1.0f;
    };
    mirror(data.in_handle);
    mirror(data.out_handle);
    mover_path_set_keyframe_data(keyframe, data);
}

// Stock reissues a deleted object's UID once the undo stacks let go of it, which must not revive its path data.
void mover_path_uid_generated(int uid)
{
    AlpineLevelProperties& props = CDedLevel::Get()->GetAlpineLevelProperties();
    props.mover_path_keyframes.erase(uid);
    props.mover_path_orient_modes.erase(uid);
}

GroupEntry* find_moving_group_from_selection()
{
    auto* level = CDedLevel::Get();
    if (!level || level->selection.size <= 0 || !level->selection[0]) {
        return nullptr;
    }

    // When the keyframe properties dialog opens, the group's objects/keyframes are in the selection.
    DedObject* selected = level->selection[0];
    if (GroupEntry* group = moving_group_of(*level, selected)) {
        return group;
    }
    auto& groups = level->moving_groups;
    for (int i = 0; i < groups.size; i++) {
        GroupEntry* group = groups[i];
        if (!group || !group->is_moving_group()) continue;
        for (int j = 0; j < group->objects.size; j++) {
            if (group->objects[j] == selected) {
                return group;
            }
        }
    }
    return nullptr;
}

void mover_path_dialog_init(HWND hdlg)
{
    CDedLevel* level = CDedLevel::Get();
    if (!level) return;
    const AlpineLevelProperties& props = level->GetAlpineLevelProperties();
    const std::vector<DedKeyframe*> keyframes = selected_keyframes(*level);

    const int segment = shared_value(keyframes, [](const DedKeyframe& kf) {
        return static_cast<int>(mover_path_keyframe_data(kf).segment_mode);
    });
    const int tangent = shared_value(keyframes, [](const DedKeyframe& kf) {
        return static_cast<int>(mover_path_keyframe_data(kf).tangent_mode);
    });
    const int ease = shared_value(keyframes, [](const DedKeyframe& kf) {
        return static_cast<int>(mover_path_keyframe_data(kf).ease);
    });
    const int orient = shared_value(keyframes, [&](const DedKeyframe& kf) {
        const GroupEntry* group = moving_group_of(*level, &kf);
        return group ? static_cast<int>(group_orient_mode(props, *group)) : 0;
    });

    fill_combo(hdlg, IDC_KF_PATH_SEGMENT, {"Straight", "Curved"}, segment);
    fill_combo(hdlg, IDC_KF_PATH_TANGENT, {"Auto", "Aligned", "Free"}, tangent);
    fill_combo(hdlg, IDC_KF_PATH_EASE, {"None", "Ease In", "Ease Out", "Ease In and Out"}, ease);
    fill_combo(hdlg, IDC_KF_PATH_ORIENT, {"None", "Follow Path", "Keyframe Rotation"}, orient);
    update_dialog_enables(hdlg);
}

void mover_path_dialog_command(HWND hdlg, int id)
{
    if (id == IDC_KF_ROTATE_IN_PLACE) {
        update_dialog_enables(hdlg);
    }
}

void mover_path_dialog_ok(HWND hdlg)
{
    CDedLevel* level = CDedLevel::Get();
    if (!level) return;
    AlpineLevelProperties& props = level->GetAlpineLevelProperties();
    const int segment = combo_selection(hdlg, IDC_KF_PATH_SEGMENT);
    const int tangent = combo_selection(hdlg, IDC_KF_PATH_TANGENT);
    const int ease = combo_selection(hdlg, IDC_KF_PATH_EASE);
    const int orient = combo_selection(hdlg, IDC_KF_PATH_ORIENT);

    bool changed = false;
    const std::vector<DedKeyframe*> keyframes = selected_keyframes(*level);
    for (DedKeyframe* kf : keyframes) {
        const mp::KeyframeData before = mover_path_keyframe_data(*kf);
        mp::KeyframeData data = before;
        if (segment >= 0) {
            data.segment_mode = static_cast<uint8_t>(segment);
        }
        if (ease >= 0) {
            data.ease = static_cast<uint8_t>(ease);
        }
        if (!(data == before)) {
            mover_path_set_keyframe_data(*kf, data);
            changed = true;
        }
    }
    for (DedKeyframe* kf : keyframes) {
        mp::KeyframeData data = mover_path_keyframe_data(*kf);
        if (tangent >= 0 && data.tangent_mode != static_cast<uint8_t>(tangent)) {
            // Leaving Auto keeps the shape: the handles start where Auto, with this OK's segments, has them.
            GroupEntry* group = moving_group_of(*level, kf);
            if (group && data.tangent_mode == static_cast<uint8_t>(mp::TangentMode::automatic)) {
                const GroupPath path = group_path(props, *group);
                const auto it = std::find(path.keyframes.begin(), path.keyframes.end(), kf);
                if (it != path.keyframes.end()) {
                    const mp::KeyframeData& resolved = path.keys[it - path.keyframes.begin()].data;
                    data.in_handle = resolved.in_handle;
                    data.out_handle = resolved.out_handle;
                }
            }
            data.tangent_mode = static_cast<uint8_t>(tangent);
            mover_path_set_keyframe_data(*kf, data);
            changed = true;
        }
        if (GroupEntry* group = moving_group_of(*level, kf); group && orient >= 0) {
            const int uid = first_active_keyframe_uid(*group);
            const uint8_t mode = static_cast<uint8_t>(orient);
            if (group_orient_mode(props, *group) != mode) {
                if (mode == static_cast<uint8_t>(mp::OrientMode::none)) {
                    props.mover_path_orient_modes.erase(uid);
                }
                else {
                    props.mover_path_orient_modes[uid] = mode;
                }
                changed = true;
            }
        }
    }
    if (changed) {
        redraw_all_viewports();
    }
}

bool mover_path_view_lbutton_down(EditorViewport* view, UINT flags, int x, int y)
{
    try {
        cancel_handle_drag();
        CDedLevel* level = CDedLevel::Get();
        const HWND main = GetMainFrameHandle();
        if (!level || !editor_is_view(view) || !main || !IsWindowEnabled(main)) return false;
        if ((flags & (MK_CONTROL | MK_SHIFT | MK_RBUTTON | MK_MBUTTON)) || GetKeyState(VK_MENU) < 0) return false;
        if (view->rbutton_held || view->mbutton_held || view->lbutton_held || editor_transform_key_held()
            || level->transform_in_progress || level->transform_mode != 0) {
            return false;
        }
        HandleRef handle;
        if (!find_handle(*level, view, x, y, handle)) return false;

        HandleDrag d;
        d.active = true;
        d.handle = handle;
        d.view = view;
        d.hwnd = editor_view_hwnd(view);
        d.press = {x, y};
        d.before = mover_path_keyframe_data(*handle.keyframe);
        const Vector3 p = handle_pos(*handle.keyframe, handle.resolved, handle.out);
        const EditorRay ray = editor_screen_ray(static_cast<float>(x), static_cast<float>(y));
        d.plane_point = p;
        d.plane_normal = ray.d;
        const float depth = dot(p - ray.o, ray.d);
        d.grab_offset = p - (ray.o + ray.d * depth);
        g_handle_drag = d;
        SetCapture(d.hwnd);
        return true;
    }
    catch (const std::exception& e) {
        xlog::error("[MoverPath] {}", e.what());
        return false;
    }
}

void mover_path_view_lbutton_up()
{
    HandleDrag& d = g_handle_drag;
    if (!d.active) return;
    try {
        update_handle_drag();
        const mp::KeyframeData after = mover_path_keyframe_data(*d.handle.keyframe);
        DedKeyframe* keyframe = d.handle.keyframe;
        const mp::KeyframeData before = d.before;
        const bool started = d.started;
        end_handle_drag();
        if (started && !(after == before)) {
            gizmo_commit_keyframe_path(*CDedLevel::Get(), *keyframe, before, after);
        }
    }
    catch (const std::exception& e) {
        xlog::error("[MoverPath] {}", e.what());
        cancel_handle_drag();
    }
}

bool mover_path_view_rbutton_down()
{
    if (!g_handle_drag.active) return false;
    cancel_handle_drag();
    return true;
}

bool mover_path_view_mbutton_down()
{
    return g_handle_drag.active;
}

bool mover_path_dragging()
{
    return g_handle_drag.active;
}

bool mover_path_holds_view_focus()
{
    return g_handle_drag.active
        || (g_preview.panel
            && SendDlgItemMessageA(g_preview.panel, IDC_MOVER_PREVIEW_SPEED, CB_GETDROPPEDSTATE, 0, 0));
}

void mover_path_cancel_drag()
{
    cancel_handle_drag();
}

void mover_path_idle()
{
    try {
        if (!g_handle_drag.active) {
            hover_tick();
            return;
        }
        if (g_dinput_keys[DIK_ESCAPE] || GetCapture() != g_handle_drag.hwnd) {
            cancel_handle_drag();
            return;
        }
        update_handle_drag();
    }
    catch (const std::exception& e) {
        xlog::error("[MoverPath] {}", e.what());
        cancel_handle_drag();
    }
}

void mover_path_level_reset()
{
    if (g_handle_drag.active && GetCapture() == g_handle_drag.hwnd) {
        ReleaseCapture();
    }
    g_handle_drag = {};
    g_hovered_handle = {};
    g_group_save_groups.clear();
    mover_path_preview_close();
}

void mover_path_preview_open()
{
    CDedLevel* level = CDedLevel::Get();
    if (!level) return;
    GroupEntry* group = find_moving_group_from_selection();
    if (!group) {
        for (BrushNode* brush : level->selected_brushes()) {
            group = moving_group_of_brush(*level, brush);
            if (group) break;
        }
    }
    if (!group) {
        editor_report(EditorReportLevel::warn, "MoverPath",
                      "Select a keyframe or a member of a moving group to preview it.", true);
        return;
    }
    const AlpineLevelProperties& props = level->GetAlpineLevelProperties();
    const GroupPath path = group_path(props, *group);
    if (path.count() < (group->keyframes->rotate_in_place ? 1 : 2)) {
        editor_report(EditorReportLevel::warn, "MoverPath",
                      "This moving group does not have enough keyframes to preview.", true);
        return;
    }

    g_preview.group = group;
    g_preview.playing = false;
    preview_reset_sim(path);
    if (!g_preview.panel) {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES};
        InitCommonControlsEx(&icc);
        g_preview.panel = CreateDialogParamA(reinterpret_cast<HINSTANCE>(&__ImageBase),
                                             MAKEINTRESOURCEA(IDD_ALPINE_MOVER_PREVIEW), GetMainFrameHandle(),
                                             MoverPreviewProc, 0);
        if (!g_preview.panel) {
            xlog::error("[MoverPath] the preview panel could not be created ({})", GetLastError());
            g_preview.group = nullptr;
            return;
        }
    }
    const std::string title = std::string{"Mover Preview: "} + group->name.c_str();
    SetWindowTextA(g_preview.panel, title.c_str());
    ShowWindow(GetDlgItem(g_preview.panel, IDC_MOVER_PREVIEW_LEGACY), props.legacy_movers ? SW_SHOW : SW_HIDE);
    ShowWindow(g_preview.panel, SW_SHOW);
    SetForegroundWindow(g_preview.panel);
    update_preview_panel();
    editor_views_mark_repaint_all();
}

void mover_path_preview_close()
{
    const bool active = g_preview.group != nullptr;
    const HWND panel = g_preview.panel;
    g_preview.group = nullptr;
    g_preview.panel = nullptr;
    g_preview.playing = false;
    if (panel) {
        KillTimer(panel, preview_timer_id);
        DestroyWindow(panel);
    }
    if (active && g_main_frame) {
        editor_views_mark_repaint_all();
    }
}

bool mover_path_preview_shown()
{
    return g_preview.panel != nullptr;
}

bool mover_path_preview_hides(const BrushNode& brush)
{
    const CDedLevel* level = CDedLevel::Get();
    return level && preview_valid(*level, nullptr) && group_has_brush(*g_preview.group, &brush);
}

void mover_path_render(CDedLevel* level)
{
    if (!preview_valid(*level, nullptr)) return;
    const AlpineLevelProperties& props = level->GetAlpineLevelProperties();
    const GroupEntry& group = *g_preview.group;
    const GroupPath path = group_path(props, group);
    if (!preview_valid(*level, &path)) return;

    const GhostPose ghost = ghost_pose(props, group, path);
    const auto* view = static_cast<const EditorViewport*>(editor_view_at(painting_view_index));
    const bool lines = !view || view->view_type != editor_view_type_perspective;
    const EditorColorPrefs* prefs = editor_color_prefs();
    const Color mover_color = prefs ? prefs->color(EditorColorPrefIndex::mover_brush) : Color{255, 255, 0};
    for (int i = 0; i < group.brushes.size; i++) {
        BrushNode* brush = group.brushes.data_ptr[i];
        auto* geometry = brush ? static_cast<GSolid*>(brush->geometry) : nullptr;
        if (!geometry) continue;
        Vector3 pos;
        Matrix3 orient;
        ghost.place(brush->pos, brush->orient, pos, orient);
        if (lines) {
            geometry->draw(true, false, false, &pos, &orient, mover_color.r, mover_color.g, mover_color.b);
        }
        else {
            geometry->draw(false, false, false, &pos, &orient, 255, 255, 255);
        }
    }
    for (int i = 0; i < group.objects.size; i++) {
        if (DedObject* obj = group.objects.data_ptr[i]) {
            draw_ghost_object(*obj, ghost);
        }
    }
}

void ApplyMoverPathPatches()
{
    draw_keyframes_hook.install();
    keyframe_link_injection.install();
    unselected_mover_brush_draw_hook.install();
}
