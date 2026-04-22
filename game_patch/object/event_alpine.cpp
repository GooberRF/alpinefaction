#include <patch_common/CodeInjection.h>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/AsmWriter.h>
#include <common/version/version.h>
#include <xlog/xlog.h>
#include <cassert>
#include <unordered_set>
#include "event_alpine.h"
#include "../hud/hud_world.h"
#include "../misc/misc.h"
#include "../misc/level.h"
#include "../misc/achievements.h"
#include "../multi/gametype.h"
#include "../rf/object.h"
#include "../rf/event.h"
#include "../rf/entity.h"
#include "../rf/level.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/os/console.h"
#include "../rf/os/timestamp.h"
#include "../os/console.h"

namespace rf
{
    // var handler storage
    std::unordered_map<const Event*, std::unordered_map<SetVarOpts, std::function<void(Event*, const std::string&)>>>
    Event::variable_handler_storage = {};

    void activate_all_events_of_type(EventType event_type, int trigger_handle, int triggered_by_handle, bool on)
    {
        auto event_list = find_all_events_by_type(event_type);
        for (auto* event : event_list) {
            if (event) {
                event->activate(trigger_handle, triggered_by_handle, on);
            }
        }
    }

} // namespace rf

FunHook<int(const rf::String* name)> event_lookup_type_hook{
    0x004BD700,
    [](const rf::String* name) {
        if (af_rfl_version(rf::level.version)) {
            // map of alpine events and corresponding IDs
            static const std::unordered_map<std::string_view, int> custom_event_ids{
                {"Set_Variable", 100},
                {"Clone_Entity", 101},
                {"Set_Player_World_Collide", 102},
                {"Switch_Random", 103},
                {"Difficulty_Gate", 104},
                {"HUD_Message", 105},
                {"Play_Video", 106},
                {"Set_Level_Hardness", 107},
                {"Sequence", 108},
                {"Clear_Queued", 109},
                {"Remove_Link", 110},
                {"Route_Node", 111},
                {"Add_Link", 112},
                {"Valid_Gate", 113},
                {"Goal_Math", 114},
                {"Goal_Gate", 115},
                {"Scope_Gate", 116},
                {"Inside_Gate", 117},
                {"Anchor_Marker", 118},
                {"Force_Unhide", 119},
                {"Set_Difficulty", 120},
                {"Set_Fog_Far_Clip", 121},
                {"AF_When_Dead", 122},
                {"Gametype_Gate", 123},
                {"When_Picked_Up", 124},
                {"Set_Skybox", 125},
                {"Set_Life", 126},
                {"Set_Debris", 127},
                {"Set_Fog_Color", 128},
                {"Set_Entity_Flag", 129},
                {"AF_Teleport_Player", 130},
                {"Set_Item_Drop", 131},
                {"AF_Heal", 132},
                {"Anchor_Marker_Orient", 133},
                {"Light_State", 134},
                {"World_HUD_Sprite", 135},
                {"Set_Light_Color", 136},
                {"Capture_Point_Handler", 137},
                {"Respawn_Point_State", 138},
                {"Modify_Respawn_Point", 139},
                {"When_Captured", 140},
                {"Set_Capture_Point_Owner", 141},
                {"Owner_Gate", 142},
                {"Set_Gameplay_Rule", 143},
                {"When_Round_Ends", 144},
                {"Mesh_Animate", 145},
                {"Mesh_Set_Texture", 146},
                {"Mesh_Set_Collision", 147},
                {"AF_Fullscreen_Image", 148},
                {"AF_Fullscreen_Color", 149},
                {"Unhide_Glare", 150},
                {"Gas_Region_State", 151},
                {"Modify_Gas_Region", 152},
                {"Resize_Gas_Region", 153},
            };

            auto it = custom_event_ids.find(name->c_str());
            if (it != custom_event_ids.end()) {
                return it->second; // event ID
            }

            // handle stock events
            return event_lookup_type_hook.call_target(name);
        }
        // handle non-Alpine levels
        else {
            return event_lookup_type_hook.call_target(name);
        }
    }
};

