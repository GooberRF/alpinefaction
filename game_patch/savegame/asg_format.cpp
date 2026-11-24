#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <new>
#include <stdexcept>
#include <xxhash.h>
#include <zlib.h>
#include <xlog/xlog.h>
#include "asg_format.h"

namespace asg
{
    void warn_non_finite(const char* chunk, const char* field, int32_t uid)
    {
        xlog::warn("[ASG] Non-finite {} in chunk {} (uid {}) written as 0", field ? field : "float", chunk, uid);
    }

    void append_chunk(GrowableByteWriter& out, uint32_t id, uint8_t flags, std::span<const uint8_t> payload,
                      uint8_t version)
    {
        out.u32(id);
        out.u8(version);
        out.u8(flags);
        out.u32(static_cast<uint32_t>(payload.size()));
        out.bytes(payload.data(), payload.size());
    }

    void seal_file(std::vector<uint8_t>& file)
    {
        if (file.size() < file_header_size) {
            return;
        }
        uint16_t header_size = 0;
        std::memcpy(&header_size, file.data() + 6, sizeof(header_size));
        if (header_size < file_header_size || header_size > file.size()) {
            return;
        }
        const auto file_size = static_cast<uint32_t>(file.size());
        const uint64_t hash = XXH64(file.data() + header_size, file.size() - header_size, 0);
        std::memcpy(file.data() + 8, &file_size, sizeof(file_size));
        std::memcpy(file.data() + 12, &hash, sizeof(hash));
    }
}

namespace
{
    namespace a = asg;

    // Emission order of LEVL sub-chunks
    constexpr std::array level_chunk_ids{
        a::id_lhdr, a::id_room, a::id_geo1, a::id_geo2, a::id_dbrk, a::id_ents, a::id_item,
        a::id_clut, a::id_trig, a::id_movr, a::id_weap, a::id_corp, a::id_evnt, a::id_decl,
        a::id_blod, a::id_emit, a::id_push, a::id_lite, a::id_goal, a::id_wrld, a::id_alpo,
    };

    constexpr size_t max_uncompressed_size = 64u * 1024 * 1024;
    // Deflate cannot expand data further than about 1032:1
    constexpr size_t max_compression_ratio = 1032;
    // Memory the decoded model may take, all levels together
    constexpr size_t max_decoded_size = 256u * 1024 * 1024;
    constexpr size_t max_header_chunk_size = 64 * 1024;

    struct FileHeader
    {
        uint16_t version = 0;
        uint16_t header_size = 0;
        uint32_t file_size = 0;
        uint64_t hash = 0;
    };
    // After the 4-byte magic
    static_assert(4 + sizeof(FileHeader) == asg::file_header_size);

    // False if the data is shorter than the file header or lacks the magic
    bool parse_file_header(std::span<const uint8_t> data, FileHeader& out)
    {
        ByteReader r{data.data(), data.size()};
        char magic[4]{};
        r.bytes(magic, sizeof(magic));
        r.get(out.version);
        r.get(out.header_size);
        r.get(out.file_size);
        r.get(out.hash);
        return r.ok() && std::memcmp(magic, "AFSG", 4) == 0;
    }

    std::string fourcc_name(uint32_t id)
    {
        std::string s(4, '?');
        for (int i = 0; i < 4; ++i) {
            const auto c = static_cast<char>((id >> (i * 8)) & 0xFF);
            if (c >= 0x20 && c < 0x7F) {
                s[i] = c;
            }
        }
        return s;
    }

    // ─── Field layouts (shared by encode and decode) ───

    template<typename Io, typename R>
    void header_fields(Io& io, R& r)
    {
        io.str(r.writer_version, "writer_version");
        io.str(r.mod_name, "mod_name");
        io.str(r.current_level, "current_level");
        io.str(r.level_title, "level_title");
        io.u8(r.difficulty, "difficulty");
        io.f32(r.game_time, "game_time");
        io.u64(r.saved_at, "saved_at");
    }

    template<typename Io, typename R>
    void game_fields(Io& io, R& r)
    {
        io.f32(r.level_time, "level_time");
        io.f32(r.level_time_left, "level_time_left");
        io.i32(r.newest_message_index, "newest_message_index");
        io.i32(r.messages_total_height, "messages_total_height");
        io.list("messages", r.messages, [](auto& io, auto& m) {
            io.str(m.text, "text");
            io.i32(m.time_string, "time_string");
            io.i16(m.persona, "persona");
            io.i16(m.display_height, "display_height");
        });
        io.str(r.music, "music");
    }

