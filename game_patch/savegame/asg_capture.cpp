#include <algorithm>
#include <iterator>
#include <common/utils/string-utils.h>
#include "asg_internal.h"
#include "../misc/destruction.h"
#include "../multi/vehicles/physics/ragdoll.h"
#include "../rf/bmpman.h"
#include "../rf/clutter.h"
#include "../rf/corpse.h"
#include "../rf/entity.h"
#include "../rf/event.h"
#include "../rf/gameseq.h"
#include "../rf/geometry.h"
#include "../rf/gr/gr_light.h"
#include "../rf/item.h"
#include "../rf/level.h"
#include "../rf/mover.h"
#include "../rf/particle_emitter.h"
#include "../rf/player/player.h"
#include "../rf/sound/sound.h"
#include "../rf/trigger.h"
#include "../rf/vmesh.h"
#include "../rf/weapon.h"
#include "../object/object.h"

namespace
{
    std::string weapon_name(int weapon_type)
    {
        if (weapon_type < 0 || weapon_type >= rf::num_weapon_types) {
            return {};
        }
        return rf::weapon_types[weapon_type].name.c_str();
    }

    void capture_base(rf::Object& obj, std::string cls, asg::ObjectBase& b)
    {
        b.uid = obj.uid;
        b.origin = asg::uid_origin(obj.uid);
        b.cls = std::move(cls);
        b.parent_uid = asg::uid_from_handle(obj.parent_handle);
        b.host_uid = asg::uid_from_handle(obj.host_handle);
        b.pos = obj.pos;
        b.orient = obj.orient;
        b.vel = obj.p_data.vel;
        b.ang_momentum = obj.p_data.ang_momentum;
        b.life = obj.life;
        b.armor = obj.armor;
        // Restore sets the teleport bits again, as stock does. OF_HOST_FOLLOWED and OF_WAS_RENDERED are per-frame
        // state: obj_move_all and obj_clear_render_flags (0x00488200) clear them on every object each frame.
        b.obj_flags = static_cast<uint32_t>(obj.obj_flags)
            & ~static_cast<uint32_t>(rf::OF_WAS_TELEPORTED | rf::OF_UNK_2000000 | rf::OF_HOST_FOLLOWED
                                     | rf::OF_WAS_RENDERED);
        b.physics_flags = static_cast<uint32_t>(obj.p_data.flags);
        b.friendliness = obj.friendliness;
        b.host_tag_handle = obj.host_tag_handle;
        b.skin = obj_get_skin(obj);
    }

    int path_node_index(const rf::GPathNode* node)
    {
        if (!node || !rf::level.geometry) {
            return -1;
        }
        auto& nodes = rf::level.geometry->nodes.nodes;
        for (int i = 0; i < nodes.size(); ++i) {
            if (nodes[i] == node) {
                return i;
            }
        }
        return -1;
    }

    void capture_weapons(const rf::Entity& e, asg::EntityRecord& r)
    {
        const auto& ai = e.ai;
        for (int w = 0; w < rf::num_weapon_types; ++w) {
            const int ammo_type = rf::weapon_types[w].ammo_type;
            const bool has_ammo_type = ammo_type >= 0 && ammo_type < static_cast<int>(std::size(ai.ammo));
            const bool custom_mode = w < asg::num_weapon_bits && (e.weapon_custom_mode_bitmap & (1 << w));
            const bool silencer = w < asg::num_weapon_bits && (e.weapon_silencer_bitfield & (1 << w));
            const int ammo = has_ammo_type ? ai.ammo[ammo_type] : 0;
            if (!ai.has_weapon[w] && !ai.clip_ammo[w] && !ammo && !custom_mode && !silencer) {
                continue;
            }
            r.weapons.push_back({
                .weapon = weapon_name(w),
                .clip_ammo = ai.clip_ammo[w],
                .ammo = ammo,
                .possessed = ai.has_weapon[w],
                .custom_mode = custom_mode,
                .silencer = silencer,
            });
        }
    }

