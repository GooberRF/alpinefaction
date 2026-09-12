#include <cmath>
#include <cstring>
#include <common/utils/list-utils.h>
#include <xlog/xlog.h>
#include "obj_update_delta.h"
#include "alpine_packets.h"
#include "../rf/entity.h"
#include "../rf/object.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/weapon.h"
#include "../rf/os/timer.h"
#include "../os/os.h"

namespace obj_update_delta
{
    namespace
    {
        constexpr uint32_t terminator = 0xFFFFFFFF;
        constexpr uint8_t client_only_groups = 0x0A; // lag-comp 0x08 and recv-counter 0x02
        constexpr uint8_t stock_own_max_len = 4 + 1 + 1 + 1 + 3;
        constexpr uint8_t stock_max_len = stock_own_max_len + 22;

        enum Mask : uint8_t
        {
            pos_same = 0,
            pos_i8 = 1,   // 1/256 m, +-0.5 m per tick (50 m/s at 100 netfps)
            pos_i16 = 2,  // 1/256 m
            pos_float = 3,
            pos_mode_mask = 3,
            has_state = 1 << 2,
            has_vel = 1 << 3,
            has_ammo = 1 << 4,
        };

        int seq_delta(uint16_t a, uint16_t b)
        {
            return static_cast<int16_t>(static_cast<uint16_t>(a - b));
        }

        template<typename T>
        void put(uint8_t*& p, T v)
        {
            std::memcpy(p, &v, sizeof(v));
            p += sizeof(v);
        }

        template<typename T>
        bool get(const uint8_t*& p, const uint8_t* end, T& v)
        {
            if (end - p < static_cast<std::ptrdiff_t>(sizeof(v)))
                return false;
            std::memcpy(&v, p, sizeof(v));
            p += sizeof(v);
            return true;
        }

        // Stock record (server -> client layout) -> entry, inheriting absent groups from base
        Entry entry_from_stock(const uint8_t* rec, const Entry* base)
        {
            Entry e = base ? *base : Entry{};
            const uint8_t* p = rec;
            uint32_t handle;
            get(p, rec + stock_max_len, handle);
            e.handle = static_cast<int>(handle);
            const uint8_t flags = *p++;
            e.groups |= flags & 0xA5;
            if (flags & 0x01) {
                std::memcpy(&e.tick, p, 2);
                std::memcpy(e.pos, p + 2, 12);
                std::memcpy(&e.pitch, p + 14, 2);
                std::memcpy(&e.yaw, p + 16, 2);
                e.state = p[18];
                std::memcpy(e.vel, p + 19, 3);
                p += 22;
            }
            if (flags & 0x80)
                e.powerup = *p++;
            if (flags & 0x04)
                e.weapon = *p++;
            if (flags & 0x20) {
                e.health = p[0];
                e.armor = p[1];
                e.hitdir = p[2];
            }
            return e;
        }

        int entry_to_stock(const Entry& e, uint8_t flags, uint8_t* out)
        {
            uint8_t* p = out;
            put(p, static_cast<uint32_t>(e.handle));
            put(p, flags);
            if (flags & 0x01) {
                put(p, e.tick);
                std::memcpy(p, e.pos, 12);
                p += 12;
                put(p, e.pitch);
                put(p, e.yaw);
                put(p, e.state);
                std::memcpy(p, e.vel, 3);
                p += 3;
            }
            if (flags & 0x80)
                put(p, e.powerup);
            if (flags & 0x04)
                put(p, e.weapon);
            if (flags & 0x20) {
                put(p, e.health);
                put(p, e.armor);
                put(p, e.hitdir);
            }
            return static_cast<int>(p - out);
        }

        bool ammo_equal(const Entry& a, const Entry& b)
        {
            return a.has_ammo == b.has_ammo && a.ammo_type == b.ammo_type && a.clip == b.clip && a.reserve == b.reserve;
        }

