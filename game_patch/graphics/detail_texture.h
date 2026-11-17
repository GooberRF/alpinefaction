#pragma once

#include <optional>
#include "../rf/gr/gr.h"
#include "../rf/physics.h"

struct DetailTexture
{
    int bm;
    float texels_per_repeat; // base texels one repeat of the detail texture spans
    float strength;
    float inv_mean;
};

// As fallback_material: the draw takes no automatic detail
inline constexpr int detail_no_auto = -1;

// nullopt for none. fallback_material is used in automatic mode when the texture has no material.
std::optional<DetailTexture> detail_texture_find(int bm_handle, int fallback_material = rf::MATERIAL_DEFAULT);

// fallback_material for a draw blended with `blend`: additive effects and glows take no automatic detail
inline int detail_texture_draw_material(rf::gr::AlphaBlend blend, int material)
{
    const bool additive = blend == rf::gr::ALPHA_BLEND_ADDITIVE || blend == rf::gr::ALPHA_BLEND_ALPHA_ADDITIVE;
    return additive ? detail_no_auto : material;
}

// The fallback material of a first person weapon texture: metal, but flesh for the fpgun.tbl arms
int detail_texture_first_person_material(int bm_handle);

// RAII: meshes drawn while alive take this material as their fallback; the mesh path gets no object
class [[nodiscard]] ScopedMeshObjectMaterial
{
public:
    explicit ScopedMeshObjectMaterial(int material) noexcept : prev_{material_} { material_ = material; }
    ~ScopedMeshObjectMaterial() noexcept { material_ = prev_; }
    ScopedMeshObjectMaterial(const ScopedMeshObjectMaterial&) = delete;
    ScopedMeshObjectMaterial& operator=(const ScopedMeshObjectMaterial&) = delete;

    static int current() noexcept { return material_; }

private:
    static inline int material_ = rf::MATERIAL_DEFAULT;
    int prev_;
};

// Forgets every handle's detail; bitmap handles mean other textures in the next level.
void detail_texture_level_reset();

void detail_texture_register_commands();
