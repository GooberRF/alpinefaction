#include <algorithm>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <common/utils/string-utils.h>
#include <xlog/xlog.h>
#include "asg_internal.h"
#include "../misc/destruction.h"
#include "../multi/vehicles/physics/ragdoll.h"
#include "../rf/ai.h"
#include "../rf/bmpman.h"
#include "../rf/character.h"
#include "../rf/clutter.h"
#include "../rf/corpse.h"
#include "../rf/entity.h"
#include "../rf/event.h"
#include "../rf/geometry.h"
#include "../rf/gr/gr_light.h"
#include "../rf/item.h"
#include "../rf/level.h"
#include "../rf/misc.h"
#include "../rf/mover.h"
#include "../rf/particle_emitter.h"
#include "../rf/physics.h"
#include "../rf/player/player.h"
#include "../rf/sound/sound.h"
#include "../rf/trigger.h"
#include "../rf/vmesh.h"
#include "../rf/weapon.h"
#include "../object/object.h"

namespace
{
    // Moves obj_next_uid below every saved runtime uid and renumbers the live objects that hold one
    void reserve_runtime_uids(const asg::LevelState& lvl)
    {
        std::unordered_set<int32_t> saved;
        const auto add = [&](const auto& records) {
            for (const auto& r : records) {
                if (r.base.origin == 1) {
                    saved.insert(r.base.uid);
                }
            }
        };
        add(lvl.entities.entities);
        if (lvl.corpses) {
            add(*lvl.corpses);
        }
        if (lvl.items) {
            add(*lvl.items);
        }
        if (lvl.clutter) {
            add(*lvl.clutter);
        }
        if (lvl.triggers) {
            add(*lvl.triggers);
        }
        if (lvl.movers) {
            add(*lvl.movers);
        }
        if (lvl.weapons) {
            add(*lvl.weapons);
        }
        if (lvl.events) {
            for (const auto& r : *lvl.events) {
                if (r.origin == 1) {
                    saved.insert(r.uid);
                }
            }
        }
        if (lvl.alpine_objects) {
            asg::add_runtime_alpine_uids(*lvl.alpine_objects, saved);
        }
        if (saved.empty()) {
            return;
        }
        const int32_t lowest = *std::min_element(saved.begin(), saved.end());
        if (lowest <= rf::obj_next_uid) {
            rf::obj_next_uid = lowest - 1;
        }
        for (rf::Object* o = rf::object_list.next_obj; o != &rf::object_list; o = o->next_obj) {
            if (saved.contains(o->uid)) {
                xlog::debug("[ASG] Renumbering object {} (type {}), whose uid the save uses", o->uid,
                            static_cast<int>(o->type));
                o->uid = rf::obj_next_uid--;
            }
        }
    }

    // Where restore_player put the player, for replay_rf2_geomods: the carves come after it
    struct PlayerPlacement
    {
        int entity_handle = -1;
        rf::Vector3 pos;
        rf::Matrix3 orient;
        bool stick_to_ground = false;
    };
    PlayerPlacement g_player_placement;

    // A live object of the type has the uid
    bool has_object_with_uid(int32_t uid, rf::ObjectType type)
    {
        const rf::Object* o = rf::obj_lookup_from_uid(uid);
        return o && o->type == type;
    }

    rf::PCollisionSphere collision_sphere(const asg::CollisionSphere& s)
    {
        rf::PCollisionSphere sphere;
        sphere.center = s.center;
        sphere.radius = s.radius;
        sphere.spring_const = s.spring_const;
        sphere.spring_length = s.spring_length;
        return sphere;
    }

    void set_position(rf::Object& obj, const asg::ObjectBase& b)
    {
        asg::set_position(obj, b.pos, b.orient);
    }

    // As obj_deserialize (0x0048AE50), at full precision
    void restore_base(rf::Object& obj, const asg::ObjectBase& b, asg::RefResolver& refs)
    {
        obj.uid = b.uid;
        obj.life = b.life;
        obj.armor = b.armor;
        set_position(obj, b);
        obj.friendliness = static_cast<rf::ObjFriendliness>(b.friendliness);
        if ((obj.obj_flags & rf::OF_HIDDEN) && !(b.obj_flags & rf::OF_HIDDEN)) {
            rf::obj_unhide(&obj);
        }
        obj.obj_flags = static_cast<rf::ObjectFlags>(b.obj_flags);
        refs.add(b.parent_uid, &obj.parent_handle);
        refs.add(b.host_uid, &obj.host_handle);
        obj.host_tag_handle = b.host_tag_handle;
        // A recreated entity or corpse has the default skin
        obj_set_skin(obj, b.skin);

        rf::ObjectCreateInfo oci;
        oci.drag = obj.p_data.drag;
        oci.material = obj.material;
        oci.mass = obj.p_data.mass;
        oci.body_inv = obj.p_data.body_inv;
        oci.pos = b.pos;
        oci.orient = b.orient;
        oci.vel = b.vel;
        if (obj.p_data.mass > 0.0f) {
            oci.rotvel = b.ang_momentum / obj.p_data.mass;
        }
        oci.radius = obj.radius;
        oci.physics_flags = static_cast<int>(b.physics_flags);
        rf::physics_init_object(&obj.p_data, &oci, true);
        obj.p_data.ang_momentum = b.ang_momentum;
        obj.p_data.vel = b.vel;
        obj.p_data.flags = static_cast<int>(b.physics_flags);
        obj.obj_flags |= rf::OF_WAS_TELEPORTED | rf::OF_UNK_2000000;
    }

    int lookup_weapon(const std::string& name)
    {
        if (name.empty()) {
            return -1;
        }
        const int type = rf::weapon_lookup_type(name.c_str());
        if (type < 0) {
            xlog::warn("[ASG] Unknown weapon '{}'", name);
        }
        return type;
    }