        // cur as a full record (base == nullptr) or a delta against base; fire bits in flags pass through
        int encode(const Entry& cur, uint8_t flags, const Entry* base, uint8_t age, uint8_t index, uint8_t* out)
        {
            uint8_t* p = out;
            put(p, age);
            if (base)
                put(p, index);
            else
                put(p, static_cast<uint32_t>(cur.handle));

            uint8_t mask = pos_float;
            long q[3] = {};
            if (base) {
                if ((flags & 0x80) && cur.powerup == base->powerup)
                    flags &= ~0x80;
                if ((flags & 0x04) && cur.weapon == base->weapon)
                    flags &= ~0x04;
                if ((flags & 0x20) && cur.health == base->health && cur.armor == base->armor && cur.hitdir == base->hitdir)
                    flags &= ~0x20;
                if ((flags & 0x01) && (base->groups & 0x01)) {
                    bool fits8 = true, fits16 = true, same = true;
                    for (int i = 0; i < 3; ++i) {
                        const float d = cur.pos[i] - base->pos[i];
                        const long qi = std::lround(d * 256.0f);
                        fits8 = fits8 && qi >= -128 && qi <= 127;
                        fits16 = fits16 && qi >= -32768 && qi <= 32767;
                        same = same && qi == 0;
                        q[i] = qi;
                    }
                    mask = same ? pos_same : fits8 ? pos_i8 : fits16 ? pos_i16 : pos_float;
                }
                if (cur.state != base->state)
                    mask |= has_state;
                if (std::memcmp(cur.vel, base->vel, 3) != 0)
                    mask |= has_vel;
                if (cur.has_ammo && !ammo_equal(cur, *base))
                    mask |= has_ammo;
            }
            else {
                mask |= has_state | has_vel;
                if (cur.has_ammo)
                    mask |= has_ammo;
            }

            put(p, flags);
            put(p, mask);
            if (flags & 0x01) {
                put(p, cur.tick);
                switch (mask & pos_mode_mask) {
                case pos_i8:
                    for (long v : q)
                        put(p, static_cast<int8_t>(v));
                    break;
                case pos_i16:
                    for (long v : q)
                        put(p, static_cast<int16_t>(v));
                    break;
                case pos_float:
                    std::memcpy(p, cur.pos, 12);
                    p += 12;
                    break;
                default:
                    break;
                }
                put(p, cur.pitch);
                put(p, cur.yaw);
                if (mask & has_state)
                    put(p, cur.state);
                if (mask & has_vel) {
                    std::memcpy(p, cur.vel, 3);
                    p += 3;
                }
            }
            if (flags & 0x80)
                put(p, cur.powerup);
            if (flags & 0x04)
                put(p, cur.weapon);
            if (flags & 0x20) {
                put(p, cur.health);
                put(p, cur.armor);
                put(p, cur.hitdir);
            }
            if (mask & has_ammo) {
                put(p, cur.ammo_type);
                put(p, cur.clip);
                put(p, cur.reserve);
            }
            return static_cast<int>(p - out);
        }

        // Reads the record body after the age/index/handle prefix. `out` starts as a copy of base.
        bool decode_body(const uint8_t*& p, const uint8_t* end, const Entry* base, Entry& out, uint8_t& wire_flags)
        {
            uint8_t flags, mask;
            if (!get(p, end, flags) || !get(p, end, mask))
                return false;
            if (flags & 0x01) {
                if (!get(p, end, out.tick))
                    return false;
                switch (mask & pos_mode_mask) {
                case pos_same:
                    if (!base || !(base->groups & 0x01))
                        return false;
                    break;
                case pos_i8:
                    for (float& v : out.pos) {
                        int8_t d;
                        if (!get(p, end, d))
                            return false;
                        v += static_cast<float>(d) / 256.0f;
                    }
                    break;
                case pos_i16:
                    for (float& v : out.pos) {
                        int16_t d;
                        if (!get(p, end, d))
                            return false;
                        v += static_cast<float>(d) / 256.0f;
                    }
                    break;
                default:
                    if (end - p < 12)
                        return false;
                    std::memcpy(out.pos, p, 12);
                    p += 12;
                    break;
                }
                if (!get(p, end, out.pitch) || !get(p, end, out.yaw))
                    return false;
                if ((mask & has_state) && !get(p, end, out.state))
                    return false;
                if (mask & has_vel) {
                    if (end - p < 3)
                        return false;
                    std::memcpy(out.vel, p, 3);
                    p += 3;
                }
                out.groups |= 0x01;
            }
            if ((flags & 0x80) && !get(p, end, out.powerup))
                return false;
            if ((flags & 0x04) && !get(p, end, out.weapon))
                return false;
            if (flags & 0x20) {
                if (!get(p, end, out.health) || !get(p, end, out.armor) || !get(p, end, out.hitdir))
                    return false;
            }
            out.groups |= flags & 0xA4;
            if (mask & has_ammo) {
                if (!get(p, end, out.ammo_type) || !get(p, end, out.clip) || !get(p, end, out.reserve))
                    return false;
                out.has_ammo = true;
            }
            wire_flags = flags;
            return true;
        }