    template<typename Io, typename R>
    void player_fields(Io& io, R& r)
    {
        io.i32(r.entity_uid, "entity_uid");
        io.u32(r.player_flags, "player_flags");
        io.i32_list(r.key_items, "key_items");
        io.i32(r.view_obj_uid, "view_obj_uid");
        io.str_list(r.weapon_prefs, "weapon_prefs");
        io.v3(r.fpgun_pos, "fpgun_pos");
        io.m3(r.fpgun_orient, "fpgun_orient");
        io.u8(r.spew_vector_index, "spew_vector_index");
        io.v3(r.spew_pos, "spew_pos");
        io.u8(r.grenade_mode, "grenade_mode");
        io.b(r.show_silencer, "show_silencer");
        io.b(r.remote_charge_in_hand, "remote_charge_in_hand");
        io.b(r.undercover_active, "undercover_active");
        io.i32(r.undercover_team, "undercover_team");
        io.i32(r.undercover_cover_id, "undercover_cover_id");
        io.b(r.ai_high_flag, "ai_high_flag");
        io.b(r.collides_with_world, "collides_with_world");
        io.i32(r.vehicle_uid, "vehicle_uid");
        io.i32(r.vehicle_seat, "vehicle_seat");
        io.b(r.blackout_active, "blackout_active");
        io.f32(r.blackout_alpha, "blackout_alpha");
        io.f32(r.blackout_time_sec, "blackout_time_sec");
    }

    template<typename Io, typename R>
    void base_fields(Io& io, R& r)
    {
        io.base([&](auto& io) {
            io.i32(r.uid, "uid");
            io.u8(r.origin, "origin");
            io.str(r.cls, "cls");
            io.i32(r.parent_uid, "parent_uid");
            io.i32(r.host_uid, "host_uid");
            io.v3(r.pos, "pos");
            io.m3(r.orient, "orient");
            io.v3(r.vel, "vel");
            io.v3(r.ang_momentum, "ang_momentum");
            io.f32(r.life, "life");
            io.f32(r.armor, "armor");
            io.u32(r.obj_flags, "obj_flags");
            io.u32(r.physics_flags, "physics_flags");
            io.i32(r.friendliness, "friendliness");
            io.i32(r.host_tag_handle, "host_tag_handle");
            io.str(r.skin, "skin");
        });
    }

