#pragma once

#include <string>
#include <vector>
#include <sstream>
#include <optional>
#include <cstddef>
#include <functional>
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
#include "../main/main.h"
#include "../rf/player/player.h"
#include "../rf/os/timestamp.h"

void set_sky_room_uid_override(int room_uid, int anchor_uid, bool relative_position, float position_scale);

namespace rf
{
    struct EventCreateParams
    {
        const Vector3* pos;
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

    enum class EnvironmentGateTests : int
    {
        multi,
        single,
        server,
        dedicated,
        client
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
        always_relevant, // todo: test and confirm this works
        always_face_player,
        only_attack_player,
        deaf,
        ignore_terrain
    };

    enum class SetClutterFlagOption : int
    {
        collide_weapon,
        collide_object,
        has_alpha,
        is_switch
    };

    // start alpine event structs
    // id 100
    struct EventSetVar : Event
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
                Object* obj = obj_from_handle(link_handle);
                if (obj && obj->type == OT_EVENT) {
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
    struct EventCloneEntity : Event
    {
        bool ignore_item_drop = 0;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::bool1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventCloneEntity*>(event);
                this_event->ignore_item_drop = (value == "true");
            };
        }

        void turn_on() override
        {
            for (int i = 0; i < this->links.size(); ++i) {

                int link = this->links[i];
                Object* obj = obj_from_handle(link);
                if (obj) {
                    Entity* entity = static_cast<Entity*>(obj);
                    Entity* new_entity =
                        entity_create(entity->info_index, entity->name, -1, pos, entity->orient, 0, -1);
                    new_entity->entity_flags = entity->entity_flags;
                    new_entity->pos = entity->pos;
                    new_entity->entity_flags2 = entity->entity_flags2;
                    new_entity->info = entity->info;
                    new_entity->info2 = entity->info2;
                    if (!ignore_item_drop) {
                        new_entity->drop_item_class = entity->drop_item_class;
                    }                    
                    new_entity->obj_flags = entity->obj_flags;
                    new_entity->ai.custom_attack_range = entity->ai.custom_attack_range;
                    new_entity->ai.use_custom_attack_range = entity->ai.use_custom_attack_range;
                    new_entity->ai.attack_style = entity->ai.attack_style;
                    new_entity->ai.cooperation = entity->ai.cooperation;
                    new_entity->ai.cover_style = entity->ai.cover_style;

                    for (int i = 0; i < 64; ++i) {
                        new_entity->ai.has_weapon[i] = entity->ai.has_weapon[i];
                        new_entity->ai.clip_ammo[i] = entity->ai.clip_ammo[i];
                    }

                    for (int i = 0; i < 32; ++i) {
                        new_entity->ai.ammo[i] = entity->ai.ammo[i];
                    }

                    new_entity->ai.current_secondary_weapon = entity->ai.current_secondary_weapon;
                    new_entity->ai.current_primary_weapon = entity->ai.current_primary_weapon;
                }
            }
        }
    };

    // id 102
    struct EventSetCollisionPlayer : Event
    {
        void turn_on() override
        {
            local_player->collides_with_world = true;
        }
        void turn_off() override
        {
            local_player->collides_with_world = false;
        }
    };