    void restore_weapons(rf::Entity& e, const asg::EntityRecord& r)
    {
        auto& ai = e.ai;
        std::fill(std::begin(ai.ammo), std::end(ai.ammo), 0);
        std::fill(std::begin(ai.clip_ammo), std::end(ai.clip_ammo), 0);
        std::fill(std::begin(ai.has_weapon), std::end(ai.has_weapon), false);
        e.weapon_custom_mode_bitmap = 0;
        e.weapon_silencer_bitfield = 0;
        for (const auto& w : r.weapons) {
            const int type = lookup_weapon(w.weapon);
            if (type < 0) {
                continue;
            }
            ai.clip_ammo[type] = w.clip_ammo;
            ai.has_weapon[type] = w.possessed;
            const int ammo_type = rf::weapon_types[type].ammo_type;
            if (ammo_type >= 0 && ammo_type < static_cast<int>(std::size(ai.ammo))) {
                ai.ammo[ammo_type] = w.ammo;
            }
            if (type < asg::num_weapon_bits) {
                e.weapon_custom_mode_bitmap |= w.custom_mode ? 1 << type : 0;
                e.weapon_silencer_bitfield |= w.silencer ? 1 << type : 0;
            }
        }
        ai.current_primary_weapon = lookup_weapon(r.primary_weapon);
        ai.current_secondary_weapon = lookup_weapon(r.secondary_weapon);
    }

    rf::GPathNode* path_node(int index)
    {
        if (index < 0 || !rf::level.geometry || index >= rf::level.geometry->nodes.nodes.size()) {
            return nullptr;
        }
        return rf::level.geometry->nodes.nodes[index];
    }

    void restore_path(rf::Entity& e, const asg::EntityRecord& r)
    {
        auto& path = e.ai.current_path;
        path.previous_goal = r.path_previous_goal;
        path.current_goal = r.path_current_goal;
        path.end_node.pos = r.path_end_pos;
        path.end_node.use_pos = r.path_end_pos;
        path.adjacent_node1 = path_node(r.path_adjacent_node1_index);
        path.adjacent_node2 = path_node(r.path_adjacent_node2_index);
        // As stock, the node list is left empty; the modes that need a path rebuild one below
        path.num_nodes = 0;
        for (int i = 0; i < 4; ++i) {
            path.nodes[i] = path_node(r.path_node_indices[i]);
        }
        path.waypoint_list_index = r.path_waypoint_list_index;
        path.goal_waypoint_index = r.path_goal_waypoint_index;
        path.direction = r.path_direction;
        path.flags = r.path_flags;
        path.turn_towards_pos = r.path_turn_towards_pos;
        path.follow_style = r.path_follow_style;
        path.start_pos = r.path_start_pos;
        path.goal_pos = r.path_goal_pos;
    }

    // As entity_deserialize (0x0042BA50)
    void restore_entity(rf::Entity& e, const asg::EntityRecord& r, asg::RefResolver& refs)
    {
        restore_base(e, r.base, refs);
        auto& ai = e.ai;
        if (r.base.host_uid == asg::uid_none) {
            ai.on_turret_uid = -1;
        }
        restore_weapons(e, r);

        ai.hate_list.clear();
        for (size_t i = 0; i < r.hate_list.size(); ++i) {
            ai.hate_list.add(-1);
        }
        for (size_t i = 0; i < r.hate_list.size(); ++i) {
            refs.add(r.hate_list[i], &ai.hate_list[i]);
        }
        refs.add(r.target_uid, &ai.target_handle);
        const bool standing_in_look_at = rf::obj_is_player(&e) && (e.p_data.flags & rf::PF_UNK_400000);
        refs.add(r.look_at_uid, standing_in_look_at ? &e.control_data.standing_on_obj_handle : &ai.look_at_handle);
        refs.add(r.shoot_at_uid, &ai.shoot_at_handle);
        restore_path(e, r);
        if (r.move_mode >= rf::MOVE_MODE_NONE && r.move_mode <= rf::MOVE_MODE_JOHNS_DESCENT_FLYING) {
            e.move_mode = rf::movemode_get_mode(r.move_mode);
        }
        ai.mode = static_cast<rf::AiMode>(r.ai_mode);
        ai.submode = static_cast<rf::AiSubmode>(r.ai_submode);
        ai.mode_parm_0 = r.ai_mode_parm_0;
        ai.mode_parm_1 = r.ai_mode_parm_1;
        ai.ci.rot = r.ci_rot;
        ai.ci.move = r.ci_move;
        ai.ci.field_18 = r.ci_time_relative_mouse_delta_disabled;
        refs.add(r.corpse_carry_uid, &ai.corpse_carry_handle);
        ai.healing_left = r.ai_healing_left;
        ai.steering_vector = r.ai_steering_vector;
        ai.ai_flags = static_cast<int>(r.ai_flags);
        ai.change_weapon_state_timestamp.set(0);
        e.eye_pos = r.eye_pos;
        e.eye_orient = r.eye_orient;
        e.entity_flags = r.entity_flags;
        e.entity_flags2 = r.entity_flags2;
        e.driller_geomod_count = r.driller_geomod_count;
        e.control_data.phb = r.control_phb;
        e.control_data.eye_phb = r.control_eye_phb;
        e.control_data.local_vel = r.control_local_vel;
        if (r.on_fire && !e.entity_fire_handle) {
            e.entity_fire_handle = rf::entity_fire_create(e.handle, -1);
        }
        e.current_climb_region = r.climb_region_index >= 0 && r.climb_region_index < rf::level.ladders.size()
            ? rf::level.ladders[r.climb_region_index]
            : nullptr;
        e.current_speed = static_cast<rf::EntitySpeed>(r.current_speed);
        e.set_speed(e.current_speed);
        e.driller_rot_angle = r.driller_rot_angle;
        e.driller_rot_speed = r.driller_rot_speed;
        if (!r.item_drop_override.empty()) {
            const int item_type = rf::item_lookup_type(r.item_drop_override.c_str());
            if (item_type >= 0) {
                e.drop_item_class = item_type;
            }
        }
        entity_set_anim_state(e, {
            .current_state_anim = r.current_state_anim,
            .next_state_anim = r.next_state_anim,
            .total_transition_time = r.total_transition_time,
            .elapsed_transition_time = r.elapsed_transition_time,
            .custom_state_anim = r.state_anim_22_index,
            .custom_action_anim = r.last_custom_anim_index,
        });

        const bool on_path = ai.submode == rf::AI_SUBMODE_ON_PATH;
        if ((ai.mode == rf::AI_MODE_WAITING || ai.mode == rf::AI_MODE_SET_ALARM) && on_path) {
            rf::ai_path_create_to_pos(e.handle, &ai.current_path.goal_pos);
        }
        if (ai.mode == rf::AI_MODE_WAYPOINTS && on_path) {
            rf::ai_path_create_from_waypoints(e.handle);
            e.entity_flags2 |= rf::EF2_UNK_4000;
        }
        // Stock fix-up for one entity in l7s2 (0x0042C04B)
        if (e.uid == 0x1358 && string_iequals(rf::level.filename.c_str(), "l7s2.rfl")
            && ai.mode == rf::AI_MODE_WAITING && on_path) {
            e.entity_flags2 |= rf::EF2_UNK_4000;
        }
        if ((e.info->flags & rf::EIF_NANO_SHIELD) && !rf::entity_has_nano_shield(&e)) {
            rf::entity_end_nano_shield(&e, false);
        }
        if (ai.mode == rf::AI_MODE_BERSERK) {
            rf::ai_go_berserk(&ai, rf::frand_range(5.0f, 10.0f), true);
        }
        e.last_fired_timestamp.set(-1000);
    }

