#pragma once

#include <string>
#include <vector>
#include <sstream>
#include <optional>
#include <cstddef>
#include <functional>
#include <algorithm>
#include <common/utils/list-utils.h>
#include "../hud/hud_world.h"
#include "../rf/event.h"
#include "../rf/object.h"
#include "../rf/level.h"
#include "../rf/misc.h"
#include "../rf/multi.h"
#include "../rf/hud.h"
#include "../rf/entity.h"
#include "../rf/mover.h"
#include "../rf/clutter.h"
#include "../rf/trigger.h"
#include "../rf/particle_emitter.h"
#include "../main/main.h"
#include "object.h"
#include "../misc/player.h"
#include "../multi/gametype.h"
#include "../rf/player/player.h"
#include "../rf/os/timestamp.h"
#include "../os/os.h"
#include "../rf/os/array.h"
#include "../rf/gr/gr_light.h"
#include "../rf/glare.h"
#include "../graphics/gr.h"
#include "../misc/level.h"
#include "../misc/alpine_settings.h"

extern void set_sky_room_uid_override(
    int room_uid,
    int anchor_uid,
    bool relative_position,
    float position_scale
);

rf::Vector3 rotate_velocity(
    const rf::Vector3& old_velocity,
    const rf::Matrix3& old_orient,
    const rf::Matrix3& new_orient
);

struct EventCreateParams
{
    const rf::Vector3* pos;
    std::string class_name;
    std::string script_name;
    std::string str1;
    std::string str2;
    int int1;
    int int2;
    bool bool1;
    bool bool2;
    float float1;
    float float2;
};

enum class GoalGateTests : int
{
    equal,
    nequal,
    gt,
    lt,
    geq,
    leq,
    odd,
    even,
    divisible,
    ltinit,
    gtinit,
    leinit,
    geinit,
    eqinit
};

enum class GoalMathOperation : int
{
    add,
    sub,
    mul,
    div,
    rdiv,
    set,
    mod,
    pow,
    neg,
    abs,
    max,
    min,
    reset
};

enum class RouteNodeBehavior : int
{
    pass,
    drop,
    invert,
    force_on,
    force_off
};

enum class ScopeGateTests : int
{
    multi,
    single,
    server,
    dedicated,
    client,
    triggered_by,
    blue_team_spawned,
    red_team_spawned,
    has_flag,
    blue_team,
    red_team,
    renderer_d3d11,
    renderer_legacy
};

enum class GoalInsideCheckSubject : int
{
    player,
    triggered_by,
    all_linked,
    any_linked
};

enum class SetEntityFlagOption : int
{
    boarded,
    cower,
    question_unarmed,
    fade_corpse_immediately,
    dont_hum,
    no_shadow,
    perfect_aim,
    permanent_corpse,
    always_face_player,
    only_attack_player,
    deaf,
    ignore_terrain
};

enum class AFHealTargetOption : int
{
    linked,
    player,
    all_players,
    player_team,
    enemy_team,
    red_team,
    blue_team,
    players_in_linked_triggers
};

enum class GameplayRule : int
{
    player_has_headlamp,
};

enum class FullscreenTransitionType : int
{
    disappear = 0,           // instant on, instant off
    fade_in_disappear = 1,   // fade in, instant off
    fade_in_fade_out = 2,    // fade in, fade out
    appear_fade_out = 3      // instant on, fade out
};

// start alpine event structs
// id 100
struct EventSetVar : rf::Event
{
    SetVarOpts var;
    int value_int = 0;
    float value_float = 0.0f;
    bool value_bool = false;
    std::string value_str = "";

    void turn_on() override
    {
        this->activate(this->trigger_handle, this->triggered_by_handle, true);
    }

    void do_activate(int trigger_handle, int triggered_by_handle, bool on) override
    {
        if (this->links.empty()) {
            return;
        }

        for (int link_handle : this->links) {
            Object* obj = rf::obj_from_handle(link_handle);
            if (obj && obj->type == rf::OT_EVENT) {
                Event* linked_event = static_cast<Event*>(obj);

                try {
                    switch (var) {
                    case SetVarOpts::int1:
                    case SetVarOpts::int2:
                        linked_event->apply_var(var, std::to_string(value_int));
                        xlog::info("Applied int value {} to event UID {}", value_int, linked_event->uid);
                        break;
                    case SetVarOpts::float1:
                    case SetVarOpts::float2:
                        linked_event->apply_var(var, std::to_string(value_float));
                        xlog::info("Applied float value {} to event UID {}", value_float, linked_event->uid);
                        break;
                    case SetVarOpts::bool1:
                    case SetVarOpts::bool2:
                        linked_event->apply_var(var, value_bool ? "true" : "false");
                        xlog::info("Applied bool value {} to event UID {}", value_bool, linked_event->uid);
                        break;
                    case SetVarOpts::str1:
                    case SetVarOpts::str2:
                        linked_event->apply_var(var, value_str);
                        xlog::info("Applied string value '{}' to event UID {}", value_str, linked_event->uid);
                        break;
                    default:
                        xlog::warn("Unsupported SetVarOpts value for event UID {}", this->uid);
                        break;
                    }
                }
                catch (const std::exception& ex) {
                    xlog::error("Failed to apply variable for Set_Variable event UID {} to linked event UID {} - {}",
                                this->uid, linked_event->uid, ex.what());
                }
            }
        }
    }
};

// id 101
struct EventCloneEntity : rf::Event
{
    bool hostile_to_player = 0;
    bool find_player = 0;
    int link_from = -1;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::bool1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventCloneEntity*>(event);
            this_event->hostile_to_player = (value == "true");
        };

        handlers[SetVarOpts::bool2] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventCloneEntity*>(event);
            this_event->find_player = (value == "true");
        };

        handlers[SetVarOpts::int1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventCloneEntity*>(event);
            this_event->link_from = std::stoi(value);
        };
    }

    void turn_on() override
    {
        for (int i = 0; i < this->links.size(); ++i) {

            int link = this->links[i];
            Object* obj = rf::obj_from_handle(link);
            if (obj && obj->type == rf::OT_ENTITY) {
                rf::Entity* entity = static_cast<rf::Entity*>(obj);
                rf::Entity* new_entity =
                    rf::entity_create(entity->info_index, entity->name, -1, pos, this->orient, 0, -1);
                new_entity->entity_flags = entity->entity_flags;
                new_entity->entity_flags2 = entity->entity_flags2;
                new_entity->info->flags = entity->info->flags;
                new_entity->info->flags2 = entity->info->flags2;
                new_entity->fov_cos = entity->fov_cos;

                new_entity->drop_item_class = entity->drop_item_class;
                
                new_entity->obj_flags = entity->obj_flags;
                new_entity->ai.custom_attack_range = entity->ai.custom_attack_range;
                new_entity->ai.use_custom_attack_range = entity->ai.use_custom_attack_range;
                new_entity->ai.attack_style = entity->ai.attack_style;
                new_entity->ai.cooperation = entity->ai.cooperation;
                new_entity->ai.cover_style = entity->ai.cover_style;
                new_entity->ai.ai_flags = entity->ai.ai_flags;

                for (int i = 0; i < 64; ++i) {
                    new_entity->ai.has_weapon[i] = entity->ai.has_weapon[i];
                    new_entity->ai.clip_ammo[i] = entity->ai.clip_ammo[i];
                }

                for (int i = 0; i < 32; ++i) {
                    new_entity->ai.ammo[i] = entity->ai.ammo[i];
                }

                new_entity->ai.current_secondary_weapon = entity->ai.current_secondary_weapon;
                new_entity->ai.current_primary_weapon = entity->ai.current_primary_weapon;

                new_entity->ai.mode_default = entity->ai.mode_default;
                new_entity->ai.submode_default = entity->ai.submode_default;
                new_entity->ai.default_waypoint_path = entity->ai.default_waypoint_path;
                new_entity->ai.default_waypoint_path_flags = entity->ai.default_waypoint_path_flags;

                rf::obj_set_friendliness(new_entity, 1);
                rf::ai_set_mode(&new_entity->ai, rf::AiMode::AI_MODE_WAITING, -1, -1);

                int walk_only = 0;

                if (!rf::entity_is_flying(new_entity) || !rf::entity_is_swimming(new_entity)) {
                    walk_only = 1;
                }

                rf::ai_path_locate_pos(
                    &new_entity->pos,
                    new_entity->ai.movement_radius,
                    new_entity->ai.movement_height,
                    walk_only,
                    &new_entity->ai.current_path.adjacent_node1,
                    &new_entity->ai.current_path.adjacent_node2,
                    0);                   

                if (link_from > 0) {
                    Event* linked_event = rf::event_lookup_from_uid(link_from);
                    if (linked_event) {
                        rf::event_add_link(linked_event->handle, new_entity->handle);
                    }
                }                    

                if (find_player) {
                    rf::ai_set_mode(&new_entity->ai, rf::AiMode::AI_MODE_FIND_PLAYER, -1, -1);
                    rf::ai_set_submode(&new_entity->ai, rf::AiSubmode::AI_SUBMODE_12);
                    rf::entity_make_run(new_entity);
                }

                if (hostile_to_player) {
                    new_entity->ai.hate_list.add(rf::local_player_entity->handle);
                }
            }
        }
    }
};

// id 102
struct EventSetCollisionPlayer : rf::Event
{
    void turn_on() override
    {
        rf::local_player->collides_with_world = true;
    }
    void turn_off() override
    {
        rf::local_player->collides_with_world = false;
    }
};

