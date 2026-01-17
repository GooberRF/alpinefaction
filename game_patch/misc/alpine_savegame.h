#pragma once

#include <algorithm>
#include <optional>
#include "toml.hpp"
#include "../rf/save_restore.h"
#include "../rf/geometry.h"
#include "../rf/mover.h"
#include "../rf/player/player.h"
#include "../rf/math/quaternion.h"

namespace asg
{
    struct SavegameHeader
    {
        uint8_t version;
        float game_time;
        float level_time2;
        float level_time_left;
        std::string mod_name;
        uint8_t num_saved_levels;
        std::vector<std::string> saved_level_filenames;
        std::string current_level_filename;
        int current_level_idx;
    };

    struct AlpineLoggedHudMessage
    {
        std::string message;
        int time_string;
        int16_t persona_index;
        int16_t display_height;
    };

    struct SavegameCommonDataGame
    {
        rf::GameDifficultyLevel difficulty;
        int newest_message_index;
        int num_logged_messages;
        int messages_total_height;
        std::vector<AlpineLoggedHudMessage> messages;
    };

    struct SavegameCommonDataPlayer
    {
        int entity_host_uid;
        int16_t clip_x, clip_y, clip_w, clip_h;
        float fov_h;
        //int32_t field_10;
        int16_t player_flags;
        int field_11f8;
        int entity_uid;
        int entity_type;
        uint8_t spew_vector_index;
        rf::Vector3 spew_pos;
        uint32_t key_items[3];
        int32_t view_obj_uid;
        int weapon_prefs[32];
        rf::Matrix3 fpgun_orient;
        rf::Vector3 fpgun_pos;
        uint8_t grenade_mode;
        //uint8_t game_difficulty;
        //int32_t field_A8;
        bool show_silencer;
        bool remote_charge_in_hand;
        bool undercover_active;
        int undercover_team;
        int player_cover_id;
        bool ai_high_flag;
    };

    struct SavegameCommonData
    {
        SavegameCommonDataGame game;
        SavegameCommonDataPlayer player;
    };

    struct SavegameObjectDataBlock
    {
        int uid;
        int parent_uid;
        //int16_t life;
        //int16_t armor;
        float life;
        float armor;
        rf::ShortVector pos;
        std::optional<rf::Vector3> pos_ha;
        rf::ShortVector vel;
        std::optional<rf::Vector3> vel_ha;
        //char friendliness;
        int friendliness;
        //char host_tag_handle;
        int host_tag_handle;
        //rf::Matrix3 orient;
        rf::ShortQuat orient;
        std::optional<rf::Matrix3> orient_ha;
        int obj_flags;
        int host_uid;
        rf::Vector3 ang_momentum;
        int physics_flags;
    };

    struct SavegameEntityDataBlock
    {
        SavegameObjectDataBlock obj;
        int8_t current_primary_weapon;
        int8_t current_secondary_weapon;
        int16_t info_index;
        int16_t weapons_clip_ammo[32];
        int16_t weapons_ammo[32];
        //char field_C0[64];
        int possesed_weapons_bitfield;
        //char hate_list[32];
        std::vector<int> hate_list;
        //uint8_t hate_list_size;
        int ai_mode;
        int ai_submode;
        int entity_type;
        int move_mode;
        int ai_mode_parm_0;
        int ai_mode_parm_1;
        int target_uid;
        int look_at_uid;
        int shoot_at_uid;
        int16_t path_node_indices[4];
        int16_t path_previous_goal;
        int16_t path_current_goal;
        rf::Vector3 path_end_pos;
        int16_t path_adjacent_node1_index;
        int16_t path_adjacent_node2_index;
        int16_t path_waypoint_list_index;
        int16_t path_goal_waypoint_index;
        uint8_t path_direction;
        uint8_t path_flags;
        rf::Vector3 path_turn_towards_pos;
        int path_follow_style;
        rf::Vector3 path_start_pos;
        rf::Vector3 path_goal_pos;
        rf::Vector3 ci_rot;
        rf::Vector3 ci_move;
        bool ci_time_relative_mouse_delta_disabled;
        int corpse_carry_uid;
        float ai_healing_left;
        rf::Vector3 ai_steering_vector;
        int ai_flags;
        rf::Vector3 eye_pos;
        rf::Matrix3 eye_orient;
        int entity_flags;
        int entity_flags2;
        rf::Vector3 control_data_phb;
        rf::Vector3 control_data_eye_phb;
        rf::Vector3 control_data_local_vel;
        std::string skin_name;
        int skin_index;
        int16_t current_state_anim;
        int16_t next_state_anim;
        int16_t last_custom_anim_index;
        int16_t state_anim_22_index;
        float total_transition_time;
        float elapsed_transition_time;
        int driller_rot_angle;
        float driller_rot_speed;
        int weapon_custom_mode_bitmap;
        int weapon_silencer_bitfield;
        uint8_t current_speed;
        bool entity_on_fire;
        int climb_region_index;
        int driller_max_geomods;
    };

