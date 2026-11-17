#pragma once

// Deliberately dependency-free: the renderer includes this for the tread resolve below.

namespace rf
{
    struct Entity;
    struct VMesh;
}

namespace rf::gr
{
    struct Color;
}

// Render: a Bagman bag carrier aboard, else the driver, else the lowest occupied seat; null for an
// empty hull, which never outlines.
rf::Entity* vehicle_outline_occupant(rf::Entity* vehicle);

// Render: a jeep's four tires - instances of one shared static mesh, not hull geometry.
void vehicle_render_jeep_tires(rf::Entity* ep);

// The shared tire mesh for this level, or null before the first jeep is seen / if it failed to load.
// The D3D11 outline pass must register its sub-meshes under each jeep's entity handle, or four
// separate static draws inherit the last drawn character's outline.
rf::VMesh* vehicle_jeep_tire_mesh();

// Render: the entity whose entity_render call is on the stack, or null outside one.
rf::Entity* vehicle_rendering_entity();

// The team whose art the hull is drawn with: its driver's, else its affiliation; -1 for stock art.
int vehicle_render_team(rf::Entity* vehicle);
// Is this a tire or screw mesh, which keeps its stock art on any hull?
bool vehicle_is_spinner_mesh_filename(const char* filename);

// Render: the UV offset for the belt of the tracked hull currently being rendered, on that class's
// tiling axis, plus its table row. False with all outputs cleared for every other draw.
bool vehicle_tread_scroll_for_draw(int& config_out, float& u_out, float& v_out);

// Is this bitmap the belt texture of that table row? Matched on the BASENAME: an ATX has no ext.
bool vehicle_is_tread_bitmap(int bm_handle, int config_index);

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

// Client: a red film over everything the hull draws for a moment after it loses life to anything.
void vehicle_hit_flash_start(int vehicle_handle);
void vehicle_hit_flash_do_frame();
void vehicle_hit_flash_clear();
// The film to draw over this entity's mesh now; alpha 0 for none.
rf::gr::Color vehicle_hit_flash_color(const rf::Entity* ep);

void vehicle_render_install();
