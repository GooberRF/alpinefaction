#include <cmath>
#include <cstdint>
#include "../rf/clutter.h"
#include "../rf/object.h"
#include "alpine_obj_common.h"

bool alpine_orient_is_sane(const rf::Matrix3& orient)
{
    for (const rf::Vector3* v : {&orient.rvec, &orient.uvec, &orient.fvec}) {
        const float len_sq = v->x * v->x + v->y * v->y + v->z * v->z;
        if (!std::isfinite(len_sq) || len_sq < 0.9f || len_sq > 1.1f) {
            return false;
        }
    }
    return true;
}

void alpine_init_anchor_clutter(rf::Clutter* clutter)
{
    clutter->info_index = -1;
    clutter->corpse_index = -1;
    clutter->sound_handle = -1;
    clutter->delayed_kill_sound = -1;
    clutter->dmg_type_that_killed_me = 0;
    clutter->corpse_vmesh_handle = nullptr;
    clutter->current_skin_index = 0;
    clutter->already_spawned_glass = false;
    clutter->use_sound = -1;
    clutter->killable_index = 0xFFFF; // default: not killable

    clutter->prev = rf::clutter_list_tail;
    clutter->next = reinterpret_cast<rf::Clutter*>(&rf::clutter_list);
    rf::clutter_list_tail->next = clutter;
    rf::clutter_list_tail = clutter;
    rf::clutter_count++;
}

rf::Clutter* alpine_create_anchor_clutter(const rf::Vector3& pos, const rf::Matrix3& orient, int uid,
                                          const char* name)
{
    rf::ObjectCreateInfo oci{};
    oci.pos = pos;
    oci.orient = orient;

    rf::Object* obj = rf::obj_create(rf::OT_CLUTTER, -1, 0, &oci, 0, nullptr);
    if (!obj) {
        return nullptr;
    }

    auto* clutter = reinterpret_cast<rf::Clutter*>(obj);
    clutter->info = &rf::get_dummy_clutter_info();
    alpine_init_anchor_clutter(clutter);

    obj->uid = uid;
    if (name && name[0] != '\0') {
        obj->name = name;
    }
    obj->obj_flags = static_cast<rf::ObjectFlags>(
        static_cast<int>(obj->obj_flags) | static_cast<int>(rf::OF_INVULNERABLE)
    );

    return clutter;
}
