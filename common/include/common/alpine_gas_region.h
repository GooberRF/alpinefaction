#pragma once

// Include after the binary's rf engine types (rf::Vector3, rf::Matrix3, rf::Color).

#include <cstdint>

// Gas region info, loaded from stock RFL chunk 0xB00
struct GasRegionInfo
{
    int32_t uid = -1;
    rf::Vector3 pos{};
    rf::Matrix3 orient{};
    int32_t shape = 1;       // 1=sphere, 2=box
    float radius = 1.0f;     // sphere only
    float height = 1.0f;     // box only
    float width = 1.0f;      // box only
    float depth = 1.0f;      // box only
    rf::Color color{255, 255, 255, 255};
    float density = 1.0f;
    bool enabled = true;
};
