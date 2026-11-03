#pragma once

// RED's renderer and bitmap manager types under the game's rf::gr / rf::bm names and members (game_patch/rf/gr,
// rf/bmpman.h), for the shared Direct3D 11 core.

#include <cstddef>
#include <cstdint>
#include "rf_math.h"

namespace rf::gr
{
    struct BaseVertex
    {
        Vector3 world_pos; // world or clip space
        float sx; // screen space
        float sy;
        float sw;
        ubyte codes;
        ubyte flags;
    };

    struct Vertex : BaseVertex
    {
        float u1;
        float v1;
        float u2;
        float v2;
        ubyte r;
        ubyte g;
        ubyte b;
        ubyte a;
    };
    static_assert(sizeof(Vertex) == 0x30);
    static_assert(offsetof(Vertex, sx) == 0x0C);
    static_assert(offsetof(Vertex, u1) == 0x1C);
    static_assert(offsetof(Vertex, r) == 0x2C);

    enum TextureSource {
        TEXTURE_SOURCE_NONE = 0x0,
        TEXTURE_SOURCE_WRAP = 0x1,
        TEXTURE_SOURCE_CLAMP = 0x2,
        TEXTURE_SOURCE_CLAMP_NO_FILTERING = 0x3,
        TEXTURE_SOURCE_CLAMP_1_WRAP_0 = 0x4,
        TEXTURE_SOURCE_CLAMP_1_WRAP_0_MOD2X = 0x5,
        TEXTURE_SOURCE_CLAMP_1_CLAMP_0 = 0x6,
        TEXTURE_SOURCE_BUMPENV_MAP = 0x7,
        TEXTURE_SOURCE_MT_U_WRAP_V_CLAMP = 0x8,
        TEXTURE_SOURCE_MT_U_CLAMP_V_WRAP = 0x9,
        TEXTURE_SOURCE_MT_WRAP_TRILIN = 0xA,
        TEXTURE_SOURCE_MT_CLAMP_TRILIN = 0xB,
    };

    enum ColorSource {
        COLOR_SOURCE_VERTEX = 0x0,
        COLOR_SOURCE_TEXTURE = 0x1,
        COLOR_SOURCE_VERTEX_TIMES_TEXTURE = 0x2,
        COLOR_SOURCE_VERTEX_PLUS_TEXTURE = 0x3,
        COLOR_SOURCE_VERTEX_TIMES_TEXTURE_2X = 0x4,
    };

    enum AlphaSource {
        ALPHA_SOURCE_VERTEX = 0x0,
        ALPHA_SOURCE_VERTEX_NONDARKENING = 0x1,
        ALPHA_SOURCE_TEXTURE = 0x2,
        ALPHA_SOURCE_VERTEX_TIMES_TEXTURE = 0x3,
    };

    enum AlphaBlend {
        ALPHA_BLEND_NONE = 0x0,
        ALPHA_BLEND_ADDITIVE = 0x1,
        ALPHA_BLEND_ALPHA_ADDITIVE = 0x2,
        ALPHA_BLEND_ALPHA = 0x3,
        ALPHA_BLEND_SRC_COLOR = 0x4,
        ALPHA_BLEND_LIGHTMAP = 0x5,
        ALPHA_BLEND_SUBTRACTIVE = 0x6,
        ALPHA_BLEND_SWAPPED_SRC_DEST_COLOR = 0x7,
    };

    enum ZbufferType {
        ZBUFFER_TYPE_NONE = 0x0,
        ZBUFFER_TYPE_READ = 0x1,
        ZBUFFER_TYPE_READ_EQUAL = 0x2,
        ZBUFFER_TYPE_WRITE = 0x3,
        ZBUFFER_TYPE_FULL = 0x4,
        ZBUFFER_TYPE_FULL_ALPHA_TEST = 0x5,
    };

    enum FogType {
        FOG_ALLOWED = 0x0,
        FOG_ALLOWED_ADD2 = 0x1,
        FOG_ALLOWED_MULT2 = 0x2,
        FOG_NOT_ALLOWED = 0x3,
    };

    class Mode
    {
        static constexpr int mask = 0x1F;
        static constexpr int ts_shift = 0;
        static constexpr int cs_shift = 5;
        static constexpr int as_shift = 10;
        static constexpr int ab_shift = 15;
        static constexpr int zbt_shift = 20;
        static constexpr int ft_shift = 25;

        int value = 0;

    public:
        constexpr Mode() = default;
        constexpr Mode(TextureSource ts, ColorSource cs, AlphaSource as, AlphaBlend ab, ZbufferType zbt, FogType ft) :
            value((ts << ts_shift) | (cs << cs_shift) | (as << as_shift) | (ab << ab_shift) | (zbt << zbt_shift) | (ft << ft_shift))
        {}

        [[nodiscard]] bool operator==(const Mode& other) const = default;

        operator int() const
        {
            return value;
        }