    template<typename Io, typename R>
    void entity_fields(Io& io, R& r)
    {
        base_fields(io, r.base);
        io.str(r.primary_weapon, "primary_weapon");
        io.str(r.secondary_weapon, "secondary_weapon");
        io.list("weapons", r.weapons, [](auto& io, auto& w) {
            io.str(w.weapon, "weapon");
            io.i32(w.clip_ammo, "clip_ammo");
            io.i32(w.ammo, "ammo");
            io.b(w.possessed, "possessed");
            io.b(w.custom_mode, "custom_mode");
            io.b(w.silencer, "silencer");
        });
        io.i32_list(r.hate_list, "hate_list");
        io.i32(r.ai_mode, "ai_mode");
        io.i32(r.ai_submode, "ai_submode");
        io.i32(r.move_mode, "move_mode");
        io.i32(r.ai_mode_parm_0, "ai_mode_parm_0");
        io.i32(r.ai_mode_parm_1, "ai_mode_parm_1");
        io.i32(r.target_uid, "target_uid");
        io.i32(r.look_at_uid, "look_at_uid");
        io.i32(r.shoot_at_uid, "shoot_at_uid");
        io.u32(r.ai_flags, "ai_flags");
        io.f32(r.ai_healing_left, "ai_healing_left");
        io.v3(r.ai_steering_vector, "ai_steering_vector");
        io.i32(r.corpse_carry_uid, "corpse_carry_uid");
        for (auto& idx : r.path_node_indices) {
            io.i16(idx, "path_node_indices");
        }
        io.i16(r.path_previous_goal, "path_previous_goal");
        io.i16(r.path_current_goal, "path_current_goal");
        io.v3(r.path_end_pos, "path_end_pos");
        io.i16(r.path_adjacent_node1_index, "path_adjacent_node1_index");
        io.i16(r.path_adjacent_node2_index, "path_adjacent_node2_index");
        io.i16(r.path_waypoint_list_index, "path_waypoint_list_index");
        io.i16(r.path_goal_waypoint_index, "path_goal_waypoint_index");
        io.u8(r.path_direction, "path_direction");
        io.u8(r.path_flags, "path_flags");
        io.v3(r.path_turn_towards_pos, "path_turn_towards_pos");
        io.i32(r.path_follow_style, "path_follow_style");
        io.v3(r.path_start_pos, "path_start_pos");
        io.v3(r.path_goal_pos, "path_goal_pos");
        io.v3(r.ci_rot, "ci_rot");
        io.v3(r.ci_move, "ci_move");
        io.b(r.ci_time_relative_mouse_delta_disabled, "ci_time_relative_mouse_delta_disabled");
        io.v3(r.control_phb, "control_phb");
        io.v3(r.control_eye_phb, "control_eye_phb");
        io.v3(r.control_local_vel, "control_local_vel");
        io.v3(r.eye_pos, "eye_pos");
        io.m3(r.eye_orient, "eye_orient");
        io.u32(r.entity_flags, "entity_flags");
        io.u32(r.entity_flags2, "entity_flags2");
        io.i16(r.current_state_anim, "current_state_anim");
        io.i16(r.next_state_anim, "next_state_anim");
        io.i16(r.last_custom_anim_index, "last_custom_anim_index");
        io.i16(r.state_anim_22_index, "state_anim_22_index");
        io.f32(r.total_transition_time, "total_transition_time");
        io.f32(r.elapsed_transition_time, "elapsed_transition_time");
        io.i32(r.driller_rot_angle, "driller_rot_angle");
        io.f32(r.driller_rot_speed, "driller_rot_speed");
        io.i32(r.driller_geomod_count, "driller_geomod_count");
        io.u8(r.current_speed, "current_speed");
        io.b(r.on_fire, "on_fire");
        io.i32(r.climb_region_index, "climb_region_index");
        io.str(r.item_drop_override, "item_drop_override");
    }

    template<typename Io, typename R>
    void item_fields(Io& io, R& r)
    {
        base_fields(io, r.base);
        io.i32(r.respawn_time_ms, "respawn_time_ms");
        io.i32(r.respawn_timer, "respawn_timer");
        io.f32(r.alpha, "alpha");
        io.f32(r.create_time, "create_time");
        io.u32(r.item_flags, "item_flags");
    }

    template<typename Io, typename R>
    void clutter_fields(Io& io, R& r)
    {
        base_fields(io, r.base);
        io.i32(r.delayed_kill_timer, "delayed_kill_timer");
        io.i32(r.delayed_kill_sound, "delayed_kill_sound");
        io.i32(r.emitter_kill_timer, "emitter_kill_timer");
        io.i32(r.corpse_create_timer, "corpse_create_timer");
        io.i32(r.skin_index, "skin_index");
        io.b(r.hidden, "hidden");
    }

    template<typename Io, typename R>
    void trigger_fields(Io& io, R& r)
    {
        base_fields(io, r.base);
        io.i32(r.count, "count");
        io.f32(r.time_last_activated, "time_last_activated");
        io.u32(r.trigger_flags, "trigger_flags");
        io.i32(r.activator_uid, "activator_uid");
        io.i32(r.button_active_timer, "button_active_timer");
        io.i32(r.inside_timer, "inside_timer");
        io.i32(r.reset_timer, "reset_timer");
    }

    template<typename Io, typename R>
    void mover_fields(Io& io, R& r)
    {
        base_fields(io, r.base);
        io.f32(r.rot_cur_pos, "rot_cur_pos");
        io.i32(r.start_at_keyframe, "start_at_keyframe");
        io.i32(r.stop_at_keyframe, "stop_at_keyframe");
        io.i32(r.stop_completely_at_keyframe, "stop_completely_at_keyframe");
        io.u32(r.mover_flags, "mover_flags");
        io.f32(r.travel_time_seconds, "travel_time_seconds");
        io.f32(r.rotation_travel_time_seconds, "rotation_travel_time_seconds");
        io.i32(r.wait_timer, "wait_timer");
        io.i32(r.trigger_uid, "trigger_uid");
        io.f32(r.dist_travelled, "dist_travelled");
        io.f32(r.cur_vel, "cur_vel");
    }

