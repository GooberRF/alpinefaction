#pragma once

#include <algorithm>
#include "toml.hpp"
#include "../rf/save_restore.h"
#include "../rf/geometry.h"
#include "../rf/player/player.h"
#include "../rf/math/quaternion.h"

namespace asg
{
    struct SavegameHeader
    {
        uint8_t version;
        float game_time;
        std::string mod_name;
        uint8_t num_saved_levels;
        std::vector<std::string> saved_level_filenames;
        std::string current_level_filename;
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
        int32_t entity_host_uid;
        //int16_t clip_x, clip_y, clip_w, clip_h;
        //float fov_h;
        //int32_t field_10;
        //std::string name;
        int16_t player_flags;
        //int16_t field_36;
        int32_t entity_uid;
        //uint8_t entity_type;
        rf::ubyte spew_vector_index;
        rf::Vector3 spew_pos;
        float key_items;
        int32_t view_obj_uid;
        //char weapon_prefs[32];
        rf::Matrix3 fpgun_orient;
        rf::Vector3 fpgun_pos;
        uint8_t grenade_mode;
        //uint8_t game_difficulty;
        //int32_t field_A8;
        //uint8_t flags;
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
        int16_t life;
        int16_t armor;
        rf::ShortVector pos;
        rf::ShortVector vel;
        //char friendliness;
        int friendliness;
        //char host_tag_handle;
        int host_tag_handle;
        rf::Matrix3 orient;
        int obj_flags;
        int host_uid;
        rf::Vector3 ang_momentum;
        int physics_flags;
        std::string skin_name;
    };

    struct SavegameEntityDataBlock
    {
        SavegameObjectDataBlock obj;
        uint8_t current_primary_weapon;
        uint8_t current_secondary_weapon;
        int16_t info_index;
        int16_t weapons_clip_ammo[32];
        int16_t weapons_ammo[32];
        //char field_C0[64];
        int32_t possesed_weapons_bitfield;
        //char hate_list[32];
        std::vector<int> hate_list;
        //uint8_t hate_list_size;
        uint8_t ai_mode;
        uint8_t ai_submode;
        uint8_t move_mode;
        int32_t ai_mode_parm_0;
        int32_t ai_mode_parm_1;
        int32_t target_uid;
        int32_t look_at_uid;
        int32_t shoot_at_uid;
        // char field_13C[8];
        // int16_t field_144;
        // int16_t field_146;
        // int16_t UnkCompressedVec_148[3];
        // int16_t field_14E;
        // int16_t field_150;
        // int16_t field_152;
        // int16_t field_154;
        // int16_t field_156;
        // int16_t UnkCompressedVector_158[3];
        // uint8_t field_15E;
        // uint8_t field_15F;
        // int16_t UnkCompressedVec_160[3];
        // int16_t UnkCompressedVec_166[3];
        rf::Vector3 ci_rot;
        rf::Vector3 ci_move;
        // uint8_t field_184;
        // uint8_t pad185;
        // uint8_t pad186;
        // uint8_t pad187;
        int32_t corpse_carry_uid;
        // int32_t field_18C;
        // rf::Vector3 field_190;
        int32_t ai_flags;
        rf::Vector3 eye_pos;
        rf::Matrix3 eye_orient;
        int32_t entity_flags;
        int32_t entity_flags2;
        rf::Vector3 control_data_phb;
        rf::Vector3 control_data_eye_phb;
        rf::Vector3 control_data_local_vel;
        //int16_t field_1FC;
        //int16_t field_1FE;
        //int16_t field_200;
        //int16_t field_202;
        //int32_t field_204;
        // int32_t field_208;
        // int32_t field_20C;
        // int32_t field_210;
        // uint8_t field_214;
        // uint8_t field_215;
        // uint8_t field_216;
        // uint8_t climbing_region_index;
    };

    struct SavegameItemDataBlock
    {
        SavegameObjectDataBlock obj;
        int respawn_timer;
        int alpha;
        int create_time;
        int flags;
        int item_cls_id;
    };

    struct SavegameClutterDataBlock
    {
        SavegameObjectDataBlock obj;
        int delayed_kill_timestamp;
        int corpse_create_timestamp;
        std::vector<int> links;
    };

    struct SavegameTriggerDataBlock
    {
        int uid;
        rf::ShortVector pos;
        // 2 bytes here, unsure what for
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
        bool when_any_dead;
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
        int mover_flags;
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
        int next_weapon_uid;
        int prev_weapon_uid;
        int info_index;
        float life_left_seconds;
        int fly_sound_handle;
        int light_handle;
        int weapon_flags;
        float flicker_index;
        int sticky_host_uid;
        rf::Vector3 sticky_host_pos_offset;
        rf::Matrix3 sticky_host_orient;
        int friendliness;
        int target_uid;
        int scan_time;
        float pierce_power_left;
        float thrust_left;
        int t_flags;
        rf::Vector3 water_hit_point;
        rf::Vector3 firing_pos;
    };

    struct SavegameLevelCorpseDataBlock
    {
        SavegameObjectDataBlock obj; // transform, uid, etc.
        float create_time;
        float lifetime_seconds;
        int corpse_flags;
        int entity_type;
        std::string pose_name;
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



    SavegameData build_savegame_data(rf::Player* pp);

}
