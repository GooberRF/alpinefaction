#pragma once

#include <cstdint>
#include <limits>
#include <string>

// The phases of one Calculate Lighting, in the order they run.
enum class BakePhase
{
    layout,
    surfaces,
    blend,
    smoothing,
    movers,
    terrain,
    overflow,
    encode,
    count,
};

constexpr unsigned bake_phase_bit(BakePhase phase)
{
    return 1u << static_cast<unsigned>(phase);
}

// Brackets one Calculate Lighting. In the editor, `phases` are listed in a progress window with a Cancel button;
// in a headless bake, each phase's time goes to the bake log. The bake_progress functions do nothing outside a scope.
class BakeProgressScope
{
public:
    explicit BakeProgressScope(unsigned phases);
    ~BakeProgressScope();
    BakeProgressScope(const BakeProgressScope&) = delete;
    BakeProgressScope& operator=(const BakeProgressScope&) = delete;

private:
    // false for a scope opened inside another, which leaves the outer one alone
    bool owner_ = false;
};

// Whether a scope is open.
bool bake_progress_active();

constexpr std::uint64_t bake_progress_expected_steps = std::numeric_limits<std::uint64_t>::max();

// Ends the running phase and starts `phase` with `total` steps (its expected steps by default); starting the
// running phase again does nothing.
void bake_progress_phase(BakePhase phase, std::uint64_t total = bake_progress_expected_steps);
// The steps a phase that has not started yet is expected to take, for the overall estimate.
void bake_progress_expect(BakePhase phase, std::uint64_t total);
// A listed phase that turned out not to be needed.
void bake_progress_skip(BakePhase phase);
void bake_progress_step(std::uint64_t steps = 1);
// Takes a memory guard trip, as each step does, without counting a step.
void bake_progress_poll();
// The running phase, BakePhase::count when none is.
BakePhase bake_progress_current();
// Cancel was confirmed: the bake skips the rest of its work and leaves nothing behind.
bool bake_progress_cancelled();
// The bake was cancelled because RED ran out of memory (the memory guard tripped).
bool bake_progress_out_of_memory();
// A message box to show once the progress window has closed.
void bake_progress_defer_message(const char* caption, const std::string& msg);

// The phases of one Build Geometry, in the order they run.
enum class BuildPhase
{
    terrain,
    brushes,
    portals,
    cleanup,
    rooms,
    tjoints,
    repair,
    holes,
    out_of_level,
    finish,
    count,
};

constexpr unsigned build_phase_bit(BuildPhase phase)
{
    return 1u << static_cast<unsigned>(phase);
}

// The progress window across one Build Geometry's idle ticks; these do nothing outside a build or while a bake
// has it.
void build_progress_begin(unsigned phases);
void build_progress_end();
void build_progress_phase(BuildPhase phase, std::uint64_t total = 0);
void build_progress_steps(std::uint64_t done, std::uint64_t total);
// The steps a phase that has not started yet is expected to take, for the overall estimate.
void build_progress_expect(BuildPhase phase, std::uint64_t total);
void build_progress_complete();
void build_progress_skip(BuildPhase phase);
// Pumps the window's messages without changing its progress.
void build_progress_poll();
void build_progress_disallow_cancel();
// Cancel was confirmed: the build stops at its next tick.
bool build_progress_cancelled();

void ApplyBakeProgressPatches();
