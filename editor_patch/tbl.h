#pragma once

#include <string>

// Clutter class info extracted from clutter.tbl for the "To Mesh Object" conversion.
struct ClutterClassInfo {
    std::string class_name;
    std::string v3d_filename;
    float life = -1.0f;
    int material = 0; // 0=Default,1=Rock,2=Metal,3=Flesh,4=Water,5=Lava,6=Solid,7=Sand,8=Ice,9=Glass
    std::string debris_filename;
    float debris_velocity = 10.0f;
    std::string explode_vclip;      // vclip name (not index)
    float explode_radius = 1.0f;
    std::string corpse_class_name;  // name of corpse class (looked up to get mesh/material)
    float damage_type_factors[11] = {1,1,1,1,1,1,1,1,1,1,1};
};

// Look up clutter class info by class name.
// Parses clutter.tbl on first call (lazy init).
// Returns nullptr if the class is not found.
const ClutterClassInfo* clutter_tbl_find(const char* class_name);
