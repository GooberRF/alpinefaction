#include "fflink_utils.h"

#include <mutex>
#include <utility>
#include <vector>

#include "../os/console.h"

namespace fflink {

namespace {

std::mutex g_pending_console_mutex;
std::vector<std::string> g_pending_console_lines;

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

} // namespace fflink
