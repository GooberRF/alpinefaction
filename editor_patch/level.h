#pragma once

#include <cstdint>
#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include <patch_common/MemUtils.h>
#include "vtypes.h"
#include "mfc_types.h"
#include "resources.h"

constexpr std::size_t stock_cdedlevel_size = 0x608;
constexpr int alpine_props_chunk_id = 0x0AFBA5ED;

// Forward declarations
struct GFace;
struct GSolid;
struct GFaceVertex;
struct GBBox;
struct DecalPoly;

// Editor-side GRoom layout (matches stock RED.exe / RF.exe GRoom, 0x1CC bytes)
// Full game-side definition with ALPINE_FACTION extensions: game_patch/rf/geometry.h
// Serialization verified via solid_write (RED.exe 0x004a3fc0) and solid_read (RF.exe 0x004ED520)
struct GRoom
{
    bool is_detail;              // +0x00  serialized (via FUN_00426210)
    bool is_sky;                 // +0x01  serialized (via FUN_004261b0)
    bool is_invisible;           // +0x02  not serialized (editor/runtime only)
    char _pad_03;                // +0x03
    void* geo_cache;             // +0x04  runtime pointer
    Vector3 bbox_min;            // +0x08  serialized
    Vector3 bbox_max;            // +0x14  serialized
    int room_index;              // +0x20  serialized (used in portal/detail-room sections)
    int uid;                     // +0x24  serialized (first field written per room)
    char _face_list[8];          // +0x28  VList<GFace>: head ptr + count (faces serialized separately)
    VArray<void*> portals;       // +0x30  VArray<GPortal*> (serialized separately)
    void* bbox_ptr;              // +0x3C  runtime pointer (GBBox*)
    bool is_blocked;             // +0x40  not serialized in room loop
    bool is_cold;                // +0x41  serialized
    bool is_outside;             // +0x42  serialized
    bool is_airlock;             // +0x43  serialized
    bool is_pressurized;         // +0x44  not serialized
    bool ambient_light_defined;  // +0x45  serialized (gates ambient_light write)
    Color ambient_light;         // +0x46  serialized (only if ambient_light_defined)
    char eax_effect[32];         // +0x4A  serialized (strlen'd string, max 0x20 bytes)
    bool has_alpha;              // +0x6A  serialized
    char _pad_6b;                // +0x6B
    VArray<GRoom*> detail_rooms; // +0x6C  room indices written separately
    void* room_to_render_with;   // +0x78  runtime pointer (GRoom*)
    char _room_plane[16];        // +0x7C  Plane (4 floats, not serialized)
    int last_frame_rendered_normal; // +0x8C  runtime
    int last_frame_rendered_alpha;  // +0x90  runtime
    float life;                  // +0x94  serialized
    bool is_invincible;          // +0x98  set conditionally by reader
    char _pad_99[3];             // +0x99
    char _decals_and_runtime[0xE4]; // +0x9C  decals array (FArray<GDecal*, 48>) + runtime fields
    int liquid_type;             // +0x180 serialized (conditional on contains_liquid)
    bool contains_liquid;        // +0x184 serialized (gates liquid property block)
    char _pad_185[3];            // +0x185
    float liquid_depth;          // +0x188 serialized (conditional)
    Color liquid_color;          // +0x18C serialized (conditional)
    float liquid_visibility;     // +0x190 serialized (conditional)
    int liquid_surface_bitmap;   // +0x194 serialized (conditional; texture handle)
    int liquid_surface_proctex_id; // +0x198
    int liquid_ppm_u;            // +0x19C serialized (conditional)
    int liquid_ppm_v;            // +0x1A0 serialized (conditional)
    float liquid_angle;          // +0x1A4 serialized (conditional)
    int liquid_alpha;            // +0x1A8 serialized (conditional)
    bool liquid_plankton;        // +0x1AC serialized (conditional)
    char _pad_1ad[3];            // +0x1AD
    int liquid_waveform;         // +0x1B0 serialized (conditional)
    float liquid_surface_pan_u;  // +0x1B4 serialized (conditional)
    float liquid_surface_pan_v;  // +0x1B8 serialized (conditional)
    char _pad_1bc[0x10];         // +0x1BC  cached_lights VArray (12) + light_state (4)