FunHook<rf::Event*(int event_type)> event_allocate_hook{
    0x004B69D0,
    [](int event_type) {
        if (af_rfl_version(rf::level.version)) {
            using AllocFunc = std::function<rf::Event*()>;

            // map of allocators
            static const std::unordered_map<int, AllocFunc> event_allocators{
                {100, []() { return new EventSetVar(); }},
                {101, []() { return new EventCloneEntity(); }},
                {102, []() { return new EventSetCollisionPlayer(); }},
                {103, []() { return new EventSwitchRandom(); }},
                {104, []() { return new EventDifficultyGate(); }},
                {105, []() { return new EventHUDMessage(); }},
                {106, []() { return new EventPlayVideo(); }},
                {107, []() { return new EventSetLevelHardness(); }},
                {108, []() { return new EventSequence(); }},
                {109, []() { return new EventClearQueued(); }},
                {110, []() { return new EventRemoveLink(); }},
                {111, []() { return new EventRouteNode(); }},
                {112, []() { return new EventAddLink(); }},
                {113, []() { return new EventValidGate(); }},
                {114, []() { return new EventGoalMath(); }},
                {115, []() { return new EventGoalGate(); }},
                {116, []() { return new EventScopeGate(); }},
                {117, []() { return new EventInsideGate(); }},
                {118, []() { return new EventAnchorMarker(); }},
                {119, []() { return new EventForceUnhide(); }},
                {120, []() { return new EventSetDifficulty(); }},
                {121, []() { return new EventSetFogFarClip(); }},
                {122, []() { return new EventAFWhenDead(); }},
                {123, []() { return new EventGametypeGate(); }},
                {124, []() { return new EventWhenPickedUp(); }},
                {125, []() { return new EventSetSkybox(); }},
                {126, []() { return new EventSetLife(); }},
                {127, []() { return new EventSetDebris(); }},
                {128, []() { return new EventSetFogColor(); }},
                {129, []() { return new EventSetEntityFlag(); }},
                {130, []() { return new EventAFTeleportPlayer(); }},
                {131, []() { return new EventSetItemDrop(); }},
                {132, []() { return new EventAFHeal(); }},
                {133, []() { return new EventAnchorMarkerOrient(); }},
                {134, []() { return new EventLightState(); }},
                {135, []() { return new EventWorldHUDSprite(); }},
                {136, []() { return new EventSetLightColor(); }},
                {137, []() { return new EventCapturePointHandler(); }},
                {138, []() { return new EventRespawnPointState(); }},
                {139, []() { return new EventModifyRespawnPoint(); }},
                {140, []() { return new EventWhenCaptured(); }},
                {141, []() { return new EventSetCapturePointOwner(); }},
                {142, []() { return new EventOwnerGate(); }},
                {143, []() { return new EventSetGameplayRule(); }},
                {144, []() { return new EventWhenRoundEnds(); }},
                {145, []() { return new EventMeshAnimate(); }},
                {146, []() { return new EventMeshSetTexture(); }},
                {147, []() { return new EventMeshSetCollision(); }},
                {148, []() { return new EventFullscreenImage(); }},
                {149, []() { return new EventFullscreenColor(); }},
                {150, []() { return new EventUnhideGlare(); }},
                {151, []() { return new EventGasRegionState(); }},
                {152, []() { return new EventModifyGasRegion(); }},
                {153, []() { return new EventResizeGasRegion(); }},
            };

            // find type and allocate
            auto it = event_allocators.find(event_type);
            if (it != event_allocators.end()) {
                auto* event = it->second();
                event->initialize();
                return event;
            }

            // handle stock events
            return event_allocate_hook.call_target(event_type);
        }
        // handle non-Alpine levels
        else {
            return event_allocate_hook.call_target(event_type);
        }
    }
};