// id 103
struct EventSwitchRandom : rf::Event
{
    bool no_repeats = false;
    std::vector<int> used_handles;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::bool1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventSwitchRandom*>(event);
            this_event->no_repeats = (value == "true");
        };
    }

    void turn_on() override
    {
        if (this->links.empty()) {
            return;
        }

        if (no_repeats) {
            // filter out already used handles
            std::vector<int> available_handles;
            for (const int link_handle : this->links) {
                if (std::find(used_handles.begin(), used_handles.end(), link_handle) == used_handles.end()) {
                    available_handles.push_back(link_handle);
                }
            }

            // no available handles remain, reset used_handles and start fresh
            if (available_handles.empty()) {
                used_handles.clear();

                for (int link_handle : this->links) {
                    available_handles.push_back(link_handle);
                } 
            }

            // select a random link
            std::uniform_int_distribution<int> dist(0, available_handles.size() - 1);
            int random_index = dist(g_rng);
            int link_handle = available_handles[random_index];

            // add to used_handles
            used_handles.push_back(link_handle);

            rf::event_signal_on(link_handle, this->trigger_handle, this->triggered_by_handle);
        }
        else {
            // Standard random selection
            std::uniform_int_distribution<int> dist(0, this->links.size() - 1);
            int random_index = dist(g_rng);
            int link_handle = this->links[random_index];

            rf::event_signal_on(link_handle, this->trigger_handle, this->triggered_by_handle);
        }
    }
};

// id 104
struct EventDifficultyGate : rf::Event
{
    rf::GameDifficultyLevel difficulty = rf::GameDifficultyLevel::DIFFICULTY_EASY;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::int1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventDifficultyGate*>(event);
            this_event->difficulty = static_cast<rf::GameDifficultyLevel>(std::stoi(value));
        };
    }

    void turn_on() override
    {
        if (rf::game_get_skill_level() == difficulty) {
            activate_links(this->trigger_handle, this->triggered_by_handle, true);
        }
    }

    void turn_off() override
    {
        if (rf::game_get_skill_level() == difficulty) {
            activate_links(this->trigger_handle, this->triggered_by_handle, false);
        }
    }
};

// id 105
struct EventHUDMessage : rf::Event
{
    std::string message = "";
    float duration = 0.0f;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::str1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventHUDMessage*>(event);
            this_event->message = value;
        };

        handlers[SetVarOpts::float1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventHUDMessage*>(event);
            this_event->duration = std::stof(value);
        };
    }

    void turn_on() override
    {
        rf::hud_msg(message.c_str(), 0, static_cast<int>(duration * 1000), 0);
    }

    void turn_off() override
    {
        rf::hud_msg_clear();
    }
};

// id 106
struct EventPlayVideo : rf::Event
{
    std::string filename;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::str1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventPlayVideo*>(event);
            this_event->filename = value;
        };
    }

    void turn_on() override
    {
        rf::bink_play(filename.c_str());
    }
};

// id 107
struct EventSetLevelHardness : rf::Event
{
    int hardness;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::int1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventSetLevelHardness*>(event);
            this_event->hardness = std::stoi(value);
        };
    }

    void turn_on() override
    {
        rf::level.default_rock_hardness = std::clamp(hardness, 0, 100);
    }
};

// id 108
struct EventSequence : rf::Event
{
    int next_link_index = 0;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::int1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventSequence*>(event);
            this_event->next_link_index = std::stoi(value);
        };
    }

    void turn_on() override
    {

        if (this->links.empty()) {
            return;
        }

        // bounds check
        if (next_link_index < 0 || next_link_index >= static_cast<int>(this->links.size())) {
            next_link_index = 0;
        }            

        // find and activate the link
        int link_handle = this->links[next_link_index];

        rf::event_signal_on(link_handle, this->trigger_handle, this->triggered_by_handle);

        ++next_link_index;
    }
};


// id 109
struct EventClearQueued : rf::Event
{
    void turn_on() override
    {
        for (int link_handle : this->links) {
            Object* obj = rf::obj_from_handle(link_handle);

            if (obj && obj->type == rf::OT_EVENT) {
                Event* linked_event = static_cast<Event*>(obj);
                linked_event->delay_timestamp.invalidate();
                linked_event->delayed_msg = 0;
            }                
        }
    }
};

// id 110
struct EventRemoveLink : rf::Event
{
    bool remove_all = 0;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::bool1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventRemoveLink*>(event);
            this_event->remove_all = (value == "true");
        };
    }

    void turn_on() override
    {
        for (int link_handle : this->links) {
            Object* obj = rf::obj_from_handle(link_handle);

            if (obj && obj->type == rf::OT_EVENT) {
                Event* linked_event = static_cast<Event*>(obj);

                if (remove_all) {
                    linked_event->links.clear();
                }
                else {
                    // Remove only the links between events in this->links
                    linked_event->links.erase_if([this](int inner_link_handle) {
                        return std::find(this->links.begin(), this->links.end(), inner_link_handle) !=
                                this->links.end();
                    });
                }
            }
        }
    }
};

// id 111
struct EventRouteNode : rf::Event
{
    RouteNodeBehavior behaviour = RouteNodeBehavior::pass;
    bool fixed = 0;
    bool clear_trigger_info = 0;

    // logic handled in event_alpine.cpp

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];

        handlers[SetVarOpts::int1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventRouteNode*>(event);
            this_event->behaviour = static_cast<RouteNodeBehavior>(std::stoi(value));
        };

        handlers[SetVarOpts::bool1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventRouteNode*>(event);
            this_event->fixed = (value == "true");
        };

        handlers[SetVarOpts::bool2] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventRouteNode*>(event);
            this_event->clear_trigger_info = (value == "true");
        };
    }
};

// id 112
struct EventAddLink : rf::Event
{
    int subject_uid = -1;
    bool inbound = false;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];

        handlers[SetVarOpts::int1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventAddLink*>(event);
            this_event->subject_uid = std::stoi(value);
        };

        handlers[SetVarOpts::bool1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventAddLink*>(event);
            this_event->inbound = (value == "true");
        };
    }

    void turn_on() override
    {
        if (this->links.empty()) {
            return;
        }

        Object* subject_obj = rf::obj_lookup_from_uid(subject_uid);

        if (!subject_obj) {
            return;
        }

        int subject_handle = subject_obj->handle;

        for (int link_handle : this->links) {
            if (inbound) {
                rf::event_add_link(link_handle, subject_handle);
            }
            else {
                rf::event_add_link(subject_handle, link_handle);
            }
        }
    }
};

// id 113
struct EventValidGate : rf::Event
{
    int check_uid = -1;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::int1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventValidGate*>(event);
            this_event->check_uid = std::stoi(value);
        };
    }

    void turn_on() override
    {
        Object* obj = rf::obj_lookup_from_uid(check_uid);

        if (check_uid == -1 ||
            !obj ||
            obj->type == rf::OT_CORPSE ||
            (obj->type == rf::OT_ENTITY && rf::entity_from_handle(obj->handle)->life <= 0)) { // if entity, alive
            return;
        }

        activate_links(this->trigger_handle, this->triggered_by_handle, true);
    }

    void turn_off() override
    {
        Object* obj = rf::obj_lookup_from_uid(check_uid);

        if (check_uid == -1 || !obj || obj->type == rf::OT_CORPSE ||
            (obj->type == rf::OT_ENTITY && rf::entity_from_handle(obj->handle)->life <= 0)) { // if entity, alive
            return;
        }

        activate_links(this->trigger_handle, this->triggered_by_handle, false);
    }
};

// id 114
struct EventGoalMath : rf::Event
{
    std::string goal;
    GoalMathOperation operation = GoalMathOperation::add;
    int operation_value;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::str1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventGoalMath*>(event);
            this_event->goal = value;
        };

        handlers[SetVarOpts::int1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventGoalMath*>(event);
            this_event->operation = static_cast<GoalMathOperation>(std::stoi(value));
        };

        handlers[SetVarOpts::int2] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventGoalMath*>(event);
            this_event->operation_value = std::stoi(value);
        };
    }

    void turn_on() override
    {
        if (goal.empty()) {
            return;
        }

        rf::String effective_goal = goal.c_str();

        // Find level goal
        rf::GenericEvent* named_event = rf::event_find_named_event(&effective_goal);

        int* goal_count_ptr = nullptr;
        int* goal_initial_ptr = nullptr;
        if (named_event) {
            goal_count_ptr = reinterpret_cast<int*>(&named_event->event_specific_data[8]);
            goal_initial_ptr = reinterpret_cast<int*>(&named_event->event_specific_data[0]);
        }

        // Find persistent goal
        rf::PersistentGoalEvent* persist_event = rf::event_lookup_persistent_goal_event(effective_goal);

        if (goal_count_ptr) {
            //xlog::warn("Level goal count before operation: {}, initial: {}", *goal_count_ptr, *goal_initial_ptr);
        }

        if (persist_event) {
            //xlog::warn("Persistent goal '{}', count: {}, initial count: {}", persist_event->name, persist_event->count, persist_event->initial_count);
        }

        auto apply_operation = [&](int& goal_count, int initial_value) {
            switch (operation) {
            case GoalMathOperation::add:
                goal_count += operation_value;
                break;
            case GoalMathOperation::sub:
                goal_count -= operation_value;
                break;
            case GoalMathOperation::mul:
                goal_count *= operation_value;
                break;
            case GoalMathOperation::div:
                if (operation_value != 0) {
                    goal_count /= operation_value;
                }
                else {
                    xlog::error("Division by zero attempted in EventGoalMath UID {}", this->uid);
                }
                break;
            case GoalMathOperation::rdiv:
                if (goal_count != 0) {
                    goal_count = operation_value / goal_count;
                }
                else {
                    xlog::error("Division by zero attempted in reverse divide operation for EventGoalMath UID {}",
                                this->uid);
                }
                break;
            case GoalMathOperation::set:
                goal_count = operation_value;
                break;
            case GoalMathOperation::mod:
                if (operation_value != 0) {
                    goal_count %= operation_value;
                }
                else {
                    xlog::error("Modulo by zero attempted in EventGoalMath UID {}", this->uid);
                }
                break;
            case GoalMathOperation::pow:
                if (operation_value >= 0) {
                    goal_count = static_cast<int>(std::pow(goal_count, operation_value));
                }
                else {
                    xlog::error("Negative exponent {} not allowed in EventGoalMath UID {}", operation_value,
                                this->uid);
                }
                break;
            case GoalMathOperation::neg:
                goal_count = -goal_count;
                break;
            case GoalMathOperation::abs:
                goal_count = std::abs(goal_count);
                break;
            case GoalMathOperation::max:
                goal_count = std::max(goal_count, operation_value);
                break;
            case GoalMathOperation::min:
                goal_count = std::min(goal_count, operation_value);
                break;
            case GoalMathOperation::reset:
                goal_count = initial_value;
                break;
            default:
                xlog::error("Unknown operation in EventGoalMath UID {}", this->uid);
                break;
            }
        };

        // Apply to persistent goal
        if (persist_event) {
            //xlog::warn("Applying operation '{}' with value '{}' to persistent goal '{}'", static_cast<int>(operation), operation_value, persist_event->name);
            apply_operation(persist_event->count, persist_event->initial_count);
        }

        // Apply to level goal
        if (goal_count_ptr) {
            //xlog::warn("Applying operation '{}' with value '{}' to level goal '{}'", static_cast<int>(operation), operation_value, effective_goal);
            apply_operation(*goal_count_ptr, *goal_initial_ptr);
        }

        // Log final values
        if (persist_event) {
            //xlog::warn("Persistent goal '{}', new count: {}, initial count: {}", persist_event->name, persist_event->count, persist_event->initial_count);
        }

        if (goal_count_ptr) {
            //xlog::warn("Level goal count after operation: {}", *goal_count_ptr);
        }
    }
};

