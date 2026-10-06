#include "fflink_utils.h"

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

#include <xlog/xlog.h>

#include "../os/console.h"

namespace fflink {

namespace {

std::mutex g_pending_console_mutex;
std::vector<std::string> g_pending_console_lines;

std::mutex g_pending_tasks_mutex;
std::vector<std::function<void()>> g_pending_tasks;

bool is_lower_hex_char(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

} // namespace

void enqueue_console_line(std::string line)
{
    std::lock_guard lock(g_pending_console_mutex);
    g_pending_console_lines.push_back(std::move(line));
}

void drain_pending_console()
{
    std::vector<std::string> drained;
    {
        std::lock_guard lock(g_pending_console_mutex);
        if (g_pending_console_lines.empty()) {
            return;
        }
        drained.swap(g_pending_console_lines);
    }
    for (const auto& line : drained) {
        rf::console::print("{}", line);
    }
}

void enqueue_main_thread_task(std::function<void()> task)
{
    std::lock_guard lock(g_pending_tasks_mutex);
    g_pending_tasks.push_back(std::move(task));
}

void drain_pending_main_thread_tasks()
{
    std::vector<std::function<void()>> drained;
    {
        std::lock_guard lock(g_pending_tasks_mutex);
        if (g_pending_tasks.empty()) {
            return;
        }
        drained.swap(g_pending_tasks);
    }
    for (auto& task : drained) {
        try {
            task();
        }
        catch (const std::exception& e) {
            xlog::error("[fflink] main-thread task threw: {}", e.what());
        }
    }
}

std::string sanitize_for_log(std::string_view in, size_t max_len)
{
    std::string out;
    out.reserve(std::min(in.size(), max_len));
    for (unsigned char c : in) {
        if (out.size() >= max_len) {
            break;
        }
        if (c < 0x20 || c == 0x7F) {
            out.push_back('.');
        }
        else {
            out.push_back(static_cast<char>(c));
        }
    }
    // Only when the cap actually truncated the input: don't leave a partial UTF-8
    // sequence dangling at the boundary. C0/DEL are the only bytes rewritten above and
    // they are all single-byte, so high bytes survive verbatim and this walk is exact.
    if (out.size() == max_len && in.size() > max_len) {
        size_t i = out.size();
        while (i > 0 && (static_cast<unsigned char>(out[i - 1]) & 0xC0) == 0x80) {
            --i; // step back over continuation bytes (10xxxxxx)
        }
        if (i > 0) {
            const unsigned char lead = static_cast<unsigned char>(out[i - 1]);
            size_t seq_len = 0;
            if (lead < 0x80) seq_len = 1;
            else if ((lead & 0xE0) == 0xC0) seq_len = 2;
            else if ((lead & 0xF0) == 0xE0) seq_len = 3;
            else if ((lead & 0xF8) == 0xF0) seq_len = 4;
            const size_t have = out.size() - (i - 1);
            if (seq_len == 0 || have < seq_len) {
                out.resize(i - 1); // drop the incomplete (or invalid) trailing sequence
            }
        }
    }
    return out;
}

bool is_valid_gsk_format(std::string_view gsk)
{
    if (gsk.size() != 32) {
        return false;
    }
    for (char c : gsk) {
        if (!is_lower_hex_char(c)) {
            return false;
        }
    }
    return true;
}

bool is_valid_stats_key_format(std::string_view key)
{
    if (key.size() != 32) {
        return false;
    }
    for (char c : key) {
        const bool ok =
            (c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z');
        if (!ok) {
            return false;
        }
    }
    return true;
}

} // namespace fflink
