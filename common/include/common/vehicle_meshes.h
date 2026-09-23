#pragma once

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