FunHook<void(rf::Event*)> event_deallocate_hook{
    0x004B7750,
    [](rf::Event* eventp) {
        if (af_rfl_version(rf::level.version)) {
            if (!eventp)
                return;

            // Map of deallocators.
            // Note.  `Event::~Event` is not virtual, so use derived types.
            static const std::unordered_map<int, std::function<void(rf::Event*)>> event_deallocators{
                {100, [](rf::Event* e) { delete static_cast<EventSetVar*>(e); }},
                {101, [](rf::Event* e) { delete static_cast<EventCloneEntity*>(e); }},
                {102, [](rf::Event* e) { delete static_cast<EventSetCollisionPlayer*>(e); }},
                {103, [](rf::Event* e) { delete static_cast<EventSwitchRandom*>(e); }},
                {104, [](rf::Event* e) { delete static_cast<EventDifficultyGate*>(e); }},
                {105, [](rf::Event* e) { delete static_cast<EventHUDMessage*>(e); }},
                {106, [](rf::Event* e) { delete static_cast<EventPlayVideo*>(e); }},
                {107, [](rf::Event* e) { delete static_cast<EventSetLevelHardness*>(e); }},
                {108, [](rf::Event* e) { delete static_cast<EventSequence*>(e); }},
                {109, [](rf::Event* e) { delete static_cast<EventClearQueued*>(e); }},
                {110, [](rf::Event* e) { delete static_cast<EventRemoveLink*>(e); }},
                {111, [](rf::Event* e) { delete static_cast<EventRouteNode*>(e); }},
                {112, [](rf::Event* e) { delete static_cast<EventAddLink*>(e); }},
                {113, [](rf::Event* e) { delete static_cast<EventValidGate*>(e); }},
                {114, [](rf::Event* e) { delete static_cast<EventGoalMath*>(e); }},
                {115, [](rf::Event* e) { delete static_cast<EventGoalGate*>(e); }},
                {116, [](rf::Event* e) { delete static_cast<EventScopeGate*>(e); }},
                {117, [](rf::Event* e) { delete static_cast<EventInsideGate*>(e); }},
                {118, [](rf::Event* e) { delete static_cast<EventAnchorMarker*>(e); }},
                {119, [](rf::Event* e) { delete static_cast<EventForceUnhide*>(e); }},
                {120, [](rf::Event* e) { delete static_cast<EventSetDifficulty*>(e); }},
                {121, [](rf::Event* e) { delete static_cast<EventSetFogFarClip*>(e); }},
                {122, [](rf::Event* e) { delete static_cast<EventAFWhenDead*>(e); }},
                {123, [](rf::Event* e) { delete static_cast<EventGametypeGate*>(e); }},
                {124, [](rf::Event* e) { delete static_cast<EventWhenPickedUp*>(e); }},
                {125, [](rf::Event* e) { delete static_cast<EventSetSkybox*>(e); }},
                {126, [](rf::Event* e) { delete static_cast<EventSetLife*>(e); }},
                {127, [](rf::Event* e) { delete static_cast<EventSetDebris*>(e); }},
                {128, [](rf::Event* e) { delete static_cast<EventSetFogColor*>(e); }},
                {129, [](rf::Event* e) { delete static_cast<EventSetEntityFlag*>(e); }},
                {130, [](rf::Event* e) { delete static_cast<EventAFTeleportPlayer*>(e); }},
                {131, [](rf::Event* e) { delete static_cast<EventSetItemDrop*>(e); }},
                {132, [](rf::Event* e) { delete static_cast<EventAFHeal*>(e); }},
                {133, [](rf::Event* e) { delete static_cast<EventAnchorMarkerOrient*>(e); }},
                {134, [](rf::Event* e) { delete static_cast<EventLightState*>(e); }},
                {135, [](rf::Event* e) { delete static_cast<EventWorldHUDSprite*>(e); }},
                {136, [](rf::Event* e) { delete static_cast<EventSetLightColor*>(e); }},
                {137, [](rf::Event* e) { delete static_cast<EventCapturePointHandler*>(e); }},
                {138, [](rf::Event* e) { delete static_cast<EventRespawnPointState*>(e); }},
                {139, [](rf::Event* e) { delete static_cast<EventModifyRespawnPoint*>(e); }},
                {140, [](rf::Event* e) { delete static_cast<EventWhenCaptured*>(e); }},
                {141, [](rf::Event* e) { delete static_cast<EventSetCapturePointOwner*>(e); }},
                {142, [](rf::Event* e) { delete static_cast<EventOwnerGate*>(e); }},
                {143, [](rf::Event* e) { delete static_cast<EventSetGameplayRule*>(e); }},
                {144, [](rf::Event* e) { delete static_cast<EventWhenRoundEnds*>(e); }},
                {145, [](rf::Event* e) { delete static_cast<EventMeshAnimate*>(e); }},
                {146, [](rf::Event* e) { delete static_cast<EventMeshSetTexture*>(e); }},
                {147, [](rf::Event* e) { delete static_cast<EventMeshSetCollision*>(e); }},
                {148, [](rf::Event* e) { delete static_cast<EventFullscreenImage*>(e); }},
                {149, [](rf::Event* e) { delete static_cast<EventFullscreenColor*>(e); }},
                {150, [](rf::Event* e) { delete static_cast<EventUnhideGlare*>(e); }},
                {150, [](rf::Event* e) { delete static_cast<EventUnhideGlare*>(e); }},
                {151, [](rf::Event* e) { delete static_cast<EventGasRegionState*>(e); }},
                {152, [](rf::Event* e) { delete static_cast<EventModifyGasRegion*>(e); }},
                {153, [](rf::Event* e) { delete static_cast<EventResizeGasRegion*>(e); }},
            };

            // find type and deallocate
            auto it = event_deallocators.find(eventp->event_type);
            if (it != event_deallocators.end()) {
                // call deallocator
                it->second(eventp);
            }
            else {
                // handle stock events
                event_deallocate_hook.call_target(eventp);
            }
        }
        // handle non-Alpine levels
        else {
            event_deallocate_hook.call_target(eventp);
        }
    }
};