    asg::EntityRecord capture_entity(rf::Entity& e)
    {
        asg::EntityRecord r;
        capture_base(e, e.info->name.c_str(), r.base);
        const auto& ai = e.ai;
        r.primary_weapon = weapon_name(ai.current_primary_weapon);
        r.secondary_weapon = weapon_name(ai.current_secondary_weapon);
        capture_weapons(e, r);
        for (int i = 0; i < ai.hate_list.size(); ++i) {
            r.hate_list.push_back(asg::uid_from_handle(ai.hate_list[i]));
        }
        r.ai_mode = ai.mode;
        r.ai_submode = ai.submode;
        r.move_mode = rf::MOVE_MODE_RUN;
        for (int mode = rf::MOVE_MODE_NONE; mode <= rf::MOVE_MODE_JOHNS_DESCENT_FLYING; ++mode) {
            if (e.move_mode == rf::movemode_get_mode(mode)) {
                r.move_mode = mode;
                break;
            }
        }
        r.ai_mode_parm_0 = ai.mode_parm_0;
        r.ai_mode_parm_1 = ai.mode_parm_1;
        r.target_uid = asg::uid_from_handle(ai.target_handle);
        // Stock keeps a player's standing_on_obj_handle in this slot (0x0042BB74)
        const bool standing_in_look_at = rf::obj_is_player(&e) && (e.p_data.flags & rf::PF_UNK_400000);
        r.look_at_uid = asg::uid_from_handle(standing_in_look_at ? e.control_data.standing_on_obj_handle
                                                                 : ai.look_at_handle);
        r.shoot_at_uid = asg::uid_from_handle(ai.shoot_at_handle);
        r.ai_flags = static_cast<uint32_t>(ai.ai_flags);
        r.ai_healing_left = ai.healing_left;
        r.ai_steering_vector = ai.steering_vector;
        r.corpse_carry_uid = asg::uid_from_handle(ai.corpse_carry_handle);

        const auto& path = ai.current_path;
        for (int i = 0; i < 4; ++i) {
            r.path_node_indices[i] = static_cast<int16_t>(path_node_index(path.nodes[i]));
        }
        r.path_previous_goal = static_cast<int16_t>(path.previous_goal);
        r.path_current_goal = static_cast<int16_t>(path.current_goal);
        r.path_end_pos = path.end_node.pos;
        r.path_adjacent_node1_index = static_cast<int16_t>(path_node_index(path.adjacent_node1));
        r.path_adjacent_node2_index = static_cast<int16_t>(path_node_index(path.adjacent_node2));
        r.path_waypoint_list_index = static_cast<int16_t>(path.waypoint_list_index);
        r.path_goal_waypoint_index = static_cast<int16_t>(path.goal_waypoint_index);
        r.path_direction = static_cast<uint8_t>(path.direction);
        r.path_flags = static_cast<uint8_t>(path.flags);
        r.path_turn_towards_pos = path.turn_towards_pos;
        r.path_follow_style = path.follow_style;
        r.path_start_pos = path.start_pos;
        r.path_goal_pos = path.goal_pos;

        r.ci_rot = ai.ci.rot;
        r.ci_move = ai.ci.move;
        r.ci_time_relative_mouse_delta_disabled = ai.ci.field_18;
        r.control_phb = e.control_data.phb;
        r.control_eye_phb = e.control_data.eye_phb;
        r.control_local_vel = e.control_data.local_vel;
        r.eye_pos = e.eye_pos;
        r.eye_orient = e.eye_orient;
        // As stock: a reload in progress is not resumed
        r.entity_flags = e.entity_flags & ~rf::EF_RELOADING;
        r.entity_flags2 = e.entity_flags2;
        const EntityAnimState anim = entity_get_anim_state(e);
        r.current_state_anim = static_cast<int16_t>(anim.current_state_anim);
        r.next_state_anim = static_cast<int16_t>(anim.next_state_anim);
        r.last_custom_anim_index = static_cast<int16_t>(anim.custom_action_anim);
        r.state_anim_22_index = static_cast<int16_t>(anim.custom_state_anim);
        r.total_transition_time = anim.total_transition_time;
        r.elapsed_transition_time = anim.elapsed_transition_time;
        r.driller_rot_angle = e.driller_rot_angle;
        r.driller_rot_speed = e.driller_rot_speed;
        r.driller_geomod_count = e.driller_geomod_count;
        r.current_speed = static_cast<uint8_t>(e.current_speed);
        r.on_fire = e.entity_fire_handle != nullptr;
        for (int i = 0; i < rf::level.ladders.size(); ++i) {
            if (rf::level.ladders[i] == e.current_climb_region) {
                r.climb_region_index = i;
                break;
            }
        }
        if (e.drop_item_class >= 0 && e.drop_item_class < rf::num_item_types) {
            r.item_drop_override = rf::item_info[e.drop_item_class].cls_name.c_str();
        }
        return r;
    }

