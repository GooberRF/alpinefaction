#pragma once

#include <cstddef>
#include "mfc_types.h"
#include "level.h"

// Rope emitter serialization (called from level.cpp injection points)
void rope_emitter_serialize_chunk(CDedLevel& level, rf::File& file);
void rope_emitter_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len);

// Rope emitter object lifecycle
void PlaceNewRopeEmitterObject();
DedRopeEmitter* CloneRopeEmitterObject(DedRopeEmitter* source, bool add_to_level = true);
void DeleteRopeEmitterObject(DedRopeEmitter* rope);

// Properties dialog
void ShowRopeEmitterPropertiesDialog(CDedLevel* level);

// Handlers called from shared hook points in alpine_obj.cpp
void rope_emitter_render(CDedLevel* level);
void rope_emitter_pick(CDedLevel* level, int param1, int param2);
DedRopeEmitter* rope_emitter_click_pick(CDedLevel* level, float click_x, float click_y);
void rope_emitter_tree_populate(EditorTreeCtrl* tree, int master_groups, CDedLevel* level);
void rope_emitter_tree_add_object_type(EditorTreeCtrl* tree);
bool rope_emitter_copy_object(DedObject* source);
void rope_emitter_paste_objects(CDedLevel* level);
void rope_emitter_clear_clipboard();
void rope_emitter_handle_delete_or_cut(DedObject* obj);
void rope_emitter_handle_delete_selection(CDedLevel* level);
void rope_emitter_ensure_uid(int& uid);