// alpine events that can't forward messages by default
bool is_forward_exempt(rf::EventType event_type) {
    static const std::unordered_set<rf::EventType> forward_exempt_ids{
        rf::EventType::Set_Variable,
        rf::EventType::Switch_Random,
        rf::EventType::Difficulty_Gate,
        rf::EventType::Sequence,
        rf::EventType::Clear_Queued,
        rf::EventType::Remove_Link,
        rf::EventType::Add_Link,
        rf::EventType::Valid_Gate,
        rf::EventType::Goal_Gate,
        rf::EventType::Scope_Gate,
        rf::EventType::Inside_Gate,
        rf::EventType::AF_When_Dead,
        rf::EventType::Gametype_Gate,
        rf::EventType::When_Picked_Up,
        rf::EventType::Set_Entity_Flag,
        rf::EventType::Light_State,
        rf::EventType::World_HUD_Sprite,
        rf::EventType::Set_Light_Color,
        rf::EventType::Capture_Point_Handler,
        rf::EventType::Set_Capture_Point_Owner,
        rf::EventType::When_Captured,
        rf::EventType::Owner_Gate,
        rf::EventType::When_Round_Ends,
        rf::EventType::Mesh_Animate,
        rf::EventType::Mesh_Set_Texture,
        rf::EventType::Mesh_Set_Collision,
        rf::EventType::AF_Fullscreen_Image,
        rf::EventType::AF_Fullscreen_Color,
        rf::EventType::Unhide_Glare,
        rf::EventType::Gas_Region_State,
        rf::EventType::Modify_Gas_Region,
        rf::EventType::Resize_Gas_Region
    };

    // AF_Heal should be forward exempt, but this was missed when AF_Heal was added in RFL v300
    // To ensure maximum compatibility with existing Alpine levels, only exempt for RFL v304 and later.
    // Note that the likelihood of AF_Heal ever having been used in a way that makes this relevant is extremely low.
    if (event_type == rf::EventType::AF_Heal && rfl_version_minimum(304)) {
        return true;
    }

    return forward_exempt_ids.find(event_type) != forward_exempt_ids.end();
}

CodeInjection event_type_forwards_messages_patch{
    0x004B8C44, [](auto& regs) {
        if (af_rfl_version(rf::level.version)) {
            auto event_type = rf::int_to_event_type(static_cast<int>(regs.eax));

            // handle Alpine events that shouldn't forward messages
            // also do not forward for Cyclic_Timer unless legacy cyclic timers are disabled
            if (is_forward_exempt(event_type) ||
                (event_type == rf::EventType::Cyclic_Timer && !AlpineLevelProperties::instance().legacy_cyclic_timers)
                ) {
                regs.al = false;
                regs.eip = 0x004B8C5D;
            }

            // stock events are handled by original code
        }
    }
};

using EventFactory = std::function<rf::Event*(const EventCreateParams&)>;

