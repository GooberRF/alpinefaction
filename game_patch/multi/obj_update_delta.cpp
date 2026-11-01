#include <cmath>
#include <cstring>
#include <common/utils/byte-io.h>
#include <common/utils/int-utils.h>
#include <common/utils/list-utils.h>
#include <xlog/xlog.h>
#include "obj_update_delta.h"
#include "alpine_packets.h"
#include "../rf/entity.h"
#include "../rf/object.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/weapon.h"

namespace obj_update_delta
{
    namespace
    {
        constexpr uint32_t terminator = 0xFFFFFFFF;
        // Stock obj_update record groups
        constexpr uint8_t pos_group = RF_OUF_POS_ROT_ANIM;  // 22-byte keyframe block
        constexpr uint8_t weapon_group = RF_OUF_WEAPON_TYPE;
        constexpr uint8_t health_group = RF_OUF_HEATH_ARMOR; // health, armor, hit direction
        constexpr uint8_t powerup_group = RF_OUF_AMP_FLAGS;
        constexpr uint8_t slow_groups = powerup_group | weapon_group | health_group;
        constexpr uint8_t client_only_groups = RF_OUF_UNKNOWN4 | RF_OUF_UNKNOWN3; // recv counter, lag comp
        constexpr size_t full_prefix_len = 1 + 4;  // age + handle
        constexpr size_t delta_prefix_len = 1 + 1; // age + baseline index
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

        // Stock record (server -> client layout) -> entry, inheriting absent groups from base; false if truncated
        bool entry_from_stock(const uint8_t* rec, int len, const Entry* base, Entry& e)
        {
            e = base ? *base : Entry{};
            ByteReader r{rec, static_cast<size_t>(len)};
            uint32_t handle = 0;
            r.get(handle);
            e.handle = static_cast<int>(handle);
            const uint8_t flags = r.u8();
            e.groups |= flags & (slow_groups | pos_group);
            if (flags & pos_group) {
                r.get(e.tick);
                r.bytes(e.pos, 12);
                r.get(e.pitch);
                r.get(e.yaw);
                r.get(e.state);
                r.bytes(e.vel, 3);
            }
            if (flags & powerup_group)
                r.get(e.powerup);
            if (flags & weapon_group)
                r.get(e.weapon);
            if (flags & health_group) {
                r.get(e.health);
                r.get(e.armor);
                r.get(e.hitdir);
            }
            return r.ok();
        }

        void entry_to_stock(const Entry& e, uint8_t flags, ByteWriter& w)
        {
            w.u32(static_cast<uint32_t>(e.handle));
            w.u8(flags);
            if (flags & pos_group) {
                w.put(e.tick);
                w.bytes(e.pos, 12);
                w.put(e.pitch);
                w.put(e.yaw);
                w.put(e.state);
                w.bytes(e.vel, 3);
            }
            if (flags & powerup_group)
                w.put(e.powerup);
            if (flags & weapon_group)
                w.put(e.weapon);
            if (flags & health_group) {
                w.put(e.health);
                w.put(e.armor);
                w.put(e.hitdir);
            }
        }

        bool ammo_equal(const Entry& a, const Entry& b)
        {
            return a.has_ammo == b.has_ammo && a.ammo_type == b.ammo_type && a.clip == b.clip && a.reserve == b.reserve;
        }

