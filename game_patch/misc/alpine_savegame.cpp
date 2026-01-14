#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <common/version/version.h>
#include <common/utils/string-utils.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <sstream>
#include <unordered_set>
#include <xlog/xlog.h>
#include "alpine_savegame.h"
#include "alpine_settings.h"
#include "../rf/file/file.h"
#include "../os/console.h"
#include "../rf/os/array.h"
#include "../rf/gr/gr_font.h"
#include "../rf/hud.h"
#include "../rf/misc.h"
#include "../rf/event.h"
#include "../rf/object.h"
#include "../rf/entity.h"
#include "../rf/item.h"
#include "../rf/clutter.h"
#include "../rf/gameseq.h"
#include "../rf/weapon.h"
#include "../rf/level.h"
#include "../rf/mover.h"
#include "../rf/corpse.h"
#include "../rf/multi.h"
#include "../rf/trigger.h"
#include "../rf/vmesh.h"
#include "../rf/particle_emitter.h"
#include "../rf/save_restore.h"
#include "../multi/multi.h"

// global save buffer, replacement for sr_data
static asg::SavegameData g_save_data;
static rf::sr::LoggedHudMessage g_tmpLoggedMessages[80]; // temporary storage of logged messages in stock game format, used for message log UI calls

// global buffer for deleted events, replacement for g_DeletedEventsUidArray and g_DeletedEventsUidArrayLen
static std::vector<int> g_deleted_event_uids;

// global buffer for persistent goals, replacement for g_persistent_goal_events and g_num_persistent_goal_events
static std::vector<rf::PersistentGoalEvent> g_persistent_goals;

// global buffer for deleted detail rooms, replacement for glass_deleted_rooms, num_killed_glass_room_uids, killed_glass_room_uids
static std::vector<rf::GRoom*> g_deleted_rooms;
static std::vector<int> g_deleted_room_uids;

// global buffer for delayed save/restore uids and pointers, replacement for g_sr_uids, g_sr_handle_ptrs, and g_sr_uid_handle_mapping_len
static std::vector<int> g_sr_delayed_uids;
static std::vector<int*> g_sr_delayed_ptrs;

// global buffer for ponr entries, replacement for ponr and num_ponr (really, the entirety of the stock ponr system)
std::vector<asg::AlpinePonr> g_alpine_ponr;

namespace asg
{
    // Look up an object by handle and return its uid, or –1 if not found
    // prevent crashing from trying to fetch uid from invalid objects
    inline int uid_from_handle(int handle)
    {
        if (auto o = rf::obj_from_handle(handle))
            return o->uid;
        return -1;
    }

    static std::vector<float> parse_f32_array(const toml::array& arr)
    {
        std::vector<float> v;
        v.reserve(arr.size());
        for (auto& e : arr) {
            if (auto val = e.value<float>())
                v.push_back(*val);
            else
                v.push_back(0.0f);
        }
        return v;
    }

    // Parse a 3-element integer array [x,y,z] directly into a ShortVector
    bool parse_i16_vector(const toml::table& tbl, std::string_view key, rf::ShortVector& out)
    {
        if (auto arr = tbl[key].as_array()) {
            auto& a = *arr;
            if (a.size() == 3) {
                // value_or<int>() will safely convert integer-valued TOML entries
                out.x = static_cast<int16_t>(a[0].value_or<int>(0));
                out.y = static_cast<int16_t>(a[1].value_or<int>(0));
                out.z = static_cast<int16_t>(a[2].value_or<int>(0));
                return true;
            }
        }
        return false;
    }

    // Parse a 4-element integer array [x,y,z,w] directly into a ShortQuat
    bool parse_i16_quat(const toml::table& tbl, std::string_view key, rf::ShortQuat& out)
    {
        if (auto arr = tbl[key].as_array()) {
            auto& a = *arr;
            if (a.size() == 4) {
                out.x = static_cast<int16_t>(a[0].value_or<int>(0));
                out.y = static_cast<int16_t>(a[1].value_or<int>(0));
                out.z = static_cast<int16_t>(a[2].value_or<int>(0));
                out.w = static_cast<int16_t>(a[3].value_or<int>(0));
                return true;
            }
        }
        return false;
    }

    size_t pick_ponr_eviction_slot(const std::vector<std::string>& savedLevels, const std::string& currentLevel)
    {
        xlog::warn("[PONR] pick_ponr_eviction_slot: currentLevel='{}'", currentLevel);
        xlog::warn("[PONR] savedLevels ({}):", savedLevels.size());
        for (size_t i = 0; i < savedLevels.size(); ++i) {
            xlog::warn("[PONR]   [{}] '{}'", i, savedLevels[i]);
        }

        // 1) find the PONR entry for this level
        const AlpinePonr* ponrEntry = nullptr;
        for (auto& e : g_alpine_ponr) {
            if (_stricmp(e.current_level_filename.c_str(), currentLevel.c_str()) == 0) {
                ponrEntry = &e;
                break;
            }
        }
        if (!ponrEntry) {
            xlog::warn("[PONR] no PONR entry for '{}', default evict slot 0", currentLevel);
            return 0;
        }

        xlog::warn("[PONR] found PONR entry for '{}'; keepList ({}):", currentLevel, ponrEntry->levels_to_save.size());
        for (auto const& lvl : ponrEntry->levels_to_save) {
            xlog::warn("[PONR]   keep '{}'", lvl);
        }

        size_t slotCount = savedLevels.size();
        size_t keptSoFar = 0;

        // 2) scan in-order looking for the first slot *not* in keepList
        for (size_t i = 0; i < slotCount; ++i) {
            bool isKeep =
                std::any_of(ponrEntry->levels_to_save.begin(), ponrEntry->levels_to_save.end(),
                            [&](auto const& want) { return _stricmp(want.c_str(), savedLevels[i].c_str()) == 0; });

            xlog::warn("[PONR] slot[{}]='{}' → isKeep={}", i, savedLevels[i], isKeep);

            if (isKeep) {
                ++keptSoFar;
                // if *all* slots are in the keep-list, evict the *last* one
                if (keptSoFar == slotCount) {
                    xlog::warn("[PONR] all {} slots are keep-list; evict last slot {}", slotCount, slotCount - 1);
                    return slotCount - 1;
                }
            }
            else {
                // first slot *not* on the keep-list → evict it
                xlog::warn("[PONR] evicting slot {} ('{}') — first not in keep-list", i, savedLevels[i]);
                return i;
            }
        }

        // should never get here, but just in case:
        xlog::warn("[PONR] fallback: evict slot 0");
        return 0;
    }


    inline size_t ensure_current_level_slot()
    {
        using namespace rf;
        std::string cur = string_to_lower(level.filename);

        auto& hdr = g_save_data.header;
        auto& levels = g_save_data.levels;

        hdr.current_level_filename = cur;

        // find existing
        auto it = std::find(hdr.saved_level_filenames.begin(), hdr.saved_level_filenames.end(), cur);
        size_t idx;

        if (it == hdr.saved_level_filenames.end()) {
            // new level
            /* if (hdr.saved_level_filenames.size() >= 4) {
                // drop oldest
                hdr.saved_level_filenames.erase(hdr.saved_level_filenames.begin());
                levels.erase(levels.begin());
            }*/

            // drop using ponr - consider making the 4 configurable
            if (hdr.saved_level_filenames.size() >= 4) {
                size_t evict = pick_ponr_eviction_slot(hdr.saved_level_filenames, hdr.current_level_filename);
                hdr.saved_level_filenames.erase(hdr.saved_level_filenames.begin() + evict);
                levels.erase(levels.begin() + evict);
            }


            // append slot
            hdr.saved_level_filenames.push_back(cur);
            hdr.num_saved_levels = uint8_t(hdr.saved_level_filenames.size());

            SavegameLevelData lvl;

            levels.push_back(std::move(lvl));
            idx = levels.size() - 1;
        }
        else {
            // already present — just update time in case it changed
            idx = std::distance(hdr.saved_level_filenames.begin(), it);
            
        }

        // build level data header
        hdr.current_level_idx = idx;
        xlog::warn("writing idx {}, filename {}", idx, cur);
        levels[idx].header.filename = cur;
        levels[idx].header.level_time = level_time_flt;
        levels[idx].header.aabb_min = rf::world_solid->bbox_min;
        levels[idx].header.aabb_max = rf::world_solid->bbox_max;

        return idx;
    }

    inline void serialize_player(rf::Player* pp, SavegameCommonDataPlayer& data)
    {
        xlog::warn("[SP] enter serialize_player pp={}", reinterpret_cast<void*>(pp));
        if (!pp) {
            xlog::error("[SP] pp is nullptr!");
            return;
        }

        // 1) host UID
        if (auto e = rf::entity_from_handle(pp->entity_handle)) {
            auto host = rf::obj_from_handle(e->host_handle);
            data.entity_host_uid = host ? host->uid : -1;
            xlog::warn("[SP] entity_host_uid = {}", data.entity_host_uid);
        }
        else {
            data.entity_host_uid = -1;
            xlog::warn("[SP] no entity, entity_host_uid = -1");
        }

        // 2) viewport
        data.clip_x = pp->viewport.clip_x;
        data.clip_y = pp->viewport.clip_y;
        data.clip_w = pp->viewport.clip_w;
        data.clip_h = pp->viewport.clip_h;
        data.fov_h = pp->viewport.fov_h;
        xlog::warn("[SP] viewport: x={} y={} w={} h={} fov={}", data.clip_x, data.clip_y, data.clip_w, data.clip_h,
                   data.fov_h);

        // 3) flags & entity info
        data.player_flags = pp->flags;
        data.field_11f8 = static_cast<int16_t>(pp->field_11F8);
        xlog::warn("[SP] player_flags = 0x{:X}", data.player_flags);

        if (auto ent = rf::local_player_entity) {
            data.entity_uid = ent->uid;
            data.entity_type = pp->entity_type;
            xlog::warn("[SP] entity_uid = {} entity_type = {}", data.entity_uid, (int)data.entity_type);
        }
        else {
            data.entity_uid = -1;
            data.entity_type = 0;
            xlog::warn("[SP] no entity, entity_uid = -1, entity_type = 0");
        }

        // 4) spew
        data.spew_vector_index = pp->spew_vector_index;
        xlog::warn("[SP] spew_vector_index = {}", data.spew_vector_index);
        //pp->spew_pos.assign(&data.spew_pos, &pp->spew_pos);
        data.spew_pos = pp->spew_pos;
        xlog::warn("[SP] spew_pos = ({:.3f},{:.3f},{:.3f})", data.spew_pos.x, data.spew_pos.y, data.spew_pos.z);

        // 5) key items bitmask
        {
            uint32_t mask = 0;
            for (int i = 0; i < 32; ++i)
                if (pp->key_items[i])
                    mask |= (1u << i);
            data.key_items = *reinterpret_cast<float*>(&mask);
            xlog::warn("[SP] key_items mask = 0x{:08X}", mask);
        }

        // 6) view object
        if (auto v = rf::obj_from_handle(pp->view_from_handle)) {
            data.view_obj_uid = v->uid;
            xlog::warn("[SP] view_obj_uid = {}", data.view_obj_uid);
        }
        else {
            data.view_obj_uid = -1;
            xlog::warn("[SP] no view_obj, view_obj_uid = -1");
        }

        // 7) weapon_prefs
        for (int i = 0; i < 32; ++i) {
            data.weapon_prefs[i] = pp->weapon_prefs[i];
        }
        xlog::warn("[SP] weapon_prefs[0..3] = {},{},{},{}", data.weapon_prefs[0], data.weapon_prefs[1],
                   data.weapon_prefs[2], data.weapon_prefs[3]);

        // 8) first-person gun
        {
            data.fpgun_pos = pp->fpgun_data.fpgun_pos;
            data.fpgun_orient = pp->fpgun_data.fpgun_orient;
            xlog::warn("[SP] fpgun_pos = ({:.3f},{:.3f},{:.3f})", data.fpgun_pos.x, data.fpgun_pos.y, data.fpgun_pos.z);
            xlog::warn("[SP] fpgun_orient.rvec = ({:.3f},{:.3f},{:.3f})", data.fpgun_orient.rvec.x,
                       data.fpgun_orient.rvec.y, data.fpgun_orient.rvec.z);
        }

        // 9) grenade mode
        {
            data.grenade_mode = static_cast<uint8_t>(pp->fpgun_data.grenade_mode);
            xlog::warn("[SP] grenade_mode = {}", data.grenade_mode);
        }

        // 10) flags bit-packing
        {
            /* uint8_t low, high;
            bool bit0 = (*reinterpret_cast<const uint8_t*>(reinterpret_cast<const char*>(pp) +
                                                           offsetof(rf::Player, shield_decals) + 21) &
                         1) != 0;
            bool bit1 = (*reinterpret_cast<const uint8_t*>(reinterpret_cast<const char*>(pp) +
                                                           offsetof(rf::Player, shield_decals) + 23) &
                         1) != 0;
            int cover = (rf::g_player_cover_id & 1);
            auto ent = rf::entity_from_handle(pp->entity_handle);
            int ai_high = ent ? ((ent->ai.ai_flags >> 16) & 1) : 0;

            low = static_cast<uint8_t>(data.flags);
            uint8_t part = bit0 ? 1 : 0;
            low = static_cast<uint8_t>((low ^ ((low ^ part) & 1)) & 0xF1);

            int tmp = (2 * cover) | ai_high;
            tmp = 2 * tmp;
            tmp = bit1 ? (tmp | 1) : tmp;
            high = static_cast<uint8_t>(2 * tmp);

            data.flags = low | high;
            xlog::warn("[SP] flags packing → bit0={} bit1={} cover={} ai_high={} result=0x{:02X}", bit0, bit1, cover,
                       ai_high, data.flags);*/

            uint8_t out = 0; // keep bits 4..7

            out |= (pp->fpgun_data.show_silencer ? 1u : 0u) << 0;
            out |= (pp->fpgun_data.remote_charge_in_hand ? 1u : 0u) << 1;
            auto ent = rf::entity_from_handle(pp->entity_handle);
            out |= (((ent ? ent->ai.ai_flags : 0) >> 16) & 1u) << 2;
            out |= (rf::g_player_cover_id & 1u) << 3;

            data.flags = out;
            xlog::warn("[SP] flags packing → result=0x{:02X}", data.flags);
        }

        xlog::warn("[SP] exit serialize_player OK");
    }


    inline void fill_object_block(rf::Object* o, SavegameObjectDataBlock& out)
    {
        if (!o) {
            xlog::error("fill_object_block called for null object");
            return;
        }

        out.uid = o->uid;
        out.parent_uid = uid_from_handle(o->parent_handle);
        out.host_uid = uid_from_handle(o->host_handle);

        out.life = o->life;
        out.armor = o->armor;

        rf::compress_vector3(rf::world_solid, &o->p_data.pos, &out.pos);

        {
            rf::Quaternion q;
            // this is the stock Quaternion::__ct_matrix call
            q.from_matrix(&o->orient);
            // now assign into your compressed quat
            //rf::ShortQuat::assign_0(&out.orient, &q);
            out.orient.from_quat(&q);
        }

        out.friendliness = o->friendliness;
        out.obj_flags = o->obj_flags;
        out.host_tag_handle = (o->host_tag_handle < 0 ? -1 : o->host_tag_handle);

        rf::Vector3 tmp;
        o->p_data.ang_momentum.assign(&tmp, &o->p_data.ang_momentum);
        out.ang_momentum = tmp;

        if (o->p_data.vel.len() >= 1024.0f) {
            rf::Vector3 zero = rf::zero_vector;
            rf::compress_velocity(&zero, &out.vel);
        }
        else {
            rf::compress_velocity(&o->p_data.vel, &out.vel);
        }

        // — Physics flags
        out.physics_flags = o->p_data.flags;

        // — (skin_name etc)
        out.skin_name = "";

        xlog::warn("made block for obj {}", o->uid);

        /* out.uid = o->uid;
        out.parent_uid = o->parent_handle; // may need to make this uid
        out.life = o->life;
        out.armor = o->armor;
        //out.pos = rf::ShortVector::compress(rf::world_solid, o->pos);
        //out.vel = rf::ShortVector::compress(rf::world_solid, o->p_data.vel);
        rf::compress_vector3(rf::world_solid, &o->pos, &out.pos);
        rf::compress_vector3(rf::world_solid, &o->p_data.vel, &out.vel);
        out.friendliness = o->friendliness;
        out.host_tag_handle = o->host_tag_handle;
        //out.orient = o->orient;
        rf::Quaternion q;
        q.extract_matrix(&o->orient);
        out.orient.from_quat(&q);
        out.obj_flags = o->obj_flags;
        if (auto t = rf::obj_from_handle(o->host_handle))
            out.host_uid = t->uid;
        else
            out.host_uid = -1;
        out.ang_momentum = o->p_data.ang_momentum;
        out.physics_flags = o->p_data.flags;
        out.skin_name = "";*/
    }

    inline SavegameEntityDataBlock make_entity_block(rf::Entity* e)
    {
        SavegameEntityDataBlock b{};
        fill_object_block(e, b.obj);

        // ——— Weapon & AI state ———
        b.current_primary_weapon = static_cast<uint8_t>(e->ai.current_primary_weapon);
        b.current_secondary_weapon = static_cast<uint8_t>(e->ai.current_secondary_weapon);
        b.info_index = e->info_index;
        // ammo arrays
        for (int i = 0; i < 32; ++i) {
            b.weapons_clip_ammo[i] = e->ai.clip_ammo[i];
            b.weapons_ammo[i] = e->ai.ammo[i];
        }
        // bitfield
        {
            int mask = 0;
            for (int i = 0; i < 32; ++i)
                if (e->ai.has_weapon[i])
                    mask |= (1 << i);
            b.possesed_weapons_bitfield = mask;
        }
        // hate list
        b.hate_list.clear();
        for (auto handle_ptr : e->ai.hate_list) {
            b.hate_list.push_back(handle_ptr);
        }

        // more AI parameters
        b.ai_mode = e->ai.mode;
        b.ai_submode = e->ai.submode;
        //b.move_mode = e->move_mode->mode;

        int mm;
        for (mm = rf::MM_NONE; mm < 16; ++mm) {
            if (e->move_mode == rf::movemode_get_mode(static_cast<rf::MovementMode>(mm)))
                break;
        }
        if (mm >= 16)
            mm = rf::MM_NONE;
        b.move_mode = mm;

        b.ai_mode_parm_0 = e->ai.mode_parm_0;
        b.ai_mode_parm_1 = e->ai.mode_parm_1;

        if (auto t = rf::obj_from_handle(e->ai.target_handle))
            b.target_uid = t->uid;
        else
            b.target_uid = -1;

        if (auto l = rf::obj_from_handle(e->ai.look_at_handle))
            b.look_at_uid = l->uid;
        else
            b.look_at_uid = -1;

        if (auto s = rf::obj_from_handle(e->ai.shoot_at_handle))
            b.shoot_at_uid = s->uid;
        else
            b.shoot_at_uid = -1;

        // compressed vectors & small fields
        b.ci_rot = e->ai.ci.rot;
        b.ci_move = e->ai.ci.move;

        if (auto c = rf::obj_from_handle(e->ai.corpse_carry_handle))
            b.corpse_carry_uid = c->uid;
        else
            b.corpse_carry_uid = -1;
    
        b.ai_flags = e->ai.ai_flags;

        // vision & control
        b.eye_pos = e->eye_pos;
        b.eye_orient = e->eye_orient;
        b.entity_flags = e->entity_flags;
        b.entity_flags2 = e->entity_flags2;
        b.control_data_phb = e->control_data.phb;
        b.control_data_eye_phb = e->control_data.eye_phb;
        b.control_data_local_vel = e->control_data.local_vel;

        return b;
    }

    inline SavegameItemDataBlock make_item_block(rf::Item* it)
    {
        SavegameItemDataBlock b{};
        fill_object_block(it, b.obj);
        b.respawn_time_ms = it->respawn_time_ms;
        rf::sr::serialize_timestamp(&it->respawn_next, &b.respawn_next_timer);
        b.alpha = it->alpha;
        b.create_time = it->create_time;
        b.flags = it->item_flags;
        b.item_cls_id = it->info_index;
        return b;
    }

    inline SavegameClutterDataBlock make_clutter_block(rf::Clutter* c)
    {
        SavegameClutterDataBlock b{};
        fill_object_block(c, b.obj);
        rf::sr::serialize_timestamp(&c->delayed_kill_timestamp, &b.delayed_kill_timestamp);
        rf::sr::serialize_timestamp(&c->corpse_create_timestamp, &b.corpse_create_timestamp);

        b.links.clear();
        for (auto handle_ptr : c->links) {
            // convert handle to uid
            int handle_ptr_uid = uid_from_handle(handle_ptr);
            b.links.push_back(handle_ptr_uid);
        }

        return b;
    }

    inline SavegameTriggerDataBlock make_trigger_block(rf::Trigger* t)
    {
        SavegameTriggerDataBlock b{};
        b.uid = t->uid;
        //b.pos = rf::ShortVector::compress(rf::world_solid, t->pos);
        rf::compress_vector3(rf::world_solid, &t->pos, &b.pos);
        b.count = t->count;
        b.time_last_activated = t->time_last_activated;
        b.trigger_flags = t->trigger_flags;
        b.activator_handle = uid_from_handle(t->activator_handle);
        rf::sr::serialize_timestamp(&t->button_active_timestamp, &b.button_active_timestamp);
        rf::sr::serialize_timestamp(&t->inside_timestamp, &b.inside_timestamp);

        b.links.clear();
        for (auto handle_ptr : t->links) {
            // convert handle to uid
            int handle_ptr_uid = uid_from_handle(handle_ptr);
            b.links.push_back(handle_ptr_uid);
        }

        return b;
    }

    inline SavegameEventDataBlock make_event_base_block(rf::Event* e)
    {
        SavegameEventDataBlock b{};
        b.event_type = e->event_type;
        b.uid = e->uid;
        b.delay = e->delay_seconds;
        xlog::warn("saved delay {} to {} for uid {}", e->delay_seconds, b.delay, e->uid);
        b.is_on_state = e->delayed_msg;
        rf::sr::serialize_timestamp(&e->delay_timestamp, &b.delay_timer);
        b.activated_by_entity_uid = uid_from_handle(e->triggered_by_handle);
        b.activated_by_trigger_uid = uid_from_handle(e->trigger_handle);

        b.links.clear();
        for (auto handle_ptr : e->links) {
            // convert handle to uid
            int handle_ptr_uid = uid_from_handle(handle_ptr);
            b.links.push_back(handle_ptr_uid);
        }
        return b;
    }

    inline SavegameEventMakeInvulnerableDataBlock make_invulnerable_event_block(rf::Event* e)
    {
        SavegameEventMakeInvulnerableDataBlock m;
        // pull in all the common fields correctly
        m.ev = make_event_base_block(e);

        auto* event = static_cast<rf::MakeInvulnerableEvent*>(e);

        if (event) {            
            rf::sr::serialize_timestamp(&event->make_invuln_timestamp, &m.time_left);
            xlog::warn("event {} is a valid Make_Invulnerable event with time_left {}", event->uid, m.time_left);
        }
        else {
            m.time_left = -1;
        }

        return m;
    }

