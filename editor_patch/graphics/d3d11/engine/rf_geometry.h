#pragma once

// RED's level geometry under the game's rf:: names and members (game_patch/rf/geometry.h), for the shared
// Direct3D 11 core. Stock layouts: RED keeps the stock decal arrays where the game has its Alpine fields. The
// shim asserts every field the core touches against the editor's own GSolid, GRoom and GFace (level.h).

#include <cstddef>
#include <cstdint>
#include "rf_math.h"
#include "rf_gr.h"

namespace rf
{
    struct GVertex;
    struct GFaceVertex;
    struct GFace;
    struct GSurface;
    struct GBBox;
    struct GDecal;
    struct GPortal;
    struct GTextureMover;
    struct GRoom;
    struct GPathNode;
    struct GLightmap;
    struct GrLight;
    struct DecalPoly;

    struct GNodeNetwork
    {
        VArray<GPathNode*> nodes;
    };

    struct GClipWnd
    {
        float left;
        float top;
        float right;
        float bot;
    };

    enum GFaceListId
    {
        FACE_LIST_SOLID = 0,
        FACE_LIST_BBOX = 1,
        FACE_LIST_ROOM = 2,
        FACE_LIST_NUM = 3,
    };

    enum GFaceFlags
    {
        FACE_SHOW_SKY = 1,
        FACE_LIQUID = 0x4,
        FACE_IS_DETAIL = 0x8,
        FACE_SCROLL_TEXTURE = 0x10,
        FACE_FULL_BRIGHT = 0x20,
        FACE_SEE_THRU = 0x40,
        FACE_INVISIBLE = 0x2000,
        // Set by the decal clip to the face (0x00493ABE) for a decal without, and with, DF_LEVEL_DECAL
        FACE_HAS_DECAL = 0x100000,
        FACE_HAS_LEVEL_DECAL = 0x200000,
    };

    enum DecalFlags
    {
        DF_NEVER_DESTROY = 0x1,
        DF_SKIP_ORIENT_SETUP = 0x2,
        DF_RANDOM_ORIENT = 0x4,
        DF_LEVEL_DECAL = 0x8,
        DF_SELF_ILLUMINATED = 0x20,
        DF_TILING_U = 0x40,
        DF_TILING_V = 0x80,
        DF_FROM_WEAPON = 0x100,
        DF_GEOMOD = 0x200,
        DF_NEVER_SKIP_FADE_OUT = 0x400,
        DF_LIQUID = 0x8000000,
        DF_FADING_OUT = 0x40000000,
        DF_DESTROY_NOW = 0x80000000,
    };

    struct GSolid
    {
        GBBox *bbox;
        char name[64];
        int modifiability;
        Vector3 bbox_min;
        Vector3 bbox_max;
        float bounding_sphere_radius;
        Vector3 bounding_sphere_center;
        VList<GFace, FACE_LIST_SOLID> face_list;
        VArray<GVertex*> vertices;
        VArray<GRoom*> children;
        VArray<GRoom*> all_rooms;
        VArray<GRoom*> cached_normal_room_list;
        VArray<GRoom*> cached_detail_room_list;
        VArray<GPortal*> portals;
        VArray<GSurface*> surfaces;
        VArray<GVertex*> sel_vertices;
        VArray<GFace*> sel_faces;
        VArray<GFace*> last_sel_faces;
        FArray<GDecal*, 128> decals;
        VArray<GTextureMover*> texture_movers;
        GNodeNetwork nodes;
        ubyte cubes[64];
        float cube_size;
        float cube_zero_pos[3];
        int current_frame; // used for caching
        VArray<GrLight*> lights_affecting_me;
        int last_light_state;
        int field_370;
        int field_374;
    };
    static_assert(sizeof(GSolid) == 0x378);
    static_assert(offsetof(GSolid, face_list) == 0x70);
    static_assert(offsetof(GSolid, all_rooms) == 0x90);
    static_assert(offsetof(GSolid, cached_normal_room_list) == 0x9C);
    static_assert(offsetof(GSolid, cached_detail_room_list) == 0xA8);
    static_assert(offsetof(GSolid, surfaces) == 0xC0);
    static_assert(offsetof(GSolid, texture_movers) == 0x2F4);
    static_assert(offsetof(GSolid, current_frame) == 0x35C);

