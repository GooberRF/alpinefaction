#pragma once

#include <cstddef>
#include <vector>
#include "mfc_types.h"
#include "level.h"

// Ambience region serialization (called from level.cpp injection points and the .rfg group hooks)
void ambience_region_serialize_chunk(CDedLevel& level, rf::File& file);
void ambience_region_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len);

// Ambience region property dialog
void ShowAmbienceRegionPropertiesDialog(CDedLevel* level);

// Ambience region object lifecycle
void PlaceNewAmbienceRegionObject();
DedAmbienceRegion* CloneAmbienceRegionObject(DedAmbienceRegion* source, bool add_to_level = true);

// Handlers called from shared hook points in alpine_obj.cpp
void ambience_region_render(CDedLevel* level);
void ambience_region_pick(CDedLevel* level, int param1, int param2);
DedAmbienceRegion* ambience_region_click_pick(CDedLevel* level, float click_x, float click_y);
void ambience_region_tree_populate(EditorTreeCtrl* tree, int master_groups, CDedLevel* level);
void ambience_region_tree_add_object_type(EditorTreeCtrl* tree);
bool ambience_region_copy_object(DedObject* source);
void ambience_region_paste_objects(CDedLevel* level);
void ambience_region_clear_clipboard();
// Swaps the staged clones with `other`, so a duplicate can copy and paste with the user's clipboard set aside.
void ambience_region_swap_clipboard(std::vector<DedAmbienceRegion*>& other);
void ambience_region_handle_delete_or_cut(DedObject* obj);
void ambience_region_ensure_uid(int& uid);