    inline SavegameEventWhenDeadDataBlock make_when_dead_event_block(rf::Event* e)
    {
        SavegameEventWhenDeadDataBlock m;
        // pull in all the common fields correctly
        m.ev = make_event_base_block(e);

        auto* event = static_cast<rf::WhenDeadEvent*>(e);

        if (event) {
            m.message_sent = event->message_sent ? true : false;
            m.when_any_dead = event->when_any_dead ? true : false;
        }
        else {
            m.message_sent = false;
            m.when_any_dead = false;
        }

        return m;
    }

    inline SavegameEventGoalCreateDataBlock make_goal_create_event_block(rf::Event* e)
    {
        SavegameEventGoalCreateDataBlock m;
        m.ev = make_event_base_block(e);

        auto* event = static_cast<rf::GoalCreateEvent*>(e);

        if (event) {
            m.count = event->count ? true : false;
        }
        else {
            m.count = 0;
        }

        return m;
    }

    inline SavegameEventAlarmSirenDataBlock make_alarm_siren_event_block(rf::Event* e)
    {
        SavegameEventAlarmSirenDataBlock m;
        m.ev = make_event_base_block(e);

        auto* event = static_cast<rf::AlarmSirenEvent*>(e);

        if (event) {
            m.alarm_siren_playing = event->alarm_siren_playing ? true : false;
        }
        else {
            m.alarm_siren_playing = false;
        }

        return m;
    }

    inline SavegameEventCyclicTimerDataBlock make_cyclic_timer_event_block(rf::Event* e)
    {
        SavegameEventCyclicTimerDataBlock m;
        m.ev = make_event_base_block(e);

        auto* event = static_cast<rf::CyclicTimerEvent*>(e);

        if (event) {
            rf::sr::serialize_timestamp(&event->next_fire_timestamp, &m.next_fire_timer);
            m.send_count = event->send_count;

            xlog::warn("event {} is a valid Cyclic_Timer event with next_fire_timer {}, send_count {}, send_seconds {}",
                       event->uid, m.next_fire_timer, m.send_count, event->send_interval_seconds);
        }
        else {
            m.next_fire_timer = -1;
            m.send_count = 0;
        }

        return m;
    }

    static SavegameLevelDecalDataBlock make_decal_block(rf::GDecal* d)
    {
        SavegameLevelDecalDataBlock b;
        b.pos = d->pos;
        b.orient = d->orient;
        b.width = d->width;
        b.flags = d->flags;
        b.alpha = d->alpha;
        //b.tiling_scale = d->tiling_scale;
        b.tiling_scale = 1.0f; //todo: come back to this, may be wrong
        b.bitmap_filename = rf::bm::get_filename(d->bitmap_id);

        return b;
    }

    inline SavegameLevelBoltEmitterDataBlock make_bolt_emitter_block(rf::BoltEmitter* e)
    {
        SavegameLevelBoltEmitterDataBlock b{};
        b.uid = e->uid;
        b.active = e->active;

        return b;
    }

    inline SavegameLevelParticleEmitterDataBlock make_particle_emitter_block(rf::ParticleEmitter* e)
    {
        SavegameLevelParticleEmitterDataBlock b{};
        b.uid = e->uid;
        b.active = e->active;

        return b;
    }

    inline SavegameLevelPushRegionDataBlock make_push_region_block(rf::PushRegion* p)
    {
        SavegameLevelPushRegionDataBlock b{};
        b.uid = p->uid;
        b.active = p->is_enabled;

        return b;
    }

    inline SavegameLevelKeyframeDataBlock make_keyframe_block(rf::Mover* m)
    {
        SavegameLevelKeyframeDataBlock b{};
        fill_object_block(m, b.obj);
        b.rot_cur_pos = m->rot_cur_pos;
        b.start_at_keyframe = m->start_at_keyframe;
        b.stop_at_keyframe = m->stop_at_keyframe;
        b.mover_flags = m->mover_flags;
        b.travel_time_seconds = m->travel_time_seconds;
        b.rotation_travel_time_seconds = m->rotation_travel_time_seconds_unk;
        rf::sr::serialize_timestamp(&m->wait_timestamp, &b.wait_timestamp);
        b.trigger_uid = uid_from_handle(m->trigger_handle);
        b.dist_travelled = m->dist_travelled;
        b.cur_vel = m->cur_vel;
        b.stop_completely_at_keyframe = m->stop_completely_at_keyframe;

        return b;
    }

    static SavegameLevelWeaponDataBlock make_weapon_block(rf::Weapon* w)
    {
        SavegameLevelWeaponDataBlock b{};

        fill_object_block(w, b.obj);

        b.info_index = w->info_index;
        b.life_left_seconds = w->lifeleft_seconds;
        b.weapon_flags = w->weapon_flags;
        b.sticky_host_uid = uid_from_handle(w->sticky_host_handle);
        b.sticky_host_pos_offset = w->sticky_host_pos_offset;
        b.sticky_host_orient = w->sticky_host_orient;
        b.weap_friendliness = static_cast<uint8_t>(w->friendliness);
        b.target_uid = uid_from_handle(w->target_handle);
        b.pierce_power_left = w->pierce_power_left;
        b.thrust_left = w->thrust_left;
        b.firing_pos = w->firing_pos;

        return b;
    }

    static SavegameLevelCorpseDataBlock make_corpse_block(rf::Corpse* c)
    {
        SavegameLevelCorpseDataBlock b{};

        fill_object_block(c, b.obj);

        b.create_time = c->create_time;
        b.lifetime_seconds = c->lifetime_seconds;
        b.corpse_flags = c->corpse_flags;
        b.entity_type = c->entity_type;
        b.pose_name = c->corpse_pose_name.c_str();
        rf::sr::serialize_timestamp(&c->emitter_kill_timestamp, &b.emitter_kill_timestamp);
        b.body_temp = c->body_temp;
        b.state_anim = c->corpse_state_vmesh_anim_index;
        b.action_anim = c->corpse_action_vmesh_anim_index;
        b.drop_anim = c->corpse_drop_vmesh_anim_index;
        b.carry_anim = c->corpse_carry_vmesh_anim_index;
        b.corpse_pose = c->corpse_pose;
        if (c->helmet_v3d_handle)
            b.helmet_name = rf::vmesh_get_name(c->helmet_v3d_handle);
        b.item_uid = uid_from_handle(c->item_handle);
        b.body_drop_sound_handle = c->body_drop_sound_handle;

        // optional: copy spheres & mass/radius
        b.mass = c->p_data.mass;
        b.radius = c->p_data.radius;

        b.cspheres.clear();
        for (int i = 0; i < c->p_data.cspheres.size(); ++i) {
            b.cspheres.push_back(c->p_data.cspheres[i]);
        }

        return b;
    }

    static SavegameLevelBloodPoolDataBlock make_blood_pool_block(rf::EntityBloodPool* p)
    {
        SavegameLevelBloodPoolDataBlock b;
        b.pos = p->pool_pos;
        b.orient = p->pool_orient;
        b.pool_color = p->pool_color;
        return b;
    }

    // serialize all entities into the given vector
    inline void serialize_all_entities(std::vector<SavegameEntityDataBlock>& out, std::vector<int>& dead_entity_uids)
    {
        out.clear();
        dead_entity_uids.clear();

        for (rf::Entity* e = rf::entity_list.next;
            e != &rf::entity_list;
            e = e->next)
        {
            if (!e)
                continue;

            if (e->obj_flags & rf::ObjectFlags::OF_IN_LEVEL_TRANSITION) // IN_LEVEL_TRANSITION
            {
                xlog::warn("skipping UID {} - transition flag is set", e->uid);
                continue;
            }

            std::string nm = e->info->name;
            if (nm == "Bat" || nm == "Fish" || rf::obj_is_hidden(e)) {
                // drop the UID
                xlog::warn("UID {} dropped from save buffer", e->uid);
                dead_entity_uids.push_back(e->uid);
            }
            else {
                // serialize it normally
                if (e->p_data.flags & 0x80000000) // physics_active
                    e->obj_flags |= rf::ObjectFlags::OF_UNK_SAVEGAME_ENT;
                out.push_back(make_entity_block(e));
            }
        }
    }

    // serialize all items into the given vector
    inline void serialize_all_items(std::vector<SavegameItemDataBlock>& out)
    {
        out.clear();
        for (rf::Item* i = rf::item_list.next; i != &rf::item_list; i = i->next) {
            if (i) {
                //xlog::warn("[ASG]   serializing item {}", i->uid);
                out.push_back(make_item_block(i));
            }
        }
    }

    // serialize all clutters into the given vector
    inline void serialize_all_clutters(std::vector<SavegameClutterDataBlock>& out)
    {
        out.clear();
        for (rf::Clutter* c = rf::clutter_list.next; c != &rf::clutter_list; c = c->next) {
            if (c) {
                //xlog::warn("[ASG]   serializing clutter {}", c->uid);
                out.push_back(make_clutter_block(c));
            }
        }
    }

    // serialize all triggers into the given vector
    inline void serialize_all_triggers(std::vector<SavegameTriggerDataBlock>& out)
    {
        out.clear();
        for (rf::Trigger* t = rf::trigger_list.next; t != &rf::trigger_list; t = t->next) {
            if (t) {
                //xlog::warn("[ASG]   serializing trigger {}", t->uid);
                out.push_back(make_trigger_block(t));
            }
        }
    }

    // serialize all events which we need to track by type into the given vector
    inline void serialize_all_events(SavegameLevelData* lvl)
    {
        // clear every event vector
        lvl->when_dead_events.clear();
        lvl->make_invulnerable_events.clear();
        lvl->goal_create_events.clear();
        lvl->alarm_siren_events.clear();
        lvl->cyclic_timer_events.clear();
        lvl->other_events.clear();

        // grab the full array once
        auto full = rf::event_list;
        int n = full.size();

        for (int i = 0; i < n; ++i) {
            rf::Event* e = full[i];
            if (!e)
                continue;

            switch (static_cast<rf::EventType>(e->event_type)) {
            case rf::EventType::When_Dead:
                lvl->when_dead_events.push_back(make_when_dead_event_block(e));
                break;
            case rf::EventType::Make_Invulnerable:
                lvl->make_invulnerable_events.push_back(make_invulnerable_event_block(e));
                break;
            case rf::EventType::Goal_Create:
                lvl->goal_create_events.push_back(make_goal_create_event_block(e));
                break;
            case rf::EventType::Alarm_Siren:
                lvl->alarm_siren_events.push_back(make_alarm_siren_event_block(e));
                break;
            case rf::EventType::Cyclic_Timer:
                lvl->cyclic_timer_events.push_back(make_cyclic_timer_event_block(e));
                break;
            default: // other events saved when they have a queued message
                if (e->delay_timestamp.is_set()) { // todo: maybe include all events? or allow mapper to designate?
                    lvl->other_events.push_back(make_event_base_block(e));
                }
                break;
            }
        }
        xlog::warn("[ASG]       got {} When_Dead events for level '{}'", int(lvl->when_dead_events.size()), lvl->header.filename);
        xlog::warn("[ASG]       got {} Make_Invulnerable events for level '{}'", int(lvl->make_invulnerable_events.size()), lvl->header.filename);
        xlog::warn("[ASG]       got {} Goal_Create events for level '{}'", int(lvl->goal_create_events.size()), lvl->header.filename);
        xlog::warn("[ASG]       got {} Alarm_Siren events for level '{}'", int(lvl->alarm_siren_events.size()), lvl->header.filename);
        xlog::warn("[ASG]       got {} Cyclic_Timer events for level '{}'", int(lvl->cyclic_timer_events.size()), lvl->header.filename);
        xlog::warn("[ASG]       got {} Generic events for level '{}'", int(lvl->other_events.size()), lvl->header.filename);
    }

    inline void serialize_deleted_events(std::vector<int>& out)
    {
        out.clear();
        out = g_deleted_event_uids;
    }

    inline void serialize_killed_rooms(std::vector<int>& out)
    {
        out.clear();
        out.reserve(g_deleted_room_uids.size());

        std::unordered_set<int> seen;
        seen.reserve(g_deleted_room_uids.size());

        for (int uid : g_deleted_room_uids) {
            if (seen.insert(uid).second) {
                out.push_back(uid);
            }
        }
    }

    inline void serialize_all_persistent_goals(std::vector<SavegameLevelPersistentGoalDataBlock>& out)
    {
        out.clear();
        for (auto const& ev : g_persistent_goals) {
            // only names <16 chars, like the stock code did
            if (ev.name.size() < 16) {
                SavegameLevelPersistentGoalDataBlock h;
                h.goal_name = ev.name.c_str();
                h.count = ev.count;
                out.push_back(std::move(h));
            }
        }
    }

    static void serialize_all_decals(std::vector<SavegameLevelDecalDataBlock>& out)
    {
        out.clear();
        rf::GDecal* list;
        int num;
        rf::g_decal_get_list(&list, &num);
        if (!list)
            return;

        rf::GDecal* cur = list;
        do {
            if ((cur->flags & 1) == 0 && cur->bitmap_id > 0 && (cur->flags & 0x400) == 0) {
                out.push_back(make_decal_block(cur));
            }
            cur = cur->next;
        } while (cur && cur != list);
    }

    static void serialize_all_bolt_emitters(std::vector<SavegameLevelBoltEmitterDataBlock>& out)
    {
        out.clear();
        auto& list = rf::bolt_emitter_list;
        size_t n = list.size();
        for (size_t i = 0; i < n; ++i) {
            if (auto* be = list.get(i)) {
                out.push_back(make_bolt_emitter_block(be));
            }
        }
    }

    static void serialize_all_particle_emitters(std::vector<SavegameLevelParticleEmitterDataBlock>& out)
    {
        out.clear();
        auto& list = rf::particle_emitter_list;
        size_t n = list.size();
        for (size_t i = 0; i < n; ++i) {
            if (auto* be = list.get(i)) {
                out.push_back(make_particle_emitter_block(be));
            }
        }
    }

    static void serialize_all_push_regions(std::vector<SavegameLevelPushRegionDataBlock>& out)
    {
        out.clear();
        auto& list = rf::push_region_list;
        size_t n = list.size();
        for (size_t i = 0; i < n; ++i) {
            if (auto* be = list.get(i)) {
                out.push_back(make_push_region_block(be));
            }
        }
    }

    inline void serialize_all_keyframes(std::vector<SavegameLevelKeyframeDataBlock>& out)
    {
        out.clear();
        rf::Mover* cur = rf::mover_list.next;
        while (cur && cur != &rf::mover_list) {
            out.push_back(make_keyframe_block(cur));
            cur = cur->next;
        }
    }

    inline void serialize_all_weapons(std::vector<SavegameLevelWeaponDataBlock>& out)
    {
        out.clear();
        for (rf::Weapon* t = rf::weapon_list.next; t != &rf::weapon_list; t = t->next) {
            // 0x800 is IN_LEVEL_TRANSITION
            // 0x20000000 is unknown
            if (t) {
                if ((t->obj_flags & 0x20000800) == 0)
                    out.push_back(make_weapon_block(t));
            }
        }
    }

    static void serialize_all_corpses(std::vector<SavegameLevelCorpseDataBlock>& out)
    {
        out.clear();
        for (rf::Corpse* t = rf::corpse_list.next; t != &rf::corpse_list; t = t->next) {
            if (t) {
                // 0x800 is IN_LEVEL_TRANSITION
                if ((t->obj_flags & rf::ObjectFlags::OF_IN_LEVEL_TRANSITION) == 0)
                    out.push_back(make_corpse_block(t));
            }
        }
    }

    static void serialize_all_blood_pools(std::vector<SavegameLevelBloodPoolDataBlock>& out)
    {
        out.clear();
        rf::EntityBloodPool* head = rf::g_blood_used_list;
        if (!head)
            return;

        rf::EntityBloodPool* cur = head;
        do {
            out.push_back(make_blood_pool_block(cur));
            cur = cur->next;
        } while (cur && cur != head);
    }

    void serialize_all_objects(SavegameLevelData* data)
    {
        // entities
        xlog::warn("[ASG]     populating entities for level '{}'", data->header.filename);
        data->entities.clear();
        serialize_all_entities(data->entities, data->dead_entity_uids);
        xlog::warn("[ASG]       got {} entities for level '{}'", int(data->entities.size()), data->header.filename);

        // items
        xlog::warn("[ASG]     populating items for level '{}'", data->header.filename);
        data->items.clear();
        serialize_all_items(data->items);
        xlog::warn("[ASG]       got {} items for level '{}'", int(data->items.size()), data->header.filename);

        // clutters
        xlog::warn("[ASG]     populating clutters for level '{}'", data->header.filename);
        data->clutter.clear();
        serialize_all_clutters(data->clutter);
        xlog::warn("[ASG]       got {} clutters for level '{}'", int(data->clutter.size()), data->header.filename);

        // triggers
        xlog::warn("[ASG]     populating triggers for level '{}'", data->header.filename);
        data->triggers.clear();
        serialize_all_triggers(data->triggers);
        xlog::warn("[ASG]       got {} triggers for level '{}'", int(data->triggers.size()), data->header.filename);

        // events
        xlog::warn("[ASG]     populating events for level '{}'", data->header.filename);
        // vectors cleared in serialize_all_events
        serialize_all_events(data);

        // deleted events
        xlog::warn("[ASG]   populating {} deleted_event_uids", g_deleted_event_uids.size());
        data->deleted_event_uids.clear();
        serialize_deleted_events(data->deleted_event_uids);

        // persistent goals
        data->persistent_goals.clear();
        if (!rf::sr::g_disable_saving_persistent_goals) {
            xlog::warn("[ASG]     populating persistent goals for level '{}'", data->header.filename);
            serialize_all_persistent_goals(data->persistent_goals);
            xlog::warn("[ASG]       got {} persistent goals for level '{}'", int(data->persistent_goals.size()), data->header.filename);
        }
        else {
            xlog::warn("[ASG]     skipping population of persistent goals for level '{}'", data->header.filename);
        }

        // decals
        xlog::warn("[ASG]     populating decals for level '{}'", data->header.filename);
        data->decals.clear();
        serialize_all_decals(data->decals);
        xlog::warn("[ASG]       got {} decals for level '{}'", int(data->decals.size()), data->header.filename);

        // killed rooms
        xlog::warn("[ASG]     populating killed rooms for level '{}'", data->header.filename);
        data->killed_room_uids.clear();
        serialize_killed_rooms(data->killed_room_uids);
        xlog::warn("[ASG]       got {} killed rooms for level '{}'", int(data->killed_room_uids.size()), data->header.filename);

        // bolt emitters
        xlog::warn("[ASG]     populating bolt emitters for level '{}'", data->header.filename);
        data->bolt_emitters.clear();
        serialize_all_bolt_emitters(data->bolt_emitters);
        xlog::warn("[ASG]       got {} bolt emitters for level '{}'", int(data->bolt_emitters.size()), data->header.filename);

        // particle emitters
        xlog::warn("[ASG]     populating particle emitters for level '{}'", data->header.filename);
        data->particle_emitters.clear();
        serialize_all_particle_emitters(data->particle_emitters);
        xlog::warn("[ASG]       got {} particle emitters for level '{}'", int(data->particle_emitters.size()), data->header.filename);

        // push regions
        xlog::warn("[ASG]     populating push_regions for level '{}'", data->header.filename);
        data->push_regions.clear();
        serialize_all_push_regions(data->push_regions);
        xlog::warn("[ASG]       got {} push_regions for level '{}'", int(data->push_regions.size()), data->header.filename);

        // movers
        xlog::warn("[ASG]     populating movers for level '{}'", data->header.filename);
        data->movers.clear();
        serialize_all_keyframes(data->movers);
        xlog::warn("[ASG]       got {} movers for level '{}'", int(data->movers.size()), data->header.filename);

        // weapons
        xlog::warn("[ASG]     populating weapons for level '{}'", data->header.filename);
        data->weapons.clear();
        serialize_all_weapons(data->weapons);
        xlog::warn("[ASG]       got {} weapons for level '{}'", int(data->weapons.size()), data->header.filename);

        // corpses
        xlog::warn("[ASG]     populating corpses for level '{}'", data->header.filename);
        data->corpses.clear();
        serialize_all_corpses(data->corpses);
        xlog::warn("[ASG]       got {} corpses for level '{}'", int(data->corpses.size()), data->header.filename);

        // blood pools
        xlog::warn("[ASG]     populating blood pools for level '{}'", data->header.filename);
        data->blood_pools.clear();
        serialize_all_blood_pools(data->blood_pools);
        xlog::warn("[ASG]       got {} blood pools for level '{}'", int(data->blood_pools.size()), data->header.filename);

        // geo craters
        int n = rf::num_geomods_this_level;
        xlog::warn("{} geomods this level", n);
        xlog::warn("[ASG]     populating {} geomod_craters for level '{}'", n, data->header.filename);
        data->geomod_craters.clear();
        if (n > 0) {
            data->geomod_craters.resize(n);
            std::memcpy(data->geomod_craters.data(), rf::geomods_this_level, sizeof(rf::GeomodCraterData) * size_t(n));
        }
        xlog::warn("[ASG]       got {} geomod_craters for level '{}'", int(data->geomod_craters.size()), data->header.filename);

        data->header.aabb_min = rf::world_solid->bbox_min;
        data->header.aabb_max = rf::world_solid->bbox_max;
    }

