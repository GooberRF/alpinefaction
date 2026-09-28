#pragma once

// Bounds checked reader for the Alpine RFL chunks, shared by the game and RED. File is each DLL's
// rf::File: int read(void*, size), error(). Bind `remaining` to the same counter as the
// rf::File::ChunkGuard that guards the chunk, so the guard still skips whatever went unread.

#include <cstddef>
#include <cstdint>
#include <string>

template<typename File>
struct RflChunkReader
{
    File& file;
    std::size_t& remaining;
    // Set by every failed read, never cleared.
    bool read_error = false;

    bool read_bytes(void* dst, std::size_t n)
    {
        if (remaining < n) {
            read_error = true;
            return false;
        }
        int got = file.read(dst, n);
        if (got != static_cast<int>(n) || file.error()) {
            if (got > 0) remaining -= got;
            read_error = true;
            return false;
        }
        remaining -= n;
        return true;
    }

    // u16 length prefixed string. `out` is empty on every failure path, and nothing is allocated for a
    // length that does not fit the chunk.
    bool read_string(std::string& out)
    {
        out.clear();
        std::uint16_t len = 0;
        if (!read_bytes(&len, sizeof(len))) {
            return false;
        }
        if (len == 0) {
            return true;
        }
        if (remaining < len) {
            read_error = true;
            return false;
        }
        out.assign(len, '\0');
        if (!read_bytes(out.data(), len)) {
            out.clear();
            return false;
        }
        return true;
    }

    template<typename T>
    bool read(T& v)
    {
        return read_bytes(&v, sizeof(T));
    }

    bool failed() const
    {
        return read_error;
    }
};
