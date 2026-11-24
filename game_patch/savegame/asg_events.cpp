#include <algorithm>
#include <chrono>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <xlog/xlog.h>
#include "asg_format.h"
#include "asg_internal.h"
#include "../object/event_alpine.h"
#include "../os/os.h"
#include "../rf/bmpman.h"
#include "../rf/event.h"
#include "../rf/object.h"

// ─── PayloadIo ───

namespace
{
    template<typename T, typename R>
    bool read_field(asg::Reader& r, T& v, R&& read)
    {
        const size_t before = r.remaining();
        read(v);
        return r.ok() && r.remaining() != before;
    }
}

void asg::warn_bad_payload(const char* what, int32_t id, const std::string& error)
{
    xlog::warn("[ASG] Bad payload for {} {}: {}", what, id, error);
}

void asg::PayloadIo::b(bool& v, const char*)
{
    if (capturing()) {
        m_w->b(v);
        return;
    }
    bool x = v;
    if (read_field(*m_r, x, [&](bool& o) { m_r->b(o); })) {
        v = x;
    }
}

void asg::PayloadIo::u8(uint8_t& v, const char*)
{
    if (capturing()) {
        m_w->u8(v);
        return;
    }
    uint8_t x = v;
    if (read_field(*m_r, x, [&](uint8_t& o) { m_r->u8(o); })) {
        v = x;
    }
}

void asg::PayloadIo::i32(int& v, const char*)
{
    if (capturing()) {
        m_w->i32(v);
        return;
    }
    int32_t x = v;
    if (read_field(*m_r, x, [&](int32_t& o) { m_r->i32(o); })) {
        v = x;
    }
}

void asg::PayloadIo::f32(float& v, const char* n)
{
    if (capturing()) {
        m_w->f32(v, n);
        return;
    }
    float x = v;
    if (read_field(*m_r, x, [&](float& o) { m_r->f32(o); })) {
        v = x;
    }
}

void asg::PayloadIo::str(std::string& v, const char*)
{
    if (capturing()) {
        m_w->str(v);
        return;
    }
    std::string x;
    if (read_field(*m_r, x, [&](std::string& o) { m_r->str(o); })) {
        v = std::move(x);
    }
}

void asg::PayloadIo::v3(rf::Vector3& v, const char* n)
{
    const std::string base = n;
    f32(v.x, (base + ".x").c_str());
    f32(v.y, (base + ".y").c_str());
    f32(v.z, (base + ".z").c_str());
}

void asg::PayloadIo::m3(rf::Matrix3& v, const char* n)
{
    const std::string base = n;
    v3(v.rvec, (base + ".rvec").c_str());
    v3(v.uvec, (base + ".uvec").c_str());
    v3(v.fvec, (base + ".fvec").c_str());
}

void asg::PayloadIo::color(rf::gr::Color& v, const char* n)
{
    u8(v.red, n);
    u8(v.green, n);
    u8(v.blue, n);
    u8(v.alpha, n);
}

void asg::PayloadIo::ts(rf::Timestamp& v, const char* n)
{
    int value = capturing() ? capture_timestamp(v) : ts_invalid;
    const bool had = !capturing() && m_r->remaining() > 0;
    i32(value, n);
    if (restoring() && had && m_r->ok()) {
        restore_timestamp(v, value);
    }
}

void asg::PayloadIo::hrt(HighResTimer& v, const char* n)
{
    using std::chrono::milliseconds;
    bool valid = capturing() && v.valid();
    const auto clamp_ms = [](int64_t ms) {
        return static_cast<int>(std::clamp<int64_t>(ms, INT32_MIN, INT32_MAX));
    };
    int duration = valid ? clamp_ms(v.duration_ms()) : 0;
    int elapsed = valid ? clamp_ms(v.time_since_ms()) : 0;
    if (capturing()) {
        m_w->b(valid);
        m_w->i32(duration);
        m_w->i32(elapsed);
        return;
    }
    const bool had = m_r->remaining() > 0;
    b(valid, n);
    i32(duration, n);
    i32(elapsed, n);
    if (had && m_r->ok()) {
        if (valid) {
            v.set_started_ago(milliseconds{duration}, milliseconds{std::max(elapsed, 0)});
        }
        else {
            v.invalidate();
        }
    }
}

