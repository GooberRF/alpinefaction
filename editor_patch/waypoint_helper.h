#pragma once

#include <cstddef>
#include <vector>
#include "mfc_types.h"
#include "level.h"

// Waypoint helper serialization (called from level.cpp injection points and the .rfg group hooks)
void waypoint_helper_serialize_chunk(CDedLevel& level, rf::File& file);
void waypoint_helper_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len);

// Waypoint helper property dialog
void ShowWaypointHelperPropertiesDialog(CDedLevel* level);

// Waypoint helper object lifecycle
void PlaceNewWaypointHelperObject();
DedWaypointHelper* CloneWaypointHelperObject(DedWaypointHelper* source, bool add_to_level = true);

// Link mode lets a helper link only to another helper (not to itself).
bool waypoint_helper_link_allowed(const DedObject* src, const DedObject* dst);

// Handlers called from shared hook points in alpine_obj.cpp
void waypoint_helper_render(CDedLevel* level);
void waypoint_helper_pick(CDedLevel* level, int param1, int param2);
DedWaypointHelper* waypoint_helper_click_pick(CDedLevel* level, float click_x, float click_y);
void waypoint_helper_tree_populate(EditorTreeCtrl* tree, int master_groups, CDedLevel* level);
void waypoint_helper_tree_add_object_type(EditorTreeCtrl* tree);
bool waypoint_helper_copy_object(DedObject* source);
void waypoint_helper_paste_objects(CDedLevel* level);
void waypoint_helper_clear_clipboard();
// Swaps the staged clones with `other`, so a duplicate can copy and paste with the user's clipboard set aside.
void waypoint_helper_swap_clipboard(std::vector<DedWaypointHelper*>& other);
void waypoint_helper_handle_delete_or_cut(DedObject* obj);
void waypoint_helper_ensure_uid(int& uid);
