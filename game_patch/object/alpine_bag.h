#pragma once

#include <optional>
#include <cstddef>
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
std::optional<AlpineBagInfo> get_first_bag_object();