        void gather_ammo(rf::Entity* ep, Entry& e)
        {
            e.has_ammo = false;
            if (!ep || rf::entity_is_dying(ep))
                return;
            const int weapon = ep->ai.current_primary_weapon;
            if (weapon < 0 || weapon > 63)
                return;
            const int ammo_type = rf::weapon_types[weapon].ammo_type;
            if (ammo_type < 0 || ammo_type > 31)
                return;
            e.has_ammo = true;
            e.ammo_type = static_cast<uint8_t>(ammo_type);
            e.clip = static_cast<uint16_t>(ep->ai.clip_ammo[weapon]);
            e.reserve = static_cast<uint16_t>(ep->ai.ammo[ammo_type]);
        }

        const Entry* find_baseline(Sender& s, int handle, uint8_t& age, uint8_t& index)
        {
            for (int a = 1; a <= max_age; ++a) {
                const uint16_t seq = static_cast<uint16_t>(s.next_seq - a);
                const Snapshot* snap = s.ring.find(seq);
                if (!snap || !snap->acked)
                    continue;
                for (size_t i = 0; i < snap->entries.size() && i < 256; ++i) {
                    if (snap->entries[i].handle == handle) {
                        age = static_cast<uint8_t>(a);
                        index = static_cast<uint8_t>(i);
                        return &snap->entries[i];
                    }
                }
            }
            return nullptr;
        }

        // Client state
        Ring g_rx;
        uint16_t g_newest_seq = 0;
        bool g_any_received = false;

        // Client stream stats, logged every 5 s
        struct
        {
            int packets = 0, incomplete = 0, full = 0, delta = 0, age_sum = 0;
            size_t wire_bytes = 0, stock_bytes = 0;
            int last_log = 0;
        } g_stats;

        void log_stats(int now_ms)
        {
            if (now_ms - g_stats.last_log < 5000)
                return;
            if (g_stats.packets) {
                const int recs = g_stats.full + g_stats.delta;
                xlog::info("obj_update_delta: {} pkts ({} incomplete), {} full / {} delta records, avg age {:.1f}, "
                           "{} wire B vs {} stock B ({:.0f}%)",
                           g_stats.packets, g_stats.incomplete, g_stats.full, g_stats.delta,
                           g_stats.delta ? static_cast<float>(g_stats.age_sum) / static_cast<float>(g_stats.delta) : 0.0f,
                           g_stats.wire_bytes, g_stats.stock_bytes,
                           g_stats.stock_bytes ? 100.0f * static_cast<float>(g_stats.wire_bytes) / static_cast<float>(g_stats.stock_bytes) : 0.0f);
                (void)recs;
            }
            g_stats = {};
            g_stats.last_log = now_ms;
        }
    }

    Snapshot* Ring::find(uint16_t seq)
    {
        Snapshot& s = slots[seq % ring_size];
        return s.valid && s.seq == seq ? &s : nullptr;
    }

    void Ring::store(uint16_t seq, std::vector<Entry>&& entries)
    {
        Snapshot& s = slots[seq % ring_size];
        s.seq = seq;
        s.valid = true;
        s.acked = false;
        s.entries = std::move(entries);
    }

    void Ring::clear()
    {
        for (Snapshot& s : slots) {
            s.valid = false;
            s.acked = false;
            s.entries.clear();
        }
    }

    int server_pack(rf::Player* pp, rf::Entity* ep, uint8_t* rec, int len)
    {
        if (len < 5)
            return 0;
        const uint8_t flags = rec[4];
        if (flags & client_only_groups)
            return 0; // server -> client records never carry these
        Sender& s = pp->delta_sender;
        if (s.last)
            s.pending.push_back(*s.last); // the loop copied it into the packet after we returned
        uint8_t age = 0, index = 0;
        const Entry* base = find_baseline(s, static_cast<int>(*reinterpret_cast<uint32_t*>(rec)), age, index);
        Entry cur = entry_from_stock(rec, base);
        if (flags & 0x01) // own records (no keyframe) carry no ammo
            gather_ammo(ep, cur);
        uint8_t buf[64];
        const int n = encode(cur, flags, base, age, index, buf);
        // Store what the client will reconstruct (quantised), not what we sampled
        Entry visible = base ? *base : Entry{};
        visible.handle = cur.handle;
        const uint8_t* p = buf + (base ? 2 : 5);
        uint8_t wire_flags;
        if (!decode_body(p, buf + n, base, visible, wire_flags) || p != buf + n) {
            // Encoder/decoder disagree: never ship a record the client cannot mirror
            xlog::error("obj_update_delta: encode/decode mismatch for handle {:x}", cur.handle);
            return 0;
        }
        s.last = visible;
        std::memcpy(rec, buf, n);
        return n;
    }

