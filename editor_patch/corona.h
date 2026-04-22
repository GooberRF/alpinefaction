#pragma once

#include <cstddef>
#include <vector>
#include "mfc_types.h"
#include "level.h"

struct GlareClassInfo;

// Corona serialization (called from level.cpp injection points)
void corona_serialize_chunk(CDedLevel& level, rf::File& file);
void corona_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len);

// Corona property dialog
void ShowCoronaPropertiesDialog(CDedLevel* level);

// Corona object lifecycle
void PlaceNewCoronaObject();
DedCorona* CloneCoronaObject(DedCorona* source, bool add_to_level = true);
void DeleteCoronaObject(DedCorona* corona);
void DestroyDedCorona(DedCorona* corona);

// Handlers called from shared hook points in alpine_obj.cpp
void corona_render(CDedLevel* level);
void corona_pick(CDedLevel* level, int param1, int param2);
DedCorona* corona_click_pick(CDedLevel* level, float click_x, float click_y);
void corona_tree_populate(EditorTreeCtrl* tree, int master_groups, CDedLevel* level);
void corona_tree_add_object_type(EditorTreeCtrl* tree);
bool corona_copy_object(DedObject* source);
void corona_paste_objects(CDedLevel* level);
void corona_clear_clipboard();
void corona_handle_delete_or_cut(DedObject* obj);
void corona_handle_delete_selection(CDedLevel* level);
void corona_ensure_uid(int& uid);

// Create corona objects for all "corona_N" tag points found in a vmesh.
// Each tag gets a corona using the corresponding glare from the glare_names list.
// If glare_names has one entry, it applies to all tags (clutter convention).
// Returns the newly created coronas (already added to level).
std::vector<DedCorona*> corona_create_from_mesh_tags(
    CDedLevel* level, EditorVMesh* vmesh,
    const Vector3& obj_pos, const Matrix3& obj_orient,
    const std::vector<std::string>& glare_names);
