#pragma once

#include <cstddef>
#include "mfc_types.h"
#include "level.h"

void bag_serialize_chunk(CDedLevel& level, rf::File& file);
void bag_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len);

void PlaceNewBagObject();
DedBag* CloneBagObject(DedBag* source, bool add_to_level = true);
void DeleteBagObject(DedBag* bag);

void bag_render(CDedLevel* level);
void bag_pick(CDedLevel* level, int param1, int param2);
DedBag* bag_click_pick(CDedLevel* level, float click_x, float click_y);
void bag_tree_populate(EditorTreeCtrl* tree, int master_groups, CDedLevel* level);
void bag_tree_add_object_type(EditorTreeCtrl* tree);
bool bag_copy_object(DedObject* source);
void bag_paste_objects(CDedLevel* level);
void bag_clear_clipboard();
void bag_handle_delete_or_cut(DedObject* obj);
void bag_handle_delete_selection(CDedLevel* level);
void bag_ensure_uid(int& uid);