    // Allocate uninitialized GRoom (0x1CC bytes); must call init() afterwards
    static GRoom* alloc()
    {
        return static_cast<GRoom*>(AddrCaller{0x0052ee74}.c_call<void*>(sizeof(GRoom)));
    }

    // FUN_004854e0: initialize all fields, assign UID, register in solid
    void init(GSolid* solid, const Vector3* bbox_min_ptr, const Vector3* bbox_max_ptr)
    {
        AddrCaller{0x004854e0}.this_call(this, solid, bbox_min_ptr, bbox_max_ptr);
    }

    // FUN_004857c0: move face from its current room into this room, update bbox
    void add_face(GFace* face)
    {
        AddrCaller{0x004857c0}.this_call(this, face);
    }

    // FUN_00486a10: set is_detail flag and register with solid
    void set_detail(GSolid* solid, int detail)
    {
        AddrCaller{0x00486a10}.this_call(this, solid, detail);
    }
};
static_assert(sizeof(GRoom) == 0x1CC);
static_assert(offsetof(GRoom, is_detail) == 0x00);
static_assert(offsetof(GRoom, bbox_min) == 0x08);
static_assert(offsetof(GRoom, bbox_max) == 0x14);
static_assert(offsetof(GRoom, room_index) == 0x20);
static_assert(offsetof(GRoom, uid) == 0x24);
static_assert(offsetof(GRoom, is_cold) == 0x41);
static_assert(offsetof(GRoom, ambient_light_defined) == 0x45);
static_assert(offsetof(GRoom, ambient_light) == 0x46);
static_assert(offsetof(GRoom, eax_effect) == 0x4A);
static_assert(offsetof(GRoom, has_alpha) == 0x6A);
static_assert(offsetof(GRoom, life) == 0x94);
static_assert(offsetof(GRoom, liquid_type) == 0x180);
static_assert(offsetof(GRoom, contains_liquid) == 0x184);

// Editor-side GFace layout (0x60 bytes, matches stock RED.exe / RF.exe GFace)
// Full game-side definition: game_patch/rf/geometry.h
// GFaceAttributes fields (game-side nested struct) are inlined here for direct access.
struct GFace
{
    Plane plane;                 // +0x00  face plane (normal + dist)
    Vector3 bounding_box_min;    // +0x10  face AABB min
    Vector3 bounding_box_max;    // +0x1C  face AABB max
    // GFaceAttributes (inlined):
    int flags;                   // +0x28  GFaceFlags bitfield
    int group_id;                // +0x2C  face grouping (union with GTextureMover* at runtime)
    int bitmap_id;               // +0x30  texture handle
    short portal_id;             // +0x34  portal index + 2, or 0
    short surface_index;         // +0x36  surface/lightmap index
    int face_id;                 // +0x38  unique face identifier (used for brush-to-room mapping)
    int smoothing_groups;        // +0x3C  smoothing group bitfield
    GFaceVertex* edge_loop;      // +0x40  linked list of per-face vertices
    GRoom* which_room;           // +0x44  owning room (assigned by room builder)
    GBBox* which_bbox;           // +0x48  owning bounding box node
    DecalPoly* decal_list;       // +0x4C  linked list of decal polygons on this face
    short unk_cache_index;       // +0x50
    char _pad_52[2];             // +0x52  alignment padding
    GFace* next_solid;           // +0x54  next[FACE_LIST_SOLID] (GSolid face linked list)
    GFace* next_bbox;            // +0x58  next[FACE_LIST_BBOX]
    GFace* next_room;            // +0x5C  next[FACE_LIST_ROOM]
};
static_assert(sizeof(GFace) == 0x60);
static_assert(offsetof(GFace, plane) == 0x00);
static_assert(offsetof(GFace, bounding_box_min) == 0x10);
static_assert(offsetof(GFace, bounding_box_max) == 0x1C);
static_assert(offsetof(GFace, flags) == 0x28);
static_assert(offsetof(GFace, bitmap_id) == 0x30);
static_assert(offsetof(GFace, face_id) == 0x38);
static_assert(offsetof(GFace, edge_loop) == 0x40);
static_assert(offsetof(GFace, which_room) == 0x44);
static_assert(offsetof(GFace, next_solid) == 0x54);