static std::unordered_map<rf::EventType, EventFactory> event_factories {
    // Set_Variable
    {
        rf::EventType::Set_Variable, [](const EventCreateParams& params) {
         auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Set_Variable));
            auto* event = dynamic_cast<EventSetVar*>(base_event);
            if (event) {
                event->var = static_cast<SetVarOpts>(params.int1);
                event->value_int = params.int2;
                event->value_float = params.float1;
                event->value_bool = params.bool1;
                event->value_str = params.str1;
            }
            return event;
        }
    },
    // Clone_Entity
    {
        rf::EventType::Clone_Entity, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Clone_Entity));
            auto* event = dynamic_cast<EventCloneEntity*>(base_event);
            if (event) {                
                event->hostile_to_player = params.bool1;
                event->find_player = params.bool2;
                event->link_from = params.int1;
            }
            return event;
        }
    },
    // Switch_Random
    {
        rf::EventType::Switch_Random, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Switch_Random));
            auto* event = dynamic_cast<EventSwitchRandom*>(base_event);
            if (event) {                
                event->no_repeats = params.bool1;
            }
            return event;
        }
    },
    // Difficulty_Gate
    {
        rf::EventType::Difficulty_Gate, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Difficulty_Gate));
            auto* event = dynamic_cast<EventDifficultyGate*>(base_event);
            if (event) {
                event->difficulty = static_cast<rf::GameDifficultyLevel>(params.int1);
            }
            return event;
        }
    },
    // HUD_Message
    {
        rf::EventType::HUD_Message, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::HUD_Message));
            auto* event = dynamic_cast<EventHUDMessage*>(base_event);
            if (event) {
                event->message = params.str1;
                event->duration = params.float1;
            }
            return event;
        }
    },
    // Play_Video
    {
        rf::EventType::Play_Video, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Play_Video));
            auto* event = dynamic_cast<EventPlayVideo*>(base_event);
            if (event) {
                event->filename = params.str1;
            }
            return event;
        }
    },
    // Set_Level_Hardness
    {
        rf::EventType::Set_Level_Hardness, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Set_Level_Hardness));
            auto* event = dynamic_cast<EventSetLevelHardness*>(base_event);
            if (event) {
                event->hardness = params.int1;
            }
            return event;
        }
    },
    // Sequence
    {
        rf::EventType::Sequence, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Sequence));
            auto* event = dynamic_cast<EventSequence*>(base_event);
            if (event) {
                event->next_link_index = params.int1;
            }
            return event;
        }
    },
    // Remove_Link
    {
        rf::EventType::Remove_Link, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Remove_Link));
            auto* event = dynamic_cast<EventRemoveLink*>(base_event);
            if (event) {
                event->remove_all = params.bool1;
            }
            return event;
        }
    },
    // Route_Node
    {
        rf::EventType::Route_Node, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Route_Node));
            auto* event = dynamic_cast<EventRouteNode*>(base_event);
            if (event) {
                event->behaviour = static_cast<RouteNodeBehavior>(params.int1);
                event->fixed = params.bool1;
                event->clear_trigger_info = params.bool2;
            }
            return event;
        }
    },
    // Add_Link
    {
        rf::EventType::Add_Link, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Add_Link));
            auto* event = dynamic_cast<EventAddLink*>(base_event);
            if (event) {
                event->subject_uid = params.int1;
                event->inbound = params.bool1;
            }
            return event;
        }
    },
    // Valid_Gate
    {
        rf::EventType::Valid_Gate, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Valid_Gate));
            auto* event = dynamic_cast<EventValidGate*>(base_event);
            if (event) {
                event->check_uid = params.int1;
            }
            return event;
        }
    },
    // Goal_Math
    {
        rf::EventType::Goal_Math, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Goal_Math));
            auto* event = dynamic_cast<EventGoalMath*>(base_event);
            if (event) {
                event->goal = params.str1;
                event->operation = static_cast<GoalMathOperation>(params.int1);
                event->operation_value = params.int2;
            }
            return event;
        }
    },
    // Goal_Gate
    {
        rf::EventType::Goal_Gate, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Goal_Gate));
            auto* event = dynamic_cast<EventGoalGate*>(base_event);
            if (event) {
                event->goal = params.str1;
                event->test_type = static_cast<GoalGateTests>(params.int1);
                event->test_value = params.int2;
            }
            return event;
        }
    },
    // Scope_Gate
    {
        rf::EventType::Scope_Gate, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Scope_Gate));
            auto* event = dynamic_cast<EventScopeGate*>(base_event);
            if (event) {
                event->scope = static_cast<ScopeGateTests>(params.int1);
            }
            return event;
        }
    },
    // Inside_Gate
    {
        rf::EventType::Inside_Gate, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Inside_Gate));
            auto* event = dynamic_cast<EventInsideGate*>(base_event);
            if (event) {
                event->check_uid = params.int1;
            }
            return event;
        }
    },
    // Set_Difficulty
    {
        rf::EventType::Set_Difficulty, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Set_Difficulty));
            auto* event = dynamic_cast<EventSetDifficulty*>(base_event);
            if (event) {
                event->difficulty = static_cast<rf::GameDifficultyLevel>(params.int1);
            }
            return event;
        }
    },
    // Set_Fog_Far_Clip
    {
        rf::EventType::Set_Fog_Far_Clip, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Set_Fog_Far_Clip));
            auto* event = dynamic_cast<EventSetFogFarClip*>(base_event);
            if (event) {
                event->far_clip = params.float1;
            }
            return event;
        }
    },
    // AF_When_Dead
    {
        rf::EventType::AF_When_Dead, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::AF_When_Dead));
            auto* event = dynamic_cast<EventAFWhenDead*>(base_event);
            if (event) {
                event->any_dead = params.bool1;
            }
            return event;
        }
    },
    // Gametype_Gate
    {
        rf::EventType::Gametype_Gate, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Gametype_Gate));
            auto* event = dynamic_cast<EventGametypeGate*>(base_event);
            if (event) {
                event->gametype = static_cast<rf::NetGameType>(params.int1);
            }
            return event;
        }
    },
    // Set_Skybox
    {
        rf::EventType::Set_Skybox, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Set_Skybox));
            auto* event = dynamic_cast<EventSetSkybox*>(base_event);
            if (event) {
                event->new_sky_room_uid = params.int1;
                event->new_sky_room_anchor_uid = params.int2;
                event->relative_position = params.bool1;
                event->position_scale = params.float1;
            }
            return event;
        }
    },
    // Set_Life
    {
        rf::EventType::Set_Life, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Set_Life));
            auto* event = dynamic_cast<EventSetLife*>(base_event);
            if (event) {
                event->new_life = params.float1;
            }
            return event;
        }
    },
    // Set_Debris
    {
        rf::EventType::Set_Debris, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Set_Debris));
            auto* event = dynamic_cast<EventSetDebris*>(base_event);
            if (event) {
                event->debris_filename = params.str1;
                event->debris_sound_set = params.str2;
                event->explode_anim_vclip = params.int1;
                event->explode_anim_radius = params.float1;
                event->debris_velocity = params.float2;
            }
            return event;
        }
    },
    // Set_Fog_Color
    {
        rf::EventType::Set_Fog_Color, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Set_Fog_Color));
            auto* event = dynamic_cast<EventSetFogColor*>(base_event);
            if (event) {
                event->fog_color = params.str1;
            }
            return event;
        }
    },
    // Set_Entity_Flag
    {
        rf::EventType::Set_Entity_Flag, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Set_Entity_Flag));
            auto* event = dynamic_cast<EventSetEntityFlag*>(base_event);
            if (event) {
                event->flag = static_cast<SetEntityFlagOption>(params.int1);
            }
            return event;
        }
    },
    // AF_Teleport_Player
    {
        rf::EventType::AF_Teleport_Player, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::AF_Teleport_Player));
            auto* event = dynamic_cast<EventAFTeleportPlayer*>(base_event);
            if (event) {
                event->reset_velocity = params.bool1;
                event->force_exit_vehicle = params.bool2;
                event->entrance_vclip = params.str1;
                event->exit_vclip = params.str2;
            }
            return event;
        }
    },
    // Set_Item_Drop
    {
        rf::EventType::Set_Item_Drop, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Set_Item_Drop));
            auto* event = dynamic_cast<EventSetItemDrop*>(base_event);
            if (event) {
                event->item_name = params.str1;
            }
            return event;
        }
    },
    // AF_Heal
    {
        rf::EventType::AF_Heal, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::AF_Heal));
            auto* event = dynamic_cast<EventAFHeal*>(base_event);
            if (event) {
                event->amount = params.int1;
                event->target = static_cast<AFHealTargetOption>(params.int2);
                event->apply_to_armor = params.bool1;
                event->super = params.bool2;
            }
            return event;
        }
    },
    // World_HUD_Sprite
    {
        rf::EventType::World_HUD_Sprite, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::World_HUD_Sprite));
            auto* event = dynamic_cast<EventWorldHUDSprite*>(base_event);
            if (event) {
                event->enabled = params.bool1;
                event->render_mode = static_cast<WorldHUDRenderMode>(params.int1);
                event->scale = params.float1;
                event->sprite_filename = params.str1;
                event->sprite_filename_blue = params.str2;
            }
            return event;
        }
    },
    // Set_Light_Color
    {
        rf::EventType::Set_Light_Color, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Set_Light_Color));
            auto* event = dynamic_cast<EventSetLightColor*>(base_event);
            if (event) {
                event->light_color = params.str1;
                event->randomize = params.bool1;
            }
            return event;
        }
    },
    // Capture_Point_Handler
    {
        rf::EventType::Capture_Point_Handler, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Capture_Point_Handler));
            auto* event = dynamic_cast<EventCapturePointHandler*>(base_event);
            if (event) {
                event->name = params.str1;
                event->outline_offset = params.float1;
                event->capture_rate = params.float2;
                event->sphere_to_cylinder = params.bool1;
                event->stage = params.int1;
                event->position = params.int2;
            }
            return event;
        }
    },
    // Modify_Respawn_Point
    {
        rf::EventType::Modify_Respawn_Point, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Modify_Respawn_Point));
            auto* event = dynamic_cast<EventModifyRespawnPoint*>(base_event);
            if (event) {
                event->red = params.bool1;
                event->blue = params.bool2;
            }
            return event;
        }
    },
    // Set_Capture_Point_Owner
    {
        rf::EventType::Set_Capture_Point_Owner, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Set_Capture_Point_Owner));
            auto* event = dynamic_cast<EventSetCapturePointOwner*>(base_event);
            if (event) {
                event->owner = params.int1;
            }
            return event;
        }
    },
    // Owner_Gate
    {
        rf::EventType::Owner_Gate, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Owner_Gate));
            auto* event = dynamic_cast<EventOwnerGate*>(base_event);
            if (event) {
                event->handler_uid = params.int1;
                event->required_owner = params.int2;
            }
            return event;
        }
    },
    // Set_Gameplay_Rule
    {
        rf::EventType::Set_Gameplay_Rule, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Set_Gameplay_Rule));
            auto* event = dynamic_cast<EventSetGameplayRule*>(base_event);
            if (event) {
                event->rule = static_cast<GameplayRule>(params.int1);
            }
            return event;
        }
    },
    // Mesh_Animate
    {
        rf::EventType::Mesh_Animate, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Mesh_Animate));
            auto* event = dynamic_cast<EventMeshAnimate*>(base_event);
            if (event) {
                event->animate_type = params.int1;
                event->anim_filename = params.str1;
                event->blend_weight = (params.float1 > 0.0f) ? params.float1 : 1.0f;
            }
            return event;
        }
    },
    // Mesh_Set_Texture
    {
        rf::EventType::Mesh_Set_Texture, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Mesh_Set_Texture));
            auto* event = dynamic_cast<EventMeshSetTexture*>(base_event);
            if (event) {
                event->texture_slot = params.int1;
                event->texture_filename = params.str1;
            }
            return event;
        }
    },
    // Mesh_Set_Collision
    {
        rf::EventType::Mesh_Set_Collision, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Mesh_Set_Collision));
            auto* event = dynamic_cast<EventMeshSetCollision*>(base_event);
            if (event) {
                event->collision_type = params.int1;
            }
            return event;
        }
    },
    // AF_Fullscreen_Image
    {
        rf::EventType::AF_Fullscreen_Image, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::AF_Fullscreen_Image));
            auto* event = dynamic_cast<EventFullscreenImage*>(base_event);
            if (event) {
                event->filename = params.str1;
                event->duration = params.float1;
                event->transition_time = std::max(0.0f, params.float2);
                int tt = params.int1;
                event->transition_type = static_cast<FullscreenTransitionType>(tt >= 0 && tt <= 3 ? tt : 0);
                event->max_alpha_raw = params.int2;
            }
            return event;
        }
    },
    // AF_Fullscreen_Color
    {
        rf::EventType::AF_Fullscreen_Color, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::AF_Fullscreen_Color));
            auto* event = dynamic_cast<EventFullscreenColor*>(base_event);
            if (event) {
                event->color_string = params.str1;
                event->duration = params.float1;
                event->transition_time = std::max(0.0f, params.float2);
                int tt = params.int1;
                event->transition_type = static_cast<FullscreenTransitionType>(tt >= 0 && tt <= 3 ? tt : 0);
                event->max_alpha_raw = params.int2;
            }
            return event;
        }
    },
    // Modify_Gas_Region
    {
        rf::EventType::Modify_Gas_Region, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Modify_Gas_Region));
            auto* event = dynamic_cast<EventModifyGasRegion*>(base_event);
            if (event) {
                event->color_string = params.str1;
                event->density = params.float1;
                event->transition_time = std::max(0.0f, params.float2);
            }
            return event;
        }
    },
    // Resize_Gas_Region
    {
        rf::EventType::Resize_Gas_Region, [](const EventCreateParams& params) {
            auto* base_event = rf::event_create(params.pos, rf::event_type_to_int(rf::EventType::Resize_Gas_Region));
            auto* event = dynamic_cast<EventResizeGasRegion*>(base_event);
            if (event) {
                event->shape = params.int1 + 1; // dropdown index counts from 0, must be incremented
                event->sphere_radius = params.float1;
                event->box_dimensions = params.str1;
                event->transition_time = std::max(0.0f, params.float2);
            }
            return event;
        }
    },
};