// id 115
struct EventGoalGate : rf::Event
{
    std::string goal;
    GoalGateTests test_type = GoalGateTests::equal;
    int test_value;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::str1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventGoalGate*>(event);
            this_event->goal = value;
        };

        handlers[SetVarOpts::int1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventGoalGate*>(event);
            this_event->test_type = static_cast<GoalGateTests>(std::stoi(value));
        };

        handlers[SetVarOpts::int2] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventGoalGate*>(event);
            this_event->test_value = std::stoi(value);
        };
    }

    void turn_on() override
    {
        if (goal.empty()) {
            return;
        }

        rf::String effective_goal = goal.c_str();

        // find persistent goal
        rf::PersistentGoalEvent* persist_event = rf::event_lookup_persistent_goal_event(effective_goal);

        // find level goal
        rf::GenericEvent* named_event = rf::event_find_named_event(&effective_goal);

        int* goal_count_ptr = nullptr;
        int* goal_initial_ptr = nullptr;

        if (!persist_event && !named_event) {
            xlog::error("Goal_Gate ({}) did not find a level or persistent goal named '{}'",
                this->uid, effective_goal);
            return;
        }

        if (named_event) {
            goal_count_ptr = reinterpret_cast<int*>(&named_event->event_specific_data[8]);
            goal_initial_ptr = reinterpret_cast<int*>(&named_event->event_specific_data[0]);
        }

        int current_value = 0;
        int initial_value = 0;
        if (goal_count_ptr) {
            current_value = *goal_count_ptr;
            initial_value = goal_initial_ptr ? *goal_initial_ptr : 0;
        }
        else if (persist_event) {
            current_value = persist_event->count;
            initial_value = persist_event->initial_count;
        }

        //xlog::info("Current goal value for '{}': {}, Initial value: {}", effective_goal, current_value, initial_value);

        bool pass = false;

        // Determine test type
        switch (test_type) {
        case GoalGateTests::equal:
            pass = (current_value == test_value);
            break;
        case GoalGateTests::nequal:
            pass = (current_value != test_value);
            break;
        case GoalGateTests::gt:
            pass = (current_value > test_value);
            break;
        case GoalGateTests::lt:
            pass = (current_value < test_value);
            break;
        case GoalGateTests::geq:
            pass = (current_value >= test_value);
            break;
        case GoalGateTests::leq:
            pass = (current_value <= test_value);
            break;
        case GoalGateTests::odd:
            pass = (current_value % 2 != 0);
            break;
        case GoalGateTests::even:
            pass = (current_value % 2 == 0);
            break;
        case GoalGateTests::divisible:
            if (test_value != 0) {
                pass = (current_value % test_value == 0);
            }
            else {
                xlog::error("Division by zero in divisible test for EventGoalGate UID {}", this->uid);
            }
            break;
        case GoalGateTests::ltinit:
            pass = (current_value < initial_value);
            break;
        case GoalGateTests::gtinit:
            pass = (current_value > initial_value);
            break;
        case GoalGateTests::leinit:
            pass = (current_value <= initial_value);
            break;
        case GoalGateTests::geinit:
            pass = (current_value >= initial_value);
            break;
        case GoalGateTests::eqinit:
            pass = (current_value == initial_value);
            break;
        default:
            xlog::error("Unknown test type in EventGoalGate UID {}", this->uid);
            break;
        }

        // Log test result
        if (pass) {
            //xlog::warn("Test '{}' passed for EventGoalGate UID {}", static_cast<int>(test_type), this->uid);
            activate_links(this->trigger_handle, this->triggered_by_handle, true);
        }
        else {
            //xlog::warn("Test '{}' failed for EventGoalGate UID {}", static_cast<int>(test_type), this->uid);
        }
    }
};


// id 116
struct EventScopeGate : rf::Event
{
    ScopeGateTests scope = ScopeGateTests::multi;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];

        handlers[SetVarOpts::int1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventScopeGate*>(event);
            this_event->scope = static_cast<ScopeGateTests>(std::stoi(value));
        };
    }

    void turn_on() override
    {
        bool pass = false;

        switch (scope) {
        case ScopeGateTests::multi:
            pass = rf::is_multi;
            break;
        case ScopeGateTests::single:
            pass = !rf::is_multi;
            break;
        case ScopeGateTests::server:
            pass = (rf::is_server || rf::is_dedicated_server);
            break;
        case ScopeGateTests::dedicated:
            pass = rf::is_dedicated_server;
            break;
        case ScopeGateTests::client:
            pass = !(rf::is_server || rf::is_dedicated_server);
            break;
        case ScopeGateTests::triggered_by:
            pass = (rf::local_player && (rf::local_player->entity_handle == this->triggered_by_handle));
            break;
        case ScopeGateTests::blue_team_spawned:
            pass = (rf::local_player_entity && (rf::local_player_entity->team == 1));
            break;
        case ScopeGateTests::red_team_spawned:
            pass = (rf::local_player_entity && (rf::local_player_entity->team == 0));
            break;
        case ScopeGateTests::has_flag:
            pass = (rf::local_player && (rf::multi_ctf_get_blue_flag_player() == rf::local_player || rf::multi_ctf_get_red_flag_player() == rf::local_player));
            break;
        case ScopeGateTests::blue_team:
            pass = (rf::local_player && (rf::local_player->team == 1));
            break;
        case ScopeGateTests::red_team:
            pass = (rf::local_player && (rf::local_player->team == 0));
            break;
        case ScopeGateTests::renderer_d3d11:
            pass = is_d3d11();
            break;
        case ScopeGateTests::renderer_legacy:
            pass = !is_d3d11();
            break;
        default:
            break;
        }

        if (pass) {
            activate_links(this->trigger_handle, this->triggered_by_handle, true);
        }
    }
};

// id 117
struct EventInsideGate : rf::Event
{
    int check_uid = -1;
    GoalInsideCheckSubject subject_type = GoalInsideCheckSubject::player;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::int1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventInsideGate*>(event);
            this_event->check_uid = std::stoi(value);
        };

        handlers[SetVarOpts::int2] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventInsideGate*>(event);
            this_event->subject_type = static_cast<GoalInsideCheckSubject>(std::stoi(value));
        };
    }

    void turn_on() override
    {
        Object* check_obj = rf::obj_lookup_from_uid(check_uid);
        rf::GRoom* check_room = rf::level_room_from_uid(check_uid);

        if (check_uid == -1 || (!check_obj && !check_room)) {
            xlog::warn("UID {} is not a valid trigger/room for Inside_Gate UID {}", check_uid, this->uid);
            return;
        }

        std::vector<int> objects_to_check;

        // Populate object handles to check
        switch (subject_type) {
        case GoalInsideCheckSubject::player:
            if (rf::local_player_entity) {
                objects_to_check.push_back(rf::local_player_entity->handle);
            }
            break;
        case GoalInsideCheckSubject::triggered_by:
            if (triggered_by_handle != -1) {
                objects_to_check.push_back(triggered_by_handle);
            }
            break;
        case GoalInsideCheckSubject::all_linked:
        case GoalInsideCheckSubject::any_linked:
            objects_to_check.assign(this->links.begin(), this->links.end());
            break;
        }

        // early return if no objects to check
        if (objects_to_check.empty()) {
            return;
        }

        bool pass = (subject_type == GoalInsideCheckSubject::all_linked);

        // evaluate objects_to_check
        for (const int obj_handle : objects_to_check) {
            Object* obj = rf::obj_from_handle(obj_handle);
            bool is_inside = false;

            if (obj) {
                if (check_obj && check_obj->type == rf::OT_TRIGGER) {
                    rf::Trigger* trigger = static_cast<rf::Trigger*>(check_obj);
                    is_inside = (trigger->type == 0)
                                    ? rf::trigger_inside_bounding_sphere(trigger, obj)
                                    : (trigger->type == 1 && rf::trigger_inside_bounding_box(trigger, obj));
                }
                else if (check_room) {
                    is_inside = (obj->room == check_room);
                }
            }

            if (subject_type == GoalInsideCheckSubject::all_linked) {
                if (!is_inside) {
                    pass = false; // One failure invalidates "all linked"
                    break;
                }
            }
            else if (subject_type == GoalInsideCheckSubject::any_linked) {
                if (is_inside) {
                    pass = true; // One success validates "any linked"
                    break;
                }
            }
            else { // For player or triggered_by
                pass = is_inside;
                break;
            }
        }

        if (pass) {
            xlog::debug("Test passed for Inside_Gate UID {}", this->uid);
            activate_links(this->trigger_handle, this->triggered_by_handle, true);
        }
    }
};

