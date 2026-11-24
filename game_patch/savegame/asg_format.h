#pragma once

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <common/utils/byte-io.h>
#include "asg_model.h"

namespace asg
{
    constexpr uint16_t format_version = 1;
    constexpr uint16_t file_header_size = 20;
    constexpr size_t chunk_header_size = 10;
    constexpr uint8_t chunk_compressed = 0x01;
    constexpr uint8_t chunk_required = 0x02;

    // FourCC whose bytes in the file are the 4 characters in order
    constexpr uint32_t fourcc(const char (&id)[5])
    {
        return static_cast<uint32_t>(static_cast<uint8_t>(id[0]))
            | static_cast<uint32_t>(static_cast<uint8_t>(id[1])) << 8
            | static_cast<uint32_t>(static_cast<uint8_t>(id[2])) << 16
            | static_cast<uint32_t>(static_cast<uint8_t>(id[3])) << 24;
    }

    constexpr uint32_t id_hdr = fourcc("HDR ");
    constexpr uint32_t id_game = fourcc("GAME");
    constexpr uint32_t id_plyr = fourcc("PLYR");
    constexpr uint32_t id_levl = fourcc("LEVL");
    constexpr uint32_t id_lhdr = fourcc("LHDR");
    constexpr uint32_t id_room = fourcc("ROOM");
    constexpr uint32_t id_geo1 = fourcc("GEO1");
    constexpr uint32_t id_geo2 = fourcc("GEO2");
    constexpr uint32_t id_dbrk = fourcc("DBRK");
    constexpr uint32_t id_ents = fourcc("ENTS");
    constexpr uint32_t id_item = fourcc("ITEM");
    constexpr uint32_t id_clut = fourcc("CLUT");
    constexpr uint32_t id_trig = fourcc("TRIG");
    constexpr uint32_t id_movr = fourcc("MOVR");
    constexpr uint32_t id_weap = fourcc("WEAP");
    constexpr uint32_t id_corp = fourcc("CORP");
    constexpr uint32_t id_evnt = fourcc("EVNT");
    constexpr uint32_t id_decl = fourcc("DECL");
    constexpr uint32_t id_blod = fourcc("BLOD");
    constexpr uint32_t id_emit = fourcc("EMIT");
    constexpr uint32_t id_push = fourcc("PUSH");
    constexpr uint32_t id_lite = fourcc("LITE");
    constexpr uint32_t id_goal = fourcc("GOAL");
    constexpr uint32_t id_wrld = fourcc("WRLD");
    constexpr uint32_t id_alpo = fourcc("ALPO");

    // Writes 0 for a non-finite float, which the reader rejects, and logs where it was
    void warn_non_finite(const char* chunk, const char* field, int32_t uid);

    // Writer and Reader share method names so one field function serves both directions
    class Writer
    {
    public:
        static constexpr bool reading = false;

        explicit Writer(const char* chunk = "payload") : m_chunk(chunk) {}

        void u8(uint8_t v, const char* = nullptr)
        {
            m_w.u8(v);
        }

        void u16(uint16_t v, const char* = nullptr)
        {
            m_w.u16(v);
        }

        void i16(int16_t v, const char* = nullptr)
        {
            m_w.put(v);
        }

        void u32(uint32_t v, const char* = nullptr)
        {
            m_w.u32(v);
        }

        void i32(int32_t v, const char* n = nullptr)
        {
            if (n && std::string_view{n} == "uid") {
                m_uid = v;
            }
            m_w.i32(v);
        }

        void u64(uint64_t v, const char* = nullptr)
        {
            m_w.put(v);
        }

        void f32(float v, const char* n = nullptr)
        {
            if (!std::isfinite(v)) {
                warn_non_finite(m_chunk, n, m_uid);
                v = 0.0f;
            }
            m_w.f32(v);
        }

        void b(bool v, const char* = nullptr)
        {
            m_w.u8(v ? 1 : 0);
        }

        void v3(const rf::Vector3& v, const char* n = nullptr)
        {
            f32(v.x, n);
            f32(v.y, n);
            f32(v.z, n);
        }

        void m3(const rf::Matrix3& m, const char* n = nullptr)
        {
            v3(m.rvec, n);
            v3(m.uvec, n);
            v3(m.fvec, n);
        }

        // Longer strings are truncated to the u16 length limit
        void str(std::string_view s, const char* = nullptr)
        {
            const size_t n = s.size() < 0xFFFF ? s.size() : 0xFFFF;
            m_w.u16(static_cast<uint16_t>(n));
            m_w.bytes(s.data(), n);
        }

