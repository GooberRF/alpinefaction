#pragma once

#include <cstddef>
#include <vector>
#include "../rf/file/file.h"
#include "../rf/math/vector.h"
#include "../rf/math/matrix.h"

struct AlpineBagInfo
{
    int uid = -1;
    rf::Vector3 pos{};
    rf::Matrix3 orient{};
};

void alpine_bag_load_chunk(rf::File& file, std::size_t chunk_len);
void alpine_bag_clear_state();
const std::vector<AlpineBagInfo>& get_bag_objects();