    static void deserialize_object_state(rf::Object* o, const SavegameObjectDataBlock& src)
    {
        if (!o) {
            xlog::error("deserialize_object_state: failed, null object pointer");
            return;
        }
        //xlog::warn("setting UID {} for previous UID {}", src.uid, o->uid);
        o->uid = src.uid;
        
        // todo
        //o->life = rf::decompress_life_armor(static_cast<uint16_t>(src.life));
        //o->armor = rf::decompress_life_armor(static_cast<uint16_t>(src.armor));
        o->life = src.life;
        o->armor = src.armor;

        rf::decompress_vector3(rf::world_solid, &src.pos, &o->p_data.pos);
        o->pos = o->p_data.pos;
        o->last_pos = o->p_data.pos;
        o->p_data.next_pos = o->p_data.pos;

         rf::Quaternion q;
        q.unpack(&src.orient);
        q.extract_matrix(&o->p_data.orient);
        //rf::Matrix3::orthogonalize(&o->p_data.orient);
        o->p_data.orient.orthogonalize();
        o->orient = o->p_data.orient;

        // if we were hidden before but unhidden in the save, call obj_unhide
        auto old_flags = o->obj_flags;
        o->friendliness = static_cast<rf::ObjFriendliness>(src.friendliness);
        if ((old_flags & rf::ObjectFlags::OF_HIDDEN) && !(src.obj_flags & (int)rf::ObjectFlags::OF_HIDDEN)) {
            rf::obj_unhide(o);
        }

        o->obj_flags = static_cast<rf::ObjectFlags>(src.obj_flags);

        // parent_handle and host_handle are resolved later
        //add_handle_for_delayed_resolution(src.uid, &o->uid);
        add_handle_for_delayed_resolution(src.parent_uid, &o->parent_handle);
        add_handle_for_delayed_resolution(src.host_uid, &o->host_handle);

        // host_tag_handle is a raw byte in the save
        o->host_tag_handle = (src.host_tag_handle == -1 ? -1 : (int8_t)src.host_tag_handle);

        // ——— build physics info & re-init the body ———
        // copy back angular momentum and flags
        o->p_data.ang_momentum = src.ang_momentum;
        o->p_data.flags = src.physics_flags;

        // prepare the stock’s ObjectCreateInfo
        rf::ObjectCreateInfo oci{};

        src.ang_momentum.get_divided(&oci.rotvel, o->p_data.mass);
        oci.body_inv = o->p_data.body_inv;
        oci.drag = o->p_data.drag;
        oci.mass = o->p_data.mass;
        oci.material = o->material;
        oci.orient = o->p_data.orient;
        oci.physics_flags = src.physics_flags;
        oci.pos = o->p_data.pos;
        oci.radius = o->radius;
        oci.solid = nullptr;
        //rf::VArray::reset(&oci.spheres);
        oci.spheres.clear(); // reset = clear?

        // decompress velocity (stock calls decompress_velocity_vector)
        rf::decompress_velocity_vector(&src.vel.x, &oci.vel);

        // call the stock physics re-init
        rf::physics_init_object(&o->p_data, &oci, /*call_callback=*/true);

        // restore momentum & velocity
        o->p_data.ang_momentum = src.ang_momentum;
        rf::decompress_velocity_vector(&src.vel.x, &o->p_data.vel);

        // restore flags and mark “just restored” bit (0x6000000)
        o->p_data.flags = src.physics_flags;
        o->obj_flags |= static_cast<rf::ObjectFlags>(0x06000000);
    }

    static void deserialize_entity_state(rf::Entity* e, const SavegameEntityDataBlock& src)
    {
        xlog::warn("unpacking entity {}", src.obj.uid);
        deserialize_object_state(e, src.obj);

        e->ai.current_primary_weapon = src.current_primary_weapon;
        e->ai.current_secondary_weapon = src.current_secondary_weapon;
        e->info_index = src.info_index;

        for (int i = 0; i < 32; ++i) {
            e->ai.clip_ammo[i] = src.weapons_clip_ammo[i];
            e->ai.ammo[i] = src.weapons_ammo[i];
        }

        for (int i = 0; i < 32; ++i) {
            e->ai.has_weapon[i] = (src.possesed_weapons_bitfield >> i) & 1;
        }

        e->ai.hate_list.clear();
        for (int h : src.hate_list) e->ai.hate_list.add(h);

        // AI params
        e->ai.mode = static_cast<rf::AiMode>(src.ai_mode);
        e->ai.submode = static_cast<rf::AiSubmode>(src.ai_submode);

        e->move_mode = rf::movemode_get_mode(static_cast<rf::MovementMode>(src.move_mode));

        e->ai.mode_parm_0 = src.ai_mode_parm_0;
        e->ai.mode_parm_1 = src.ai_mode_parm_1;

            add_handle_for_delayed_resolution(src.target_uid, &e->ai.target_handle);

            add_handle_for_delayed_resolution(src.look_at_uid, &e->ai.look_at_handle);

            add_handle_for_delayed_resolution(src.shoot_at_uid, &e->ai.shoot_at_handle);


        e->ai.ci.rot = src.ci_rot;
        e->ai.ci.move = src.ci_move;

        add_handle_for_delayed_resolution(src.corpse_carry_uid, &e->ai.corpse_carry_handle);

        e->ai.ai_flags = src.ai_flags;

        e->eye_pos = src.eye_pos;
        e->eye_orient = src.eye_orient;
        e->entity_flags = src.entity_flags;
        e->entity_flags2 = src.entity_flags2;
        e->control_data.phb = src.control_data_phb;
        e->control_data.eye_phb = src.control_data_eye_phb;
        e->control_data.local_vel = src.control_data_local_vel;
    }

    inline void entity_deserialize_all_state(const std::vector<SavegameEntityDataBlock>& blocks, const std::vector<int>& dead_uids)
    {
        bool in_transition = (rf::gameseq_get_state() == rf::GS_LEVEL_TRANSITION);
        xlog::warn("in transition? {}", in_transition);

        // 1) replay or kill/hide existing ents
        for (rf::Entity* ent = rf::entity_list.next; ent != &rf::entity_list; ent = ent->next) {
            bool found = false;

            // only replay if not “in transition only,” or we’re out of that mode
            if ((ent->obj_flags & rf::ObjectFlags::OF_IN_LEVEL_TRANSITION) == 0 || !in_transition) {
                //xlog::warn("looking at ent {}", ent->uid);
                // search for matching UID in our vector
                for (auto const& blk : blocks) {
                    //xlog::warn("ent uid in this block is {}", blk.obj.uid);
                    if (ent->uid == blk.obj.uid) {
                        xlog::warn("found a savegame block for ent {}", ent->uid);
                        deserialize_entity_state(ent, blk);
                        found = true;
                        break;
                    }
                }
            }
            
            if (found) {
                // if this entity was dying in the save AND has that “end‐of‐level” flag, launch endgame
                if ((ent->obj_flags & 2 || rf::entity_is_dying(ent)) && (ent->entity_flags & 0x400000)) {
                    rf::endgame_launch(ent->name);
                }
                else if (ent->obj_flags & 2 || rf::entity_is_dying(ent)) {
                    rf::obj_flag_dead(ent);
                }
            }
            else {
                std::string nm = ent->info->name;
                bool is_dead = false; // already dead = hide, not dead = flag for death
                if (nm == "Bat" || nm == "Fish") {
                    is_dead = false; // kill bats and fish (shouldn't have been in the save struct anyway)
                }
                else {
                    for (int uid : dead_uids)
                        if (ent->uid == uid) {
                            is_dead = true; // hide already dead entities
                            break;
                        }
                    // kill any entities missing from savegame file but not already dead
                }
                if (is_dead)
                    rf::obj_hide(ent);
                else if (ent != rf::local_player_entity && !rf::obj_is_hidden(ent)) // don't kill player or hidden entities
                    rf::obj_flag_dead(ent);
            }
        }

        // 2) spawn any “transition‐only” entities that were marked with 0x400 in their saved obj_flags
        for (auto const& blk : blocks) {
            if (blk.obj.obj_flags & 0x400) {
                // skip if already in the world
                bool exists = false;
                for (rf::Entity* ent = rf::entity_list.next; ent != &rf::entity_list; ent = ent->next) {
                    if (ent->uid == blk.obj.uid) {
                        exists = true;
                        break;
                    }
                }
                if (exists)
                    continue;

                rf::Quaternion q;
                q.unpack(&blk.obj.orient);
                rf::Matrix3 m;
                q.extract_matrix(&m);
                
                rf::Vector3 p;
                rf::decompress_vector3(rf::world_solid, &blk.obj.pos, &p);

                // create and replay
                rf::Entity* ne = rf::entity_create(static_cast<int>(blk.info_index), &rf::default_entity_name, -1, p, m, 0, -1);
                if (ne)
                    deserialize_entity_state(ne, blk);
            }
        }

    }

    static void apply_event_base_fields(rf::Event* e, const SavegameEventDataBlock& b)
    {
        e->delay_seconds = b.delay;        
        rf::sr::deserialize_timestamp(&e->delay_timestamp, &b.delay_timer);
        e->delayed_msg = b.is_on_state;
        add_handle_for_delayed_resolution(b.activated_by_entity_uid, &e->triggered_by_handle);
        add_handle_for_delayed_resolution(b.activated_by_trigger_uid, &e->trigger_handle);

        e->links.clear();
        for (int link_uid : b.links) {
            // push a placeholder handle
            e->links.add(-1);
            // reference to slot
            int& slot = e->links[e->links.size() - 1];
            // queue the UID for resolution
            add_handle_for_delayed_resolution(link_uid, &slot);
        }
    }

    static void apply_generic_event(rf::Event* e, const SavegameEventDataBlock& b)
    {
        apply_event_base_fields(e, b);
    }

    static void apply_make_invuln_event(rf::Event* e, const SavegameEventMakeInvulnerableDataBlock& blk)
    {
        apply_event_base_fields(e, blk.ev);
        auto* ev = static_cast<rf::MakeInvulnerableEvent*>(e);
        rf::sr::deserialize_timestamp(&ev->make_invuln_timestamp, &blk.time_left);
    }

    // When_Dead
    static void apply_when_dead_event(rf::Event* e, const SavegameEventWhenDeadDataBlock& blk)
    {
        apply_event_base_fields(e, blk.ev);
        auto* ev = static_cast<rf::WhenDeadEvent*>(e);
        ev->message_sent = blk.message_sent;
        ev->when_any_dead = blk.when_any_dead;
    }

    // Goal_Create
    static void apply_goal_create_event(rf::Event* e, const SavegameEventGoalCreateDataBlock& blk)
    {
        apply_event_base_fields(e, blk.ev);
        auto* ev = static_cast<rf::GoalCreateEvent*>(e);
        ev->count = blk.count;
    }

    // Alarm_Siren
    static void apply_alarm_siren_event(rf::Event* e, const SavegameEventAlarmSirenDataBlock& blk)
    {
        apply_event_base_fields(e, blk.ev);
        auto* ev = static_cast<rf::AlarmSirenEvent*>(e);
        ev->alarm_siren_playing = blk.alarm_siren_playing;
    }

    // Cyclic_Timer
    static void apply_cyclic_timer_event(rf::Event* e, const SavegameEventCyclicTimerDataBlock& blk)
    {
        apply_event_base_fields(e, blk.ev);
        auto* ev = static_cast<rf::CyclicTimerEvent*>(e);
        rf::sr::deserialize_timestamp(&ev->next_fire_timestamp, &blk.next_fire_timer);
        ev->send_count = blk.send_count;
    }

    void event_deserialize_all_state(const SavegameLevelData& lvl)
    {
        std::unordered_map<int, SavegameEventDataBlock> generic_map;
        std::unordered_map<int, SavegameEventMakeInvulnerableDataBlock> invuln_map;
        std::unordered_map<int, SavegameEventWhenDeadDataBlock> when_dead_map;
        std::unordered_map<int, SavegameEventGoalCreateDataBlock> goal_create_map;
        std::unordered_map<int, SavegameEventAlarmSirenDataBlock> alarm_siren_map;
        std::unordered_map<int, SavegameEventCyclicTimerDataBlock> cyclic_timer_map;

        generic_map.reserve(lvl.other_events.size());
        for (auto const& ev : lvl.other_events) generic_map[ev.uid] = ev;
        invuln_map.reserve(lvl.make_invulnerable_events.size());
        for (auto const& ev : lvl.make_invulnerable_events) invuln_map[ev.ev.uid] = ev;
        when_dead_map.reserve(lvl.when_dead_events.size());
        for (auto const& ev : lvl.when_dead_events) when_dead_map[ev.ev.uid] = ev;
        goal_create_map.reserve(lvl.goal_create_events.size());
        for (auto const& ev : lvl.goal_create_events) goal_create_map[ev.ev.uid] = ev;
        alarm_siren_map.reserve(lvl.alarm_siren_events.size());
        for (auto const& ev : lvl.alarm_siren_events) alarm_siren_map[ev.ev.uid] = ev;
        cyclic_timer_map.reserve(lvl.cyclic_timer_events.size());
        for (auto const& ev : lvl.cyclic_timer_events) cyclic_timer_map[ev.ev.uid] = ev;

        std::unordered_set<int> deleted_ids(lvl.deleted_event_uids.begin(), lvl.deleted_event_uids.end());

        auto full = rf::event_list;
        for (int i = 0, n = full.size(); i < n; ++i) {
            rf::Event* e = full[i];
            if (!e)
                continue;

            int uid = e->uid;
            // delete events marked for deletion
            if (deleted_ids.count(uid)) {
                xlog::warn("Found in deleted UIDs array, deleting event UID {}", uid);
                rf::event_delete(e);
                continue;
            }

            // restore events by type
            switch (static_cast<rf::EventType>(e->event_type)) {
                case rf::EventType::Make_Invulnerable: {
                    auto it = invuln_map.find(uid);
                    if (it != invuln_map.end())
                        apply_make_invuln_event(e, it->second);
                    break;
                }
                case rf::EventType::When_Dead: {
                    auto it = when_dead_map.find(uid);
                    if (it != when_dead_map.end())
                        apply_when_dead_event(e, it->second);
                    break;
                }
                case rf::EventType::Goal_Create: {
                    auto it = goal_create_map.find(uid);
                    if (it != goal_create_map.end())
                        apply_goal_create_event(e, it->second);
                    break;
                }
                case rf::EventType::Alarm_Siren: {
                    auto it = alarm_siren_map.find(uid);
                    if (it != alarm_siren_map.end())
                        apply_alarm_siren_event(e, it->second);
                    break;
                }
                case rf::EventType::Cyclic_Timer: {
                    auto it = cyclic_timer_map.find(uid);
                    if (it != cyclic_timer_map.end())
                        apply_cyclic_timer_event(e, it->second);
                    break;
                }
                default: {
                    auto it = generic_map.find(uid);
                    if (it != generic_map.end())
                        apply_generic_event(e, it->second);
                    break;
                }
            }
        }

    }

    static void apply_trigger_fields(rf::Trigger* t, const SavegameTriggerDataBlock& b)
    {
        // position
        rf::decompress_vector3(rf::world_solid, &b.pos, &t->p_data.pos);
        t->pos = t->p_data.pos;
        t->p_data.next_pos = t->p_data.pos;

        // simple scalars
        t->count = b.count;
        t->trigger_flags = b.trigger_flags;
        t->time_last_activated = b.time_last_activated;

        // activator handle
        add_handle_for_delayed_resolution(b.activator_handle, &t->activator_handle);

        // timestamps
        rf::sr::deserialize_timestamp(&t->button_active_timestamp, &b.button_active_timestamp);
        rf::sr::deserialize_timestamp(&t->inside_timestamp, &b.inside_timestamp);
    }

    static void trigger_deserialize_all_state(const std::vector<SavegameTriggerDataBlock>& blocks)
    {
        // build UID→block map
        std::unordered_map<int, SavegameTriggerDataBlock> tbl;
        tbl.reserve(blocks.size());
        for (auto const& b : blocks) tbl[b.uid] = b;

        // walk the stock trigger_list
        for (rf::Trigger* t = rf::trigger_list.next; t != &rf::trigger_list; t = t->next) {
            auto it = tbl.find(t->uid);
            if (it != tbl.end()) {
                // found a savegame record → replay
                apply_trigger_fields(t, it->second);
            }
            else {
                // no record → kill/hide it
                rf::obj_flag_dead(t);
            }
        }

    }

    static void apply_clutter_fields(rf::Clutter* c, const SavegameClutterDataBlock& b)
    {
        // 1) restore the common Object fields (life, armor, pos/orient, physics, obj_flags, etc)
        deserialize_object_state(c, b.obj);
        xlog::warn("attempting to deserialize clutter {}", b.obj.uid);

        // 2) rehydrate the two timestamps
        rf::sr::deserialize_timestamp(&c->delayed_kill_timestamp, &b.delayed_kill_timestamp);
        rf::sr::deserialize_timestamp(&c->corpse_create_timestamp, &b.corpse_create_timestamp);

        // 3) rebuild the link list, queuing each uid for delayed resolution
        c->links.clear();
        for (int uid : b.links) {
            c->links.add(-1);
            int& slot = c->links[c->links.size() - 1];
            add_handle_for_delayed_resolution(uid, &slot);
        }
    }

    static void clutter_deserialize_all_state(const std::vector<SavegameClutterDataBlock>& blocks)
    {
        xlog::warn("deserializing clutter...");

        // build a UID → block map
        std::unordered_map<int, SavegameClutterDataBlock> blkmap;
        blkmap.reserve(blocks.size());
        for (auto const& b : blocks) blkmap[b.obj.uid] = b;

        // if you want to skip transition-only clutter exactly like stock:
        bool in_transition = (rf::gameseq_get_state() == rf::GS_LEVEL_TRANSITION);

        // walk the stock clutter_list
        for (rf::Clutter* c = rf::clutter_list.next; c != &rf::clutter_list; c = c->next) {
            // stock only replays if not “transition only” or we’re out of transition…
            //if ((c->obj_flags & rf::ObjectFlags::OF_IN_LEVEL_TRANSITION) && in_transition)
            //    continue;
            
            auto it = blkmap.find(c->uid);
            if (it != blkmap.end()) {
                xlog::warn("checking clutter from asg uid {}, original uid {}", it->second.obj.uid, c->uid);
                apply_clutter_fields(c, it->second);
            }
            else {
                // no saved block → kill it
                rf::obj_flag_dead(c);
            }
        }

    }

    static void apply_item_fields(rf::Item* it, const SavegameItemDataBlock& b)
    {
        // 1) common Object fields
        deserialize_object_state(it, b.obj);

        // 2) restore the two timestamps
        it->respawn_time_ms = b.respawn_time_ms;
        rf::sr::deserialize_timestamp(&it->respawn_next, &b.respawn_next_timer);
        it->alpha = b.alpha;
        it->create_time = b.create_time;
        it->item_flags = b.flags;
    }

    static void item_deserialize_all_state(const std::vector<SavegameItemDataBlock>& blocks)
    {
        // build UID → block map
        std::unordered_map<int, SavegameItemDataBlock> map;
        map.reserve(blocks.size());
        for (auto const& b : blocks) map[b.obj.uid] = b;

        bool in_transition = (rf::gameseq_get_state() == rf::GS_LEVEL_TRANSITION);

        // 1) replay or kill all existing items
        for (rf::Item* it = rf::item_list.next; it != &rf::item_list; it = it->next) {
            // skip transition-only items
            if ((it->obj_flags & rf::ObjectFlags::OF_IN_LEVEL_TRANSITION) && in_transition)
                continue;

            auto f = map.find(it->uid);
            if (f != map.end()) {
                apply_item_fields(it, f->second);
            }
            else {
                // not in save → kill
                if (!(it->obj_flags & rf::ObjectFlags::OF_IN_LEVEL_TRANSITION))
                    rf::obj_flag_dead(it);
            }
        }

        // spawn items that were in the save but not in the world by default
        // todo: same for entities and clutter
        for (auto const& b : blocks) {
            if (!rf::obj_lookup_from_uid(b.obj.uid)) {
                // decompress orient & pos
                rf::Quaternion q;
                q.unpack(&b.obj.orient);
                rf::Matrix3 m;
                q.extract_matrix(&m);
                rf::Vector3 p;
                rf::decompress_vector3(rf::world_solid, &b.obj.pos, &p);

                // stock signature: item_create(cls_id, default_name, info_mesh, -1, &p, &m, -1,0,0)
                rf::Item* ni = rf::item_create(b.item_cls_id, "", rf::item_counts[20 * b.item_cls_id], -1, &p, &m, -1, 0, 0);
                if (ni)
                    apply_item_fields(ni, b);
            }
        }

    }

    static void deserialize_bolt_emitters(const std::vector<SavegameLevelBoltEmitterDataBlock>& blocks)
    {
        auto& list = rf::bolt_emitter_list;
        for (size_t i = 0, n = list.size(); i < n; ++i) {
            auto* e = list.get(i);
            if (!e)
                continue;
            // find a saved block with matching uid
            auto it = std::find_if(blocks.begin(), blocks.end(), [&](auto const& blk) { return blk.uid == e->uid; });
            if (it != blocks.end()) {
                e->active = it->active;
            }
        }
    }

    static void deserialize_particle_emitters(const std::vector<SavegameLevelParticleEmitterDataBlock>& blocks)
    {
        auto& list = rf::particle_emitter_list;
        for (size_t i = 0, n = list.size(); i < n; ++i) {
            auto* e = list.get(i);
            if (!e)
                continue;
            auto it = std::find_if(blocks.begin(), blocks.end(), [&](auto const& blk) { return blk.uid == e->uid; });
            if (it != blocks.end()) {
                e->active = it->active;
            }
        }
    }

    static void apply_mover_fields(rf::Mover* mv, const SavegameLevelKeyframeDataBlock& b)
    {
        // 1) restore the shared Object fields
        deserialize_object_state(mv, b.obj);

        // 2) replay all the mover-specific fields
        mv->rot_cur_pos = b.rot_cur_pos;
        mv->start_at_keyframe = b.start_at_keyframe;
        mv->stop_at_keyframe = b.stop_at_keyframe;
        mv->mover_flags = b.mover_flags;
        mv->travel_time_seconds = b.travel_time_seconds;
        mv->rotation_travel_time_seconds_unk = b.rotation_travel_time_seconds;
        rf::sr::deserialize_timestamp(&mv->wait_timestamp, &b.wait_timestamp);

        add_handle_for_delayed_resolution(b.trigger_uid, &mv->trigger_handle);

        mv->dist_travelled = b.dist_travelled;
        mv->cur_vel = b.cur_vel;
        mv->stop_completely_at_keyframe = b.stop_completely_at_keyframe;
    }

    static void mover_deserialize_all_state(const std::vector<SavegameLevelKeyframeDataBlock>& blocks)
    {
        // build a UID → block map
        std::unordered_map<int, SavegameLevelKeyframeDataBlock> blkmap;
        blkmap.reserve(blocks.size());
        for (auto const& b : blocks) blkmap[b.obj.uid] = b;

        bool in_transition = (rf::gameseq_get_state() == rf::GS_LEVEL_TRANSITION);

        // replay or kill/hide each mover in the world
        for (rf::Mover* mv = rf::mover_list.next; mv != &rf::mover_list; mv = mv->next) {
            // stock skips “in-transition only” movers while still in transition
            if ((mv->obj_flags & rf::ObjectFlags::OF_IN_LEVEL_TRANSITION) && in_transition)
                continue;

            auto it = blkmap.find(mv->uid);
            if (it != blkmap.end()) {
                apply_mover_fields(mv, it->second);
            }
            else {
                // no saved block → kill it
                rf::obj_flag_dead(mv);
            }
        }

    }

    static void apply_push_region_fields(rf::PushRegion* pr, const SavegameLevelPushRegionDataBlock& b)
    {
        // simply restore the “enabled” flag
        pr->is_enabled = b.active;
    }

