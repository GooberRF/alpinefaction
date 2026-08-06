#pragma once

namespace rf
{
    struct Object;
    struct Player;
    struct Entity;
    struct Vector3;

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
bool is_monitor_screen_bitmap(int bitmap_handle);
void obj_mesh_lighting_alloc_one(rf::Object *objp);
void obj_mesh_lighting_free_one(rf::Object *objp);
void obj_mesh_lighting_update_one(rf::Object *objp);
void obj_mesh_lighting_maybe_update(rf::Object *objp);
void evaluate_fullbright_meshes();
void trigger_send_state_info(rf::Player* player);
rf::AlpineRespawnPoint* get_alpine_respawn_point_by_uid(int uid);
void set_alpine_respawn_point_enabled(rf::AlpineRespawnPoint* point, bool enabled);
void set_alpine_respawn_point_teams(rf::AlpineRespawnPoint* point, bool red, bool blue);
std::vector<rf::AlpineRespawnPoint> get_alpine_respawn_points();
void entity_set_gib_flag(rf::Entity* ep);
void riot_shield_apply_remote_state(rf::Entity* ep, float life, const rf::Vector3& impact_pos);
void riot_shield_do_frame();
void riot_shield_on_player_spawn(rf::Player* player);
void riot_shield_reset_fp_decals(rf::Player* player);
void riot_shield_on_multi_level_init();

constexpr size_t old_obj_limit = 1024;
constexpr size_t obj_limit = 65536;
