#pragma once

namespace rf
{
    struct Entity;
}

void distribute_effective_health(rf::Entity* ep, float amount, float max_life_cap, float max_armor_cap);
