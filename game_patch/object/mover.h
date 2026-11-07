#pragma once
#include <cstddef>
#include "../rf/mover.h"

namespace rf
{
    class File;
    struct Entity;
}

rf::Mover* mover_find_by_mover_brush(const rf::MoverBrush* mover_brush);
bool alpine_mover_holds_open(const rf::Mover* mp);
void alpine_mover_clear_hold_open();
void alpine_mover_path_load_chunk(rf::File& file, std::size_t chunk_len);
void alpine_mover_path_clear_state();

// The ground carry (0x0041E370) for movers with rider options. alpine_mover_carry_rider sets ep's carry for its
// ground object and returns true, or returns false for ground that is no such mover, leaving the stock copy.
bool alpine_mover_has_riders();
bool alpine_mover_carry_rider(rf::Entity& ep, const rf::Object& ground);
void alpine_mover_rider_frame_done();