// Editor-side GSolid partial layout (matches stock RED.exe / RF.exe GSolid)
// Full game-side definition with ALPINE_FACTION extensions: game_patch/rf/geometry.h
struct GSolid
{
    void* bbox;                  // +0x00
    char name[64];               // +0x04
    int modifiability;           // +0x44
    Vector3 bbox_min;            // +0x48
    Vector3 bbox_max;            // +0x54
    float bounding_sphere_radius;// +0x60
    Vector3 bounding_sphere_center; // +0x64
    GFace* face_list_head;       // +0x70  VList<GFace, FACE_LIST_SOLID>: head pointer
    int face_list_count;         // +0x74  VList<GFace, FACE_LIST_SOLID>: element count
    VArray<void*> vertices;      // +0x78
    VArray<GRoom*> children;     // +0x84
    VArray<GRoom*> all_rooms;    // +0x90
};
static_assert(offsetof(GSolid, face_list_head) == 0x70);
static_assert(offsetof(GSolid, all_rooms) == 0x90);

// Brush state enum (BrushNode::state at +0x48)
// Determined via byte-pattern searches and cross-referencing comparison/assignment sites:
//   state==0: cmp [reg+48h],0  (set in deselect-all FUN_0042c740, toggle FUN_0042b810, constructor)
//   state==1: cmp [reg+48h],1  (checked in FUN_0042c020, FUN_0042adb0 - skipped during picking/selection;
//                                never explicitly assigned to BrushNodes in code, may be set via file load)
//   state==2: cmp [reg+48h],2  (checked in FUN_0042c020, FUN_0042adb0, FUN_0042e560 - skipped during
//                                picking/selection; assigned in hide-brush code at 0x00442073)
//   state==3: cmp [reg+48h],3  (ubiquitous - tested in ~40 functions for "is selected" checks)
enum BrushState : int
{
    BRUSH_STATE_NORMAL   = 0,  // default / unselected
    BRUSH_STATE_RED      = 1,  // non-selectable (red wireframe?); skipped by picking; never set in code
    BRUSH_STATE_HIDDEN   = 2,  // hidden; skipped by picking; set by hide-brush toggle at 0x00442073
    BRUSH_STATE_SELECTED = 3,  // selected (red highlight); tested everywhere
};

// Brush type enum (BrushNode::brush_type at +0x40)
// Maps 1:1 with the toolbar combobox entries ("Undefined" / "Air" / "Solid")
// initialized at 0x0043ea00 via CB_ADDSTRING.
// NOT bit flags — it's an index/enum value.
//
// Serialized in .rfl as a packed bitfield (FUN_0044d830 encode / FUN_0044d870 decode):
//   bit 0 = is_portal, bit 1 = is_air (brush_type==1), bit 2 = is_detail, bit 4 = is_scrolling
// CSG behavior:
//   Air (1): brush geometry defines void/carved space (subtractive — default for RED)
//   Solid (2): face normals inverted (via FUN_00490a10) before CSG, making it additive
enum BrushType : int
{
    BRUSH_TYPE_UNDEFINED = 0,  // "Undefined" — fallback when multi-selection has mixed types
    BRUSH_TYPE_AIR       = 1,  // "Air" — subtractive brush (default)
    BRUSH_TYPE_SOLID     = 2,  // "Solid" — additive brush (normals flipped for CSG)
};

