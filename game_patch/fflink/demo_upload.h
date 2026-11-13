#pragma once

#include <string>

// Dedicated-server demo upload to FactionFiles. Auto-recorded .afd segments that carry a
// valid afstats identity are handed to FactionFiles via a two-call ticket/PUT flow (see
// the demo-upload contract). All queue/state mutation happens on the main thread; a single
// detached worker at a time reads the file, computes its CRC, and performs the HTTP calls,
// reporting the outcome back through fflink::enqueue_main_thread_task.
namespace fflink {

// Register the status console command and one-time setup. Called from fflink::do_patch().
void demo_upload_do_patch();

// Per-frame pump: starts the next due upload when idle and eligible. A cheap no-op off a
// stats-enabled server. Called from fflink::do_frame().
void demo_upload_do_frame();

// Enqueue a just-closed, kept, auto-recorded demo segment if it is eligible. No-op when
// upload is disabled or the file fails eligibility. Called from close_segment.
void demo_upload_on_segment_closed(const std::string& path);

} // namespace fflink
