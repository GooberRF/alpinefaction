#pragma once

#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <unordered_map>

// ─── Generic .tbl tokenizer ─────────────────────────────────────────────────

// Tokenizer for the .tbl text format used by the Red Faction engine.
// Handles $Field: values, #Section markers, "quoted strings", numbers, and // comments.
struct TblTokenizer {
    const char* pos;
    const char* end;

    TblTokenizer() : pos(nullptr), end(nullptr) {}
    TblTokenizer(const char* data, size_t len) : pos(data), end(data + len) {}

    void skip_ws()
    {
        while (pos < end) {
            if (*pos == '/' && pos + 1 < end && pos[1] == '/') {
                while (pos < end && *pos != '\n') ++pos;
            }
            else if (*pos <= ' ') {
                ++pos;
            }
            else {
                break;
            }
        }
    }

    [[nodiscard]] bool at_end() const { return pos >= end; }

    // Peek at the next token without consuming it (case-insensitive prefix match)
    bool peek(const char* str)
    {
        skip_ws();
        size_t len = strlen(str);
        return pos + len <= end && _strnicmp(pos, str, len) == 0;
    }

    // Try to consume a specific token (case-insensitive prefix match)
    bool match(const char* str)
    {
        skip_ws();
        size_t len = strlen(str);
        if (pos + len <= end && _strnicmp(pos, str, len) == 0) {
            pos += len;
            return true;
        }
        return false;
    }

    // Read a quoted string, returns content without quotes.
    // Falls back to reading a bare word (whitespace-delimited) if no opening quote.
    std::string read_string()
    {
        skip_ws();
        if (pos < end && *pos == '"') {
            ++pos;
            const char* start = pos;
            while (pos < end && *pos != '"') ++pos;
            std::string result(start, pos);
            if (pos < end) ++pos;
            return result;
        }
        const char* start = pos;
        while (pos < end && *pos > ' ' && *pos != '"') ++pos;
        return std::string(start, pos);
    }

    float read_float()
    {
        skip_ws();
        const char* start = pos;
        if (pos < end && (*pos == '-' || *pos == '+')) ++pos;
        while (pos < end && ((*pos >= '0' && *pos <= '9') || *pos == '.')) ++pos;
        char tmp[64];
        size_t n = std::min<size_t>(pos - start, sizeof(tmp) - 1);
        memcpy(tmp, start, n);
        tmp[n] = '\0';
        return static_cast<float>(atof(tmp));
    }

    int read_int()
    {
        skip_ws();
        const char* start = pos;
        if (pos < end && (*pos == '-' || *pos == '+')) ++pos;
        while (pos < end && *pos >= '0' && *pos <= '9') ++pos;
        char tmp[64];
        size_t n = std::min<size_t>(pos - start, sizeof(tmp) - 1);
        memcpy(tmp, start, n);
        tmp[n] = '\0';
        return atoi(tmp);
    }

    // Skip to the next line
    void skip_line()
    {
        while (pos < end && *pos != '\n') ++pos;
        if (pos < end) ++pos;
    }

    // Read a token delimited by whitespace, commas, or closing parens.
    // Useful for parsing flag lists like (flag1, flag2, flag3).
    std::string read_token()
    {
        skip_ws();
        // skip leading commas between tokens
        while (pos < end && *pos == ',') { ++pos; skip_ws(); }
        if (pos < end && *pos == '"') return read_string();
        const char* start = pos;
        while (pos < end && *pos > ' ' && *pos != ',' && *pos != ')' && *pos != '"') ++pos;
        return std::string(start, pos);
    }

    // Save/restore position
    [[nodiscard]] const char* save() const { return pos; }
    void restore(const char* p) { pos = p; }
};

// ─── Generic helpers ────────────────────────────────────────────────────────

// Opens a .tbl file via the engine's VFS (handles VPP archives) and reads
// its entire contents into a buffer. Returns empty vector on failure.
std::vector<char> tbl_read_file(const char* filename);

// Case-insensitive hash/equal for use with std::unordered_map keyed by name.
struct CaseInsensitiveHash {
    size_t operator()(const std::string& s) const {
        size_t h = 0;
        for (unsigned char c : s) h = h * 31 + static_cast<unsigned char>(tolower(c));
        return h;
    }
};
struct CaseInsensitiveEqual {
    bool operator()(const std::string& a, const std::string& b) const {
        return _stricmp(a.c_str(), b.c_str()) == 0;
    }
};

template<typename V>
using CaseInsensitiveMap = std::unordered_map<std::string, V, CaseInsensitiveHash, CaseInsensitiveEqual>;

// ─── Common enum lookups ────────────────────────────────────────────────────

// Material name to index (matches game's material table ordering).
// Returns 0 (Default) for unknown names.
int tbl_parse_material(const std::string& name);

// Damage type name to index. Returns -1 for unknown names.
int tbl_parse_damage_type(const std::string& name);

// ─── Clutter ────────────────────────────────────────────────────────────────

// Clutter flag bits (from $Flags: field in clutter.tbl)
enum ClutterFlags : int {
    CLUTTER_FLAG_COLLECTABLE     = 0x001,
    CLUTTER_FLAG_COLLIDE_WEAPON  = 0x002,
    CLUTTER_FLAG_COLLIDE_OBJECT  = 0x004,
    CLUTTER_FLAG_IS_SCREEN       = 0x008,
    CLUTTER_FLAG_SHATTERS        = 0x010,
    CLUTTER_FLAG_HAS_ALPHA       = 0x020,
    CLUTTER_FLAG_IS_SWITCH       = 0x040,
    CLUTTER_FLAG_CAN_CARRY       = 0x080,
    CLUTTER_FLAG_IS_CLOCK        = 0x100,
};

struct ClutterClassInfo {
    std::string class_name;
    std::string v3d_filename;
    float life = -1.0f;
    int material = 0; // 0=Default,1=Rock,2=Metal,3=Flesh,4=Water,5=Lava,6=Solid,7=Sand,8=Ice,9=Glass
    int flags = 0;    // ClutterFlags bitmask
    std::string debris_filename;
    float debris_velocity = 10.0f;
    std::string explode_vclip;      // vclip name (not index)
    float explode_radius = 1.0f;
    std::string corpse_class_name;  // name of corpse class (looked up to get mesh/material)
    float damage_type_factors[11] = {1,1,1,1,1,1,1,1,1,1,1};
    std::string glare_name;             // $Glare: name from effects.tbl (empty = none)

    // Derived collision mode: 0=None, 1=Only Weapons, 2=All
    int collision_mode() const {
        if (flags & CLUTTER_FLAG_COLLIDE_OBJECT) return 2;
        if (flags & CLUTTER_FLAG_COLLIDE_WEAPON) return 1;
        return 0;
    }
};

// Look up clutter class info by name. Parses clutter.tbl on first call.
const ClutterClassInfo* clutter_tbl_find(const char* class_name);

// ─── Effects (Glares) ──────────────────────────────────────────────────────

struct GlareClassInfo {
    std::string name;
    uint8_t color_r = 255, color_g = 255, color_b = 255;
    std::string corona_bitmap;
    float cone_angle = 90.0f;           // full angle in degrees
    float intensity = 1.0f;
    float radius_distance = 0.6f;
    float radius_scale = 1.0f;
    float diminish_distance = -0.05f;
    std::string volumetric_bitmap;
    float volumetric_height = 0.0f;
    float volumetric_length = 0.0f;
};

// Look up glare info by name. Parses effects.tbl on first call.
const GlareClassInfo* glare_tbl_find(const char* glare_name);
