#include <algorithm>
#include <climits>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <common/utils/string-utils.h>
#include <xlog/xlog.h>
#include "asg_format.h"
#include "asg_internal.h"
#include "../bmpman/atx.h"
#include "../graphics/scene_capture.h"
#include "../graphics/weather.h"
#include "../misc/level.h"
#include "../misc/player.h"
#include "../object/alpine_dir_light.h"
#include "../object/event_alpine.h"
#include "../rf/clutter.h"
#include "../rf/entity.h"
#include "../rf/event.h"
#include "../rf/glare.h"
#include "../rf/level.h"
#include "../rf/object.h"
#include "../sound/ambience_region.h"

// WRLD blocks hold level-wide Alpine state, ALPO records the state of Alpine objects and regions by uid. Each layout
// is documented beside its field function, which serves capture and restore alike (asg::PayloadIo).
namespace
{
    // ─── WRLD ───

    // SKYB: i32 room_uid | i32 anchor_uid | f32 position_scale (Set_Skybox's override, -1/-1/0 for none)
    void skybox_block(asg::PayloadIo& io)
    {
        int room_uid = -1;
        int anchor_uid = -1;
        float position_scale = 0.0f;
        if (io.capturing()) {
            get_sky_room_uid_override(room_uid, anchor_uid, position_scale);
        }
        io.i32(room_uid, "room_uid");
        io.i32(anchor_uid, "anchor_uid");
        io.f32(position_scale, "position_scale");
        if (io.restoring()) {
            set_sky_room_uid_override(room_uid, anchor_uid, position_scale > 0.0f, position_scale);
        }
    }

    // FOG : color distance_fog_color | f32 distance_fog_far_clip. The near clip follows from the level file's.
    void fog_block(asg::PayloadIo& io)
    {
        rf::gr::Color color = rf::level.distance_fog_color;
        float far_clip = rf::level.distance_fog_far_clip;
        io.color(color, "color");
        io.f32(far_clip, "far_clip");
        if (io.restoring()) {
            rf::level.distance_fog_color = color;
            rf::level.distance_fog_far_clip = far_clip;
            level_sanitize_fog_near_clip();
        }
    }

    // HARD: i32 default_rock_hardness
    void hardness_block(asg::PayloadIo& io)
    {
        int hardness = rf::level.default_rock_hardness;
        io.i32(hardness, "hardness");
        if (io.restoring()) {
            rf::level.default_rock_hardness = std::clamp(hardness, 0, 100);
        }
    }

    // HLMP: b headlamp toggle enabled (Set_Gameplay_Rule)
    void headlamp_block(asg::PayloadIo& io)
    {
        bool enabled = g_headlamp_toggle_enabled;
        io.b(enabled, "enabled");
        if (io.restoring()) {
            set_headlamp_toggle_enabled(enabled);
        }
    }

    struct ClutterDebris
    {
        int index = -1;
        std::string cls;
        std::string debris_filename;
        std::string debris_sound_set;
        int explode_anim_vclip = -1;
        float explode_anim_radius = 0.0f;
        float debris_velocity = 0.0f;

        bool same_fields(const ClutterDebris& o) const
        {
            return debris_filename == o.debris_filename && debris_sound_set == o.debris_sound_set
                && explode_anim_vclip == o.explode_anim_vclip && explode_anim_radius == o.explode_anim_radius
                && debris_velocity == o.debris_velocity;
        }
    };

    struct EntityDebris
    {
        int index = -1;
        std::string cls;
        std::string debris_filename;
        int explode_vclip_index = -1;
        float explode_vclip_radius = 0.0f;

        bool same_fields(const EntityDebris& o) const
        {
            return debris_filename == o.debris_filename && explode_vclip_index == o.explode_vclip_index
                && explode_vclip_radius == o.explode_vclip_radius;
        }
    };

    ClutterDebris clutter_debris(int index)
    {
        const rf::ClutterInfo& ci = rf::clutter_types[index];
        return {index, ci.cls_name.c_str(), ci.debris_filename.c_str(), ci.debris_sound_set.c_str(),
                ci.explode_anim_vclip, ci.explode_anim_radius, ci.debris_velocity};
    }

