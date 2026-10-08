#pragma once

#include <cstddef>
#include <common/alpine_dir_light.h>
#include "mfc_types.h"
#include "level.h"

// Directional light serialization (called from level.cpp injection points and the .rfg group hooks)
void directional_light_serialize_chunk(CDedLevel& level, rf::File& file);
void directional_light_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len,
                                         int content_version);

// The object's fields as the shared record.
alpine_dir_light::Record directional_light_record(const DedDirectionalLight& light);

// Directional light property dialog
void ShowDirectionalLightPropertiesDialog(CDedLevel* level);

// Directional light object lifecycle
void PlaceNewDirectionalLightObject();
DedDirectionalLight* CloneDirectionalLightObject(DedDirectionalLight* source, bool add_to_level = true);

// Handlers called from shared hook points in alpine_obj.cpp
void directional_light_render(CDedLevel* level);
void directional_light_pick(CDedLevel* level, int param1, int param2);
DedDirectionalLight* directional_light_click_pick(CDedLevel* level, float click_x, float click_y);
void directional_light_tree_populate(EditorTreeCtrl* tree, int master_groups, CDedLevel* level);
void directional_light_tree_add_object_type(EditorTreeCtrl* tree);
bool directional_light_copy_object(DedObject* source);
void directional_light_paste_objects(CDedLevel* level);
void directional_light_clear_clipboard();
void directional_light_handle_delete_or_cut(DedObject* obj);
void directional_light_ensure_uid(int& uid);

// The level sun's viewport arrow. One object for the process lifetime, never in master_objects: it is
// shown while the level properties enable the sun, pinned above the player start, and rotating it
// rewrites sun_yaw / sun_pitch.
void sun_arrow_sync(CDedLevel* level);
void sun_arrow_render(CDedLevel* level);
DedSunArrow* sun_arrow_click_pick(float click_x, float click_y);
void sun_arrow_remove_from_selection(CDedLevel* level);
// Hides the arrow and drops it from the selection; it never enters master_objects or a group.
void sun_arrow_detach(CDedLevel* level);