void asg::PayloadIo::handle(int& v, const char* n)
{
    int uid = capturing() ? uid_from_handle(v) : uid_none;
    const bool had = !capturing() && m_r->remaining() > 0;
    i32(uid, n);
    if (restoring() && had && m_r->ok() && m_refs) {
        m_refs->add(uid, &v);
    }
}

void asg::PayloadIo::handle_list(std::vector<int>& v, const char* n)
{
    uint32_t count = static_cast<uint32_t>(v.size());
    if (!list_count(count, n)) {
        return;
    }
    if (restoring()) {
        // Sized once: the resolver keeps pointers into it
        v.assign(count, -1);
    }
    for (uint32_t i = 0; i < count && ok(); ++i) {
        if (restoring()) {
            handle(v[i], nullptr);
        }
        else {
            int uid = uid_from_handle(v[i]);
            m_w->i32(uid);
        }
    }
}

bool asg::PayloadIo::list_count(uint32_t& count, const char*)
{
    if (capturing()) {
        m_w->u32(count);
        return true;
    }
    uint32_t x = 0;
    if (!read_field(*m_r, x, [&](uint32_t& o) { m_r->u32(o); })) {
        return false;
    }
    // Every item takes a byte at least
    if (x > m_r->remaining()) {
        m_r->fail("list count exceeds its payload");
        return false;
    }
    count = x;
    return true;
}

// ─── Event payloads ───

namespace
{
    // Per-type event payloads. Alpine events store every parameter their Set_Variable handlers can change, and their
    // progress state. Types without an entry (no state, nothing Set_Variable can change, or multiplayer-only) have
    // an empty payload.
    struct PayloadType
    {
        rf::EventType type;
        void (*fields)(asg::PayloadIo& io, rf::Event& e);
    };

    template<typename E, void (*F)(asg::PayloadIo&, E&)>
    void fields_of(asg::PayloadIo& io, rf::Event& e)
    {
        F(io, static_cast<E&>(e));
    }

    // When_Dead: b message_sent
    void when_dead(asg::PayloadIo& io, rf::WhenDeadEvent& e)
    {
        io.b(e.message_sent, "message_sent");
    }

    // Cyclic_Timer: b active | ts next_fire | i32 send_count
    void cyclic_timer(asg::PayloadIo& io, rf::CyclicTimerEvent& e)
    {
        io.b(e.active, "active");
        io.ts(e.next_fire_timestamp, "next_fire");
        io.i32(e.send_count, "send_count");
    }

    // Make_Invulnerable: ts invulnerable
    void make_invulnerable(asg::PayloadIo& io, rf::MakeInvulnerableEvent& e)
    {
        io.ts(e.invulnerable_timestamp, "invulnerable");
    }

    // Goal_Create: i32 count
    void goal_create(asg::PayloadIo& io, rf::GoalCreateEvent& e)
    {
        io.i32(e.count, "count");
    }

    // Alarm_Siren: b siren_playing
    void alarm_siren(asg::PayloadIo& io, rf::AlarmSirenEvent& e)
    {
        io.b(e.siren_playing, "siren_playing");
    }

    // Set_Liquid_Depth: f32 depth | f32 duration (level fixes such as L5S2's change them)
    void set_liquid_depth(asg::PayloadIo& io, rf::SetLiquidDepthEvent& e)
    {
        io.f32(e.depth, "depth");
        io.f32(e.duration, "duration");
    }

    // Clone_Entity: b hostile_to_player | b find_player | i32 link_from
    void clone_entity(asg::PayloadIo& io, EventCloneEntity& e)
    {
        io.b(e.hostile_to_player, "hostile_to_player");
        io.b(e.find_player, "find_player");
        io.i32(e.link_from, "link_from");
    }

    // Switch_Random: b no_repeats | list<uid> used_handles
    void switch_random(asg::PayloadIo& io, EventSwitchRandom& e)
    {
        io.b(e.no_repeats, "no_repeats");
        io.handle_list(e.used_handles, "used");
    }

    // Difficulty_Gate: i32 difficulty
    void difficulty_gate(asg::PayloadIo& io, EventDifficultyGate& e)
    {
        io.e32(e.difficulty, "difficulty");
    }

    // HUD_Message: str message | f32 duration
    void hud_message(asg::PayloadIo& io, EventHUDMessage& e)
    {
        io.str(e.message, "message");
        io.f32(e.duration, "duration");
    }

