#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// .rfa skeletal animation files, read by the game and the level editor.

inline constexpr uint32_t rfa_signature = 0x46564D56; // "VMVF"
// 160 ticks per 30 fps frame.
inline constexpr int rfa_ticks_per_sec = 4800;

// The leading fields of the file header.
struct RfaHeader
{
    uint32_t signature;
    int32_t version;
    float pos_reduction;
    float rot_reduction;
    int32_t start_time;
    int32_t end_time;
    int32_t num_bones;
    int32_t num_morph_vertices;
    int32_t num_morph_keyframes;
};
static_assert(sizeof(RfaHeader) == 0x24);
static_assert(offsetof(RfaHeader, start_time) == 0x10);
static_assert(offsetof(RfaHeader, num_bones) == 0x18);

// The file an animation name loads: the engine cuts the name at its first dot and appends ".rfa", so a
// tbl's .mvf name or a multi-dot name resolves as it does in the engine.
inline std::string rfa_load_name(std::string_view name)
{
    return std::string{name.substr(0, name.find('.'))} + ".rfa";
}