// Brush linked-list node in the editor (CDedLevel + 0x118 points to head)
// Doubly-linked circular list; total size = 0x54 bytes (allocated at FUN_00412c27 via FUN_0052ee74(0x54))
//
// Constructor: FUN_0044d5a0 (0x0044d5a0) -- sets vtable, zeros pos, identity orient, nulls links
// Clone/copy:  FUN_0044d620 (0x0044d620) -- copies all fields from source, clones geometry, nulls links
// Destructor:  FUN_0044d8b0 -> FUN_0044d600 -- releases geometry if non-null
// Vtable:      0x005570ec
//
// Key accessors:
//   FUN_00483970 -- find brush by UID (iterates list comparing +0x04)
//   FUN_00484230 -- generate next unique UID (scans all brush UIDs to find max+1)
//   FUN_00412800 -- get next in face/sub-object list (returns *(param+0x54), NOT BrushNode::next)
struct BrushNode
{
    void* vtable;                // +0x00  vtable pointer (0x005570ec)
    int uid;                     // +0x04  unique brush identifier (init -1; assigned via FUN_00484230)
    Vector3 pos;                 // +0x08  brush position (3 floats: x +0x08, y +0x0C, z +0x10)
    Matrix3 orient;              // +0x14  brush orientation matrix (3x3 identity on init; 0x24 bytes)
    void* geometry;              // +0x38  pointer to geometry/CSG data (0x378-byte object; NULL on init)
    uint8_t is_portal;           // +0x3C  portal flag (byte; 0 on init)
    uint8_t is_detail;           // +0x3D  detail brush flag (byte; 0 on init; must be 1 for life value)
    uint8_t is_scrolling;        // +0x3E  scrolling flag (byte; 0 on init; propagated to faces)
    char _pad_3f;                // +0x3F  padding
    BrushType brush_type;        // +0x40  brush type (init AIR=1; combobox index in toolbar)
    int life;                    // +0x44  life/destroyable time (init -1; only valid when is_detail==1)
    BrushState state;            // +0x48  brush state (0=normal, 2=hidden, 3=selected)
    BrushNode* next;             // +0x4C  next node in circular doubly-linked list
    BrushNode* prev;             // +0x50  prev node in circular doubly-linked list
};
static_assert(sizeof(BrushNode) == 0x54);
static_assert(offsetof(BrushNode, vtable) == 0x00);
static_assert(offsetof(BrushNode, uid) == 0x04);
static_assert(offsetof(BrushNode, pos) == 0x08);
static_assert(offsetof(BrushNode, orient) == 0x14);
static_assert(offsetof(BrushNode, geometry) == 0x38);
static_assert(offsetof(BrushNode, is_portal) == 0x3C);
static_assert(offsetof(BrushNode, is_detail) == 0x3D);
static_assert(offsetof(BrushNode, is_scrolling) == 0x3E);
static_assert(offsetof(BrushNode, brush_type) == 0x40);
static_assert(offsetof(BrushNode, life) == 0x44);
static_assert(offsetof(BrushNode, state) == 0x48);
static_assert(offsetof(BrushNode, next) == 0x4C);
static_assert(offsetof(BrushNode, prev) == 0x50);

// should match structure in game_patch\misc\level.h
struct AlpineLevelProperties
{
    // defaults for new levels
    // v1
    bool legacy_cyclic_timers = false;
    // v2
    bool legacy_movers = false;
    bool starts_with_headlamp = true;
    // v3
    bool override_static_mesh_ambient_light_modifier = false;
    float static_mesh_ambient_light_modifier = 2.0f;
    // v4
    bool rf2_style_geomod = false;
    std::vector<int32_t> geoable_brush_uids;
    std::vector<int32_t> geoable_room_uids; // computed at save time, parallel to geoable_brush_uids
    std::vector<int32_t> breakable_brush_uids;
    std::vector<int32_t> breakable_room_uids; // computed at save time, parallel to breakable_brush_uids
    std::vector<uint8_t> breakable_materials;  // material type per entry

