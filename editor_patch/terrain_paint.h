#pragma once

struct CDedLevel;
struct DedTerrain;

// Terrain paint mode: the modeless Terrain Tools panel (layer, hole, height and geoable chunk tools), viewport input
// while it is open, the brush cursor and the per-terrain paint undo stacks.

void ApplyTerrainPaintPatches();

// Opens the panel on `terrain` (or retargets an open one).
void terrain_paint_open(CDedLevel* level, DedTerrain* terrain);
// Tools > Terrain Tools: the first selected terrain.
void terrain_paint_open_for_selection(CDedLevel* level);
// The panel's Properties... button: closes the panel, then opens Terrain Properties on its terrain.
void terrain_paint_show_properties(CDedLevel* level);

// The panel is open: left-drag in a viewport paints, and Undo/Redo act on paint strokes.
bool terrain_paint_active();

// Run from RED's idle loop (CEditorApp::OnIdle 0x00482F00, hooked in main.cpp).
void terrain_paint_idle();

// Reload Textures ran: the layer swatches are read again.
void terrain_paint_textures_reloaded();

// The brush cursor, drawn in the Alpine object pass of each viewport.
void terrain_paint_draw_cursor(CDedLevel& level);

// Paint undo / redo on the panel's terrain; no-ops when nothing applies.
void terrain_paint_undo();
void terrain_paint_redo();
bool terrain_paint_can_undo();
bool terrain_paint_can_redo();

// A left-drag paint stroke is in progress.
bool terrain_paint_stroke_active();

// Commits the stroke in progress on `terrain` (any terrain when null) as an undo step, so an edit
// that shares or replaces its grid (copy, properties dialog) never sees a half-owned stroke grid.
void terrain_paint_end_stroke(const DedTerrain* terrain);

// Called before a terrain is freed: drops its undo stack and closes the panel if it targets it.
void terrain_paint_forget(const DedTerrain* terrain);