    static void push_region_deserialize_all_state(const std::vector<SavegameLevelPushRegionDataBlock>& blocks)
    {
        // build a quick UID→active map
        std::unordered_map<int, bool> active_map;
        active_map.reserve(blocks.size());
        for (auto const& blk : blocks) {
            active_map[blk.uid] = blk.active;
        }

        // walk the engine’s push_region_list
        auto& list = rf::push_region_list;
        for (size_t i = 0, n = list.size(); i < n; ++i) {
            if (auto* pr = list.get(i)) {
                auto it = active_map.find(pr->uid);
                if (it != active_map.end()) {
                    apply_push_region_fields(pr, SavegameLevelPushRegionDataBlock{pr->uid, it->second});
                }
                // stock doesn’t delete missing push regions, so we don’t either
            }
        }
    }

    static void apply_decal_fields(const SavegameLevelDecalDataBlock& b)
    {
        // rebuild the stock GDecalCreateInfo
        rf::GDecalCreateInfo dci{};
        dci.pos = b.pos;
        dci.orient = b.orient;
        dci.extents = b.width;
        // load the same bitmap
        dci.texture = rf::bm::load(b.bitmap_filename.c_str(), -1, true);
        dci.object_handle = -1;
        dci.flags = b.flags;
        dci.alpha = b.alpha;
        dci.scale = b.tiling_scale;
        // find a room for it
        dci.room = rf::world_solid->find_new_room(0, &dci.pos, &dci.pos, 0);
        dci.solid = rf::world_solid;

        rf::g_decal_add(&dci);
    }

    static void decal_deserialize_all_state(const std::vector<SavegameLevelDecalDataBlock>& blocks)
    {
        // stock only ever re-adds saved decals, it never removes existing ones,
        // so we just replay every saved block
        for (auto const& blk : blocks) {
            apply_decal_fields(blk);
        }
    }

    static void apply_weapon_fields(rf::Weapon* w, const SavegameLevelWeaponDataBlock& b)
    {
        // 1) Restore common Object state
        deserialize_object_state(w, b.obj);

        w->lifeleft_seconds = b.life_left_seconds;
        w->weapon_flags = b.weapon_flags;
        add_handle_for_delayed_resolution(b.sticky_host_uid, &w->sticky_host_handle);
        w->sticky_host_pos_offset = b.sticky_host_pos_offset;
        w->sticky_host_orient = b.sticky_host_orient;
        w->friendliness = static_cast<rf::ObjFriendliness>(b.weap_friendliness);
        w->weap_friendliness = static_cast<rf::ObjFriendliness>(b.weap_friendliness);
        add_handle_for_delayed_resolution(b.target_uid, &w->target_handle);
        w->pierce_power_left = b.pierce_power_left;
        w->thrust_left = b.thrust_left;
        w->firing_pos = b.firing_pos;
    }

    static void weapon_deserialize_all_state(const std::vector<SavegameLevelWeaponDataBlock>& blocks)
    {
        for (auto const& b : blocks) {
            // decompress orientation & position from ObjectSavegameBlock
            rf::Quaternion q;
            q.unpack(&b.obj.orient);

            rf::Matrix3 m;
            q.extract_matrix(&m);

            rf::Vector3 p;
            rf::decompress_vector3(rf::world_solid, &b.obj.pos, &p);

            rf::Weapon* w = rf::weapon_create(b.info_index, -1, &p, &m, 0, 0);
            if (!w)
                continue;

            apply_weapon_fields(w, b);
        }

    }

    // helper to unlink a pool from the free‐list
    static void remove_from_blood_pool_free_list(rf::EntityBloodPool* p)
    {
        auto next = p->next;
        auto prev = p->prev;

        if (next == p) {
            rf::g_blood_free_list = nullptr;
        }
        else {
            if (rf::g_blood_free_list == p)
                rf::g_blood_free_list = next;
            next->prev = prev;
            prev->next = next;
        }
        p->next = p->prev = nullptr;
    }

    // helper to insert a pool onto the used‐list
    static void add_to_blood_pool_used_list(rf::EntityBloodPool* p)
    {
        if (!rf::g_blood_used_list) {
            rf::g_blood_used_list = p;
            p->next = p->prev = p;
        }
        else {
            auto head = rf::g_blood_used_list;
            auto tail = head->prev;
            tail->next = p;
            p->prev = tail;
            p->next = head;
            head->prev = p;
        }
    }

    static void blood_pool_deserialize_all_state(const std::vector<SavegameLevelBloodPoolDataBlock>& blocks)
    {
        // replay all saved blood‐pools in order
        for (auto const& b : blocks) {
            // grab the first free pool
            rf::EntityBloodPool* p = rf::g_blood_free_list;
            if (!p)
                break;

            // 1) unlink it from the free list
            remove_from_blood_pool_free_list(p);

            // 2) restore our saved state
            p->pool_pos = b.pos;
            p->pool_orient = b.orient;
            p->pool_color = b.pool_color;

            // 3) link it into the used‐list
            add_to_blood_pool_used_list(p);
        }
    }

    static void apply_corpse_fields(rf::Corpse* c, const SavegameLevelCorpseDataBlock& b)
    {
        // 1) common Object fields (position/orient/physics/flags/etc)
        deserialize_object_state(c, b.obj);

        // 2) corpse‐specific simple fields
        c->create_time = b.create_time;
        c->lifetime_seconds = b.lifetime_seconds;
        c->corpse_flags = b.corpse_flags;
        c->entity_type = b.entity_type;
        c->corpse_pose_name = b.pose_name.c_str();
        rf::sr::deserialize_timestamp(&c->emitter_kill_timestamp, &b.emitter_kill_timestamp);
        c->body_temp = b.body_temp;
        c->corpse_state_vmesh_anim_index = b.state_anim;
        c->corpse_action_vmesh_anim_index = b.action_anim;
        c->corpse_drop_vmesh_anim_index = b.drop_anim;
        c->corpse_carry_vmesh_anim_index = b.carry_anim;
        c->corpse_pose = b.corpse_pose;

        // 3) re‐attach item handle
        add_handle_for_delayed_resolution(b.item_uid, &c->item_handle);

        // 4) any other handles
        c->body_drop_sound_handle = b.body_drop_sound_handle;

        // 5) rebuild collision spheres
        c->p_data.mass = b.mass;
        c->p_data.radius = b.radius;
        c->p_data.cspheres.clear();
        for (auto const& sph : b.cspheres) c->p_data.cspheres.add(sph);
    }

    static void corpse_deserialize_all_state(const std::vector<SavegameLevelCorpseDataBlock>& blocks)
    {
        bool in_transition = (rf::gameseq_get_state() == rf::GS_LEVEL_TRANSITION);

        // Build a quick UID→block map
        std::unordered_map<int, SavegameLevelCorpseDataBlock> blkmap;
        blkmap.reserve(blocks.size());
        for (auto const& b : blocks) blkmap[b.obj.uid] = b;

        // 1) Replay or kill existing corpses
        for (rf::Corpse* c = rf::corpse_list.next; c != &rf::corpse_list; c = c->next) {
            // stock skips IN_LEVEL_TRANSITION‐only corpses while transitioning
            if ((c->obj_flags & rf::ObjectFlags::OF_IN_LEVEL_TRANSITION) && in_transition)
                continue;

            auto it = blkmap.find(c->uid);
            if (it != blkmap.end()) {
                apply_corpse_fields(c, it->second);
            }
            else {
                rf::obj_flag_dead(c);
            }
        }

        // 2) Spawn any corpses that were in the save but aren't in the world yet
        for (auto const& b : blocks) {
            if (auto obj_ep = rf::obj_lookup_from_uid(b.obj.uid)) {
                // --- decompress orientation & position ---
                rf::Quaternion q;
                q.unpack(&b.obj.orient);
                rf::Matrix3 m;
                q.extract_matrix(&m);
                rf::Vector3 p;
                rf::decompress_vector3(rf::world_solid, &b.obj.pos, &p);
                if (obj_ep->type == rf::ObjectType::OT_ENTITY) {
                    auto ep = reinterpret_cast<rf::Entity*>(obj_ep);
                    if (auto* newc = rf::corpse_create(ep, ep->info->corpse_anim_string, &p, &m, false, false)) {
                        newc->uid = b.obj.uid;
                        apply_corpse_fields(newc, b);
                    }
                }
                
                /*
                // --- stock uses ObjectCreateInfo to build a Corpse object ---
                rf::ObjectCreateInfo ci{};
                ci.mass = b.mass;
                ci.radius = b.radius;
                ci.solid = nullptr; // or rf::world_solid if they expect it
                ci.orient = m;
                ci.pos = p;
                ci.material =  3;
                ci.physics_flags =  51;
                // copy over collision spheres
                for (auto const& sph : b.cspheres) ci.spheres.add(sph);

                // --- now actually create it ---
                if (auto* newc = reinterpret_cast<rf::Corpse*>(rf::obj_create(rf::ObjectType::OT_CORPSE,
                                                                               -1,  -1, &ci,
                                                                               true))) {
                    // give it the right UID back if needed:
                    newc->baseclass_0.uid = b.obj.uid;

                    apply_corpse_fields(newc, b);
                }*/
            }
        }

    }

    static void apply_killed_glass_room(int room_uid)
    {
        if (auto* room = rf::world_solid->find_room_by_id(room_uid)) {
            if (room->get_is_detail()) {
                rf::glass_delete_room(room);
            }
        }
    }

    static void glass_deserialize_all_killed_state(const std::vector<int>& killed_room_uids)
    {
        if (killed_room_uids.empty())
            return;

        for (int uid : killed_room_uids) {
            apply_killed_glass_room(uid);
        }
    }

    void deserialize_all_objects(SavegameLevelData* lvl)
    {
        // reset our “UID → int*” mapping
        clear_delayed_handles();

        event_deserialize_all_state(*lvl);
        deserialize_bolt_emitters(lvl->bolt_emitters);
        deserialize_particle_emitters(lvl->particle_emitters);
        entity_deserialize_all_state(lvl->entities, lvl->dead_entity_uids);
        item_deserialize_all_state(lvl->items);
        clutter_deserialize_all_state(lvl->clutter);
        trigger_deserialize_all_state(lvl->triggers);
        mover_deserialize_all_state(lvl->movers);
        push_region_deserialize_all_state(lvl->push_regions);
        decal_deserialize_all_state(lvl->decals);
        weapon_deserialize_all_state(lvl->weapons);
        blood_pool_deserialize_all_state(lvl->blood_pools);
        corpse_deserialize_all_state(lvl->corpses);
        glass_deserialize_all_killed_state(lvl->killed_room_uids);

        resolve_delayed_handles();

        xlog::warn("restoring current level time {} to buffer time {}", rf::level_time_flt, lvl->header.level_time);
        rf::level_time_flt = lvl->header.level_time;
        
    }

    // Replaces the stock sr_load_player; builds the in-world Entity and restores Player state
    bool load_player(const asg::SavegameCommonDataPlayer* pd, rf::Player* player, const asg::SavegameEntityDataBlock* blk)
    {
        using namespace rf;
        xlog::warn("1 unpacking player");

        if (!pd || !player || !blk)
            return false;

        xlog::warn("2 unpacking player");

        Quaternion q; // v25
        q.unpack(&blk->obj.orient);
        Matrix3 m; // a2
        q.extract_matrix(&m);

        Vector3 world_pos; // a3
        decompress_vector3(world_solid, &blk->obj.pos, &world_pos);

        Entity* ent = player_create_entity(player, static_cast<uint8_t>(pd->entity_type), &world_pos, &m, -1);
        if (!ent) {
            xlog::warn("failed to create player entity");
            return false;
        }
            

        if ((pd->flags & 0x04) != 0) {
            // bit-3 of flags -> team (0/1)
            player_undercover_start((pd->flags >> 3) & 1);
            ent = local_player_entity;
            int uw = undercover_weapon;
            ent->ai.current_primary_weapon = uw;
            player_fpgun_get_vmesh_handle(player, uw);
            player_undercover_set_gun_skin();
        }

        if (!ent)
            return false;

        deserialize_entity_state(ent, *blk);

        if (pd->entity_host_uid >= 0) {
            if (auto host = obj_lookup_from_uid(pd->entity_host_uid))
                ent->host_handle = host->handle;
            else
                ent->host_handle = -1;
        }

        player->flags = pd->player_flags & 0xFFF7; // clear bit-3 out
        player->entity_type = static_cast<uint8_t>(pd->entity_type);
        player->field_11F8 = pd->field_11f8;

        player->spew_vector_index = pd->spew_vector_index;
        //Vector3::assign(&player->spew_pos, &world_pos, &pd->spew_pos);
        player->spew_pos.assign(&world_pos, &pd->spew_pos);

        {
            uint32_t bits = *reinterpret_cast<const uint32_t*>(&pd->key_items);
            for (int i = 0; i < 32; ++i) player->key_items[i] = (bits >> i) & 1;
        }

        if (pd->view_obj_uid >= 0) {
            if (auto v = obj_lookup_from_uid(pd->view_obj_uid))
                player->view_from_handle = v->handle;
            else
                player->view_from_handle = -1;
        }
        else {
            player->view_from_handle = -1;
        }

        for (int i = 0; i < 32; ++i) {
            player->weapon_prefs[i] = static_cast<int>(pd->weapon_prefs[i]);
        }

        player->fpgun_data.fpgun_pos.assign(&world_pos, &pd->fpgun_pos);
        player->fpgun_data.fpgun_orient.assign(&pd->fpgun_orient);

        bool dec21 = (pd->flags & 0x01) != 0;
        bool dec23 = (pd->flags & 0x02) != 0;
        player->fpgun_data.show_silencer = dec21;
        player->fpgun_data.grenade_mode = static_cast<int>(pd->grenade_mode);
        player->fpgun_data.remote_charge_in_hand = dec23;

        int cur = ent->ai.current_primary_weapon;
        if (cur == remote_charge_weapon_type && !dec23)
            ent->ai.current_primary_weapon = remote_charge_det_weapon_type;

        if (ent->host_handle != -1) {
            int ht = ent->host_tag_handle;
            ent->host_tag_handle = -1;

            if (auto host = obj_from_handle(ent->host_handle); host && host->type == OT_ENTITY) {
                //auto host_ent = reinterpret_cast<rf::Entity*>(host);
                //auto host_ent = reinterpret_cast<rf::Entity*>(host);
                entity_headlamp_turn_off(ent);
                ent->attach_leech(ent->handle, ht);
                //Entity::attach_leech(host, ent->handle, ht);
                //host_ent.leech_attach(ent->handle, ht); // todo
                //ent->last_pos.x = (ent->last_pos.x & ~0x0030000) | 0x0010000;
                uint32_t bits = std::bit_cast<uint32_t>(ent->last_pos.x);
                bits = (bits & ~0x0030000u) | 0x0010000u;
                ent->last_pos.x = std::bit_cast<float>(bits);
                obj_set_friendliness(ent, 1);

                // copy two vectors at offsets +108, +120
                /* Vector3::assign(
                    reinterpret_cast<Vector3*>(&host->start_orient.fvec.y), &world_pos,
                    reinterpret_cast<const Vector3*>(reinterpret_cast<const char*>(&host->correct_pos) + 108));
                Vector3::assign(
                    reinterpret_cast<Vector3*>(&host->root_bone_index), &world_pos,
                    reinterpret_cast<const Vector3*>(reinterpret_cast<const char*>(&host->correct_pos) + 120));*/

                auto src1 = reinterpret_cast<const rf::Vector3*>(reinterpret_cast<const char*>(&host->correct_pos) + 108);
                auto dst1 = reinterpret_cast<rf::Vector3*>(&host->start_orient.fvec.y);
                rf::Vector3 tmp1;
                dst1->assign(&tmp1, src1);

                auto src2 = reinterpret_cast<const rf::Vector3*>(reinterpret_cast<const char*>(&host->correct_pos) + 120);
                auto dst2 = reinterpret_cast<rf::Vector3*>(&host->root_bone_index);
                rf::Vector3 tmp2;
                dst2->assign(&tmp2, src2);

                if (entity_is_jeep_gunner(ent)) {
                    ent->min_rel_eye_phb.assign(&world_pos, &jeep_gunner_min_phb);
                    ent->max_rel_eye_phb.assign(&world_pos, &jeep_gunner_max_phb);
                }
                if (entity_is_automobile(ent))
                    obj_physics_activate(host);
            }
        }

        entity_update_collision_spheres(ent);

        if (ent->entity_flags & 0x400) {
            entity_crouch(ent);
            player->is_crouched = true;
        }

        return true;

        /*
        // Only spawn a new player-entity when we're *not* in the middle of a level transition:
        if (1==1||gameseq_get_state() != GS_LEVEL_TRANSITION) {
            xlog::warn("3 unpacking player");
            Quaternion q;
            q.unpack(&blk->obj.orient);
            Matrix3 m;
            q.extract_matrix(&m);

            // 2) Unpack position
            //Vector3 world_pos = ShortVector::decompress(world_solid, blk->obj.pos);
            Vector3 world_pos;
            rf::decompress_vector3(rf::world_solid, &blk->obj.pos, &world_pos);
            xlog::warn("[ASG]   spawn position = x={:.3f}, y={:.3f}, z={:.3f}", world_pos.x, world_pos.y, world_pos.z);

            // 3) Spawn the Entity and replay its saved state
            Entity* ent = rf::player_create_entity(player, static_cast<int>(blk->info_index), &world_pos, &m, -1);
            if (!ent)
                return false;

            // copy back everything the stock would have:
            asg::deserialize_entity_state(ent, *blk);

            // 2) Unpack position
            // Vector3 world_pos = ShortVector::decompress(world_solid, blk->obj.pos);
            Vector3 world_pos2;
            rf::decompress_vector3(rf::world_solid, &blk->obj.pos, &world_pos2);
            xlog::warn("[ASG]   spawn position = x={:.3f}, y={:.3f}, z={:.3f}", world_pos2.x, world_pos2.y, world_pos2.z);
            ent->pos = world_pos2;

            // 4) Re-attach host (if any)
            if (pd->entity_host_uid >= 0) {
                if (auto host_obj = obj_lookup_from_uid(pd->entity_host_uid))
                    ent->host_handle = host_obj->handle;
                else
                    ent->host_handle = -1;
            }

            // 5) Restore Player-specific fields:

            // — spew index & position
            player->spew_vector_index = pd->spew_vector_index;
            player->spew_pos = pd->spew_pos;

            // — key-items bitfield
            {
                uint32_t mask = *reinterpret_cast<const uint32_t*>(&pd->key_items);
                for (int i = 0; i < 32; ++i) player->key_items[i] = (mask >> i) & 1;
            }

            // — view_from handle
            if (pd->view_obj_uid >= 0) {
                if (auto v = obj_lookup_from_uid(pd->view_obj_uid))
                    player->view_from_handle = v->handle;
                else
                    player->view_from_handle = -1;
            }
            else {
                player->view_from_handle = -1;
            }

            // — first-person gun transform
            //   we know shield_decals layout; offset 12 bytes is gun-pos, next 3×Vector3 is orient
            Vector3* gun_pos = reinterpret_cast<Vector3*>(reinterpret_cast<char*>(&player->shield_decals) + 12);
            Matrix3* gun_orient =
                reinterpret_cast<Matrix3*>(reinterpret_cast<char*>(&player->shield_decals) + 12 + sizeof(Vector3));
            *gun_pos = pd->fpgun_pos;
            *gun_orient = pd->fpgun_orient;

            // — grenade mode
            //player->gren = pd->grenade_mode;
        }

        return true;*/
    }

    SavegameData build_savegame_data(rf::Player* pp)
    {
        SavegameData data;

        // ——— HEADER ———
        data.header.version = g_save_data.header.version;
        data.header.game_time = g_save_data.header.game_time;
        data.header.mod_name = g_save_data.header.mod_name;
        data.header.level_time_left = rf::level_time2;
        //data.header.level_time2 = 

        xlog::warn("[ASG]   header → time={} mod='{}'", data.header.game_time, data.header.mod_name);

        // ——— COMMON.GAME ———
        data.common.game.difficulty = g_save_data.common.game.difficulty;
        data.common.game.newest_message_index = g_save_data.common.game.newest_message_index;
        data.common.game.messages = g_save_data.common.game.messages;
        data.common.game.num_logged_messages = g_save_data.common.game.num_logged_messages;
        data.common.game.messages_total_height = g_save_data.common.game.messages_total_height;

        xlog::warn("[ASG]   common.game → difficulty={} messages={}", int(data.common.game.difficulty),
                   int(data.common.game.messages.size()));

        // ——— COMMON.PLAYER ———
        serialize_player(pp, data.common.player);

        xlog::warn("[ASG]   common.player → entity_host_uid={} spew=({}, {}, …)", data.common.player.entity_host_uid,
                   data.common.player.spew_pos.x, data.common.player.spew_pos.y);

        // — now sync up level‐slots in data.header/ data.levels  —
        ensure_current_level_slot();

        // copy all of g_save_data over
        data.header = g_save_data.header;
        data.levels = g_save_data.levels;

        // — then snapshot the live entities into *each* level’s list  —
        for (auto& lvl : data.levels) {
            serialize_all_objects(&lvl);
        }

        xlog::warn("[ASG] build_savegame_data returning levels={}", int(data.levels.size()));

        return data;
    }

    int add_handle_for_delayed_resolution(int uid, int* obj_handle_ptr)
    {
        if (uid != -1) {
            g_sr_delayed_uids.push_back(uid);
            g_sr_delayed_ptrs.push_back(obj_handle_ptr);
        }
        else {
            *obj_handle_ptr = -1;
        }
        return int(g_sr_delayed_uids.size());
    }

    void clear_delayed_handles()
    {
        g_sr_delayed_uids.clear();
        g_sr_delayed_ptrs.clear();
    }

    void resolve_delayed_handles()
    {
        std::vector<int> unresolved_uids;
        std::vector<int*> unresolved_ptrs;
        unresolved_uids.reserve(g_sr_delayed_uids.size());
        unresolved_ptrs.reserve(g_sr_delayed_ptrs.size());

        for (size_t i = 0; i < g_sr_delayed_uids.size(); ++i) {
            int uid = g_sr_delayed_uids[i];
            int* dst = g_sr_delayed_ptrs[i];

            if (auto obj = rf::obj_lookup_from_uid(uid)) {
                *dst = obj->handle;
            }
            else {
                *dst = -1;
                unresolved_uids.push_back(uid);
                unresolved_ptrs.push_back(dst);
            }
        }

        g_sr_delayed_uids.swap(unresolved_uids);
        g_sr_delayed_ptrs.swap(unresolved_ptrs);
    }
} // namespace asg

static toml::table make_header_table(const asg::SavegameHeader& h)
{
    toml::table hdr;
    hdr.insert("asg_version", ASG_VERSION);
    hdr.insert("game_time", rf::level.global_time);
    hdr.insert("mod_name", rf::mod_param.found() ? rf::mod_param.get_arg() : "");
    hdr.insert("current_level_filename", h.current_level_filename);
    hdr.insert("current_level_idx", h.current_level_idx);
    hdr.insert("num_saved_levels", h.num_saved_levels);

    toml::array slots;
    for (auto const& fn : h.saved_level_filenames) slots.push_back(fn);
    hdr.insert("saved_level_filenames", std::move(slots));

    return hdr;
}