    // Play_Video: str filename
    void play_video(asg::PayloadIo& io, EventPlayVideo& e)
    {
        io.str(e.filename, "filename");
    }

    // Set_Level_Hardness: i32 hardness
    void set_level_hardness(asg::PayloadIo& io, EventSetLevelHardness& e)
    {
        io.i32(e.hardness, "hardness");
    }

    // Sequence: i32 next_link_index
    void sequence(asg::PayloadIo& io, EventSequence& e)
    {
        io.i32(e.next_link_index, "next_link_index");
    }

    // Remove_Link: b remove_all
    void remove_link(asg::PayloadIo& io, EventRemoveLink& e)
    {
        io.b(e.remove_all, "remove_all");
    }

    // Route_Node: i32 behaviour | b fixed | b clear_trigger_info
    void route_node(asg::PayloadIo& io, EventRouteNode& e)
    {
        io.e32(e.behaviour, "behaviour");
        io.b(e.fixed, "fixed");
        io.b(e.clear_trigger_info, "clear_trigger_info");
    }

    // Add_Link: i32 subject_uid | b inbound
    void add_link(asg::PayloadIo& io, EventAddLink& e)
    {
        io.i32(e.subject_uid, "subject_uid");
        io.b(e.inbound, "inbound");
    }

    // Valid_Gate: i32 check_uid
    void valid_gate(asg::PayloadIo& io, EventValidGate& e)
    {
        io.i32(e.check_uid, "check_uid");
    }

    // Goal_Math: str goal | i32 operation | i32 operation_value
    void goal_math(asg::PayloadIo& io, EventGoalMath& e)
    {
        io.str(e.goal, "goal");
        io.e32(e.operation, "operation");
        io.i32(e.operation_value, "operation_value");
    }

    // Goal_Gate: str goal | i32 test_type | i32 test_value
    void goal_gate(asg::PayloadIo& io, EventGoalGate& e)
    {
        io.str(e.goal, "goal");
        io.e32(e.test_type, "test_type");
        io.i32(e.test_value, "test_value");
    }

    // Scope_Gate: i32 scope
    void scope_gate(asg::PayloadIo& io, EventScopeGate& e)
    {
        io.e32(e.scope, "scope");
    }

    // Inside_Gate: i32 check_uid | i32 subject_type
    void inside_gate(asg::PayloadIo& io, EventInsideGate& e)
    {
        io.i32(e.check_uid, "check_uid");
        io.e32(e.subject_type, "subject_type");
    }

    // Set_Difficulty: i32 difficulty
    void set_difficulty(asg::PayloadIo& io, EventSetDifficulty& e)
    {
        io.e32(e.difficulty, "difficulty");
    }

    // Set_Fog_Far_Clip: f32 far_clip
    void set_fog_far_clip(asg::PayloadIo& io, EventSetFogFarClip& e)
    {
        io.f32(e.far_clip, "far_clip");
    }

    // AF_When_Dead: b any_dead
    void af_when_dead(asg::PayloadIo& io, EventAFWhenDead& e)
    {
        io.b(e.any_dead, "any_dead");
    }

    // Gametype_Gate: i32 gametype
    void gametype_gate(asg::PayloadIo& io, EventGametypeGate& e)
    {
        io.e32(e.gametype, "gametype");
    }

    // Set_Skybox: i32 room_uid | i32 anchor_uid | f32 position_scale | b relative_position
    void set_skybox(asg::PayloadIo& io, EventSetSkybox& e)
    {
        io.i32(e.new_sky_room_uid, "room_uid");
        io.i32(e.new_sky_room_anchor_uid, "anchor_uid");
        io.f32(e.position_scale, "position_scale");
        io.b(e.relative_position, "relative_position");
    }

    // Set_Life: f32 new_life
    void set_life(asg::PayloadIo& io, EventSetLife& e)
    {
        io.f32(e.new_life, "new_life");
    }

    // Set_Debris: str debris_filename | str debris_sound_set | i32 explode_anim_vclip | f32 explode_anim_radius |
    // f32 debris_velocity
    void set_debris(asg::PayloadIo& io, EventSetDebris& e)
    {
        io.str(e.debris_filename, "debris_filename");
        io.str(e.debris_sound_set, "debris_sound_set");
        io.i32(e.explode_anim_vclip, "explode_anim_vclip");
        io.f32(e.explode_anim_radius, "explode_anim_radius");
        io.f32(e.debris_velocity, "debris_velocity");
    }

