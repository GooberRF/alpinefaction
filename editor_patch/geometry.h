#pragma once

#include <windows.h>

// Forward declarations
extern HMODULE g_module;

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
