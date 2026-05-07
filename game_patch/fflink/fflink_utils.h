#pragma once

#include <functional>
#include <string>

namespace fflink {

// Enqueue a console line to be printed on the main thread.
void enqueue_console_line(std::string line);

// Print and clear any pending console messages enqueued by background workers.
void drain_pending_console();

// Schedule a function to be invoked on the main thread on the next fflink::do_frame() tick
void enqueue_main_thread_task(std::function<void()> task);

// Drain and run any pending main-thread tasks. MUST be called from the main thread.
void drain_pending_main_thread_tasks();

} // namespace fflink
