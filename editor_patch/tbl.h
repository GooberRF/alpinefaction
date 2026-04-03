#pragma once

#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <algorithm>
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
        for (char c : s) h = h * 31 + static_cast<unsigned char>(tolower(c));
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

struct ClutterClassInfo {
    std::string class_name;
    std::string v3d_filename;
    float life = -1.0f;
    int material = 0; // 0=Default,1=Rock,2=Metal,3=Flesh,4=Water,5=Lava,6=Solid,7=Sand,8=Ice,9=Glass
    std::string debris_filename;
    float debris_velocity = 10.0f;
    std::string explode_vclip;      // vclip name (not index)
    float explode_radius = 1.0f;
    std::string corpse_class_name;  // name of corpse class (looked up to get mesh/material)
    float damage_type_factors[11] = {1,1,1,1,1,1,1,1,1,1,1};
};

// Look up clutter class info by name. Parses clutter.tbl on first call.
const ClutterClassInfo* clutter_tbl_find(const char* class_name);