        void blob(const std::vector<uint8_t>& v, const char* = nullptr)
        {
            m_w.u32(static_cast<uint32_t>(v.size()));
            m_w.bytes(v.data(), v.size());
        }

        void i32_list(const std::vector<int32_t>& v, const char* = nullptr)
        {
            m_w.u32(static_cast<uint32_t>(v.size()));
            for (int32_t x : v) {
                m_w.i32(x);
            }
        }

        void str_list(const std::vector<std::string>& v, const char* = nullptr)
        {
            m_w.u32(static_cast<uint32_t>(v.size()));
            for (const auto& s : v) {
                str(s);
            }
        }

        // u32 count, then each item as a u32-sized record
        template<typename V, typename F>
        void list(const char*, const V& v, F&& f)
        {
            list(v, f);
        }

        template<typename V, typename F>
        void list(const V& v, F&& f)
        {
            m_w.u32(static_cast<uint32_t>(v.size()));
            for (const auto& item : v) {
                record([&](Writer& w) { f(w, item); });
            }
        }

        template<typename F>
        void record(F&& f)
        {
            const size_t at = m_w.size();
            m_w.u32(0);
            f(*this);
            m_w.patch(at, static_cast<uint32_t>(m_w.size() - at - 4));
        }

        // u16-sized object base block
        template<typename F>
        void base(F&& f)
        {
            const size_t at = m_w.size();
            m_w.u16(0);
            f(*this);
            m_w.patch(at, static_cast<uint16_t>(m_w.size() - at - 2));
        }

        void raw(std::span<const uint8_t> bytes)
        {
            m_w.bytes(bytes.data(), bytes.size());
        }

        [[nodiscard]] const std::vector<uint8_t>& buffer() const
        {
            return m_w.buffer();
        }

        [[nodiscard]] std::vector<uint8_t> take()
        {
            return m_w.take();
        }

    private:
        GrowableByteWriter m_w;
        const char* m_chunk;
        int32_t m_uid = uid_none;
    };

    // Strict reader. Inside a sized block (record, base, chunk payload) a block that ends exactly on a field
    // boundary leaves the remaining fields at their defaults; anything else malformed fails the reader.
    // The first failure message is kept in `error`. A budget, shared with its sub-readers, caps the memory the
    // decoded data may take, as a small file can decompress to many records.
    class Reader
    {
    public:
        static constexpr bool reading = true;

        Reader(std::span<const uint8_t> data, std::string* error, bool default_at_end = true,
               size_t* budget = nullptr) :
            m_r(data.data(), data.size()), m_error(error), m_default_at_end(default_at_end), m_budget(budget)
        {
        }

        void u8(uint8_t& v, const char* = nullptr)
        {
            take(v);
        }

        void u16(uint16_t& v, const char* = nullptr)
        {
            take(v);
        }

        void i16(int16_t& v, const char* = nullptr)
        {
            take(v);
        }

        void u32(uint32_t& v, const char* = nullptr)
        {
            take(v);
        }

        void i32(int32_t& v, const char* = nullptr)
        {
            take(v);
        }

        void u64(uint64_t& v, const char* = nullptr)
        {
            take(v);
        }

        void f32(float& v, const char* = nullptr)
        {
            float tmp = 0.0f;
            if (take(tmp)) {
                if (!std::isfinite(tmp)) {
                    fail("non-finite float");
                }
                else {
                    v = tmp;
                }
            }
        }

        void b(bool& v, const char* = nullptr)
        {
            uint8_t tmp = 0;
            if (take(tmp)) {
                if (tmp > 1) {
                    fail("invalid bool");
                }
                else {
                    v = tmp != 0;
                }
            }
        }

        void v3(rf::Vector3& v, const char* = nullptr)
        {
            f32(v.x);
            f32(v.y);
            f32(v.z);
        }

        void m3(rf::Matrix3& m, const char* = nullptr)
        {
            v3(m.rvec);
            v3(m.uvec);
            v3(m.fvec);
        }

        void str(std::string& v, const char* = nullptr)
        {
            uint16_t n = 0;
            if (take(n) && charge(n)) {
                read_string(v, n);
            }
        }

        void blob(std::vector<uint8_t>& v, const char* = nullptr)
        {
            uint32_t n = 0;
            if (!take(n)) {
                return;
            }
            if (n > m_r.remaining()) {
                fail("blob overruns its block");
                return;
            }
            if (!charge(n)) {
                return;
            }
            v.assign(m_r.cur(), m_r.cur() + n);
            m_r.skip(n);
        }

