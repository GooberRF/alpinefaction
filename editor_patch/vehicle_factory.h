#pragma once

#include <cstddef>
#include <string>
#include <vector>
#include "mfc_types.h"
#include "level.h"

// Vehicle factory serialization (called from level.cpp injection points)
void vehicle_factory_serialize_chunk(CDedLevel& level, rf::File& file);
void vehicle_factory_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len);

// Vehicle factory property dialog
void ShowVehicleFactoryPropertiesDialog(CDedLevel* level);

// Vehicle factory object lifecycle
// An empty class keeps the factory's default.
DedVehicleFactory* PlaceNewVehicleFactoryObject(const std::string& vehicle_class, const Vector3& pos,
                                                const Matrix3& orient);
DedVehicleFactory* CloneVehicleFactoryObject(DedVehicleFactory* source, bool add_to_level = true);
// Frees the preview mesh; the next render loads it again.
void vehicle_factory_release_preview(DedVehicleFactory* factory);
// The classes the properties dialog offers, and the mesh a class previews with (empty when unknown).
std::vector<std::string> vehicle_factory_class_choices();
std::string vehicle_factory_mesh_for_class(const std::string& class_name);

// Handlers called from shared hook points in alpine_obj.cpp
void vehicle_factory_render(CDedLevel* level);
void vehicle_factory_pick(CDedLevel* level, int param1, int param2);
DedVehicleFactory* vehicle_factory_click_pick(CDedLevel* level, float click_x, float click_y,
    float* out_dist_sq);
void vehicle_factory_tree_populate(EditorTreeCtrl* tree, int master_groups, CDedLevel* level);
void vehicle_factory_tree_add_object_type(EditorTreeCtrl* tree);
bool vehicle_factory_copy_object(DedObject* source);
void vehicle_factory_paste_objects(CDedLevel* level);
void vehicle_factory_clear_clipboard();
// Swaps the staged clones with `other`, so a duplicate can copy and paste with the user's clipboard set aside.
void vehicle_factory_swap_clipboard(std::vector<DedVehicleFactory*>& other);
void vehicle_factory_handle_delete_or_cut(DedObject* obj);
void vehicle_factory_ensure_uid(int& uid);
