#pragma once

#include <string>

namespace fflink {

// Enqueue a console line to be printed on the main thread.
void enqueue_console_line(std::string line);

// Print and clear any pending console messages enqueued by background workers.
void drain_pending_console();

} // namespace fflink