    void capture_entities(asg::EntityState& out)
    {
        for (rf::Entity* e = rf::entity_list.next; e != &rf::entity_list; e = e->next) {
            if ((e->obj_flags & (rf::OF_IN_LEVEL_TRANSITION | rf::OF_DELAYED_DELETE)) || asg::is_ambient_entity(*e)) {
                continue;
            }
            if (rf::obj_is_hidden(e)) {
                out.dead_uids.push_back(e->uid);
                continue;
            }
            out.entities.push_back(capture_entity(*e));
            if (e->p_data.flags & rf::PF_ACTIVE) {
                out.entities.back().base.obj_flags |= rf::OF_SAVED_PHYSICS_ACTIVE;
            }
        }
    }

    std::vector<asg::ItemRecord> capture_items()
    {
        std::vector<asg::ItemRecord> out;
        for (rf::Item* it = rf::item_list.next; it != &rf::item_list; it = it->next) {
            if (it->obj_flags & (rf::OF_IN_LEVEL_TRANSITION | rf::OF_DELAYED_DELETE)) {
                continue;
            }
            auto& r = out.emplace_back();
            capture_base(*it, it->info->cls_name.c_str(), r.base);
            r.respawn_time_ms = it->respawn_time_ms;
            r.respawn_timer = asg::capture_timestamp(it->respawn_next);
            r.alpha = it->alpha;
            r.create_time = it->create_time;
            r.item_flags = static_cast<uint32_t>(it->item_flags);
        }
        return out;
    }

    std::vector<asg::ClutterRecord> capture_clutter()
    {
        std::vector<asg::ClutterRecord> out;
        for (rf::Clutter* c = rf::clutter_list.next; c != &rf::clutter_list; c = c->next) {
            if ((c->obj_flags & (rf::OF_IN_LEVEL_TRANSITION | rf::OF_DELAYED_DELETE)) || asg::is_alpine_clutter(*c)) {
                continue;
            }
            auto& r = out.emplace_back();
            capture_base(*c, c->info ? c->info->cls_name.c_str() : "", r.base);
            r.delayed_kill_timer = asg::capture_timestamp(c->delayed_kill_timestamp);
            r.delayed_kill_sound = c->delayed_kill_sound;
            r.emitter_kill_timer = asg::capture_timestamp(c->emitter_kill_timestamp);
            r.corpse_create_timer = asg::capture_timestamp(c->corpse_create_timestamp);
            r.skin_index = c->current_skin_index;
            r.hidden = (c->obj_flags & rf::OF_HIDDEN) != 0;
        }
        return out;
    }

    std::vector<asg::TriggerRecord> capture_triggers()
    {
        std::vector<asg::TriggerRecord> out;
        for (rf::Trigger* t = rf::trigger_list.next; t != &rf::trigger_list; t = t->next) {
            if (t->obj_flags & rf::OF_DELAYED_DELETE) {
                continue;
            }
            auto& r = out.emplace_back();
            capture_base(*t, {}, r.base);
            r.count = t->count;
            r.time_last_activated = t->time_last_activated;
            r.trigger_flags = static_cast<uint32_t>(t->trigger_flags);
            r.activator_uid = asg::uid_from_handle(t->activator_handle);
            r.button_active_timer = asg::capture_timestamp(t->button_active_timestamp);
            r.inside_timer = asg::capture_timestamp(t->inside_timestamp);
            r.reset_timer = asg::capture_timestamp(t->next_check);
        }
        return out;
    }

    void add_liquid_room(std::vector<rf::GRoom*>& rooms, rf::GRoom* room)
    {
        if (room && std::find(rooms.begin(), rooms.end(), room) == rooms.end()) {
            rooms.push_back(room);
        }
    }

