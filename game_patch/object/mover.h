#pragma once
#include "../rf/mover.h"

rf::Mover* mover_find_by_mover_brush(const rf::MoverBrush* mover_brush);
bool alpine_mover_holds_open(const rf::Mover* mp);
void alpine_mover_init_hold_open();
void alpine_mover_clear_hold_open();