// id 118
struct EventAnchorMarker : rf::Event {}; // logic handled in object.cpp

// id 119
struct EventForceUnhide : rf::Event
{
    void turn_on() override
    {
        for (const int link : this->links)
        {
            Object* obj = rf::obj_from_handle(link);
            if (obj) {
                rf::obj_unhide(obj);
            }
        }
    }

    void turn_off() override
    {
        for (const int link : this->links)
        {
            Object* obj = rf::obj_from_handle(link);
            if (obj) {
                rf::obj_hide(obj);
            }
        }
    }
};

// id 120
struct EventSetDifficulty : rf::Event
{
    rf::GameDifficultyLevel difficulty = rf::GameDifficultyLevel::DIFFICULTY_EASY;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::int1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventSetDifficulty*>(event);
            this_event->difficulty = static_cast<rf::GameDifficultyLevel>(std::stoi(value));
        };
    }

    void turn_on() override
    {
        rf::game_set_skill_level(difficulty);
    }
};

// id 121
struct EventSetFogFarClip : rf::Event
{
    float far_clip = 0.0f;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::float1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventSetFogFarClip*>(event);
            this_event->far_clip = std::stof(value);
        };
    }

    void turn_on() override
    {
        rf::level.distance_fog_far_clip = far_clip;
    }
};

// id 122
struct EventAFWhenDead : rf::Event
{
    bool any_dead = 0;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::bool1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventAFWhenDead*>(event);
            this_event->any_dead = (value == "true");
        };
    }

    void turn_on() override
    {
        // only allow this event to fire when something its linked to dies
        if (!this->links.contains(this->trigger_handle)) {
            return;
        }

        // any_dead = true
        if (any_dead) {
            activate_links(this->trigger_handle, this->triggered_by_handle, true);
            return;
        }

        // any_dead = false
        bool all_dead_or_invalid = std::all_of(this->links.begin(), this->links.end(), [this](int link_handle) {
            if (link_handle == this->trigger_handle) {
                return true; // i know im dead
            }

            Object* obj = rf::obj_from_handle(link_handle);

            // not a valid object
            if (!obj) {
                return true;
            }

            // if entity, also check if its dead
            if (obj && obj->type == rf::OT_ENTITY) {
                rf::Entity* entity = rf::entity_from_handle(obj->handle);
                return entity && entity->life <= 0;
            }

            // only care about entities and clutter
            return obj->type != rf::OT_CLUTTER && obj->type != rf::OT_ENTITY;
        });

        // everything else isn't dead yet
        if (!all_dead_or_invalid) {
            return;
        }

        // everything else is also dead, fire
        activate_links(this->trigger_handle, this->triggered_by_handle, true);
    }

    void do_activate_links(int trigger_handle, int triggered_by_handle, bool on) override
    {
        for (int link_handle : this->links) {
            Object* obj = rf::obj_from_handle(link_handle);

            if (obj) {
                rf::ObjectType type = obj->type;
                switch (type) {
                    case rf::OT_MOVER: {
                        rf::mover_activate_from_trigger(obj->handle, -1, -1);
                        break;
                    }
                    case rf::OT_TRIGGER: {
                        rf::Trigger* trigger = static_cast<rf::Trigger*>(obj);
                        rf::trigger_enable(trigger);
                        break;
                    }
                    case rf::OT_EVENT: {
                        rf::Event* event = static_cast<Event*>(obj);
                        // Note can't use activate because it isn't allocated for stock events
                        rf::event_signal_on(link_handle, -1, -1);
                        break;
                    }
                    default:
                        break;
                }
            }
        }
    }
};

// id 123
struct EventGametypeGate : rf::Event
{
    rf::NetGameType gametype = rf::NG_TYPE_DM;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::int1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventGametypeGate*>(event);
            this_event->gametype = static_cast<rf::NetGameType>(std::stoi(value));
        };
    }

    void turn_on() override
    {
        if (rf::is_multi && gametype == rf::multi_get_game_type()) {
            activate_links(this->trigger_handle, this->triggered_by_handle, true);
        }
    }
};

// id 124
struct EventWhenPickedUp : rf::Event
{
    void turn_on() override
    {
        // only allow this event to fire when something its linked to dies
        if (!this->links.contains(this->trigger_handle)) {
            return;
        }

        activate_links(this->trigger_handle, this->triggered_by_handle, true);
    }

    void do_activate_links(int trigger_handle, int triggered_by_handle, bool on) override
    {
        for (int link_handle : this->links) {
            Object* obj = rf::obj_from_handle(link_handle);

            if (obj) {
                rf::ObjectType type = obj->type;
                switch (type) {
                case rf::OT_MOVER: {
                    rf::mover_activate_from_trigger(obj->handle, -1, -1);
                    break;
                }
                case rf::OT_TRIGGER: {
                    rf::Trigger* trigger = static_cast<rf::Trigger*>(obj);
                    rf::trigger_enable(trigger);
                    break;
                }
                case rf::OT_EVENT: {
                    Event* event = static_cast<Event*>(obj);
                    // Note can't use activate because it isn't allocated for stock events
                    rf::event_signal_on(link_handle, -1, -1);
                    break;
                }
                default:
                    break;
                }
            }
        }
    }
};

// id 125
struct EventSetSkybox : rf::Event
{
    int new_sky_room_uid = 0;
    int new_sky_room_anchor_uid = 0;
    float position_scale = 0.0f;
    bool relative_position = false;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::int1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventSetSkybox*>(event);
            this_event->new_sky_room_uid = std::stoi(value);
        };

        handlers[SetVarOpts::int2] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventSetSkybox*>(event);
            this_event->new_sky_room_anchor_uid = std::stoi(value);
        };

        handlers[SetVarOpts::float1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventSetSkybox*>(event);
            this_event->position_scale = std::stof(value);
        };

        handlers[SetVarOpts::bool1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventSetSkybox*>(event);
            this_event->relative_position = (value == "true");
        };
    }

    void turn_on() override
    {
        set_sky_room_uid_override(new_sky_room_uid, new_sky_room_anchor_uid, relative_position, position_scale);
    }

    // turning off restores stock sky room via original game logic
    void turn_off() override
    {
        set_sky_room_uid_override(-1, -1, false, -1);
    }
};

// id 126
struct EventSetLife : rf::Event
{
    float new_life = 0.0f;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::float1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventSetLife*>(event);
            this_event->new_life = std::stof(value);
        };
    }

    void turn_on() override
    {
        for (int link_handle : this->links) {
            Object* obj = rf::obj_from_handle(link_handle);

            if (obj && (obj->type == rf::OT_ENTITY || obj->type == rf::OT_CLUTTER)) {
                obj->life = new_life;
            }
        }
    }
};

// id 127
struct EventSetDebris : rf::Event
{
    std::string debris_filename = "";
    std::string debris_sound_set = "";
    int explode_anim_vclip = 0;
    float explode_anim_radius = 0.0f;
    float debris_velocity = 0.0f;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::str1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventSetDebris*>(event);
            this_event->debris_filename = value;
        };

        handlers[SetVarOpts::str2] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventSetDebris*>(event);
            this_event->debris_sound_set = value;
        };

        handlers[SetVarOpts::int1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventSetDebris*>(event);
            this_event->explode_anim_vclip = std::stoi(value);
        };

        handlers[SetVarOpts::float1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventSetDebris*>(event);
            this_event->explode_anim_radius = std::stof(value);
        };

        handlers[SetVarOpts::float2] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventSetDebris*>(event);
            this_event->debris_velocity = std::stof(value);
        };
    }

    void turn_on() override
    {
        for (int link_handle : this->links) {
            Object* obj = rf::obj_from_handle(link_handle);

            if (obj) {                
                if (obj->type == rf::OT_CLUTTER) {
                    rf::Clutter* cl = static_cast<rf::Clutter*>(obj);
                    cl->info->debris_filename = debris_filename.c_str();
                    cl->info->debris_sound_set = debris_sound_set.c_str();
                    cl->info->explode_anim_vclip = explode_anim_vclip;
                    cl->info->explode_anim_radius = explode_anim_radius;
                    cl->info->debris_velocity = debris_velocity;
                }
                else if (obj->type == rf::OT_ENTITY) {
                    rf::Entity* ep = static_cast<rf::Entity*>(obj);
                    ep->info->debris_filename = debris_filename.c_str();
                    ep->info->explode_vclip_index = explode_anim_vclip;
                    ep->info->explode_vclip_radius = explode_anim_radius;
                }
            }

        }
    }
};

// id 128
struct EventSetFogColor : rf::Event
{
    std::string fog_color = "";

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::str1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventSetFogColor*>(event);
            this_event->fog_color = value;
        };
    }

    void turn_on() override
    {
        try {
            rf::Color color = rf::Color::from_rgb_string(fog_color);
            rf::level.distance_fog_color = color;
        }
        catch (const std::exception& e) {
            xlog::error("Set_Fog_Color ({}) failed to set fog color: {}", this->uid, e.what());
        }
    }
};

