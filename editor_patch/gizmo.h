#pragma once

#include <windows.h>

struct EditorViewport;
struct UndoEntry;

// The viewport transform gizmo: the top bar's Select / Move / Rotate / Scale buttons, Snap and Scale step, hotkeys
// 1-5, the handles drawn over every view at a constant screen size, and handle drags, each recorded as one stock
// undo step when the button comes up.

void ApplyGizmoPatches();

// Run from RED's idle loop: top bar state, hotkeys, the hovered handle and the drag in progress.
void gizmo_idle();

// From the shared view dispatcher. A press the gizmo keeps (true) never reaches stock, nor does the release of
// that button, wherever it lands.
// With Shift, a press on a handle first duplicates the selection and drags the copies.
// A double click's second press counts as a press.
bool gizmo_view_lbutton_down(EditorViewport* view, UINT flags, int x, int y);
void gizmo_view_lbutton_up(EditorViewport* view);
// During a drag a right press cancels it and a middle press is dropped; otherwise both go to stock.
bool gizmo_view_rbutton_down(EditorViewport* view);
bool gizmo_view_mbutton_down(EditorViewport* view);

// A handle drag is in progress.
bool gizmo_dragging();
// RED's idle hover focus must leave the views alone: a drag holds capture, or the Scale step list is dropped down.
bool gizmo_holds_view_focus();
// Ends a drag in progress, putting back what it moved, with no undo step. False if that failed part way.
bool gizmo_cancel_drag();

// The top bar's gizmo buttons (ID_GIZMO_SELECT..ID_GIZMO_SNAP) and Scale step list, from CMainFrame::OnCmdMsg.
void gizmo_button_clicked(UINT id);
void gizmo_scale_step_changed();

// The level's Undo (undone) or Redo just applied `entry`: the Alpine state a gizmo drag recorded with it (mesh draw
// scale, region sizes, brush vertices, directional light box yaw, the sun's angles) follows.
void gizmo_undo_entry_applied(UndoEntry* entry, bool undone);
// A level was loaded: no undo entry of the old one is left to apply a side record to.
void gizmo_level_loaded();
