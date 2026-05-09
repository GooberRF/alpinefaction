#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace fflink {

// Enqueue a console line to be printed on the main thread.
void enqueue_console_line(std::string line);

// Print and clear any pending console messages enqueued by background workers.
void drain_pending_console();

// Schedule a function to be invoked on the main thread on the next fflink::do_frame() tick
void enqueue_main_thread_task(std::function<void()> task);

// Drain and run any pending main-thread tasks. MUST be called from the main thread.
void drain_pending_main_thread_tasks();

// Replace control characters (and anything outside printable ASCII) with '.'
// before logging server-controlled or otherwise untrusted bytes, so a hostile
// or malformed response can't inject newlines / ANSI escapes into the log.
std::string sanitize_for_log(std::string_view in);

} // namespace fflink
