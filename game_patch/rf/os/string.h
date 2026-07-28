#pragma once

#include <cstring>
#include <format>
#include <utility>

namespace rf
{
    static auto& string_alloc = addr_as_ref<char*(unsigned size)>(0x004FF300);

    class String
    {
    public:
        // GCC follows closely Itanium ABI which requires to always pass objects by reference if class has
        // a non-trivial destructor. Therefore when passing a String by value the Pod struct should be used.
        struct Pod
        {
            int max_len = 0;
            char* buf = nullptr;
        };

    private:
        Pod m_pod;

    public:
        String()
        {
            AddrCaller{0x004FF3B0}.this_call(this);
        }

        String(const String& str)
        {
            AddrCaller{0x004FF410}.this_call(this, &str);
        }

        String(String&& str) noexcept
            : m_pod{str.m_pod}
        {
            str.m_pod = {};
        }

        String(const char* const c_str)
        {
            AddrCaller{0x004FF3D0}.this_call(this, c_str);
        }

        String(Pod pod) : m_pod(pod)
        {}

        String(const std::string_view str)
            : m_pod{
                .max_len = static_cast<int>(str.size() + 1uz),
                .buf = string_alloc(m_pod.max_len),
            }
        {
            std::memcpy(m_pod.buf, str.data(), str.size());
            m_pod.buf[str.size()] = '\0';
        }

        ~String()
        {
            AddrCaller{0x004FF470}.this_call(this);
        }

        operator const char*() const
        {
            return c_str();
        }

        operator std::string_view() const
        {
            return std::string_view{c_str()};
        }

        operator std::string() const
        {
            return std::string{c_str()};
        }

        operator Pod() const
        {
            return m_pod;
        }

        String& operator=(const String& other)
        {
            return AddrCaller{0x004FFA20}.this_call<String&>(this, &other);
        }

        String& operator=(String&& other)
        {
            if (this != &other) {
                std::swap(m_pod, other.m_pod);
            }
            return *this;
        }

        String& operator=(const char* const other)
        {
            return AddrCaller{0x004FFA80}.this_call<String&>(this, other);
        }

        [[nodiscard]] bool operator==(const String& other) const
        {
            return !std::strcmp(
                m_pod.buf ? m_pod.buf : "",
                other.m_pod.buf ? other.m_pod.buf : ""
            );
        }

        [[nodiscard]] bool operator==(const char* const other) const
        {
            return !std::strcmp(m_pod.buf ? m_pod.buf : "", other);
        }

        [[nodiscard]] const char *c_str() const
        {
            return AddrCaller{0x004FF480}.this_call<const char*>(this);
        }

        [[nodiscard]] int size() const
        {
            return AddrCaller{0x004FF490}.this_call<int>(this);
        }

        [[nodiscard]] bool empty() const
        {
            return size() == 0;
        }

        [[nodiscard]] String substr(const int begin, const int end) const
        {
            Pod result{};
            AddrCaller{0x004FF590}.this_call<String*>(this, &result, begin, end);
            return String{result};
        }

        [[nodiscard]] static String concat(const String& first, const String& second)
        {
            Pod result{};
            AddrCaller{0x004FFB50}.c_call<String&>(&result, &first, &second);
            return String{result};
        }

        template <typename... Args>
        static inline String format(
            const std::format_string<Args...> fmt,
            Args&&... args
        ) {
            const int len = std::formatted_size(fmt, std::forward<Args>(args)...);
            const int buf_size = len + 1;
            Pod result{
                .max_len = len,
                .buf = string_alloc(buf_size),
            };
            std::format_to_n(result.buf, len, fmt, std::forward<Args>(args)...);
            result.buf[len] = '\0';
            return String{result};
        }
    };
    static_assert(sizeof(String) == 8);
}

template <>
struct std::formatter<rf::String> {
    constexpr auto parse(std::format_parse_context& ctx) {
        return ctx.begin();
    }

    auto format(const rf::String& s, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", s.c_str());
    }
};

