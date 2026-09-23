#pragma once

// Deliberately dependency-free: the renderer includes this for the tread resolve below.

namespace rf
{
    struct Entity;
}

// Client: load this hull's team texture variants now, so the first draw is a map lookup. The
// in-draw resolve stays as the fallback for meshes this does not reach.
void vehicle_team_textures_prime(rf::Entity* ep);

void vehicle_update_jeep_wheels();
void vehicle_update_treads();
void vehicle_drop_jeep_tire_mesh();
void vehicle_tread_runtime_reset();

// Once per hull mesh, from the mesh draw: latch which of the mesh's own texture handles are the
// belt, so vehicle_is_tread_bitmap is an int compare per batch. mesh_key is the VifMesh being
// drawn; the handle array is no key at all, because under alt_tex it is a reused stack local.
void vehicle_tread_resolve_mesh_bitmaps(int config_index, const void* mesh_key, const int* tex_handles,
                                        int num_tex_handles);

void vehicle_render_install();