    // As entity_deserialize_all (0x0042C100)
    void restore_entities(const asg::EntityState& state, bool transition, asg::RefResolver& refs)
    {
        const auto records = asg::index_by_uid(state.entities);
        const std::unordered_set<int32_t> dead{state.dead_uids.begin(), state.dead_uids.end()};
        std::unordered_set<int32_t> matched;
        for (rf::Entity* e = rf::entity_list.next; e != &rf::entity_list; e = e->next) {
            // The player entity is restored with the player
            if (e->uid == asg::player_entity_uid || ((e->obj_flags & rf::OF_IN_LEVEL_TRANSITION) && transition)
                || asg::is_ambient_entity(*e)) {
                continue;
            }
            const auto it = records.find(e->uid);
            if (it != records.end()) {
                restore_entity(*e, *it->second, refs);
                matched.insert(e->uid);
                const bool dying = rf::entity_is_dying(e) || (e->obj_flags & rf::OF_DELAYED_DELETE);
                if (dying && (e->entity_flags & rf::EF_ENDGAME_ON_DEATH)) {
                    rf::endgame_launch(e->name.c_str());
                }
            }
            else if (dead.contains(e->uid)) {
                rf::obj_hide(e);
            }
            else {
                rf::obj_flag_dead(e);
            }
        }
        for (const auto& r : state.entities) {
            if (r.base.uid == asg::player_entity_uid || matched.contains(r.base.uid)
                || has_object_with_uid(r.base.uid, rf::OT_ENTITY)) {
                continue;
            }
            if (r.base.origin != 1 && !(r.entity_flags2 & rf::EF2_SAVE_RECREATE)) {
                xlog::warn("[ASG] Saved entity {} ({}) is not in the level", r.base.uid, r.base.cls);
                continue;
            }
            const int type = rf::entity_lookup_type(r.base.cls.c_str());
            if (type < 0) {
                xlog::warn("[ASG] Cannot recreate entity {}: unknown class '{}'", r.base.uid, r.base.cls);
                continue;
            }
            if (rf::Entity* e = rf::entity_create(type, "", -1, r.base.pos, r.base.orient, 0, -1)) {
                restore_entity(*e, r, refs);
            }
        }
    }

    void restore_item(rf::Item& it, const asg::ItemRecord& r, asg::RefResolver& refs)
    {
        restore_base(it, r.base, refs);
        it.respawn_time_ms = r.respawn_time_ms;
        asg::restore_timestamp(it.respawn_next, r.respawn_timer);
        it.alpha = r.alpha;
        it.create_time = r.create_time;
        it.item_flags = static_cast<int>(r.item_flags);
    }

    // As item_deserialize_all (0x00459D10): missing items are recreated
    void restore_items(const std::vector<asg::ItemRecord>& records, asg::RefResolver& refs)
    {
        const auto by_uid = asg::index_by_uid(records);
        std::unordered_set<int32_t> matched;
        for (rf::Item* it = rf::item_list.next; it != &rf::item_list; it = it->next) {
            const auto found = by_uid.find(it->uid);
            if (found != by_uid.end()) {
                restore_item(*it, *found->second, refs);
                matched.insert(it->uid);
            }
            else if (!(it->obj_flags & rf::OF_IN_LEVEL_TRANSITION)) {
                rf::obj_flag_dead(it);
            }
        }
        for (const auto& r : records) {
            if (matched.contains(r.base.uid) || has_object_with_uid(r.base.uid, rf::OT_ITEM)) {
                continue;
            }
            const int type = rf::item_lookup_type(r.base.cls.c_str());
            if (type < 0) {
                xlog::warn("[ASG] Cannot recreate item {}: unknown class '{}'", r.base.uid, r.base.cls);
                continue;
            }
            rf::Matrix3 orient = r.base.orient;
            if (rf::Item* it = rf::item_create(type, "", rf::item_info[type].count, -1, &r.base.pos, &orient, -1,
                                               false, false)) {
                restore_item(*it, r, refs);
            }
        }
    }

    // Stock does not restore the flags of clutter and triggers; the saved liquid bit is what
    // obj_update_liquid_status (0x00486C30) works out for the saved position
    void restore_liquid_flag(rf::Object& obj, const asg::ObjectBase& b)
    {
        const uint32_t liquid = b.obj_flags & rf::OF_IN_LIQUID;
        obj.obj_flags =
            static_cast<rf::ObjectFlags>((static_cast<uint32_t>(obj.obj_flags) & ~rf::OF_IN_LIQUID) | liquid);
    }

    void restore_clutter_state(rf::Clutter& c, const asg::ClutterRecord& r, asg::RefResolver& refs)
    {
        set_position(c, r.base);
        restore_liquid_flag(c, r.base);
        c.set_room(nullptr);
        c.life = r.base.life;
        // Stock leaves clutter at rest; one still moving when saved carries on
        c.p_data.vel = r.base.vel;
        c.p_data.ang_momentum = r.base.ang_momentum;
        refs.add(r.base.parent_uid, &c.parent_handle);
        asg::restore_timestamp(c.delayed_kill_timestamp, r.delayed_kill_timer);
        c.delayed_kill_sound = r.delayed_kill_sound;
        asg::restore_timestamp(c.emitter_kill_timestamp, r.emitter_kill_timer);
        asg::restore_timestamp(c.corpse_create_timestamp, r.corpse_create_timer);
        c.current_skin_index = r.skin_index;
        const bool hidden = (c.obj_flags & rf::OF_HIDDEN) != 0;
        if (r.hidden && !hidden) {
            rf::obj_hide(&c);
        }
        else if (!r.hidden && hidden) {
            rf::obj_unhide(&c);
        }
    }