    template<typename Io, typename R>
    void weapon_fields(Io& io, R& r)
    {
        base_fields(io, r.base);
        io.f32(r.life_left_seconds, "life_left_seconds");
        io.u32(r.weapon_flags, "weapon_flags");
        io.i32(r.sticky_host_uid, "sticky_host_uid");
        io.v3(r.sticky_host_pos_offset, "sticky_host_pos_offset");
        io.m3(r.sticky_host_orient, "sticky_host_orient");
        io.u8(r.weap_friendliness, "weap_friendliness");
        io.i32(r.target_uid, "target_uid");
        io.f32(r.pierce_power_left, "pierce_power_left");
        io.f32(r.thrust_left, "thrust_left");
        io.v3(r.firing_pos, "firing_pos");
    }

    template<typename Io, typename R>
    void corpse_fields(Io& io, R& r)
    {
        base_fields(io, r.base);
        io.f32(r.create_time, "create_time");
        io.f32(r.lifetime_seconds, "lifetime_seconds");
        io.u32(r.corpse_flags, "corpse_flags");
        io.str(r.entity_cls, "entity_cls");
        io.i32(r.source_entity_uid, "source_entity_uid");
        io.str(r.mesh_name, "mesh_name");
        io.i32(r.emitter_kill_timer, "emitter_kill_timer");
        io.f32(r.body_temp, "body_temp");
        io.i32(r.state_anim, "state_anim");
        io.i32(r.action_anim, "action_anim");
        io.i32(r.drop_anim, "drop_anim");
        io.i32(r.carry_anim, "carry_anim");
        io.i32(r.corpse_pose, "corpse_pose");
        io.str(r.helmet_name, "helmet_name");
        io.i32(r.item_uid, "item_uid");
        io.f32(r.mass, "mass");
        io.f32(r.radius, "radius");
        io.list("cspheres", r.cspheres, [](auto& io, auto& s) {
            io.v3(s.center, "center");
            io.f32(s.radius, "radius");
            io.f32(s.spring_const, "spring_const");
            io.i32(s.spring_length, "spring_length");
        });
        const auto bone_fields = [](auto& io, auto& bone) {
            io.m3(bone.orient, "orient");
            io.v3(bone.pos, "pos");
        };
        io.list("ragdoll_bodies", r.ragdoll_bodies, bone_fields);
        io.list("ragdoll_bones", r.ragdoll_bones, bone_fields);
    }

    template<typename Io, typename R>
    void event_fields(Io& io, R& r)
    {
        io.i32(r.uid, "uid");
        io.u8(r.origin, "origin");
        io.i32(r.event_type, "event_type");
        io.b(r.enabled, "enabled");
        io.b(r.on_state, "on_state");
        io.i32(r.delay, "delay");
        io.f32(r.delay_seconds, "delay_seconds");
        io.i32(r.activated_by, "activated_by");
        io.i32(r.triggered_by, "triggered_by");
        io.list("links", r.links, [](auto& io, auto& link) {
            io.u8(link.kind, "kind");
            io.i32(link.uid, "uid");
        });
        io.blob(r.payload, "payload");
    }

    template<typename Io, typename R>
    void decal_fields(Io& io, R& r)
    {
        io.v3(r.pos, "pos");
        io.m3(r.orient, "orient");
        io.v3(r.extents, "extents");
        io.u32(r.decal_flags, "decal_flags");
        io.u8(r.alpha, "alpha");
        io.f32(r.tiling_scale, "tiling_scale");
        io.str(r.bitmap, "bitmap");
    }

    template<typename Io, typename R>
    void blood_pool_fields(Io& io, R& r)
    {
        io.v3(r.pos, "pos");
        io.m3(r.orient, "orient");
        io.u8(r.red, "red");
        io.u8(r.green, "green");
        io.u8(r.blue, "blue");
        io.u8(r.alpha, "alpha");
    }

    template<typename Io, typename R>
    void crater_fields(Io& io, R& r)
    {
        io.u16(r.shape_index, "shape_index");
        io.u16(r.flags, "flags");
        io.i32(r.room_index, "room_index");
        for (auto& v : r.pos) {
            io.i16(v, "pos");
        }
        for (auto& v : r.hit_normal) {
            io.i16(v, "hit_normal");
        }
        for (auto& v : r.orient) {
            io.i16(v, "orient");
        }
        io.f32(r.scale, "scale");
    }