        [[nodiscard]] inline TextureSource get_texture_source() const
        {
            return static_cast<TextureSource>((value >> ts_shift) & mask);
        }

        [[nodiscard]] inline ColorSource get_color_source() const
        {
            return static_cast<ColorSource>((value >> cs_shift) & mask);
        }

        [[nodiscard]] inline AlphaSource get_alpha_source() const
        {
            return static_cast<AlphaSource>((value >> as_shift) & mask);
        }

        [[nodiscard]] inline AlphaBlend get_alpha_blend() const
        {
            return static_cast<AlphaBlend>((value >> ab_shift) & mask);
        }

        [[nodiscard]] inline ZbufferType get_zbuffer_type() const
        {
            return static_cast<ZbufferType>((value >> zbt_shift) & mask);
        }

        [[nodiscard]] inline FogType get_fog_type() const
        {
            return static_cast<FogType>((value >> ft_shift) & mask);
        }

        inline void set_texture_source(TextureSource ts)
        {
            value = (value & ~(mask << ts_shift)) | (static_cast<int>(ts) << ts_shift);
        }

        inline void set_color_source(ColorSource cs)
        {
            value = (value & ~(mask << cs_shift)) | (static_cast<int>(cs) << cs_shift);
        }

        inline void set_alpha_source(AlphaSource as)
        {
            value = (value & ~(mask << as_shift)) | (static_cast<int>(as) << as_shift);
        }

        inline void set_alpha_blend(AlphaBlend ab)
        {
            value = (value & ~(mask << ab_shift)) | (static_cast<int>(ab) << ab_shift);
        }

        inline void set_zbuffer_type(ZbufferType zbt)
        {
            value = (value & ~(mask << zbt_shift)) | (static_cast<int>(zbt) << zbt_shift);
        }

        inline void set_fog_type(FogType ft)
        {
            value = (value & ~(mask << ft_shift)) | (static_cast<int>(ft) << ft_shift);
        }
    };
    static_assert(sizeof(Mode) == 4);

    enum Depthbuffer
    {
        DEPTHBUFFER_Z = 0x0,
        DEPTHBUFFER_W = 0x1,
        DEPTHBUFFER_NONE = 0x2,
    };

    struct Screen
    {
        int signature;
        int max_w;
        int max_h;
        int mode;
        int window_mode;
        bool dual_monitors;
        float aspect;
        int rowsize;
        int bits_per_pixel;
        int bytes_per_pixel;
        float far_zvalue;
        int offset_x;
        int offset_y;
        int clip_width;
        int clip_height;
        int max_bitmap_w;
        int max_bitmap_h;
        int clip_left;
        int clip_right;
        int clip_top;
        int clip_bottom;
        Color current_color;
        int current_texture_1;
        int current_texture_2;
        bool fog_mode;
        Color fog_color;
        float fog_near;
        float fog_far;
        float fog_far_scaled;
        bool force_tint;
        float tint_r;
        float tint_g;
        float tint_b;
        bool sync_to_retrace;
        int zbias;
        Depthbuffer depthbuffer_type;
    };
    static_assert(sizeof(Screen) == 0x90);
    static_assert(offsetof(Screen, offset_x) == 0x2C);
    static_assert(offsetof(Screen, clip_width) == 0x34);
    static_assert(offsetof(Screen, clip_left) == 0x44);
    static_assert(offsetof(Screen, clip_top) == 0x4C);
    static_assert(offsetof(Screen, current_color) == 0x54);
    static_assert(offsetof(Screen, current_texture_1) == 0x58);
    static_assert(offsetof(Screen, fog_mode) == 0x60);
    static_assert(offsetof(Screen, fog_color) == 0x61);
    static_assert(offsetof(Screen, fog_far) == 0x6C);
    static_assert(offsetof(Screen, depthbuffer_type) == 0x8C);

    enum LockMode
    {
        LOCK_READ_ONLY = 0,
        LOCK_READ_ONLY_WRITE = 1,
        LOCK_WRITE_ONLY = 2,
    };

    enum TMapperFlags
    {
        TMAP_FLAG_TEXTURED = 1,
        TMAP_FLAG_TEXTURED_2 = 2,
        TMAP_FLAG_RGB = 4,
        TMAP_FLAG_ALPHA = 8,
    };

    enum VertexFlags
    {
        VF_PROJECTED = 1,
        // gr_d3d_project_vertex (0x004EE300) found it at or behind the eye while clipping on the CPU
        VF_BEHIND_EYE = 2,
    };

    enum LightType
    {
        LT_NONE = 0x0,
        LT_DIRECTIONAL = 0x1,
        LT_POINT = 0x2,
        LT_SPOT = 0x3,
        LT_TUBE = 0x4,
    };

    enum LightShadowcastCondition
    {
        SHADOWCAST_NEVER = 0x0,
        SHADOWCAST_EDITOR = 0x1,
        SHADOWCAST_RUNTIME = 0x2,
    };

