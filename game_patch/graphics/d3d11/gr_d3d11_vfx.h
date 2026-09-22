#pragma once

#include <d3d11.h>
#include <unordered_map>
#include <vector>
#include <common/ComPtr.h>
#include "../../rf/math/vector.h"
#include "../../rf/gr/gr.h"
#include "gr_d3d11_buffer.h"
#include "gr_d3d11_shader.h"
#include "gr_d3d11_vertex.h"

namespace rf
{
    struct VfxSfxoChunk;
    struct VfxSfxoRenderObj;
}

namespace gr::d3d11
{
    class RenderContext;

    // True if VfxMeshRenderer can draw this SFXO triangle chunk; also returns the object-space
    // bounding radius the caller needs for light gathering. Decided before any light-list
    // mutation so a "false" can still fall through to the stock CPU path untouched.
    bool vfx_gpu_eligible(const rf::VfxSfxoRenderObj* obj, float* radius_out);

    // GPU replacement for gr_d3d_render_vfx (0x00553EE0): uploads the engine's already
    // decompressed per-frame positions/UVs and draws per material through the standard mesh
    // shader with GPU lighting, instead of the stock per-face software T&L + gr_poly loop.
    class VfxMeshRenderer
    {
    public:
        VfxMeshRenderer(ComPtr<ID3D11Device> device, ShaderManager& shader_manager, RenderContext& render_context);
        void render(rf::VfxSfxoRenderObj* obj, float frame); // obj must have passed vfx_gpu_eligible

    private:
        // Compact copy of the chunk's static topology: the engine's 0x90-byte face records and
        // 0x2C-byte vertex records are three cache lines per face to walk every frame. Face corners
        // with the same (vertex, record, material, uv) are merged into one GPU vertex.
        struct Topology
        {
            struct Face
            {
                int vertex[3];
                int record[3]; // vertex record per corner, -1 if none
                int corner[3]; // unique vertex index
                int slot;
            };
            struct Unique
            {
                int vertex;
                int record;   // -1 if none
                int face;     // compact face index (normal fallback)
                int src_face; // original face index (face_uvs lookup)
                int uv_corner;
                int slot;
            };
            // validation against the live chunk
            int num_faces = 0;
            int num_vertices = 0;
            int num_records = 0;
            const void* faces_ptr = nullptr;
            const void* records_ptr = nullptr;
            std::vector<Face> faces; // only drawable faces
            std::vector<Unique> unique;
            std::vector<int> record_vertex; // per record: vertex index (-1 if invalid)
        };
        const Topology& get_topology(const rf::VfxSfxoChunk* chunk, const rf::VfxSfxoRenderObj* obj);

        RenderContext& render_context_;
        RingBuffer<GpuVertex> vertex_ring_buffer_;
        RingBuffer<rf::ushort> index_ring_buffer_;
        VertexShaderAndLayout vertex_shader_;
        ComPtr<ID3D11PixelShader> pixel_shader_;
        ComPtr<ID3D11PixelShader> pixel_shader_no_gas_;
        std::unordered_map<const rf::VfxSfxoChunk*, Topology> topology_cache_;
        // per-frame scratch, reused
        std::vector<GpuVertex> vertex_scratch_;
        std::vector<rf::ushort> index_scratch_;
        std::vector<rf::Vector3> face_normals_;
        std::vector<rf::Vector3> record_normals_;
        std::vector<rf::Color> record_lit_;
    };
}
