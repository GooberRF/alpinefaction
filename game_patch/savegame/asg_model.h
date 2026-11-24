#pragma once

#include <array>
#include <climits>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "../rf/math/vector.h"
#include "../rf/math/matrix.h"

// In-memory form of an Alpine savegame (.asg). Timestamps are ms relative to the game clock at capture, object
// references are uids.
namespace asg
{
    constexpr int32_t ts_invalid = INT32_MIN;
    constexpr int32_t uid_none = -1;

    struct Header
    {
        std::string writer_version;
        std::string mod_name;
        std::string current_level;
        std::string level_title;
        uint8_t difficulty = 0;
        float game_time = 0.0f;
        uint64_t saved_at = 0; // unix seconds UTC
    };

    struct HudMessage
    {
        std::string text;
        int32_t time_string = 0;
        int16_t persona = -1;
        int16_t display_height = 0;
    };

    struct GameState
    {
        float level_time = 0.0f;
        float level_time_left = 0.0f;
        int32_t newest_message_index = 0;
        int32_t messages_total_height = 0;
        std::vector<HudMessage> messages;
        std::string music;
    };

    struct PlayerState
    {
        int32_t entity_uid = uid_none;
        uint32_t player_flags = 0;
        std::vector<int32_t> key_items; // indices of held key items
        int32_t view_obj_uid = uid_none;
        std::vector<std::string> weapon_prefs; // weapon names, most preferred first
        rf::Vector3 fpgun_pos;
        rf::Matrix3 fpgun_orient;
        uint8_t spew_vector_index = 0;
        rf::Vector3 spew_pos;
        uint8_t grenade_mode = 0;
        bool show_silencer = false;
        bool remote_charge_in_hand = false;
        bool undercover_active = false;
        int32_t undercover_team = 0;
        int32_t undercover_cover_id = 0;
        bool ai_high_flag = false;
        bool collides_with_world = true;
        int32_t vehicle_uid = uid_none;
        int32_t vehicle_seat = -1;
        bool blackout_active = false;
        float blackout_alpha = 0.0f;
        float blackout_time_sec = 0.0f;
    };

    struct ObjectBase
    {
        int32_t uid = uid_none;
        uint8_t origin = 0;
        std::string cls;
        int32_t parent_uid = uid_none;
        int32_t host_uid = uid_none;
        rf::Vector3 pos;
        rf::Matrix3 orient;
        rf::Vector3 vel;
        rf::Vector3 ang_momentum;
        float life = 0.0f;
        float armor = 0.0f;
        uint32_t obj_flags = 0;
        uint32_t physics_flags = 0;
        int32_t friendliness = 0;
        int32_t host_tag_handle = -1;
        std::string skin;
    };

    struct EntityWeaponState
    {
        std::string weapon;
        int32_t clip_ammo = 0;
        int32_t ammo = 0;
        bool possessed = false;
        bool custom_mode = false;
        bool silencer = false;
    };

    struct EntityRecord
    {
        ObjectBase base;
        std::string primary_weapon;
        std::string secondary_weapon;
        std::vector<EntityWeaponState> weapons;
        std::vector<int32_t> hate_list; // uids
        int32_t ai_mode = 0;
        int32_t ai_submode = 0;
        int32_t move_mode = 0;
        int32_t ai_mode_parm_0 = 0;
        int32_t ai_mode_parm_1 = 0;
        int32_t target_uid = uid_none;
        int32_t look_at_uid = uid_none;
        int32_t shoot_at_uid = uid_none;
        uint32_t ai_flags = 0;
        float ai_healing_left = 0.0f;
        rf::Vector3 ai_steering_vector;
        int32_t corpse_carry_uid = uid_none;
        std::array<int16_t, 4> path_node_indices{-1, -1, -1, -1};
        int16_t path_previous_goal = -1;
        int16_t path_current_goal = -1;
        rf::Vector3 path_end_pos;
        int16_t path_adjacent_node1_index = -1;
        int16_t path_adjacent_node2_index = -1;
        int16_t path_waypoint_list_index = -1;
        int16_t path_goal_waypoint_index = -1;
        uint8_t path_direction = 0;
        uint8_t path_flags = 0;
        rf::Vector3 path_turn_towards_pos;
        int32_t path_follow_style = 0;
        rf::Vector3 path_start_pos;
        rf::Vector3 path_goal_pos;
        rf::Vector3 ci_rot;
        rf::Vector3 ci_move;
        bool ci_time_relative_mouse_delta_disabled = false;
        rf::Vector3 control_phb;
        rf::Vector3 control_eye_phb;
        rf::Vector3 control_local_vel;
        rf::Vector3 eye_pos;
        rf::Matrix3 eye_orient;
        uint32_t entity_flags = 0;
        uint32_t entity_flags2 = 0;
        int16_t current_state_anim = -1;
        int16_t next_state_anim = -1;
        int16_t last_custom_anim_index = -1;
        int16_t state_anim_22_index = -1;
        float total_transition_time = 0.0f;
        float elapsed_transition_time = 0.0f;
        int32_t driller_rot_angle = 0;
        float driller_rot_speed = 0.0f;
        int32_t driller_geomod_count = 0;
        uint8_t current_speed = 0;
        bool on_fire = false;
        int32_t climb_region_index = -1; // into rf::level.ladders
        std::string item_drop_override;
    };