    struct EntitySkinState
    {
        std::string name;
        int index = -1;
    };

    struct SavegameItemDataBlock
    {
        SavegameObjectDataBlock obj;
        int respawn_time_ms;
        int respawn_next_timer;
        float alpha;
        float create_time;
        int flags;
        int item_cls_id;
    };

    struct SavegameClutterDataBlock
    {
        int uid;
        int parent_uid;
        rf::ShortVector pos;
        std::optional<rf::Vector3> pos_ha;
        // no low accuracy orient because it causes incremental rotatation
        // stock game format doesn't save orient for clutter
        std::optional<rf::Matrix3> orient_ha;
        int delayed_kill_timestamp;
        int corpse_create_timestamp;
        bool hidden;
        int skin_index;
        std::vector<int> links;
    };

    struct SavegameTriggerDataBlock
    {
        int uid;
        rf::ShortVector pos;
        std::optional<rf::Vector3> pos_ha;
        // no low accuracy orient because it causes incremental rotatation
        // stock game format doesn't save orient for triggers
        std::optional<rf::Matrix3> orient_ha;
        int count;
        float time_last_activated;
        int trigger_flags;
        int activator_handle;
        int button_active_timestamp;
        int inside_timestamp;
        std::vector<int> links;
    };

    struct SavegameEventDataBlock
    {
        int event_type;
        int uid;
        float delay;
        bool is_on_state; // from delayed_message in event struct
        int delay_timer;
        int activated_by_entity_uid;
        int activated_by_trigger_uid;
        int event_flags;
        std::vector<int> links;
    };

    struct SavegameEventMakeInvulnerableDataBlock
    {
        SavegameEventDataBlock ev;
        int time_left;
    };

    struct SavegameEventWhenDeadDataBlock
    {
        SavegameEventDataBlock ev;
        bool message_sent;
        //bool when_any_dead;
    };

    struct SavegameEventGoalCreateDataBlock
    {
        SavegameEventDataBlock ev;
        int count;
    };

    struct SavegameEventAlarmSirenDataBlock
    {
        SavegameEventDataBlock ev;
        bool alarm_siren_playing;
    };

    struct SavegameEventCyclicTimerDataBlock // not too concerned with what stock game saved (formatted very weird), saving what we know we need
    {
        SavegameEventDataBlock ev;
        int next_fire_timer;
        int send_count;
    };

    struct SavegameLevelDataHeader
    {
        std::string filename;
        float level_time;
        rf::Vector3 aabb_min;
        rf::Vector3 aabb_max;
    };

    struct SavegameLevelPersistentGoalDataBlock
    {
        std::string goal_name;
        int count;
    };

    struct SavegameLevelDecalDataBlock
    {
        rf::Vector3 pos;
        rf::Matrix3 orient;
        rf::Vector3 width;
        uint32_t flags;
        uint8_t alpha;
        float tiling_scale;
        std::string bitmap_filename;
    };

    struct SavegameLevelBoltEmitterDataBlock
    {
        int uid;
        bool active;
    };

    struct SavegameLevelParticleEmitterDataBlock
    {
        int uid;
        bool active;
    };