    // The rooms whose depth can have changed: those Set_Liquid_Depth events act on (as
    // EventSetLiquidDepth_turn_on_new picks them) and those with a depth change in progress
    std::vector<asg::LiquidDepthRecord> capture_liquid_depths()
    {
        std::vector<rf::GRoom*> rooms;
        for (rf::Event* e : rf::find_all_events_by_type(rf::EventType::Set_Liquid_Depth)) {
            if (e->links.size() == 0) {
                add_liquid_room(rooms, e->room);
            }
            for (int room_uid : e->links) {
                add_liquid_room(rooms, rf::level_room_from_uid(room_uid));
            }
        }
        for (const rf::LiquidDepthUpdate* update : rf::liquid_depth_updates) {
            add_liquid_room(rooms, update->room);
        }
        std::vector<asg::LiquidDepthRecord> out;
        for (rf::GRoom* room : rooms) {
            auto& r = out.emplace_back();
            r.room_uid = room->uid;
            r.depth = room->liquid_depth;
            for (const rf::LiquidDepthUpdate* update : rf::liquid_depth_updates) {
                if (update->room == room) {
                    r.has_pending = true;
                    r.target_depth = update->target_depth;
                    r.remaining_duration = std::max(update->duration - update->elapsed, 0.0f);
                    break;
                }
            }
        }
        return out;
    }

    asg::RoomState capture_rooms()
    {
        asg::RoomState out;
        // room_kill appends without a bound check, so the list holds every killed room
        for (int i = 0; i < rf::num_killed_rooms; ++i) {
            const int32_t uid = rf::killed_room_uids[i];
            if (std::ranges::find(out.killed_room_uids, uid) == out.killed_room_uids.end()) {
                out.killed_room_uids.push_back(uid);
            }
        }
        out.liquid_depths = capture_liquid_depths();
        if (rf::level.geometry) {
            out.world_bbox_min = rf::level.geometry->bbox_min;
            out.world_bbox_max = rf::level.geometry->bbox_max;
        }
        return out;
    }

    // geomod_create (0x00467020) stops recording and carving craters at max_geomod_craters, so the array holds them
    // all; RF2-style carves keep no record there
    std::vector<asg::CraterRecord> capture_craters()
    {
        std::vector<asg::CraterRecord> out;
        const int n = std::clamp(rf::g_num_geomods_this_level, 0, rf::max_geomod_craters);
        for (int i = 0; i < n; ++i) {
            const auto& c = rf::g_geomod_crater_records[i];
            out.push_back({
                .shape_index = c.shape_index,
                .flags = c.flags,
                .room_index = c.room_index,
                .pos = {c.pos[0], c.pos[1], c.pos[2]},
                .hit_normal = {c.hit_normal[0], c.hit_normal[1], c.hit_normal[2]},
                .orient = {c.orient[0], c.orient[1], c.orient[2], c.orient[3]},
                .scale = c.scale,
            });
        }
        return out;
    }

    std::vector<asg::Rf2GeomodRecord> capture_rf2_geomods()
    {
        std::vector<asg::Rf2GeomodRecord> out;
        for (const rf::GeomodParams& p : destruction_rf2_geomod_records()) {
            out.push_back({
                .shape_index = p.shape_index,
                .room_index = p.room_index,
                .pos = p.pos,
                .orient = p.orient,
                .flags = p.flags,
                .scale = p.scale,
                .hit_normal = p.hit_normal,
                .field_4c = p.field_4C,
                .field_58 = p.field_58,
            });
        }
        return out;
    }

    // Partial damage of destructible detail rooms; destroyed ones are in the killed list
    std::vector<asg::RoomLifeRecord> capture_breakable_rooms()
    {
        std::vector<asg::RoomLifeRecord> out;
        if (!rf::level.geometry) {
            return out;
        }
        for (rf::GRoom* room : rf::level.geometry->all_rooms) {
            if (room && room->is_detail && room->life >= 0.0f) {
                out.push_back({room->uid, room->life});
            }
        }
        return out;
    }

