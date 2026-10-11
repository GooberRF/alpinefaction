#pragma once

// An emergency block of address space held back from RED. The first of RED's (or, in MSVC builds, AF's) allocations
// that fails gives it back and is retried, and the guard "trips" so the running Build Geometry or Calculate Lighting
// can cancel at its next safe point instead of failing halfway. Once it is spent, a failure behaves as before.

// Names the address space RED runs with: 2 GB, or 4 GB when large address aware.
const char* memory_guard_advice();
inline constexpr const char* memory_guard_warning =
    "RED is nearly out of memory. Save your work under a new name and restart RED.";

// An allocation was served from the reserve since the last call, cleared for the caller to report.
bool memory_guard_take_tripped();
// The same, left for the reporter: a pass that allocates as it goes scales down for the rest of its run.
bool memory_guard_tripped();
// False when an unreported trip is pending (taken here) or the block is spent and cannot be held again: an operation
// that needs a lot of memory must not start.
bool memory_guard_ready();
// Holds the block again if it was spent and the address space allows, leaving any trip to be reported; true when held.
bool memory_guard_rearm();
// RED's idle tick: outside a build, bake or quiet scope, reports a trip once and re-arms.
void memory_guard_idle();
// Held across a stock command that runs RED's idle tick midway (the surface pass redraws through OnIdle): a trip is
// left to the command that reports it or, failing that, to the next idle tick.
class MemoryGuardQuietScope
{
public:
    MemoryGuardQuietScope();
    ~MemoryGuardQuietScope();
    MemoryGuardQuietScope(const MemoryGuardQuietScope&) = delete;
    MemoryGuardQuietScope& operator=(const MemoryGuardQuietScope&) = delete;
};
// An exception escaped stock code during `operation` (worded to follow "part-way through"), whose state can no longer
// be trusted: tells the user (the bake log in a headless bake) and ends the process without running anything else.
// Only from a catch handler: it rethrows the exception to name it in the log. `level_file_untouched`: the operation
// never writes the level file, so the message can say it is unchanged.
[[noreturn]] void memory_guard_fatal(const char* operation, bool level_file_untouched = true);

void ApplyMemoryGuardPatches();