    // As clutter_deserialize_all (0x00411020). Stock never recreates clutter; clutter made at runtime (such as the
    // corpse of destroyed clutter) is recreated here.
    void restore_clutter(const std::vector<asg::ClutterRecord>& records, bool transition, asg::RefResolver& refs)
    {
        const auto by_uid = asg::index_by_uid(records);
        std::unordered_set<int32_t> matched;
        for (rf::Clutter* c = rf::clutter_list.next; c != &rf::clutter_list; c = c->next) {
            if (((c->obj_flags & rf::OF_IN_LEVEL_TRANSITION) && transition) || asg::is_alpine_clutter(*c)) {
                continue;
            }
            const auto found = by_uid.find(c->uid);
            if (found == by_uid.end()) {
                rf::obj_flag_dead(c);
                continue;
            }
            matched.insert(c->uid);
            restore_clutter_state(*c, *found->second, refs);
        }
        for (const auto& r : records) {
            if (matched.contains(r.base.uid) || has_object_with_uid(r.base.uid, rf::OT_CLUTTER)) {
                continue;
            }
            if (r.base.origin != 1) {
                xlog::warn("[ASG] Saved clutter {} ({}) is not in the level", r.base.uid, r.base.cls);
                continue;
            }
            const int type = rf::clutter_lookup_type(r.base.cls.c_str());
            if (type < 0) {
                xlog::warn("[ASG] Cannot recreate clutter {}: unknown class '{}'", r.base.uid, r.base.cls);
                continue;
            }
            if (rf::Clutter* c = rf::clutter_create(type, "", -1, &r.base.pos, &r.base.orient, false)) {
                c->uid = r.base.uid;
                restore_clutter_state(*c, r, refs);
            }
        }
    }

    // As trigger_deserialize_all (0x004C0EE0). Its L19S3 fix-up for a missing trigger 0x2F97 only moves two
    // movers, whose state the MOVR chunk stores.
    void restore_triggers(const std::vector<asg::TriggerRecord>& records, asg::RefResolver& refs)
    {
        const auto by_uid = asg::index_by_uid(records);
        for (rf::Trigger* t = rf::trigger_list.next; t != &rf::trigger_list; t = t->next) {
            const auto found = by_uid.find(t->uid);
            if (found == by_uid.end()) {
                rf::obj_flag_dead(t);
                continue;
            }
            const auto& r = *found->second;
            set_position(*t, r.base);
            restore_liquid_flag(*t, r.base);
            t->count = r.count;
            t->time_last_activated = r.time_last_activated;
            t->trigger_flags = static_cast<int>(r.trigger_flags);
            refs.add(r.activator_uid, &t->activator_handle);
            asg::restore_timestamp(t->button_active_timestamp, r.button_active_timer);
            asg::restore_timestamp(t->inside_timestamp, r.inside_timer);
            asg::restore_timestamp(t->next_check, r.reset_timer);
        }
    }

    // As the stock load (0x004B4AA0): craters are carved before the objects come back, then the level solid gets its
    // saved bbox back
    void restore_craters(const std::vector<asg::CraterRecord>& craters)
    {
        int n = static_cast<int>(craters.size());
        if (n > rf::max_geomod_craters) {
            // geomod_create cannot make more, so only an edited file has them
            xlog::warn("[ASG] Ignoring {} craters past the engine's {}", n - rf::max_geomod_craters,
                       rf::max_geomod_craters);
            n = rf::max_geomod_craters;
        }
        for (int i = 0; i < n; ++i) {
            const auto& c = craters[i];
            auto& out = rf::g_geomod_crater_records[i];
            out.shape_index = c.shape_index;
            out.flags = c.flags;
            out.room_index = c.room_index;
            std::copy(c.pos.begin(), c.pos.end(), out.pos);
            std::copy(c.hit_normal.begin(), c.hit_normal.end(), out.hit_normal);
            std::copy(c.orient.begin(), c.orient.end(), out.orient);
            out.scale = c.scale;
        }
        rf::g_num_geomods_this_level = n;
        rf::levelmod_load_state();
    }

    void restore_rf2_geomods(const std::vector<asg::Rf2GeomodRecord>& records)
    {
        std::vector<rf::GeomodParams> params;
        for (const auto& r : records) {
            auto& p = params.emplace_back();
            p.shape_index = r.shape_index;
            p.room_index = r.room_index;
            p.pos = r.pos;
            p.orient = r.orient;
            p.flags = r.flags;
            p.scale = r.scale;
            p.hit_normal = r.hit_normal;
            p.field_4C = r.field_4c;
            p.field_58 = r.field_58;
        }
        destruction_queue_rf2_geomod_replay(std::move(params));
    }

    // As the stock glass load (0x00494980), which kills each detail room again
    void restore_killed_rooms(const std::vector<int32_t>& uids)
    {
        rf::num_killed_rooms = 0;
        if (!rf::level.geometry) {
            return;
        }
        for (int32_t uid : uids) {
            rf::GRoom* room = rf::level.geometry->find_room_by_uid(uid);
            if (room && room->is_detail) {
                rf::room_kill(room);
            }
        }
    }

    void restore_liquid_depths(const std::vector<asg::LiquidDepthRecord>& records)
    {
        for (const auto& r : records) {
            rf::GRoom* room = rf::level_room_from_uid(r.room_uid);
            if (!room) {
                xlog::warn("[ASG] Saved liquid room {} is not in the level", r.room_uid);
                continue;
            }
            if (room->liquid_depth != r.depth) {
                room->liquid_depth = r.depth;
                room->reset_liquid(rf::level.geometry);
            }
            if (r.has_pending) {
                rf::add_liquid_depth_update(room, r.target_depth, r.remaining_duration);
            }
        }
    }