    EntityDebris entity_debris(int index)
    {
        const rf::EntityInfo& ei = rf::entity_types[index];
        return {index, ei.name.c_str(), ei.debris_filename.c_str(), ei.explode_vclip_index, ei.explode_vclip_radius};
    }

    void apply_clutter_debris(int index, const ClutterDebris& d)
    {
        rf::ClutterInfo& ci = rf::clutter_types[index];
        ci.debris_filename = d.debris_filename.c_str();
        ci.debris_sound_set = d.debris_sound_set.c_str();
        ci.explode_anim_vclip = d.explode_anim_vclip;
        ci.explode_anim_radius = d.explode_anim_radius;
        ci.debris_velocity = d.debris_velocity;
    }

    void apply_entity_debris(int index, const EntityDebris& d)
    {
        rf::EntityInfo& ei = rf::entity_types[index];
        ei.debris_filename = d.debris_filename.c_str();
        ei.explode_vclip_index = d.explode_vclip_index;
        ei.explode_vclip_radius = d.explode_vclip_radius;
    }

    // The debris fields as the tables were parsed, taken before the first level loads (Set_Debris runs in levels)
    struct TblDebris
    {
        bool taken = false;
        std::vector<ClutterDebris> clutter;
        std::vector<EntityDebris> entities;
    };
    TblDebris g_tbl_debris;

    // The class at a saved index, when it still has the saved name; else the first class with that name
    template<typename Lookup, typename Name>
    int saved_class(int index, const std::string& cls, int count, Lookup&& lookup, Name&& name)
    {
        if (index >= 0 && index < count && string_iequals(name(index), cls)) {
            return index;
        }
        return lookup(cls.c_str());
    }

    // DEBR: list clutter classes {i32 index | str cls | str debris_filename | str debris_sound_set |
    // i32 explode_anim_vclip | f32 explode_anim_radius | f32 debris_velocity} | list entity classes {i32 index |
    // str cls | str debris_filename | i32 explode_vclip_index | f32 explode_vclip_radius}. Set_Debris writes these
    // class tables, which outlive the level; only the classes that differ from the parsed tables are kept.
    void debris_block(asg::PayloadIo& io)
    {
        std::vector<ClutterDebris> clutter;
        std::vector<EntityDebris> entities;
        if (io.capturing() && g_tbl_debris.taken) {
            for (int i = 0; i < rf::num_clutter_types && i < static_cast<int>(g_tbl_debris.clutter.size()); ++i) {
                if (ClutterDebris d = clutter_debris(i); !d.same_fields(g_tbl_debris.clutter[i])) {
                    clutter.push_back(std::move(d));
                }
            }
            for (int i = 0; i < rf::num_entity_types && i < static_cast<int>(g_tbl_debris.entities.size()); ++i) {
                if (EntityDebris d = entity_debris(i); !d.same_fields(g_tbl_debris.entities[i])) {
                    entities.push_back(std::move(d));
                }
            }
        }
        io.list(clutter, "clutter", [](asg::PayloadIo& io, ClutterDebris& d) {
            io.i32(d.index, "index");
            io.str(d.cls, "cls");
            io.str(d.debris_filename, "debris_filename");
            io.str(d.debris_sound_set, "debris_sound_set");
            io.i32(d.explode_anim_vclip, "explode_anim_vclip");
            io.f32(d.explode_anim_radius, "explode_anim_radius");
            io.f32(d.debris_velocity, "debris_velocity");
        });
        io.list(entities, "entities", [](asg::PayloadIo& io, EntityDebris& d) {
            io.i32(d.index, "index");
            io.str(d.cls, "cls");
            io.str(d.debris_filename, "debris_filename");
            io.i32(d.explode_vclip_index, "explode_vclip_index");
            io.f32(d.explode_vclip_radius, "explode_vclip_radius");
        });
        if (!io.restoring() || !io.ok()) {
            return;
        }
        // The saved differences are from the parsed tables, not from what this session has done to them
        if (g_tbl_debris.taken) {
            for (int i = 0; i < rf::num_clutter_types && i < static_cast<int>(g_tbl_debris.clutter.size()); ++i) {
                apply_clutter_debris(i, g_tbl_debris.clutter[i]);
            }
            for (int i = 0; i < rf::num_entity_types && i < static_cast<int>(g_tbl_debris.entities.size()); ++i) {
                apply_entity_debris(i, g_tbl_debris.entities[i]);
            }
        }
        for (const auto& d : clutter) {
            const int type = saved_class(d.index, d.cls, rf::num_clutter_types, rf::clutter_lookup_type,
                                         [](int i) { return rf::clutter_types[i].cls_name.c_str(); });
            if (type < 0) {
                xlog::warn("[ASG] Unknown clutter class '{}' in the saved debris settings", d.cls);
                continue;
            }
            apply_clutter_debris(type, d);
        }
        for (const auto& d : entities) {
            const int type = saved_class(d.index, d.cls, rf::num_entity_types, rf::entity_lookup_type,
                                         [](int i) { return rf::entity_types[i].name.c_str(); });
            if (type < 0) {
                xlog::warn("[ASG] Unknown entity class '{}' in the saved debris settings", d.cls);
                continue;
            }
            apply_entity_debris(type, d);
        }
    }

