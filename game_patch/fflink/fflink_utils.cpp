#include "fflink_utils.h"

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

std::string sanitize_for_log(std::string_view in)
{
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        if (c < 0x20 || c == 0x7F) {
            out.push_back('.');
        }
        else {
            out.push_back(static_cast<char>(c));
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