    // The stock room render cache header; the core only marks it (state).
    struct GCache
    {
        void* vertices;
        int num_vertices;
        int field_8;
        void* field_c;
        void* field_10;
        void* batches;
        short num_batches;
        short field_1a;
        int field_1c;
        int state; // 0 = valid, 1 = touched by boolean, 2 = rebuild on next render
    };
    static_assert(sizeof(GCache) == 0x24);
    static_assert(offsetof(GCache, state) == 0x20);

    struct GRoom
    {
        bool is_detail;
        bool is_sky;
        bool is_invisible;
        GCache *geo_cache;
        Vector3 bbox_min;
        Vector3 bbox_max;
        int room_index;
        int uid;
        VList<GFace, FACE_LIST_ROOM> face_list;
        VArray<GPortal*> portals;
        GBBox *bbox;
        bool is_blocked;
        bool is_cold;
        bool is_outside;
        bool is_airlock;
        bool is_pressurized;
        bool ambient_light_defined;
        Color ambient_light;
        char eax_effect[32];
        bool has_alpha;
        VArray<GRoom*> detail_rooms;
        GRoom *room_to_render_with;
        Plane room_plane;
        int last_frame_rendered_normal;
        int last_frame_rendered_alpha;
        float life;
        bool is_invincible;
        FArray<GDecal*, 48> decals;
        bool visited_this_frame;
        bool visited_this_search;
        int render_depth;
        int creation_id;
        GClipWnd clip_wnd;
        int bfs_visited;
        int liquid_type;
        bool contains_liquid;
        float liquid_depth;
        Color liquid_color;
        float liquid_visibility;
        int liquid_surface_bitmap;
        int liquid_surface_proctex_id;
        int liquid_ppm_u;
        int liquid_ppm_v;
        float liquid_angle;
        int liquid_alpha;
        bool liquid_plankton;
        int liquid_waveform;
        float liquid_surface_pan_u;
        float liquid_surface_pan_v;
        VArray<GrLight*> cached_lights;
        int light_state;
    };
    static_assert(sizeof(GRoom) == 0x1CC);
    static_assert(offsetof(GRoom, geo_cache) == 0x04);
    static_assert(offsetof(GRoom, bbox_min) == 0x08);
    static_assert(offsetof(GRoom, room_index) == 0x20);
    static_assert(offsetof(GRoom, uid) == 0x24);
    static_assert(offsetof(GRoom, face_list) == 0x28);
    static_assert(offsetof(GRoom, detail_rooms) == 0x6C);
    static_assert(offsetof(GRoom, room_to_render_with) == 0x78);
    static_assert(offsetof(GRoom, decals) == 0x9C);
    static_assert(offsetof(GRoom, clip_wnd) == 0x16C);
    static_assert(offsetof(GRoom, liquid_type) == 0x180);
    static_assert(offsetof(GRoom, contains_liquid) == 0x184);
    static_assert(offsetof(GRoom, liquid_depth) == 0x188);
    static_assert(offsetof(GRoom, liquid_color) == 0x18C);
    static_assert(offsetof(GRoom, liquid_visibility) == 0x190);
    static_assert(offsetof(GRoom, liquid_alpha) == 0x1A8);

    struct GFaceAttributes
    {
        uint flags;
        union {
            int group_id; // temporarily used for face grouping/sorting
            GTextureMover* texture_mover; // temporarily used by room render cache code
        };
        int bitmap_id;
        short portal_id; // portal index + 2 or 0
        short surface_index;
        int face_id;
        int smoothing_groups; // bitfield of smoothing groups

        bool is_show_sky() const
        {
            return (flags & FACE_SHOW_SKY) != 0;
        }

        bool is_liquid() const
        {
            return (flags & FACE_LIQUID) != 0;
        }

        bool is_see_thru() const
        {
            return (flags & FACE_SEE_THRU) != 0;
        }

        bool is_invisible() const
        {
            return (flags & FACE_INVISIBLE) != 0;
        }

        bool is_portal() const
        {
            return portal_id > 0;
        }
    };
    static_assert(sizeof(GFaceAttributes) == 0x18);

    struct GFace
    {
        Plane plane;
        Vector3 bounding_box_min;
        Vector3 bounding_box_max;
        GFaceAttributes attributes;
        GFaceVertex *edge_loop;
        GRoom *which_room;
        GBBox *which_bbox;
        DecalPoly *decal_list;
        short unk_cache_index;
        GFace* next[FACE_LIST_NUM];
    };
    static_assert(sizeof(GFace) == 0x60);
    static_assert(offsetof(GFace, attributes) == 0x28);
    static_assert(offsetof(GFace, edge_loop) == 0x40);
    static_assert(offsetof(GFace, which_room) == 0x44);
    static_assert(offsetof(GFace, decal_list) == 0x4C);
    static_assert(offsetof(GFace, next) == 0x54);