    static constexpr std::uint32_t current_alpine_chunk_version = 4u;

    // defaults for existing levels, overwritten for maps with these fields in their alpine level props chunk
    // relevant for maps without alpine level props and maps with older alpine level props versions
    // should always match stock game behaviour
    void LoadDefaults()
    {
        legacy_cyclic_timers = true;
        legacy_movers = true;
        starts_with_headlamp = true;
        override_static_mesh_ambient_light_modifier = false;
        static_mesh_ambient_light_modifier = 2.0f;
        rf2_style_geomod = false;
        geoable_brush_uids.clear();
        geoable_room_uids.clear();
        breakable_brush_uids.clear();
        breakable_room_uids.clear();
        breakable_materials.clear();
    }

    void Serialize(rf::File& file) const
    {
        file.write<std::uint32_t>(current_alpine_chunk_version);

        // v1
        file.write<std::uint8_t>(legacy_cyclic_timers ? 1u : 0u);
        // v2
        file.write<std::uint8_t>(legacy_movers ? 1u : 0u);
        file.write<std::uint8_t>(starts_with_headlamp ? 1u : 0u);
        // v3
        file.write<std::uint8_t>(override_static_mesh_ambient_light_modifier ? 1u : 0u);
        file.write<float>(static_mesh_ambient_light_modifier);
        // v4
        file.write<std::uint8_t>(rf2_style_geomod ? 1u : 0u);
        // Write geoable entries as (brush_uid, room_uid) pairs
        std::uint32_t count = static_cast<std::uint32_t>(geoable_brush_uids.size());
        file.write<std::uint32_t>(count);
        for (std::uint32_t i = 0; i < count; i++) {
            file.write<int32_t>(geoable_brush_uids[i]);
            int32_t room_uid = (i < geoable_room_uids.size()) ? geoable_room_uids[i] : 0;
            file.write<int32_t>(room_uid);
        }
        // Write breakable material entries as (brush_uid, room_uid, material) triples
        std::uint32_t bcount = static_cast<std::uint32_t>(breakable_brush_uids.size());
        file.write<std::uint32_t>(bcount);
        for (std::uint32_t i = 0; i < bcount; i++) {
            file.write<int32_t>(breakable_brush_uids[i]);
            int32_t room_uid = (i < breakable_room_uids.size()) ? breakable_room_uids[i] : 0;
            file.write<int32_t>(room_uid);
            uint8_t mat = (i < breakable_materials.size()) ? breakable_materials[i] : 0;
            file.write<uint8_t>(mat);
        }
    }