    void restore_breakable_rooms(const std::vector<asg::RoomLifeRecord>& records)
    {
        if (!rf::level.geometry) {
            return;
        }
        for (const auto& r : records) {
            rf::GRoom* room = rf::level.geometry->find_room_by_uid(r.room_uid);
            if (room && room->is_detail) {
                room->life = r.life;
            }
        }
    }

    void restore_geometry(const asg::LevelState& lvl)
    {
        if (lvl.craters) {
            restore_craters(*lvl.craters);
        }
        if (lvl.rooms) {
            if (rf::level.geometry) {
                rf::level.geometry->bbox_min = lvl.rooms->world_bbox_min;
                rf::level.geometry->bbox_max = lvl.rooms->world_bbox_max;
            }
            restore_killed_rooms(lvl.rooms->killed_room_uids);
            restore_liquid_depths(lvl.rooms->liquid_depths);
        }
        if (lvl.rf2_geomods) {
            restore_rf2_geomods(*lvl.rf2_geomods);
        }
        if (lvl.breakable_rooms) {
            restore_breakable_rooms(*lvl.breakable_rooms);
        }
    }

    // As the stock mover load (0x0046B4A0), which only updates movers. The L19S3 fix-up for a missing trigger 0x2F97
    // in the stock trigger load moves two movers, whose state is restored here.
    void restore_movers(const std::vector<asg::MoverRecord>& records, asg::RefResolver& refs)
    {
        const auto by_uid = asg::index_by_uid(records);
        for (rf::Mover* m = rf::mover_list.next; m != &rf::mover_list; m = m->next) {
            const auto found = by_uid.find(m->uid);
            if (found == by_uid.end()) {
                xlog::warn("[ASG] Mover {} is not in the save", m->uid);
                continue;
            }
            const auto& r = *found->second;
            restore_base(*m, r.base, refs);
            m->rot_cur_pos = r.rot_cur_pos;
            m->start_at_keyframe = r.start_at_keyframe;
            m->stop_at_keyframe = r.stop_at_keyframe;
            m->stop_completely_at_keyframe = r.stop_completely_at_keyframe;
            m->mover_flags = static_cast<rf::MoverFlags>(r.mover_flags);
            m->travel_time_seconds = r.travel_time_seconds;
            m->rotation_travel_time_seconds_unk = r.rotation_travel_time_seconds;
            asg::restore_timestamp(m->wait_timestamp, r.wait_timer);
            refs.add(r.trigger_uid, &m->trigger_handle);
            m->dist_travelled = r.dist_travelled;
            m->cur_vel = r.cur_vel;
        }
        rf::movers_loaded_from_save = true;
    }

    void restore_weapon(rf::Weapon& w, const asg::WeaponRecord& r, asg::RefResolver& refs)
    {
        restore_base(w, r.base, refs);
        w.lifeleft_seconds = r.life_left_seconds;
        w.weapon_flags = static_cast<int>(r.weapon_flags);
        refs.add(r.sticky_host_uid, &w.sticky_host_handle);
        w.sticky_host_pos_offset = r.sticky_host_pos_offset;
        w.sticky_host_orient = r.sticky_host_orient;
        w.friendliness = static_cast<rf::ObjFriendliness>(r.weap_friendliness);
        refs.add(r.target_uid, &w.target_handle);
        w.pierce_power_left = r.pierce_power_left;
        w.thrust_left = r.thrust_left;
        w.firing_pos = r.firing_pos;
    }

    // As the stock weapon load (0x004C8F30), which creates every saved projectile
    void restore_weapons(const std::vector<asg::WeaponRecord>& records, asg::RefResolver& refs)
    {
        const auto by_uid = asg::index_by_uid(records);
        std::unordered_set<int32_t> matched;
        for (rf::Weapon* w = rf::weapon_list.next; w != &rf::weapon_list; w = w->next) {
            if (w->obj_flags & rf::OF_IN_LEVEL_TRANSITION) {
                continue;
            }
            const auto found = by_uid.find(w->uid);
            if (found == by_uid.end()) {
                rf::obj_flag_dead(w);
                continue;
            }
            restore_weapon(*w, *found->second, refs);
            matched.insert(w->uid);
        }
        for (const auto& r : records) {
            if (matched.contains(r.base.uid) || has_object_with_uid(r.base.uid, rf::OT_WEAPON)) {
                continue;
            }
            const int type = lookup_weapon(r.base.cls);
            if (type < 0) {
                continue;
            }
            if (rf::Weapon* w = rf::weapon_create(type, -1, &r.base.pos, &r.base.orient, false, 0)) {
                restore_weapon(*w, r, refs);
            }
        }
    }

    void restore_corpse(rf::Corpse& c, const asg::CorpseRecord& r, asg::RefResolver& refs)
    {
        restore_base(c, r.base, refs);
        c.create_time = r.create_time;
        c.lifetime_seconds = r.lifetime_seconds;
        c.corpse_flags = static_cast<int>(r.corpse_flags);
        asg::restore_timestamp(c.emitter_kill_timestamp, r.emitter_kill_timer);
        c.body_temp = r.body_temp;
        c.corpse_state_vmesh_anim_index = r.state_anim;
        // A recreated corpse with none keeps the action its mesh load picked
        if (r.action_anim >= 0) {
            c.corpse_action_vmesh_anim_index = r.action_anim;
        }
        c.corpse_drop_vmesh_anim_index = r.drop_anim;
        c.corpse_carry_vmesh_anim_index = r.carry_anim;
        c.corpse_pose = r.corpse_pose;
        refs.add(r.item_uid, &c.item_handle);
    }

    // A ragdoll comes back frozen in its saved pose
    void restore_ragdoll(rf::Corpse& c, const asg::CorpseRecord& r)
    {
        if (r.ragdoll_bodies.empty()) {
            return;
        }
        std::vector<rf::Matrix43> bodies;
        std::vector<rf::Matrix43> bones;
        for (const auto& b : r.ragdoll_bodies) {
            bodies.push_back({b.orient, b.pos});
        }
        for (const auto& b : r.ragdoll_bones) {
            bones.push_back({b.orient, b.pos});
        }
        if (!ragdoll_set_pose(c, bodies, bones)) {
            xlog::warn("[ASG] Corpse {} is not a ragdoll again", r.base.uid);
        }
    }