    template<typename Io, typename R>
    void rf2_geomod_fields(Io& io, R& r)
    {
        io.i32(r.shape_index, "shape_index");
        io.i32(r.room_index, "room_index");
        io.v3(r.pos, "pos");
        io.m3(r.orient, "orient");
        io.i32(r.flags, "flags");
        io.f32(r.scale, "scale");
        io.v3(r.hit_normal, "hit_normal");
        io.v3(r.field_4c, "field_4c");
        io.v3(r.field_58, "field_58");
    }

    template<typename Io, typename R>
    void liquid_depth_fields(Io& io, R& r)
    {
        io.i32(r.room_uid, "room_uid");
        io.f32(r.depth, "depth");
        io.b(r.has_pending, "has_pending");
        io.f32(r.target_depth, "target_depth");
        io.f32(r.remaining_duration, "remaining_duration");
    }

    // Writing: returns false if the chunk is absent. Reading: marks it present (fails on a duplicate).
    template<typename Io, typename O, typename F>
    bool opt_chunk(Io& io, O& o, F&& f)
    {
        if constexpr (Io::reading) {
            if (o) {
                io.fail("duplicate chunk");
                return true;
            }
            f(io, o.emplace());
        }
        else {
            if (!o) {
                return false;
            }
            f(io, *o);
        }
        return true;
    }

    template<typename Io, typename O, typename F>
    bool opt_list_chunk(Io& io, O& o, F&& f)
    {
        return opt_chunk(io, o, [&](auto& io, auto& v) { io.list(v, f); });
    }

    // Payload of one LEVL sub-chunk. Returns false for an unknown id or (writing) an absent chunk.
    template<typename Io, typename L>
    bool level_chunk(Io& io, uint32_t id, L& lvl)
    {
        switch (id) {
            case asg::id_lhdr:
                io.str(lvl.filename, "filename");
                io.u32(lvl.rfl_checksum, "rfl_checksum");
                io.f32(lvl.level_time, "level_time");
                return true;
            case asg::id_room:
                return opt_chunk(io, lvl.rooms, [](auto& io, auto& rooms) {
                    io.i32_list(rooms.killed_room_uids, "killed_room_uids");
                    io.list("liquid_depths", rooms.liquid_depths,
                            [](auto& io, auto& r) { liquid_depth_fields(io, r); });
                    io.v3(rooms.world_bbox_min, "world_bbox_min");
                    io.v3(rooms.world_bbox_max, "world_bbox_max");
                });
            case asg::id_geo1:
                return opt_list_chunk(io, lvl.craters, [](auto& io, auto& r) { crater_fields(io, r); });
            case asg::id_geo2:
                return opt_list_chunk(io, lvl.rf2_geomods, [](auto& io, auto& r) { rf2_geomod_fields(io, r); });
            case asg::id_dbrk:
                return opt_list_chunk(io, lvl.breakable_rooms, [](auto& io, auto& r) {
                    io.i32(r.room_uid, "room_uid");
                    io.f32(r.life, "life");
                });
            case asg::id_ents:
                io.list("entities", lvl.entities.entities, [](auto& io, auto& r) { entity_fields(io, r); });
                io.i32_list(lvl.entities.dead_uids, "dead_uids");
                return true;
            case asg::id_item:
                return opt_list_chunk(io, lvl.items, [](auto& io, auto& r) { item_fields(io, r); });
            case asg::id_clut:
                return opt_list_chunk(io, lvl.clutter, [](auto& io, auto& r) { clutter_fields(io, r); });
            case asg::id_trig:
                return opt_list_chunk(io, lvl.triggers, [](auto& io, auto& r) { trigger_fields(io, r); });
            case asg::id_movr:
                return opt_list_chunk(io, lvl.movers, [](auto& io, auto& r) { mover_fields(io, r); });
            case asg::id_weap:
                return opt_list_chunk(io, lvl.weapons, [](auto& io, auto& r) { weapon_fields(io, r); });
            case asg::id_corp:
                return opt_list_chunk(io, lvl.corpses, [](auto& io, auto& r) { corpse_fields(io, r); });
            case asg::id_evnt:
                return opt_list_chunk(io, lvl.events, [](auto& io, auto& r) { event_fields(io, r); });
            case asg::id_decl:
                return opt_list_chunk(io, lvl.decals, [](auto& io, auto& r) { decal_fields(io, r); });
            case asg::id_blod:
                return opt_list_chunk(io, lvl.blood_pools, [](auto& io, auto& r) { blood_pool_fields(io, r); });
            case asg::id_emit:
                return opt_list_chunk(io, lvl.emitters, [](auto& io, auto& r) {
                    io.u8(r.kind, "kind");
                    io.i32(r.uid, "uid");
                    io.b(r.active, "active");
                    io.f32(r.time_to_change, "time_to_change");
                    io.f32(r.current_state_time, "current_state_time");
                });
            case asg::id_push:
                return opt_list_chunk(io, lvl.push_regions, [](auto& io, auto& r) {
                    io.i32(r.uid, "uid");
                    io.b(r.active, "active");
                });
            case asg::id_lite:
                return opt_list_chunk(io, lvl.lights, [](auto& io, auto& r) {
                    io.i32(r.uid, "uid");
                    io.b(r.on, "on");
                    io.v3(r.color, "color");
                    io.v3(r.hue, "hue");
                });
            case asg::id_goal:
                return opt_list_chunk(io, lvl.goals, [](auto& io, auto& r) {
                    io.str(r.name, "name");
                    io.i32(r.count, "count");
                    io.i32(r.initial_count, "initial_count");
                });
            case asg::id_wrld:
                return opt_list_chunk(io, lvl.world, [](auto& io, auto& r) {
                    io.u32(r.tag, "tag");
                    io.blob(r.data, "data");
                });
            case asg::id_alpo:
                return opt_list_chunk(io, lvl.alpine_objects, [](auto& io, auto& r) {
                    io.u8(r.kind, "kind");
                    io.i32(r.uid, "uid");
                    io.blob(r.payload, "payload");
                });
            default:
                return false;
        }
    }