static toml::table make_common_game_table(const asg::SavegameCommonDataGame& g)
{
    toml::table cg;
    cg.insert("difficulty", static_cast<int>(g.difficulty));
    cg.insert("newest_message_index", g.newest_message_index);
    cg.insert("num_logged_messages", g.num_logged_messages);
    cg.insert("messages_total_height", g.messages_total_height);

    toml::array msgs;
    for (auto const& m : g.messages) {
        toml::table msg;
        msg.insert("speaker", m.persona_index);
        msg.insert("time_string", m.time_string);
        msg.insert("display_height", m.display_height);
        msg.insert("message", m.message);
        msgs.push_back(std::move(msg));
    }
    cg.insert("logged_messages", std::move(msgs));
    return cg;
}

static toml::table make_common_player_table(const asg::SavegameCommonDataPlayer& p)
{
    toml::table cp;
    cp.insert("entity_host_uid", p.entity_host_uid);
    cp.insert("spew_vector_index", int(p.spew_vector_index));

    toml::array spew;
    spew.push_back(p.spew_pos.x);
    spew.push_back(p.spew_pos.y);
    spew.push_back(p.spew_pos.z);
    cp.insert("spew_pos", std::move(spew));

    cp.insert("key_items", p.key_items);
    cp.insert("view_obj_uid", p.view_obj_uid);
    cp.insert("grenade_mode", int(p.grenade_mode));

    cp.insert("clip_x", p.clip_x);
    cp.insert("clip_y", p.clip_y);
    cp.insert("clip_w", p.clip_w);
    cp.insert("clip_h", p.clip_h);
    cp.insert("fov_h", p.fov_h);
    cp.insert("player_flags", p.player_flags);
    cp.insert("field_11f8", p.field_11f8);
    cp.insert("entity_uid", p.entity_uid);
    cp.insert("entity_type", int(p.entity_type));

    // weapon prefs:
    toml::array wp;
    for (auto w : p.weapon_prefs) wp.push_back(int(w));
    cp.insert("weapon_prefs", std::move(wp));

    cp.insert("flags", int(p.flags));

    // fpgun_orient
    toml::array orient;
    for (auto const& row : {p.fpgun_orient.rvec, p.fpgun_orient.uvec, p.fpgun_orient.fvec}) {
        toml::array r;
        r.push_back(row.x);
        r.push_back(row.y);
        r.push_back(row.z);
        orient.push_back(std::move(r));
    }
    cp.insert("fpgun_orient", std::move(orient));

    // fpgun_pos
    toml::array fp;
    fp.push_back(p.fpgun_pos.x);
    fp.push_back(p.fpgun_pos.y);
    fp.push_back(p.fpgun_pos.z);
    cp.insert("fpgun_pos", std::move(fp));

    return cp;
}

static toml::table make_level_header_table(const asg::SavegameLevelDataHeader& h)
{
    toml::table lt;
    lt.insert("filename", h.filename);
    lt.insert("level_time", h.level_time);

    toml::array amin, amax;
    amin.push_back(h.aabb_min.x);
    amin.push_back(h.aabb_min.y);
    amin.push_back(h.aabb_min.z);
    lt.insert("aabb_min", std::move(amin));

    amax.push_back(h.aabb_max.x);
    amax.push_back(h.aabb_max.y);
    amax.push_back(h.aabb_max.z);
    lt.insert("aabb_max", std::move(amax));

    return lt;
}

static toml::table make_object_table(const asg::SavegameObjectDataBlock& o)
{
    toml::table t;
    t.insert("uid", o.uid);
    t.insert("parent_uid", o.parent_uid);
    t.insert("life", o.life);
    t.insert("armor", o.armor);

    // pos
    toml::array pos{o.pos.x, o.pos.y, o.pos.z};
    t.insert("pos", std::move(pos));

    // vel
    toml::array vel{o.vel.x, o.vel.y, o.vel.z};
    t.insert("vel", std::move(vel));

    t.insert("friendliness", o.friendliness);
    t.insert("host_tag_handle", o.host_tag_handle);

    // orient as 3×3 array
    toml::array orient;
    /* for (auto const& row : {o.orient.rvec, o.orient.uvec, o.orient.fvec}) {
        toml::array r{row.x, row.y, row.z};
        orient.push_back(std::move(r));
    }
    t.insert("orient", std::move(orient));*/
    toml::array quat{ o.orient.x, o.orient.y, o.orient.z, o.orient.w };
    t.insert("orient", std::move(quat));

    t.insert("obj_flags", o.obj_flags);
    t.insert("host_uid", o.host_uid);

    toml::array ang{o.ang_momentum.x, o.ang_momentum.y, o.ang_momentum.z};
    t.insert("ang_momentum", std::move(ang));

    t.insert("physics_flags", o.physics_flags);
    t.insert("skin_name", o.skin_name);

    return t;
}

static toml::table make_entity_table(const asg::SavegameEntityDataBlock& e)
{
    toml::table t = make_object_table(e.obj);

    t.insert("current_primary_weapon", e.current_primary_weapon);
    t.insert("current_secondary_weapon", e.current_secondary_weapon);
    t.insert("info_index", e.info_index);

    // ammo
    toml::array clip, amm;
    for (int i = 0; i < 32; ++i) {
        clip.push_back(e.weapons_clip_ammo[i]);
        amm.push_back(e.weapons_ammo[i]);
    }
    t.insert("weapons_clip_ammo", std::move(clip));
    t.insert("weapons_ammo", std::move(amm));

    t.insert("possesed_weapons_bitfield", e.possesed_weapons_bitfield);

    // hate_list
    toml::array hate;
    for (auto h : e.hate_list) hate.push_back(h);
    t.insert("hate_list", std::move(hate));

    // AI
    t.insert("ai_mode", e.ai_mode);
    t.insert("ai_submode", e.ai_submode);
    t.insert("move_mode", e.move_mode);
    t.insert("ai_mode_parm_0", e.ai_mode_parm_0);
    t.insert("ai_mode_parm_1", e.ai_mode_parm_1);
    t.insert("target_uid", e.target_uid);
    t.insert("look_at_uid", e.look_at_uid);
    t.insert("shoot_at_uid", e.shoot_at_uid);

    // compressed vectors
    toml::array ci_rot{e.ci_rot.x, e.ci_rot.y, e.ci_rot.z}, ci_move{e.ci_move.x, e.ci_move.y, e.ci_move.z};
    t.insert("ci_rot", std::move(ci_rot));
    t.insert("ci_move", std::move(ci_move));

    t.insert("corpse_carry_uid", e.corpse_carry_uid);
    t.insert("ai_flags", e.ai_flags);

    // vision & control
    toml::array eye_pos{e.eye_pos.x, e.eye_pos.y, e.eye_pos.z};
    t.insert("eye_pos", std::move(eye_pos));

    toml::array eye_orient;
    for (auto const& row : {e.eye_orient.rvec, e.eye_orient.uvec, e.eye_orient.fvec}) {
        toml::array r{row.x, row.y, row.z};
        eye_orient.push_back(std::move(r));
    }
    t.insert("eye_orient", std::move(eye_orient));

    t.insert("entity_flags", e.entity_flags);
    t.insert("entity_flags2", e.entity_flags2);

    // control
    auto pack_vec = [&](const rf::Vector3& v) {
        toml::array a{v.x, v.y, v.z};
        return a;
    };
    t.insert("control_data_phb", pack_vec(e.control_data_phb));
    t.insert("control_data_eye_phb", pack_vec(e.control_data_eye_phb));
    t.insert("control_data_local_vel", pack_vec(e.control_data_local_vel));

    return t;
}

static toml::table make_item_table(const asg::SavegameItemDataBlock& it)
{
    toml::table t = make_object_table(it.obj);
    t.insert("respawn_time_ms", it.respawn_time_ms);
    t.insert("respawn_next_timer", it.respawn_next_timer);
    t.insert("alpha", it.alpha);
    t.insert("create_time", it.create_time);
    t.insert("flags", it.flags);
    t.insert("item_cls_id", it.item_cls_id);
    return t;
}

static toml::table make_clutter_table(const asg::SavegameClutterDataBlock& c)
{
    toml::table t = make_object_table(c.obj);
    t.insert("delayed_kill_timestamp", c.delayed_kill_timestamp);
    t.insert("corpse_create_timestamp", c.corpse_create_timestamp);
    toml::array links;
    for (auto l : c.links) links.push_back(l);
    t.insert("links", std::move(links));
    return t;
}

static toml::table make_trigger_table(const asg::SavegameTriggerDataBlock& b)
{
    toml::table t;
    t.insert("uid", b.uid);
    t.insert("pos", toml::array{b.pos.x, b.pos.y, b.pos.z});
    t.insert("count", b.count);
    t.insert("time_last_activated", b.time_last_activated);
    t.insert("trigger_flags", b.trigger_flags);
    t.insert("activator_handle", b.activator_handle);
    t.insert("button_active_timestamp", b.button_active_timestamp);
    t.insert("inside_timestamp", b.inside_timestamp);
    toml::array links;
    for (auto l : b.links) links.push_back(l);
    t.insert("links", std::move(links));
    return t;
}

static toml::table make_bolt_emitters_table(const asg::SavegameLevelBoltEmitterDataBlock& b)
{
    toml::table t;
    t.insert("uid", b.uid);
    t.insert("active", b.active);
    return t;
}

static toml::table make_particle_emitters_table(const asg::SavegameLevelParticleEmitterDataBlock& b)
{
    toml::table t;
    t.insert("uid", b.uid);
    t.insert("active", b.active);
    return t;
}

static toml::table make_push_region_table(const asg::SavegameLevelPushRegionDataBlock& b)
{
    toml::table t;
    t.insert("uid", b.uid);
    t.insert("active", b.active);
    return t;
}

static toml::table make_event_table(const asg::SavegameEventDataBlock& ev)
{
    toml::table t;
    t.insert("event_type", ev.event_type);
    t.insert("uid", ev.uid);
    t.insert("delay", ev.delay);
    t.insert("is_on_state", ev.is_on_state);
    t.insert("delay_timer", ev.delay_timer);
    t.insert("activated_by_entity_uid", ev.activated_by_entity_uid);
    t.insert("activated_by_trigger_uid", ev.activated_by_trigger_uid);

    toml::array links;
    for (auto link_uid : ev.links) {
        links.push_back(link_uid);
    }
    t.insert("links", std::move(links));

    return t;
}

static toml::table make_make_invuln_event_table(const asg::SavegameEventMakeInvulnerableDataBlock& ev)
{
    toml::table t = make_event_table(ev.ev);

    t.insert("time_left", ev.time_left);
    return t;
}

static toml::table make_when_dead_event_table(const asg::SavegameEventWhenDeadDataBlock& ev)
{
    toml::table t = make_event_table(ev.ev);

    t.insert("message_sent", ev.message_sent);
    t.insert("when_any_dead", ev.when_any_dead);
    return t;
}

static toml::table make_goal_create_table(asg::SavegameEventGoalCreateDataBlock const& ev)
{
    toml::table t = make_event_table(ev.ev);

    t.insert("count", ev.count);
    return t;
}

static toml::table make_alarm_siren_table(asg::SavegameEventAlarmSirenDataBlock const& ev)
{
    toml::table t = make_event_table(ev.ev);

    t.insert("alarm_siren_playing", ev.alarm_siren_playing);
    return t;
}

static toml::table make_cyclic_timer_table(asg::SavegameEventCyclicTimerDataBlock const& ev)
{
    toml::table t = make_event_table(ev.ev);

    t.insert("next_fire_timer", ev.next_fire_timer);
    t.insert("send_count", ev.send_count);
    return t;
}

static toml::table make_geomod_crater_table(const rf::GeomodCraterData& c)
{
    toml::table ct;
    ct.insert("shape_index", c.shape_index);
    ct.insert("flags", c.flags);
    ct.insert("room_index", c.room_index);

    toml::array pos{c.pos.x, c.pos.y, c.pos.z};
    ct.insert("pos", std::move(pos));

    toml::array hn{c.hit_normal.x, c.hit_normal.y, c.hit_normal.z};
    ct.insert("hit_normal", std::move(hn));

    toml::array ori{c.orient.x, c.orient.y, c.orient.z, c.orient.w};
    ct.insert("orient", std::move(ori));

    ct.insert("scale", c.scale);
    return ct;
}

static toml::table make_persistent_goal_table(const asg::SavegameLevelPersistentGoalDataBlock& g)
{
    toml::table t;
    t.insert("goal_name", g.goal_name);
    t.insert("count", g.count);
    return t;
}

static toml::table make_decal_table(const asg::SavegameLevelDecalDataBlock& d)
{
    toml::table t;
    t.insert("pos", toml::array{d.pos.x, d.pos.y, d.pos.z});
    toml::array orient;
    for (auto const& row : {d.orient.rvec, d.orient.uvec, d.orient.fvec})
        orient.push_back(toml::array{row.x, row.y, row.z});
    t.insert("orient", std::move(orient));
    t.insert("width", toml::array{d.width.x, d.width.y, d.width.z});
    t.insert("bitmap_filename", d.bitmap_filename);
    t.insert("flags", d.flags);
    t.insert("alpha", d.alpha);
    t.insert("tiling_scale", d.tiling_scale);
    return t;
}

static toml::table make_mover_table(const asg::SavegameLevelKeyframeDataBlock& b)
{
    toml::table t = make_object_table(b.obj);

    t.insert("rot_cur_pos", b.rot_cur_pos);
    t.insert("start_at_keyframe", b.start_at_keyframe);
    t.insert("stop_at_keyframe", b.stop_at_keyframe);
    t.insert("mover_flags", b.mover_flags);
    t.insert("travel_time_seconds", b.travel_time_seconds);
    t.insert("rotation_travel_time_seconds", b.rotation_travel_time_seconds);
    t.insert("wait_timestamp", b.wait_timestamp);
    t.insert("trigger_uid", b.trigger_uid);
    t.insert("dist_travelled", b.dist_travelled);
    t.insert("cur_vel", b.cur_vel);
    t.insert("stop_completely_at_keyframe", b.stop_completely_at_keyframe);
    return t;
}

static toml::table make_weapon_table(const asg::SavegameLevelWeaponDataBlock& w)
{
    toml::table t = make_object_table(w.obj);

    t.insert("info_index", w.info_index);
    t.insert("life_left_seconds", w.life_left_seconds);
    t.insert("weapon_flags", w.weapon_flags);
    t.insert("sticky_host_uid", w.sticky_host_uid);
    t.insert("sticky_host_pos_offset",
        toml::array{w.sticky_host_pos_offset.x, w.sticky_host_pos_offset.y, w.sticky_host_pos_offset.z});

    toml::array sho;
    for (auto const& row : {w.sticky_host_orient.rvec, w.sticky_host_orient.uvec, w.sticky_host_orient.fvec}) {
        sho.push_back(toml::array{row.x, row.y, row.z});
    }
    t.insert("sticky_host_orient", sho);

    t.insert("weap_friendliness", static_cast<int64_t>(w.weap_friendliness));
    t.insert("target_uid", w.target_uid);
    t.insert("pierce_power_left", w.pierce_power_left);
    t.insert("thrust_left", w.thrust_left);
    t.insert("firing_pos", toml::array{w.firing_pos.x, w.firing_pos.y, w.firing_pos.z});

    return t;
}

static toml::table make_corpse_table(const asg::SavegameLevelCorpseDataBlock& c)
{
    toml::table t = make_object_table(c.obj);

    // corpse‐specific
    t.insert("create_time", c.create_time);
    t.insert("lifetime_seconds", c.lifetime_seconds);
    t.insert("corpse_flags", c.corpse_flags);
    t.insert("entity_type", c.entity_type);
    t.insert("pose_name", c.pose_name);
    t.insert("emitter_kill_timestamp", c.emitter_kill_timestamp);
    t.insert("body_temp", c.body_temp);

    t.insert("state_anim", c.state_anim);
    t.insert("action_anim", c.action_anim);
    t.insert("drop_anim", c.drop_anim);
    t.insert("carry_anim", c.carry_anim);
    t.insert("corpse_pose", c.corpse_pose);

    t.insert("helmet_name", c.helmet_name);
    t.insert("item_uid", c.item_uid);

    t.insert("body_drop_sound_handle", c.body_drop_sound_handle);

    // collision spheres if you like:
    t.insert("mass", c.mass);
    t.insert("radius", c.radius);
    toml::array spheres;
    for (auto const& s : c.cspheres) {
        toml::table st;
        st.insert("center", toml::array{s.center.x, s.center.y, s.center.z});
        st.insert("r", s.radius);
        spheres.push_back(std::move(st));
    }
    t.insert("collision_spheres", std::move(spheres));

    return t;
}

static toml::table make_blood_pool_table(const asg::SavegameLevelBloodPoolDataBlock& b)
{
    toml::table t;
    t.insert("pos", toml::array{b.pos.x, b.pos.y, b.pos.z});
    toml::array ori;
    for (auto const& row : {b.orient.rvec, b.orient.uvec, b.orient.fvec})
        ori.push_back(toml::array{row.x, row.y, row.z});
    t.insert("orient", std::move(ori));

    // RGBA as array
    toml::array col{b.pool_color.red, b.pool_color.green, b.pool_color.blue, b.pool_color.alpha};
    t.insert("pool_color", std::move(col));

    return t;
}

bool serialize_savegame_to_asg_file(const std::string& filename, const asg::SavegameData& d)
{
    toml::table root;

    // HEADER
    root.insert("_header", make_header_table(d.header));

    // COMMON
    toml::table common;
    common.insert("game", make_common_game_table(d.common.game));
    common.insert("player", make_common_player_table(d.common.player));
    root.insert("common", std::move(common));

    // LEVELS
    toml::array level_arr;
    for (auto const& lvl : d.levels) {
        toml::table lt = make_level_header_table(lvl.header);

        toml::array ent_arr, itm_arr, clu_arr;
        for (auto const& e : lvl.entities) ent_arr.push_back(make_entity_table(e));
        for (auto const& i : lvl.items) itm_arr.push_back(make_item_table(i));
        for (auto const& c : lvl.clutter) clu_arr.push_back(make_clutter_table(c));
        lt.insert("entities", std::move(ent_arr));
        lt.insert("items", std::move(itm_arr));
        lt.insert("clutter", std::move(clu_arr));

        toml::array trig_arr;
        for (auto const& tr : lvl.triggers) trig_arr.push_back(make_trigger_table(tr));
        lt.insert("triggers", std::move(trig_arr));

        toml::array gen_ev_arr;
        for (auto const& ev : lvl.other_events) {
            gen_ev_arr.push_back(make_event_table(ev));
        }
        lt.insert("events_generic", std::move(gen_ev_arr));

        toml::array invuln_arr;
        for (auto const& miev : lvl.make_invulnerable_events) {
            invuln_arr.push_back(make_make_invuln_event_table(miev));
        }
        lt.insert("events_make_invulnerable", std::move(invuln_arr));

        toml::array wd_arr;
        for (auto const& wdev : lvl.when_dead_events) {
            wd_arr.push_back(make_when_dead_event_table(wdev));
        }
        lt.insert("events_when_dead", std::move(wd_arr));

        toml::array gc_arr;
        for (auto const& gcev : lvl.goal_create_events) {
            gc_arr.push_back(make_goal_create_table(gcev));
        }
        lt.insert("events_goal_create", std::move(gc_arr));

        toml::array as_arr;
        for (auto const& asev : lvl.alarm_siren_events) {
            as_arr.push_back(make_alarm_siren_table(asev));
        }
        lt.insert("events_alarm_siren", std::move(as_arr));

        toml::array ct_arr;
        for (auto const& ctev : lvl.cyclic_timer_events) {
            ct_arr.push_back(make_cyclic_timer_table(ctev));
        }
        lt.insert("events_cyclic_timer", std::move(ct_arr));

        toml::array decal_arr;
        for (auto const& dec : lvl.decals) {
            decal_arr.push_back(make_decal_table(dec));
        }
        lt.insert("decals", std::move(decal_arr));

        toml::array killed_arr;
        for (int uid : lvl.killed_room_uids) {
            killed_arr.push_back(uid);
        }
        lt.insert("dead_room_uids", std::move(killed_arr));

        toml::array be_arr;
        for (auto const& be : lvl.bolt_emitters) be_arr.push_back(make_bolt_emitters_table(be));
        lt.insert("bolt_emitters", std::move(be_arr));

        toml::array pe_arr;
        for (auto const& pe : lvl.particle_emitters) pe_arr.push_back(make_particle_emitters_table(pe));
        lt.insert("particle_emitters", std::move(pe_arr));

        toml::array pr_arr;
        for (auto const& pr : lvl.push_regions) pr_arr.push_back(make_push_region_table(pr));
        lt.insert("push_regions", std::move(pr_arr));

        toml::array mov_arr;
        for (auto const& mov : lvl.movers) {
            mov_arr.push_back(make_mover_table(mov));
        }
        lt.insert("movers", std::move(mov_arr));

        toml::array weap_arr;
        for (auto const& w : lvl.weapons) weap_arr.push_back(make_weapon_table(w));
        lt.insert("weapons", std::move(weap_arr));

        toml::array corpse_arr;
        for (auto const& c : lvl.corpses) corpse_arr.push_back(make_corpse_table(c));
        lt.insert("corpses", std::move(corpse_arr));

        toml::array bp_arr;
        for (auto const& bp : lvl.blood_pools) bp_arr.push_back(make_blood_pool_table(bp));
        lt.insert("blood_pools", std::move(bp_arr));

        toml::array del_arr;
        for (int uid : lvl.deleted_event_uids) {
            del_arr.push_back(uid);
        }
        lt.insert("deleted_event_uids", std::move(del_arr));

        toml::array crater_arr;
        for (auto const& c : lvl.geomod_craters) crater_arr.push_back(make_geomod_crater_table(c));
        lt.insert("geomod_craters", std::move(crater_arr));

        toml::array pg_arr;
        for (auto const& pg : lvl.persistent_goals) {
            pg_arr.push_back(make_persistent_goal_table(pg));
        }
        lt.insert("persistent_goals", std::move(pg_arr));

        level_arr.push_back(std::move(lt));
    }
    root.insert("levels", std::move(level_arr));

    // write…
    std::ofstream ofs{filename};
    ofs << root;
    return bool(ofs);
}

// returns false on any missing fields
bool parse_asg_header(const toml::table& root, asg::SavegameHeader& out)
{
    xlog::warn("attempting to parse header for asg file");
    // grab the “_header” table
    if (auto hdr_node = root["_header"]; hdr_node && hdr_node.is_table()) {
        auto hdr = hdr_node.as_table();

        // simple scalars
        out.mod_name = (*hdr)["mod_name"].value_or(std::string{});
        out.game_time = (*hdr)["game_time"].value_or(0.f);
        out.current_level_filename = (*hdr)["current_level_filename"].value_or(std::string{});
        out.current_level_idx = (*hdr)["current_level_idx"].value_or(0);
        out.num_saved_levels = (*hdr)["num_saved_levels"].value_or(0);

        // array of strings
        out.saved_level_filenames.clear();
        if (auto arr = (*hdr)["saved_level_filenames"].as_array()) {
            out.saved_level_filenames.reserve(arr->size());
            for (auto& el : *arr)
                if (auto s = el.value<std::string>())
                    out.saved_level_filenames.push_back(*s);
        }
        return true;
    }
    return false;
}