rf::Event* construct_alpine_event(int event_type, const EventCreateParams& params)
{
    auto it = event_factories.find(rf::int_to_event_type(event_type));
    if (it != event_factories.end()) {
        return it->second(params);
    }

    // Default for events without specific factories
    return rf::event_create(params.pos, event_type);
}

// assignment of factories for AF event types
CodeInjection level_read_events_factories_patch {
    0x00462910, [](auto& regs) {
        if (af_rfl_version(rf::level.version)) {
            int event_type = static_cast<int>(regs.ebp);

            if (event_type >= 100) { // only handle AF events, stock events handled by original code
                rf::Vector3* pos = regs.edx;

                EventCreateParams params{
                    pos,
                    *reinterpret_cast<rf::String*>(regs.esp + 0x5C),    // class_name
                    *reinterpret_cast<rf::String*>(regs.esp + 0x54),    // script_name
                    *reinterpret_cast<rf::String*>(regs.esp + 0x44),    // str1
                    *reinterpret_cast<rf::String*>(regs.esp + 0x4C),    // str2
                    *reinterpret_cast<int*>(regs.esp - 0x24),           // int1
                    *reinterpret_cast<int*>(regs.esp - 0x18),           // int2
                    *reinterpret_cast<bool*>(regs.esp + 0x18),          // bool1
                    *reinterpret_cast<bool*>(regs.esp + 0x20),          // bool2
                    *reinterpret_cast<float*>(regs.esp + 0x1C),         // float1
                    *reinterpret_cast<float*>(regs.esp + 0x24)          // float2
                };

                xlog::debug("Constructing event type {}: class_name: {}, script_name: {}, str1: {}, str2: {}, int1: {}, "
                           "int2: {}, bool1: {}, bool2: {}, float1: {}, float2: {}",
                           event_type, params.class_name, params.script_name, params.str1, params.str2, params.int1,
                           params.int2, params.bool1, params.bool2, params.float1, params.float2);


                auto* this_event = construct_alpine_event(event_type, params);
                regs.eax = this_event; // set eax to created event so level_read_events can continue to use it

                regs.eip = 0x00462915; // made the event, set stack pointer after jump table
            }
        }
    }
};