    // Every mover, as stock (0x0046B3D0); a load only updates them
    std::vector<asg::MoverRecord> capture_movers()
    {
        std::vector<asg::MoverRecord> out;
        for (rf::Mover* m = rf::mover_list.next; m != &rf::mover_list; m = m->next) {
            auto& r = out.emplace_back();
            capture_base(*m, {}, r.base);
            r.rot_cur_pos = m->rot_cur_pos;
            r.start_at_keyframe = m->start_at_keyframe;
            r.stop_at_keyframe = m->stop_at_keyframe;
            r.stop_completely_at_keyframe = m->stop_completely_at_keyframe;
            r.mover_flags = static_cast<uint32_t>(m->mover_flags);
            r.travel_time_seconds = m->travel_time_seconds;
            r.rotation_travel_time_seconds = m->rotation_travel_time_seconds_unk;
            r.wait_timer = asg::capture_timestamp(m->wait_timestamp);
            r.trigger_uid = asg::uid_from_handle(m->trigger_handle);
            r.dist_travelled = m->dist_travelled;
            r.cur_vel = m->cur_vel;
        }
        return out;
    }

    std::vector<asg::WeaponRecord> capture_weapons()
    {
        std::vector<asg::WeaponRecord> out;
        for (rf::Weapon* w = rf::weapon_list.next; w != &rf::weapon_list; w = w->next) {
            if (w->obj_flags & (rf::OF_IN_LEVEL_TRANSITION | rf::OF_DELAYED_DELETE | rf::OF_UNK_20000000)) {
                continue;
            }
            auto& r = out.emplace_back();
            capture_base(*w, weapon_name(w->info_index), r.base);
            r.life_left_seconds = w->lifeleft_seconds;
            r.weapon_flags = static_cast<uint32_t>(w->weapon_flags);
            r.sticky_host_uid = asg::uid_from_handle(w->sticky_host_handle);
            r.sticky_host_pos_offset = w->sticky_host_pos_offset;
            r.sticky_host_orient = w->sticky_host_orient;
            r.weap_friendliness = static_cast<uint8_t>(w->friendliness);
            r.target_uid = asg::uid_from_handle(w->target_handle);
            r.pierce_power_left = w->pierce_power_left;
            r.thrust_left = w->thrust_left;
            r.firing_pos = w->firing_pos;
        }
        return out;
    }

    std::vector<asg::CorpseRecord> capture_corpses()
    {
        std::vector<asg::CorpseRecord> out;
        for (rf::Corpse* c = rf::corpse_list.next; c != &rf::corpse_list; c = c->next) {
            if (c->obj_flags & (rf::OF_IN_LEVEL_TRANSITION | rf::OF_DELAYED_DELETE)) {
                continue;
            }
            auto& r = out.emplace_back();
            capture_base(*c, {}, r.base);
            r.create_time = c->create_time;
            r.lifetime_seconds = c->lifetime_seconds;
            // As stock (0x004176A0): a restored corpse holds the last frame of its pose
            r.corpse_flags = static_cast<uint32_t>(c->corpse_flags) | rf::corpse_flag_hold_last_frame;
            if (c->entity_type >= 0 && c->entity_type < rf::num_entity_types) {
                r.entity_cls = rf::entity_types[c->entity_type].name.c_str();
            }
            if (c->vmesh) {
                r.mesh_name = rf::vmesh_get_name(c->vmesh);
            }
            r.emitter_kill_timer = asg::capture_timestamp(c->emitter_kill_timestamp);
            r.body_temp = c->body_temp;
            r.state_anim = c->corpse_state_vmesh_anim_index;
            r.action_anim = c->corpse_action_vmesh_anim_index;
            r.drop_anim = c->corpse_drop_vmesh_anim_index;
            r.carry_anim = c->corpse_carry_vmesh_anim_index;
            r.corpse_pose = c->corpse_pose;
            if (c->helmet_v3d_handle) {
                r.helmet_name = rf::vmesh_get_name(c->helmet_v3d_handle);
            }
            r.item_uid = asg::uid_from_handle(c->item_handle);
            r.mass = c->p_data.mass;
            r.radius = c->p_data.radius;
            for (const auto& s : c->p_data.cspheres) {
                r.cspheres.push_back({s.center, s.radius, s.spring_const, s.spring_length});
            }
            std::vector<rf::Matrix43> bodies;
            std::vector<rf::Matrix43> bones;
            if (ragdoll_get_pose(*c, bodies, bones)) {
                for (const auto& m : bodies) {
                    r.ragdoll_bodies.push_back({m.orient, m.origin});
                }
                for (const auto& m : bones) {
                    r.ragdoll_bones.push_back({m.orient, m.origin});
                }
            }
        }
        return out;
    }