// id 129
struct EventSetEntityFlag : rf::Event
{
    SetEntityFlagOption flag = SetEntityFlagOption::boarded;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::int1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventSetEntityFlag*>(event);
            this_event->flag = static_cast<SetEntityFlagOption>(std::stoi(value));
        };
    }

    void turn_on() override
    {
        for (int link_handle : this->links) {
            Object* obj = rf::obj_from_handle(link_handle);

            if (obj && obj->type == rf::OT_ENTITY) {
                rf::Entity* ep = static_cast<rf::Entity*>(obj);
                //xlog::warn("attem {},,, {}", static_cast<int>(flag), ep->info->flags);
                switch (flag) {
                case SetEntityFlagOption::boarded:
                    ep->entity_flags |= 0x10000;
                    break;
                case SetEntityFlagOption::cower:
                    ep->entity_flags |= 0x800000;
                    break;
                case SetEntityFlagOption::question_unarmed:
                    ep->entity_flags |= 0x1000000;
                    break;
                case SetEntityFlagOption::fade_corpse_immediately:
                    ep->entity_flags |= 0x4000000;
                    break;
                case SetEntityFlagOption::dont_hum:
                    ep->entity_flags2 |= 0x1;
                    break;
                case SetEntityFlagOption::no_shadow:
                    ep->entity_flags2 |= rf::EF2_NO_SHADOW;
                    break;
                case SetEntityFlagOption::perfect_aim:
                    ep->entity_flags2 |= 0x4;
                    break;
                case SetEntityFlagOption::permanent_corpse:
                    ep->entity_flags2 |= 0x8;
                    break;
                case SetEntityFlagOption::always_face_player:
                    ep->ai.ai_flags |= 0x8;
                    break;
                case SetEntityFlagOption::only_attack_player:
                    ep->ai.ai_flags |= 0x80;
                    break;
                case SetEntityFlagOption::deaf:
                    ep->ai.ai_flags |= 0x800;
                    break;
                case SetEntityFlagOption::ignore_terrain:
                    ep->ai.ai_flags |= 0x1000;
                    break;
                default:
                    break;
                }
                //xlog::warn("attem {},,, {}", static_cast<int>(flag), ep->info->flags);
            }
        }
    }

    void turn_off() override
    {
        for (int link_handle : this->links) {
            Object* obj = rf::obj_from_handle(link_handle);

            if (obj && obj->type == rf::OT_ENTITY) {
                rf::Entity* ep = static_cast<rf::Entity*>(obj);
                //xlog::warn("attem {},,, {}", static_cast<int>(flag), ep->info->flags);
                switch (flag) {
                case SetEntityFlagOption::boarded:
                    ep->entity_flags &= ~0x10000;
                    break;
                case SetEntityFlagOption::cower:
                    ep->entity_flags &= ~0x800000;
                    break;
                case SetEntityFlagOption::question_unarmed:
                    ep->entity_flags &= ~0x1000000;
                    break;
                case SetEntityFlagOption::fade_corpse_immediately:
                    ep->entity_flags &= ~0x4000000;
                    break;
                case SetEntityFlagOption::dont_hum:
                    ep->entity_flags2 &= ~0x1;
                    break;
                case SetEntityFlagOption::no_shadow:
                    ep->entity_flags2 &= ~rf::EF2_NO_SHADOW;
                    break;
                case SetEntityFlagOption::perfect_aim:
                    ep->entity_flags2 &= ~0x4;
                    break;
                case SetEntityFlagOption::permanent_corpse:
                    ep->entity_flags2 &= ~0x8;
                    break;
                case SetEntityFlagOption::always_face_player:
                    ep->ai.ai_flags &= ~0x8;
                    break;
                case SetEntityFlagOption::only_attack_player:
                    ep->ai.ai_flags &= ~0x80;
                    break;
                case SetEntityFlagOption::deaf:
                    ep->ai.ai_flags &= ~0x800;
                    break;
                case SetEntityFlagOption::ignore_terrain:
                    ep->ai.ai_flags &= ~0x1000;
                    break;
                default:
                    break;
                }
                //xlog::warn("attem {},,, {}", static_cast<int>(flag), ep->info->flags);
            }
        }
    }
};

// id 130
struct EventAFTeleportPlayer : rf::Event
{
    bool reset_velocity = false;
    bool force_exit_vehicle = false;
    std::string entrance_vclip = "";
    std::string exit_vclip = "";        

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::bool1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventAFTeleportPlayer*>(event);
            this_event->reset_velocity = (value == "true");
        };

        handlers[SetVarOpts::bool2] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventAFTeleportPlayer*>(event);
            this_event->force_exit_vehicle = (value == "true");
        };

        handlers[SetVarOpts::str1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventAFTeleportPlayer*>(event);
            this_event->entrance_vclip = value;
        };

        handlers[SetVarOpts::str2] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventAFTeleportPlayer*>(event);
            this_event->exit_vclip = value;
        };
    }

    void turn_on() override
    {
        rf::Entity* teleported_entity = rf::entity_from_handle(this->triggered_by_handle);
        
        if (teleported_entity) {
            //xlog::warn("teleporting {}, exit dir {},{},{}", teleported_entity->name, this->orient.fvec.x, this->orient.fvec.y, this->orient.fvec.z);

            // Handle ejection from vehicle
            if (!rf::is_multi) {
                if (force_exit_vehicle) {
                    rf::entity_detach_from_host(teleported_entity);
                }
                else {
                    rf::Entity* host_vehicle = rf::entity_from_handle(teleported_entity->host_handle);
                    if (host_vehicle) {
                        teleported_entity = host_vehicle;
                    }
                }
            }

            // Play entrance vclip
            if (!entrance_vclip.empty()) {
                int entrance_vclip_id = rf::vclip_lookup(entrance_vclip.c_str());
                if (entrance_vclip_id > -1) {
                    rf::vclip_play_3d(entrance_vclip_id, teleported_entity->room, &teleported_entity->pos,
                                        &teleported_entity->pos, 1.0f, -1, &teleported_entity->orient.fvec, 1);
                }
            }

            // Teleport player position
            teleported_entity->p_data.next_pos = this->pos;
            teleported_entity->move(&this->pos);
            if (teleported_entity->obj_interp) {
                teleported_entity->obj_interp->Clear();
            }

            // Rotate velocity on exit
            if (reset_velocity) {
                teleported_entity->p_data.vel.zero();
            }
            else {
                teleported_entity->p_data.vel =
                    rotate_velocity(teleported_entity->p_data.vel, teleported_entity->p_data.orient, this->orient);
            }

            // Teleport player orientation
            teleported_entity->orient = this->orient;
            teleported_entity->p_data.orient = this->orient;
            teleported_entity->p_data.next_orient = this->orient;
            teleported_entity->eye_orient = this->orient;

            float pitch = 0.0f, roll = 0.0f, yaw = 0.0f;
            teleported_entity->orient.extract_angles(&pitch, &roll, &yaw);

            teleported_entity->control_data.phb.set(-pitch, yaw, roll);

            // Play exit vclip
            if (!exit_vclip.empty()) {
                int exit_vclip_id = rf::vclip_lookup(exit_vclip.c_str());
                if (exit_vclip_id > -1) {
                    rf::vclip_play_3d(
                        exit_vclip_id, this->room, &this->pos, &this->pos, 1.0f, -1, &this->orient.fvec, 1);
                }
            }
        }
    }
};

// id 131
struct EventSetItemDrop : rf::Event
{
    std::string item_name = "";

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::str1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventSetItemDrop*>(event);
            this_event->item_name = value;
        };
    }

    void turn_on() override
    {
        for (int link_handle : this->links) {
            Object* obj = rf::obj_from_handle(link_handle);

            if (obj && obj->type == rf::OT_ENTITY) {
                rf::Entity* ep = static_cast<rf::Entity*>(obj);

                int item_id_to_drop = rf::item_lookup_type(item_name.c_str());
                if (item_id_to_drop > -1) {
                    ep->drop_item_class = item_id_to_drop;
                }
            }
        }
    }
};