// set p_data orient for Anchor_Marker_Orient when event is created (required for use in moving groups)
CodeInjection level_read_events_movers_patch {
    0x0046294F, [](auto& regs) {
        if (af_rfl_version(rf::level.version)) {
            rf::Event* event = regs.esi;

            if (event->event_type == rf::event_type_to_int(rf::EventType::Anchor_Marker_Orient)) {
                rf::Matrix3 event_orient = event->orient;

                event->start_orient = event_orient;
                event->p_data.orient = event_orient;
                event->p_data.next_orient = event_orient;
            }
        }
    }
};

CodeInjection event_activate_route_node{
    0x004B8B97,
    [](auto& regs) {
        rf::Event* event = regs.esi;

        if (is_achievement_system_initialized()) {
            achievement_check_event(event);
        }

        if (af_rfl_version(rf::level.version)) {
            // verify it's a Route_Node
            if (event->event_type == rf::event_type_to_int(rf::EventType::Route_Node)) {
                auto* delay_event = reinterpret_cast<EventRouteNode*>(event);
                bool on = *reinterpret_cast<bool*>(regs.esp + 0x18);
                bool proceed = true;

                // mode = drop, or mode = fixed + existing delay
                if (delay_event->behaviour == RouteNodeBehavior::drop ||
                    (delay_event->fixed && event->delay_timestamp.valid())) {
                    xlog::debug("Ignoring message request in {} ({})", event->name, event->uid); // would be nice to use dbg_events
                    proceed = false; // ignoring message, stop
                }

                // calculate desired on/off state
                bool real_on = false;
                switch (delay_event->behaviour) {
                    case RouteNodeBehavior::force_on:
                        real_on = true;
                        break;
                    case RouteNodeBehavior::force_off:
                        real_on = false;
                        break;
                    case RouteNodeBehavior::invert:
                        real_on = !on;
                        break;
                    case RouteNodeBehavior::pass:
                    default:
                        real_on = on;
                        break;
                }

                // handle clearing trigger info, doing it here ensures it's maintained when process handles it after delay
                if (delay_event->clear_trigger_info) {
                    delay_event->trigger_handle = -1;
                    delay_event->triggered_by_handle = -1;
                }

                if (proceed && delay_event->delay_seconds > 0.0f) {
                    delay_event->delay_timestamp.set(static_cast<int>(delay_event->delay_seconds * 1000));
                    delay_event->delayed_msg = real_on;
                    proceed = false; // set delay, stop
                }

                // activate here if no delay, activation happens in process if there is a delay
                // Note this doesn't call turn_on or turn_off - unneeded since this event does nothing,
                // but turn_on or turn_off is called if its activated via process (original code)
                if (proceed) {
                    delay_event->delay_timestamp.invalidate();
                    delay_event->activate_links(delay_event->trigger_handle, delay_event->triggered_by_handle, real_on);
                }

                regs.eip = 0x004B8C35; // skip to end of function
            }
        }
    }
};