    // Alpine Faction: the longest face edge loop its code walks; a longer one is taken as corrupt.
    constexpr int max_face_vertices = 10000;

    struct GVertex
    {
        Vector3 pos;
        Vector3 rotated_pos;
        int last_frame; // last frame when vertex was transformed and possibly rendered
        int clip_codes; // also used for other things by geometry cache
        VArray<GFace*> adjacent_faces;
    };
    static_assert(sizeof(GVertex) == 0x2C);
    static_assert(offsetof(GVertex, adjacent_faces) == 0x20);

    struct GFaceVertex
    {
        GVertex *vertex;
        float texture_u;
        float texture_v;
        float lightmap_u;
        float lightmap_v;
        GFaceVertex *next;
        GFaceVertex *prev;
    };
    static_assert(sizeof(GFaceVertex) == 0x1C);

    struct GSurface
    {
        int index;
        int lightstate;
        ubyte flags;
        bool should_smooth;
        bool fullbright;
        GLightmap *lightmap;
        int xstart;
        int ystart;
        int width;
        int height;
        VArray<char> border_info;
        float x_pixels_per_meter;
        float y_pixels_per_meter;
        Vector3 bbox_mn;
        Vector3 bbox_mx;
        Vector2 uv_scale;
        Vector2 uv_add;
        int dropped_coefficient;
        int u_coefficient;
        int v_coefficient;
        int room_index;
        Plane plane;
    };
    static_assert(sizeof(GSurface) == 0x7C);
    static_assert(offsetof(GSurface, lightmap) == 0x0C);
    static_assert(offsetof(GSurface, bbox_mn) == 0x34);
    static_assert(offsetof(GSurface, room_index) == 0x68);

    struct GTextureMover
    {
        int face_id;
        float u_pan_speed;
        float v_pan_speed;
        VArray<GFace*> faces;
    };
    static_assert(sizeof(GTextureMover) == 0x18);

    struct GDecal
    {
        Vector3 pos;
        Matrix3 orient;
        Vector3 width;
        int bitmap_id;
        GRoom *room;
        GRoom *room2;
        GSolid *solid;
        ubyte alpha;
        int flags;
        int object_handle;
        float tiling_scale;
        Plane decal_poly_planes[6];
        Vector3 bb_min;
        Vector3 bb_max;
        DecalPoly *poly_list;
        int num_decal_polys;
        float lifetime_sec;
        GDecal *next;
        GDecal *prev;
        GSolid *editor_geometry;
    };
    static_assert(sizeof(GDecal) == 0xEC);

    struct DecalVertex
    {
        Vector2 uv;
        Vector2 lightmap_uv;
        Vector3 pos;
        Vector3 rotated_pos;
        int field_28;
    };
    static_assert(sizeof(DecalVertex) == 0x2C);

    struct DecalPoly
    {
        Vector2 uvs[25];
        int face_priority;
        int lightmap_bm_handle;
        int nv;
        DecalVertex verts[25];
        GFace *face;
        GDecal *my_decal;
        DecalPoly *next;
        DecalPoly *prev;
        DecalPoly *next_for_face;
    };
    static_assert(sizeof(DecalPoly) == 0x534);

    struct GLightmap
    {
        ubyte *unk;
        int w;
        int h;
        ubyte *buf;
        int bm_handle;
        int index;
    };
    static_assert(sizeof(GLightmap) == 0x18);

    enum ObjectFlags : int
    {
        OF_HIDDEN = 0x4000,
    };

    // The game's mover brush (an Object). RED has no movers, so it never has one; only the fields the core reads.
    struct MoverBrush
    {
        GRoom* room;
        char pad_04[0x3C - 0x04];
        Vector3 pos;
        Matrix3 orient;
        char pad_6C[0x7C - 0x6C];
        ObjectFlags obj_flags;
        char pad_80[0x28C - 0x80];
        MoverBrush* next;
        MoverBrush* prev;
        GSolid* geometry;
    };
    static_assert(offsetof(MoverBrush, pos) == 0x3C);
    static_assert(offsetof(MoverBrush, orient) == 0x48);
    static_assert(offsetof(MoverBrush, obj_flags) == 0x7C);
    static_assert(offsetof(MoverBrush, next) == 0x28C);
    static_assert(offsetof(MoverBrush, geometry) == 0x294);
}