// id 132
struct EventAFHeal : rf::Event
{
    int amount = 0;
    AFHealTargetOption target = AFHealTargetOption::linked;
    bool apply_to_armor = false;
    bool super = false;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::int1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventAFHeal*>(event);
            this_event->amount = std::stoi(value);
        };

        handlers[SetVarOpts::int2] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventAFHeal*>(event);
            this_event->target = static_cast<AFHealTargetOption>(std::stoi(value));
        };

        handlers[SetVarOpts::bool1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventAFHeal*>(event);
            this_event->apply_to_armor = (value == "true");
        };

        handlers[SetVarOpts::bool2] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventAFHeal*>(event);
            this_event->super = (value == "true");
        };
    }

    void turn_on() override
    {
        auto apply_healing = [&](rf::Entity* ep) {
            if (ep && (!rf::is_multi || rf::is_dedicated_server || rf::is_server)) {
                if (apply_to_armor) {
                    ep->armor = std::clamp(ep->armor + amount, 0.0f, super ? 200.0f : 100.0f);
                }
                else {
                    ep->life = std::clamp(ep->life + amount, 0.0f, super ? 200.0f : 100.0f);
                }
            }
        };

        auto player_list = SinglyLinkedList{rf::player_list};

        switch (target) {
            case AFHealTargetOption::linked: {
                if (!this->links.empty()) {
                    for (int link_handle : this->links) {
                        rf::Entity* ep = rf::entity_from_handle(link_handle);
                        if (ep) {
                            apply_healing(ep);
                        }
                    }
                }
                break;
            }

            case AFHealTargetOption::player: {
                rf::Entity* ep = rf::entity_from_handle(this->triggered_by_handle);
                apply_healing(ep);
                break;
            }

            case AFHealTargetOption::all_players: {
                for (auto& player : player_list) {
                    rf::Entity* ep = rf::entity_from_handle(player.entity_handle);
                    apply_healing(ep);
                }
                break;
            }

            case AFHealTargetOption::player_team: {
                rf::Entity* triggering_entity = rf::entity_from_handle(this->triggered_by_handle);
                if (triggering_entity) {
                    int team = triggering_entity->team;
                    for (auto& player : player_list) {
                        rf::Entity* ep = rf::entity_from_handle(player.entity_handle);
                        if (ep && ep->team == team) {
                            apply_healing(ep);
                        }
                    }
                }
                break;
            }

            case AFHealTargetOption::enemy_team: {
                rf::Entity* triggering_entity = rf::entity_from_handle(this->triggered_by_handle);
                if (triggering_entity) {
                    int team = triggering_entity->team;
                    for (auto& player : player_list) {
                        rf::Entity* ep = rf::entity_from_handle(player.entity_handle);
                        if (ep && ep->team != team) {
                            apply_healing(ep);
                        }
                    }
                }
                break;
            }

            case AFHealTargetOption::red_team: {
                for (auto& player : player_list) {
                    rf::Entity* ep = rf::entity_from_handle(player.entity_handle);
                    if (ep && ep->team == 0) { // Red team
                        apply_healing(ep);
                    }
                }
                break;
            }

            case AFHealTargetOption::blue_team: {
                for (auto& player : player_list) {
                    rf::Entity* ep = rf::entity_from_handle(player.entity_handle);
                    if (ep && ep->team == 1) { // Blue team
                        apply_healing(ep);
                    }
                }
                break;
            }

            case AFHealTargetOption::players_in_linked_triggers: {
                if (!rf::is_multi) {
                    break;
                }

                std::vector<rf::Trigger*> triggers_to_check;
                triggers_to_check.reserve(this->links.size());

                for (int link_handle : this->links) {
                    Object* obj = rf::obj_from_handle(link_handle);
                    if (obj && obj->type == rf::OT_TRIGGER) {
                        triggers_to_check.push_back(static_cast<rf::Trigger*>(obj));
                    }
                }

                if (triggers_to_check.empty()) {
                    xlog::warn("AF_Heal UID {} has no linked triggers to check", this->uid);
                    break;
                }

                for (auto& player : player_list) {
                    rf::Entity* ep = rf::entity_from_handle(player.entity_handle);
                    if (!ep) {
                        continue;
                    }

                    bool is_inside = false;
                    for (rf::Trigger* trigger : triggers_to_check) {
                        is_inside = (trigger->type == 0)
                            ? rf::trigger_inside_bounding_sphere(trigger, ep)
                            : (trigger->type == 1 && rf::trigger_inside_bounding_box(trigger, ep));
                        if (is_inside) {
                            break;
                        }
                    }

                    if (is_inside) {
                        apply_healing(ep);
                    }
                }
                break;
            }
        }
    }
};

// id 133
struct EventAnchorMarkerOrient : rf::Event {}; // logic handled in object.cpp

// id 134
struct EventLightState : rf::Event
{
    void turn_on() override
    {
        for (const auto& linked_uid : this->links) {
            if (auto* light = static_cast<rf::gr::Light*>(rf::gr::light_get_from_handle(rf::gr::level_get_light_handle_from_uid(linked_uid)))) {
                light->on = true;
            }
        }
    }

    void turn_off() override
    {
        for (const auto& linked_uid : this->links) {
            if (auto* light = static_cast<rf::gr::Light*>(rf::gr::light_get_from_handle(rf::gr::level_get_light_handle_from_uid(linked_uid)))) {
                light->on = false;
            }
        }
    }
};

// id 135
struct EventWorldHUDSprite : rf::Event
{
    bool enabled = false;
    WorldHUDRenderMode render_mode = WorldHUDRenderMode::no_overdraw;
    float scale = 0.0f;
    std::string sprite_filename = "";
    std::string sprite_filename_blue = "";

    std::optional<int> sprite_filename_int;
    std::optional<int> sprite_filename_blue_int;

    // optimization to avoid running bm_load every frame
    // build_sprite_ints is run once during level_init_post and every time the event is turned on
    void build_sprite_ints() {
        if (!sprite_filename.empty()) {
            sprite_filename_int = rf::bm::load(sprite_filename.c_str(), -1, true);
        }

        if (!sprite_filename_blue.empty()) {
            sprite_filename_blue_int = rf::bm::load(sprite_filename_blue.c_str(), -1, true);
        }
    }

    void turn_on() override
    {
        build_sprite_ints();
        enabled = true;
    }

    void turn_off() override
    {
        enabled = false;
    }
};

// id 136
struct EventSetLightColor : rf::Event
{
    std::string light_color = "";
    bool randomize = false;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::str1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventSetLightColor*>(event);
            this_event->light_color = value;
        };

        handlers[SetVarOpts::bool1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventSetLightColor*>(event);
            this_event->randomize = (value == "true");
        };
    }

    void turn_on() override
    {
        try {
            rf::Color color;

            if (randomize) {
                std::uniform_int_distribution<int> dist(0, 255);
                color.red = static_cast<rf::ubyte>(dist(g_rng));
                color.green = static_cast<rf::ubyte>(dist(g_rng));
                color.blue = static_cast<rf::ubyte>(dist(g_rng));
            }
            else {
                color = rf::Color::from_rgb_string(light_color);
            }

            for (const auto& linked_uid : this->links) {
                if (auto* light = static_cast<rf::gr::Light*>(rf::gr::light_get_from_handle(rf::gr::level_get_light_handle_from_uid(linked_uid)))) {
                    light->r = static_cast<float>(color.red) / 255.0f;
                    light->g = static_cast<float>(color.green) / 255.0f;
                    light->b = static_cast<float>(color.blue) / 255.0f;
                }
            }
        }
        catch (const std::exception& e) {
            xlog::error("Set_Light_Color ({}) failed to set light color: {}", this->uid, e.what());
        }
    }
};

// id 137
struct EventCapturePointHandler : rf::Event
{
    std::string name = "";
    int trigger_uid = -1;
    float outline_offset = 0.0f;
    float capture_rate = 0.0f;
    bool sphere_to_cylinder = false;
    int stage = 0;
    int position = 0; // HillRole

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::str1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventCapturePointHandler*>(event);
            this_event->name = value;
        };

        handlers[SetVarOpts::float1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventCapturePointHandler*>(event);
            this_event->outline_offset = std::stof(value);
        };

        handlers[SetVarOpts::float2] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventCapturePointHandler*>(event);
            this_event->capture_rate = std::stof(value);
        };
    }
};

// id 138
struct EventRespawnPointState : rf::Event
{
    void turn_on() override
    {
        try {
            for (const auto& linked_uid : this->links) {
                if (auto* rp = get_alpine_respawn_point_by_uid(linked_uid)) {
                    set_alpine_respawn_point_enabled(rp, true);
                }
            }
        }
        catch (const std::exception& e) {
            xlog::error("Respawn_Point_State ({}) failed to set enabled: {}", this->uid, e.what());
        }
    }

    void turn_off() override
    {
        try {
            for (const auto& linked_uid : this->links) {
                if (auto* rp = get_alpine_respawn_point_by_uid(linked_uid)) {
                    set_alpine_respawn_point_enabled(rp, false);
                }
            }
        }
        catch (const std::exception& e) {
            xlog::error("Respawn_Point_State ({}) failed to set disabled: {}", this->uid, e.what());
        }
    }
};

// id 139
struct EventModifyRespawnPoint : rf::Event
{
    bool red = false;
    bool blue = false;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::bool1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventModifyRespawnPoint*>(event);
            this_event->red = (value == "true");
        };

        handlers[SetVarOpts::bool2] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventModifyRespawnPoint*>(event);
            this_event->blue = (value == "true");
        };
    }

    void turn_on() override
    {
        try {
            for (const auto& linked_uid : this->links) {
                if (auto* rp = get_alpine_respawn_point_by_uid(linked_uid)) {
                    set_alpine_respawn_point_teams(rp, red, blue);
                }
            }
        }
        catch (const std::exception& e) {
            xlog::error("Modify_Respawn_Point ({}) failed to set teams: {}", this->uid, e.what());
        }
    }
};

// id 140
struct EventWhenCaptured : rf::Event
{
    void turn_on() override
    {
        if (!this->links.contains(this->trigger_handle)) {
            return;
        }

        activate_links(this->trigger_handle, this->triggered_by_handle, true);
    }

    void do_activate_links(int trigger_handle, int triggered_by_handle, bool on) override
    {
        for (int link_handle : this->links) {
            Object* obj = rf::obj_from_handle(link_handle);

            if (obj) {
                rf::ObjectType type = obj->type;
                switch (type) {
                    case rf::OT_MOVER: {
                        rf::mover_activate_from_trigger(obj->handle, -1, -1);
                        break;
                    }
                    case rf::OT_TRIGGER: {
                        rf::Trigger* trigger = static_cast<rf::Trigger*>(obj);
                        rf::trigger_enable(trigger);
                        break;
                    }
                    case rf::OT_EVENT: {
                        rf::Event* event = static_cast<Event*>(obj);
                        if (event->event_type !=
                            static_cast<int>(rf::EventType::Capture_Point_Handler)) { // don't activate the handler
                            rf::event_signal_on(link_handle, -1, -1);
                        }
                        break;
                    }
                    default:
                        break;
                    }
                }
        }
    }
};

// id 141
struct EventSetCapturePointOwner : rf::Event
{
    int owner = 0; // HillOwner

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::int1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventSetCapturePointOwner*>(event);
            this_event->owner = std::stoi(value);
        };
    }

    void turn_on() override
    {
        apply_owner();
    }

private:
    void apply_owner()
    {
        if (!rf::is_multi || !rf::is_server) {
            return; // ignore on client
        }

        bool found_any = false;
        bool changed_any = false;

        for (int link_handle : this->links) {
            if (auto* obj = rf::obj_from_handle(link_handle)) {
                if (obj->type != rf::ObjectType::OT_EVENT)
                    continue;

                auto* linked_event = static_cast<rf::Event*>(obj);
                if (linked_event->event_type != rf::event_type_to_int(rf::EventType::Capture_Point_Handler))
                    continue;

                auto* handler = static_cast<EventCapturePointHandler*>(linked_event);
                found_any = true;
                if (koth_set_capture_point_owner(handler, owner)) {
                    changed_any = true;
                }
            }
        }

        if (!found_any) {
            xlog::warn("Set_Capture_Point_Owner ({}) did not find any linked capture points", this->uid);
        }
        else if (!changed_any) {
            xlog::debug("Set_Capture_Point_Owner ({}) left capture point owners unchanged", this->uid);
        }
    }
};