    // Set_Fog_Color: str fog_color
    void set_fog_color(asg::PayloadIo& io, EventSetFogColor& e)
    {
        io.str(e.fog_color, "fog_color");
    }

    // Set_Entity_Flag: i32 flag
    void set_entity_flag(asg::PayloadIo& io, EventSetEntityFlag& e)
    {
        io.e32(e.flag, "flag");
    }

    // AF_Teleport_Player: b reset_velocity | b force_exit_vehicle | str entrance_vclip | str exit_vclip
    void af_teleport_player(asg::PayloadIo& io, EventAFTeleportPlayer& e)
    {
        io.b(e.reset_velocity, "reset_velocity");
        io.b(e.force_exit_vehicle, "force_exit_vehicle");
        io.str(e.entrance_vclip, "entrance_vclip");
        io.str(e.exit_vclip, "exit_vclip");
    }

    // Set_Item_Drop: str item_name
    void set_item_drop(asg::PayloadIo& io, EventSetItemDrop& e)
    {
        io.str(e.item_name, "item_name");
    }

    // AF_Heal: i32 amount | i32 target | b apply_to_armor | b super
    void af_heal(asg::PayloadIo& io, EventAFHeal& e)
    {
        io.i32(e.amount, "amount");
        io.e32(e.target, "target");
        io.b(e.apply_to_armor, "apply_to_armor");
        io.b(e.super, "super");
    }

    // World_HUD_Sprite: b enabled. level_init_post loads the sprites of every one.
    void world_hud_sprite(asg::PayloadIo& io, EventWorldHUDSprite& e)
    {
        io.b(e.enabled, "enabled");
    }

    // Set_Light_Color: str light_color | b randomize
    void set_light_color(asg::PayloadIo& io, EventSetLightColor& e)
    {
        io.str(e.light_color, "light_color");
        io.b(e.randomize, "randomize");
    }

    // Set_Gameplay_Rule: i32 rule
    void set_gameplay_rule(asg::PayloadIo& io, EventSetGameplayRule& e)
    {
        io.e32(e.rule, "rule");
    }

    // Mesh_Animate: i32 animate_type | str anim_filename | f32 blend_weight
    void mesh_animate(asg::PayloadIo& io, EventMeshAnimate& e)
    {
        io.i32(e.animate_type, "animate_type");
        io.str(e.anim_filename, "anim_filename");
        io.f32(e.blend_weight, "blend_weight");
    }

    // Mesh_Set_Texture: i32 texture_slot | str texture_filename
    void mesh_set_texture(asg::PayloadIo& io, EventMeshSetTexture& e)
    {
        io.i32(e.texture_slot, "texture_slot");
        io.str(e.texture_filename, "texture_filename");
    }

    // Mesh_Set_Collision: i32 collision_type
    void mesh_set_collision(asg::PayloadIo& io, EventMeshSetCollision& e)
    {
        io.i32(e.collision_type, "collision_type");
    }

    // Mesh_Set_Scale: f32 scale
    void mesh_set_scale(asg::PayloadIo& io, EventMeshSetScale& e)
    {
        io.f32(e.scale, "scale");
    }

    // Fullscreen overlays: f32 duration | f32 transition_time | i32 transition_type | i32 max_alpha_raw | b active |
    // b fading_out_early | hrt main_timer | hrt fadeout_timer | f32 fadeout_start_alpha
    void fullscreen_overlay(asg::PayloadIo& io, EventFullscreenOverlayBase& e)
    {
        io.f32(e.duration, "duration");
        io.f32(e.transition_time, "transition_time");
        io.e32(e.transition_type, "transition_type");
        io.i32(e.max_alpha_raw, "max_alpha_raw");
        io.b(e.active, "active");
        io.b(e.fading_out_early, "fading_out_early");
        io.hrt(e.main_timer, "main_timer");
        io.hrt(e.fadeout_timer, "fadeout_timer");
        io.f32(e.fadeout_start_alpha, "fadeout_start_alpha");
    }