    void server_send(rf::Player* pp, const uint8_t* packet, int len, bool final)
    {
        Sender& s = pp->delta_sender;
        if (final && s.last) {
            s.pending.push_back(*s.last);
            s.last.reset();
        }
        // stock: [0x26][u16 size][records][terminator]; ours adds seq after the size
        uint8_t out[rf::max_packet_size];
        uint8_t* p = out;
        put(p, static_cast<uint8_t>(af_packet_type::af_obj_update_delta));
        put(p, static_cast<uint16_t>(len - 3 + 2));
        put(p, s.next_seq);
        std::memcpy(p, packet + 3, len - 3);
        p += len - 3;
        rf::multi_io_send(pp, out, static_cast<int>(p - out));
        s.ring.store(s.next_seq, std::move(s.pending));
        s.pending.clear();
        ++s.next_seq;
    }

    void server_on_ack(rf::Player* pp, uint16_t newest_seq, uint32_t bits)
    {
        Sender& s = pp->delta_sender;
        if (seq_delta(s.next_seq, newest_seq) <= 0)
            return; // not something we sent
        if (Snapshot* snap = s.ring.find(newest_seq))
            snap->acked = true;
        for (int i = 0; i < 32; ++i) {
            if (!(bits & (1u << i)))
                continue;
            if (Snapshot* snap = s.ring.find(static_cast<uint16_t>(newest_seq - 1 - i)))
                snap->acked = true;
        }
    }

    void client_receive(const uint8_t* payload, size_t len, const rf::NetAddr& addr)
    {
        const uint8_t* p = payload;
        const uint8_t* end = payload + len;
        uint16_t seq;
        if (!get(p, end, seq))
            return;

        // Expanded stock packet fed to the engine parser; ~40 B per record vs >= 4 on the wire
        static uint8_t expanded[rf::max_packet_size * 12];
        uint8_t* q = expanded;
        put(q, static_cast<uint8_t>(RF_GPT_OBJECT_UPDATE));
        q += 2; // size, unused by the parser
        std::vector<Entry> entries;
        bool complete = true;
        while (true) {
            uint32_t head;
            if (!get(p, end, head)) {
                complete = false;
                break;
            }
            if (head == terminator)
                break;
            p -= 4;
            uint8_t age;
            get(p, end, age);
            const Entry* base = nullptr;
            Entry e;
            if (age == 0) {
                uint32_t handle;
                if (!get(p, end, handle)) {
                    complete = false;
                    break;
                }
                e.handle = static_cast<int>(handle);
            }
            else {
                uint8_t index;
                if (!get(p, end, index) || age > max_age) {
                    complete = false;
                    break;
                }
                Snapshot* snap = g_rx.find(static_cast<uint16_t>(seq - age));
                if (!snap || index >= snap->entries.size()) {
                    complete = false;
                    break;
                }
                base = &snap->entries[index];
                e = *base;
            }
            uint8_t wire_flags;
            if (!decode_body(p, end, base, e, wire_flags)) {
                complete = false;
                break;
            }
            if (q + stock_max_len + 4 > expanded + sizeof(expanded))
                break;
            q += entry_to_stock(e, wire_flags, q);
            entries.push_back(e);
            if (age)
                g_stats.delta++, g_stats.age_sum += age;
            else
                g_stats.full++;
            // Stock cost of this record: handle+flags, keyframe, slow groups, 10-byte af_obj_update ammo entry
            g_stats.stock_bytes += 5 + ((wire_flags & 0x01) ? 22 : 0) + 5 + (e.has_ammo ? 10 : 0);
        }
        put(q, terminator);
        const uint16_t size = static_cast<uint16_t>(q - expanded - 3);
        std::memcpy(expanded + 1, &size, 2);
        g_stats.packets++;
        g_stats.incomplete += !complete;
        g_stats.wire_bytes += len + 3;
        g_stats.stock_bytes += 7; // stock framing: header + terminator
        log_stats(static_cast<int>(timer::get_i64(1000)));

        rf::process_obj_update_packet(reinterpret_cast<char*>(expanded + 3), addr);

        // Ammo after the parser so the weapon-match rule sees the weapon this record set
        for (const Entry& e : entries) {
            if (!e.has_ammo)
                continue;
            rf::Object* obj = rf::obj_from_remote_handle(e.handle);
            rf::Entity* ep = obj ? rf::entity_from_handle(obj->handle) : nullptr;
            if (ep && ep != rf::local_player_entity)
                af_apply_remote_ammo(ep, e.weapon, e.ammo_type, e.clip, e.reserve);
        }

        if (!complete)
            return; // never acked: the server must not delta against a snapshot we lack
        g_rx.store(seq, std::move(entries));
        if (!g_any_received || seq_delta(seq, g_newest_seq) > 0)
            g_newest_seq = seq;
        g_any_received = true;
    }