    // ATX : list controllers {str handle | i32 current_frame | i32 direction | b playing | f32 time_in_frame_s |
    // i32 base_frame_time_ms}
    void atx_block(asg::PayloadIo& io)
    {
        std::vector<AtxPlaybackState> states;
        if (io.capturing()) {
            states = atx_get_playback_states();
        }
        io.list(states, "atx", [](asg::PayloadIo& io, AtxPlaybackState& s) {
            io.str(s.handle, "handle");
            io.i32(s.current_frame, "current_frame");
            io.i32(s.direction, "direction");
            io.b(s.playing, "playing");
            io.f32(s.time_in_frame_s, "time_in_frame_s");
            io.i32(s.base_frame_time_ms, "base_frame_time_ms");
        });
        if (io.restoring()) {
            for (const auto& s : states) {
                if (!atx_set_playback_state(s)) {
                    xlog::warn("[ASG] Saved ATX '{}' is not loaded in the level", s.handle);
                }
            }
        }
    }

    // GASR: list regions {i32 uid | b enabled | i32 shape | f32 radius | f32 height | f32 width | f32 depth |
    // color | f32 density} | list transitions {i32 uid | hrt timer | b has_modify | color start_color |
    // color target_color | f32 start_density | f32 target_density | b has_resize | i32 target_shape |
    // f32 start_radius | f32 target_radius | f32 start_height | f32 start_width | f32 start_depth |
    // f32 target_height | f32 target_width | f32 target_depth}
    void gas_block(asg::PayloadIo& io)
    {
        std::vector<GasRegionInfo> regions;
        std::vector<GasRegionTransition> transitions;
        if (io.capturing()) {
            regions = gas_region_get_all();
            transitions = gas_region_get_transitions();
        }
        io.list(regions, "regions", [](asg::PayloadIo& io, GasRegionInfo& r) {
            io.i32(r.uid, "uid");
            io.b(r.enabled, "enabled");
            io.i32(r.shape, "shape");
            io.f32(r.radius, "radius");
            io.f32(r.height, "height");
            io.f32(r.width, "width");
            io.f32(r.depth, "depth");
            io.color(r.color, "color");
            io.f32(r.density, "density");
        });
        io.list(transitions, "transitions", [](asg::PayloadIo& io, GasRegionTransition& t) {
            io.i32(t.region_uid, "uid");
            io.hrt(t.timer, "timer");
            io.b(t.has_modify, "has_modify");
            io.color(t.start_color, "start_color");
            io.color(t.target_color, "target_color");
            io.f32(t.start_density, "start_density");
            io.f32(t.target_density, "target_density");
            io.b(t.has_resize, "has_resize");
            io.i32(t.target_shape, "target_shape");
            io.f32(t.start_radius, "start_radius");
            io.f32(t.target_radius, "target_radius");
            io.f32(t.start_height, "start_height");
            io.f32(t.start_width, "start_width");
            io.f32(t.start_depth, "start_depth");
            io.f32(t.target_height, "target_height");
            io.f32(t.target_width, "target_width");
            io.f32(t.target_depth, "target_depth");
        });
        if (!io.restoring()) {
            return;
        }
        for (const auto& r : regions) {
            GasRegionInfo* region = gas_region_get_by_uid(r.uid);
            if (!region) {
                xlog::warn("[ASG] Saved gas region {} is not in the level", r.uid);
                continue;
            }
            region->enabled = r.enabled;
            region->shape = r.shape;
            region->radius = r.radius;
            region->height = r.height;
            region->width = r.width;
            region->depth = r.depth;
            region->color = r.color;
            region->density = r.density;
        }
        std::erase_if(transitions, [](const GasRegionTransition& t) { return !gas_region_get_by_uid(t.region_uid); });
        gas_region_set_transitions(std::move(transitions));
    }