    struct SavegameLevelKeyframeDataBlock
    {
        SavegameObjectDataBlock obj;
        float rot_cur_pos;
        int start_at_keyframe;
        int stop_at_keyframe;
        rf::MoverFlags mover_flags;
        float travel_time_seconds;
        float rotation_travel_time_seconds;
        int wait_timestamp;
        int trigger_uid;
        float dist_travelled;
        float cur_vel;
        int stop_completely_at_keyframe;
    };

    struct SavegameLevelPushRegionDataBlock
    {
        int uid;
        bool active;
    };

    struct SavegameLevelWeaponDataBlock
    {
        SavegameObjectDataBlock obj;
        int info_index;
        float life_left_seconds;
        int weapon_flags;
        int sticky_host_uid;
        rf::Vector3 sticky_host_pos_offset;
        rf::Matrix3 sticky_host_orient;
        uint8_t weap_friendliness;
        int target_uid;
        float pierce_power_left;
        float thrust_left;
        rf::Vector3 firing_pos;
    };

    struct SavegameLevelCorpseDataBlock
    {
        SavegameObjectDataBlock obj; // transform, uid, etc.
        float create_time;
        float lifetime_seconds;
        int corpse_flags;
        int entity_type;
        std::string mesh_name;
        int emitter_kill_timestamp;
        float body_temp;
        int state_anim;
        int action_anim;
        int drop_anim;
        int carry_anim;
        int corpse_pose;
        std::string helmet_name;
        int item_uid;
        int body_drop_sound_handle;
        float mass;
        float radius;
        std::vector<rf::PCollisionSphere> cspheres;
    };

    struct SavegameLevelBloodPoolDataBlock
    {
        rf::Vector3 pos;
        rf::Matrix3 orient;
        rf::Color pool_color;
    };

    struct SavegameLevelData
    {
        SavegameLevelDataHeader header;
        std::vector<int> killed_room_uids;
        std::vector<int> dead_entity_uids;
        std::vector<int> deleted_event_uids;
        std::vector<rf::GeomodCraterData> geomod_craters;
        std::vector<SavegameLevelPersistentGoalDataBlock> persistent_goals;
        std::vector<SavegameEntityDataBlock> entities;
        std::vector<SavegameItemDataBlock> items;
        std::vector<SavegameClutterDataBlock> clutter;
        std::vector<SavegameTriggerDataBlock> triggers;
        std::vector<SavegameEventDataBlock> other_events;
        std::vector<SavegameEventMakeInvulnerableDataBlock> make_invulnerable_events;
        std::vector<SavegameEventWhenDeadDataBlock> when_dead_events;
        std::vector<SavegameEventGoalCreateDataBlock> goal_create_events;
        std::vector<SavegameEventAlarmSirenDataBlock> alarm_siren_events;
        std::vector<SavegameEventCyclicTimerDataBlock> cyclic_timer_events;
        std::vector<SavegameLevelDecalDataBlock> decals;
        std::vector<SavegameLevelBoltEmitterDataBlock> bolt_emitters;
        std::vector<SavegameLevelParticleEmitterDataBlock> particle_emitters;
        std::vector<SavegameLevelKeyframeDataBlock> movers;
        std::vector<SavegameLevelPushRegionDataBlock> push_regions;
        std::vector<SavegameLevelWeaponDataBlock> weapons;
        std::vector<SavegameLevelCorpseDataBlock> corpses;
        std::vector<SavegameLevelBloodPoolDataBlock> blood_pools;
    };
    // maybe: lights (on/off state), 

    struct SavegameData
    {
        SavegameHeader header;
        SavegameCommonData common;
        std::vector<SavegameLevelData> levels;
    };

    struct AlpinePonr
    {
        std::string current_level_filename;
        std::vector<std::string> levels_to_save;
    };

    SavegameData build_savegame_data(rf::Player* pp);
    int add_handle_for_delayed_resolution(int uid, int* obj_handle_ptr);
    void clear_delayed_handles();
    void resolve_delayed_handles();
    extern bool g_use_high_accuracy_savegame;

}
