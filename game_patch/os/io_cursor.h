#pragma once

#include <cstring>
#include <algorithm>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace io_cursor_detail {

template <typename T>
concept PodLike = std::is_trivially_copyable_v<T>
    && std::is_standard_layout_v<T>;

template <typename Storage>
concept Read = requires(
    const Storage& storage,
    size_t position,
    std::span<char> out
) {
    { storage.ptr() } noexcept -> std::same_as<const char*>;
    { storage.len() } noexcept -> std::same_as<size_t>;
    { storage.read_into(position, out) } noexcept -> std::same_as<size_t>;
};

template <typename Storage>
concept Write = requires(
    Storage& storage,
    size_t position,
    std::span<const char> in
) {
    { storage.write_into(position, in) } -> std::same_as<size_t>;
};

class ReadOnlySpanStorage {
public:
    ReadOnlySpanStorage() = default;

    explicit ReadOnlySpanStorage(const std::span<const char> buf) noexcept
        : view{buf} {}

    explicit ReadOnlySpanStorage(const std::vector<char>& buf) noexcept
        : view{buf} {}

    template <size_t N>
    explicit ReadOnlySpanStorage(const char (&buf)[N]) noexcept
        : view{buf, N} {}

    explicit ReadOnlySpanStorage(const void* const buf, const size_t len) noexcept
        : view{static_cast<const char*>(buf), len} {}

    [[nodiscard]]
    const char* ptr(this const ReadOnlySpanStorage& self) noexcept {
        return self.view.data();
    }

    [[nodiscard]]
    size_t len(this const ReadOnlySpanStorage& self) noexcept {
        return self.view.size();
    }

    size_t read_into(
        this const ReadOnlySpanStorage& self,
        const size_t position,
        const std::span<char> out
    ) noexcept {
        if (position > self.view.size()) {
            return 0;
        }
        const size_t num_bytes = std::min(out.size(), self.view.size() - position);
        if (num_bytes > 0) {
            std::memcpy(out.data(), self.view.data() + position, num_bytes);
        }
        return num_bytes;
    }

private:
    std::span<const char> view;
};

class FixedSpanStorage {
public:
    FixedSpanStorage() = default;

    explicit FixedSpanStorage(const std::span<char> buf) noexcept
        : view{buf} {}

    template <size_t N>
    explicit FixedSpanStorage(char (&buf)[N]) noexcept
        : view{buf, N} {}

    explicit FixedSpanStorage(void* const buf, const size_t len) noexcept
        : view{static_cast<char*>(buf), len} {}

    [[nodiscard]]
    const char* ptr(this const FixedSpanStorage& self) noexcept {
        return self.view.data();
    }

    [[nodiscard]]
    size_t len(this const FixedSpanStorage& self) noexcept {
        return self.view.size();
    }

    size_t read_into(
        this const FixedSpanStorage& self,
        const size_t position,
        const std::span<char> out
    ) noexcept {
        if (position > self.view.size()) {
            return 0;
        }
        const size_t num_bytes = std::min(out.size(), self.view.size() - position);
        if (num_bytes > 0) {
            std::memcpy(out.data(), self.view.data() + position, num_bytes);
        }
        return num_bytes;
    }

    size_t write_into(
        this FixedSpanStorage& self,
        const size_t position,
        const std::span<const char> in
    ) noexcept {
        if (position > self.view.size()) {
            return 0;
        }
        const size_t num_bytes = std::min(in.size(), self.view.size() - position);
        if (num_bytes > 0) {
            std::memcpy(self.view.data() + position, in.data(), num_bytes);
        }
        return num_bytes;
    }

private:
    std::span<char> view;
};

class VectorGrowStorage {
public:
    VectorGrowStorage() = default;

    explicit VectorGrowStorage(std::vector<char>& buf) noexcept
        : buf{buf} {}

    [[nodiscard]]
    const char* ptr(this const VectorGrowStorage& self) noexcept {
        return self.buf.data();
    }

    [[nodiscard]]
    size_t len(this const VectorGrowStorage& self) noexcept {
        return self.buf.size();
    }

    size_t read_into(
        this const VectorGrowStorage& self,
        const size_t position,
        const std::span<char> out
    ) noexcept {
        if (position > self.buf.size()) {
            return 0;
        }
        const size_t num_bytes = std::min(out.size(), self.buf.size() - position);
        if (num_bytes > 0) {
            std::memcpy(out.data(), self.buf.data() + position, num_bytes);
        }
        return num_bytes;
    }

    size_t write_into(
        this VectorGrowStorage& self,
        const size_t position,
        const std::span<const char> in
    ) {
        if (in.empty()) {
            return 0;
        }
        const size_t end = position + in.size();
        if (self.buf.size() < end) {
            self.buf.resize(end);
        }
        std::memcpy(self.buf.data() + position, in.data(), in.size());
        return in.size();
    }

private:
    std::vector<char>& buf;
};

}