    struct WorldBlock
    {
        uint32_t tag;
        void (*fields)(asg::PayloadIo& io);
    };

    const WorldBlock world_blocks[] = {
        {asg::fourcc("SKYB"), skybox_block},
        {asg::fourcc("FOG "), fog_block},
        {asg::fourcc("HARD"), hardness_block},
        {asg::fourcc("HLMP"), headlamp_block},
        {asg::fourcc("DEBR"), debris_block},
        {asg::fourcc("ATX "), atx_block},
        {asg::fourcc("GASR"), gas_block},
    };

    const WorldBlock* find_world_block(uint32_t tag)
    {
        for (const auto& b : world_blocks) {
            if (b.tag == tag) {
                return &b;
            }
        }
        return nullptr;
    }

    // ─── ALPO ───

    enum AlpineObjectKind : uint8_t
    {
        alpo_object = 1,    // Alpine clutter: meshes and the anchors of coronas, ropes and projection cameras
        alpo_glares = 2,    // the glares of a clutter object, by its uid
        alpo_dir_light = 3,
        alpo_weather_region = 4,
        alpo_ambience_region = 5,
        alpo_climb_region = 6,
        alpo_projector = 7, // a projecting Display_Projection event, by its uid
    };

    struct AlpineObjectState
    {
        rf::Vector3 pos;
        rf::Matrix3 orient;
        float life = 0.0f;
        bool hidden = false;
        bool is_mesh = false;
        AlpineMeshSaveState mesh;
    };

    // object: v3 pos | m3 orient | f32 life | b hidden | b is_mesh | b has_anim | i32 anim_type |
    // str anim_filename | f32 blend_weight | b anim_paused | f32 anim_time | list textures {i32 slot | str texture} |
    // i32 collision | f32 scale | i32 corpse_of_uid. The mesh fields are defaults for an anchor. A rope is on while
    // its anchor is not hidden.
    void object_fields(asg::PayloadIo& io, AlpineObjectState& s)
    {
        io.v3(s.pos, "pos");
        io.m3(s.orient, "orient");
        io.f32(s.life, "life");
        io.b(s.hidden, "hidden");
        io.b(s.is_mesh, "is_mesh");
        auto& m = s.mesh;
        io.b(m.has_anim, "has_anim");
        io.i32(m.anim_type, "anim_type");
        io.str(m.anim_filename, "anim_filename");
        io.f32(m.blend_weight, "blend_weight");
        io.b(m.anim_paused, "anim_paused");
        io.f32(m.anim_time, "anim_time");
        io.list(m.textures, "textures", [](asg::PayloadIo& io, std::pair<int, std::string>& t) {
            io.i32(t.first, "slot");
            io.str(t.second, "texture");
        });
        io.i32(m.collision, "collision");
        io.f32(m.scale, "scale");
        io.i32(m.corpse_of_uid, "corpse_of_uid");
    }

    struct GlareState
    {
        bool enabled = true;
    };

    // glares: list {b enabled}, in glare list order
    void glare_fields(asg::PayloadIo& io, std::vector<GlareState>& glares)
    {
        io.list(glares, "glares", [](asg::PayloadIo& io, GlareState& g) { io.b(g.enabled, "enabled"); });
    }

