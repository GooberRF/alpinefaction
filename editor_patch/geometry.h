#pragma once

#include <vector>
#include <windows.h>

// Forward declarations
extern HMODULE g_module;
struct BrushNode;
struct CDedLevel;

// The selected brushes that have geometry, in brush list order.
std::vector<BrushNode*> collect_selected_brushes(CDedLevel* level);

// Face mode operations
void handle_face_delete();
void handle_face_delete_ext();
void handle_face_split();
void handle_face_flip_normal();

// Brush mode operations
void handle_brush_mirror();
void handle_brush_convert();

// Group mode operations
void handle_group_mirror();

// Vertex mode operations
void handle_vertex_delete();
void handle_vertex_bridge();

void ApplyGeometryPatches();
