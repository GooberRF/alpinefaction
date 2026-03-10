#pragma once

#include <cstddef>
#include "mfc_types.h"
#include "level.h"

// Mesh serialization (called from level.cpp injection points)
void mesh_serialize_chunk(CDedLevel& level, rf::File& file);
void mesh_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len);

// Mesh property dialog
void ShowMeshPropertiesDialog(DedMesh* mesh);
void ShowMeshPropertiesForSelection(CDedLevel* level);

// Mesh object lifecycle
void PlaceNewMeshObject();
DedMesh* CloneMeshObject(DedMesh* source, bool add_to_level = true);
void DeleteMeshObject(DedMesh* mesh);
void DestroyDedMesh(DedMesh* mesh);

// Rendering (called from alpine_obj.cpp render hook)
void mesh_render(CDedLevel* level);

// Handlers called from shared hook points in alpine_obj.cpp
DedMesh* mesh_click_pick(CDedLevel* level, float click_x, float click_y, float* out_dist_sq);
void mesh_pick(CDedLevel* level, int param1, int param2);
void mesh_tree_populate(EditorTreeCtrl* tree, int master_groups, CDedLevel* level);
void mesh_tree_add_object_type(EditorTreeCtrl* tree);
bool mesh_copy_object(DedObject* source);
void mesh_paste_objects(CDedLevel* level);
void mesh_clear_clipboard();
void mesh_handle_delete_or_cut(DedObject* obj);
void mesh_handle_delete_selection(CDedLevel* level);
void mesh_ensure_uid(int& uid);
