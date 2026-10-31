#pragma once
#include <cstddef>
#include "../rf/mover.h"

namespace rf
{
    class File;
}

rf::Mover* mover_find_by_mover_brush(const rf::MoverBrush* mover_brush);
bool alpine_mover_holds_open(const rf::Mover* mp);
void alpine_mover_clear_hold_open();
void alpine_mover_path_load_chunk(rf::File& file, std::size_t chunk_len);
void alpine_mover_path_clear_state();