    // The saved spheres: at load the bones still hold the bind pose (0x0051B2E0), which gives standing spheres
    void restore_collision_spheres(rf::Corpse& c, const asg::CorpseRecord& r)
    {
        c.p_data.radius = r.radius;
        c.p_data.cspheres.clear();
        for (const auto& s : r.cspheres) {
            c.p_data.cspheres.add(collision_sphere(s));
        }
    }

    // As the stock corpse load does for a corpse it creates (0x00417AC6): a body mesh plays its action animation to
    // the end, which poses it
    void load_corpse_meshes(rf::Corpse& c, const asg::CorpseRecord& r)
    {
        c.helmet_v3d_handle = r.helmet_name.empty() ? nullptr : rf::vmesh_load(r.helmet_name.c_str(), 1, -1);
        if (r.mesh_name.empty()) {
            c.vmesh = nullptr;
            return;
        }
        if (r.mesh_name.find("v3d") != std::string::npos) {
            c.vmesh = rf::vmesh_load(r.mesh_name.c_str(), 1, -1);
            return;
        }
        c.vmesh = rf::vmesh_create_character(r.mesh_name.c_str(), 0, 0);
        if (!c.vmesh) {
            return;
        }
        rf::vmesh_reset_actions(c.vmesh);
        if (c.corpse_action_vmesh_anim_index < 0) {
            // As stock (0x00417B2E): a corpse with no action plays the character's last action animation
            const auto* ci = static_cast<const rf::CharacterInstance*>(c.vmesh->instance);
            const rf::Character* character = ci ? ci->base_character : nullptr;
            for (int i = character ? character->num_anims - 1 : -1; i >= 0; --i) {
                if (!character->anim_is_state[i]) {
                    c.corpse_action_vmesh_anim_index = i;
                    break;
                }
            }
        }
        if (c.corpse_action_vmesh_anim_index >= 0) {
            const int anim = c.corpse_action_vmesh_anim_index;
            rf::vmesh_play_action_by_index(c.vmesh, anim, 1.0f, 1);
            rf::vmesh_process(c.vmesh, rf::vmesh_anim_length(c.vmesh, anim), 0, nullptr, nullptr, 1);
            rf::vmesh_process(c.vmesh, 0.3f, 0, nullptr, nullptr, 1);
            rf::corpse_update_collision_spheres(&c);
        }
    }

    // As the stock load (0x00417860): it creates the corpse object itself rather than through corpse_create, which
    // needs the entity that died
    rf::Corpse* create_corpse(const asg::CorpseRecord& r)
    {
        const int entity_type = rf::entity_lookup_type(r.entity_cls.c_str());
        if (entity_type < 0) {
            xlog::warn("[ASG] Cannot recreate corpse {}: unknown entity class '{}'", r.base.uid, r.entity_cls);
            return nullptr;
        }
        // As Alpine's fix to the stock load (0x004179E2): an entity of the class loads its action animations
        if (rf::Entity* ep = rf::entity_create(entity_type, "", -1, r.base.pos, r.base.orient, 0, -1)) {
            rf::obj_flag_dead(ep);
        }
        rf::ObjectCreateInfo oci;
        oci.material = 3;
        oci.mass = r.mass;
        oci.pos = r.base.pos;
        oci.orient = r.base.orient;
        oci.radius = r.radius;
        oci.physics_flags = rf::PF_GRAVITY | rf::PF_UNK_02 | rf::PF_COLLIDE_WORLD | rf::PF_COLLIDE_OBJECTS;
        for (const auto& s : r.cspheres) {
            oci.spheres.add(collision_sphere(s));
        }
        auto* c = static_cast<rf::Corpse*>(rf::obj_create(rf::OT_CORPSE, -1, -1, &oci, 0, nullptr));
        if (!c) {
            return nullptr;
        }
        c->entity_type = entity_type;
        c->corpse_action_vmesh_anim_index = r.action_anim;
        c->shadow_info.vmesh_handle = nullptr;
        c->fire_info = nullptr;
        c->body_drop_sound_handle = -1;
        load_corpse_meshes(*c, r);
        rf::Color ambient;
        rf::obj_get_ambient_color(&ambient, c);
        c->ambient_color = ambient;
        c->prev = rf::corpse_list.prev;
        c->next = &rf::corpse_list;
        rf::corpse_list.prev->next = c;
        rf::corpse_list.prev = c;
        ++rf::corpse_count;
        return c;
    }

    void restore_corpses(const std::vector<asg::CorpseRecord>& records, asg::RefResolver& refs)
    {
        const auto by_uid = asg::index_by_uid(records);
        std::unordered_set<int32_t> matched;
        for (rf::Corpse* c = rf::corpse_list.next; c != &rf::corpse_list; c = c->next) {
            if (c->obj_flags & rf::OF_IN_LEVEL_TRANSITION) {
                continue;
            }
            const auto found = by_uid.find(c->uid);
            if (found == by_uid.end()) {
                rf::obj_flag_dead(c);
                continue;
            }
            restore_corpse(*c, *found->second, refs);
            restore_ragdoll(*c, *found->second);
            restore_collision_spheres(*c, *found->second);
            matched.insert(c->uid);
        }
        for (const auto& r : records) {
            if (matched.contains(r.base.uid) || has_object_with_uid(r.base.uid, rf::OT_CORPSE)) {
                continue;
            }
            if (rf::Corpse* c = create_corpse(r)) {
                restore_corpse(*c, r, refs);
                restore_ragdoll(*c, r);
                restore_collision_spheres(*c, r);
            }
        }
    }

    // As the stock decal load (0x004B5190), which adds every saved decal to those the level made
    void restore_decals(const std::vector<asg::DecalRecord>& records)
    {
        if (!rf::level.geometry) {
            return;
        }
        for (const auto& r : records) {
            if (r.bitmap.empty()) {
                continue;
            }
            rf::GDecalCreateInfo dci{};
            dci.pos = r.pos;
            dci.orient = r.orient;
            dci.extents = r.extents;
            dci.texture = rf::bm::load(r.bitmap.c_str(), -1, true);
            dci.room = rf::level.geometry->find_room(nullptr, &dci.pos, &dci.pos, nullptr);
            dci.alpha = r.alpha;
            dci.flags = static_cast<int>(r.decal_flags);
            dci.object_handle = -1;
            dci.solid = rf::level.geometry;
            dci.scale = r.tiling_scale;
            rf::g_decal_add(&dci);
        }
    }

