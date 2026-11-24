#pragma once

#include <climits>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "asg_format.h"
#include "asg_model.h"

class HighResTimer;

namespace rf
{
    struct Clutter;
    struct Entity;
    struct Object;
    struct Player;
    struct Timestamp;
    struct PersistentGoalEvent;
    namespace gr
    {
        struct Color;
        struct Light;
        struct LevelLight;
    }
}

// Shared by the capture, restore and integration code of the Alpine savegame
namespace asg
{
    constexpr int32_t player_entity_uid = -999;
    // Custom mode and silencer state are per-weapon bits of an int
    constexpr int num_weapon_bits = sizeof(int) * CHAR_BIT;

    // Records by uid
    template<typename R>
    std::unordered_map<int32_t, const R*> index_by_uid(const std::vector<R>& records)
    {
        std::unordered_map<int32_t, const R*> out;
        for (const auto& r : records) {
            if constexpr (requires { r.base.uid; }) {
                out.emplace(r.base.uid, &r);
            }
            else {
                out.emplace(r.uid, &r);
            }
        }
        return out;
    }

    // Handle fields restored from uids. Restore code queues each field while it creates and updates objects;
    // resolve() then writes the handle of the object with that uid, or -1. Resolving again once more objects
    // exist (the player entity) rewrites every queued field.
    class RefResolver
    {
    public:
        void add(int32_t uid, int* handle);
        void resolve(bool warn_unresolved);

    private:
        struct Ref
        {
            int32_t uid;
            int* handle;
        };
        std::vector<Ref> m_refs;
    };

    void warn_bad_payload(const char* what, int32_t id, const std::string& error);

    // One field function serves capture (engine -> bytes) and restore (bytes -> engine) of an opaque payload.
    // Restore leaves fields missing at the end as they are.
    class PayloadIo
    {
    public:
        explicit PayloadIo(Writer& w) : m_w(&w), m_mode(Mode::capture) {}
        PayloadIo(Reader& r, RefResolver* refs) : m_r(&r), m_refs(refs), m_mode(Mode::restore) {}

        [[nodiscard]] bool capturing() const
        {
            return m_mode == Mode::capture;
        }

        [[nodiscard]] bool restoring() const
        {
            return m_mode == Mode::restore;
        }

        [[nodiscard]] bool ok() const
        {
            return !m_r || m_r->ok();
        }

        void b(bool& v, const char* n);
        void u8(uint8_t& v, const char* n);
        void i32(int& v, const char* n);
        void f32(float& v, const char* n);
        void str(std::string& v, const char* n);
        // 3 x f32 xyz
        void v3(rf::Vector3& v, const char* n);
        // 3 x v3 rvec, uvec, fvec
        void m3(rf::Matrix3& v, const char* n);
        // 4 x u8 rgba
        void color(rf::gr::Color& v, const char* n);
        // ts
        void ts(rf::Timestamp& v, const char* n);
        // Wall-clock timer: b valid | i32 duration ms | i32 elapsed ms
        void hrt(HighResTimer& v, const char* n);
        // An object handle stored as its uid, resolved by the restore's RefResolver
        void handle(int& v, const char* n);
        // u32 count + handles as uids
        void handle_list(std::vector<int>& v, const char* n);

        template<typename E>
        void e32(E& v, const char* n)
        {
            int x = static_cast<int>(v);
            i32(x, n);
            if (restoring()) {
                v = static_cast<E>(x);
            }
        }

        // u32 count + items. Restore replaces the list with what it reads.
        template<typename V, typename F>
        void list(V& v, const char* n, F&& f)
        {
            uint32_t count = static_cast<uint32_t>(v.size());
            if (!list_count(count, n)) {
                return;
            }
            if (capturing()) {
                for (auto& item : v) {
                    f(*this, item);
                }
                return;
            }
            v.clear();
            for (uint32_t i = 0; i < count && ok(); ++i) {
                typename V::value_type item{};
                f(*this, item);
                v.push_back(std::move(item));
            }
        }