    // AF_Fullscreen_Image: overlay | str filename. An active one gets its bitmap back as turn_on loads it.
    void fullscreen_image(asg::PayloadIo& io, EventFullscreenImage& e)
    {
        fullscreen_overlay(io, e);
        io.str(e.filename, "filename");
        if (io.restoring()) {
            e.bitmap_handle = -1;
            e.bm_w = 0;
            e.bm_h = 0;
            if (e.active && !e.filename.empty()) {
                e.bitmap_handle = rf::bm::load(e.filename.c_str(), -1, true);
                if (e.bitmap_handle >= 0) {
                    rf::bm::get_dimensions(e.bitmap_handle, &e.bm_w, &e.bm_h);
                }
            }
        }
    }

    // AF_Fullscreen_Color: overlay | str color_string. An active one gets its color back as turn_on parses it.
    void fullscreen_color(asg::PayloadIo& io, EventFullscreenColor& e)
    {
        fullscreen_overlay(io, e);
        io.str(e.color_string, "color_string");
        if (io.restoring() && e.active) {
            e.parsed_color = {0, 0, 0, 255};
            if (!e.color_string.empty()) {
                try {
                    e.parsed_color = rf::gr::Color::from_rgb_string(e.color_string);
                }
                catch (const std::exception&) {
                }
            }
        }
    }

    // Modify_Gas_Region: str color_string | f32 density | f32 transition_time
    void modify_gas_region(asg::PayloadIo& io, EventModifyGasRegion& e)
    {
        io.str(e.color_string, "color_string");
        io.f32(e.density, "density");
        io.f32(e.transition_time, "transition_time");
    }

    // Resize_Gas_Region: i32 shape | f32 sphere_radius | str box_dimensions | f32 transition_time
    void resize_gas_region(asg::PayloadIo& io, EventResizeGasRegion& e)
    {
        io.i32(e.shape, "shape");
        io.f32(e.sphere_radius, "sphere_radius");
        io.str(e.box_dimensions, "box_dimensions");
        io.f32(e.transition_time, "transition_time");
    }

    // ATX_Set_Frame: i32 frame_index | str handle
    void atx_set_frame(asg::PayloadIo& io, EventATXSetFrame& e)
    {
        io.i32(e.frame_index, "frame_index");
        io.str(e.handle, "handle");
    }

    // ATX_Play: str handle
    void atx_play(asg::PayloadIo& io, EventATXPlay& e)
    {
        io.str(e.handle, "handle");
    }

    // ATX_Pause: str handle
    void atx_pause(asg::PayloadIo& io, EventATXPause& e)
    {
        io.str(e.handle, "handle");
    }

    // ATX_Set_Frame_Time: i32 frame_time_ms | str handle
    void atx_set_frame_time(asg::PayloadIo& io, EventATXSetFrameTime& e)
    {
        io.i32(e.frame_time_ms, "frame_time_ms");
        io.str(e.handle, "handle");
    }

    // Display_Projection: str handle | i32 render_width | i32 render_height | f32 fov | f32 update_interval. Whether
    // it projects is in ALPO (projector records).
    void display_projection(asg::PayloadIo& io, EventDisplayProjection& e)
    {
        io.str(e.handle, "handle");
        io.i32(e.render_width, "render_width");
        io.i32(e.render_height, "render_height");
        io.f32(e.fov, "fov");
        io.f32(e.update_interval, "update_interval");
    }

    // When_Destroyed: b any_dead | b fired_all | list<i32> fired brush uids, ascending. The brush registry rebuilds
    // itself from the killed rooms.
    void when_destroyed(asg::PayloadIo& io, EventWhenDestroyed& e)
    {
        io.b(e.any_dead, "any_dead");
        io.b(e.fired_all, "fired_all");
        std::vector<int> fired{e.fired_uids.begin(), e.fired_uids.end()};
        std::sort(fired.begin(), fired.end());
        io.list(fired, "fired", [](asg::PayloadIo& io, int& uid) { io.i32(uid, "uid"); });
        if (io.restoring()) {
            e.fired_uids = {fired.begin(), fired.end()};
        }
    }

    template<typename E, void (*F)(asg::PayloadIo&, E&)>
    constexpr PayloadType row(rf::EventType type)
    {
        return {type, fields_of<E, F>};
    }

    using T = rf::EventType;

