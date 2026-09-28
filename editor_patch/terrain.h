#pragma once

#include <cstddef>
#include <string>
#include "mfc_types.h"
#include "level.h"

// Warnings go to RED's log pane; `popup` also raises a message box outside a headless bake.
void terrain_report(const std::string& msg, bool popup);
// Clamps a terrain into the ranges every reader accepts, as the writer and the geometry build see it.
void terrain_prepare(DedTerrain& terrain);

// Terrain serialization (called from level.cpp injection points). A group (.rfg) carries no build
// state: it writes no build mapping, and its terrains load unbuilt with no message box of their own.
void terrain_serialize_chunk(CDedLevel& level, rf::File& file, bool group = false);
// Appends the chunk's terrains, or none when a record is malformed, memory runs out or the level would
// pass the terrain count or data budget; false then.
bool terrain_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len, bool group = false);

// Terrain object lifecycle
void PlaceNewTerrainObject();
DedTerrain* CloneTerrainObject(DedTerrain* source, bool add_to_level = true);
void DeleteTerrainObject(DedTerrain* terrain);

// Properties dialog, for `terrain` or for the first selected terrain
void terrain_show_properties(CDedLevel* level, DedTerrain* terrain);
void ShowTerrainPropertiesDialog(CDedLevel* level);

// Handlers called from shared hook points in alpine_obj.cpp
void terrain_render_surfaces(CDedLevel* level);
void terrain_render(CDedLevel* level);
void terrain_pick(CDedLevel* level, int param1, int param2);
// Centre of the terrain's bounding box, where its icon is drawn and picked.
Vector3 terrain_icon_pos(const DedTerrain& terrain);
DedTerrain* terrain_click_pick(CDedLevel* level, float click_x, float click_y);
void terrain_tree_populate(EditorTreeCtrl* tree, int master_groups, CDedLevel* level);
void terrain_tree_add_object_type(EditorTreeCtrl* tree);
bool terrain_copy_object(DedObject* source);
void terrain_paste_objects(CDedLevel* level);
void terrain_clear_clipboard();
void terrain_handle_delete_or_cut(DedObject* obj);
void terrain_handle_delete_selection(CDedLevel* level);
void terrain_ensure_uid(int& uid);
