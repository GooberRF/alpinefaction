#pragma once

#include <cstdint>
#include <string_view>
#include <common/utils/string-utils.h>

// Reader cap on Vehicle Factory records in a level, shared by the game and the level editor.
inline constexpr uint32_t vehicle_factory_max_records = 1000;

// entity.tbl classes Vehicle Factories do not support, which the level editor leaves off its class list and the
// Reference menu leaves out.
inline constexpr const char* vehicle_factory_unsupported_classes[] = {
    "masako_fighter", "Stationary Turret", "Shuttle",
};

inline bool vehicle_factory_class_is_unsupported(std::string_view class_name)
{
    for (const char* name : vehicle_factory_unsupported_classes) {
        if (string_iequals(class_name, name)) {
            return true;
        }
    }
    return false;
}

// Hull meshes Alpine Faction substitutes for the stock vehicle classes at level init, shared so the
// level editor's Vehicle Factory preview shows the hull the game will actually spawn. Any extra
// per-class override the game needs (cockpit VFX) stays game side, keyed on these class names.

struct AlpineVehicleClassMesh
{
    const char* class_name;     // entity.tbl class name
    const char* vmesh_filename; // what $V3D Filename becomes for the level
};

inline constexpr AlpineVehicleClassMesh alpine_vehicle_class_meshes[] = {
    {"Jeep01", "af_Jeep01.v3m"},
    {"APC", "af_APC.v3m"},
    {"Fighter01", "af_Fighter01m.v3m"},
    {"Driller01", "af_Driller01.v3m"},
    {"sub", "af_Sub_Mini01.v3m"},
};

// The hull Alpine Faction gives entity.tbl class `cls`, or nullptr where it keeps the stock one.
inline const char* alpine_vehicle_class_mesh(std::string_view cls)
{
    for (const AlpineVehicleClassMesh& m : alpine_vehicle_class_meshes) {
        if (string_iequals(m.class_name, cls)) {
            return m.vmesh_filename;
        }
    }
    return nullptr;
}
