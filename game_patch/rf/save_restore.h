#pragma once

#include <patch_common/MemUtils.h>

namespace rf {
    struct Player;
}

namespace rf::sr {
    constexpr uint8_t MAX_GOAL_CREATE_EVENTS = 8;
    constexpr uint8_t MAX_ALARM_SIREN_EVENTS = 8;
    constexpr uint8_t MAX_WHEN_DEAD_EVENTS = 20;
    constexpr uint8_t MAX_CYCLIC_TIMER_EVENTS = 12;
    constexpr uint8_t MAX_MAKE_INVULNERABLE_EVENTS = 8;
    constexpr uint8_t MAX_OTHER_EVENTS = 32;
    constexpr uint8_t MAX_EMITTERS = 16;
    constexpr uint8_t MAX_DECALS = 64;
    constexpr uint8_t MAX_ENTITIES = 64;
    constexpr uint8_t MAX_ITEMS = 64;
    constexpr uint16_t MAX_CLUTTERS = 512;
    constexpr uint8_t MAX_TRIGGERS = 96;
    constexpr uint8_t MAX_MOVERS = 128;
    constexpr uint8_t MAX_PUSH_REGIONS = 32;
    constexpr uint8_t MAX_PERSISTENT_GOALS = 10;
    constexpr uint8_t MAX_WEAPONS = 8;
    constexpr uint8_t MAX_BLOOD_POOLS = 16;
    constexpr uint8_t MAX_CORPSES = 32;
    constexpr uint8_t MAX_GEOMOD_CRATERS = 128;
    constexpr uint8_t MAX_KILLED_ROOMS = 128;
    constexpr uint8_t MAX_DELETED_EVENTS = 32;

    struct LevelData
    {
        std::byte unk[0x18708];
        uint8_t num_goal_create_events;
        uint8_t num_alarm_siren_events;
        uint8_t num_when_dead_events;
        uint8_t num_cyclic_timer_events;
        uint8_t num_make_invulnerable_events;
        uint8_t num_other_events;
        uint8_t num_emitters;
        uint8_t num_decals;
        uint8_t num_entities;
        uint8_t num_items;
        uint16_t num_clutter;
        uint8_t num_triggers;
        uint8_t num_keyframes;
        uint8_t num_push_regions;
        uint8_t num_persistent_goals;
        uint8_t num_weapons;
        uint8_t num_blood_pools;
        uint8_t num_corpse;
        uint8_t num_geomod_craters;
        uint8_t num_killed_room_ids;
        uint8_t num_dead_entities;
        uint8_t num_deleted_events;
    };
    static_assert(sizeof(LevelData) == 0x18720);

    static auto& save_game = addr_as_ref<bool(const char *filename, Player *pp)>(0x004B3B30);
    static auto& can_save_now = addr_as_ref<bool()>(0x004B61A0);
    // The Quick Load control: queues a load of the quick save, in single player.
    static auto& quick_load = addr_as_ref<void()>(0x004B6180);
    // The save's current level file (32 bytes) and title (48 bytes); game_time receives the float's bits
    static auto& read_header =
        addr_as_ref<bool(const char* filename, char* level, int* game_time, char* title)>(0x004B3850);

    static auto& savegame_path = addr_as_ref<char[260]>(0x007DB3EC);
    // The save the death popup offers to load: set by sr_save_game and the load menu, cleared by sr_init
    static auto& last_save_filename = addr_as_ref<char[256]>(0x007DAFEC);

    // The HUD message log: an 80-entry ring in the save data, read by the message log panel
    struct LoggedHudMessage
    {
        char message[256];
        int time_string;
        int16_t persona_index;
        int16_t display_height;
    };
    static_assert(sizeof(LoggedHudMessage) == 0x108);

    static auto& logged_message_newest_index = addr_as_ref<int()>(0x004B57C0);
    static auto& logged_message_get = addr_as_ref<LoggedHudMessage*(int index)>(0x004B5800);
}