    // As the stock blood pool load (0x0042E6B0): each pool takes a free one, full grown
    void restore_blood_pools(const std::vector<asg::BloodPoolRecord>& records)
    {
        for (const auto& r : records) {
            rf::EntityBloodPool* p = rf::blood_pool_free_list;
            if (!p) {
                xlog::warn("[ASG] No free blood pool for {} saved ones", records.size());
                break;
            }
            p->pool_age = 8.0f;
            p->last_known_size = 0.5f;
            p->grow_time_seconds = 8.0f;
            p->pool_size = 0.5f;
            p->seconds_to_radians_scalar = 0.19634954f;
            p->pool_pos = r.pos;
            p->pool_orient = r.orient;
            p->pool_color = {r.red, r.green, r.blue, r.alpha};
            p->pool_room = rf::level.geometry
                ? rf::level.geometry->find_room(nullptr, &p->pool_pos, &p->pool_pos, nullptr)
                : nullptr;
            if (p->next == p) {
                rf::blood_pool_free_list = nullptr;
            }
            else {
                rf::blood_pool_free_list = p->next;
                p->prev->next = p->next;
                p->next->prev = p->prev;
            }
            if (rf::EntityBloodPool* head = rf::blood_pool_used_list) {
                p->next = head;
                p->prev = head->prev;
                head->prev->next = p;
                head->prev = p;
            }
            else {
                p->next = p;
                p->prev = p;
                rf::blood_pool_used_list = p;
            }
        }
    }

    void restore_emitters(const std::vector<asg::EmitterRecord>& records)
    {
        for (const auto& r : records) {
            if (r.kind == 0) {
                if (rf::ParticleEmitter* e = rf::level_get_particle_emitter_from_uid(r.uid)) {
                    e->active = r.active;
                    e->time_to_change = r.time_to_change;
                    e->current_state_time = r.current_state_time;
                }
            }
            else if (rf::BoltEmitter* e = rf::level_get_bolt_emitter_from_uid(r.uid)) {
                e->active = r.active;
            }
        }
    }

    void restore_push_regions(const std::vector<asg::PushRegionRecord>& records)
    {
        for (const auto& r : records) {
            if (rf::PushRegion* p = rf::level_get_push_region_from_uid(r.uid)) {
                p->is_enabled = r.active;
            }
        }
    }

    // Each dynamic level light (Light_State, Set_Light_Color): the renderer's light, and the hue it is derived from
    void restore_lights(const std::vector<asg::LightRecord>& records)
    {
        for (const auto& r : records) {
            rf::gr::LevelLight* level_light = rf::gr::level_light_lookup_from_uid(r.uid);
            rf::gr::Light* light = level_light ? asg::level_light_gr_light(*level_light) : nullptr;
            if (!light) {
                xlog::warn("[ASG] Saved light {} is not in the level", r.uid);
                continue;
            }
            light->on = r.on;
            light->r = r.color.x;
            light->g = r.color.y;
            light->b = r.color.z;
            level_light->hue_r = r.hue.x;
            level_light->hue_g = r.hue.y;
            level_light->hue_b = r.hue.z;
        }
    }

    // As the stock goal load (0x004BE370): a goal keeps its initial count once it exists
    void restore_goals(const std::vector<asg::GoalRecord>& records)
    {
        auto& goals = asg::persistent_goals();
        for (const auto& r : records) {
            const auto it = std::find_if(goals.begin(), goals.end(), [&](const rf::PersistentGoalEvent& goal) {
                return string_iequals(goal.name.c_str(), r.name);
            });
            if (it != goals.end()) {
                it->count = r.count;
            }
            else {
                auto& goal = goals.emplace_back();
                goal.name = r.name.c_str();
                goal.initial_count = r.initial_count;
                goal.count = r.count;
            }
        }
    }

    void noop_death_fade_callback(rf::Player*) {}
}

void asg::set_position(rf::Object& obj, const rf::Vector3& pos, const rf::Matrix3& orient)
{
    obj.p_data.pos = pos;
    obj.p_data.next_pos = pos;
    obj.pos = pos;
    obj.last_pos = pos;
    obj.p_data.orient = orient;
    obj.orient = orient;
}

void asg::restore_timestamp(rf::Timestamp& ts, int32_t value)
{
    if (value == ts_invalid) {
        ts.invalidate();
    }
    else {
        ts.set(value);
    }
}

void asg::RefResolver::add(int32_t uid, int* handle)
{
    *handle = -1;
    if (uid != uid_none) {
        m_refs.push_back({uid, handle});
    }
}

void asg::RefResolver::resolve(bool warn_unresolved)
{
    for (const auto& ref : m_refs) {
        rf::Object* obj = rf::obj_lookup_from_uid(ref.uid);
        *ref.handle = obj ? obj->handle : -1;
        if (!obj && warn_unresolved) {
            xlog::warn("[ASG] Unresolved object reference to uid {}", ref.uid);
        }
    }
}

void asg::restore_level(const LevelState& lvl, bool transition, RefResolver& refs)
{
    clear_deferred_alpine_restore();
    g_player_placement = {};
    reserve_runtime_uids(lvl);
    restore_geometry(lvl);
    restore_entities(lvl.entities, transition, refs);
    if (lvl.corpses) {
        restore_corpses(*lvl.corpses, refs);
    }
    if (lvl.items) {
        restore_items(*lvl.items, refs);
    }
    if (lvl.clutter) {
        restore_clutter(*lvl.clutter, transition, refs);
    }
    if (lvl.triggers) {
        restore_triggers(*lvl.triggers, refs);
    }
    if (lvl.movers) {
        restore_movers(*lvl.movers, refs);
    }
    if (lvl.weapons) {
        restore_weapons(*lvl.weapons, refs);
    }
    if (lvl.events) {
        restore_events(*lvl.events, refs);
    }
    if (lvl.alpine_objects) {
        restore_alpine_objects(*lvl.alpine_objects);
    }
    refs.resolve(transition);
    if (lvl.emitters) {
        restore_emitters(*lvl.emitters);
    }
    if (lvl.push_regions) {
        restore_push_regions(*lvl.push_regions);
    }
    if (lvl.lights) {
        restore_lights(*lvl.lights);
    }
    if (lvl.decals) {
        restore_decals(*lvl.decals);
    }
    if (lvl.blood_pools) {
        restore_blood_pools(*lvl.blood_pools);
    }
    if (lvl.goals) {
        restore_goals(*lvl.goals);
    }
    if (lvl.world) {
        restore_world(*lvl.world, transition);
    }
    rf::level.time = lvl.level_time;
}