        void i32_list(std::vector<int32_t>& v, const char* = nullptr)
        {
            uint32_t n = 0;
            if (!take(n) || !check_count(n, 4) || !charge(uint64_t{n} * sizeof(int32_t))) {
                return;
            }
            v.assign(n, 0);
            for (auto& x : v) {
                take(x, true);
            }
        }

        void str_list(std::vector<std::string>& v, const char* = nullptr)
        {
            uint32_t n = 0;
            if (!take(n) || !check_count(n, 2) || !charge(uint64_t{n} * sizeof(std::string))) {
                return;
            }
            v.assign(n, {});
            for (auto& s : v) {
                if (!m_ok || !require(2)) {
                    return;
                }
                str(s);
            }
        }

        template<typename V, typename F>
        void list(const char*, V& v, F&& f)
        {
            list(v, f);
        }

        template<typename V, typename F>
        void list(V& v, F&& f)
        {
            uint32_t n = 0;
            if (!take(n) || !check_count(n, 4)) {
                return;
            }
            // Grown per record rather than sized up front, so a bogus count cannot allocate ahead of the data
            v.clear();
            for (uint32_t i = 0; i < n && charge(sizeof(typename V::value_type)); ++i) {
                auto& item = v.emplace_back();
                record([&](Reader& r) { f(r, item); });
            }
        }

        template<typename F>
        void record(F&& f)
        {
            uint32_t size = 0;
            if (!take(size, true)) {
                return;
            }
            // Every record has fields
            if (size == 0) {
                fail("empty record");
                return;
            }
            sub_block(size, f);
        }

        template<typename F>
        void base(F&& f)
        {
            uint16_t size = 0;
            if (take(size)) {
                sub_block(size, f);
            }
        }

        void fail(std::string_view msg)
        {
            if (m_ok && m_error && m_error->empty()) {
                m_error->assign(msg);
            }
            m_ok = false;
        }

        [[nodiscard]] bool ok() const
        {
            return m_ok;
        }

        [[nodiscard]] size_t remaining() const
        {
            return m_r.remaining();
        }

        // Takes n bytes of decoded data from the budget
        bool charge(uint64_t n)
        {
            if (!m_ok) {
                return false;
            }
            if (m_budget) {
                if (n > *m_budget) {
                    fail("savegame is too large to load");
                    return false;
                }
                *m_budget -= static_cast<size_t>(n);
            }
            return true;
        }

    private:
        // Returns false if the value was not read (left at its default, or the reader failed)
        template<typename T>
        bool take(T& v, bool strict = false)
        {
            if (!m_ok) {
                return false;
            }
            if (!strict && m_default_at_end && m_r.remaining() == 0) {
                return false;
            }
            if (!m_r.get(v)) {
                fail("truncated data");
                return false;
            }
            return true;
        }

        bool require(size_t n)
        {
            if (n > m_r.remaining()) {
                fail("truncated data");
                return false;
            }
            return true;
        }

        bool check_count(uint32_t n, size_t min_item_size)
        {
            if (static_cast<uint64_t>(n) * min_item_size > m_r.remaining()) {
                fail("list count exceeds its block");
                return false;
            }
            return true;
        }

        void read_string(std::string& v, size_t n)
        {
            if (!require(n)) {
                return;
            }
            v.assign(reinterpret_cast<const char*>(m_r.cur()), n);
            m_r.skip(n);
        }

        template<typename F>
        void sub_block(size_t size, F& f)
        {
            if (size > m_r.remaining()) {
                fail("record overruns its block");
                return;
            }
            Reader sub{{m_r.cur(), size}, m_error, true, m_budget};
            f(sub);
            if (!sub.ok()) {
                m_ok = false;
                return;
            }
            m_r.skip(size);
        }

        ByteReader m_r;
        std::string* m_error;
        bool m_default_at_end;
        size_t* m_budget;
        bool m_ok = true;
    };

    // Appends a chunk header and payload
    void append_chunk(GrowableByteWriter& out, uint32_t id, uint8_t flags, std::span<const uint8_t> payload,
                      uint8_t version = 1);

    // Writes file_size and the hash into a buffer that starts with a file header
    void seal_file(std::vector<uint8_t>& file);
}

// Empty if a level cannot be compressed
std::optional<std::vector<uint8_t>> asg_encode(const asg::SaveModel& m);
bool asg_decode(std::span<const uint8_t> file, asg::SaveModel& out, std::string& error);
bool asg_read_header(const std::filesystem::path& path, asg::Header& out);
