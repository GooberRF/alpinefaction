#pragma once

#include <windows.h>
#include <span>
#include "vtypes.h"

struct BrushNode;
struct CDedLevel;
struct GFace;
struct UndoEntry;

// Texture mode: the viewport room caches follow UV edits, the panel's Wrap, Fit and Justify, Cylinder and Sphere Map,
// Copy and Paste, the hovered face's readout, and UV Unwrap's tools and its rotation keeping the texture's aspect.

void ApplyTextureModePatches();

// The Texture mode side panel was just created from its template, with the panel's map type.
void texture_mode_panel_created(HWND hwnd, TextureMapType map_type);

// UV Unwrap is about to open as a modal dialog.
void uv_unwrap_dialog_opening();

// From RED's idle loop: in Texture mode, the face under the cursor (or the first selected one) in the status bar.
void texture_mode_idle();

// From the view paint, over everything: the hovered face's outline.
void texture_mode_render_view();

// The level geometry faces given (those with the brush face's id) lie on the brush face: it hasn't moved since it
// was built.
bool level_faces_lie_on_brush_face(BrushNode& brush, GFace& face, std::span<GFace* const> built);

// The level is being reset (File > New, File > Open, close).
void texture_mode_level_reset();

// An undo or redo just swapped an entry's brushes in: the level geometry built from brush faces whose textures (UVs,
// texture, Face Properties) it changed takes them again, as the edit had given them.
void texture_undo_entry_applied(CDedLevel& level, const UndoEntry& entry);