    bool is_required_level_chunk(uint32_t id)
    {
        return id == asg::id_lhdr || id == asg::id_ents;
    }

    std::vector<uint8_t> compress_payload(const std::vector<uint8_t>& src)
    {
        uLongf dst_len = compressBound(static_cast<uLong>(src.size()));
        std::vector<uint8_t> out(4 + dst_len);
        const auto size = static_cast<uint32_t>(src.size());
        std::memcpy(out.data(), &size, sizeof(size));
        if (compress2(out.data() + 4, &dst_len, src.data(), static_cast<uLong>(src.size()), Z_DEFAULT_COMPRESSION)
            != Z_OK) {
            return {};
        }
        out.resize(4 + dst_len);
        return out;
    }

    bool decompress_payload(std::span<const uint8_t> src, std::vector<uint8_t>& out, std::string& error)
    {
        uint32_t size = 0;
        if (src.size() < sizeof(size)) {
            error = "compressed chunk too small";
            return false;
        }
        std::memcpy(&size, src.data(), sizeof(size));
        if (size > max_uncompressed_size || size / max_compression_ratio > src.size() - 4) {
            error = "compressed chunk too large";
            return false;
        }
        out.resize(size);
        uLongf dst_len = size;
        // zlib rejects a null destination even for zero bytes
        Bytef dummy = 0;
        const int rc = uncompress(size ? out.data() : &dummy, &dst_len, src.data() + 4,
                                  static_cast<uLong>(src.size() - 4));
        if (rc != Z_OK || dst_len != size) {
            error = "failed to decompress chunk";
            return false;
        }
        return true;
    }

    struct ChunkView
    {
        uint32_t id;
        uint8_t version;
        uint8_t flags;
        std::span<const uint8_t> payload;
    };

    // Walks a chunk sequence. `on_chunk(const ChunkView&, bool known)` returns false to stop.
    template<typename IsKnown, typename F>
    bool walk_chunks(std::span<const uint8_t> data, IsKnown&& is_known, F&& on_chunk, std::string& error)
    {
        ByteReader r{data.data(), data.size()};
        while (r.remaining() > 0) {
            ChunkView c{};
            uint32_t size = 0;
            if (r.remaining() < asg::chunk_header_size || !r.get(c.id) || !r.get(c.version) || !r.get(c.flags)
                || !r.get(size)) {
                error = "truncated chunk header";
                return false;
            }
            if (size > r.remaining()) {
                error = std::format("chunk {} overruns its parent", fourcc_name(c.id));
                return false;
            }
            c.payload = {r.cur(), size};
            r.skip(size);
            const bool reserved_flags = (c.flags & ~(asg::chunk_compressed | asg::chunk_required)) != 0;
            if (!is_known(c.id) || c.version != 1 || reserved_flags) {
                if (c.flags & asg::chunk_required) {
                    error = std::format("unsupported required chunk {} (version {}, flags {:#x})",
                                        fourcc_name(c.id), c.version, c.flags);
                    return false;
                }
                xlog::warn("[ASG] Skipping unsupported chunk {} (version {}, flags {:#x})", fourcc_name(c.id),
                           c.version, c.flags);
                continue;
            }
            if (!on_chunk(c)) {
                return false;
            }
        }
        return true;
    }

