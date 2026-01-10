#pragma once

#include <optional>
#include <vector>

template <typename E>
requires std::is_enum_v<E>
    && (sizeof(std::underlying_type_t<E>) == 1)
class TlvWriter {
public:
    explicit TlvWriter(std::vector<uint8_t>& buf)
        : _buf{buf} {
    }

    template <std::integral T>
    void write_le(const E type, const T value) {
        _buf.push_back(std::to_underlying(type));
        _buf.push_back(sizeof(T));

        using U = std::make_unsigned_t<T>;
        const U unsigned_value = static_cast<U>(value);

        for (size_t i = 0; i < sizeof(T); ++i) {
            const auto octet = (unsigned_value >> 8 * i) & 0xFF;
            _buf.push_back(static_cast<uint8_t>(octet));
        }
    }

private:
    std::vector<uint8_t>& _buf;
};

template <typename E>
requires std::is_enum_v<E>
    && (sizeof(std::underlying_type_t<E>) == 1)
class TlvReader {
public:
    TlvReader(const uint8_t* const begin, const uint8_t* const end)
        : _begin{begin}
        , _end{end}
        , _len{static_cast<size_t>(_end - _begin)}
        , _offset{0} {
    }

    explicit TlvReader(const std::vector<uint8_t>& buf)
        : TlvReader{buf.data(), buf.data() + buf.size()} {
    }

    struct Value {
        E type;
        const uint8_t* value;
        size_t len;

        template <std::integral T>
        std::optional<T> read_le() const {
            if (len != sizeof(T)) {
                return std::nullopt;
            }

            using U = std::make_unsigned_t<T>;
            U res = 0;
            for (size_t i = 0; i < len && i < sizeof(T); ++i) {
                res |= static_cast<U>(value[i]) << 8 * i;
            }
            return static_cast<T>(res);
        }
    };

    std::optional<Value> next() {
        if (_offset + 2 > _len) {
            return std::nullopt;
        }

        const E type = static_cast<E>(_begin[_offset++]);
        const uint8_t len = _begin[_offset++];

        if (_offset + len > _len) {
            return std::nullopt;
        }

        const uint8_t* ptr = _begin + _offset;
        _offset += len;

        return Value{type, ptr, len};
    }

private:
    const uint8_t* _begin;
    const uint8_t* _end;
    size_t _len;
    size_t _offset;
};