rf::Vector3 transform_vector(const rf::Matrix3& mat, const rf::Vector3& vec)
{
    return {
        mat.rvec.x * vec.x + mat.uvec.x * vec.y + mat.fvec.x * vec.z,
        mat.rvec.y * vec.x + mat.uvec.y * vec.y + mat.fvec.y * vec.z,
        mat.rvec.z * vec.x + mat.uvec.z * vec.y + mat.fvec.z * vec.z
    };
}

rf::Matrix3 matrix_transpose(const rf::Matrix3& mat)
{
    return {
        { mat.rvec.x, mat.uvec.x, mat.fvec.x },
        { mat.rvec.y, mat.uvec.y, mat.fvec.y },
        { mat.rvec.z, mat.uvec.z, mat.fvec.z }
    };
}

rf::Matrix3 multiply_matrices(const rf::Matrix3& a, const rf::Matrix3& b)
{
    return {
        transform_vector(a, b.rvec),
        transform_vector(a, b.uvec),
        transform_vector(a, b.fvec)
    };
}

rf::Vector3 rotate_velocity(const rf::Vector3& old_velocity, const rf::Matrix3& old_orient, const rf::Matrix3& new_orient)
{
    // Compute the relative rotation matrix
    rf::Matrix3 relative_rotation = multiply_matrices(new_orient, matrix_transpose(old_orient));

    // Apply the relative rotation to the velocity
    return transform_vector(relative_rotation, old_velocity);
}

void apply_alpine_events()
{
    AsmWriter(0x004B68A3).jmp(0x004B68A9);          // make event_create process events with any ID (params specified)
    event_lookup_type_hook.install();               // define AF event IDs
    event_allocate_hook.install();                  // load AF events at level start
    event_deallocate_hook.install();                // unload AF events at level end
    event_type_forwards_messages_patch.install();   // handle AF events that shouldn't forward messages by default
    level_read_events_factories_patch.install();    // assign factories for AF events
    event_activate_route_node.install();            // handle activations for Route_Node event
    level_read_events_movers_patch.install();       // handle p_data orient assignment for Anchor_Marker_Orient event
}