bool sr_read_header_asg(const std::string& path, std::string& out_level, float& out_time)
{
    toml::table root;
    try {
        xlog::warn("parsing toml: {}", path);
        root = toml::parse_file(path);
    }
    catch (...) {
        xlog::warn("toml parse failed on {}", path);
        return false;
    }

    asg::SavegameHeader hdr;
    if (!parse_asg_header(root, hdr))
        return false;

    out_level = hdr.current_level_filename;
    out_time = hdr.game_time;
    return true;
}

bool parse_common_game(const toml::table& tbl, asg::SavegameCommonDataGame& out)
{
    out.difficulty = static_cast<rf::GameDifficultyLevel>(tbl["difficulty"].value_or(0));
    out.newest_message_index = tbl["newest_message_index"].value_or(0);
    out.num_logged_messages = tbl["num_logged_messages"].value_or(0);
    out.messages_total_height = tbl["messages_total_height"].value_or(0);

    out.messages.clear();
    if (auto arr = tbl["logged_messages"].as_array()) {
        for (auto& node : *arr) {
            if (!node.is_table())
                continue;
            auto m = *node.as_table();
            asg::AlpineLoggedHudMessage msg;
            msg.persona_index = m["speaker"].value_or(0);
            msg.time_string = m["time_string"].value_or(0);
            msg.display_height = m["display_height"].value_or(0);
            msg.message = m["message"].value_or(std::string{});
            out.messages.push_back(std::move(msg));
        }
    }
    return true;
}

bool parse_common_player(const toml::table& tbl, asg::SavegameCommonDataPlayer& out)
{
    out.entity_host_uid = tbl["entity_host_uid"].value_or(-1);
    out.spew_vector_index = tbl["spew_vector_index"].value_or(0);
    if (auto arr = tbl["spew_pos"].as_array()) {
        auto v = asg::parse_f32_array(*arr);
        if (v.size() == 3)
            out.spew_pos = {v[0], v[1], v[2]};
    }
    out.key_items = tbl["key_items"].value_or(0.f);
    out.view_obj_uid = tbl["view_obj_uid"].value_or(-1);
    out.grenade_mode = static_cast<uint8_t>(tbl["grenade_mode"].value_or(0));

    out.clip_x = tbl["clip_x"].value_or(0);
    out.clip_y = tbl["clip_y"].value_or(0);
    out.clip_w = tbl["clip_w"].value_or(0);
    out.clip_h = tbl["clip_h"].value_or(0);
    out.fov_h = tbl["fov_h"].value_or(0.f);
    out.player_flags = tbl["player_flags"].value_or(0);
    out.field_11f8 = static_cast<int16_t>(tbl["field_11f8"].value_or(tbl["field_36"].value_or(0)));
    out.entity_uid = tbl["entity_uid"].value_or(-1);
    xlog::warn("read entity_uid {}", out.entity_uid);
    out.entity_type = tbl["entity_type"].value_or(0);
    xlog::warn("read entity_type {}", out.entity_type);

    if (auto arr = tbl["weapon_prefs"].as_array()) {
        for (size_t i = 0; i < arr->size() && i < 32; ++i)
            out.weapon_prefs[i] = static_cast<uint8_t>((*arr)[i].value_or(0));
    }

    out.flags = static_cast<uint8_t>(tbl["flags"].value_or(0));

    // orient
    if (auto orient = tbl["fpgun_orient"].as_array()) {
        int i = 0;
        for (auto& row : *orient) {
            if (auto a = row.as_array()) {
                auto v = asg::parse_f32_array(*a);
                if (v.size() == 3) {
                    if (i == 0)
                        out.fpgun_orient.rvec = {v[0], v[1], v[2]};
                    if (i == 1)
                        out.fpgun_orient.uvec = {v[0], v[1], v[2]};
                    if (i == 2)
                        out.fpgun_orient.fvec = {v[0], v[1], v[2]};
                }
            }
            ++i;
        }
    }

    // pos
    if (auto pos = tbl["fpgun_pos"].as_array()) {
        auto v = asg::parse_f32_array(*pos);
        if (v.size() == 3)
            out.fpgun_pos = {v[0], v[1], v[2]};
    }

    return true;
}

bool parse_object(const toml::table& tbl, asg::SavegameObjectDataBlock& o)
{
    // basic ints
    o.uid = tbl["uid"].value_or(0);
    o.parent_uid = tbl["parent_uid"].value_or(-1);
    o.life = tbl["life"].value_or(100.0f);
    o.armor = tbl["armor"].value_or(0.0f);

    asg::parse_i16_vector(tbl, "pos", o.pos);
    asg::parse_i16_vector(tbl, "vel", o.vel);

    o.friendliness = tbl["friendliness"].value_or(0);
    o.host_tag_handle = tbl["host_tag_handle"].value_or(0);

    asg::parse_i16_quat(tbl, "orient", o.orient);

    o.obj_flags = tbl["obj_flags"].value_or(0);
    o.host_uid = tbl["host_uid"].value_or(-1);

    // ang_momentum [x,y,z] → Vector3
    if (auto a = tbl["ang_momentum"].as_array()) {
        auto v = asg::parse_f32_array(*a);
        if (v.size() == 3)
            o.ang_momentum = {v[0], v[1], v[2]};
    }

    o.physics_flags = tbl["physics_flags"].value_or(0);
    o.skin_name = tbl["skin_name"].value_or("");

    return true;
}

bool parse_entities(const toml::array& arr, std::vector<asg::SavegameEntityDataBlock>& out)
{
    out.clear();
    for (auto& node : arr) {
        if (!node.is_table())
            continue;
        auto tbl = *node.as_table();

        asg::SavegameEntityDataBlock e{};
        // 2a) object sub‐block
        parse_object(tbl, e.obj);

        // 2b) AI & weapon state
        e.current_primary_weapon = static_cast<uint8_t>(tbl["current_primary_weapon"].value_or(0));
        e.current_secondary_weapon = static_cast<uint8_t>(tbl["current_secondary_weapon"].value_or(0));
        e.info_index = tbl["info_index"].value_or(0);

        // ammo arrays
        if (auto ca = tbl["weapons_clip_ammo"].as_array()) {
            for (size_t i = 0; i < ca->size() && i < 32; ++i)
                e.weapons_clip_ammo[i] = static_cast<int16_t>((*ca)[i].value_or<int>(0));
        }
        if (auto aa = tbl["weapons_ammo"].as_array()) {
            for (size_t i = 0; i < aa->size() && i < 32; ++i)
                e.weapons_ammo[i] = static_cast<int16_t>((*aa)[i].value_or<int>(0));
        }

        e.possesed_weapons_bitfield = tbl["possesed_weapons_bitfield"].value_or(0);

        // hate list
        e.hate_list.clear();
        if (auto ha = tbl["hate_list"].as_array()) {
            for (auto& x : *ha)
                if (auto vv = x.value<int>())
                    e.hate_list.push_back(*vv);
        }

        // more AI…
        e.ai_mode = tbl["ai_mode"].value_or(0);
        e.ai_submode = tbl["ai_submode"].value_or(0);
        e.move_mode = tbl["move_mode"].value_or(0);
        e.ai_mode_parm_0 = tbl["ai_mode_parm_0"].value_or(0);
        e.ai_mode_parm_1 = tbl["ai_mode_parm_1"].value_or(0);
        e.target_uid = tbl["target_uid"].value_or(-1);
        e.look_at_uid = tbl["look_at_uid"].value_or(-1);
        e.shoot_at_uid = tbl["shoot_at_uid"].value_or(-1);

        // compressed-vector AI state
        if (auto a = tbl["ci_rot"].as_array()) {
            auto v = asg::parse_f32_array(*a);
            if (v.size() == 3)
                e.ci_rot = {v[0], v[1], v[2]};
        }
        if (auto a = tbl["ci_move"].as_array()) {
            auto v = asg::parse_f32_array(*a);
            if (v.size() == 3)
                e.ci_move = {v[0], v[1], v[2]};
        }

        e.corpse_carry_uid = tbl["corpse_carry_uid"].value_or(-1);
        e.ai_flags = tbl["ai_flags"].value_or(0);

        // vision & control
        if (auto a = tbl["eye_pos"].as_array()) {
            auto v = asg::parse_f32_array(*a);
            if (v.size() == 3)
                e.eye_pos = {v[0], v[1], v[2]};
        }
        if (auto eye = tbl["eye_orient"].as_array()) {
            int idx = 0;
            for (auto& row : *eye) {
                if (auto r = row.as_array()) {
                    auto v = asg::parse_f32_array(*r);
                    if (v.size() == 3) {
                        switch (idx) {
                        case 0:
                            e.eye_orient.rvec = {v[0], v[1], v[2]};
                            break;
                        case 1:
                            e.eye_orient.uvec = {v[0], v[1], v[2]};
                            break;
                        case 2:
                            e.eye_orient.fvec = {v[0], v[1], v[2]};
                            break;
                        }
                    }
                }
                ++idx;
            }
        }

        e.entity_flags = tbl["entity_flags"].value_or(0);
        e.entity_flags2 = tbl["entity_flags2"].value_or(0);

        // control_data vectors
        if (auto a = tbl["control_data_phb"].as_array()) {
            auto v = asg::parse_f32_array(*a);
            if (v.size() == 3)
                e.control_data_phb = {v[0], v[1], v[2]};
        }
        if (auto a = tbl["control_data_eye_phb"].as_array()) {
            auto v = asg::parse_f32_array(*a);
            if (v.size() == 3)
                e.control_data_eye_phb = {v[0], v[1], v[2]};
        }
        if (auto a = tbl["control_data_local_vel"].as_array()) {
            auto v = asg::parse_f32_array(*a);
            if (v.size() == 3)
                e.control_data_local_vel = {v[0], v[1], v[2]};
        }

        out.push_back(std::move(e));
    }
    return true;
}

static asg::SavegameEventDataBlock parse_event_base_fields(const toml::table& tbl)
{
    asg::SavegameEventDataBlock b;
    b.event_type = tbl["event_type"].value_or(-1);
    b.uid = tbl["uid"].value_or(-1);
    b.delay = tbl["delay"].value_or(0.0f);
    b.is_on_state = tbl["is_on_state"].value_or(false);    
    b.delay_timer = tbl["delay_timer"].value_or(-1);
    b.activated_by_entity_uid = tbl["activated_by_entity_uid"].value_or(-1);
    b.activated_by_trigger_uid = tbl["activated_by_trigger_uid"].value_or(-1);
    if (auto arr = tbl["links"].as_array()) {
        for (auto& v : *arr) b.links.push_back(v.value_or(-1));
    }
    return b;
}

static bool parse_generic_events(const toml::array& arr, std::vector<asg::SavegameEventDataBlock>& out)
{
    out.clear();
    out.reserve(arr.size());
    for (auto& node : arr) {
        if (!node.is_table())
            continue;
        out.push_back(parse_event_base_fields(*node.as_table()));
    }
    return true;
}

static bool parse_make_invuln_events(const toml::array& arr, std::vector<asg::SavegameEventMakeInvulnerableDataBlock>& out)
{
    out.clear();
    out.reserve(arr.size());
    for (auto& node : arr) {
        if (!node.is_table())
            continue;
        auto tbl = *node.as_table();
        asg::SavegameEventMakeInvulnerableDataBlock mb;
        mb.ev = parse_event_base_fields(tbl);
        mb.time_left = tbl["time_left"].value_or(-1);
        out.push_back(std::move(mb));
    }
    return true;
}

static bool parse_when_dead_events(const toml::array& arr, std::vector<asg::SavegameEventWhenDeadDataBlock>& out)
{
    out.clear();
    out.reserve(arr.size());
    for (auto& node : arr) {
        if (!node.is_table())
            continue;
        auto tbl = *node.as_table();
        asg::SavegameEventWhenDeadDataBlock ev;
        ev.ev = parse_event_base_fields(tbl);
        ev.message_sent = tbl["message_sent"].value_or(false);
        ev.when_any_dead = tbl["when_any_dead"].value_or(false);
        out.push_back(std::move(ev));
    }
    return true;
}

static bool parse_goal_create_events(const toml::array& arr, std::vector<asg::SavegameEventGoalCreateDataBlock>& out)
{
    out.clear();
    out.reserve(arr.size());
    for (auto& node : arr) {
        if (!node.is_table())
            continue;
        auto tbl = *node.as_table();
        asg::SavegameEventGoalCreateDataBlock ev;
        ev.ev = parse_event_base_fields(tbl);
        ev.count = tbl["count"].value_or(0);
        out.push_back(std::move(ev));
    }
    return true;
}

static bool parse_alarm_siren_events(const toml::array& arr, std::vector<asg::SavegameEventAlarmSirenDataBlock>& out)
{
    out.clear();
    out.reserve(arr.size());
    for (auto& node : arr) {
        if (!node.is_table())
            continue;
        auto tbl = *node.as_table();
        asg::SavegameEventAlarmSirenDataBlock ev;
        ev.ev = parse_event_base_fields(tbl);
        ev.alarm_siren_playing = tbl["alarm_siren_playing"].value_or(false);
        out.push_back(std::move(ev));
    }
    return true;
}

static bool parse_cyclic_timer_events(const toml::array& arr, std::vector<asg::SavegameEventCyclicTimerDataBlock>& out)
{
    out.clear();
    out.reserve(arr.size());
    for (auto& node : arr) {
        if (!node.is_table())
            continue;
        auto tbl = *node.as_table();
        asg::SavegameEventCyclicTimerDataBlock ev;
        ev.ev = parse_event_base_fields(tbl);
        ev.next_fire_timer = tbl["next_fire_timer"].value_or(-1);
        ev.send_count = tbl["send_count"].value_or(0);
        out.push_back(std::move(ev));
    }
    return true;
}

bool parse_clutter(const toml::array& arr, std::vector<asg::SavegameClutterDataBlock>& out)
{
    out.clear();
    for (auto& node : arr) {
        if (!node.is_table())
            continue;
        auto ct = *node.as_table();
        asg::SavegameClutterDataBlock cb;
        parse_object(ct, cb.obj);
        cb.delayed_kill_timestamp = ct["delayed_kill_timestamp"].value_or(-1);
        cb.corpse_create_timestamp = ct["corpse_create_timestamp"].value_or(-1);
        if (auto links = ct["links"].as_array())
            for (auto& v : *links)
                if (auto uid = v.value<int>())
                    cb.links.push_back(*uid);
        out.push_back(std::move(cb));
    }
    return true;
}

bool parse_items(const toml::array& arr, std::vector<asg::SavegameItemDataBlock>& out)
{
    out.clear();
    for (auto& node : arr) {
        if (!node.is_table())
            continue;
        auto tbl = *node.as_table();

        asg::SavegameItemDataBlock ib{};
        // 1) common object fields
        parse_object(tbl, ib.obj);

        // 2) item‐specific
        ib.respawn_time_ms = tbl["respawn_time_ms"].value_or(-1);
        ib.respawn_next_timer = tbl["respawn_next_timer"].value_or(-1);
        ib.alpha = tbl["alpha"].value_or(0);
        ib.create_time = tbl["create_time"].value_or(0);
        ib.flags = tbl["flags"].value_or(0);
        ib.item_cls_id = tbl["item_cls_id"].value_or(0);

        out.push_back(std::move(ib));
    }
    return true;
}

bool parse_triggers(const toml::array& arr, std::vector<asg::SavegameTriggerDataBlock>& out)
{
    out.clear();
    for (auto& node : arr) {
        if (!node.is_table())
            continue;
        auto tbl = *node.as_table();

        asg::SavegameTriggerDataBlock tb{};
        tb.uid = tbl["uid"].value_or(0);
        asg::parse_i16_vector(tbl, "pos", tb.pos);
        tb.count = tbl["count"].value_or(0);
        tb.time_last_activated = tbl["time_last_activated"].value_or(0);
        tb.trigger_flags = tbl["trigger_flags"].value_or(0);
        tb.activator_handle = tbl["activator_handle"].value_or(-1);
        tb.button_active_timestamp = tbl["button_active_timestamp"].value_or(-1);
        tb.inside_timestamp = tbl["inside_timestamp"].value_or(-1);

        // links
        if (auto la = tbl["links"].as_array()) {
            for (auto& v : *la)
                if (auto uid = v.value<int>())
                    tb.links.push_back(*uid);
        }

        out.push_back(std::move(tb));
    }
    return true;
}

bool parse_geomod_craters(const toml::array& arr, std::vector<rf::GeomodCraterData>& out)
{
    out.clear();
    for (auto& node : arr) {
        if (!node.is_table())
            continue;
        auto tbl = *node.as_table();

        rf::GeomodCraterData c{};
        c.shape_index = tbl["shape_index"].value_or(0);
        c.flags = tbl["flags"].value_or(0);
        c.room_index = tbl["room_index"].value_or(0);

        asg::parse_i16_vector(tbl, "pos", c.pos);
        asg::parse_i16_vector(tbl, "hit_normal", c.hit_normal);
        asg::parse_i16_quat(tbl, "orient", c.orient);

        c.scale = tbl["scale"].value_or(1.0f);

        out.push_back(std::move(c));
    }
    return true;
}

bool parse_bolt_emitters(const toml::array& arr, std::vector<asg::SavegameLevelBoltEmitterDataBlock>& out)
{
    out.clear();
    for (auto& node : arr) {
        if (!node.is_table())
            continue;
        auto tbl = *node.as_table();
        asg::SavegameLevelBoltEmitterDataBlock b{};
        b.uid = tbl["uid"].value_or(0);
        b.active = tbl["active"].value_or(false);
        out.push_back(std::move(b));
    }
    return true;
}

bool parse_particle_emitters(const toml::array& arr, std::vector<asg::SavegameLevelParticleEmitterDataBlock>& out)
{
    out.clear();
    for (auto& node : arr) {
        if (!node.is_table())
            continue;
        auto tbl = *node.as_table();
        asg::SavegameLevelParticleEmitterDataBlock p{};
        p.uid = tbl["uid"].value_or(0);
        p.active = tbl["active"].value_or(false);
        out.push_back(std::move(p));
    }
    return true;
}

bool parse_movers(const toml::array& arr, std::vector<asg::SavegameLevelKeyframeDataBlock>& out)
{
    out.clear();
    for (auto& node : arr) {
        if (!node.is_table())
            continue;
        auto tbl = *node.as_table();

        asg::SavegameLevelKeyframeDataBlock b{};
        // 1) common Object sub‐block
        parse_object(tbl, b.obj);

        // 2) mover‐specific fields
        b.rot_cur_pos = tbl["rot_cur_pos"].value_or(0.0f);
        b.start_at_keyframe = tbl["start_at_keyframe"].value_or(0);
        b.stop_at_keyframe = tbl["stop_at_keyframe"].value_or(0);
        b.mover_flags = static_cast<rf::MoverFlags>(tbl["mover_flags"].value_or(0));
        b.travel_time_seconds = tbl["travel_time_seconds"].value_or(0.0f);
        b.rotation_travel_time_seconds = tbl["rotation_travel_time_seconds"].value_or(0.0f);
        b.wait_timestamp = tbl["wait_timestamp"].value_or(-1);
        b.trigger_uid = tbl["trigger_uid"].value_or(-1);
        b.dist_travelled = tbl["dist_travelled"].value_or(0.0f);
        b.cur_vel = tbl["cur_vel"].value_or(0.0f);
        b.stop_completely_at_keyframe = tbl["stop_completely_at_keyframe"].value_or(0);

        out.push_back(std::move(b));
    }
    return true;
}

bool parse_push_regions(const toml::array& arr, std::vector<asg::SavegameLevelPushRegionDataBlock>& out)
{
    out.clear();
    for (auto& node : arr) {
        if (!node.is_table())
            continue;
        auto tbl = *node.as_table();

        asg::SavegameLevelPushRegionDataBlock p{};
        p.uid = tbl["uid"].value_or(0);
        p.active = tbl["active"].value_or(false);
        out.push_back(std::move(p));
    }
    return true;
}

bool parse_decals(const toml::array& arr, std::vector<asg::SavegameLevelDecalDataBlock>& out)
{
    out.clear();
    for (auto& node : arr) {
        if (!node.is_table())
            continue;
        auto tbl = *node.as_table();

        asg::SavegameLevelDecalDataBlock d{};
        // pos [x,y,z]
        if (auto pa = tbl["pos"].as_array()) {
            auto v = asg::parse_f32_array(*pa);
            if (v.size() == 3)
                d.pos = {v[0], v[1], v[2]};
        }

        // orient [ [rvec], [uvec], [fvec] ]
        if (auto oa = tbl["orient"].as_array()) {
            int i = 0;
            for (auto& rowNode : *oa) {
                if (auto ra = rowNode.as_array()) {
                    auto v = asg::parse_f32_array(*ra);
                    if (v.size() == 3) {
                        switch (i) {
                        case 0:
                            d.orient.rvec = {v[0], v[1], v[2]};
                            break;
                        case 1:
                            d.orient.uvec = {v[0], v[1], v[2]};
                            break;
                        case 2:
                            d.orient.fvec = {v[0], v[1], v[2]};
                            break;
                        }
                    }
                }
                ++i;
            }
        }

        // width [x,y,z]
        if (auto wa = tbl["width"].as_array()) {
            auto v = asg::parse_f32_array(*wa);
            if (v.size() == 3)
                d.width = {v[0], v[1], v[2]};
        }

        d.bitmap_filename = tbl["bitmap_filename"].value_or(std::string{});
        d.flags = tbl["flags"].value_or(0);
        {
            int alpha = tbl["alpha"].value_or(255);
            if (alpha < 0)
                alpha = 0;
            if (alpha > 255)
                alpha = 255;
            d.alpha = static_cast<uint8_t>(alpha);
        }
        d.tiling_scale = tbl["tiling_scale"].value_or(1.0f);

        out.push_back(std::move(d));
    }
    return true;
}

