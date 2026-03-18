#pragma once

#include "object.h"
#include "gr/gr.h"

namespace rf {
    struct GFace;
    struct MoverBrush;
    struct Player;

    struct GlareInfo
    {
        String name;                // +0x00
        gr::Color light_color;      // +0x08
        int corona_bitmap;          // +0x0C (bm handle, 0 = none)
        float cone_angle;           // +0x10 (degrees * 0.5, as stored by effects.tbl parser)
        float intensity;            // +0x14
        float radius_scale;         // +0x18
        float radius_distance;      // +0x1C
        float diminish_distance;    // +0x20
        int volumetric_bitmap;      // +0x24 (bm handle, 0 = none)
        float volumetric_height;    // +0x28
        float volumetric_length;    // +0x2C
        int reflection_bitmap;      // +0x30 (bm handle, 0 = none)
    };
    static_assert(sizeof(GlareInfo) == 0x34);

    struct Glare : Object
    {
        bool enabled;
        bool visible;
        int last_covering_objh;
        MoverBrush *last_covering_mover_brush;
        GFace *last_covering_face;
        float last_rendered_intensity[2];
        float last_rendered_radius[2];
        GlareInfo *info;
        int info_index;
        int flags;
        Glare *next;
        Glare *prev;
        Vector3 field_2C0;
        Player *parent_player;
        bool is_rod;
        Vector3 rod_pos1;
        Vector3 rod_pos2;
    };
    static_assert(sizeof(Glare) == 0x2EC);

    static auto& glare_collide_entity = addr_as_ref<bool(Object *obj, Glare *glare, Vector3 *eye_pos)>(0x00415280);

    // Glare linked list
    static auto& glare_list = addr_as_ref<Glare>(0x005C9BA8);
    static auto& glare_list_tail = addr_as_ref<Glare*>(0x005C9E64);
}