    // Runs a field function over a known chunk's payload, decompressing it first if needed
    template<typename F>
    bool read_chunk(const ChunkView& c, size_t& budget, std::string& error, F&& f)
    {
        std::vector<uint8_t> inflated;
        std::span<const uint8_t> payload = c.payload;
        if (c.flags & asg::chunk_compressed) {
            if (!decompress_payload(c.payload, inflated, error)) {
                return false;
            }
            payload = inflated;
        }
        std::string field_error;
        a::Reader reader{payload, &field_error, true, &budget};
        f(reader, payload);
        if (!reader.ok()) {
            error = std::format("chunk {}: {}", fourcc_name(c.id), field_error);
            return false;
        }
        return true;
    }

    bool decode_level(const ChunkView& levl, a::LevelState& lvl, size_t& budget, std::string& error)
    {
        return read_chunk(levl, budget, error, [&](a::Reader& outer, std::span<const uint8_t> payload) {
            bool first = true;
            bool has_lhdr = false;
            bool has_ents = false;
            const auto is_known = [](uint32_t id) {
                return std::find(level_chunk_ids.begin(), level_chunk_ids.end(), id) != level_chunk_ids.end();
            };
            const bool walked = walk_chunks(payload, is_known, [&](const ChunkView& c) {
                if (first != (c.id == asg::id_lhdr)) {
                    error = "LHDR must be the first level chunk";
                    return false;
                }
                first = false;
                bool* seen = c.id == asg::id_lhdr ? &has_lhdr : c.id == asg::id_ents ? &has_ents : nullptr;
                if (seen) {
                    if (*seen) {
                        error = std::format("duplicate chunk {}", fourcc_name(c.id));
                        return false;
                    }
                    *seen = true;
                }
                return read_chunk(c, budget, error, [&](a::Reader& r, std::span<const uint8_t>) {
                    level_chunk(r, c.id, lvl);
                });
            }, error);
            if (walked && (!has_lhdr || !has_ents)) {
                error = "level is missing LHDR or ENTS";
            }
            if (!walked || !has_lhdr || !has_ents) {
                outer.fail(error);
            }
        });
    }
}

std::optional<std::vector<uint8_t>> asg_encode(const asg::SaveModel& m)
{
    asg::Writer game{"GAME"};
    game_fields(game, m.game);
    asg::Writer player{"PLYR"};
    player_fields(player, m.player);

    std::vector<std::vector<uint8_t>> levels;
    for (const auto& lvl : m.levels) {
        GrowableByteWriter sub;
        for (uint32_t id : level_chunk_ids) {
            const std::string chunk = fourcc_name(id);
            asg::Writer w{chunk.c_str()};
            if (level_chunk(w, id, lvl)) {
                asg::append_chunk(sub, id, is_required_level_chunk(id) ? asg::chunk_required : 0, w.buffer());
            }
        }
        levels.push_back(compress_payload(sub.buffer()));
        if (levels.back().empty()) {
            xlog::error("[ASG] Cannot compress level {}", lvl.filename);
            return std::nullopt;
        }
    }

    asg::Writer hdr{"HDR"};
    header_fields(hdr, m.header);

    GrowableByteWriter out;
    out.bytes("AFSG", 4);
    out.u16(asg::format_version);
    out.u16(asg::file_header_size);
    out.u32(0);
    out.put(uint64_t{0});
    asg::append_chunk(out, asg::id_hdr, asg::chunk_required, hdr.buffer());
    asg::append_chunk(out, asg::id_game, asg::chunk_required, game.buffer());
    asg::append_chunk(out, asg::id_plyr, asg::chunk_required, player.buffer());
    for (const auto& lvl : levels) {
        asg::append_chunk(out, asg::id_levl, asg::chunk_compressed | asg::chunk_required, lvl);
    }
    auto file = out.take();
    asg::seal_file(file);
    return file;
}

