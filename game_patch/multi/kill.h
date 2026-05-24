#pragma once

namespace rf
{
    struct Entity;
}

// Distribute an "effective health" reward across life and armor: life is topped
// up first toward `max_life_cap`, then the remainder is split in half and applied
// toward `max_armor_cap` (armor counts as half the damage absorption of life).
// Caller is responsible for choosing the caps — e.g. kill_reward respects its
// own "super" caps, bagman uses the entity's class max_life/max_armor.
void distribute_effective_health(rf::Entity* ep, float amount, float max_life_cap, float max_armor_cap);