    // id 103
    struct EventSwitchRandom : Event
    {
        bool no_repeats = false;
        std::vector<int> used_handles;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::bool1] = [](Event* event, const std::string& value) {
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

                event_signal_on(link_handle, this->trigger_handle, this->triggered_by_handle);
            }
            else {
                // Standard random selection
                std::uniform_int_distribution<int> dist(0, this->links.size() - 1);
                int random_index = dist(g_rng);
                int link_handle = this->links[random_index];

                event_signal_on(link_handle, this->trigger_handle, this->triggered_by_handle);
            }
        }
    };

    // id 104
    struct EventDifficultyGate : Event
    {
        GameDifficultyLevel difficulty = GameDifficultyLevel::DIFFICULTY_EASY;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventDifficultyGate*>(event);
                this_event->difficulty = static_cast<GameDifficultyLevel>(std::stoi(value));
            };
        }

        void turn_on() override
        {
            if (game_get_skill_level() == difficulty) {
                activate_links(this->trigger_handle, this->triggered_by_handle, true);
            }
        }

        void turn_off() override
        {
            if (game_get_skill_level() == difficulty) {
                activate_links(this->trigger_handle, this->triggered_by_handle, false);
            }
        }
    };

    // id 105
    struct EventHUDMessage : Event
    {
        std::string message = "";
        float duration = 0.0f;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::str1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventHUDMessage*>(event);
                this_event->message = value;
            };

            handlers[SetVarOpts::float1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventHUDMessage*>(event);
                this_event->duration = std::stof(value);
            };
        }

        void turn_on() override
        {
            hud_msg(message.c_str(), 0, static_cast<int>(duration * 1000), 0);
        }

        void turn_off() override
        {
            hud_msg_clear();
        }
    };

    // id 106
    struct EventPlayVideo : Event
    {
        std::string filename;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::str1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventPlayVideo*>(event);
                this_event->filename = value;
            };
        }

        void turn_on() override
        {
            bink_play(filename.c_str());
        }
    };

    // id 107
    struct EventSetLevelHardness : Event
    {
        int hardness;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventSetLevelHardness*>(event);
                this_event->hardness = std::stoi(value);
            };
        }

        void turn_on() override
        {
            level.default_rock_hardness = std::clamp(hardness, 0, 100);
        }
    };

    // id 108
    struct EventSequence : Event
    {
        int next_link_index = 0;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
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

            event_signal_on(link_handle, this->trigger_handle, this->triggered_by_handle);

            ++next_link_index;
        }
    };


    // id 109
    struct EventClearQueued : Event
    {
        void turn_on() override
        {
            for (size_t i = 0; i < this->links.size(); ++i) {
                int link_handle = this->links[i];
                Object* obj = obj_from_handle(link_handle);

                if (obj && obj->type == OT_EVENT) {
                    Event* linked_event = static_cast<Event*>(obj);
                    linked_event->delay_timestamp.invalidate();
                    linked_event->delayed_msg = 0;
                }                
            }
        }
    };

    // id 110
    struct EventRemoveLink : Event
    {
        bool remove_all = 0;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::bool1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventRemoveLink*>(event);
                this_event->remove_all = (value == "true");
            };
        }

        void turn_on() override
        {
            for (size_t i = 0; i < this->links.size(); ++i) {
                int link_handle = this->links[i];
                Object* obj = obj_from_handle(link_handle);

                if (obj && obj->type == OT_EVENT) {
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
    struct EventRouteNode : Event
    {
        RouteNodeBehavior behaviour = RouteNodeBehavior::pass;
        bool fixed = 0;
        bool clear_trigger_info = 0;

        // logic handled in event_alpine.cpp

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];

            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventRouteNode*>(event);
                this_event->behaviour = static_cast<RouteNodeBehavior>(std::stoi(value));
            };

            handlers[SetVarOpts::bool1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventRouteNode*>(event);
                this_event->fixed = (value == "true");
            };

            handlers[SetVarOpts::bool2] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventRouteNode*>(event);
                this_event->clear_trigger_info = (value == "true");
            };
        }
    };

    // id 112
    struct EventAddLink : Event
    {
        int subject_uid = -1;
        bool inbound = false;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];

            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventAddLink*>(event);
                this_event->subject_uid = std::stoi(value);
            };

            handlers[SetVarOpts::bool1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventAddLink*>(event);
                this_event->inbound = (value == "true");
            };
        }

        void turn_on() override
        {
            if (this->links.empty()) {
                return;
            }

            Object* subject_obj = obj_lookup_from_uid(subject_uid);

            if (!subject_obj) {
                return;
            }

            int subject_handle = subject_obj->handle;

            for (size_t i = 0; i < this->links.size(); ++i) {
                int target_link_handle = this->links[i];

                if (inbound) {
                    event_add_link(target_link_handle, subject_handle);
                }
                else {
                    event_add_link(subject_handle, target_link_handle);
                }
            }
        }
    };

    // id 113
    struct EventValidGate : Event
    {
        int check_uid = -1;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventValidGate*>(event);
                this_event->check_uid = std::stoi(value);
            };
        }

        void turn_on() override
        {
            Object* obj = obj_lookup_from_uid(check_uid);

            if (check_uid == -1 ||
                !obj ||
                obj->type == OT_CORPSE ||
                (obj->type == OT_ENTITY && entity_from_handle(obj->handle)->life <= 0)) { // if entity, alive
                return;
            }

            activate_links(this->trigger_handle, this->triggered_by_handle, true);
        }

        void turn_off() override
        {
            Object* obj = obj_lookup_from_uid(check_uid);

            if (check_uid == -1 || !obj || obj->type == OT_CORPSE ||
                (obj->type == OT_ENTITY && entity_from_handle(obj->handle)->life <= 0)) { // if entity, alive
                return;
            }

            activate_links(this->trigger_handle, this->triggered_by_handle, false);
        }
    };

    // id 114
    struct EventGoalMath : Event
    {
        std::string goal;
        GoalMathOperation operation = GoalMathOperation::add;
        int operation_value;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::str1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventGoalMath*>(event);
                this_event->goal = value;
            };

            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventGoalMath*>(event);
                this_event->operation = static_cast<GoalMathOperation>(std::stoi(value));
            };

            handlers[SetVarOpts::int2] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventGoalMath*>(event);
                this_event->operation_value = std::stoi(value);
            };
        }

        void turn_on() override
        {
            if (goal.empty()) {
                return;
            }

            String effective_goal = goal.c_str();

            // Find level goal
            GenericEvent* named_event = event_find_named_event(&effective_goal);

            int* goal_count_ptr = nullptr;
            int* goal_initial_ptr = nullptr;
            if (named_event) {
                goal_count_ptr = reinterpret_cast<int*>(&named_event->event_specific_data[8]); // 11 in original code
                goal_initial_ptr = reinterpret_cast<int*>(&named_event->event_specific_data[0]);
            }

            // Find persistent goal
            PersistentGoalEvent* persist_event = event_lookup_persistent_goal_event(effective_goal);

            if (goal_count_ptr) {
                xlog::warn("Level goal count before operation: {}, initial: {}", *goal_count_ptr, *goal_initial_ptr);
            }

            if (persist_event) {
                xlog::warn("Persistent goal '{}', count: {}, initial count: {}", persist_event->name,
                           persist_event->count, persist_event->initial_count);
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
                xlog::warn("Applying operation '{}' with value '{}' to persistent goal '{}'",
                           static_cast<int>(operation), operation_value, persist_event->name);
                apply_operation(persist_event->count, persist_event->initial_count);
            }

            // Apply to level goal
            if (goal_count_ptr) {
                xlog::warn("Applying operation '{}' with value '{}' to level goal '{}'", static_cast<int>(operation),
                           operation_value, effective_goal);
                apply_operation(*goal_count_ptr, *goal_initial_ptr);
            }

            // Log final values
            if (persist_event) {
                xlog::warn("Persistent goal '{}', new count: {}, initial count: {}", persist_event->name,
                           persist_event->count, persist_event->initial_count);
            }

            if (goal_count_ptr) {
                xlog::warn("Level goal count after operation: {}", *goal_count_ptr);
            }
        }
    };

    // id 115
    struct EventGoalGate : Event
    {
        std::string goal;
        GoalGateTests test_type = GoalGateTests::equal;
        int test_value;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::str1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventGoalGate*>(event);
                this_event->goal = value;
            };

            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventGoalGate*>(event);
                this_event->test_type = static_cast<GoalGateTests>(std::stoi(value));
            };

            handlers[SetVarOpts::int2] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventGoalGate*>(event);
                this_event->test_value = std::stoi(value);
            };
        }

        void turn_on() override
        {
            if (goal.empty()) {
                return;
            }

            String effective_goal = goal.c_str();

            // find persistent goal
            PersistentGoalEvent* persist_event = event_lookup_persistent_goal_event(effective_goal);

            // find level goal
            GenericEvent* named_event = event_find_named_event(&effective_goal);

            int* goal_count_ptr = nullptr;
            int* goal_initial_ptr = nullptr;            

            if (!persist_event && !named_event) {
                xlog::error("Goal_Gate ({}) did not find a level or persistent goal named '{}'",
                    this->uid, effective_goal);
                return;
            }

            if (named_event) {
                goal_count_ptr = reinterpret_cast<int*>(&named_event->event_specific_data[8]); // 11 in original code
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

            xlog::warn("Current goal value for '{}': {}, Initial value: {}", effective_goal, current_value,
                       initial_value);

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
                xlog::warn("Test '{}' passed for EventGoalGate UID {}", static_cast<int>(test_type), this->uid);
                activate_links(this->trigger_handle, this->triggered_by_handle, true);
            }
            else {
                xlog::warn("Test '{}' failed for EventGoalGate UID {}", static_cast<int>(test_type), this->uid);
            }
        }
    };


    // id 116
    struct EventEnvironmentGate : Event
    {
        EnvironmentGateTests environment = EnvironmentGateTests::multi;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];

            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventEnvironmentGate*>(event);
                this_event->environment = static_cast<EnvironmentGateTests>(std::stoi(value));
            };
        }

        void turn_on() override
        {
            bool pass = false;

            switch (environment) {
            case EnvironmentGateTests::multi:
                pass = is_multi;
                break;
            case EnvironmentGateTests::single:
                pass = !is_multi;
                break;
            case EnvironmentGateTests::server:
                pass = (is_server || is_dedicated_server);
                break;
            case EnvironmentGateTests::dedicated:
                pass = is_dedicated_server;
                break;
            case EnvironmentGateTests::client:
                pass = (!is_server && !is_dedicated_server);
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
    struct EventInsideGate : Event
    {
        int check_uid = -1;
        GoalInsideCheckSubject subject_type = GoalInsideCheckSubject::player;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventInsideGate*>(event);
                this_event->check_uid = std::stoi(value);
            };

            handlers[SetVarOpts::int2] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventInsideGate*>(event);
                this_event->subject_type = static_cast<GoalInsideCheckSubject>(std::stoi(value));
            };
        }

        void turn_on() override
        {
            Object* check_obj = obj_lookup_from_uid(check_uid);
            GRoom* check_room = level_room_from_uid(check_uid);

            if (check_uid == -1 || (!check_obj && !check_room)) {
                xlog::warn("UID {} is not a valid trigger/room for Inside_Gate UID {}", check_uid, this->uid);
                return;
            }

            std::vector<int> objects_to_check;

            // Populate object handles to check
            switch (subject_type) {
            case GoalInsideCheckSubject::player:
                if (local_player_entity) {
                    objects_to_check.push_back(local_player_entity->handle);
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
                Object* obj = obj_from_handle(obj_handle);
                bool is_inside = false;

                if (obj) {
                    if (check_obj && check_obj->type == OT_TRIGGER) {
                        Trigger* trigger = static_cast<Trigger*>(check_obj);
                        is_inside = (trigger->type == 0)
                                        ? trigger_inside_bounding_sphere(trigger, obj)
                                        : (trigger->type == 1 && trigger_inside_bounding_box(trigger, obj));
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
                xlog::warn("Test passed for Inside_Gate UID {}", this->uid);
                activate_links(this->trigger_handle, this->triggered_by_handle, true);
            }
        }
    };

    // id 118
    struct EventAnchorMarker : Event {}; // no allocations needed, logic handled in object.cpp

    // id 119
    struct EventForceUnhide : Event
    {
        void turn_on() override
        {
            for (const int link : this->links)
            {
                Object* obj = obj_from_handle(link);
                if (obj) {
                    obj_unhide(obj);
                }
            }
        }

        void turn_off() override
        {
            for (const int link : this->links)
            {
                Object* obj = obj_from_handle(link);
                if (obj) {
                    obj_hide(obj);
                }
            }
        }
    };

    // id 120
    struct EventSetDifficulty : Event
    {
        GameDifficultyLevel difficulty = GameDifficultyLevel::DIFFICULTY_EASY;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventSetDifficulty*>(event);
                this_event->difficulty = static_cast<GameDifficultyLevel>(std::stoi(value));
            };
        }

        void turn_on() override
        {
            game_set_skill_level(difficulty);
        }
    };

    // id 121
    struct EventSetFogFarClip : Event
    {
        int far_clip = 0;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventSetFogFarClip*>(event);
                this_event->far_clip = std::stoi(value);
            };
        }

        void turn_on() override
        {
            level.distance_fog_far_clip = far_clip;
        }
    };

    // id 122
    struct EventAFWhenDead : Event
    {
        bool any_dead = 0;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::bool1] = [](Event* event, const std::string& value) {
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

                Object* obj = obj_from_handle(link_handle);

                // not a valid object
                if (!obj) {
                    return true;
                }

                // if entity, also check if its dead
                if (obj && obj->type == OT_ENTITY) {
                    Entity* entity = entity_from_handle(obj->handle);
                    return entity && entity->life <= 0;
                }

                // only care about entities and clutter
                return obj->type != OT_CLUTTER && obj->type != OT_ENTITY;
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
                Object* obj = obj_from_handle(link_handle);

                if (obj) {
                    ObjectType type = obj->type;
                    switch (type) {
                        case OT_MOVER: {
                            mover_activate_from_trigger(obj->handle, -1, -1);
                            break;
                        }
                        case OT_TRIGGER: {
                            Trigger* trigger = static_cast<Trigger*>(obj);
                            trigger_enable(trigger);
                            break;
                        }
                        case OT_EVENT: {
                            Event* event = static_cast<Event*>(obj);
                            // Note can't use activate because it isn't allocated for stock events
                            event_signal_on(link_handle, -1, -1);
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
    struct EventGametypeGate : Event
    {
        NetGameType gametype = NG_TYPE_DM;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventGametypeGate*>(event);
                this_event->gametype = static_cast<NetGameType>(std::stoi(value));
            };
        }

        void turn_on() override
        {
            if (!is_multi) {
                return;
            }

            bool pass = false;
            switch (gametype) {
            case NG_TYPE_DM:
                pass = (multi_get_game_type() == NG_TYPE_DM);
                break;
            case NG_TYPE_TEAMDM:
                pass = (multi_get_game_type() == NG_TYPE_TEAMDM);
                break;
            case NG_TYPE_CTF:
                pass = (multi_get_game_type() == NG_TYPE_CTF);
                break;
            default:
                xlog::error("Unknown gametype '{}' for EventGametypeGate UID {}",
                    static_cast<int>(gametype), this->uid);
                return;
            }

            if (pass) {
                activate_links(this->trigger_handle, this->triggered_by_handle, true);
            }
        }
    };

    // id 124
    struct EventWhenPickedUp : Event
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
                Object* obj = obj_from_handle(link_handle);

                if (obj) {
                    ObjectType type = obj->type;
                    switch (type) {
                    case OT_MOVER: {
                        mover_activate_from_trigger(obj->handle, -1, -1);
                        break;
                    }
                    case OT_TRIGGER: {
                        Trigger* trigger = static_cast<Trigger*>(obj);
                        trigger_enable(trigger);
                        break;
                    }
                    case OT_EVENT: {
                        Event* event = static_cast<Event*>(obj);
                        // Note can't use activate because it isn't allocated for stock events
                        event_signal_on(link_handle, -1, -1);
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
    struct EventSetSkybox : Event
    {
        int new_sky_room_uid = 0;
        int new_sky_room_anchor_uid = 0;
        float position_scale = 0.0f;
        bool relative_position = false;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventSetSkybox*>(event);
                this_event->new_sky_room_uid = std::stoi(value);
            };

            handlers[SetVarOpts::int2] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventSetSkybox*>(event);
                this_event->new_sky_room_anchor_uid = std::stoi(value);
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
    struct EventSetLife : Event
    {
        int new_life = 0;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventSetLife*>(event);
                this_event->new_life = std::stoi(value);
            };
        }

        void turn_on() override
        {
            for (int link_handle : this->links) {
                Object* obj = obj_from_handle(link_handle);

                if (obj && (obj->type == OT_ENTITY || obj->type == OT_CLUTTER)) {
                    obj->life = new_life;
                }
            }
        }
    };

    // id 127
    struct EventSetDebris : Event
    {
        std::string debris_filename = "";
        std::string debris_sound_set = "";
        int explode_anim_vclip = 0;
        float explode_anim_radius = 0.0f;
        float debris_velocity = 0.0f;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::str1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventSetDebris*>(event);
                this_event->debris_filename = value;
            };

            handlers[SetVarOpts::str2] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventSetDebris*>(event);
                this_event->debris_sound_set = value;
            };

            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventSetDebris*>(event);
                this_event->explode_anim_vclip = std::stoi(value);
            };

            handlers[SetVarOpts::float1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventSetDebris*>(event);
                this_event->explode_anim_radius = std::stof(value);
            };

            handlers[SetVarOpts::float2] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventSetDebris*>(event);
                this_event->debris_velocity = std::stoi(value);
            };
        }

        void turn_on() override
        {
            for (int link_handle : this->links) {
                Object* obj = obj_from_handle(link_handle);

                if (obj) {                
                    if (obj->type == OT_CLUTTER) {
                        Clutter* cl = static_cast<Clutter*>(obj);
                        cl->info->debris_filename = debris_filename.c_str();
                        cl->info->debris_sound_set = debris_sound_set.c_str();
                        cl->info->explode_anim_vclip = explode_anim_vclip;
                        cl->info->explode_anim_radius = explode_anim_radius;
                        cl->info->debris_velocity = debris_velocity;
                    }
                    else if (obj->type == OT_ENTITY) {
                        Entity* ep = static_cast<Entity*>(obj);
                        ep->info->debris_filename = debris_filename.c_str();
                        ep->info->explode_vclip_index = explode_anim_vclip;
                        ep->info->explode_vclip_radius = explode_anim_radius;
                    }
                }

            }
        }
    };

    // id 128
    struct EventSetFogColor : Event
    {
        std::string fog_color = "";

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::str1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventSetFogColor*>(event);
                this_event->fog_color = value;
            };
        }

        void turn_on() override
        {
            try {
                Color color = Color::from_rgb_string(fog_color);
                level.distance_fog_color = color;
            }
            catch (const std::exception& e) {
                xlog::error("Set_Fog_Color ({}) failed to set fog color: {}", this->uid, e.what());
            }
        }
    };

    // id 129
    struct EventSetEntityFlag : Event
    {
        SetEntityFlagOption flag = SetEntityFlagOption::boarded;

        void register_variable_handlers() override
        {
            Event::register_variable_handlers();

            auto& handlers = variable_handler_storage[this];
            handlers[SetVarOpts::int1] = [](Event* event, const std::string& value) {
                auto* this_event = static_cast<EventSetEntityFlag*>(event);
                this_event->flag = static_cast<SetEntityFlagOption>(std::stoi(value));
            };
        }

        void turn_on() override
        {
            for (int link_handle : this->links) {
                Object* obj = obj_from_handle(link_handle);

                if (obj && obj->type == OT_ENTITY) {
                    Entity* ep = static_cast<Entity*>(obj);
                    xlog::warn("attem {},,, {}", static_cast<int>(flag), ep->info->flags);
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
                        ep->entity_flags2 |= 0x2;
                        break;
                    case SetEntityFlagOption::perfect_aim:
                        ep->entity_flags2 |= 0x4;
                        break;
                    case SetEntityFlagOption::permanent_corpse:
                        ep->entity_flags2 |= 0x8;
                        break;
                    case SetEntityFlagOption::always_relevant:
                        ep->entity_flags2 |= 0x400;
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
                    xlog::warn("attem {},,, {}", static_cast<int>(flag), ep->info->flags);
                }
            }
        }

        void turn_off() override
        {
            for (int link_handle : this->links) {
                Object* obj = obj_from_handle(link_handle);

                if (obj && obj->type == OT_ENTITY) {
                    Entity* ep = static_cast<Entity*>(obj);
                    xlog::warn("attem {},,, {}", static_cast<int>(flag), ep->info->flags);
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
                        ep->entity_flags2 &= ~0x2;
                        break;
                    case SetEntityFlagOption::perfect_aim:
                        ep->entity_flags2 &= ~0x4;
                        break;
                    case SetEntityFlagOption::permanent_corpse:
                        ep->entity_flags2 &= ~0x8;
                        break;
                    case SetEntityFlagOption::always_relevant:
                        ep->entity_flags2 &= ~0x400;
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
                    xlog::warn("attem {},,, {}", static_cast<int>(flag), ep->info->flags);
                }
            }
        }
    };

} // namespace rf