    // The stock types stock save/load keeps state for (0x004BE020..0x004BE2C0), Set_Liquid_Depth, and the Alpine
    // types. Multiplayer-only types (capture points, respawn points, rounds, Payload) are left out.
    constexpr PayloadType payload_types[] = {
        row<rf::WhenDeadEvent, when_dead>(T::When_Dead),
        row<rf::CyclicTimerEvent, cyclic_timer>(T::Cyclic_Timer),
        row<rf::MakeInvulnerableEvent, make_invulnerable>(T::Make_Invulnerable),
        row<rf::GoalCreateEvent, goal_create>(T::Goal_Create),
        row<rf::AlarmSirenEvent, alarm_siren>(T::Alarm_Siren),
        row<rf::SetLiquidDepthEvent, set_liquid_depth>(T::Set_Liquid_Depth),
        row<EventCloneEntity, clone_entity>(T::Clone_Entity),
        row<EventSwitchRandom, switch_random>(T::Switch_Random),
        row<EventDifficultyGate, difficulty_gate>(T::Difficulty_Gate),
        row<EventHUDMessage, hud_message>(T::HUD_Message),
        row<EventPlayVideo, play_video>(T::Play_Video),
        row<EventSetLevelHardness, set_level_hardness>(T::Set_Level_Hardness),
        row<EventSequence, sequence>(T::Sequence),
        row<EventRemoveLink, remove_link>(T::Remove_Link),
        row<EventRouteNode, route_node>(T::Route_Node),
        row<EventAddLink, add_link>(T::Add_Link),
        row<EventValidGate, valid_gate>(T::Valid_Gate),
        row<EventGoalMath, goal_math>(T::Goal_Math),
        row<EventGoalGate, goal_gate>(T::Goal_Gate),
        row<EventScopeGate, scope_gate>(T::Scope_Gate),
        row<EventInsideGate, inside_gate>(T::Inside_Gate),
        row<EventSetDifficulty, set_difficulty>(T::Set_Difficulty),
        row<EventSetFogFarClip, set_fog_far_clip>(T::Set_Fog_Far_Clip),
        row<EventAFWhenDead, af_when_dead>(T::AF_When_Dead),
        row<EventGametypeGate, gametype_gate>(T::Gametype_Gate),
        row<EventSetSkybox, set_skybox>(T::Set_Skybox),
        row<EventSetLife, set_life>(T::Set_Life),
        row<EventSetDebris, set_debris>(T::Set_Debris),
        row<EventSetFogColor, set_fog_color>(T::Set_Fog_Color),
        row<EventSetEntityFlag, set_entity_flag>(T::Set_Entity_Flag),
        row<EventAFTeleportPlayer, af_teleport_player>(T::AF_Teleport_Player),
        row<EventSetItemDrop, set_item_drop>(T::Set_Item_Drop),
        row<EventAFHeal, af_heal>(T::AF_Heal),
        row<EventWorldHUDSprite, world_hud_sprite>(T::World_HUD_Sprite),
        row<EventSetLightColor, set_light_color>(T::Set_Light_Color),
        row<EventSetGameplayRule, set_gameplay_rule>(T::Set_Gameplay_Rule),
        row<EventMeshAnimate, mesh_animate>(T::Mesh_Animate),
        row<EventMeshSetTexture, mesh_set_texture>(T::Mesh_Set_Texture),
        row<EventMeshSetCollision, mesh_set_collision>(T::Mesh_Set_Collision),
        row<EventMeshSetScale, mesh_set_scale>(T::Mesh_Set_Scale),
        row<EventFullscreenImage, fullscreen_image>(T::AF_Fullscreen_Image),
        row<EventFullscreenColor, fullscreen_color>(T::AF_Fullscreen_Color),
        row<EventModifyGasRegion, modify_gas_region>(T::Modify_Gas_Region),
        row<EventResizeGasRegion, resize_gas_region>(T::Resize_Gas_Region),
        row<EventATXSetFrame, atx_set_frame>(T::ATX_Set_Frame),
        row<EventATXPlay, atx_play>(T::ATX_Play),
        row<EventATXPause, atx_pause>(T::ATX_Pause),
        row<EventATXSetFrameTime, atx_set_frame_time>(T::ATX_Set_Frame_Time),
        row<EventDisplayProjection, display_projection>(T::Display_Projection),
        row<EventWhenDestroyed, when_destroyed>(T::When_Destroyed),
    };