    // The decals stock saves (0x004B4660), less the geomod blast marks: replaying the craters makes those again
    // (geomod_step state 0, 0x00466D2F), where stock ends up with two of each
    std::vector<asg::DecalRecord> capture_decals()
    {
        std::vector<asg::DecalRecord> out;
        constexpr int skipped_flags = rf::DF_NEVER_DESTROY | rf::DF_NEVER_SKIP_FADE_OUT | rf::DF_GEOMOD;
        rf::GDecal* const head = rf::g_decal_list;
        for (rf::GDecal* d = head; d; d = d->next) {
            const char* bitmap = d->bitmap_id > 0 ? rf::bm::get_filename(d->bitmap_id) : nullptr;
            if (!(d->flags & skipped_flags) && bitmap && *bitmap) {
                out.push_back({
                    .pos = d->pos,
                    .orient = d->orient,
                    .extents = d->width,
                    .decal_flags = static_cast<uint32_t>(d->flags),
                    .alpha = d->alpha,
                    .tiling_scale = d->tiling_scale,
                    .bitmap = bitmap,
                });
            }
            if (d->next == head) {
                break;
            }
        }
        return out;
    }

    std::vector<asg::BloodPoolRecord> capture_blood_pools()
    {
        std::vector<asg::BloodPoolRecord> out;
        rf::EntityBloodPool* const head = rf::blood_pool_used_list;
        for (rf::EntityBloodPool* p = head; p; p = p->next) {
            out.push_back({
                .pos = p->pool_pos,
                .orient = p->pool_orient,
                .red = p->pool_color.red,
                .green = p->pool_color.green,
                .blue = p->pool_color.blue,
                .alpha = p->pool_color.alpha,
            });
            if (p->next == head) {
                break;
            }
        }
        return out;
    }

    std::vector<asg::EmitterRecord> capture_emitters()
    {
        std::vector<asg::EmitterRecord> out;
        for (rf::ParticleEmitter* e : rf::level.pemitters) {
            out.push_back({.kind = 0, .uid = e->uid, .active = e->active, .time_to_change = e->time_to_change,
                           .current_state_time = e->current_state_time});
        }
        for (rf::BoltEmitter* e : rf::level.bemitters) {
            out.push_back({.kind = 1, .uid = e->uid, .active = e->active});
        }
        return out;
    }

    std::vector<asg::PushRegionRecord> capture_push_regions()
    {
        std::vector<asg::PushRegionRecord> out;
        for (rf::PushRegion* p : rf::level.pushers) {
            out.push_back({p->uid, p->is_enabled});
        }
        return out;
    }

    std::vector<asg::LightRecord> capture_lights()
    {
        std::vector<asg::LightRecord> out;
        for (rf::gr::LevelLight* level_light : rf::level.lights) {
            if (!level_light || !level_light->dynamic) {
                continue;
            }
            if (rf::gr::Light* light = asg::level_light_gr_light(*level_light)) {
                out.push_back({level_light->uid, light->on, {light->r, light->g, light->b},
                               {level_light->hue_r, level_light->hue_g, level_light->hue_b}});
            }
        }
        return out;
    }

    std::vector<asg::GoalRecord> capture_goals()
    {
        std::vector<asg::GoalRecord> out;
        for (const auto& goal : asg::persistent_goals()) {
            out.push_back({goal.name.c_str(), goal.count, goal.initial_count});
        }
        return out;
    }
}

rf::gr::Light* asg::level_light_gr_light(const rf::gr::LevelLight& level_light)
{
    int handle = level_light.gr_light_handle;
    if (handle < 0) {
        handle = rf::gr::level_get_light_handle_from_uid(level_light.uid);
    }
    return handle >= 0 ? rf::gr::light_get_from_handle(handle) : nullptr;
}

bool asg::is_alpine_clutter(const rf::Clutter& c)
{
    // Alpine objects are clutter outside clutter.tbl (alpine_init_anchor_clutter), stored with the Alpine objects
    return c.info_index < 0;
}

int32_t asg::capture_timestamp(const rf::Timestamp& ts)
{
    return ts.valid() ? ts.time_until() : ts_invalid;
}

// Runtime objects take their uid from obj_next_uid, which counts down from 0x7FFFFFFF
uint8_t asg::uid_origin(int32_t uid)
{
    return uid != player_entity_uid && uid > rf::obj_next_uid ? 1 : 0;
}