    void Deserialize(rf::File& file, std::size_t chunk_len)
    {
        std::size_t remaining = chunk_len;

        // scope-exit: always skip any unread tail (forward compatibility for unknown newer fields)
        struct Tail
        {
            rf::File& f;
            std::size_t& rem;
            bool active = true;
            ~Tail()
            {
                if (active && rem) {
                    f.seek(static_cast<int>(rem), rf::File::seek_cur);
                }
            }
            void dismiss()
            {
                active = false;
            }
        } tail{file, remaining};

        auto read_bytes = [&](void* dst, std::size_t n) -> bool {
            if (remaining < n)
                return false;
            int got = file.read(dst, n);
            if (got != static_cast<int>(n) || file.error())
                return false;
            remaining -= n;
            return true;
        };

        // version
        std::uint32_t version = 0;
        if (!read_bytes(&version, sizeof(version))) {
            xlog::warn("[AlpineLevelProps] chunk too small for version header (len={})", chunk_len);
            return;
        }
        if (version < 1) {
            xlog::warn("[AlpineLevelProps] unexpected version {} (chunk_len={})", version, chunk_len);
            return;
        }
        xlog::debug("[AlpineLevelProps] version {}", version);

        if (version >= 1) {
            std::uint8_t u8 = 0;
            if (!read_bytes(&u8, sizeof(u8)))
                return;
            legacy_cyclic_timers = (u8 != 0);
            xlog::debug("[AlpineLevelProps] legacy_cyclic_timers {}", legacy_cyclic_timers);
        }

        if (version >= 2) {
            std::uint8_t u8 = 0;
            if (!read_bytes(&u8, sizeof(u8)))
                return;
            legacy_movers = (u8 != 0);
            xlog::debug("[AlpineLevelProps] legacy_movers {}", legacy_movers);
            if (!read_bytes(&u8, sizeof(u8)))
                return;
            starts_with_headlamp = (u8 != 0);
            xlog::debug("[AlpineLevelProps] starts_with_headlamp {}", starts_with_headlamp);
        }

        if (version >= 3) {
            std::uint8_t u8 = 0;
            if (!read_bytes(&u8, sizeof(u8)))
                return;
            override_static_mesh_ambient_light_modifier = (u8 != 0);
            xlog::debug("[AlpineLevelProps] override_static_mesh_ambient_light_modifier {}", override_static_mesh_ambient_light_modifier);
            if (!read_bytes(&static_mesh_ambient_light_modifier, sizeof(static_mesh_ambient_light_modifier)))
                return;
            xlog::debug("[AlpineLevelProps] static_mesh_ambient_light_modifier {}", static_mesh_ambient_light_modifier);
        }

        if (version >= 4) {
            std::uint8_t u8 = 0;
            if (!read_bytes(&u8, sizeof(u8)))
                return;
            rf2_style_geomod = (u8 != 0);
            xlog::debug("[AlpineLevelProps] rf2_style_geomod {}", rf2_style_geomod);

            // Geoable entries as (brush_uid, room_uid) pairs
            std::uint32_t count = 0;
            if (!read_bytes(&count, sizeof(count)))
                return;
            if (count > 10000) count = 10000;
            geoable_brush_uids.resize(count);
            geoable_room_uids.resize(count);
            for (std::uint32_t i = 0; i < count; i++) {
                int32_t brush_uid = 0;
                if (!read_bytes(&brush_uid, sizeof(brush_uid)))
                    return;
                geoable_brush_uids[i] = brush_uid;
                int32_t room_uid = 0;
                if (!read_bytes(&room_uid, sizeof(room_uid)))
                    return;
                geoable_room_uids[i] = room_uid;
            }
            xlog::debug("[AlpineLevelProps] geoable entries count={}", count);

            // Breakable material entries as (brush_uid, room_uid, material) triples
            std::uint32_t bcount = 0;
            if (!read_bytes(&bcount, sizeof(bcount)))
                return;
            if (bcount > 10000) bcount = 10000;
            breakable_brush_uids.resize(bcount);
            breakable_room_uids.resize(bcount);
            breakable_materials.resize(bcount);
            for (std::uint32_t i = 0; i < bcount; i++) {
                int32_t brush_uid = 0;
                if (!read_bytes(&brush_uid, sizeof(brush_uid)))
                    return;
                breakable_brush_uids[i] = brush_uid;
                int32_t room_uid = 0;
                if (!read_bytes(&room_uid, sizeof(room_uid)))
                    return;
                breakable_room_uids[i] = room_uid;
                uint8_t mat = 0;
                if (!read_bytes(&mat, sizeof(mat)))
                    return;
                breakable_materials[i] = mat;
            }
        }
    }
};

enum class DedRoomEffectType : int
{
    Liquid = 2,
};

