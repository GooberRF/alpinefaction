#include <algorithm>
#include <new>
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include <xlog/xlog.h>
#include "../rf/os/string.h"
#include "string.h"

namespace
{
rf::String* string_substr_return_empty(rf::String* str_out)
{
    if (!str_out) {
        return nullptr;
    }
    new (str_out) rf::String("");
    return str_out;
}

int string_substr_bounded_content_len(const char* src_buf, int max_len)
{
    if (!src_buf || max_len <= 0) {
        return 0;
    }

    int len = 0;
    while (len < max_len && src_buf[len] != '\0') {
        ++len;
    }
    return len;
}

rf::String* __fastcall string_substr_safeguard_fn(
    rf::String* this_ptr, void* edx, rf::String* str_out, int begin, int end_inclusive);

FunHook<rf::String* __fastcall(rf::String*, void*, rf::String*, int, int)> string_substr_safeguard_hook{
    0x004FF590,
    string_substr_safeguard_fn,
};

rf::String* __fastcall string_substr_safeguard_fn(
    rf::String* this_ptr, void* edx, rf::String* str_out, int begin, int end_inclusive)
{
    static bool logged_invalid_ptr = false;
    static bool logged_len_sanitized = false;
    static bool logged_sanitized_range = false;

    if (!str_out) {
        return nullptr;
    }
    if (!this_ptr) {
        if (!logged_invalid_ptr) {
            xlog::debug("String::substr safeguard: null source string pointer");
            logged_invalid_ptr = true;
        }
        return string_substr_return_empty(str_out);
    }

    // In RF runtime, String::Pod::max_len stores current string length (not capacity).
    const auto* src_pod = reinterpret_cast<const rf::String::Pod*>(this_ptr);
    const char* src_buf = src_pod->buf;
    const int src_declared_len = src_pod->max_len;
    if (!src_buf || src_declared_len < 0) {
        if (!logged_invalid_ptr) {
            xlog::debug("String::substr safeguard: invalid source string state (buf={}, len={})",
                static_cast<const void*>(src_buf), src_declared_len);
            logged_invalid_ptr = true;
        }
        return string_substr_return_empty(str_out);
    }

    constexpr int k_max_reasonable_len = 1 << 20;
    const int src_content_len = string_substr_bounded_content_len(src_buf, k_max_reasonable_len);

    int src_len = src_declared_len;
    if (src_len > src_content_len) {
        src_len = src_content_len;
    }
    if (src_len > k_max_reasonable_len) {
        src_len = k_max_reasonable_len;
    }
    if (src_len != src_declared_len && !logged_len_sanitized) {
        xlog::debug("String::substr safeguard: sanitized suspicious source length declared={} content={}",
            src_declared_len, src_content_len);
        logged_len_sanitized = true;
    }
    if (src_len <= 0) {
        return string_substr_return_empty(str_out);
    }

    const bool had_invalid_range =
        begin < 0
        || begin > src_len
        || end_inclusive < -1
        || (end_inclusive != -1 && end_inclusive >= src_len)
        || (end_inclusive != -1 && end_inclusive < begin);

    const int resolved_end = (end_inclusive == -1) ? (src_len - 1) : end_inclusive;
    const int safe_begin = std::clamp(begin, 0, src_len);

    int safe_end = resolved_end;
    if (safe_end < 0) {
        safe_end = 0;
    }
    else if (safe_end >= src_len) {
        safe_end = src_len - 1;
    }

    if (safe_end < safe_begin) {
        if (had_invalid_range && !logged_sanitized_range) {
            xlog::debug("String::substr safeguard: dropped invalid range begin={} end={} len={}",
                begin, end_inclusive, src_len);
            logged_sanitized_range = true;
        }
        return string_substr_return_empty(str_out);
    }

    if (had_invalid_range && !logged_sanitized_range) {
        xlog::debug("String::substr safeguard: sanitized range begin={} end={} len={} -> begin={} end={} str=\"{}\"",
            begin, end_inclusive, src_len, safe_begin, safe_end, src_buf);
        logged_sanitized_range = true;
    }

    return string_substr_safeguard_hook.call_target(this_ptr, edx, str_out, safe_begin, safe_end);
}
}

void misc_string_apply_patch()
{
    string_substr_safeguard_hook.install();
}
