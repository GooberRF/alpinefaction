#pragma once

#include <cstddef>
#include "mfc_types.h"
#include "level.h"

// Weather region serialization (called from level.cpp injection points)
void weather_region_serialize_chunk(CDedLevel& level, rf::File& file);
void weather_region_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len);

// Weather region property dialog
void ShowWeatherRegionPropertiesDialog(CDedLevel* level);

// Weather region object lifecycle
void PlaceNewWeatherRegionObject();
DedWeatherRegion* CloneWeatherRegionObject(DedWeatherRegion* source, bool add_to_level = true);
void DeleteWeatherRegionObject(DedWeatherRegion* weather_region);

// Handlers called from shared hook points in alpine_obj.cpp
void weather_region_render(CDedLevel* level);
void weather_region_pick(CDedLevel* level, int param1, int param2);
DedWeatherRegion* weather_region_click_pick(CDedLevel* level, float click_x, float click_y);
void weather_region_tree_populate(EditorTreeCtrl* tree, int master_groups, CDedLevel* level);
void weather_region_tree_add_object_type(EditorTreeCtrl* tree);
bool weather_region_copy_object(DedObject* source);
void weather_region_paste_objects(CDedLevel* level);
void weather_region_clear_clipboard();
void weather_region_handle_delete_or_cut(DedObject* obj);
void weather_region_handle_delete_selection(CDedLevel* level);
void weather_region_ensure_uid(int& uid);
