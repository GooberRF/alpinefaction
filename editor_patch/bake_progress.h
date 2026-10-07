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
// in a headless bake, each phase's time goes to the bake log. The functions below do nothing outside a scope.
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
// The running phase, BakePhase::count when none is.
BakePhase bake_progress_current();
// Cancel was confirmed: the bake skips the rest of its work and leaves nothing behind.
bool bake_progress_cancelled();
// A message box to show once the progress window has closed.
void bake_progress_defer_message(const char* caption, const std::string& msg);

void ApplyBakeProgressPatches();