// As sr_load_player (0x004B4C80)
bool asg::restore_player(const PlayerState& state, const EntityRecord& record, rf::Player& player, RefResolver& refs)
{
    const int entity_type = rf::entity_lookup_type(record.base.cls.c_str());
    if (entity_type < 0) {
        xlog::error("[ASG] Unknown player entity class '{}'", record.base.cls);
        return false;
    }
    rf::Vector3 pos = record.base.pos;
    rf::Matrix3 orient = record.base.orient;
    rf::Entity* ent = rf::player_create_entity(&player, entity_type, &pos, &orient, -1);
    if (!ent) {
        return false;
    }
    if (state.undercover_active) {
        rf::player_undercover_start(state.undercover_team);
        ent = rf::local_player_entity;
        if (!ent) {
            return false;
        }
        ent->ai.current_primary_weapon = rf::undercover_weapon_type;
        rf::player_fpgun_set_weapon_mesh(&player, rf::undercover_weapon_type);
        rf::player_undercover_set_gun_skin();
    }
    restore_entity(*ent, record, refs);
    refs.resolve(false);

    player.flags = state.player_flags & ~rf::PF_UNK_8;
    player.entity_type = entity_type;
    player.spew_vector_index = state.spew_vector_index;
    player.spew_pos = state.spew_pos;
    std::fill(std::begin(player.key_items), std::end(player.key_items), false);
    for (int32_t key : state.key_items) {
        if (key >= 0 && key < static_cast<int>(std::size(player.key_items))) {
            player.key_items[key] = true;
        }
    }
    // As stock, the view is the player's own. Weapon preferences stay the player's current ones, as Alpine has the
    // stock load leave them.
    player.view_from_handle = -1;
    player.fpgun_data.fpgun_pos = state.fpgun_pos;
    player.fpgun_data.fpgun_orient = state.fpgun_orient;
    player.fpgun_data.show_silencer = state.show_silencer;
    player.fpgun_data.grenade_mode = state.grenade_mode;
    player.fpgun_data.remote_charge_in_hand = state.remote_charge_in_hand;
    if (ent->ai.current_primary_weapon == rf::remote_charge_weapon_type && !state.remote_charge_in_hand) {
        ent->ai.current_primary_weapon = rf::remote_charge_det_weapon_type;
    }
    rf::player_cover_id = state.undercover_cover_id;
    player.collides_with_world = state.collides_with_world;

    rf::Object* host = rf::obj_from_handle(ent->host_handle);
    if (host) {
        const int tag = ent->host_tag_handle;
        ent->host_tag_handle = -1;
        if (host->type == rf::OT_ENTITY) {
            auto* vehicle = static_cast<rf::Entity*>(host);
            rf::entity_headlamp_turn_off(vehicle);
            rf::entity_attach_leech(vehicle, ent->handle, tag);
            vehicle->entity_flags = (vehicle->entity_flags & ~rf::EF_UNK_20000) | rf::EF_BOARDED;
            rf::obj_set_friendliness(vehicle, rf::OBJ_NEUTRAL);
            vehicle->min_rel_eye_phb = vehicle->info->min_rel_eye_phb;
            vehicle->max_rel_eye_phb = vehicle->info->max_rel_eye_phb;
            if (rf::entity_is_jeep_gunner(ent)) {
                ent->min_rel_eye_phb = rf::jeep_gunner_min_rel_eye_phb;
                ent->max_rel_eye_phb = rf::jeep_gunner_max_rel_eye_phb;
            }
            if (rf::entity_is_automobile(vehicle)) {
                rf::obj_physics_activate(vehicle);
            }
        }
    }
    rf::entity_update_collision_spheres(ent);
    if (ent->entity_flags & rf::EF_CROUCHING) {
        rf::entity_crouch(ent);
        player.is_crouched = true;
    }

    // The fade callback cannot be stored; the player flags carry what follows a blackout
    const bool blackout_flags = player.flags & (rf::PF_KILL_AFTER_BLACKOUT | rf::PF_END_LEVEL_AFTER_BLACKOUT);
    if (state.blackout_active || blackout_flags) {
        rf::player_start_death_fade(&player, state.blackout_time_sec > 0.0f ? state.blackout_time_sec : 1.5f,
                                    noop_death_fade_callback);
        if (state.blackout_active) {
            player.death_fade_alpha = state.blackout_alpha;
        }
    }

    refs.resolve(true);
    // Stock puts the player on the ground below, and makes one with none below fall (0x004A0840): not when swimming
    const bool stick_to_ground = record.move_mode != rf::MOVE_MODE_SWIM;
    if (stick_to_ground) {
        rf::physics_stick_to_ground(ent);
    }
    g_player_placement = {ent->handle, record.base.pos, record.base.orient, stick_to_ground};
    return true;
}

void asg::replay_rf2_geomods()
{
    const PlayerPlacement placement = std::exchange(g_player_placement, {});
    if (!destruction_replay_rf2_geomods()) {
        return;
    }
    // The ground restore_player put the player on can be what the carves removed
    rf::Object* obj = rf::obj_from_handle(placement.entity_handle);
    if (obj && obj->type == rf::OT_ENTITY) {
        auto* ent = static_cast<rf::Entity*>(obj);
        set_position(*ent, placement.pos, placement.orient);
        if (placement.stick_to_ground) {
            rf::physics_stick_to_ground(ent);
        }
    }
}

void asg::restore_game(const GameState& game)
{
    rf::level.global_time = game.level_time;
    rf::level.time_left = game.level_time_left;
    hud_message_log() = game.messages;
    if (!game.music.empty() && !rf::music_suppressed()) {
        std::string music = game.music;
        rf::snd_music_play(music.data(), 1.0f);
    }
}
