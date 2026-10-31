#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <bit>

// Values are copied in native byte order, which is the little-endian wire order on every supported target
static_assert(std::endian::native == std::endian::little);

// Bounds-checked little-endian writer into a fixed buffer. A write that does not fit fails the writer
// and every later write is dropped, so callers check ok() once at the end.
class ByteWriter
{
public:
    ByteWriter(void* buf, size_t cap, size_t off = 0) :
        m_buf(static_cast<std::byte*>(buf)), m_cap(cap), m_off(off <= cap ? off : cap), m_ok(off <= cap)
    {
    }

    template<typename T>
        requires std::is_trivially_copyable_v<T>
    void put(const T& v)
    {
        bytes(&v, sizeof(v));
    }

    void u8(uint8_t v)
    {
        put(v);
    }

    void u16(uint16_t v)
    {
        put(v);
    }

    void u32(uint32_t v)
    {
        put(v);
    }

    void i32(int32_t v)
    {
        put(v);
    }

    void f32(float v)
    {
        put(v);
    }

    // u8 length prefix; longer strings are truncated to 255 bytes
    void str(std::string_view s)
    {
        const size_t n = s.size() < 255 ? s.size() : 255;
        u8(static_cast<uint8_t>(n));
        bytes(s.data(), n);
    }

    void bytes(const void* src, size_t n)
    {
        if (!m_ok || n > m_cap - m_off) {
            m_ok = false;
            return;
        }
        if (n) {
            std::memcpy(m_buf + m_off, src, n); // an empty source's data() may be null
        }
        m_off += n;
    }

    void fail()
    {
        m_ok = false;
    }

    [[nodiscard]] bool ok() const
    {
        return m_ok;
    }

    [[nodiscard]] size_t size() const
    {
        return m_off;
    }

private:
    std::byte* m_buf;
    size_t m_cap;
    size_t m_off;
    bool m_ok = true;
};

// Bounds-checked little-endian reader. A read past the end fails the reader: it returns zero / false
// and every later read fails too, so callers can check ok() once after a group of reads.
class ByteReader
{
public:
    ByteReader(const void* data, size_t len) : m_data(static_cast<const uint8_t*>(data)), m_len(len) {}

    template<typename T>
        requires std::is_trivially_copyable_v<T>
    bool get(T& v)
    {
        return bytes(&v, sizeof(v));
    }

    // Reads without consuming; does not fail the reader
    template<typename T>
        requires std::is_trivially_copyable_v<T>
    bool peek(T& v) const
    {
        if (!m_ok || sizeof(v) > m_len - m_pos) {
            return false;
        }
        std::memcpy(&v, m_data + m_pos, sizeof(v));
        return true;
    }

    bool bytes(void* dst, size_t n)
    {
        if (!m_ok || n > m_len - m_pos) {
            m_ok = false;
            return false;
        }
        if (n) {
            std::memcpy(dst, m_data + m_pos, n); // a zero-length destination may be null
        }
        m_pos += n;
        return true;
    }

    uint8_t u8()
    {
        return read<uint8_t>();
    }

    uint16_t u16()
    {
        return read<uint16_t>();
    }

    uint32_t u32()
    {
        return read<uint32_t>();
    }

    int32_t i32()
    {
        return read<int32_t>();
    }

    float f32()
    {
        return read<float>();
    }

    // u8 length prefix
    std::string str()
    {
        const uint8_t n = u8();
        if (!m_ok || n > m_len - m_pos) {
            m_ok = false;
            return {};
        }
        std::string v(reinterpret_cast<const char*>(m_data + m_pos), n);
        m_pos += n;
        return v;
    }

    void skip(size_t n)
    {
        if (!m_ok || n > m_len - m_pos) {
            m_ok = false;
            return;
        }
        m_pos += n;
    }

    [[nodiscard]] bool ok() const
    {
        return m_ok;
    }

    [[nodiscard]] size_t remaining() const
    {
        return m_ok ? m_len - m_pos : 0;
    }

    [[nodiscard]] const uint8_t* cur() const
    {
        return m_data + m_pos;
    }

private:
    template<typename T>
    T read()
    {
        T v{};
        get(v);
        return v;
    }

    const uint8_t* m_data;
    size_t m_len;
    size_t m_pos = 0;
    bool m_ok = true;
};