    struct ItemRecord
    {
        ObjectBase base;
        int32_t respawn_time_ms = 0;
        int32_t respawn_timer = ts_invalid;
        float alpha = 0.0f;
        float create_time = 0.0f;
        uint32_t item_flags = 0;
    };

    struct ClutterRecord
    {
        ObjectBase base;
        int32_t delayed_kill_timer = ts_invalid;
        int32_t delayed_kill_sound = -1;
        int32_t emitter_kill_timer = ts_invalid;
        int32_t corpse_create_timer = ts_invalid;
        int32_t skin_index = 0;
        bool hidden = false;
    };

    struct TriggerRecord
    {
        ObjectBase base;
        int32_t count = 0;
        float time_last_activated = 0.0f;
        uint32_t trigger_flags = 0;
        int32_t activator_uid = uid_none;
        int32_t button_active_timer = ts_invalid;
        int32_t inside_timer = ts_invalid;
        int32_t reset_timer = ts_invalid;
    };

    struct MoverRecord
    {
        ObjectBase base;
        float rot_cur_pos = 0.0f;
        int32_t start_at_keyframe = 0;
        int32_t stop_at_keyframe = 0;
        int32_t stop_completely_at_keyframe = 0;
        uint32_t mover_flags = 0;
        float travel_time_seconds = 0.0f;
        float rotation_travel_time_seconds = 0.0f;
        int32_t wait_timer = ts_invalid;
        int32_t trigger_uid = uid_none;
        float dist_travelled = 0.0f;
        float cur_vel = 0.0f;
    };

    struct WeaponRecord
    {
        ObjectBase base;
        float life_left_seconds = 0.0f;
        uint32_t weapon_flags = 0;
        int32_t sticky_host_uid = uid_none;
        rf::Vector3 sticky_host_pos_offset;
        rf::Matrix3 sticky_host_orient;
        uint8_t weap_friendliness = 0;
        int32_t target_uid = uid_none;
        float pierce_power_left = 0.0f;
        float thrust_left = 0.0f;
        rf::Vector3 firing_pos;
    };

    struct CollisionSphere
    {
        rf::Vector3 center;
        float radius = 0.0f;
        float spring_const = 0.0f;
        int32_t spring_length = 0;
    };

    struct RagdollBone
    {
        rf::Matrix3 orient;
        rf::Vector3 pos;
    };

    struct CorpseRecord
    {
        ObjectBase base;
        float create_time = 0.0f;
        float lifetime_seconds = 0.0f;
        uint32_t corpse_flags = 0;
        std::string entity_cls;
        int32_t source_entity_uid = uid_none;
        std::string mesh_name;
        int32_t emitter_kill_timer = ts_invalid;
        float body_temp = 0.0f;
        int32_t state_anim = -1;
        int32_t action_anim = -1;
        int32_t drop_anim = -1;
        int32_t carry_anim = -1;
        int32_t corpse_pose = -1;
        std::string helmet_name;
        int32_t item_uid = uid_none;
        float mass = 0.0f;
        float radius = 0.0f;
        std::vector<CollisionSphere> cspheres;
        // A ragdoll's pose, empty if it has none: each body's world transform, then each bone's in its body
        std::vector<RagdollBone> ragdoll_bodies;
        std::vector<RagdollBone> ragdoll_bones;
    };

    struct EventLink
    {
        uint8_t kind = 0; // 0 = object uid, 1 = raw uid
        int32_t uid = uid_none;
    };

    struct EventRecord
    {
        int32_t uid = uid_none;
        uint8_t origin = 0;
        int32_t event_type = -1;
        bool enabled = true;
        bool on_state = false;
        int32_t delay = ts_invalid; // pending delayed activation
        float delay_seconds = 0.0f;
        int32_t activated_by = uid_none; // Event::triggered_by_handle
        int32_t triggered_by = uid_none; // Event::trigger_handle
        std::vector<EventLink> links;
        std::vector<uint8_t> payload; // per event type, written by the event capture code
    };