int32_t asg::uid_from_handle(int handle)
{
    rf::Object* obj = rf::obj_from_handle(handle);
    return obj ? obj->uid : uid_none;
}

bool asg::is_ambient_entity(const rf::Entity& e)
{
    // Stock neither saves nor restores these (0x0042C1BF)
    const char* name = e.info->name.c_str();
    return string_iequals(name, "bat") || string_iequals(name, "fish");
}

asg::LevelState asg::capture_level(bool with_goals)
{
    LevelState lvl;
    lvl.filename = rf::level.filename.c_str();
    lvl.rfl_checksum = rf::level.checksum;
    lvl.level_time = rf::level.time;
    lvl.rooms = capture_rooms();
    lvl.craters = capture_craters();
    lvl.rf2_geomods = capture_rf2_geomods();
    lvl.breakable_rooms = capture_breakable_rooms();
    capture_entities(lvl.entities);
    lvl.items = capture_items();
    lvl.clutter = capture_clutter();
    lvl.triggers = capture_triggers();
    lvl.movers = capture_movers();
    lvl.weapons = capture_weapons();
    lvl.corpses = capture_corpses();
    lvl.events = capture_events();
    lvl.decals = capture_decals();
    lvl.blood_pools = capture_blood_pools();
    lvl.emitters = capture_emitters();
    lvl.push_regions = capture_push_regions();
    lvl.lights = capture_lights();
    lvl.world = capture_world();
    lvl.alpine_objects = capture_alpine_objects();
    // Persistent goals span levels; as stock, only a save to file keeps them (0x004B44C9)
    if (with_goals) {
        lvl.goals = capture_goals();
    }
    return lvl;
}

void asg::capture_player(rf::Player& player, PlayerState& out)
{
    out = {};
    rf::Entity* entity = rf::entity_from_handle(player.entity_handle);
    out.entity_uid = entity ? entity->uid : uid_none;
    out.player_flags = player.flags;
    for (int i = 0; i < static_cast<int>(std::size(player.key_items)); ++i) {
        if (player.key_items[i]) {
            out.key_items.push_back(i);
        }
    }
    out.view_obj_uid = uid_from_handle(player.view_from_handle);
    for (int weapon_type : player.weapon_prefs) {
        out.weapon_prefs.push_back(weapon_name(weapon_type));
    }
    out.fpgun_pos = player.fpgun_data.fpgun_pos;
    out.fpgun_orient = player.fpgun_data.fpgun_orient;
    out.spew_vector_index = player.spew_vector_index;
    out.spew_pos = player.spew_pos;
    out.grenade_mode = static_cast<uint8_t>(player.fpgun_data.grenade_mode);
    out.show_silencer = player.fpgun_data.show_silencer;
    out.remote_charge_in_hand = player.fpgun_data.remote_charge_in_hand;
    out.undercover_active = entity && (entity->ai.ai_flags & rf::AIF_UNDERCOVER);
    out.undercover_team = rf::player_cover_id & 1;
    out.undercover_cover_id = rf::player_cover_id;
    out.ai_high_flag = out.undercover_active;
    out.collides_with_world = player.collides_with_world;
    if (entity) {
        out.vehicle_uid = uid_from_handle(entity->host_handle);
        out.vehicle_seat = out.vehicle_uid != uid_none ? entity->host_tag_handle : -1;
    }
    out.blackout_active = player.death_fade_alpha > 0.0f;
    out.blackout_alpha = player.death_fade_alpha;
    out.blackout_time_sec = player.death_fade_time_sec;
}

void asg::capture_game(GameState& out)
{
    out = {};
    out.level_time = rf::level.global_time;
    out.level_time_left = rf::level.time_left;
    out.messages = hud_message_log();
    out.newest_message_index = static_cast<int32_t>(out.messages.size()) - 1;
    for (const auto& m : out.messages) {
        out.messages_total_height += m.display_height;
    }
    // As stock: from a menu the music is the one that was playing when gameplay was left
    const char* music = rf::gameseq_get_state() == rf::GS_GAMEPLAY ? rf::snd_music_get_current_filename()
                                                                   : rf::gameplay_left_music_filename;
    out.music = music ? music : "";
}
