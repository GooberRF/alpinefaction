#pragma once

struct CDedLevel;
struct GFace;

// Textured brush preview: brushes changed since the last Build (or being transformed in Brush or Group mode) drawn
// textured in the 3D view, in place of the level geometry built from them.

void ApplyBrushPreviewPatches();

// From the 3D view's paint, before the level: brings the level geometry faces the preview leaves out up to date.
void brush_preview_view_paint(CDedLevel& level);

// The level is being reset (File > New, File > Open, close).
void brush_preview_level_reset();

// A Build Geometry is starting; the preview holds as it is until it ends.
void brush_preview_build_started(CDedLevel& level);

// A Build Geometry finished, or was cancelled after numbering the brush faces again.
void brush_preview_build_ended(bool completed);

// A level geometry face the preview leaves out.
bool brush_preview_hides(const GFace& face);