bool parse_weapons(const toml::array& arr, std::vector<asg::SavegameLevelWeaponDataBlock>& out)
{
    out.clear();
    out.reserve(arr.size());

    for (auto& node : arr) {
        if (!node.is_table())
            continue;

        const auto& tbl = *node.as_table();

        asg::SavegameLevelWeaponDataBlock b{};

        // 1) common ObjectSavegameBlock fields
        parse_object(tbl, b.obj);

        // 2) WeaponSavegameBlock fields (stock)
        b.info_index = tbl["info_index"].value_or(0);
        b.life_left_seconds = tbl["life_left_seconds"].value_or(0.0f);
        b.weapon_flags = tbl["weapon_flags"].value_or(0);
        b.sticky_host_uid = tbl["sticky_host_uid"].value_or(-1);

        // sticky_host_pos_offset [x,y,z]
        if (auto so = tbl["sticky_host_pos_offset"].as_array()) {
            auto v = asg::parse_f32_array(*so);
            if (v.size() == 3)
                b.sticky_host_pos_offset = {v[0], v[1], v[2]};
        }

        // sticky_host_orient as [[rvec],[uvec],[fvec]]
        if (auto o = tbl["sticky_host_orient"].as_array()) {
            int i = 0;
            for (auto& rowNode : *o) {
                if (auto ra = rowNode.as_array()) {
                    auto v = asg::parse_f32_array(*ra);
                    if (v.size() == 3) {
                        switch (i) {
                        case 0:
                            b.sticky_host_orient.rvec = {v[0], v[1], v[2]};
                            break;
                        case 1:
                            b.sticky_host_orient.uvec = {v[0], v[1], v[2]};
                            break;
                        case 2:
                            b.sticky_host_orient.fvec = {v[0], v[1], v[2]};
                            break;
                        default:
                            break;
                        }
                    }
                }
                ++i;
                if (i >= 3)
                    break;
            }
        }

        // stored as integer, clamp to uint8 range
        {
            int f = tbl["weap_friendliness"].value_or(0);
            if (f < 0)
                f = 0;
            if (f > 255)
                f = 255;
            b.weap_friendliness = static_cast<uint8_t>(f);
        }

        b.target_uid = tbl["target_uid"].value_or(-1);
        b.pierce_power_left = tbl["pierce_power_left"].value_or(0.0f);
        b.thrust_left = tbl["thrust_left"].value_or(0.0f);

        // firing_pos [x,y,z]
        if (auto fp = tbl["firing_pos"].as_array()) {
            auto v = asg::parse_f32_array(*fp);
            if (v.size() == 3)
                b.firing_pos = {v[0], v[1], v[2]};
        }

        out.push_back(std::move(b));
    }

    return true;
}


bool parse_blood_pools(const toml::array& arr, std::vector<asg::SavegameLevelBloodPoolDataBlock>& out)
{
    out.clear();
    for (auto& node : arr) {
        if (!node.is_table())
            continue;
        auto tbl = *node.as_table();

        asg::SavegameLevelBloodPoolDataBlock b{};
        // pos [x,y,z]
        if (auto pa = tbl["pos"].as_array()) {
            auto v = asg::parse_f32_array(*pa);
            if (v.size() == 3)
                b.pos = {v[0], v[1], v[2]};
        }

        // orient [[rvec],[uvec],[fvec]]
        if (auto oa = tbl["orient"].as_array()) {
            int i = 0;
            for (auto& rowNode : *oa) {
                if (auto ra = rowNode.as_array()) {
                    auto v = asg::parse_f32_array(*ra);
                    if (v.size() == 3) {
                        switch (i) {
                        case 0:
                            b.orient.rvec = {v[0], v[1], v[2]};
                            break;
                        case 1:
                            b.orient.uvec = {v[0], v[1], v[2]};
                            break;
                        case 2:
                            b.orient.fvec = {v[0], v[1], v[2]};
                            break;
                        }
                    }
                }
                ++i;
            }
        }

        // pool_color [r,g,b,a]
        if (auto ca = tbl["pool_color"].as_array()) {
            auto v = asg::parse_f32_array(*ca);
            if (v.size() == 4) {
                b.pool_color.red = v[0];
                b.pool_color.green = v[1];
                b.pool_color.blue = v[2];
                b.pool_color.alpha = v[3];
            }
        }

        out.push_back(std::move(b));
    }
    return true;
}

bool parse_corpses(const toml::array& arr, std::vector<asg::SavegameLevelCorpseDataBlock>& out)
{
    out.clear();
    for (auto& node : arr) {
        if (!node.is_table())
            continue;
        auto tbl = *node.as_table();

        asg::SavegameLevelCorpseDataBlock b{};
        // 1) common Object fields
        parse_object(tbl, b.obj);

        // 2) corpse‐specific fields
        b.create_time = tbl["create_time"].value_or(0);
        b.lifetime_seconds = tbl["lifetime_seconds"].value_or(0.0f);
        b.corpse_flags = tbl["corpse_flags"].value_or(0);
        b.entity_type = tbl["entity_type"].value_or(0);
        b.pose_name = tbl["pose_name"].value_or("");
        b.emitter_kill_timestamp = tbl["emitter_kill_timestamp"].value_or(-1);
        b.body_temp = tbl["body_temp"].value_or(0.0f);
        b.state_anim = tbl["state_anim"].value_or(0);
        b.action_anim = tbl["action_anim"].value_or(0);
        b.drop_anim = tbl["drop_anim"].value_or(0);
        b.carry_anim = tbl["carry_anim"].value_or(0);
        b.corpse_pose = tbl["corpse_pose"].value_or(0);
        b.helmet_name = tbl["helmet_name"].value_or("");
        b.item_uid = tbl["item_uid"].value_or(-1);
        b.body_drop_sound_handle = tbl["body_drop_sound_handle"].value_or(0);

        // optional collision spheres
        if (auto sa = tbl["collision_spheres"].as_array()) {
            for (auto& sn : *sa) {
                if (!sn.is_table())
                    continue;
                auto st = *sn.as_table();
                rf::PCollisionSphere s{};
                if (auto ca = st["center"].as_array()) {
                    auto v = asg::parse_f32_array(*ca);
                    if (v.size() == 3)
                        s.center = {v[0], v[1], v[2]};
                }
                s.radius = st["r"].value_or(0.0f);
                b.cspheres.push_back(std::move(s));
            }
        }

        // mass & radius
        b.mass = tbl["mass"].value_or(0.0f);
        b.radius = tbl["radius"].value_or(0.0f);

        out.push_back(std::move(b));
    }
    return true;
}

bool parse_killed_rooms(const toml::array& arr, std::vector<int>& out)
{
    out.clear();
    out.reserve(arr.size());
    std::unordered_set<int> seen;
    seen.reserve(arr.size());
    for (auto& v : arr) {
        if (auto uid = v.value<int>()) {
            if (seen.insert(*uid).second) {
                out.push_back(*uid);
            }
        }
    }
    return true;
}

bool parse_levels(const toml::table& root, std::vector<asg::SavegameLevelData>& outLevels)
{
    auto levels_node = root["levels"];
    if (!levels_node || !levels_node.is_array())
        return false;

    outLevels.clear();
    for (auto& lvl_node : *levels_node.as_array()) {
        if (!lvl_node.is_table())
            continue;

        auto tbl = *lvl_node.as_table();
        asg::SavegameLevelData lvl;

        // — Level header —
        lvl.header.filename = tbl["filename"].value_or(std::string{});
        lvl.header.level_time = tbl["level_time"].value_or(0.0);

        if (auto a = tbl["aabb_min"].as_array()) {
            auto v = asg::parse_f32_array(*a);
            if (v.size() == 3)
                lvl.header.aabb_min = {v[0], v[1], v[2]};
        }
        if (auto a = tbl["aabb_max"].as_array()) {
            auto v = asg::parse_f32_array(*a);
            if (v.size() == 3)
                lvl.header.aabb_max = {v[0], v[1], v[2]};
        }

        // — Entities —
        if (auto ents = tbl["entities"].as_array()) {
            parse_entities(*ents, lvl.entities);
        }

        // events
        if (auto ge = tbl["events_generic"].as_array())
            parse_generic_events(*ge, lvl.other_events);
        if (auto ie = tbl["events_make_invulnerable"].as_array())
            parse_make_invuln_events(*ie, lvl.make_invulnerable_events);
        if (auto a = tbl["events_when_dead"].as_array())
            parse_when_dead_events(*a, lvl.when_dead_events);
        if (auto a = tbl["events_goal_create"].as_array())
            parse_goal_create_events(*a, lvl.goal_create_events);
        if (auto a = tbl["events_alarm_siren"].as_array())
            parse_alarm_siren_events(*a, lvl.alarm_siren_events);
        if (auto a = tbl["events_cyclic_timer"].as_array())
            parse_cyclic_timer_events(*a, lvl.cyclic_timer_events);

        if (auto cls = tbl["clutter"].as_array()) {
            parse_clutter(*cls, lvl.clutter);
        }

        if (auto items = tbl["items"].as_array()) {
            parse_items(*items, lvl.items);
        }

        if (auto trs = tbl["triggers"].as_array()) {
            parse_triggers(*trs, lvl.triggers);
        }

        if (auto crater_arr = tbl["geomod_craters"].as_array()) {
            parse_geomod_craters(*crater_arr, lvl.geomod_craters);
        }

        if (auto be = tbl["bolt_emitters"].as_array()) {
            parse_bolt_emitters(*be, lvl.bolt_emitters);
        }

        if (auto pe = tbl["particle_emitters"].as_array()) {
            parse_particle_emitters(*pe, lvl.particle_emitters);
        }

        if (auto mv = tbl["movers"].as_array()) {
            parse_movers(*mv, lvl.movers);
        }

        if (auto pr = tbl["push_regions"].as_array()) {
            parse_push_regions(*pr, lvl.push_regions);
        }

        if (auto decs = tbl["decals"].as_array()) {
            parse_decals(*decs, lvl.decals);
        }

        if (auto we = tbl["weapons"].as_array()) {
            parse_weapons(*we, lvl.weapons);
        }

        if (auto bp = tbl["blood_pools"].as_array()) {
            parse_blood_pools(*bp, lvl.blood_pools);
        }

        if (auto cs = tbl["corpses"].as_array()) {
            parse_corpses(*cs, lvl.corpses);
        }

        if (auto kro = tbl["dead_room_uids"].as_array()) {
            parse_killed_rooms(*kro, lvl.killed_room_uids);
        }

        outLevels.push_back(std::move(lvl));
    }
    return true;
}

bool deserialize_savegame_from_asg_file(const std::string& filename, asg::SavegameData& out)
{
    // confirm requested asg file exists
    if (!std::filesystem::exists(filename)) {
        xlog::error("ASG file not found: {}", filename);
        return false;
    }

    // load asg file
    toml::table root;
    try {
        root = toml::parse_file(filename);
    }
    catch (const toml::parse_error& err) {
        xlog::error("Failed to parse ASG {}: {}", filename, err.what());
        return false;
    }

    // parse header
    if (!parse_asg_header(root, out.header)) {
        xlog::error("ASG header malformed or missing");
        return false;
    }

    // parse common
    if (auto common_tbl = root["common"].as_table()) {

        if (auto game_tbl = (*common_tbl)["game"].as_table()) {
            parse_common_game(*game_tbl, out.common.game);
        }
        else {
            xlog::error("Missing or invalid [common.game]");
            return false;
        }

        if (auto player_tbl = (*common_tbl)["player"].as_table()) {
            parse_common_player(*player_tbl, out.common.player);
        }
        else {
            xlog::error("Missing or invalid [common.player]");
            return false;
        }
    }

    if (!parse_levels(root, out.levels)) {
        xlog::error("ASG malformed or missing levels section");
        return false;
    }
    /*
    // 4) Common
    if (auto common_node = root["common"]; common_node && common_node.is_table()) {
        auto common_tbl = *common_node.as_table();

        // --- common.game ---
        if (auto game_node = common_tbl["game"]; game_node && game_node.is_table()) {
            auto game_tbl = *game_node.as_table();
            auto& cg = out.common.game;

            cg.difficulty = static_cast<rf::GameDifficultyLevel>(game_tbl["difficulty"].value_or(0));
            cg.newest_message_index = game_tbl["newest_message_index"].value_or(0);
            cg.num_logged_messages = game_tbl["num_logged_messages"].value_or(0);
            cg.messages_total_height = game_tbl["messages_total_height"].value_or(0);

            cg.messages.clear();
            if (auto msgs = game_tbl["logged_messages"].as_array()) {
                for (auto& m_node : *msgs) {
                    if (!m_node.is_table())
                        continue;
                    auto m_tbl = *m_node.as_table();
                    asg::AlpineLoggedHudMessage msg;
                    msg.persona_index = m_tbl["speaker"].value_or(0);
                    msg.time_string = m_tbl["time_string"].value_or(0);
                    msg.display_height = m_tbl["display_height"].value_or(0);
                    msg.message = m_tbl["message"].value_or(std::string{});
                    cg.messages.push_back(std::move(msg));
                }
            }
        }
        else {
            xlog::error("Missing or invalid [common.game]");
            return false;
        }

        // --- common.player ---
        if (auto player_node = common_tbl["player"]; player_node && player_node.is_table()) {
            auto player_tbl = *player_node.as_table();
            auto& cp = out.common.player;

            cp.entity_host_uid = player_tbl["entity_host_uid"].value_or(-1);
            cp.spew_vector_index = player_tbl["spew_vector_index"].value_or(0);

            if (auto arr = player_tbl["spew_pos"].as_array()) {
                auto v = asg::parse_f32_array(*arr);
                if (v.size() == 3)
                    cp.spew_pos = {v[0], v[1], v[2]};
            }

            cp.key_items = player_tbl["key_items"].value_or(0.f);
            cp.view_obj_uid = player_tbl["view_obj_uid"].value_or(-1);
            cp.grenade_mode = static_cast<uint8_t>(player_tbl["grenade_mode"].value_or(0));

            // fpgun_orient is an array-of-arrays [ [rvec], [uvec], [fvec] ]
            if (auto orient_arr = player_tbl["fpgun_orient"].as_array()) {
                int idx = 0;
                for (auto& row_node : *orient_arr) {
                    if (auto row = row_node.as_array()) {
                        auto v = asg::parse_f32_array(*row);
                        if (v.size() == 3) {
                            switch (idx) {
                            case 0:
                                cp.fpgun_orient.rvec = {v[0], v[1], v[2]};
                                break;
                            case 1:
                                cp.fpgun_orient.uvec = {v[0], v[1], v[2]};
                                break;
                            case 2:
                                cp.fpgun_orient.fvec = {v[0], v[1], v[2]};
                                break;
                            }
                        }
                    }
                    ++idx;
                }
            }

            // fpgun_pos [x,y,z]
            if (auto fp = player_tbl["fpgun_pos"].as_array()) {
                auto v = asg::parse_f32_array(*fp);
                if (v.size() == 3)
                    cp.fpgun_pos = {v[0], v[1], v[2]};
            }
        }
        else {
            xlog::error("Missing or invalid [common.player]");
            return false;
        }
    }
    else {
        xlog::error("Missing [common]");
        return false;
    }


    // 5) Levels
    if (auto levels_node = root["levels"]; levels_node && levels_node.is_array()) {
        auto& levels_arr = *levels_node.as_array();
        out.levels.clear();

        for (auto& lvl_node : levels_arr) {
            if (!lvl_node.is_table())
                continue;
            auto lvl_tbl = *lvl_node.as_table();
            asg::SavegameLevelData lvl;

            // ——— Level header ———
            lvl.header.filename = lvl_tbl["filename"].value_or(std::string{});
            lvl.header.level_time = lvl_tbl["level_time"].value_or(0.0);

            if (auto a = lvl_tbl["aabb_min"].as_array()) {
                auto v = asg::parse_f32_array(*a);
                if (v.size() == 3)
                    lvl.header.aabb_min = {v[0], v[1], v[2]};
            }
            if (auto a = lvl_tbl["aabb_max"].as_array()) {
                auto v = asg::parse_f32_array(*a);
                if (v.size() == 3)
                    lvl.header.aabb_max = {v[0], v[1], v[2]};
            }

            // ——— Entities ———
            lvl.entities.clear();
            if (auto ents = lvl_tbl["entities"].as_array()) {
                for (auto& e_node : *ents) {
                    if (!e_node.is_table())
                        continue;
                    auto et = *e_node.as_table();

                    asg::SavegameEntityDataBlock e{};
                    // object fields
                    e.obj.uid = et["uid"].value_or(0);
                    e.obj.parent_uid = et["parent_uid"].value_or(-1);
                    e.obj.life = et["life"].value_or(0);
                    e.obj.armor = et["armor"].value_or(0);

                    if(auto pa = et["pos"].as_array())
                    {
                        auto v = asg::parse_f32_array(*pa);
                        if (v.size() == 3) {
                            // build a full-precision vector then compress it
                            rf::Vector3 tmp{v[0], v[1], v[2]};
                            rf::compress_vector3(rf::world_solid, &tmp, &e.obj.pos);
                        }
                    }

                    if (auto va = et["vel"].as_array()) {
                        auto v = asg::parse_f32_array(*va);
                        if (v.size() == 3) {
                            rf::Vector3 tmp{v[0], v[1], v[2]};
                            // build a full-precision vector then compress it
                            rf::compress_velocity(&tmp, &e.obj.vel);
                        }
                    }
                    e.obj.friendliness = et["friendliness"].value_or(0);
                    e.obj.host_tag_handle = et["host_tag_handle"].value_or(0);
                    if (auto qa = et["orient"].as_array()) {
                        auto v = asg::parse_f32_array(*qa);
                        if (v.size() == 4) {
                            rf::Quaternion q;
                            q.x = v[0];
                            q.y = v[1];
                            q.z = v[2];
                            q.w = v[3];
                            e.obj.orient.from_quat(&q);
                        }
                    }
                    e.obj.obj_flags = et["obj_flags"].value_or(0);
                    e.obj.host_uid = et["host_uid"].value_or(-1);
                    // …and any other object sub-fields you serialized…

                    // AI + weapon state
                    e.current_primary_weapon = static_cast<uint8_t>(et["current_primary_weapon"].value_or(0));
                    e.current_secondary_weapon = static_cast<uint8_t>(et["current_secondary_weapon"].value_or(0));
                    e.info_index = et["info_index"].value_or(0);

                    if (auto ca = et["weapons_clip_ammo"].as_array()) {
                        auto v = asg::parse_f32_array(*ca);
                        for (size_t i = 0; i < v.size() && i < 32; ++i) e.weapons_clip_ammo[i] = static_cast<int>(v[i]);
                    }
                    if (auto aa = et["weapons_ammo"].as_array()) {
                        auto v = asg::parse_f32_array(*aa);
                        for (size_t i = 0; i < v.size() && i < 32; ++i) e.weapons_ammo[i] = static_cast<int>(v[i]);
                    }
                    e.possesed_weapons_bitfield = et["possesed_weapons_bitfield"].value_or(0);

                    // hate list
                    e.hate_list.clear();
                    if (auto ha = et["hate_list"].as_array()) {
                        for (auto& x : *ha)
                            if (auto v = x.value<int>())
                                e.hate_list.push_back(*v);
                    }

                    // AI modes, flags, etc.
                    e.ai_mode = static_cast<uint8_t>(et["ai_mode"].value_or(0));
                    e.ai_submode = static_cast<uint8_t>(et["ai_submode"].value_or(0));
                    e.move_mode = et["move_mode"].value_or(0);
                    e.ai_mode_parm_0 = et["ai_mode_parm_0"].value_or(0);
                    e.ai_mode_parm_1 = et["ai_mode_parm_1"].value_or(0);
                    e.target_uid = et["target_uid"].value_or(-1);
                    e.look_at_uid = et["look_at_uid"].value_or(-1);
                    e.shoot_at_uid = et["shoot_at_uid"].value_or(-1);
                    // …and so on for ci_rot, ci_move, corpse_carry_uid, ai_flags,
                    //     eye_pos/orient, entity_flags, control_data, etc.

                    lvl.entities.push_back(std::move(e));
                }
            }

            // ——— Items ———
            lvl.items.clear();
            if (auto items = lvl_tbl["items"].as_array()) {
                for (auto& i_node : *items) {
                    if (!i_node.is_table())
                        continue;
                    auto it = *i_node.as_table();
                    asg::SavegameItemDataBlock ib{};
                    ib.obj.uid = it["uid"].value_or(0);
                    // …fill the rest of ib.obj like above…
                    ib.respawn_timer = it["respawn_timer"].value_or(-1);
                    ib.alpha = it["alpha"].value_or(0);
                    ib.create_time = it["create_time"].value_or(0);
                    ib.flags = it["flags"].value_or(0);
                    ib.item_cls_id = it["item_cls_id"].value_or(0);
                    lvl.items.push_back(std::move(ib));
                }
            }

            // ——— Clutter ———
            lvl.clutter.clear();
            if (auto cls = lvl_tbl["clutter"].as_array()) {
                for (auto& c_node : *cls) {
                    if (!c_node.is_table())
                        continue;
                    auto ct = *c_node.as_table();
                    asg::SavegameClutterDataBlock cb{};
                    cb.obj.uid = ct["uid"].value_or(0);
                    // …fill cb.obj…
                    cb.delayed_kill_timestamp = ct["delayed_kill_timestamp"].value_or(-1);
                    cb.corpse_create_timestamp = ct["corpse_create_timestamp"].value_or(-1);
                    cb.links.clear();
                    if (auto la = ct["links"].as_array()) {
                        for (auto& x : *la)
                            if (auto v = x.value<int>())
                                cb.links.push_back(*v);
                    }
                    lvl.clutter.push_back(std::move(cb));
                }
            }

            // ——— Triggers ———
            lvl.triggers.clear();
            if (auto trs = lvl_tbl["triggers"].as_array()) {
                for (auto& t_node : *trs) {
                    if (!t_node.is_table())
                        continue;
                    auto tt = *t_node.as_table();
                    asg::SavegameTriggerDataBlock tb{};
                    tb.uid = tt["uid"].value_or(0);
                    if (auto pa = tt["pos"].as_array()) {
                        auto v = asg::parse_f32_array(*pa);
                        if (v.size() == 3) {
                            // build a full-precision vector then compress it
                            rf::Vector3 tmp{v[0], v[1], v[2]};
                            rf::compress_vector3(rf::world_solid, &tmp, &tb.pos);
                        }
                    }
                    tb.count = tt["count"].value_or(0);
                    tb.time_last_activated = tt["time_last_activated"].value_or(0);
                    tb.trigger_flags = tt["trigger_flags"].value_or(0);
                    tb.activator_handle = tt["activator_handle"].value_or(-1);
                    tb.button_active_timestamp = tt["button_active_timestamp"].value_or(-1);
                    tb.inside_timestamp = tt["inside_timestamp"].value_or(-1);
                    tb.links.clear();
                    if (auto la = tt["links"].as_array()) {
                        for (auto& x : *la)
                            if (auto v = x.value<int>())
                                tb.links.push_back(*v);
                    }
                    lvl.triggers.push_back(std::move(tb));
                }
            }

            // ——— “Generic” events, invulnerable, when_dead, goal_create, alarm_siren, cyclic_timer ———
            // use exactly the same pattern:
            //   auto evs = lvl_tbl["events_generic"].as_array(); for each table → fill SavegameEventDataBlock etc…

            // ——— Decals ———
            lvl.decals.clear();
            if (auto dcs = lvl_tbl["decals"].as_array()) {
                for (auto& d_node : *dcs) {
                    if (!d_node.is_table())
                        continue;
                    auto dt = *d_node.as_table();
                    asg::SavegameLevelDecalDataBlock db{};
                    if (auto pa = dt["pos"].as_array()) {
                        auto v = asg::parse_f32_array(*pa);
                        if (v.size() == 3)
                            db.pos = {v[0], v[1], v[2]};
                    }
                    // …orient, width, flags, alpha, tiling_scale, bitmap_filename…
                    lvl.decals.push_back(std::move(db));
                }
            }

            // ——— Dead room UIDs ———
            lvl.killed_room_uids.clear();
            if (auto kro = lvl_tbl["dead_room_uids"].as_array()) {
                for (auto& x : *kro)
                    if (auto v = x.value<int>())
                        lvl.killed_room_uids.push_back(*v);
            }

            // ——— Bolt‐emitters, particle‐emitters, push_regions, movers, weapons, corpses, blood_pools,
            // deleted_event_uids, persistent_goals, geomod_craters ——— …same as above: grab each array via as_array(),
            // loop, as_table(), fill your DataBlock…

            out.levels.push_back(std::move(lvl));
        }
    }
    else {
        xlog::error("Missing or invalid [levels] array");
        return false;
    }*/


    return true;
}

