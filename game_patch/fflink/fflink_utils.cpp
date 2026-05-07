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

} // namespace fflink