struct CDedLevel
{
    // --- vtable + string properties ---
    void* vtable;                                 // +0x00
    VString unk_str_04;                           // +0x04 (VString, 8 bytes)
    VString unk_str_0C;                           // +0x0C (VString, 8 bytes)
    VString unk_str_14;                           // +0x14 (VString, 8 bytes)
    VString unk_str_1C;                           // +0x1C (VString, 8 bytes)
    VString geomod_texture;                       // +0x24 (crater texture filename)
    char _pad_2C[0x30 - 0x2C];                   // +0x2C
    int unk_30;                                   // +0x30 (small object, FUN_004b9380)
    int unk_34;                                   // +0x34
    int unk_38;                                   // +0x38
    char _pad_3C[0x44 - 0x3C];                   // +0x3C
    int unk_44;                                   // +0x44 (init 0)
    char unk_48;                                  // +0x48 (init 0)
    char _pad_49[0x4C - 0x49];                   // +0x49

    // --- compiled geometry ---
    GSolid* solid;                                // +0x4C (compiled geometry, 0x378-byte object)
    char _pad_50[0x54 - 0x50];                   // +0x50
    int uid_counters[8];                          // +0x54 (all init -1, 32 bytes)
    char unk_74;                                  // +0x74 (init 0)
    char _pad_75[0x78 - 0x75];                   // +0x75
    float default_angles[32];                     // +0x78 (all init 89.9f, 128 bytes to +0xF8)
    int unk_F8;                                   // +0xF8 (init 0)
    int unk_FC;                                   // +0xFC (init 3)
    int unk_100;                                  // +0x100 (init 0)
    int unk_104;                                  // +0x104 (init 0)
    float unk_108;                                // +0x108 (init 1.0f)
    float unk_10C;                                // +0x10C (init 0.2625f)
    int unk_110;                                  // +0x110 (init 16)
    char unk_114;                                 // +0x114 (init 0)
    char unk_115;                                 // +0x115 (init 0)
    char _pad_116[0x118 - 0x116];                // +0x116

    // --- brush linked list ---
    BrushNode* brush_list;                        // +0x118 (head of brush linked list)

    // --- editor strings + containers ---
    char _pad_11C[0x1AC - 0x11C];                // +0x11C (3 CString+container pairs at +0x11C/+0x128, +0x14C/+0x158, +0x17C/+0x188)
    void* unk_obj_1AC;                            // +0x1AC (0x14-byte allocated object)
    char _pad_1B0[0x1C0 - 0x1B0];               // +0x1B0 (CString + int)
    int unk_1C0;                                  // +0x1C0 (init 0, file filter related)
    char _pad_1C4[0x1D0 - 0x1C4];               // +0x1C4 (VArray, 12 bytes + padding)

    // --- icon texture handles ---
    int icon_sp_start;                            // +0x1D0 (Icon_SinglePlayerStart.tga)
    int icon_mp_start;                            // +0x1D4 (Icon_MultiPlayerStart.tga)
    int icon_ambient;                             // +0x1D8 (Icon_Ambient.tga)
    int icon_trigger;                             // +0x1DC (Icon_Trigger.tga)
    int icon_event_c;                             // +0x1E0 (Icon_Event_C.tga)
    int icon_event_e;                             // +0x1E4 (Icon_Event_E.tga)
    int icon_event_l;                             // +0x1E8 (Icon_Event_L.tga)
    int icon_light;                               // +0x1EC (Icon_Light.tga)
    int icon_light_editor;                        // +0x1F0 (Icon_Light_Editor_only.tga)
    int icon_geo_region;                          // +0x1F4 (Icon_GeoRegion.tga)
    int icon_emitter;                             // +0x1F8 (Icon_ParticleEmitter.tga)
    int icon_gas_region;                          // +0x1FC (Icon_GasRegion.tga)
    int icon_decal;                               // +0x200 (Icon_Decal.tga)
    int icon_room_fx;                             // +0x204 (Icon_RoomFX.tga)
    int icon_eax;                                 // +0x208 (Icon_EAX.tga)
    int icon_climb_region;                        // +0x20C (Icon_ClimbRegion.tga)
    int icon_waypoint;                            // +0x210 (Icon_Waypoint.tga)
    int icon_cutscene_path;                       // +0x214 (Icon_CutscenePathNode.tga)
    int icon_bolt_emitter;                        // +0x218 (Icon_BoltEmitter.tga)
    int icon_target;                              // +0x21C (Icon_Target.tga)
    int icon_keyframe_gold;                       // +0x220 (Icon_Keyframe_Gold.tga)
    int icon_keyframe_silver;                     // +0x224 (Icon_Keyframe_Silver.tga)
    int icon_camera;                              // +0x228 (Icon_CameraPosition.tga)
    int icon_push_region;                         // +0x22C (Icon_ClimbRegion.tga second)
    char _pad_230[0x298 - 0x230];                // +0x230 (editor state: bytes, CStrings, containers, VArrays)