template <io_cursor_detail::Read Storage>
class IoCursor {
public:
    explicit IoCursor(Storage storage)
        noexcept(std::is_nothrow_move_constructible_v<Storage>)
        : storage{std::move(storage)}
        , pos{0}
        , poisoned{false} {}

    template <typename... Args>
    requires std::constructible_from<Storage, Args...>
    explicit IoCursor(Args&&... args)
        noexcept(std::is_nothrow_constructible_v<Storage, Args...>)
        : storage{std::forward<Args>(args)...}
        , pos{0}
        , poisoned{false} {}

    template <io_cursor_detail::PodLike T>
    [[nodiscard]]
    bool available(this const IoCursor& self) noexcept {
        return !self.poisoned
            && self.pos <= self.storage.len()
            && sizeof(T) <= self.storage.len() - self.pos;
    }

    template <io_cursor_detail::PodLike T>
    bool peek(this IoCursor& self, T& out) noexcept {
        if (!self.available<T>()) {
            self.poison();
            return false;
        }
        const std::span bytes = std::span{
            reinterpret_cast<char*>(std::addressof(out)),
            sizeof(T)
        };
        if (self.storage.read_into(self.pos, bytes) != sizeof(T)) {
            self.poison();
            return false;
        }
        return true;
    }

    template <io_cursor_detail::PodLike T>
    bool skip(this IoCursor& self) noexcept {
        if (!self.available<T>()) {
            self.poison();
            return false;
        }
        self.pos += sizeof(T);
        return true;
    }

    template <io_cursor_detail::PodLike T>
    bool read(this IoCursor& self, T& out) noexcept {
        if (!self.peek(out)) {
            return false;
        }
        self.pos += sizeof(T);
        return true;
    }

    bool read_zstring(this IoCursor& self, std::string_view& out) noexcept {
        if (self.poisoned) {
            return false;
        } else if (self.pos >= self.storage.len()) {
            self.poison();
            return false;
        }
        const char* const storage = self.storage.ptr();
        const char* const begin = storage + self.pos;
        const char* const end = storage + self.storage.len();
        const char* const nul = std::find(begin, end, '\0');
        if (nul == end) {
            self.poison();
            return false;
        }
        const size_t len = std::distance(begin, nul);
        out = std::string_view{begin, len};
        self.pos += len + 1;
        return true;
    }

    bool read_zstring(this IoCursor& self, std::string& out) {
        std::string_view view{};
        if (!self.read_zstring(view)) {
            return false;
        }
        out.assign(view);
        return true;
    }

    template <io_cursor_detail::PodLike T>
    requires io_cursor_detail::Write<Storage>
    bool write(this IoCursor& self, const T& value) {
        if (self.poisoned) {
            return false;
        }
        const std::span bytes = std::span{
            reinterpret_cast<const char*>(std::addressof(value)),
            sizeof(T)
        };
        const size_t num_bytes = self.storage.write_into(self.pos, bytes);
        if (num_bytes != sizeof(T)) {
            self.poison();
            return false;
        }
        self.pos += num_bytes;
        return true;
    }

    template <typename S = Storage>
    requires io_cursor_detail::Write<S>
    bool write_zstring(this IoCursor& self, const std::string_view text) {
        if (self.poisoned) {
            return false;
        }
        const size_t num_bytes =
            self.storage.write_into(self.pos, std::span{text});
        if (num_bytes != text.size()) {
            self.poison();
            return false;
        }
        self.pos += num_bytes;
        constexpr char NUL = '\0';
        if (self.storage.write_into(self.pos, std::span{&NUL, 1}) != 1) {
            self.poison();
            return false;
        }
        self.pos += 1;
        return true;
    }

    [[nodiscard]]
    bool is_poisoned(this const IoCursor& self) noexcept {
        return self.poisoned;
    }

    [[nodiscard]]
    size_t position(this const IoCursor& self) noexcept {
        return self.pos;
    }

    [[nodiscard]]
    size_t len(this const IoCursor& self) noexcept {
        return self.storage.len();
    }

private:
    void poison(this IoCursor& self) noexcept {
        self.poisoned = true;
    }

    size_t pos;
    Storage storage;
    bool poisoned;
};

template <size_t N>
IoCursor(const char (&)[N]) -> IoCursor<io_cursor_detail::ReadOnlySpanStorage>;
IoCursor(std::span<const char>) -> IoCursor<io_cursor_detail::ReadOnlySpanStorage>;
IoCursor(const std::vector<char>&) -> IoCursor<io_cursor_detail::ReadOnlySpanStorage>;
IoCursor(const void*, size_t) -> IoCursor<io_cursor_detail::ReadOnlySpanStorage>;

template <size_t N>
IoCursor(char (&)[N]) -> IoCursor<io_cursor_detail::FixedSpanStorage>;
IoCursor(std::span<char>) -> IoCursor<io_cursor_detail::FixedSpanStorage>;
IoCursor(void*, size_t) -> IoCursor<io_cursor_detail::FixedSpanStorage>;

IoCursor(std::vector<char>&) -> IoCursor<io_cursor_detail::VectorGrowStorage>;