    bool client_build_ack(uint16_t& newest_seq, uint32_t& bits)
    {
        if (!g_any_received)
            return false;
        newest_seq = g_newest_seq;
        bits = 0;
        for (int i = 0; i < 32; ++i) {
            if (g_rx.find(static_cast<uint16_t>(g_newest_seq - 1 - i)))
                bits |= 1u << i;
        }
        return true;
    }

    void client_reset()
    {
        g_rx.clear();
        g_any_received = false;
    }

    void on_level_init()
    {
        client_reset();
        if (rf::is_server) {
            for (rf::Player& p : SinglyLinkedList{rf::player_list}) {
                p.delta_sender.ring.clear();
                p.delta_sender.pending.clear();
                p.delta_sender.last.reset();
            }
        }
    }

    // Codec round trips; logs rather than asserts (Debug builds already pop enough dialogs)
    void selfcheck()
    {
        int failures = 0;
        const auto check = [&](bool ok, const char* what) {
            if (!ok) {
                ++failures;
                xlog::error("obj_update_delta selfcheck FAILED: {}", what);
            }
        };
        Entry a{};
        a.handle = 0x1234;
        a.pos[0] = 10.5f;
        a.pos[1] = -3.25f;
        a.pos[2] = 100.0f;
        a.pitch = -1000;
        a.yaw = 2000;
        a.state = 4;
        a.vel[0] = 5;
        a.health = 100;
        a.armor = 50;
        a.weapon = 3;
        a.has_ammo = true;
        a.ammo_type = 2;
        a.clip = 30;
        a.reserve = 200;
        uint8_t buf[64];
        uint8_t wf;
        // Full record: stock -> entry -> delta(full) -> entry round trip
        const uint8_t flags = 0x01 | 0x80 | 0x04 | 0x20 | 0x40;
        uint8_t stock[64];
        entry_to_stock(a, flags, stock);
        Entry a2 = entry_from_stock(stock, nullptr);
        a2.has_ammo = a.has_ammo; // the stock record carries no ammo; server_pack gathers it separately
        a2.ammo_type = a.ammo_type;
        a2.clip = a.clip;
        a2.reserve = a.reserve;
        int n = encode(a2, flags, nullptr, 0, 0, buf);
        Entry d{};
        d.handle = a.handle;
        const uint8_t* p = buf + 5;
        check(decode_body(p, buf + n, nullptr, d, wf) && p == buf + n && wf == flags, "full record decode");
        check(std::memcmp(d.pos, a.pos, 12) == 0 && d.weapon == 3 && d.clip == 30 && d.groups == 0xA5, "full record fields");
        // Deltas: each pos mode, slow-field elision, inheritance
        const float steps[] = {0.0f, 0.25f, 3.0f, 600.0f}; // same, i8, i16, float
        for (float step : steps) {
            Entry b = d;
            b.pos[0] += step;
            b.pos[2] -= step * 0.5f;
            b.tick = 77;
            n = encode(b, flags, &d, 3, 9, buf);
            check(buf[0] == 3 && buf[1] == 9, "delta header");
            Entry r = d;
            p = buf + 2;
            check(decode_body(p, buf + n, &d, r, wf) && p == buf + n, "delta decode");
            check(wf == (0x01 | 0x40), "unchanged slow groups elided");
            check(std::fabs(r.pos[0] - b.pos[0]) <= 1.0f / 512.0f && std::fabs(r.pos[2] - b.pos[2]) <= 1.0f / 512.0f,
                  "position within quantisation error");
            check(r.health == 100 && r.clip == 30 && r.tick == 77, "inherited fields");
        }
        // Own record (no keyframe), health change only
        Entry own = d;
        own.health = 60;
        n = encode(own, 0x80 | 0x04 | 0x20, &d, 1, 0, buf);
        check(n == 2 + 2 + 3, "own record size");
        Entry r = d;
        p = buf + 2;
        check(decode_body(p, buf + n, &d, r, wf) && wf == 0x20 && r.health == 60, "own record decode");
        if (!failures)
            xlog::info("obj_update_delta selfcheck passed");
    }
}