// id 142
struct EventOwnerGate : rf::Event
{
    int handler_uid = -1;
    int required_owner = 0; // HillOwner

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::int1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventOwnerGate*>(event);
            this_event->handler_uid = std::stoi(value);
        };

        handlers[SetVarOpts::int2] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventOwnerGate*>(event);
            this_event->required_owner = std::stoi(value);
        };
    }

    void turn_on() override
    {
        if (condition_passes()) {
            activate_links(this->trigger_handle, this->triggered_by_handle, true);
        }
    }

    void turn_off() override
    {
        if (condition_passes()) {
            activate_links(this->trigger_handle, this->triggered_by_handle, false);
        }
    }

private:
    bool condition_passes() const
    {
        if (handler_uid < 0) {
            return false;
        }

        Object* obj = rf::obj_lookup_from_uid(handler_uid);
        if (!obj || obj->type != rf::ObjectType::OT_EVENT) {
            return false;
        }

        auto* linked_event = static_cast<Event*>(obj);
        if (linked_event->event_type != rf::event_type_to_int(rf::EventType::Capture_Point_Handler)) {
            return false;
        }

        auto* handler = static_cast<EventCapturePointHandler*>(linked_event);
        if (auto* hill = koth_find_hill_by_handler(handler)) {
            return static_cast<int>(hill->ownership) == required_owner;
        }

        return false;
    }
};

// id 143
struct EventSetGameplayRule : rf::Event
{
    GameplayRule rule = GameplayRule::player_has_headlamp;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::int1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventSetGameplayRule*>(event);
            this_event->rule = static_cast<GameplayRule>(std::stoi(value));
        };
    }

    void turn_on() override
    {
        switch (rule) {
            case GameplayRule::player_has_headlamp:
                set_headlamp_toggle_enabled(true);
                break;
            default:
                break;
        }
    }

    void turn_off() override
    {
        switch (rule) {
            case GameplayRule::player_has_headlamp:
                set_headlamp_toggle_enabled(false);
                break;
            default:
                break;
        }
    }
};

// id 144
struct EventWhenRoundEnds : rf::Event
{
    void turn_on() override
    {
        activate_links(this->trigger_handle, this->triggered_by_handle, true);
    }

    void do_activate_links(int trigger_handle, int triggered_by_handle, bool on) override
    {
        for (int link_handle : this->links) {
            Object* obj = rf::obj_from_handle(link_handle);

            if (obj) {
                rf::ObjectType type = obj->type;
                switch (type) {
                case rf::OT_MOVER: {
                    rf::mover_activate_from_trigger(obj->handle, -1, -1);
                    break;
                }
                case rf::OT_TRIGGER: {
                    rf::Trigger* trigger = static_cast<rf::Trigger*>(obj);
                    rf::trigger_enable(trigger);
                    break;
                }
                case rf::OT_EVENT: {
                    rf::Event* event = static_cast<Event*>(obj);
                    // Note can't use activate because it isn't allocated for stock events
                    rf::event_signal_on(link_handle, -1, -1);
                    break;
                }
                default:
                    break;
                }
            }
        }
    }
};

// id 145
struct EventMeshAnimate : rf::Event
{
    char padding_align[3];
    int animate_type = 0;        // 0=Action, 1=Action Hold Last, 2=State
    std::string anim_filename;
    float blend_weight = 1.0f;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();
        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::int1] = [](rf::Event* event, const std::string& value) {
            auto* e = static_cast<EventMeshAnimate*>(event);
            e->animate_type = std::stoi(value);
        };
        handlers[SetVarOpts::str1] = [](rf::Event* event, const std::string& value) {
            auto* e = static_cast<EventMeshAnimate*>(event);
            e->anim_filename = value;
        };
        handlers[SetVarOpts::float1] = [](rf::Event* event, const std::string& value) {
            auto* e = static_cast<EventMeshAnimate*>(event);
            e->blend_weight = std::stof(value);
        };
    }

    void turn_on() override
    {
        xlog::debug("[EventMeshAnimate] turn_on: uid={} type={} anim='{}' weight={:.2f} links={}",
            this->uid, animate_type, anim_filename, blend_weight,
            this->links.size());
        for (int i = 0; i < static_cast<int>(this->links.size()); i++) {
            int link_handle = this->links[i];
            Object* obj = rf::obj_from_handle(link_handle);
            if (obj) {
                alpine_mesh_animate(obj, animate_type, anim_filename, blend_weight);
            } else {
                xlog::warn("[EventMeshAnimate] link[{}]: handle={} -> NULL (stale handle!)", i, link_handle);
            }
        }
    }
};

// id 146
struct EventMeshSetTexture : rf::Event
{
    char padding_align[3];
    int texture_slot = 0;
    std::string texture_filename;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();
        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::int1] = [](rf::Event* event, const std::string& value) {
            auto* e = static_cast<EventMeshSetTexture*>(event);
            e->texture_slot = std::stoi(value);
        };
        handlers[SetVarOpts::str1] = [](rf::Event* event, const std::string& value) {
            auto* e = static_cast<EventMeshSetTexture*>(event);
            e->texture_filename = value;
        };
    }

    void turn_on() override
    {
        xlog::debug("[EventMeshSetTexture] turn_on: uid={} slot={} texture='{}'",
            this->uid, texture_slot, texture_filename);
        for (int i = 0; i < static_cast<int>(this->links.size()); i++) {
            int link_handle = this->links[i];
            Object* obj = rf::obj_from_handle(link_handle);
            if (obj) {
                if (texture_filename.empty()) {
                    alpine_mesh_clear_texture(obj, texture_slot);
                }
                else {
                    alpine_mesh_set_texture(obj, texture_slot, texture_filename);
                }
            } else {
                xlog::warn("[EventMeshSetTexture]   link[{}]: handle={} -> NULL (stale handle!)", i, link_handle);
            }
        }
    }
};

// id 147
struct EventMeshSetCollision : rf::Event
{
    char padding_align[3];
    int collision_type = 0; // 0=None, 1=Only Weapons, 2=All

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();
        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::int1] = [](rf::Event* event, const std::string& value) {
            auto* e = static_cast<EventMeshSetCollision*>(event);
            e->collision_type = std::stoi(value);
        };
    }

    void turn_on() override
    {
        xlog::debug("[EventMeshSetCollision] turn_on: uid={} collision_type={} links={}",
            this->uid, collision_type, this->links.size());
        for (int i = 0; i < static_cast<int>(this->links.size()); i++) {
            int link_handle = this->links[i];
            Object* obj = rf::obj_from_handle(link_handle);
            if (obj) {
                alpine_mesh_set_collision(obj, collision_type);
            } else {
                xlog::warn("[EventMeshSetCollision] link[{}]: handle={} -> NULL", i, link_handle);
            }
        }
    }
};

// Base class for fullscreen overlay events
struct EventFullscreenOverlayBase : rf::Event
{
    float duration = 0.0f;
    float transition_time = 0.0f;
    FullscreenTransitionType transition_type = FullscreenTransitionType::disappear;
    int max_alpha_raw = 0; // 0 means 255 (full opacity)

    // Runtime state (not serialized)
    bool active = false;
    bool fading_out_early = false;
    HighResTimer main_timer;
    HighResTimer fadeout_timer;
    float fadeout_start_alpha = 1.0f;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();
        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::float1] = [](rf::Event* event, const std::string& value) {
            auto* e = static_cast<EventFullscreenOverlayBase*>(event);
            e->duration = std::stof(value);
        };
        handlers[SetVarOpts::float2] = [](rf::Event* event, const std::string& value) {
            auto* e = static_cast<EventFullscreenOverlayBase*>(event);
            e->transition_time = std::max(0.0f, std::stof(value));
        };
        handlers[SetVarOpts::int1] = [](rf::Event* event, const std::string& value) {
            auto* e = static_cast<EventFullscreenOverlayBase*>(event);
            int raw = std::stoi(value);
            if (raw < 0 || raw > 3) raw = 0;
            e->transition_type = static_cast<FullscreenTransitionType>(raw);
        };
        handlers[SetVarOpts::int2] = [](rf::Event* event, const std::string& value) {
            auto* e = static_cast<EventFullscreenOverlayBase*>(event);
            e->max_alpha_raw = std::stoi(value);
        };
    }

    float effective_max_alpha() const
    {
        return (max_alpha_raw <= 0 || max_alpha_raw > 255) ? 1.0f : static_cast<float>(max_alpha_raw) / 255.0f;
    }

    bool has_fade_in() const
    {
        return transition_type == FullscreenTransitionType::fade_in_disappear ||
               transition_type == FullscreenTransitionType::fade_in_fade_out;
    }

    bool has_fade_out() const
    {
        return transition_type == FullscreenTransitionType::fade_in_fade_out ||
               transition_type == FullscreenTransitionType::appear_fade_out;
    }

    float compute_alpha() const
    {
        float trans = transition_time;
        float max_a = effective_max_alpha();

        // Early fade-out override
        if (fading_out_early) {
            if (trans <= 0.0f) return 0.0f;
            return fadeout_start_alpha * (1.0f - fadeout_timer.elapsed_frac());
        }

        float elapsed = main_timer.time_since_sec();
        bool infinite = (duration <= 0.0f);

        // Fade-in phase
        if (has_fade_in() && trans > 0.0f && elapsed < trans) {
            return (elapsed / trans) * max_a;
        }

        // Hold phase
        float after_fadein = elapsed - (has_fade_in() ? trans : 0.0f);
        if (infinite || after_fadein < duration) {
            return max_a;
        }

        // Fade-out phase
        if (has_fade_out() && trans > 0.0f) {
            float fadeout_elapsed = after_fadein - duration;
            float progress = fadeout_elapsed / trans;
            if (progress >= 1.0f) return 0.0f;
            return (1.0f - progress) * max_a;
        }

        return 0.0f;
    }

    bool is_finished() const
    {
        if (fading_out_early) {
            if (transition_time <= 0.0f) return true;
            return fadeout_timer.elapsed();
        }

        if (duration <= 0.0f) return false; // infinite hold

        return main_timer.elapsed();
    }

    void turn_on() override
    {
        active = true;
        fading_out_early = false;
        float fade_in_time = has_fade_in() ? transition_time : 0.0f;
        float fade_out_time = has_fade_out() ? transition_time : 0.0f;
        float total_time = fade_in_time + duration + fade_out_time;
        main_timer.set_sec(total_time);
    }

    void turn_off() override
    {
        if (!active) return;

        if (has_fade_out() && transition_time > 0.0f && !fading_out_early) {
            fadeout_start_alpha = compute_alpha(); // capture before setting fading_out_early
            fading_out_early = true;
            fadeout_timer.set_sec(transition_time);
        }
        else {
            active = false;
        }
    }

    virtual void render(float alpha) = 0;
};