    struct DecalRecord
    {
        rf::Vector3 pos;
        rf::Matrix3 orient;
        rf::Vector3 extents;
        uint32_t decal_flags = 0;
        uint8_t alpha = 255;
        float tiling_scale = 1.0f;
        std::string bitmap;
    };

    struct BloodPoolRecord
    {
        rf::Vector3 pos;
        rf::Matrix3 orient;
        uint8_t red = 0;
        uint8_t green = 0;
        uint8_t blue = 0;
        uint8_t alpha = 255;
    };

    struct EmitterRecord
    {
        uint8_t kind = 0; // 0 = particle emitter, 1 = bolt emitter
        int32_t uid = uid_none;
        bool active = false;
        float time_to_change = 0.0f;
        float current_state_time = 0.0f;
    };

    struct PushRegionRecord
    {
        int32_t uid = uid_none;
        bool active = false;
    };

    struct LightRecord
    {
        int32_t uid = uid_none;
        bool on = false;
        rf::Vector3 color;
        rf::Vector3 hue; // the level light's, which Set_Light_Color changes and flickering re-derives from
    };

    struct GoalRecord
    {
        std::string name;
        int32_t count = 0;
        int32_t initial_count = 0;
    };

    // Stock crater record (rf::GeomodCraterRecord), kept bit for bit: the engine stores it compressed
    struct CraterRecord
    {
        uint16_t shape_index = 0;
        uint16_t flags = 0;
        int32_t room_index = -1;
        std::array<int16_t, 3> pos{};
        std::array<int16_t, 3> hit_normal{};
        std::array<int16_t, 4> orient{};
        float scale = 1.0f;
    };

    // The rf::GeomodParams of a confirmed RF2-style carve, which is replayed through geomod_init
    struct Rf2GeomodRecord
    {
        int32_t shape_index = 0;
        int32_t room_index = -1;
        rf::Vector3 pos;
        rf::Matrix3 orient;
        int32_t flags = 0;
        float scale = 1.0f;
        rf::Vector3 hit_normal;
        rf::Vector3 field_4c;
        rf::Vector3 field_58;
    };

    struct RoomLifeRecord
    {
        int32_t room_uid = uid_none;
        float life = 0.0f;
    };

    struct LiquidDepthRecord
    {
        int32_t room_uid = uid_none;
        float depth = 0.0f;
        bool has_pending = false;
        float target_depth = 0.0f;
        float remaining_duration = 0.0f;
    };

    struct RoomState // ROOM
    {
        std::vector<int32_t> killed_room_uids;
        std::vector<LiquidDepthRecord> liquid_depths;
        // The level solid's bbox, which stock load puts back after replaying craters (0x004B4AD5)
        rf::Vector3 world_bbox_min;
        rf::Vector3 world_bbox_max;
    };

    struct EntityState // ENTS
    {
        std::vector<EntityRecord> entities;
        std::vector<int32_t> dead_uids;
    };

    struct TaggedBlob // WRLD block
    {
        uint32_t tag = 0;
        std::vector<uint8_t> data;
    };

    struct AlpineObjectRecord // ALPO
    {
        uint8_t kind = 0;
        int32_t uid = uid_none;
        std::vector<uint8_t> payload;
    };

    // An absent chunk keeps the level-file state; a present one (even empty) is authoritative
    struct LevelState
    {
        std::string filename;
        uint32_t rfl_checksum = 0;
        float level_time = 0.0f;
        std::optional<RoomState> rooms;
        std::optional<std::vector<CraterRecord>> craters;
        std::optional<std::vector<Rf2GeomodRecord>> rf2_geomods;
        std::optional<std::vector<RoomLifeRecord>> breakable_rooms;
        EntityState entities;
        std::optional<std::vector<ItemRecord>> items;
        std::optional<std::vector<ClutterRecord>> clutter;
        std::optional<std::vector<TriggerRecord>> triggers;
        std::optional<std::vector<MoverRecord>> movers;
        std::optional<std::vector<WeaponRecord>> weapons;
        std::optional<std::vector<CorpseRecord>> corpses;
        std::optional<std::vector<EventRecord>> events;
        std::optional<std::vector<DecalRecord>> decals;
        std::optional<std::vector<BloodPoolRecord>> blood_pools;
        std::optional<std::vector<EmitterRecord>> emitters;
        std::optional<std::vector<PushRegionRecord>> push_regions;
        std::optional<std::vector<LightRecord>> lights;
        std::optional<std::vector<GoalRecord>> goals;
        std::optional<std::vector<TaggedBlob>> world;
        std::optional<std::vector<AlpineObjectRecord>> alpine_objects;
    };

    struct SaveModel
    {
        Header header;
        GameState game;
        PlayerState player;
        std::vector<LevelState> levels;
    };
}
