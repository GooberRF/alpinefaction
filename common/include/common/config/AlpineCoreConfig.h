#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>

class AlpineCoreConfig
{
public:
    // Configurable fields
    bool vsync = false;
    bool gamepad_rawinput_enabled = false;

    // Experimental settings
    bool low_frame_latency = false;
    bool allow_tearing = false;
    bool skip_gamma_pass = false;

    std::vector<std::string> orphaned_lines;
    bool load(const std::string& filename = "alpine_system.ini");
    void save(const std::string& filename = "alpine_system.ini") const;

private:
    static bool string_to_bool(const std::string& str);
    static std::string bool_to_string(bool val);
};
