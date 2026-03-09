#pragma once

#include <cstddef>
#include "mfc_types.h"
#include "level.h"

// Note serialization (called from level.cpp injection points)
void note_serialize_chunk(CDedLevel& level, rf::File& file);
void note_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len);

// Note property dialog
void ShowNotePropertiesDialog(CDedLevel* level);

// Note object lifecycle
void PlaceNewNoteObject();
DedNote* CloneNoteObject(DedNote* source, bool add_to_level = true);
void DeleteNoteObject(DedNote* note);

// Handlers called from shared hook points in alpine_obj.cpp
void note_render(CDedLevel* level);
void note_pick(CDedLevel* level, int param1, int param2);
DedNote* note_click_pick(CDedLevel* level, float click_x, float click_y);
void note_tree_populate(EditorTreeCtrl* tree, int master_groups, CDedLevel* level);
void note_tree_add_object_type(EditorTreeCtrl* tree);
bool note_copy_object(DedObject* source);
void note_paste_objects(CDedLevel* level);
void note_clear_clipboard();
void note_handle_delete_or_cut(DedObject* obj);
void note_handle_delete_selection(CDedLevel* level);
void note_ensure_uid(int& uid);