// id 148
struct EventFullscreenImage : EventFullscreenOverlayBase
{
    std::string filename;
    int bitmap_handle = -1;
    int bm_w = 0;
    int bm_h = 0;

    void register_variable_handlers() override
    {
        EventFullscreenOverlayBase::register_variable_handlers();
        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::str1] = [](rf::Event* event, const std::string& value) {
            auto* e = static_cast<EventFullscreenImage*>(event);
            e->filename = value;
        };
    }

    void turn_on() override
    {
        bitmap_handle = -1;
        bm_w = 0;
        bm_h = 0;
        if (filename.empty()) {
            xlog::warn("AF_Fullscreen_Image ({}): no filename set", this->uid);
        }
        else {
            bitmap_handle = rf::bm::load(filename.c_str(), -1, true);
            if (bitmap_handle < 0) {
                xlog::warn("AF_Fullscreen_Image ({}): failed to load '{}'", this->uid, filename);
            }
            if (has_fade_out() && transition_time > 0.0f && !fading_out_early) {
                fadeout_start_alpha = compute_alpha(); // capture before setting fading_out_early
                fading_out_early = true;
                fadeout_timer.set_sec(transition_time);
            }
            else {
                rf::bm::get_dimensions(bitmap_handle, &bm_w, &bm_h);
            }
        }
        EventFullscreenOverlayBase::turn_on();
    }

    void render(float alpha) override
    {
        if (bitmap_handle < 0 || bm_w <= 0 || bm_h <= 0) return;
        int a = static_cast<int>(alpha * 255.0f);
        if (a <= 0) return;
        if (a > 255) a = 255;

        rf::gr::set_color(255, 255, 255, a);
        static rf::gr::Mode mode{
            rf::gr::TEXTURE_SOURCE_CLAMP,
            rf::gr::COLOR_SOURCE_TEXTURE,
            rf::gr::ALPHA_SOURCE_VERTEX_TIMES_TEXTURE,
            rf::gr::ALPHA_BLEND_ALPHA,
            rf::gr::ZBUFFER_TYPE_NONE,
            rf::gr::FOG_NOT_ALLOWED,
        };
        gr_bitmap_scaled_float(bitmap_handle, 0.0f, 0.0f,
            static_cast<float>(rf::gr::screen.max_w), static_cast<float>(rf::gr::screen.max_h),
            0.0f, 0.0f, static_cast<float>(bm_w), static_cast<float>(bm_h),
            false, false, mode);
    }
};

// id 149
struct EventFullscreenColor : EventFullscreenOverlayBase
{
    std::string color_string;
    rf::gr::Color parsed_color{0, 0, 0, 255};

    void register_variable_handlers() override
    {
        EventFullscreenOverlayBase::register_variable_handlers();
        auto& handlers = variable_handler_storage[this];

        handlers[SetVarOpts::str1] = [](rf::Event* event, const std::string& value) {
            auto* e = static_cast<EventFullscreenColor*>(event);
            e->color_string = value;
        };
    }

    void turn_on() override
    {
        parsed_color = {0, 0, 0, 255};
        if (!color_string.empty()) {
            try {
                parsed_color = rf::gr::Color::from_rgb_string(color_string);
            }
            catch (const std::exception& e) {
                xlog::error("AF_Fullscreen_Color ({}) failed to parse color '{}': {}",
                    this->uid, color_string, e.what());
            }
        }
        EventFullscreenOverlayBase::turn_on();
    }

    void render(float alpha) override
    {
        int a = static_cast<int>(alpha * 255.0f);
        if (a <= 0) return;
        if (a > 255) a = 255;

        rf::gr::set_color(parsed_color.red, parsed_color.green, parsed_color.blue, a);
        static rf::gr::Mode mode{
            rf::gr::TEXTURE_SOURCE_NONE,
            rf::gr::COLOR_SOURCE_VERTEX,
            rf::gr::ALPHA_SOURCE_VERTEX,
            rf::gr::ALPHA_BLEND_ALPHA,
            rf::gr::ZBUFFER_TYPE_NONE,
            rf::gr::FOG_NOT_ALLOWED,
        };
        rf::gr::rect(0, 0, rf::gr::screen.max_w, rf::gr::screen.max_h, mode);
    }
};

// id 150
struct EventUnhideGlare : rf::Event
{
    static void set_glares_enabled(const rf::VArray<int>& links, bool enabled)
    {
        for (rf::Glare* g = rf::glare_list.next; g != &rf::glare_list; g = g->next) {
            for (const int link : links) {
                if (g->parent_handle == link) {
                    g->enabled = enabled;
                    break;
                }
            }
        }
    }

    void turn_on() override
    {
        set_glares_enabled(this->links, true);
    }

    void turn_off() override
    {
        set_glares_enabled(this->links, false);
    }
};

// id 151
struct EventGasRegionState : rf::Event
{
    void turn_on() override
    {
        for (const auto& linked_uid : this->links) {
            if (auto* gas_region = gas_region_get_by_uid(linked_uid)) {
                gas_region->enabled = true;
            }
        }
    }

    void turn_off() override
    {
        for (const auto& linked_uid : this->links) {
            if (auto* gas_region = gas_region_get_by_uid(linked_uid)) {
                gas_region->enabled = false;
            }
        }
    }
};

// id 152
struct EventModifyGasRegion : rf::Event
{
    std::string color_string;
    float density = 1.0f;
    float transition_time = 0.0f;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::str1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventModifyGasRegion*>(event);
            this_event->color_string = value;
        };

        handlers[SetVarOpts::float1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventModifyGasRegion*>(event);
            this_event->density = std::stof(value);
        };

        handlers[SetVarOpts::float2] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventModifyGasRegion*>(event);
            this_event->transition_time = std::max(0.0f, std::stof(value));
        };
    }

    void turn_on() override
    {
        try {
            rf::Color color = rf::Color::from_rgb_string(color_string);

            for (const auto& linked_uid : this->links) {
                if (auto* gas_region = gas_region_get_by_uid(linked_uid)) {
                    if (transition_time > 0.0f) {
                        gas_region_add_modify_transition(linked_uid, color, density, transition_time);
                    }
                    else {
                        gas_region->color = color;
                        gas_region->density = density;
                    }
                }
            }
        }
        catch (const std::exception& e) {
            xlog::error("Modify_Gas_Region ({}) failed: {}", this->uid, e.what());
        }
    }
};

// id 153
struct EventResizeGasRegion : rf::Event
{
    int shape = 1; // 1=sphere, 2=box
    float sphere_radius = 1.0f;
    std::string box_dimensions; // HWD
    float transition_time = 0.0f;

    void register_variable_handlers() override
    {
        rf::Event::register_variable_handlers();

        auto& handlers = variable_handler_storage[this];
        handlers[SetVarOpts::int1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventResizeGasRegion*>(event);
            this_event->shape = std::stoi(value) + 1; // 0->sphere(1), 1->box(2)
        };

        handlers[SetVarOpts::float1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventResizeGasRegion*>(event);
            this_event->sphere_radius = std::stof(value);
        };

        handlers[SetVarOpts::str1] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventResizeGasRegion*>(event);
            this_event->box_dimensions = value;
        };

        handlers[SetVarOpts::float2] = [](rf::Event* event, const std::string& value) {
            auto* this_event = static_cast<EventResizeGasRegion*>(event);
            this_event->transition_time = std::max(0.0f, std::stof(value));
        };
    }

    void turn_on() override
    {
        try {
            auto dims = rf::Vector3::from_string(box_dimensions, rf::Vector3{1.0f, 1.0f, 1.0f});

            for (const auto& linked_uid : this->links) {
                if (auto* gas_region = gas_region_get_by_uid(linked_uid)) {
                    if (transition_time > 0.0f) {
                        gas_region_add_resize_transition(linked_uid, shape,
                            sphere_radius, dims.x, dims.y, dims.z, transition_time);
                    }
                    else {
                        gas_region->shape = shape;

                        if (shape == 1) { // sphere
                            gas_region->radius = sphere_radius;
                        }
                        else if (shape == 2) { // box - parsed as "H W D"
                            gas_region->height = dims.x;
                            gas_region->width = dims.y;
                            gas_region->depth = dims.z;
                        }
                    }
                }
            }
        }
        catch (const std::exception& e) {
            xlog::error("Resize_Gas_Region ({}) failed: {}", this->uid, e.what());
        }
    }
};