        // cur as a full record (base == nullptr) or a delta against base; fire bits in flags pass through
        void encode(const Entry& cur, uint8_t flags, const Entry* base, uint8_t age, uint8_t index, ByteWriter& w)
        {
            w.u8(age);
            if (base)
                w.u8(index);
            else
                w.u32(static_cast<uint32_t>(cur.handle));

            uint8_t mask = pos_float;
            long q[3] = {};
            if (base) {
                if ((flags & powerup_group) && cur.powerup == base->powerup)
                    flags &= ~powerup_group;
                if ((flags & weapon_group) && cur.weapon == base->weapon)
                    flags &= ~weapon_group;
                if ((flags & health_group) && cur.health == base->health && cur.armor == base->armor
                    && cur.hitdir == base->hitdir)
                    flags &= ~health_group;
                if ((flags & pos_group) && (base->groups & pos_group)) {
                    bool fits8 = true, fits16 = true, same = true;
                    for (int i = 0; i < 3; ++i) {
                        const float d = (cur.pos[i] - base->pos[i]) * 256.0f;
                        // lround returns 0 for NaN, inf and out-of-range values: those go as floats
                        if (!(std::fabs(d) < 32767.0f)) {
                            fits8 = fits16 = same = false;
                            continue;
                        }
                        const long qi = std::lround(d);
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

            w.u8(flags);
            w.u8(mask);
            if (flags & pos_group) {
                w.put(cur.tick);
                switch (mask & pos_mode_mask) {
                case pos_i8:
                    for (long v : q)
                        w.put(static_cast<int8_t>(v));
                    break;
                case pos_i16:
                    for (long v : q)
                        w.put(static_cast<int16_t>(v));
                    break;
                case pos_float:
                    w.bytes(cur.pos, 12);
                    break;
                default:
                    break;
                }
                w.put(cur.pitch);
                w.put(cur.yaw);
                if (mask & has_state)
                    w.put(cur.state);
                if (mask & has_vel)
                    w.bytes(cur.vel, 3);
            }
            if (flags & powerup_group)
                w.put(cur.powerup);
            if (flags & weapon_group)
                w.put(cur.weapon);
            if (flags & health_group) {
                w.put(cur.health);
                w.put(cur.armor);
                w.put(cur.hitdir);
            }
            if (mask & has_ammo) {
                w.put(cur.ammo_type);
                w.put(cur.clip);
                w.put(cur.reserve);
            }
        }

        // Reads the record body after the age/index/handle prefix. `out` starts as a copy of base.
        bool decode_body(ByteReader& r, const Entry* base, Entry& out, uint8_t& wire_flags)
        {
            uint8_t flags, mask;
            if (!r.get(flags) || !r.get(mask))
                return false;
            if (mask & ~(pos_mode_mask | has_state | has_vel | has_ammo))
                return false; // unknown encoding
            // The expanded record goes to the stock parser, which sizes these groups' payloads itself
            if (flags & client_only_groups)
                return false;
            if (flags & pos_group) {
                if (!r.get(out.tick))
                    return false;
                switch (mask & pos_mode_mask) {
                case pos_same:
                    if (!base || !(base->groups & pos_group))
                        return false;
                    break;
                case pos_i8:
                    for (float& v : out.pos) {
                        int8_t d;
                        if (!r.get(d))
                            return false;
                        v += static_cast<float>(d) / 256.0f;
                    }
                    break;
                case pos_i16:
                    for (float& v : out.pos) {
                        int16_t d;
                        if (!r.get(d))
                            return false;
                        v += static_cast<float>(d) / 256.0f;
                    }
                    break;
                default:
                    if (!r.bytes(out.pos, 12))
                        return false;
                    break;
                }
                if (!r.get(out.pitch) || !r.get(out.yaw))
                    return false;
                if ((mask & has_state) && !r.get(out.state))
                    return false;
                if ((mask & has_vel) && !r.bytes(out.vel, 3))
                    return false;
                out.groups |= pos_group;
            }
            if ((flags & powerup_group) && !r.get(out.powerup))
                return false;
            if ((flags & weapon_group) && !r.get(out.weapon))
                return false;
            if (flags & health_group) {
                if (!r.get(out.health) || !r.get(out.armor) || !r.get(out.hitdir))
                    return false;
            }
            out.groups |= flags & slow_groups;
            if (mask & has_ammo) {
                if (!r.get(out.ammo_type) || !r.get(out.clip) || !r.get(out.reserve))
                    return false;
                out.has_ammo = true;
            }
            wire_flags = flags;
            return true;
        }

        void gather_ammo(rf::Entity* ep, Entry& e)
        {
            const auto ammo = af_gather_remote_ammo(ep);
            e.has_ammo = ammo.has_value();
            if (ammo) {
                e.ammo_type = ammo->ammo_type;
                e.clip = ammo->clip;
                e.reserve = ammo->reserve;
            }
        }

        // Ages stop one short of max_age: a mid-loop flush moves the record into the next packet (age + 1)
        const Entry* find_baseline(Sender& s, int handle, uint8_t& age, uint8_t& index)
        {
            for (int a = 1; a < max_age; ++a) {
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
    }

    Snapshot* Ring::find(uint16_t seq)
    {
        Snapshot& s = slots[seq % ring_size];
        return s.valid && s.seq == seq ? &s : nullptr;
    }

    void Ring::store(uint16_t seq, std::vector<Entry>& entries)
    {
        Snapshot& s = slots[seq % ring_size];
        s.seq = seq;
        s.valid = true;
        s.acked = false;
        // The caller gets the slot's old storage back, emptied, so neither side reallocates every packet
        s.entries.swap(entries);
        entries.clear();
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
        if (s.last) {
            s.pending.push_back(*s.last); // the loop copied it into the packet after we returned
            s.last.reset();
            s.last_rec = nullptr;
        }
        uint32_t handle;
        std::memcpy(&handle, rec, sizeof(handle));
        uint8_t age = 0, index = 0;
        const Entry* base = find_baseline(s, static_cast<int>(handle), age, index);
        Entry cur;
        if (!entry_from_stock(rec, len, base, cur))
            return 0;
        // Own records (no keyframe) carry no ammo; neither do vehicle hulls, whose seats predict theirs
        if ((flags & pos_group) && rf::player_from_entity_handle(ep->handle))
            gather_ammo(ep, cur);
        uint8_t buf[64];
        ByteWriter w{buf, sizeof(buf)};
        encode(cur, flags, base, age, index, w);
        const size_t n = w.size();
        // Store what the client will reconstruct (quantised), not what we sampled
        Entry visible = base ? *base : Entry{};
        visible.handle = cur.handle;
        const size_t prefix = base ? delta_prefix_len : full_prefix_len;
        ByteReader r{buf + prefix, n - prefix};
        uint8_t wire_flags;
        if (!w.ok() || !decode_body(r, base, visible, wire_flags) || r.remaining() != 0) {
            // Encoder/decoder disagree: never ship a record the client cannot mirror
            ERR_ONCE("obj_update_delta: encode/decode mismatch for handle {:x}", cur.handle);
            return 0;
        }
        s.last = visible;
        s.last_rec = rec;
        std::memcpy(rec, buf, n);
        return static_cast<int>(n);
    }

    void server_send(rf::Player* pp, const uint8_t* packet, int len, bool final)
    {
        Sender& s = pp->delta_sender;
        if (final && s.last) {
            s.pending.push_back(*s.last);
            s.last.reset();
        }
        else if (!final && s.last_rec && s.last_rec[0] != 0) {
            // The loop flushes before copying the last packed record, so it lands in the next packet: its
            // age was computed against this one
            ++s.last_rec[0];
        }
        s.last_rec = nullptr;
        // stock: [0x26][u16 size][records][terminator]; ours adds seq after the size
        uint8_t out[rf::max_packet_size];
        ByteWriter w{out, sizeof(out)};
        w.u8(static_cast<uint8_t>(af_packet_type::af_obj_update_delta));
        w.u16(static_cast<uint16_t>(len - 3 + 2));
        w.u16(s.next_seq);
        w.bytes(packet + 3, len - 3);
        if (w.ok()) {
            rf::multi_io_send(pp, out, static_cast<int>(w.size()));
        }
        s.ring.store(s.next_seq, s.pending);
        ++s.next_seq;
    }

    void server_on_ack(rf::Player* pp, uint16_t newest_seq, uint32_t bits)
    {
        Sender& s = pp->delta_sender;
        if (wrapped_diff16(s.next_seq, newest_seq) <= 0)
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
        ByteReader r{payload, len};
        uint16_t seq;
        if (!r.get(seq))
            return;
        // Too late to be a baseline, and its ring slot may already hold a live snapshot
        if (g_any_received && wrapped_diff16(seq, g_newest_seq) <= -max_age)
            return;

        // Expanded stock packet fed to the engine parser; ~40 B per record vs >= 4 on the wire
        static uint8_t expanded[rf::max_packet_size * 12];
        ByteWriter w{expanded, sizeof(expanded)};
        w.u8(RF_GPT_OBJECT_UPDATE);
        w.u16(0); // size, filled in below
        static std::vector<Entry> entries; // reuses the storage Ring::store hands back
        entries.clear();
        bool complete = true;
        while (true) {
            uint32_t head;
            if (!r.peek(head)) {
                complete = false;
                break;
            }
            if (head == terminator)
                break;
            const uint8_t age = r.u8();
            const Entry* base = nullptr;
            Entry e;
            if (age == 0) {
                uint32_t handle;
                if (!r.get(handle)) {
                    complete = false;
                    break;
                }
                e.handle = static_cast<int>(handle);
            }
            else {
                uint8_t index;
                if (!r.get(index) || age > max_age) {
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
            if (!decode_body(r, base, e, wire_flags)) {
                complete = false;
                break;
            }
            if (sizeof(expanded) - w.size() < stock_max_len + sizeof(terminator)) {
                complete = false;
                break;
            }
            // Stock servers send the slow groups in every record; re-feed the inherited ones, since an
            // elided group only matches the acked baseline, not what later unacked packets applied
            entry_to_stock(e, wire_flags | (e.groups & slow_groups), w);
            entries.push_back(e);
        }
        w.u32(terminator);
        const uint16_t size = static_cast<uint16_t>(w.size() - 3);
        std::memcpy(expanded + 1, &size, 2);

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
        g_rx.store(seq, entries);
        if (!g_any_received || wrapped_diff16(seq, g_newest_seq) > 0)
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
                p.delta_sender.last_rec = nullptr;
            }
        }
    }
}
