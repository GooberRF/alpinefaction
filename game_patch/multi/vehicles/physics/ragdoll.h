#pragma once

namespace rf
{
    struct Entity;
    struct Object;
    struct Weapon;
}

bool ragdolls_wanted();
bool ragdoll_is_ghost(const rf::Object* obj);
void ragdoll_note_weapon_hit(const rf::Weapon* wp);
void ragdoll_after_entity_dead(rf::Entity* ep);
void ragdoll_apply_patches();
