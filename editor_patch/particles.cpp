#include <algorithm>
#include <chrono>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include "mfc_types.h"
#include "vtypes.h"

namespace
{

// FUN_00480700 moves every particle by `dt`, which RED passes as a fixed 0.04 s each tick (OnIdle 0x00483326, the
// emitter dialog's timer 0x0045C1C2) while emitters spawn on real time; the real time since the last call is passed
// instead, at most a quarter second.
FunHook<void __cdecl(float)> particle_move_all_hook{
    0x00480700,
    [](float) {
        using clock = std::chrono::steady_clock;
        static clock::time_point last = clock::now();
        const clock::time_point now = clock::now();
        const float dt = std::clamp(std::chrono::duration<float>(now - last).count(), 0.0f, 0.25f);
        last = now;
        particle_move_all_hook.call_target(dt);
    },
};

// As the game's particle_emitter_create_hook: at most 60 spawns a second, so a continuous emitter spawns on a 1/60 s
// timer instead of every tick. Done per update since the emitter dialog rewrites these fields while it is open.
FunHook<void __fastcall(EditorParticleEmitter*)> particle_emitter_update_hook{
    0x004814F0,
    [](EditorParticleEmitter* emitter) FASTCALL_LAMBDA {
        constexpr float min_spawn_delay = 1.0f / 60.0f;
        if ((emitter->emitter_flags & EPEF_CONTINUOUS) || emitter->min_spawn_delay < min_spawn_delay) {
            if (emitter->emitter_flags & EPEF_CONTINUOUS) {
                emitter->emitter_flags &= ~EPEF_CONTINUOUS;
                // only a timed emitter that starts on had its timer armed
                if (emitter->spawn_timer < 0) {
                    timestamp_set(&emitter->spawn_timer, 0);
                }
            }
            emitter->min_spawn_delay = std::max(emitter->min_spawn_delay, min_spawn_delay);
            emitter->max_spawn_delay = std::max(emitter->max_spawn_delay, min_spawn_delay);
        }
        particle_emitter_update_hook.call_target(emitter);
    },
};

// As the game's particle_update_accel_patch: acceleration along a zero velocity's direction is skipped (ESI the
// particle)
CodeInjection particle_update_accel_patch{
    0x0047FCFE,
    [](auto& regs) {
        const auto* particle = reinterpret_cast<const EditorParticle*>(static_cast<uintptr_t>(regs.esi));
        if (length(particle->velocity) < 0.0001f) {
            regs.eip = 0x0047FD7F;
        }
    },
};

} // namespace

void ApplyParticlePatches()
{
    particle_move_all_hook.install();
    particle_emitter_update_hook.install();
    particle_update_accel_patch.install();
}