    struct DirLightState
    {
        bool on = true;
        float color[3] = {1.0f, 1.0f, 1.0f};
    };

    // dir light: b on | f32 r | f32 g | f32 b
    void dir_light_fields(asg::PayloadIo& io, DirLightState& s)
    {
        io.b(s.on, "on");
        io.f32(s.color[0], "r");
        io.f32(s.color[1], "g");
        io.f32(s.color[2], "b");
    }

    // weather, ambience and climbing regions: b enabled
    void region_fields(asg::PayloadIo& io, bool& enabled)
    {
        io.b(enabled, "enabled");
    }

    // projector: empty
    void projector_fields(asg::PayloadIo&, int&) {}

    template<typename S>
    asg::AlpineObjectRecord make_record(uint8_t kind, int32_t uid, void (*fields)(asg::PayloadIo&, S&), S state)
    {
        return {kind, uid, asg::capture_payload([&](asg::PayloadIo& io) { fields(io, state); })};
    }

    template<typename S>
    bool read_record(const asg::AlpineObjectRecord& r, void (*fields)(asg::PayloadIo&, S&), S& state)
    {
        return asg::restore_payload(r.payload, nullptr, "Alpine object", r.uid,
                                    [&](asg::PayloadIo& io) { fields(io, state); });
    }

    bool is_alpine_object(const rf::Clutter& c)
    {
        return asg::is_alpine_clutter(c) && !(c.obj_flags & rf::OF_DELAYED_DELETE);
    }

    AlpineObjectState capture_object(const rf::Clutter& c)
    {
        AlpineObjectState s;
        s.pos = c.pos;
        s.orient = c.orient;
        s.life = c.life;
        s.hidden = (c.obj_flags & rf::OF_HIDDEN) != 0;
        s.is_mesh = alpine_mesh_export_state(&c, s.mesh);
        return s;
    }

    void apply_object(rf::Clutter& c, const AlpineObjectState& s)
    {
        asg::set_position(c, s.pos, s.orient);
        c.life = s.life;
        const bool hidden = (c.obj_flags & rf::OF_HIDDEN) != 0;
        if (s.hidden && !hidden) {
            rf::obj_hide(&c);
        }
        else if (!s.hidden && hidden) {
            rf::obj_unhide(&c);
        }
        if (s.is_mesh) {
            alpine_mesh_import_state(&c, s.mesh);
        }
    }

    // As Alpine clutter is level state the CLUT chunk leaves alone: a level object the save does not list was
    // destroyed, and a corpse mesh comes back from the mesh it replaced
    void restore_objects(const std::vector<const asg::AlpineObjectRecord*>& records)
    {
        std::unordered_map<int32_t, AlpineObjectState> saved;
        // A record whose payload is bad still says its object exists, which then keeps its level state
        std::unordered_set<int32_t> listed;
        for (const auto* r : records) {
            listed.insert(r->uid);
            AlpineObjectState s;
            if (read_record(*r, object_fields, s)) {
                saved.emplace(r->uid, std::move(s));
            }
        }
        std::vector<rf::Clutter*> missing;
        std::unordered_set<int32_t> matched;
        for (rf::Clutter* c = rf::clutter_list.next; c != &rf::clutter_list; c = c->next) {
            if (!is_alpine_object(*c)) {
                continue;
            }
            const auto it = saved.find(c->uid);
            if (it == saved.end()) {
                if (!listed.contains(c->uid)) {
                    missing.push_back(c);
                }
                continue;
            }
            apply_object(*c, it->second);
            matched.insert(c->uid);
        }
        for (const auto& [uid, s] : saved) {
            if (matched.contains(uid)) {
                continue;
            }
            const auto source = std::find_if(missing.begin(), missing.end(), [&](const rf::Clutter* c) {
                return s.is_mesh && c->uid == s.mesh.corpse_of_uid;
            });
            rf::Object* corpse = source != missing.end() ? alpine_mesh_respawn_corpse(*source, uid) : nullptr;
            if (corpse && corpse->type == rf::OT_CLUTTER) {
                apply_object(*static_cast<rf::Clutter*>(corpse), s);
            }
            else {
                xlog::warn("[ASG] Saved Alpine object {} is not in the level", uid);
            }
        }
        for (rf::Clutter* c : missing) {
            rf::obj_flag_dead(c);
        }
    }

