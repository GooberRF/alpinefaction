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

// Sanitize response strings before logging
std::string sanitize_for_log(std::string_view in);

// GSK format: exactly 32 lowercase hex chars.
bool is_valid_gsk_format(std::string_view gsk);

// Session/stats key format: exactly 32 alphanumeric chars. Shared by the GSSK,
// the player stats key (PSK) and the player stats session key (PSSK).
bool is_valid_stats_key_format(std::string_view key);

} // namespace fflink
