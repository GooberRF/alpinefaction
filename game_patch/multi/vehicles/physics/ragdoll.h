#pragma once

#include <vector>
#include "../../../rf/math/matrix.h"

namespace rf
{
    struct Corpse;
    struct Entity;
    struct Object;
    struct Weapon;
}

bool ragdolls_wanted();
bool ragdoll_is_ghost(const rf::Object* obj);
void ragdoll_note_weapon_hit(const rf::Weapon* wp);
void ragdoll_after_entity_dead(rf::Entity* ep);
// For savegames: the pose of a corpse's ragdoll once it has left the death animation, as each body's world transform
// and each bone's in its body. False when it has none.
bool ragdoll_get_pose(const rf::Corpse& corpse, std::vector<rf::Matrix43>& bodies, std::vector<rf::Matrix43>& bones);
// Makes a corpse a frozen ragdoll in that pose, which a hit wakes. False when ragdolls are off or the pose does not fit
// the corpse's rig.
bool ragdoll_set_pose(rf::Corpse& corpse, const std::vector<rf::Matrix43>& bodies,
                      const std::vector<rf::Matrix43>& bones);
void ragdoll_apply_patches();
