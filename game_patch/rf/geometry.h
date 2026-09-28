#pragma once

#include <cstddef>
#include "math/vector.h"
#include "math/matrix.h"
#include "math/plane.h"
#include "os/array.h"
#include "os/string.h"
#include "os/linklist.h"
#include "gr/gr.h"
#include "sound/sound.h"
#include "object.h"

namespace rf
{
    struct GCollisionInput;
    struct GCollisionOutput;
    struct GVertex;
    struct GFaceVertex;
    struct GFace;
    struct GSurface;
    struct GCache;
    struct GBBox;
    struct GDecal;
    struct GPortal;
    struct GTextureMover;
    struct GRoom;
    struct GPathNode;
    struct GLightmap;
    struct GrLight;
    struct DecalPoly;

    using GPathNodeType = int;

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
        // Set when 0x004D00B0 splits an edge of a face with FACE_HAS_LEVEL_DECAL; 0x004DBBB0 then
        // recomputes the face's decal UVs.
        FACE_LEVEL_DECAL_UVS_STALE = 0x80000,
        // Set by g_decal_clip_to_face for a DF_LEVEL_DECAL decal
        FACE_HAS_LEVEL_DECAL = 0x200000,
        // Boolean face state. State 2 (0x004DC990) marks both faces of a pair it intersected (0x004DEA10);
        // state 0 (0x004DBDF0) clears the mark and gives the second operand's faces type 1 (0x004DEA30).
        FACE_BOOLEAN_TYPE_1 = 0x800000,
        FACE_BOOLEAN_INTERSECTED = 0x8000000,
        // The side a boolean classified the face on, bits 28-30 (0x004DE9E0)
        FACE_BOOLEAN_SIDE = 0x70000000,
    };
    constexpr int face_boolean_side_shift = 28;

    enum CollideFlags
    {
        CF_ANY_HIT = 0x1,
        CF_SKIP_SEE_THRU = 0x2,
        CF_SKIP_SHOOT_THRU = 0x4,
        CF_SEE_THRU_ALPHA_TEST = 0x8,
        CF_SHOOT_THRU_ALPHA_TEST = 0x10,
        CF_SKIP_DESTRUCTIBLE_GLASS = 0x20,
        CF_PROCESS_LIQUID_FACES = 0x40,
        CF_PROCESS_INVISIBLE_FACES = 0x80,
    };

    enum GCollisionFlags
    {
        GCF_ANY_HIT = 0x1, // stop at first detected hit
        GCF_PROCESS_PORTALS = 0x2,
        GCF_MESH_SPACE = 0x4, // coordinates in model space
        GCF_PROCESS_SKYROOM = 0x8,
        GCF_10 = 0x10,
        GCF_SKIP_SEE_THRU = 0x20,
        GCF_SKIP_SHOOT_THRU = 0x40,
        GCF_SEE_THRU_ALPHA_TEST = 0x80,
        GCF_SHOOT_THRU_ALPHA_TEST = 0x100,
        GCF_200 = 0x200,
        GCF_SKIP_DESTRUCTIBLE_GLASS = 0x400,
        GCF_PROCESS_SHOW_SKY_FACES = 0x800,
        GCF_PROCESS_LIQUID_FACES = 0x1000,
        GCF_PROCESS_INVISIBLE_FACES = 0x2000,
    };

    enum class GBooleanOperation
    {
        BOP_ALL = 0x0,
        BOP_UNION = 0x1,
        BOP_INTERSECTION = 0x2,
        BOP_DIFFERENCE = 0x3,
        BOP_DIFFERENCE_PORTAL = 0x4,
        BOP_PORTAL = 0x5,
        BOP_LIQUID = 0x6,
        BOP_UNION_NONE_FROM_B = 0x7,
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

    enum GeomodFlags : uint32_t {
        GEOMOD_LOCAL_CREATED = 0x01,
        GEOMOD_SKIP_CSG = 0x02,
        GEOMOD_FROM_SERVER = 0x04,
        GEOMOD_ORIENTED = 0x08, // directional orientation
        GEOMOD_ICE_TEXTURE = 0x10,
        GEOMOD_RF2_STYLE = 0x20, // Alpine 1.3
    };

    // Material type for breakable detail brushes (life != -1)
    enum class DetailMaterial : uint8_t {
        Glass  = 0, // default — stock glass shatter behavior
        Rock   = 1,
        Wood   = 2,
        Metal  = 3,
        Cement = 4,
        Ice    = 5,
        Count
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
#ifdef ALPINE_FACTION
        VArray<GDecal*> decals;
        int padding[126];
#else
        FArray<GDecal*, 128> decals;
#endif
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

        void collide(GCollisionInput *in, GCollisionOutput *out, bool clear_fraction)
        {
            AddrCaller{0x004DF1C0}.this_call(this, in, out, clear_fraction);
        }

        void set_levelmod_blast_autotexture_ppm(float ppm)
        {
            AddrCaller{0x004F8730}.this_call(this, ppm);
        }

        // Extract all faces with matching group_id into a new GSolid.
        // Removes originals from this solid (face_list, room face_list, vertices).
        GSolid* extract_faces_by_group(int group_id)
        {
            return AddrCaller{0x004d0590}.this_call<GSolid*>(this, group_id);
        }

        // Find room containing the given position. hint can be null.
        // FUN_004cd970: __thiscall, RET 0x10 (4 stack params)
        GRoom* find_room(GRoom* hint, const Vector3* pos1, const Vector3* pos2, void* param4)
        {
            return AddrCaller{0x004CD970}.this_call<GRoom*>(this, hint, pos1, pos2, param4);
        }

        // Put every room's last-rendered-frame markers back to -1 so a following pass over the
        // same solid doesn't skip rooms an earlier one already drew.
        void reset_room_render_frames()
        {
            AddrCaller{0x004D2E10}.this_call(this);
        }
    };
    static_assert(sizeof(GSolid) == 0x378);

    // A legacy room-cache batch as geo_cache_prepare_room 0x004F0C00 builds it; the static solid
    // renderer 0x0055F5E0 draws it with `mode` and writes `color` into every vertex's diffuse.
    struct GCacheBatchFace
    {
        char pad[0x18];
        GFace* face;
    };
    static_assert(sizeof(GCacheBatchFace) == 0x1C);

    struct GCacheBatch
    {
        char pad0[0x20];
        gr::Mode mode;
        unsigned color; // D3DCOLOR
        char pad28[0x8];
        GCacheBatchFace* faces;
        char pad34[0xC];
        int bm0;
        int bm1;
        short num_vertices;
        short num_faces;
        char pad4c[0x4];
    };
    static_assert(sizeof(GCacheBatch) == 0x50);
    static_assert(offsetof(GCacheBatch, mode) == 0x20 && offsetof(GCacheBatch, faces) == 0x30);
    static_assert(offsetof(GCacheBatch, bm0) == 0x40 && offsetof(GCacheBatch, num_faces) == 0x4A);

    // Stock (D3D9) room render cache header.
    struct GCache
    {
        void* vertices;        // num_vertices * Vector3
        int num_vertices;
        int field_8;
        void* field_c;
        void* field_10;
        GCacheBatch* batches;
        short num_batches;
        short field_1a;
        int field_1c;
        int state;             // 0 = valid, 1 = touched by boolean (0x004DDA88), 2 = rebuild on next render
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
#ifdef ALPINE_FACTION
        VArray<GDecal*> decals;
        bool is_geoable;                // Alpine 1.3: rf2-style brush-based geoable
        DetailMaterial material_type;   // Alpine 1.3: breakable brush material
        bool no_debris;                 // Alpine 1.3: skip debris creation on destruction
        char _pad_geoable[1];
        int padding[45];
#else
        FArray<GDecal*, 48> decals;
#endif
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

        bool is_breakable_glass()
        {
            return AddrCaller{0x00465F00}.this_call<bool>(this);
        }
    };
    static_assert(sizeof(GRoom) == 0x1CC);
    static_assert(offsetof(GRoom, clip_wnd) == 0x16C);
    static_assert(offsetof(GRoom, contains_liquid) == 0x184);

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

        // FUN_004e03e0: count vertices in the circular edge_loop
        int vertex_count() const
        {
            return AddrCaller{0x004E03E0}.this_call<int>(this);
        }
    };
    static_assert(sizeof(GFace) == 0x60);

    // Alpine Faction: the longest face edge loop its code walks; a longer one is taken as corrupt.
    constexpr int max_face_vertices = 10000;

    // A node of a solid's or room's bounding box tree (0x004F97B0 allocates them)
    struct GBBox
    {
        Vector3 min;
        Vector3 max;
        VList<GFace, FACE_LIST_BBOX> face_list;
        GBBox* children[2];
    };
    static_assert(sizeof(GBBox) == 0x28);
    static_assert(offsetof(GBBox, face_list) == 0x18 && offsetof(GBBox, children) == 0x20);

    struct GVertex
    {
        Vector3 pos;
        Vector3 rotated_pos;
        int last_frame; // last frame when vertex was transformed and possibly rendered
        int clip_codes; // also used for other things by geometry cache
        VArray<GFace*> adjacent_faces;
    };
    static_assert(sizeof(GVertex) == 0x2C);

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
        VArray<char> border_info; // Unknown
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

    struct GTextureMover
    {
        int face_id;
        float u_pan_speed;
        float v_pan_speed;
        VArray<GFace*> faces;

        void update_solid(GSolid* solid)
        {
            AddrCaller{0x004E60C0}.this_call(this, solid);
        }
    };
    static_assert(sizeof(GTextureMover) == 0x18);

    struct GPathNode
    {
        Vector3 pos;
        Vector3 use_pos;
        float original_radius;
        float radius;
        float height;
        float pause_time_seconds;
        VArray<GPathNode*> visible_nodes;
        bool visited;
        bool unusable;
        short adjacent;
        float distance;
        GPathNode *backptr;
        GPathNodeType type;
        bool directional;
        Matrix3 orient;
        int index;
        VArray<int> linked_uids;
    };
    static_assert(sizeof(GPathNode) == 0x7C);

    struct GDecalCreateInfo
    {
        Vector3 pos;
        Matrix3 orient;
        Vector3 extents;
        int texture;
        GRoom* room;
        ubyte alpha;
        ubyte pad[3];
        int flags;
        int object_handle;
        GSolid* solid;
        float scale;
    };
    static_assert(sizeof(GDecalCreateInfo) == 0x58);

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

    struct GCollisionInput
    {
        GFace *face;
        Vector3 geometry_pos;
        Matrix3 geometry_orient;
        Vector3 start_pos;
        Vector3 len;
        float radius;
        int flags;
        Vector3 start_pos_transformed;
        Vector3 len_transformed;

        GCollisionInput()
        {
            AddrCaller{0x004161F0}.this_call(this);
        }
    };
    static_assert(sizeof(GCollisionInput) == 0x6C);

    struct GCollisionOutput
    {
        int num_hits;
        float fraction;
        Vector3 hit_point;
        Vector3 normal;
        int field_20;
        GFace *face;

        GCollisionOutput()
        {
            AddrCaller{0x00416230}.this_call(this);
        }
    };
    static_assert(sizeof(GCollisionOutput) == 0x28);

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

    using ProcTexType = int;
    struct GProceduralTexture
    {
        int last_frame_updated;
        int last_frame_needs_updating;
        GProceduralTexture *next;
        GProceduralTexture *prev;
        int width;
        int height;
        int user_bm_handle;
        ProcTexType type;
        void (*update_function)(GProceduralTexture *pt);
        int base_bm_handle;
        float slide_pos_xc; // unused?
        float slide_pos_xt;
        float slide_pos_yc;
        float slide_pos_yt;
    };
    static_assert(sizeof(GProceduralTexture) == 0x38);

    struct GPortalObject
    {
        unsigned int id;
        Vector3 pos;
        float radius;
        bool has_alpha;
        bool did_draw;
        bool is_behind_brush;
        bool lights_enabled;
        bool use_static_lights;
        Plane *object_plane;
        Vector3 *bbox_min;
        Vector3 *bbox_max;
        float z_value;
        void (*render_function)(int, GSolid *);
    };
    static_assert(sizeof(GPortalObject) == 0x30);

    struct GeomodParams
    {
        int shape_index;
        int room_index;
        Vector3 pos;
        Matrix3 orient;
        int flags;
        float scale;
        Vector3 hit_normal;
        Vector3 field_4C;
        Vector3 field_58;
    };
    static_assert(sizeof(GeomodParams) == 0x64);

    struct GeomodEvent
    {
        GeomodEvent* next;
        GeomodEvent* prev;
        GeomodParams parameters;
        void* smoke_emitters[6];
    };
    static_assert(sizeof(GeomodEvent) == 0x84);

    static auto& geomod_queue_add = addr_as_ref<void(GeomodParams* params)>(0x00437230);
    // geomod_create's (0x00467020) own flag, not a GeomodParams flag: crater scale 1.0 instead of radius-derived.
    constexpr int geomod_create_flag_unit_scale = 0x8;
    static auto& g_geomod_pending_list = addr_as_ref<GeomodEvent>(0x00637168);

    // Geomod state machine globals
    static auto& g_geomod_pos = addr_as_ref<Vector3>(0x006485A0);
    static auto& g_geomod_outer_state = addr_as_ref<int>(0x0059C9F4);        // states 0-3, -1=done
    static auto& g_boolean_inner_state = addr_as_ref<int>(0x005A3A34);       // states 0-7 in FUN_004dbc50
    static auto& g_boolean_fast_path_var = addr_as_ref<int>(0x01370F64);
    // Operands of the boolean FUN_004dbc50 iterates
    static auto& g_boolean_solid = addr_as_ref<GSolid*>(0x00C968B4);
    static auto& g_boolean_op = addr_as_ref<GBooleanOperation>(0x00C9B4C0);
    // The faces inner state 1 finds outside the other solid
    static auto& g_boolean_outside_list = addr_as_ref<VList<GFace, FACE_LIST_SOLID>>(0x00C9F5A0);
    // The first solid's faces inner state 0 registered
    static auto& g_boolean_num_registered_faces = addr_as_ref<int>(0x00C9F624);
    // The rooms the boolean changed, which state 5 relinks the detail rooms of
    static auto& g_boolean_affected_rooms = addr_as_ref<GRoom*[(0x00C9F638 - 0x00C9F4DC) / 4]>(0x00C9F4DC);
    static auto& g_boolean_num_affected_rooms = addr_as_ref<int>(0x00C9F638);
    static auto& g_level_solid = addr_as_ref<GSolid*>(0x006460E8);
    static auto& g_geomod_crater_solid = addr_as_ref<GSolid*>(0x00646A20);
    static auto& geomod_get_crater_solid = addr_as_ref<GSolid*(int shape_index)>(0x004375B0); // null if out of range
    static auto& g_geomod_texture_index = addr_as_ref<int>(0x00647C94);
    // Bitmaps FUN_004f8740 puts on new crater faces; geomod_init sets all three to the level rock texture.
    static auto& g_boolean_crater_face_bitmaps = addr_as_ref<int[3]>(0x005A3EA0);
    static auto& g_geomod_scale = addr_as_ref<float>(0x00648598);
    static auto& g_geomod_flags = addr_as_ref<uint8_t>(0x0064858C);       // bit 0x1=local, 0x8=driller
    static auto& g_num_geomods_this_level = addr_as_ref<int>(0x00647C9C);
    // geomod_create 0x00467020 writes each crater's records at index g_num_geomods_this_level.
    constexpr int max_geomod_craters = 128;
    static auto& g_geomod_crater_records = addr_as_ref<uint8_t[max_geomod_craters][0x20]>(0x00648600);
    static auto& g_geomod_crater_pushes = addr_as_ref<uint8_t[max_geomod_craters][0x24]>(0x00646A28);
    static auto& g_geomod_separate_solids = addr_as_ref<bool>(0x00647C28);

    // Geomod emitter template indices (set by geomod_init FUN_00437130)
    static auto& g_geomod_emitter_default_idx = addr_as_ref<int>(0x00596EE4);
    static auto& g_geomod_emitter_driller_idx = addr_as_ref<int>(0x00596EE8);

    // Geomod effect constants
    static auto& g_geomod_emitter_radius_scale = addr_as_ref<float>(0x005895D4);
    static auto& g_geomod_shake_threshold_sq = addr_as_ref<float>(0x005894B4);

    // Geomod effect functions
    static auto& geomod_push_nearby_entities = addr_as_ref<bool(Vector3* pos)>(0x004C0160);
    static auto& g_decal_add = addr_as_ref<GDecal*(GDecalCreateInfo* dci)>(0x004D52E0);
    static auto& g_decal_destroy = addr_as_ref<void(GDecal*)>(0x004D6C50);
    // Live decals, linked through GDecal::next
    static auto& g_decal_list = addr_as_ref<GDecal*>(0x00C4D56C);
    // The faces the orientation pass 0x004D5DA0 took for a decal, which 0x004D6910 then clips it to
    static auto& g_decal_pass_list_a = addr_as_ref<VArray<GFace*>>(0x009BB6F0);
    // The distinct normals of those faces
    static auto& g_decal_pass_list_b = addr_as_ref<VArray<Vector3>>(0x009C2F00);
    // Least dot product of a face normal with the decal's forward vector for 0x004D5DA0 to take the face
    static auto& g_decal_orient_min_dot = addr_as_ref<float>(0x00589570);

    // Whether boxes (min1, max1) and (min2, max2) overlap, bounds included
    inline bool bbox_overlap(const Vector3* min1, const Vector3* max1, const Vector3* min2, const Vector3* max2)
    {
        return AddrCaller{0x00507990}.c_call<bool>(min1, max1, min2, max2);
    }

    // The engine's x87 dot product
    inline double vector_dot_prod(const Vector3* a, const Vector3* b)
    {
        return AddrCaller{0x0040A0B0}.this_call<double>(a, b);
    }

    // Clips `decal` to `face`; skip_if_present skips a face the decal already has a poly on
    inline void g_decal_clip_to_face(GDecal* decal, GFace* face, bool skip_if_present)
    {
        AddrCaller{0x004D6240}.c_call<void>(decal, face, skip_if_present);
    }

    // Whether `face` is near enough to `decal` for the orientation pass to take it
    inline bool g_decal_face_in_reach(GFace* face, GDecal* decal)
    {
        return AddrCaller{0x004D60D0}.c_call<bool>(face, decal);
    }

    inline void g_decal_pass_list_a_add(GFace* face)
    {
        AddrCaller{0x0045EC40}.this_call<void>(&g_decal_pass_list_a, face);
    }

    // Adds `normal` unless the list holds it already
    inline void g_decal_pass_list_b_add_unique(const Vector3& normal)
    {
        AddrCaller{0x004D7F70}.this_call<void>(&g_decal_pass_list_b, normal);
    }
    static auto& geomod_create_rock_debris = addr_as_ref<int(Vector3* orientation, float scaled_radius,
        Vector3* source_dir, int texture, int room_ptr)>(0x0048FE30);
    // Recycles every live glass shard back into the pool and re-resolves foley sounds/bitmaps
    // (cached lookups). Does not touch broken-pane state (that lives in room/face data);
    // safe to call mid-level.
    static auto& glass_shard_level_init = addr_as_ref<void()>(0x00490F60);
    // Recycles every live geomod debris rock back into the pool. Craters themselves are
    // geometry and are unaffected; safe to call mid-level.
    static auto& geomod_debris_level_init = addr_as_ref<void()>(0x0048F400);

    static auto& g_cache_clear = addr_as_ref<void()>(0x004F0B90);
    // The rooms g_cache_clear drops the caches of, and the cache arena's next free byte
    static auto& geo_cache_num_rooms = addr_as_ref<int>(0x013761B8);
    static auto& geo_cache_arena_pos = addr_as_ref<char*>(0x013F1DD4);
    // The detail rooms geo_cache_prepare_room 0x004F0C00 merged into the cache it builds
    static auto& geo_cache_num_detail_rooms = addr_as_ref<int>(0x01398CCC);
    // Batch scroll clock of the static solid renderer 0x0055F5E0, which advances it a frame per call
    static auto& gr_static_solid_scroll_clock = addr_as_ref<float>(0x01EA574C);
    // The rooms g_solid_collect_visible_rooms_recursive 0x004D4860 lists for the frame
    static auto& g_num_visible_rooms = addr_as_ref<int>(0x009BB57C);
    static auto& set_currently_rendered_room = addr_as_ref<void(GRoom* room)>(0x004D3350);
    static auto& gr_decal_self_illuminated_mode = addr_as_ref<gr::Mode>(0x01818340);
    static auto& g_get_room_render_list = addr_as_ref<void(GRoom ***rooms, int *num_rooms)>(0x004D3330);

    using GRoomRenderItemFn = void(*)(void* user, GSolid* solid);
    // Queues a render item for the current room pass. Returns false when the sphere (cull_pos, radius)
    // is frustum culled or the 2048 entry list is full; a null render_fn queues nothing.
    static auto& g_room_render_item_add = addr_as_ref<bool(void* user, const Vector3& pos, const Vector3& cull_pos,
        float radius, GRoomRenderItemFn render_fn, bool sortable, const Plane* group_plane,
        const Vector3* liquid_test_low, const Vector3* liquid_test_high, bool gather_lights,
        bool flag11)>(0x004D3560);

    static auto& find_room = addr_as_ref<GRoom*(GSolid* solid, const Vector3* pos)>(0x004E1630);

    static auto& g_solid_portal_render =
        addr_as_ref<void(GSolid* solid, GRoom* eye_room, int flags, const Matrix3* sky_rotation)>(0x004D45D0);

    // Sky room rendering globals (set by stock engine before sky room render call)
    static auto& sky_room_center = addr_as_ref<Vector3>(0x0088FB10);
    static auto& sky_room_offset = addr_as_ref<Vector3>(0x0087BB00);
    static auto& sky_room_orient = addr_as_ref<Matrix3*>(0x009BB56C);
    static auto& sky_room_rotation_axis = addr_as_ref<char>(0x0064601E);
    static auto& sky_room_rotation_rate = addr_as_ref<float>(0x00646020);
    static auto& sky_room_rotation_angle = addr_as_ref<float>(0x00646024);
    static auto& sky_room_rotation = addr_as_ref<Matrix3>(0x00646028);

    static auto& g_solid_load_v3d_embedded = addr_as_ref<GSolid*(const char*)>(0x00586E70);
    static auto& g_solid_load_v3d = addr_as_ref<GSolid*(const char*)>(0x00586F5C);
    static auto& decompress_vector3 = addr_as_ref<void(GSolid* solid, const ShortVector* in_vec, Vector3* out_vec)>(0x004B5900);
    static auto& compress_vector3 = addr_as_ref<int(GSolid* solid, Vector3* in_vec, ShortVector* out_vec)>(0x004B5820);

    static auto& material_find_impact_sound_set = addr_as_ref<ImpactSoundSet*(const char* name)>(0x004689A0);

    static auto& bbox_intersect = addr_as_ref<bool(const Vector3& bbox1_min, const Vector3& bbox1_max, const Vector3& bbox2_min, const Vector3& bbox2_max)>(0x0046C340);

    // Global temp buffer used by FUN_004d1330 (GSolid bbox computation after face extraction).
    static auto& g_geomod_bbox_temp = addr_as_ref<uint8_t[64]>(0x00647ce0);
}
