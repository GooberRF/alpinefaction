#pragma once

#include <cstddef>
#include "mfc_types.h"
#include "level.h"

// Mesh serialization (called from level.cpp injection points)
void mesh_serialize_chunk(CDedLevel& level, rf::File& file);
void mesh_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len, int content_version);

// Mesh property dialog
void ShowMeshPropertiesDialog(DedMesh* mesh);
void ShowMeshPropertiesForSelection(CDedLevel* level);

// Mesh object lifecycle
void PlaceNewMeshObject();
DedMesh* CloneMeshObject(DedMesh* source, bool add_to_level = true);
void DeleteMeshObject(DedMesh* mesh);
void DestroyDedMesh(DedMesh* mesh);

// VMesh loading
struct EditorVMesh;
EditorVMesh* mesh_load_vmesh_file(const char* filename);
void mesh_load_vmesh(DedMesh* mesh);

// Looping playback of a one-shot .rfa action. An animation's own ramp_in_time/ramp_out_time
// envelope is exactly zero at both ends of its span, and a zero weighted action is left out of
// the pose blend entirely, which leaves the bone at its bind transform. So a frame must never be
// drawn at either endpoint: advance first, and when that advance finishes the action, restart and
// step past the seam before the caller paints.
void mesh_play_v3c_action_looping(EditorVMesh* vmesh, int action_index);
bool mesh_v3c_action_finished(EditorVMesh* vmesh);

// Stock v3d/v3m/v3c parser overflow hardening
void apply_mesh_parser_hardening();

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
