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
#include <xlog/xlog.h>
#include "alpine_savegame.h"
#include "alpine_settings.h"
#include "../rf/os/array.h"
#include "../rf/gr/gr_font.h"
#include "../rf/hud.h"
#include "../rf/misc.h"
#include "../rf/event.h"
#include "../rf/object.h"
#include "../rf/entity.h"
#include "../rf/item.h"
#include "../rf/clutter.h"
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

// global buffer for deleted events, replacement for g_DeletedEventsUidArray and g_DeletedEventsUidArrayLen
static std::vector<int> g_deleted_event_uids;

// global buffer for persistent goals, replacement for g_persistent_goal_events and g_num_persistent_goal_events
static std::vector<rf::PersistentGoalEvent> g_persistent_goals;

// global buffer for deleted detail rooms, replacement for glass_deleted_rooms, num_killed_glass_room_uids, killed_glass_room_uids
static std::vector<rf::GRoom*> g_deleted_rooms;
static std::vector<int> g_deleted_room_uids;

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

    /// Ensure that `g_save_data` has a slot for the *current* level,
    /// dropping the oldest if we already have four.
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
            if (hdr.saved_level_filenames.size() >= 4) {
                // drop oldest
                hdr.saved_level_filenames.erase(hdr.saved_level_filenames.begin());
                levels.erase(levels.begin());
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
        levels[idx].header.filename = cur;
        levels[idx].header.level_time = level.time;
        levels[idx].header.aabb_min = rf::world_solid->bbox_min;
        levels[idx].header.aabb_max = rf::world_solid->bbox_max;

        return idx;
    }

    inline void serialize_player(rf::Player* pp, SavegameCommonDataPlayer& out)
    {
        if (auto ent = rf::entity_from_handle(pp->entity_handle))
            out.entity_host_uid = ent->uid;
        else
            out.entity_host_uid = -1;

        out.spew_vector_index = pp->spew_vector_index;
        out.spew_pos          = pp->spew_pos;

        {
            // replicate what the stock does: OR each bit into a uint32, then reinterpret
            uint32_t bits = 0;
            for (int i = 0; i < 32; ++i) {
                if (pp->key_items[i]) bits |= (1u << i);
            }
            out.key_items = *reinterpret_cast<float*>(&bits);
        }

        auto rf_obj = rf::obj_from_handle(pp->view_from_handle);
        rf_obj ? out.view_obj_uid = rf_obj->uid : out.view_obj_uid = -1;

        out.fpgun_pos    = *reinterpret_cast<const rf::Vector3*>(reinterpret_cast<const char*>(pp) + offsetof(rf::Player, shield_decals) + 12);
        out.fpgun_orient = *reinterpret_cast<const rf::Matrix3*>(reinterpret_cast<const char*>(pp) + offsetof(rf::Player, shield_decals) + 3 * sizeof(rf::Vector3));

        out.grenade_mode = *reinterpret_cast<const uint8_t*>(reinterpret_cast<const char*>(pp) + offsetof(rf::Player, shield_decals) + 22);
    }

    inline void fill_object_block(rf::Object* o, SavegameObjectDataBlock& out)
    {
        out.uid = o->uid;
        out.parent_uid = o->parent_handle; // may need to make this uid
        out.life = o->life;
        out.armor = o->armor;
        out.pos = rf::ShortVector::from(o->pos);
        out.vel = rf::ShortVector::from(o->p_data.vel);
        out.friendliness = o->friendliness;
        out.host_tag_handle = o->host_tag_handle;
        out.orient = o->orient;
        out.obj_flags = o->obj_flags;
        if (auto t = rf::obj_from_handle(o->host_handle))
            out.host_uid = t->uid;
        else
            out.host_uid = -1;
        out.ang_momentum = o->p_data.ang_momentum;
        out.physics_flags = o->p_data.flags;
        out.skin_name = "";
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
        b.move_mode = reinterpret_cast<uint8_t>(e->move_mode);
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
        b.respawn_timer = it->respawn_time_ms;
        b.alpha = *reinterpret_cast<int*>(&it->alpha);
        b.create_time = *reinterpret_cast<int*>(&it->create_time);
        b.flags = it->item_flags;
        b.item_cls_id = it->info_index;
        return b;
    }

    inline SavegameClutterDataBlock make_clutter_block(rf::Clutter* c)
    {
        SavegameClutterDataBlock b{};
        fill_object_block(c, b.obj);
        b.delayed_kill_timestamp = c->delayed_kill_timestamp.is_set() ? c->delayed_kill_timestamp.time_until() : -1;
        b.corpse_create_timestamp = c->corpse_create_timestamp.is_set() ? c->corpse_create_timestamp.time_until() : -1;

        b.links.clear();
        for (auto handle_ptr : c->links) {
            b.links.push_back(handle_ptr);
        }

        return b;
    }

    inline SavegameTriggerDataBlock make_trigger_block(rf::Trigger* t)
    {
        SavegameTriggerDataBlock b{};
        b.uid = t->uid;
        b.pos = rf::ShortVector::from(t->pos);
        b.count = t->count;
        b.time_last_activated = t->time_last_activated;
        b.trigger_flags = t->trigger_flags;
        b.activator_handle = uid_from_handle(t->activator_handle);
        b.button_active_timestamp = t->button_active_timestamp.is_set() ? t->button_active_timestamp.time_until() : -1;
        b.inside_timestamp = t->inside_timestamp.is_set() ? t->inside_timestamp.time_until() : -1;

        b.links.clear();
        for (auto handle_ptr : t->links) {
            b.links.push_back(handle_ptr);
        }

        return b;
    }

    inline SavegameEventDataBlock make_event_base_block(rf::Event* e)
    {
        SavegameEventDataBlock b{};
        b.event_type = e->event_type;
        b.uid = e->uid;
        b.delay = e->delay_seconds;
        b.is_on_state = e->delayed_msg;
        b.delay_timer = e->delay_timestamp.is_set() ? e->delay_timestamp.time_until() : -1;
        b.activated_by_entity_uid = uid_from_handle(e->triggered_by_handle);
        b.activated_by_trigger_uid = uid_from_handle(e->trigger_handle);

        b.links.clear();
        for (auto handle_ptr : e->links) {
            b.links.push_back(handle_ptr);
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
            m.time_left = event->make_invuln_timestamp.is_set() ? event->make_invuln_timestamp.time_until() : -1;
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
            m.next_fire_timer = event->next_fire_timestamp.is_set() ? event->next_fire_timestamp.time_until() : -1;
            m.send_count = event->send_count;

            // keeping the below around for later debugging - prints next 48 bytes after base event props
            /*
            auto* start = reinterpret_cast<uint8_t*>(event) + 693;
            // print the pointer itself in hex
            xlog::warn("[ASG] ---- CyclicTimerEvent @ {0:#x} ----", reinterpret_cast<uintptr_t>(event));

            for (int i = 0; i < 48; ++i) {
                // index in 2-digit decimal, byte in 0xHH hex
                xlog::warn("[ASG] +{0:02d} = {1:#04x}", i, start[i]);
            }
            */
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
        b.wait_timestamp = m->wait_timestamp.time_until();
        b.trigger_uid = uid_from_handle(m->trigger_handle);
        b.dist_travelled = m->dist_travelled;
        b.cur_vel = m->cur_vel;
        b.stop_completely_at_keyframe = m->stop_completely_at_keyframe;

        return b;
    }

    // serialize all entities into the given vector
    inline void serialize_all_entities(std::vector<SavegameEntityDataBlock>& out)
    {
        out.clear();
        for (rf::Entity* e = rf::entity_list.next;
            e != &rf::entity_list;
            e = e->next)
        {
            // todo: skip dead? need to investigate stock game behaviour to match
            // stock game omits bats and fish, would be good to have that mapper-configurable for optimization
            if (e) {
                xlog::warn("[ASG]   serializing entity {}", e->uid);
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
                xlog::warn("[ASG]   serializing item {}", i->uid);
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
                xlog::warn("[ASG]   serializing clutter {}", c->uid);
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
                xlog::warn("[ASG]   serializing trigger {}", t->uid);
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
                if (e->delay_timestamp.is_set()) {
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
        out = g_deleted_room_uids;
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

    inline void serialize_all_keyframes(std::vector<SavegameLevelKeyframeDataBlock>& out)
    {
        out.clear();
        rf::Mover* cur = rf::mover_list.next;
        while (cur && cur != &rf::mover_list) {
            out.push_back(make_keyframe_block(cur));
            cur = cur->next;
        }
    }

    void serialize_all_objects(SavegameLevelData* data)
    {
        // entities
        xlog::warn("[ASG]     populating entities for level '{}'", data->header.filename);
        data->entities.clear();
        serialize_all_entities(data->entities);
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

        xlog::warn("[ASG]     populating movers for level '{}'", data->header.filename);
        data->movers.clear();
        serialize_all_keyframes(data->movers);
        xlog::warn("[ASG]       got {} movers for level '{}'", int(data->movers.size()), data->header.filename);

        // geo craters
        int n = rf::num_geomods_this_level;
        xlog::warn("[ASG]     populating {} geomod_craters for level '{}'", n, data->header.filename);
        data->geomod_craters.clear();
        if (n > 0) {
            data->geomod_craters.resize(n);
            std::memcpy(data->geomod_craters.data(), rf::geomods_this_level, sizeof(rf::GeomodCraterData) * size_t(n));
        }
        xlog::warn("[ASG]       got {} geomod_craters for level '{}'", int(data->geomod_craters.size()), data->header.filename);
    }

    SavegameData build_savegame_data(rf::Player* pp)
    {
        SavegameData data;

        // ——— HEADER ———
        data.header.version = g_save_data.header.version;
        data.header.game_time = g_save_data.header.game_time;
        data.header.mod_name = g_save_data.header.mod_name;

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
} // namespace asg

static toml::table make_header_table(const asg::SavegameHeader& h)
{
    toml::table hdr;
    hdr.insert("asg_version", ASG_VERSION);
    hdr.insert("game_time", rf::level.global_time);
    hdr.insert("mod_name", rf::mod_param.found() ? rf::mod_param.get_arg() : "");
    hdr.insert("current_level_filename", h.current_level_filename);
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
    for (auto const& row : {o.orient.rvec, o.orient.uvec, o.orient.fvec}) {
        toml::array r{row.x, row.y, row.z};
        orient.push_back(std::move(r));
    }
    t.insert("orient", std::move(orient));

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
    t.insert("respawn_timer", it.respawn_timer);
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

        toml::array mov_arr;
        for (auto const& mov : lvl.movers) {
            mov_arr.push_back(make_mover_table(mov));
        }
        lt.insert("movers", std::move(mov_arr));

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
            asg::SavegameData data = asg::build_savegame_data(pp);
            return serialize_savegame_to_asg_file(newName, data);
        }
        else {
            xlog::warn("writing legacy format save {} for player {}", filename, pp->name);
            return sr_save_game_hook.call_target(filename, pp);
        }
    }
};

// save data to buffer during level transition
FunHook<void(rf::Player* pp)> sr_transitional_save_hook{
    0x004B52E0,
    [](rf::Player *pp) {

        if (g_alpine_game_config.use_new_savegame_format) {
            size_t idx = asg::ensure_current_level_slot();
            xlog::warn("[ASG] transitional_save: slot #{} = {}", idx, g_save_data.header.saved_level_filenames[idx]);

            asg::serialize_all_objects(&g_save_data.levels[idx]);
            xlog::warn("[ASG] transitional_save: serialized level {}", g_save_data.levels[idx].header.filename);

            // maintain old code temporarily
            sr_transitional_save_hook.call_target(pp);
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
                if (string_equals_ignore_case(ev.name, name)) {
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
                    if (string_equals_ignore_case(ev.name, name)) {
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
        g_deleted_room_uids.push_back(uid);
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

void alpine_savegame_apply_patch()
{
    sr_save_game_hook.install();
    sr_transitional_save_hook.install();
    sr_reset_save_data_hook.install();
    hud_save_persona_msg_hook.install();

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
}
