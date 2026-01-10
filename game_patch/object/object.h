#pragma once

namespace rf
{
    struct Object;
    struct Player;

    struct AlpineRespawnPoint
    {
        int uid;
        bool enabled;
        String name;
        Vector3 position;
        Matrix3 orientation;
        bool red_team;
        bool blue_team;
        float dist_other_player;
    };
}

void object_do_patch();
void obj_mesh_lighting_alloc_one(rf::Object *objp);
void obj_mesh_lighting_free_one(rf::Object *objp);
void obj_mesh_lighting_update_one(rf::Object *objp);
void obj_mesh_lighting_maybe_update(rf::Object *objp);
void trigger_send_state_info(rf::Player* player);
rf::AlpineRespawnPoint* get_alpine_respawn_point_by_uid(int uid);
void set_alpine_respawn_point_enabled(rf::AlpineRespawnPoint* point, bool enabled);
void set_alpine_respawn_point_teams(rf::AlpineRespawnPoint* point, bool red, bool blue);
std::vector<rf::AlpineRespawnPoint> get_alpine_respawn_points();
void entity_set_gib_flag(rf::Entity* ep);

constexpr size_t old_obj_limit = 1024;
constexpr size_t obj_limit = 65536;