// save data to file when requested
FunHook<bool(const char* filename, rf::Player* pp)> sr_save_game_hook{
    0x004B3B30,
    [](const char *filename, rf::Player *pp) {

        if (g_alpine_game_config.use_new_savegame_format) {
            // use .asg extension
            std::filesystem::path p{filename};
            p.replace_extension(".asg");
            std::string newName = p.string();

            xlog::warn("writing new format save {} for player {}", newName, pp->name);
            rf::sr::g_disable_saving_persistent_goals = false;
            asg::SavegameData data = asg::build_savegame_data(pp);
            return serialize_savegame_to_asg_file(newName, data);
        }
        else {
            xlog::warn("writing legacy format save {} for player {}", filename, pp->name);
            return sr_save_game_hook.call_target(filename, pp);
        }
    }
};

FunHook<void()> do_quick_load_hook{
    0x004B5EB0,
    []() {
        if (g_alpine_game_config.use_new_savegame_format) {
            std::filesystem::path save_dir{ rf::sr::savegame_path };
            auto asg_path = save_dir / "quicksav.asg";
            std::string asg_file = asg_path.string();

            xlog::warn("loading new format quicksave from {}", asg_file);

            // 2) Read just the header to get the level name
            std::string level_name;
            float saved_time = 0.f;
            if (!sr_read_header_asg(asg_file, level_name, saved_time)) {
                xlog::error("Failed to parse ASG header from {}", asg_file);
                return; // malformed header
            }

            rf::level_set_level_to_load(level_name.c_str(), asg_file.c_str());
            rf::gameseq_set_state(rf::GameState::GS_NEW_LEVEL, 0);
        }
        else {
            xlog::warn("loading legacy format quicksave");
            do_quick_load_hook.call_target();
        }
    }
};

// save data to buffer during level transition
FunHook<void(rf::Player* pp)> sr_transitional_save_hook{
    0x004B52E0,
    [](rf::Player *pp) {

        if (g_alpine_game_config.use_new_savegame_format) {
            rf::sr::g_disable_saving_persistent_goals = true;

            size_t idx = asg::ensure_current_level_slot();
            xlog::warn("[ASG] transitional_save: slot #{} = {}", idx, g_save_data.header.saved_level_filenames[idx]);

            asg::serialize_all_objects(&g_save_data.levels[idx]);
            xlog::warn("[ASG] transitional_save: serialized level {}", g_save_data.levels[idx].header.filename);

            g_save_data.header.level_time_left = rf::level_time2;
        }
        else {
            sr_transitional_save_hook.call_target(pp);
        }
    }
};

// clear save data when launching a new playthrough
FunHook<void()> sr_reset_save_data_hook{
    0x004B52C0,
    []() {
        if (g_alpine_game_config.use_new_savegame_format) {
            
            // clear new save data global
            g_save_data = {};

            // clear legacy save data global
            sr_reset_save_data_hook.call_target();
        }
        else {
            // clear legacy save data global
            sr_reset_save_data_hook.call_target();
        }
    }
};

// save logged messages to buffer immediately when they're received from Message event
FunHook<void(const char* msg, int16_t persona_type)> hud_save_persona_msg_hook{
    0x00437FB0,
    [](const char* msg, int16_t persona_type) {

        if (g_alpine_game_config.use_new_savegame_format) {

            char buf[256];
            std::strncpy(buf, msg, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';

            // wrap
            int Count[6] = {};
            int start_idx[6] = {};
            double wrap_w = double(240 * rf::gr::clip_width()) * 0.0015625;
            int num_lines = rf::gr::split_str(Count, start_idx, buf, int(wrap_w), 6, 0, rf::hud_msg_font_num);

            // convert to safe data type after using built-in game functions to parse
            std::string wrapped;
            wrapped.reserve(std::strlen(buf) + num_lines);
            for (int i = 0; i < num_lines; ++i) {
                wrapped.append(buf + start_idx[i], Count[i]);
            }

            // pack into structure
            asg::AlpineLoggedHudMessage m;
            m.message = std::move(wrapped);
            //m.message = wrapped;
            m.time_string = int(std::time(nullptr));
            m.persona_index = persona_type;
            m.display_height = rf::gr::get_font_height(-1) * (num_lines + 1);

            // insert into array
            auto& cg = g_save_data.common.game;
            int slot;
            if (cg.messages.size() < 80) {
                cg.messages.push_back(std::move(m));
                slot = int(cg.messages.size()) - 1;
            }
            else {
                // overwrite oldest
                slot = (cg.newest_message_index + 1) % 80;

                // subtract the old height before overwriting
                cg.messages_total_height -= cg.messages[slot].display_height;
                cg.messages[slot] = std::move(m);
            }

            cg.messages_total_height += cg.messages[slot].display_height;
            cg.newest_message_index = slot;
            cg.num_logged_messages = int(cg.messages.size());

            xlog::warn("[ASG] saved HUD message to buffer: {}", msg);

            // maintain old code temporarily
            hud_save_persona_msg_hook.call_target(msg, persona_type);
        }
        else {
            hud_save_persona_msg_hook.call_target(msg, persona_type);
        }
    }
};

FunHook<bool(const char* filename, rf::Player* pp)> sr_load_level_state_hook{
    0x004B47A0,
    [](const char* filename, rf::Player* pp) {

        if (!g_alpine_game_config.use_new_savegame_format) {
            // fall back to the stock format
            return sr_load_level_state_hook.call_target(filename, pp);
        }

        // A small helper to do exactly what you do for the "auto." path:
        auto do_transition_load = [&](int slot_idx) -> bool {
            using namespace rf;
            // pause the game timer
            timer_inc_game_paused();

            auto& hdr = g_save_data.header;
            auto& lvl = g_save_data.levels[slot_idx];
            xlog::warn("geomod_craters size from save = {}", lvl.geomod_craters.size());
            // restore geomods
            num_geomods_this_level = lvl.geomod_craters.size();
            std::memcpy(geomods_this_level, lvl.geomod_craters.data(), sizeof(GeomodCraterData) * num_geomods_this_level);
            xlog::warn("restored {} geomods", num_geomods_this_level);
            levelmod_load_state();

            // restore world bounds
            world_solid->bbox_min = lvl.header.aabb_min;
            world_solid->bbox_max = lvl.header.aabb_max;

            // restore everything else
            deserialize_all_objects(&lvl);

            // if we're out of the “transition” state, create the player entity
            if (gameseq_get_state() != GS_LEVEL_TRANSITION) {
                asg::SavegameEntityDataBlock const* player_blk = nullptr;
                for (auto& e : lvl.entities) {
                    if (e.obj.uid == -999) {
                        player_blk = &e;
                        break;
                    }
                }
                if (!player_blk || !load_player(&g_save_data.common.player, pp, player_blk)) {
                    timer_dec_game_paused();
                    return false;
                }
                asg::resolve_delayed_handles();
                asg::clear_delayed_handles();

                if (auto o = obj_lookup_from_uid(-999); o && o->type == ObjectType::OT_ENTITY) {
                    physics_stick_to_ground(reinterpret_cast<Entity*>(o));
                }

                // restore difficulty
                game_set_skill_level(g_save_data.common.game.difficulty);
            }

            // final touches
            trigger_disable_all();
            timer_dec_game_paused();
            return true;
        };

        std::string fn = filename;
        auto path = std::filesystem::path(fn);

        // 1) the in-memory “auto.” buffer
        if (string_istarts_with(fn, "auto.")) {
            // find which slot matches the current level
            auto& hdr = g_save_data.header;
            std::string cur = string_to_lower(rf::level.filename);
            auto it = std::find(hdr.saved_level_filenames.begin(), hdr.saved_level_filenames.end(), cur);
            if (it == hdr.saved_level_filenames.end())
                return false;
            return do_transition_load(int(std::distance(hdr.saved_level_filenames.begin(), it)));
        }

        // 2) an on-disk “.asg” file
        if (path.extension() == ".asg") {
            // parse TOML into g_save_data
            if (!deserialize_savegame_from_asg_file(fn, g_save_data))
                return false;

            // now that g_save_data is populated, do exactly the same transition logic
            auto& hdr = g_save_data.header;
            std::string cur = string_to_lower(rf::level.filename);
            auto it = std::find(hdr.saved_level_filenames.begin(), hdr.saved_level_filenames.end(), cur);
            if (it == hdr.saved_level_filenames.end())
                return false;
            return do_transition_load(int(std::distance(hdr.saved_level_filenames.begin(), it)));
        }

        // fall back to allow loading legacy saves
        return sr_load_level_state_hook.call_target(filename, pp);
    }
};

CodeInjection event_delete_injection{
    0x004B67EC,
    [](auto& regs) {
        int deleted_uid = regs.ecx;
        g_deleted_event_uids.push_back(deleted_uid);
    },
};

CodeInjection event_shutdown_injection{
    0x004B65AA,
    []() {
        g_deleted_event_uids.clear();
    },
};

CodeInjection event_init_injection{
    0x004B66E5,
    []() {
        g_deleted_event_uids.clear();
        g_persistent_goals.clear();
    },
};

CodeInjection event_level_init_injection{
    0x004B6714,
    []() {
        g_deleted_event_uids.clear();
    },
};

FunHook<rf::PersistentGoalEvent*(const char* name)> event_lookup_persistent_goal_event_hook{
    0x004B8680,
    [](const char* name) {
        if (g_alpine_game_config.use_new_savegame_format) {
            for (auto& ev : g_persistent_goals) {
                if (string_iequals(ev.name, name)) {
                    return &ev;
                }
            }
            return static_cast<rf::PersistentGoalEvent*>(nullptr);
        }
        else {
            return event_lookup_persistent_goal_event_hook.call_target(name);
        }
    }
};

FunHook<void(const char* name, int initial_count, int current_count)> event_add_persistent_goal_event_hook{
    0x004B8610,
    [](const char* name, int initial_count, int current_count) {
        if (g_alpine_game_config.use_new_savegame_format) {
            for (auto& ev : g_persistent_goals) {
                    if (string_iequals(ev.name, name)) {
                        // update counts
                        ev.initial_count = initial_count;
                        ev.count = current_count;
                        return;
                    }
                }
                // not found, append
                rf::PersistentGoalEvent new_ev;
                new_ev.name          = name;
                new_ev.initial_count = initial_count;
                new_ev.count = current_count;
                g_persistent_goals.push_back(std::move(new_ev));
        }
        else {
            event_add_persistent_goal_event_hook.call_target(name, initial_count, current_count);
        }
    }
};

CodeInjection event_clear_persistent_goal_events_injection{
    0x004BDA10,
    []() {
        g_persistent_goals.clear();
    },
};

CodeInjection glass_shard_level_init_injection{
    0x00491064,
    []() {
        g_deleted_rooms.clear();
        g_deleted_room_uids.clear();
    },
};

// delete any rooms that have been marked for deletion
FunHook<void()> glass_delete_rooms_hook{
    0x004921A0,
    []() {
        if (g_alpine_game_config.use_new_savegame_format) {
            if (!rf::g_boolean_is_in_progress()) {
                for (auto room : g_deleted_rooms) {
                    //xlog::warn("killing room {}", room->uid);
                    rf::glass_delete_room(room);
                }
                g_deleted_rooms.clear();
            }
        }
        else {
            glass_delete_rooms_hook.call_target();
        }
    }
};

CodeInjection glass_delete_room_injection{
    0x00492306,
    [](auto& regs) {
        int uid = regs.edx;
        if (std::find(g_deleted_room_uids.begin(), g_deleted_room_uids.end(), uid) == g_deleted_room_uids.end()) {
            g_deleted_room_uids.push_back(uid);
        }
    },
};

// determine which specific faces to shatter
FunHook<bool(rf::GFace* f)> glass_face_can_be_destroyed_hook{
    0x00490BB0,
    [](rf::GFace* f) {

        // only shatter glass faces with 4 vertices
        if (!f || f->num_verts() != 4) {
            return false;
        }

        auto room = f->which_room;
        auto it = std::find(g_deleted_rooms.begin(), g_deleted_rooms.end(), room);
        if (it == g_deleted_rooms.end()) {
            g_deleted_rooms.push_back(room);
            //xlog::warn("deleting room {}", room->uid);
            return true;
        }
        else {
            //xlog::warn("already dead room {}", room->uid);
            return false;
        }
    }
};

CallHook<void(rf::Object* obj, rf::Vector3* pos, rf::Matrix3* orient)> game_restore_object_after_level_transition_hook{
    0x00435EE0, [](rf::Object* obj, rf::Vector3* pos, rf::Matrix3* orient) {
        xlog::warn("[ASG] restoring object {} after level transition, pos {},{},{}", obj->uid, pos->x, pos->y, pos->z);

        game_restore_object_after_level_transition_hook.call_target(obj, pos, orient);
    }
};

// todo: consider refactor to consolidate these with the similar functions in alpine_options.cpp
// helper: trim whitespace from both ends
static inline std::string trim(const std::string& s)
{
    auto ws = [](char c) { return std::isspace(static_cast<unsigned char>(c)); };
    auto b = s.begin(), e = s.end();
    b = std::find_if_not(b, e, ws);
    e = std::find_if_not(s.rbegin(), std::string::const_reverse_iterator(b), ws).base();
    return (b < e ? std::string(b, e) : std::string());
}

// helper: strip surrounding quotes if present
static inline std::string unquote(const std::string& s)
{
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

bool alpine_parse_ponr(const std::string& path)
{
    //xlog::warn("[PONR] Opening '{}'", path);
    rf::File tbl;
    if (tbl.open(path.c_str()) != 0) {
        //xlog::warn("[PONR]  -> file open failed");
        return false;
    }

    // read entire file into a string
    std::string content;
    std::string buf(2048, '\0');
    int got;
    while ((got = tbl.read(&buf[0], buf.size() - 1)) > 0) {
        buf.resize(got);
        content += buf;
        buf.resize(2048, '\0');
    }
    tbl.close();

     g_alpine_ponr.clear();
    std::istringstream in(content);
    std::string line;
    int line_no = 0;

    while (std::getline(in, line)) {
        ++line_no;
        std::string orig = line;
        line = trim(line);
        //xlog::warn("[PONR] Line {:3d}: '{}'", line_no, orig);

        // skip blank lines or comments
        if (line.empty() || line[0] == '#') {
            //xlog::warn("[PONR]  -> skipping blank/comment");
            continue;
        }

        // start a new block on "$Level:"
        if (line.rfind("$Level:", 0) == 0) {
            //xlog::warn("[PONR]  -> found $Level: directive");
            asg::AlpinePonr entry;

            // parse the filename after the colon
            auto pos = line.find(':');
            if (pos == std::string::npos) {
                xlog::warn("[PONR]     malformed $Level: line, no ':'");
                continue;
            }
            entry.current_level_filename = trim(line.substr(pos + 1));
            entry.current_level_filename = unquote(entry.current_level_filename);
            //xlog::warn("[PONR]     current_level_filename = '{}'", entry.current_level_filename);

            // expect next non-comment line: "$Levels to save:"
            while (std::getline(in, line)) {
                ++line_no;
                orig = line;
                line = trim(line);
                //xlog::warn("[PONR] Line {:3d}: '{}'", line_no, orig);

                if (line.empty() || line[0] == '#') {
                    //xlog::warn("[PONR]  -> skipping blank/comment");
                    continue;
                }
                if (line.rfind("$Levels to save:", 0) == 0) {
                    //xlog::warn("[PONR]  -> found $Levels to save:");
                    // grab the integer
                    int count = std::stoi(line.substr(line.find(':') + 1));
                    //xlog::warn("[PONR]     count = {}", count);
                    // read exactly `count` "+Save:" lines
                    for (int i = 0; i < count; ++i) {
                        if (!std::getline(in, line)) {
                            xlog::warn("[PONR]     unexpected EOF while reading +Save entries");
                            break;
                        }
                        ++line_no;
                        orig = line;
                        line = trim(line);
                        //xlog::warn("[PONR] Line {:3d}: '{}'", line_no, orig);

                        if (line.rfind("+Save:", 0) != 0) {
                            //xlog::warn("[PONR]     skipping non-+Save: line, retrying");
                            --i;
                            continue;
                        }
                        auto qpos = line.find(':');
                        std::string fn = trim(line.substr(qpos + 1));
                        fn = unquote(fn);
                        entry.levels_to_save.push_back(fn);
                        //xlog::warn("[PONR]     +Save entry[{}] = '{}'", i, fn);
                    }
                }
                else {
                    //xlog::warn("[PONR]  -> expected $Levels to save:, got '{}'", line);
                }
                break;
            }

            // store the block
            //xlog::warn("[PONR]  -> pushing AlpinePonr for '{}' ({} saves)", entry.current_level_filename, entry.levels_to_save.size());
            g_alpine_ponr.push_back(std::move(entry));
        }
        // optional exit on explicit "#End"
        else if (line == "#End") {
            //xlog::warn("[PONR]  -> encountered #End, stopping parse");
            break;
        }
        else {
            //xlog::warn("[PONR]  -> unrecognized directive, skipping");
        }
    }

    //xlog::warn("[PONR] Finished parsing, total entries = {}", g_alpine_ponr.size());
    return !g_alpine_ponr.empty();
}


FunHook<bool()> sr_parse_ponr_table_hook{
    0x004B36F0,
    []() {
         if (g_alpine_game_config.use_new_savegame_format) {
            if (alpine_parse_ponr("ponr.tbl")) {
                 //xlog::warn("Parsed {} entries from ponr.tbl", g_alpine_ponr.size());
                 return true;
            }
            return false;
        }
        else {
            return sr_parse_ponr_table_hook.call_target();
        }
    }
};

FunHook<int()> sr_get_num_logged_messages_hook{
    0x004B57A0,
    []() {
         if (g_alpine_game_config.use_new_savegame_format) {
            return g_save_data.common.game.num_logged_messages;
        }
        else {
            return sr_get_num_logged_messages_hook.call_target();
        }
    }
};

FunHook<int()> sr_get_logged_messages_total_height_hook{
    0x004B57E0,
    []() {
         if (g_alpine_game_config.use_new_savegame_format) {
            return g_save_data.common.game.messages_total_height;
        }
        else {
            return sr_get_logged_messages_total_height_hook.call_target();
        }
    }
};

FunHook<int()> sr_get_most_recent_logged_message_index_hook{
    0x004B57C0,
    []() {
         if (g_alpine_game_config.use_new_savegame_format) {
            return g_save_data.common.game.newest_message_index;
        }
        else {
            return sr_get_most_recent_logged_message_index_hook.call_target();
        }
    }
};

FunHook<rf::sr::LoggedHudMessage*(int index)> sr_get_logged_message_hook{
    0x004B5800,
    [](int index) {
         if (g_alpine_game_config.use_new_savegame_format) {
            int count = (int)g_save_data.common.game.messages.size();
            if (index < 0 || index >= count) {
                xlog::error("Failed to get logged message ID {}", index);
                return sr_get_logged_message_hook.call_target(index);
            }

            auto& alm = g_save_data.common.game.messages[index];
            auto& out = g_tmpLoggedMessages[index];

            memset(&out, 0, sizeof out);
            strncpy(out.message, alm.message.c_str(), sizeof(out.message) - 1);
            out.message[255] = '\0';
            out.time_string = alm.time_string;
            out.persona_index = alm.persona_index;
            out.display_height = alm.display_height;
            return &out;
        }
        else {
            return sr_get_logged_message_hook.call_target(index);
        }
    }
};

ConsoleCommand2 parse_ponr_cmd{
    "dbg_ponrparse",
    []() {
        rf::console::print("Parsing ponr.tbl...");
        if (!alpine_parse_ponr("ponr.tbl")) {
            rf::console::print("Failed to parse ponr.tbl");
            return;
        }
        const size_t total = g_alpine_ponr.size();
        rf::console::print("Parsed {} entries from ponr.tbl", total);
        if (total == 0)
            return;

        rf::console::print("Number of levels listed in PONR store for each level file:");

        std::string lineBuf;
        for (size_t i = 0; i < total; ++i) {
            const auto& e = g_alpine_ponr[i];
            if (i % 8 != 0)
                lineBuf += ", ";
            lineBuf += e.current_level_filename + "(" + std::to_string(e.levels_to_save.size()) + ")";

            // flush every 8 entries, or at end
            if ((i % 8 == 7) || (i == total - 1)) {
                rf::console::print("  {}", lineBuf);
                lineBuf.clear();
            }
        }
    },
    "Force a parse of ponr.tbl",
};

void alpine_savegame_apply_patch()
{
    game_restore_object_after_level_transition_hook.install();

    // handle serializing and saving asg files
    sr_save_game_hook.install();
    do_quick_load_hook.install();
    sr_transitional_save_hook.install();
    sr_reset_save_data_hook.install();

    // handle hud messages using new save buffer
    hud_save_persona_msg_hook.install();
    sr_get_num_logged_messages_hook.install();
    sr_get_logged_messages_total_height_hook.install();
    sr_get_most_recent_logged_message_index_hook.install();
    sr_get_logged_message_hook.install();

    // handle deserializing and loading asg files
    sr_load_level_state_hook.install();

    // handle new array for event deletion
    event_delete_injection.install();
    event_shutdown_injection.install();
    event_init_injection.install(); // handles both deleted events and persistent goals
    event_level_init_injection.install();

    // handle new array for persistent goals
    event_lookup_persistent_goal_event_hook.install();
    event_add_persistent_goal_event_hook.install();
    event_clear_persistent_goal_events_injection.install();

    // handle new array for deleted detail rooms (glass)
    glass_shard_level_init_injection.install();
    glass_delete_rooms_hook.install();
    glass_delete_room_injection.install();
    glass_face_can_be_destroyed_hook.install();

    // handle new ponr system
    sr_parse_ponr_table_hook.install();

    // console commands
    parse_ponr_cmd.register_cmd();
}
