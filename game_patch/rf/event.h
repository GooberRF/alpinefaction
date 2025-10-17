#pragma once

#include <string>
#include <vector>
#include <sstream>
#include <optional>
#include <cstddef>
#include "object.h"
#include "level.h"
#include "misc.h"
#include "multi.h"
#include "hud.h"
#include "entity.h"
#include "trigger.h"
#include "../main/main.h"
#include "player/player.h"
#include "os/timestamp.h"

namespace rf
{
    enum class SetVarOpts : int
    {
        delay,
        int1,
        int2,
        float1,
        float2,
        bool1,
        bool2,
        str1,
        str2
    };
}

namespace std
{
    template<>
    struct hash<rf::SetVarOpts> // Allow SetVarOpts as a key in std::unordered_map
    {
        std::size_t operator()(const rf::SetVarOpts& opt) const noexcept
        {
            return static_cast<std::size_t>(opt);
        }
    };
} // namespace std

namespace rf
{
    enum EventFlags : int
    {
        EVENT_FLAG_PAUSED = 0x1,
#ifdef DASH_FACTION
        EVENT_FLAG_QUEUED = 0x2
#endif
    };
#pragma pack(push, 1)
    struct Event : Object
    {
        int event_type;
        float delay_seconds;
        Timestamp delay_timestamp;
        VArray<int> links;
        int triggered_by_handle;
        int trigger_handle;
        int event_flags;
        bool delayed_msg;

        // handler storage, defined in event.cpp
        static std::unordered_map<const Event*, std::unordered_map<rf::SetVarOpts, std::function<void(Event*, const std::string&)>>>
            variable_handler_storage;



        // register variable handlers (AF new) plus default event initialization (does nothing)
        // safe to override, but include call to base struct initialize for var handler registration
        virtual void initialize()
        {
            register_variable_handlers(); // possibly codeinjection this into the stock init function?
            AddrCaller{0x004B8CD0}.this_call(this);
        };

        // default event turning on (switch based on some default event types)
        // safe to override
        virtual void turn_on()
        {
            AddrCaller{0x004B9070}.this_call(this);
        };

        // default event turning off (switch based on some default event types)
        // safe to override
        virtual void turn_off()
        {
            AddrCaller{0x004B9F80}.this_call(this);
        };

        // default event processing, handles delays (set in activate) and switch on some default event types
        // if overridden, delays need to be handled otherwise event won't work with delay specified (just like some stock events)
        virtual void process()
        {
            AddrCaller{0x004B8CE0}.this_call(this);
        };

        // game does not allocate - only usable in new code
        virtual void activate(int trigger_handle, int triggered_by_handle, bool on)
        {
            do_activate(trigger_handle, triggered_by_handle, on);
        }

        // game does not allocate - only usable in new code
        virtual void activate_links(int trigger_handle, int triggered_by_handle, bool on)
        {
            do_activate_links(trigger_handle, triggered_by_handle, on);
        }

    protected:
        // default internal event activation, handles delays, turn on/off, and event forwarding
        // if overridden, delays need to be handled otherwise event won't work with delay specified
        virtual void do_activate(int trigger_handle, int triggered_by_handle, bool on)
        {
            AddrCaller{0x004B8B70}.this_call(this, trigger_handle, triggered_by_handle, on);
        }

        // default event link activation, handles sending on/off signals to links
        // safe to override, but be careful - this is standard behaviour for all stock events
        virtual void do_activate_links(int trigger_handle, int triggered_by_handle, bool on)
        {
            AddrCaller{0x004B8B00}.this_call(this, trigger_handle, triggered_by_handle, on);
        }

        virtual void register_variable_handlers()
        {
            auto& handlers = variable_handler_storage[this];

            handlers[SetVarOpts::delay] = [](Event* event, const std::string& value) {
                event->delay_seconds = std::stof(value);
                xlog::info("apply_var: delay set to {}", event->delay_seconds);
            };
        }

    public:
        void apply_var(SetVarOpts var, const std::string& value)
        {
            auto it = variable_handler_storage.find(this);
            if (it != variable_handler_storage.end()) {
                auto& handlers = it->second;
                auto handler_it = handlers.find(var);
                if (handler_it != handlers.end()) {
                    try {
                        handler_it->second(this, value);
                    }
                    catch (const std::exception& ex) {
                        xlog::error("apply_var: Failed to set var={} with value={} - {}", static_cast<int>(var), value,
                                    ex.what());
                    }
                }
                else {
                    xlog::warn("apply_var: Unsupported var={} for Event", static_cast<int>(var));
                }
            }
            else {
                xlog::warn("apply_var: No handlers registered for Event");
            }
        }


    };
    static_assert(sizeof(Event) == 0x2B5);

    struct GenericEvent : Event
    {
        // in original code Event is aligned to 1 byte and size 0x2B5, but GenericEvent is aligned to 4 bytes
        char padding[3];
        char event_specific_data[24];
    };
    static_assert(sizeof(GenericEvent) == 0x2D0);

    struct PersistentGoalEvent
    {
        rf::String name;
        int initial_count;
        int count;
    };
    static_assert(sizeof(PersistentGoalEvent) == 0x10);
#pragma pack(pop)