    const PayloadType* find_payload_type(int event_type)
    {
        for (const auto& t : payload_types) {
            if (static_cast<int>(t.type) == event_type) {
                return &t;
            }
        }
        return nullptr;
    }

    // The level loader (0x00460820) turns Enable_Navpoint links into navpoint indices rather than handles, so they
    // stay as the level file has them
    bool links_restorable(int event_type)
    {
        return event_type != static_cast<int>(rf::EventType::Enable_Navpoint);
    }

    asg::EventRecord capture_event(rf::Event& e)
    {
        asg::EventRecord r;
        r.uid = e.uid;
        r.origin = asg::uid_origin(e.uid);
        r.event_type = e.event_type;
        r.enabled = !(e.event_flags & rf::EVENT_FLAG_PAUSED);
        r.on_state = e.delayed_msg;
        r.delay = asg::capture_timestamp(e.delay_timestamp);
        r.delay_seconds = e.delay_seconds;
        r.activated_by = asg::uid_from_handle(e.triggered_by_handle);
        r.triggered_by = asg::uid_from_handle(e.trigger_handle);
        // As Alpine resolves links: a link is an object handle if it is one, else a raw uid (lights, regions, rooms)
        for (int link : e.links) {
            const rf::Object* obj = rf::obj_from_handle(link);
            r.links.push_back(obj ? asg::EventLink{0, obj->uid} : asg::EventLink{1, link});
        }
        const PayloadType* t = find_payload_type(e.event_type);
        if (t) {
            r.payload = asg::capture_payload([&](asg::PayloadIo& io) { t->fields(io, e); });
        }
        return r;
    }

    void restore_event(rf::Event& e, const asg::EventRecord& r, asg::RefResolver& refs)
    {
        e.delay_seconds = r.delay_seconds;
        e.delayed_msg = r.on_state;
        asg::restore_timestamp(e.delay_timestamp, r.delay);
        refs.add(r.activated_by, &e.triggered_by_handle);
        refs.add(r.triggered_by, &e.trigger_handle);
        e.event_flags = r.enabled ? e.event_flags & ~rf::EVENT_FLAG_PAUSED : e.event_flags | rf::EVENT_FLAG_PAUSED;
        if (links_restorable(e.event_type)) {
            // Both kinds as uids: the level loader resolves object uids to handles once the state is restored
            e.links.clear();
            for (const auto& link : r.links) {
                e.links.add(link.uid);
            }
        }
        const PayloadType* t = find_payload_type(e.event_type);
        if (t && !r.payload.empty()) {
            asg::restore_payload(r.payload, &refs, "event", r.uid, [&](asg::PayloadIo& io) { t->fields(io, e); });
        }
    }
}

std::vector<asg::EventRecord> asg::capture_events()
{
    std::vector<EventRecord> out;
    for (rf::Event* e : rf::event_list) {
        if (e && !(e->obj_flags & rf::OF_DELAYED_DELETE)) {
            out.push_back(capture_event(*e));
        }
    }
    return out;
}

// As the stock event loads (0x004BDF80 and siblings) and deleted events (0x004BDFF0): an event the chunk does not list
// was deleted
void asg::restore_events(const std::vector<EventRecord>& records, RefResolver& refs)
{
    const auto by_uid = index_by_uid(records);
    std::unordered_set<int32_t> matched;
    for (rf::Event* e : rf::event_list) {
        if (!e || (e->obj_flags & rf::OF_DELAYED_DELETE)) {
            continue;
        }
        const auto it = by_uid.find(e->uid);
        if (it == by_uid.end()) {
            rf::obj_flag_dead(e);
            continue;
        }
        matched.insert(e->uid);
        if (it->second->event_type != e->event_type) {
            xlog::warn("[ASG] Saved event {} has type {}, the level's has type {}", e->uid, it->second->event_type,
                       e->event_type);
            continue;
        }
        restore_event(*e, *it->second, refs);
    }
    // Nothing creates events at runtime: a saved event missing from the level is from a changed level file
    for (const auto& r : records) {
        if (!matched.contains(r.uid)) {
            xlog::warn("[ASG] Saved event {} (type {}) is not in the level", r.uid, r.event_type);
        }
    }
}
