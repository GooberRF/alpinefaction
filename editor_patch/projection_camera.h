#pragma once

#include <cstddef>
#include "mfc_types.h"
#include "level.h"

// Projection camera serialization (called from level.cpp injection points)
void projection_camera_serialize_chunk(CDedLevel& level, rf::File& file);
void projection_camera_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len);

// Projection camera object lifecycle
void PlaceNewProjectionCameraObject();
DedProjectionCamera* CloneProjectionCameraObject(DedProjectionCamera* source, bool add_to_level = true);
void DeleteProjectionCameraObject(DedProjectionCamera* camera);

// Handlers called from shared hook points in alpine_obj.cpp
void projection_camera_render(CDedLevel* level);
void projection_camera_pick(CDedLevel* level, int param1, int param2);
DedProjectionCamera* projection_camera_click_pick(CDedLevel* level, float click_x, float click_y);
void projection_camera_tree_populate(EditorTreeCtrl* tree, int master_groups, CDedLevel* level);
void projection_camera_tree_add_object_type(EditorTreeCtrl* tree);
bool projection_camera_copy_object(DedObject* source);
void projection_camera_paste_objects(CDedLevel* level);
void projection_camera_clear_clipboard();
void projection_camera_handle_delete_or_cut(DedObject* obj);
void projection_camera_handle_delete_selection(CDedLevel* level);
void projection_camera_ensure_uid(int& uid);