    // --- selection ---
    VArray<DedObject*> selection;                 // +0x298
    char _pad_2A4[0x340 - 0x2A4];                // +0x2A4 (13 VArrays + 1 container, internal editor state)

    // --- object VArrays (21 contiguous, 12 bytes each) ---
    VArray<DedObject*> items;                     // +0x340
    VArray<DedObject*> entities;                  // +0x34C
    VArray<DedObject*> respawn_points;            // +0x358
    VArray<DedObject*> triggers;                  // +0x364
    VArray<DedObject*> events;                    // +0x370
    VArray<DedObject*> ambient_sounds;            // +0x37C
    VArray<DedObject*> clutter;                   // +0x388
    VArray<DedObject*> lights;                    // +0x394
    VArray<DedObject*> geo_regions;               // +0x3A0
    VArray<DedObject*> nav_points;                // +0x3AC
    VArray<DedObject*> cutscene_cameras;          // +0x3B8
    VArray<DedObject*> cutscene_path_nodes;       // +0x3C4
    VArray<DedObject*> emitters;                  // +0x3D0
    VArray<DedObject*> gas_regions;               // +0x3DC
    VArray<DedObject*> room_effects;              // +0x3E8
    VArray<DedObject*> eax_effects;               // +0x3F4
    VArray<DedObject*> climb_regions;             // +0x400
    VArray<DedObject*> bolt_emitters;             // +0x40C
    VArray<DedObject*> targets;                   // +0x418
    VArray<DedObject*> decals;                    // +0x424
    VArray<DedObject*> push_regions;              // +0x430
    char _pad_43C[0x444 - 0x43C];                // +0x43C

    // --- editor dialog panel pointers (28 MFC dialog objects) ---
    void* dialog_panels[28];                      // +0x444 (28 pointers, ends at +0x4B4)

    // --- moving groups (Keyframes) ---
    VArray<DedObject*> moving_groups;             // +0x4B4 (each element has nested VArray at +0x1C)
    char _pad_4C0[0x608 - 0x4C0];                // +0x4C0 (VArrays, containers, strings to end)

    std::size_t BeginRflSection(rf::File& file, int chunk_id)
    {
        return AddrCaller{0x00430B60}.this_call<std::size_t>(this, &file, chunk_id);
    }

    void EndRflSection(rf::File& file, std::size_t start_pos)
    {
        return AddrCaller{0x00430B90}.this_call(this, &file, start_pos);
    }

    AlpineLevelProperties& GetAlpineLevelProperties()
    {
        return struct_field_ref<AlpineLevelProperties>(this, stock_cdedlevel_size);
    }

    static CDedLevel* Get()
    {
        return AddrCaller{0x004835F0}.c_call<CDedLevel*>();
    }
};
static_assert(sizeof(CDedLevel) == 0x608);

// GRoom UID counter (RED.exe global, starts at 0x7FFFFFFF, decrements on each GRoom construction)
// Final compiled rooms in all_rooms are clones that skip the constructor and get uid=-1;
// they must be assigned from this counter manually before serialization.
static auto& g_groom_uid_counter = addr_as_ref<int>(0x0057C954);

void DedLevel_DoBackLink();
