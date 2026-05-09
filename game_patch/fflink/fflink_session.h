#pragma once

#include <string>
#include <string_view>

namespace fflink {

enum class SessionStatus
{
    none,             // no exchange has been attempted yet (or no GSK configured)
    pending,          // exchange in flight
    valid,            // GSSK in hand
    bad_gsk_format,   // local validation rejected the configured GSK; will not retry
    rejected_by_server, // server returned unknown_or_disabled_gsk; will not retry until config change
    failed,           // last exchange attempt failed (transient); retry scheduled
};

struct SessionState
{
    SessionStatus status = SessionStatus::none;
    int server_id = 0;
    std::string last_error; // human-readable, safe to display
};

// Trigger an asynchronous session key exchange using the GSK from g_alpine_server_config.
// Safe to call multiple times; an in-flight exchange will not be re-issued.
// No-op if no GSK is configured.
void start_session_exchange();

// Snapshot the current state for display (returns a copy).
SessionState snapshot_state();

// Returns the in-memory GSSK if a valid session exists. Empty otherwise.
// The returned string is a copy; the GSSK must never be persisted to disk.
std::string get_gssk();

// Register console commands and any other one-time setup for the session subsystem.
// Called from fflink::do_patch().
void session_do_patch();

} // namespace fflink