    static auto& event_lookup_from_uid = addr_as_ref<Event*(int uid)>(0x004B6820);
    static auto& event_lookup_from_handle = addr_as_ref<Event*(int handle)>(0x004B6800);
    static auto& event_lookup_type = addr_as_ref<int(String* class_name)>(0x004BD700);
    static auto& event_create = addr_as_ref<Event*(const rf::Vector3* pos, int event_type)>(0x004B6870);
    static auto& event_delete = addr_as_ref<void(rf::Event*)>(0x004B67C0);
    static auto& event_add_link = addr_as_ref<void(int event_handle, int handle)>(0x004B6790);
    static auto& event_signal_on =
        addr_as_ref<void(int link_handle, int trigger_handle, int triggered_by_handle)>(0x004B65C0);
    static auto& event_signal_off =
        addr_as_ref<void(int link_handle, int trigger_handle, int triggered_by_handle, bool skip_movers)>(0x004B6640);
    static auto& event_find_named_event = addr_as_ref<GenericEvent*(String* name)>(0x004BD740);
    static auto& event_lookup_persistent_goal_event = addr_as_ref<PersistentGoalEvent*(const char* name)>(0x004B8680);
    static auto& event_list = addr_as_ref<VArray<Event*>>(0x00856470);
    static auto& event_type_forwards_messages = addr_as_ref<bool(int event_type)>(0x004B8C40);
    
    static auto& Event__process = addr_as_ref<void __fastcall(rf::Event*)>(0x004B8CE0);

    // applies only to game, not level editor
    // original game events use a different order entirely in level editor, AF events in RED use the same
    enum class EventType : int
    {
        Play_Sound, // 0
        Slay_Object,
        Remove_Object,
        Invert,
        Teleport,
        Goto,
        Goto_Player,
        Look_At,
        Shoot_At,
        Shoot_Once,
        Explode,
        Play_Animation,
        Play_Custom_Animation,
        Heal,
        Armor,
        Message,
        When_Dead,
        Continuous_Damage,
        Shake_Player,
        Give_Item_To_Player,
        Cyclic_Timer,
        Switch_Model,
        Load_Level,
        Spawn_Object,
        Make_Invulnerable,
        Make_Walk,
        Make_Fly,
        Drop_Point_Marker,
        Follow_Waypoints,
        Follow_Player,
        Set_Friendliness,
        Set_Light_State,
        Switch,
        Swap_Textures,
        Set_AI_Mode,
        Goal_Create,
        Goal_Check,
        Goal_Set,
        Attack,
        Particle_State,
        Set_Liquid_Depth,
        Music_Start,
        Music_Stop,
        Bolt_State,
        Set_Gravity,
        Alarm_Siren,
        Alarm,
        Go_Undercover,
        Delay,
        Monitor_State,
        UnHide,
        Push_Region_State,
        When_Hit,
        Headlamp_State,
        Item_Pickup_State,
        Cutscene,
        Strip_Player_Weapons,
        Fog_State,
        Detach,
        Skybox_State,
        Force_Monitor_Update,
        Black_Out_Player,
        Turn_Off_Physics,
        Teleport_Player,
        Holster_Weapon,
        Holster_Player_Weapon,
        Modify_Rotating_Mover,
        Clear_Endgame_If_Killed,
        Win_PS2_Demo,
        Enable_Navpoint,
        Play_Vclip,
        Endgame,
        Mover_Pause,
        Countdown_Begin,
        Countdown_End,
        When_Countdown_Over,
        Activate_Capek_Shield,
        When_Enter_Vehicle,
        When_Try_Exit_Vehicle,
        Fire_Weapon_No_Anim,
        Never_Leave_Vehicle,
        Drop_Weapon,
        Ignite_Entity,
        When_Cutscene_Over,
        When_Countdown_Reaches,
        Display_Fullscreen_Image,
        Defuse_Nuke,
        When_Life_Reaches,
        When_Armor_Reaches,
        Reverse_Mover, // 89
        // 90 - 99 unused
        Set_Variable = 100, // alpine events begin at 100
        Clone_Entity,
        Set_Player_World_Collide,
        Switch_Random,
        Difficulty_Gate,
        HUD_Message,
        Play_Video,
        Set_Level_Hardness,
		Sequence,
        Clear_Queued,
		Remove_Link,
        Route_Node,
        Add_Link,
        Valid_Gate,
        Goal_Math,
        Goal_Gate,
        Scope_Gate,
        Inside_Gate,
        Anchor_Marker,
        Force_Unhide,
        Set_Difficulty,
        Set_Fog_Far_Clip,
        AF_When_Dead,
        Gametype_Gate,
        When_Picked_Up,
        Set_Skybox,
        Set_Life,
        Set_Debris,
        Set_Fog_Color,
        Set_Entity_Flag,
        AF_Teleport_Player,
        Set_Item_Drop,
        AF_Heal,
        Anchor_Marker_Orient,
        Light_State,
        World_HUD_Sprite,
        Set_Light_Color,
        Capture_Point_Handler
    };

    // int to EventType
    inline EventType int_to_event_type(int id)
    {
        return static_cast<EventType>(id);
    }

    // EventType to int
    inline int event_type_to_int(EventType eventType)
    {
        return static_cast<int>(eventType);
    }

    std::vector<rf::Event*> find_all_events_by_type(rf::EventType event_type);
    bool check_if_event_is_type(rf::Event* event, rf::EventType type);
    bool check_if_object_is_event_type(rf::Object* object, rf::EventType type);
    void activate_all_events_of_type(rf::EventType event_type, int trigger_handle, int triggered_by_handle, bool on);

}