    std::map<int32_t, std::vector<rf::Glare*>> glares_by_parent()
    {
        std::map<int32_t, std::vector<rf::Glare*>> out;
        for (rf::Glare* g = rf::glare_list.next; g != &rf::glare_list; g = g->next) {
            const rf::Object* parent = rf::obj_from_handle(g->parent_handle);
            if (parent && parent->type == rf::OT_CLUTTER && !(g->obj_flags & rf::OF_DELAYED_DELETE)) {
                out[parent->uid].push_back(g);
            }
        }
        return out;
    }

    // Uid-keyed state of things that can hold several entries per uid: the first is kept
    template<typename V, typename F>
    void add_unique(std::vector<asg::AlpineObjectRecord>& out, std::unordered_set<int32_t>& seen, const V& items,
                    F&& make)
    {
        seen.clear();
        for (const auto& item : items) {
            auto r = make(item);
            if (seen.insert(r.uid).second) {
                out.push_back(std::move(r));
            }
        }
    }

    // Applied once level_init_post has made the coronas' glares and the projections can render
    struct DeferredRestore
    {
        std::string level;
        std::vector<std::pair<int32_t, std::vector<GlareState>>> glares;
        std::vector<int32_t> projectors;
    };
    DeferredRestore g_deferred;
}

void asg::snapshot_class_debris()
{
    if (g_tbl_debris.taken) {
        return;
    }
    g_tbl_debris.taken = true;
    for (int i = 0; i < rf::num_clutter_types; ++i) {
        g_tbl_debris.clutter.push_back(clutter_debris(i));
    }
    for (int i = 0; i < rf::num_entity_types; ++i) {
        g_tbl_debris.entities.push_back(entity_debris(i));
    }
}

std::vector<asg::TaggedBlob> asg::capture_world()
{
    std::vector<TaggedBlob> out;
    for (const auto& b : world_blocks) {
        out.push_back({b.tag, capture_payload(b.fields)});
    }
    return out;
}

void asg::restore_world(const std::vector<TaggedBlob>& blocks, bool transition)
{
    for (const auto& block : blocks) {
        const WorldBlock* b = find_world_block(block.tag);
        if (!b) {
            xlog::warn("[ASG] Skipping unknown world block {:#x}", block.tag);
            continue;
        }
        // The class tables are the game's, not the level's: a transition keeps what the game has done to them
        if (transition && block.tag == fourcc("DEBR")) {
            continue;
        }
        restore_payload(block.data, nullptr, "world block", static_cast<int32_t>(block.tag), b->fields);
    }
}

std::vector<asg::AlpineObjectRecord> asg::capture_alpine_objects()
{
    std::vector<AlpineObjectRecord> out;
    for (rf::Clutter* c = rf::clutter_list.next; c != &rf::clutter_list; c = c->next) {
        if (is_alpine_object(*c)) {
            out.push_back(make_record(alpo_object, c->uid, object_fields, capture_object(*c)));
        }
    }
    for (const auto& [uid, glares] : glares_by_parent()) {
        std::vector<GlareState> states;
        for (const rf::Glare* g : glares) {
            states.push_back({g->enabled});
        }
        out.push_back(make_record(alpo_glares, uid, glare_fields, std::move(states)));
    }
    std::unordered_set<int32_t> seen;
    add_unique(out, seen, alpine_dir_light_get_all(), [](const AlpineDirLight& l) {
        DirLightState s{l.on, {l.color[0], l.color[1], l.color[2]}};
        return make_record(alpo_dir_light, l.uid, dir_light_fields, s);
    });
    const auto region = [](uint8_t kind) {
        return [kind](const std::pair<int, bool>& r) { return make_record(kind, r.first, region_fields, r.second); };
    };
    add_unique(out, seen, weather_get_region_states(), region(alpo_weather_region));
    add_unique(out, seen, ambience_region_get_states(), region(alpo_ambience_region));
    add_unique(out, seen, climb_region_get_states(), region(alpo_climb_region));
    for (int uid : projector_active_event_uids()) {
        out.push_back(make_record(alpo_projector, uid, projector_fields, 0));
    }
    return out;
}

