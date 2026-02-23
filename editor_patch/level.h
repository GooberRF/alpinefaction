#pragma once

#include <cstdint>
#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include "vtypes.h"
#include "mfc_types.h"
#include "resources.h"

constexpr std::size_t stock_cdedlevel_size = 0x608;
constexpr int alpine_props_chunk_id = 0x0AFBA5ED;

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
    char _face_list[8];          // +0x70 (VList<GFace>: head ptr + count)
    VArray<void*> vertices;      // +0x78
    VArray<GRoom*> children;     // +0x84
    VArray<GRoom*> all_rooms;    // +0x90
};
static_assert(offsetof(GSolid, all_rooms) == 0x90);

// Brush state enum (BrushNode::state at +0x48)
// Determined via byte-pattern searches and cross-referencing comparison/assignment sites:
//   state==0: cmp [reg+48h],0  (set in deselect-all FUN_0042c740, toggle FUN_0042b810, constructor)
//   state==1: cmp [reg+48h],1  (checked in FUN_0042c020, FUN_0042adb0 - skipped during picking/selection;
//                                never explicitly assigned to BrushNodes in code, may be set via file load)
//   state==2: cmp [reg+48h],2  (checked in FUN_0042c020, FUN_0042adb0, FUN_0042e560 - skipped during
//                                picking/selection; assigned in hide-brush code at 0x00442073)
//   state==3: cmp [reg+48h],3  (ubiquitous - tested in ~40 functions for "is selected" checks)
//   state==4: no matches found (searched C7 ?? 48 04 00 00 00 and 83 ?? 48 04)
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
        }
    }
};

struct CDedLevel
{
    char _pad_00[0x4c];                          // +0x00
    GSolid* solid;                                // +0x4c (compiled geometry)
    char _pad_50[0x118 - 0x50];                  // +0x50
    BrushNode* brush_list;                        // +0x118 (head of brush linked list)
    char _pad_11c[0x298 - 0x11c];                // +0x11c
    VArray<DedObject*> selection;                  // +0x298
    char _pad_2a4[0x608 - 0x2a4];                // +0x2a4

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

void DedLevel_DoBackLink();
