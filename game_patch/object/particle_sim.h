#pragma once

namespace rf
{
    struct ParticleEmitter;
}

void particle_sim_do_patch();
void particle_sim_render_ui();
void particle_sim_note_create(int pool_id);
void particle_sim_level_reset();
bool particle_sim_emitter_culled(const rf::ParticleEmitter* emitter);
// Engine emitter records available in total, the stock 128 plus the ones Alpine adds.
int particle_sim_emitter_capacity();
// Particles, plankton and weather draw on the GPU: Direct3D 11 unless dbg_particlegpu turned it off.
bool particle_sim_gpu_enabled();