static bool decode_save(std::span<const uint8_t> file, asg::SaveModel& out, std::string& error)
{
    FileHeader fh;
    if (!parse_file_header(file, fh)) {
        error = "not an Alpine savegame";
        return false;
    }
    if (fh.version != asg::format_version) {
        error = std::format("unsupported savegame version {}", fh.version);
        return false;
    }
    const uint16_t header_size = fh.header_size;
    if (header_size < asg::file_header_size || header_size > file.size() || fh.file_size != file.size()) {
        error = "savegame is truncated or has a bad size";
        return false;
    }
    if (XXH64(file.data() + header_size, file.size() - header_size, 0) != fh.hash) {
        error = "savegame is corrupt (hash mismatch)";
        return false;
    }

    const auto is_known = [](uint32_t id) {
        return id == asg::id_hdr || id == asg::id_game || id == asg::id_plyr || id == asg::id_levl;
    };
    size_t budget = max_decoded_size;
    bool has_hdr = false;
    bool has_game = false;
    bool has_plyr = false;
    const bool ok = walk_chunks(file.subspan(header_size), is_known, [&](const ChunkView& c) {
        if (has_hdr == (c.id == asg::id_hdr)) {
            error = "HDR must be the first chunk";
            return false;
        }
        has_hdr = true;
        if ((c.id == asg::id_game && has_game) || (c.id == asg::id_plyr && has_plyr)) {
            error = std::format("duplicate chunk {}", fourcc_name(c.id));
            return false;
        }
        switch (c.id) {
            case asg::id_hdr:
                // The menu reads HDR without decompressing, and the writer never compresses it
                if (c.flags & asg::chunk_compressed) {
                    error = "HDR must not be compressed";
                    return false;
                }
                return read_chunk(c, budget, error, [&](asg::Reader& r, auto) {
                    header_fields(r, out.header);
                });
            case asg::id_game:
                has_game = true;
                return read_chunk(c, budget, error, [&](asg::Reader& r, auto) {
                    game_fields(r, out.game);
                });
            case asg::id_plyr:
                has_plyr = true;
                return read_chunk(c, budget, error, [&](asg::Reader& r, auto) {
                    player_fields(r, out.player);
                });
            case asg::id_levl:
                return decode_level(c, out.levels.emplace_back(), budget, error);
            default:
                return false;
        }
    }, error);
    if (!ok) {
        return false;
    }
    if (!has_hdr || !has_game || !has_plyr) {
        error = "savegame is missing a required chunk";
        return false;
    }
    return true;
}

bool asg_decode(std::span<const uint8_t> file, asg::SaveModel& out, std::string& error)
{
    out = {};
    error.clear();
    try {
        if (decode_save(file, out, error)) {
            return true;
        }
    }
    catch (const std::bad_alloc&) {
        error = "not enough memory to load the savegame";
    }
    catch (const std::length_error&) {
        error = "savegame is too large to load";
    }
    out = {};
    return false;
}

bool asg_read_header(const std::filesystem::path& path, asg::Header& out)
{
    std::ifstream f{path, std::ios::binary};
    std::array<uint8_t, asg::file_header_size> fh_bytes{};
    if (!f.read(reinterpret_cast<char*>(fh_bytes.data()), fh_bytes.size())) {
        return false;
    }
    FileHeader fh;
    if (!parse_file_header(fh_bytes, fh) || fh.version != asg::format_version
        || fh.header_size < asg::file_header_size || fh.header_size > fh.file_size) {
        return false;
    }
    std::array<uint8_t, asg::chunk_header_size> ch{};
    if (!f.seekg(fh.header_size) || !f.read(reinterpret_cast<char*>(ch.data()), ch.size())) {
        return false;
    }
    uint32_t id = 0;
    uint32_t size = 0;
    std::memcpy(&id, ch.data(), sizeof(id));
    std::memcpy(&size, ch.data() + 6, sizeof(size));
    if (id != asg::id_hdr || ch[4] != 1 || (ch[5] & ~asg::chunk_required) != 0 || size > max_header_chunk_size) {
        return false;
    }
    std::vector<uint8_t> payload(size);
    if (!f.read(reinterpret_cast<char*>(payload.data()), size)) {
        return false;
    }
    asg::Header header;
    asg::Reader r{payload, nullptr};
    header_fields(r, header);
    if (!r.ok()) {
        return false;
    }
    out = std::move(header);
    return true;
}