    // A scene light pool entry (lightmap.cpp relocates RED's pool)
    struct Light
    {
        struct Light *next;
        struct Light *prev;
        LightType type;
        Vector3 vec;
        Vector3 vec2;
        Vector3 spotlight_dir;
        float spotlight_fov1;
        float spotlight_fov2;
        float spotlight_atten;
        float rad_2;
        float r;
        float g;
        float b;
        bool on;
        bool dynamic;
        bool use_squared_fov_falloff;
        char padding[1];
        LightShadowcastCondition shadow_condition;
        int attenuation_algorithm;
        int field_58;
        Vector3 local_vec;
        Vector3 local_vec2;
        Vector3 rotated_spotlight_dir;
        float rad2_squared;
        float spotlight_fov1_dot;
        float spotlight_fov2_dot;
        float spotlight_max_radius;
        int room;
        Plane spotlight_frustrum[6];
        int spotlight_nverts[6];
    };
    static_assert(sizeof(Light) == 0x10C);
    static_assert(offsetof(Light, type) == 0x08);
    static_assert(offsetof(Light, vec) == 0x0C);
    static_assert(offsetof(Light, vec2) == 0x18);
    static_assert(offsetof(Light, spotlight_dir) == 0x24);
    static_assert(offsetof(Light, spotlight_atten) == 0x38);
    static_assert(offsetof(Light, rad_2) == 0x3C);
    static_assert(offsetof(Light, r) == 0x40);
    static_assert(offsetof(Light, use_squared_fov_falloff) == 0x4E);
    static_assert(offsetof(Light, shadow_condition) == 0x50);
    static_assert(offsetof(Light, attenuation_algorithm) == 0x54);
    static_assert(offsetof(Light, local_vec) == 0x5C);
    static_assert(offsetof(Light, local_vec2) == 0x68);
    static_assert(offsetof(Light, spotlight_fov1_dot) == 0x84);
    static_assert(offsetof(Light, spotlight_fov2_dot) == 0x88);
}

namespace rf::bm
{
    // RED's bitmaps only carry the stock formats (DXT is decompressed at load); the Alpine values are the game's.
    enum Format
    {
        FORMAT_NONE = 0x0,
        FORMAT_8_PALETTED = 0x1,
        FORMAT_8_ALPHA = 0x2,
        FORMAT_565_RGB = 0x3,
        FORMAT_4444_ARGB = 0x4,
        FORMAT_1555_ARGB = 0x5,
        FORMAT_888_RGB = 0x6,
        FORMAT_8888_ARGB = 0x7,
        FORMAT_88_BUMPDUDV = 0x8,
        FORMAT_888_BGR = 0x9,
        FORMAT_RENDER_TARGET = 0x10,
        FORMAT_DXT1 = 0x11,
        FORMAT_DXT2 = 0x12,
        FORMAT_DXT3 = 0x13,
        FORMAT_DXT4 = 0x14,
        FORMAT_DXT5 = 0x15,
    };

    enum Type
    {
        TYPE_NONE = 0x0,
        TYPE_PCX = 0x1,
        TYPE_TGA = 0x2,
        TYPE_USER = 0x3,
        TYPE_VAF = 0x4,
        TYPE_VBM = 0x5,
        TYPE_M2V = 0x6,
        TYPE_DDS = 0x10,
        TYPE_ATX = 0x11,
        TYPE_STB = 0x12,
    };

    struct BitmapEntry
    {
        char name[32];
        int name_checksum;
        int handle;
        ushort orig_width;
        ushort orig_height;
        ushort width;
        ushort height;
        int num_pixels_in_all_levels;
        Type bm_type;
        int animated_entry_type;
        Format format;
        ubyte num_levels;
        ubyte orig_num_levels;
        ubyte num_levels_in_ext_files;
        ubyte num_frames;
        ubyte vbm_version;
        void* locked_data;
        float frames_per_ms;
        void* locked_palette;
        BitmapEntry* next;
        BitmapEntry* prev;
        char field_5C;
        ubyte cached_material_idx;
        // The game's Alpine flags; always clear in RED
        bool dynamic;
        bool user_mipmap;
        int total_bytes_for_all_levels;
        int file_open_unk_arg;
        int resolution_level;
    };
    static_assert(sizeof(BitmapEntry) == 0x6C);
    static_assert(offsetof(BitmapEntry, bm_type) == 0x34);
    static_assert(offsetof(BitmapEntry, format) == 0x3C);
    static_assert(offsetof(BitmapEntry, num_frames) == 0x43);
    static_assert(offsetof(BitmapEntry, dynamic) == 0x5E);
}

namespace rf::gr
{
    struct LockInfo
    {
        int bm_handle;
        int section;
        bm::Format format;
        uint8_t *data;
        int w;
        int h;
        int stride_in_bytes;
        LockMode mode;
    };
    static_assert(sizeof(LockInfo) == 0x20);
}