void asg::add_runtime_alpine_uids(const std::vector<AlpineObjectRecord>& records, std::unordered_set<int32_t>& out)
{
    // A corpse mesh is the only Alpine object made at runtime
    for (const auto& r : records) {
        if (r.kind != alpo_object) {
            continue;
        }
        std::string error;
        Reader reader{r.payload, &error};
        PayloadIo io{reader, nullptr};
        AlpineObjectState s;
        object_fields(io, s);
        if (reader.ok() && s.is_mesh && s.mesh.corpse_of_uid != -1) {
            out.insert(r.uid);
        }
    }
}

void asg::clear_deferred_alpine_restore()
{
    g_deferred = {};
}

void asg::restore_alpine_objects(const std::vector<AlpineObjectRecord>& records)
{
    g_deferred = {rf::level.filename.c_str(), {}, {}};
    std::vector<const AlpineObjectRecord*> objects;
    for (const auto& r : records) {
        switch (r.kind) {
            case alpo_object:
                objects.push_back(&r);
                break;
            case alpo_glares: {
                std::vector<GlareState> glares;
                if (read_record(r, glare_fields, glares)) {
                    g_deferred.glares.emplace_back(r.uid, std::move(glares));
                }
                break;
            }
            case alpo_dir_light: {
                DirLightState s;
                if (read_record(r, dir_light_fields, s)
                    && (!alpine_dir_light_set_on(r.uid, s.on)
                        || !alpine_dir_light_set_color(r.uid, s.color[0], s.color[1], s.color[2]))) {
                    xlog::warn("[ASG] Saved directional light {} is not in the level", r.uid);
                }
                break;
            }
            case alpo_weather_region:
            case alpo_ambience_region:
            case alpo_climb_region: {
                bool enabled = true;
                if (!read_record(r, region_fields, enabled)) {
                    break;
                }
                bool found = true;
                if (r.kind == alpo_weather_region) {
                    found = weather_set_region_enabled(r.uid, enabled);
                }
                else if (r.kind == alpo_ambience_region) {
                    found = ambience_region_set_enabled(r.uid, enabled);
                }
                else if (climb_region_get_by_uid(r.uid)) {
                    climb_region_set_enabled(r.uid, enabled);
                }
                else {
                    found = false;
                }
                if (!found) {
                    xlog::warn("[ASG] Saved region {} (kind {}) is not in the level", r.uid, r.kind);
                }
                break;
            }
            case alpo_projector:
                g_deferred.projectors.push_back(r.uid);
                break;
            default:
                xlog::warn("[ASG] Skipping Alpine object {} of unknown kind {}", r.uid, r.kind);
                break;
        }
    }
    restore_objects(objects);
}

void asg::run_deferred_alpine_restore()
{
    DeferredRestore deferred = std::exchange(g_deferred, {});
    if (!string_iequals(deferred.level, rf::level.filename.c_str())) {
        return;
    }
    auto glares = glares_by_parent();
    for (const auto& [uid, states] : deferred.glares) {
        const auto it = glares.find(uid);
        if (it == glares.end()) {
            xlog::warn("[ASG] Saved glares of {} are not in the level", uid);
            continue;
        }
        for (size_t i = 0; i < states.size() && i < it->second.size(); ++i) {
            it->second[i]->enabled = states[i].enabled;
        }
    }
    for (int32_t uid : deferred.projectors) {
        rf::Event* e = rf::event_lookup_from_uid(uid);
        if (e && e->event_type == static_cast<int>(rf::EventType::Display_Projection)) {
            e->turn_on();
        }
        else {
            xlog::warn("[ASG] Saved projection {} is not in the level", uid);
        }
    }
}
