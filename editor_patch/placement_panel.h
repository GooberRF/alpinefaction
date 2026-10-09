#pragma once

#include <windows.h>

struct EditorObjectPanel;

// The placement panel above the object mode tree: pick a clutter, entity, item, vehicle or mesh class, then
// double-click its preview to place one under the camera, or drag the preview into a viewport. Leaves of the
// object tree drag into the viewports the same way.

void ApplyPlacementPanelPatches();

// Builds the panel into the object panel's form view; from CFormView_PostCreate_injection.
void placement_panel_create(EditorObjectPanel* object_panel, HWND form);

// Draws the dragged object where releasing would place it; from the Alpine object pass of each viewport.
void placement_panel_render_ghost();

// A drag out of the panel's preview or the object tree is in progress.
bool placement_panel_dragging();
