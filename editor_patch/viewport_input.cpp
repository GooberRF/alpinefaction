#include <windows.h>
#include <patch_common/CallHook.h>
#include <patch_common/MemUtils.h>
#include "gizmo.h"
#include "placement_panel.h"
#include "terrain_paint.h"
#include "viewport_input.h"
#include "vtypes.h"

namespace
{

// The view message map (0x0055C170): AFX_MSGMAP_ENTRY pfn slots of the mouse button messages, each a thiscall
// (UINT flags, CPoint point) handler that returns 0xC bytes. The stock handlers only latch state that RED polls
// from its idle loop.
constexpr uintptr_t msgmap_lbutton_down_pfn = 0x0055C19C;
constexpr uintptr_t msgmap_lbutton_up_pfn = 0x0055C1B4;
constexpr uintptr_t msgmap_mbutton_down_pfn = 0x0055C1CC;
constexpr uintptr_t msgmap_mbutton_up_pfn = 0x0055C1E4;
constexpr uintptr_t msgmap_rbutton_down_pfn = 0x0055C1FC;
constexpr uintptr_t msgmap_rbutton_up_pfn = 0x0055C214;
constexpr uintptr_t msgmap_rbutton_dblclk_pfn = 0x0055C25C;
constexpr uintptr_t msgmap_lbutton_dblclk_pfn = 0x0055C274;
constexpr uintptr_t msgmap_mbutton_dblclk_pfn = 0x0055C28C;

enum Button
{
    button_left,
    button_right,
    button_middle,
    button_count,
};

// A feature kept the last press of the button, so its release is kept from stock too, in whichever view it lands:
// a drag cancelled mid-press releases capture, and stock's release would click-select (0x0047CFA0) or, for Ctrl +
// right, move the camera (0x0047D469). Any press stock gets clears it.
bool g_kept_press[button_count] = {};

bool keep_press(Button button, EditorViewport* view, UINT flags, int x, int y)
{
    bool kept = false;
    switch (button) {
        case button_left:
            kept = terrain_paint_view_lbutton_down(view, flags) || gizmo_view_lbutton_down(view, flags, x, y);
            break;
        case button_right:
            kept = gizmo_view_rbutton_down(view);
            break;
        case button_middle:
            kept = gizmo_view_mbutton_down(view);
            break;
        default:
            break;
    }
    g_kept_press[button] = kept;
    return kept;
}

bool keep_release(Button button, EditorViewport* view)
{
    if (!g_kept_press[button]) return false;
    g_kept_press[button] = false;
    if (button == button_left) {
        terrain_paint_view_lbutton_up(view);
        gizmo_view_lbutton_up(view);
    }
    return true;
}

// The second press of a double click arrives as the DBLCLK message instead of the button down.
void __fastcall view_lbutton_down(EditorViewport* view, void* /*edx*/, UINT flags, int x, int y)
{
    if (!keep_press(button_left, view, flags, x, y)) {
        view->on_lbutton_down(flags, x, y);
    }
}

void __fastcall view_lbutton_dblclk(EditorViewport* view, void* /*edx*/, UINT flags, int x, int y)
{
    if (!keep_press(button_left, view, flags, x, y)) {
        view->on_lbutton_dblclk(flags, x, y);
    }
}

void __fastcall view_lbutton_up(EditorViewport* view, void* /*edx*/, UINT flags, int x, int y)
{
    if (!keep_release(button_left, view)) {
        view->on_lbutton_up(flags, x, y);
    }
}

void __fastcall view_rbutton_down(EditorViewport* view, void* /*edx*/, UINT flags, int x, int y)
{
    if (!keep_press(button_right, view, flags, x, y)) {
        view->on_rbutton_down(flags, x, y);
    }
}

void __fastcall view_rbutton_dblclk(EditorViewport* view, void* /*edx*/, UINT flags, int x, int y)
{
    if (!keep_press(button_right, view, flags, x, y)) {
        view->on_rbutton_dblclk(flags, x, y);
    }
}

void __fastcall view_rbutton_up(EditorViewport* view, void* /*edx*/, UINT flags, int x, int y)
{
    if (!keep_release(button_right, view)) {
        view->on_rbutton_up(flags, x, y);
    }
}

void __fastcall view_mbutton_down(EditorViewport* view, void* /*edx*/, UINT flags, int x, int y)
{
    if (!keep_press(button_middle, view, flags, x, y)) {
        view->on_mbutton_down(flags, x, y);
    }
}

void __fastcall view_mbutton_dblclk(EditorViewport* view, void* /*edx*/, UINT flags, int x, int y)
{
    if (!keep_press(button_middle, view, flags, x, y)) {
        view->on_mbutton_dblclk(flags, x, y);
    }
}

void __fastcall view_mbutton_up(EditorViewport* view, void* /*edx*/, UINT flags, int x, int y)
{
    if (!keep_release(button_middle, view)) {
        view->on_mbutton_up(flags, x, y);
    }
}

// RED's idle loop focuses whichever view's rect holds the cursor, even under another window such as the Terrain
// Tools panel, which cancels that window's button clicks mid-press. Stock behaviour while it is closed. A drag out
// of the placement panel or a gizmo drag leaves the focus where it was until it ends.
void* __fastcall view_hover_focus(void* view, void* edx);
CallHook<void* __fastcall(void*, void*)> view_hover_focus_hook{0x004834E1, view_hover_focus};
void* __fastcall view_hover_focus(void* view, void* edx)
{
    if (placement_panel_dragging() || gizmo_holds_view_focus()) return nullptr;
    const HWND hwnd = editor_view_hwnd(view);
    POINT cursor{};
    if (terrain_paint_panel_open() && hwnd && GetCursorPos(&cursor)) {
        const HWND over = WindowFromPoint(cursor);
        if (over && over != hwnd && !IsChild(hwnd, over)) return nullptr;
    }
    return view_hover_focus_hook.call_target(view, edx);
}

} // namespace

void ApplyViewportInputPatches()
{
    write_mem_ptr(msgmap_lbutton_down_pfn, &view_lbutton_down);
    write_mem_ptr(msgmap_lbutton_up_pfn, &view_lbutton_up);
    write_mem_ptr(msgmap_lbutton_dblclk_pfn, &view_lbutton_dblclk);
    write_mem_ptr(msgmap_rbutton_down_pfn, &view_rbutton_down);
    write_mem_ptr(msgmap_rbutton_up_pfn, &view_rbutton_up);
    write_mem_ptr(msgmap_rbutton_dblclk_pfn, &view_rbutton_dblclk);
    write_mem_ptr(msgmap_mbutton_down_pfn, &view_mbutton_down);
    write_mem_ptr(msgmap_mbutton_up_pfn, &view_mbutton_up);
    write_mem_ptr(msgmap_mbutton_dblclk_pfn, &view_mbutton_dblclk);
    view_hover_focus_hook.install();
}