    private:
        enum class Mode
        {
            capture,
            restore,
        };

        bool list_count(uint32_t& count, const char* n);

        Writer* m_w = nullptr;
        Reader* m_r = nullptr;
        RefResolver* m_refs = nullptr;
        Mode m_mode;
    };

    // Runs a field function to capture a payload
    template<typename F>
    std::vector<uint8_t> capture_payload(F&& f)
    {
        Writer w;
        PayloadIo io{w};
        f(io);
        return w.take();
    }

    // Runs a field function over a payload to restore it; false (with a warning naming `what`) if it is malformed
    template<typename F>
    bool restore_payload(std::span<const uint8_t> data, RefResolver* refs, const char* what, int32_t id, F&& f)
    {
        std::string error;
        Reader r{data, &error};
        PayloadIo io{r, refs};
        f(io);
        if (!r.ok()) {
            warn_bad_payload(what, id, error);
        }
        return r.ok();
    }

    int32_t capture_timestamp(const rf::Timestamp& ts);
    void restore_timestamp(rf::Timestamp& ts, int32_t value);
    // Puts an object at a restored position with no motion between frames
    void set_position(rf::Object& obj, const rf::Vector3& pos, const rf::Matrix3& orient);
    int32_t uid_from_handle(int handle);
    // 1 for a uid made at runtime, 0 for one from the level file
    uint8_t uid_origin(int32_t uid);
    // Ambient bat and fish entities, which stock neither saves nor restores
    bool is_ambient_entity(const rf::Entity& e);
    // Clutter that is an Alpine object, which the clutter chunk leaves alone
    bool is_alpine_clutter(const rf::Clutter& c);
    rf::gr::Light* level_light_gr_light(const rf::gr::LevelLight& level_light);

    // asg_capture.cpp. with_goals: the persistent goals, which only a save to file keeps.
    LevelState capture_level(bool with_goals);
    void capture_player(rf::Player& player, PlayerState& out);
    void capture_game(GameState& out);

    // asg_restore.cpp. restore_level resolves the references it queued; restore_player and restore_game run on a
    // normal load only, not on a level transition.
    void restore_level(const LevelState& lvl, bool transition, RefResolver& refs);
    bool restore_player(const PlayerState& state, const EntityRecord& record, rf::Player& player, RefResolver& refs);
    void restore_game(const GameState& game);
    // From level_init_post, which gives the rooms their geoable flags, while restoring: carves the level's saved RF2
    // geomods, then puts the player back where the save has it
    void replay_rf2_geomods();

    // asg_events.cpp. Restoring runs while the level's event links still hold uids (0x00461187); the level loader
    // turns them into handles afterwards.
    std::vector<EventRecord> capture_events();
    void restore_events(const std::vector<EventRecord>& records, RefResolver& refs);

    // asg_alpine.cpp: WRLD blocks and ALPO records. Restoring ALPO leaves glares and projections to
    // run_deferred_alpine_restore, which level_init_post runs.

    // Keeps the class debris fields as parsed, which DEBR is stored against; runs before the first level loads
    void snapshot_class_debris();
    std::vector<TaggedBlob> capture_world();
    void restore_world(const std::vector<TaggedBlob>& blocks, bool transition);
    std::vector<AlpineObjectRecord> capture_alpine_objects();
    // The uids of the records' objects that were made at runtime
    void add_runtime_alpine_uids(const std::vector<AlpineObjectRecord>& records, std::unordered_set<int32_t>& out);
    void restore_alpine_objects(const std::vector<AlpineObjectRecord>& records);
    void run_deferred_alpine_restore();
    // Drops what a restore left for run_deferred_alpine_restore, which a failed load never runs
    void clear_deferred_alpine_restore();

    // asg.cpp
    std::vector<HudMessage>& hud_message_log();
    std::deque<rf::PersistentGoalEvent>& persistent_goals();
}
