#include "tbl.h"
#include <vector>
#include <stdexcept>
#include "vtypes.h"
#include <unordered_map>
#include <cstring>
#include <cstdlib>
#include <xlog/xlog.h>

// Simple tokenizer for .tbl format
struct TblTokenizer {
    const char* pos;
    const char* end;

    TblTokenizer(const char* data, size_t len) : pos(data), end(data + len) {}

    void skip_ws()
    {
        while (pos < end) {
            if (*pos == '/' && pos + 1 < end && pos[1] == '/') {
                // skip line comment
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

    bool at_end() const { return pos >= end; }

    // Peek at the next non-whitespace token without consuming it
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

    // Read a quoted string, returns content without quotes
    std::string read_string()
    {
        skip_ws();
        if (pos < end && *pos == '"') {
            ++pos;
            const char* start = pos;
            while (pos < end && *pos != '"') ++pos;
            std::string result(start, pos);
            if (pos < end) ++pos; // skip closing quote
            return result;
        }
        // bare word
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
        return static_cast<float>(atof(start));
    }

    // Skip to the next line
    void skip_line()
    {
        while (pos < end && *pos != '\n') ++pos;
        if (pos < end) ++pos;
    }

    // Save/restore position
    const char* save() const { return pos; }
    void restore(const char* p) { pos = p; }
};

// Material name to index mapping (matches game's material table ordering)
static int parse_material_name(const std::string& name)
{
    static const struct { const char* name; int index; } materials[] = {
        {"Default", 0}, {"Rock", 1}, {"Metal", 2}, {"Flesh", 3}, {"Water", 4},
        {"Lava", 5}, {"Solid", 6}, {"Sand", 7}, {"Ice", 8}, {"Glass", 9},
    };
    for (auto& m : materials) {
        if (_stricmp(name.c_str(), m.name) == 0) return m.index;
    }
    return 0;
}

// Damage type name to index mapping
static int parse_damage_type(const std::string& name)
{
    static const struct { const char* name; int index; } types[] = {
        {"bash", 0}, {"bullet", 1}, {"armor piercing bullet", 2},
        {"explosive", 3}, {"fire", 4}, {"energy", 5},
        {"electrical", 6}, {"acid", 7}, {"scalding", 8},
    };
    for (auto& t : types) {
        if (_stricmp(name.c_str(), t.name) == 0) return t.index;
    }
    return -1;
}

// Case-insensitive hash map
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

static std::unordered_map<std::string, ClutterClassInfo, CaseInsensitiveHash, CaseInsensitiveEqual> g_clutter_classes;
static bool g_parsed = false;

static void parse_clutter_tbl()
{
    if (g_parsed) return;
    g_parsed = true;

    // Open clutter.tbl using the stock engine's file system (handles VPP archives)
    rf::File file;
    if (file.open_mode("clutter.tbl", 1, 0x98967f) < 0) {
        xlog::warn("clutter_tbl: failed to open clutter.tbl");
        return;
    }

    int file_size = file.get_size();
    if (file_size <= 0) {
        xlog::warn("clutter_tbl: clutter.tbl is empty or unreadable (size={})", file_size);
        file.close();
        return;
    }

    std::vector<char> buf(file_size);
    file.read(buf.data(), file_size);
    file.close();

    TblTokenizer tok(buf.data(), buf.size());

    // Find #Clutter section
    while (!tok.at_end()) {
        if (tok.match("#Clutter")) break;
        tok.skip_line();
    }

    // Parse entries
    ClutterClassInfo* current = nullptr;

    while (!tok.at_end()) {
        if (tok.peek("#End")) break;

        if (tok.match("$Class Name:")) {
            std::string name = tok.read_string();
            if (!name.empty()) {
                current = &g_clutter_classes[name];
                current->class_name = name;
            }
            continue;
        }

        if (!current) {
            tok.skip_line();
            continue;
        }

        if (tok.match("$V3D Filename:")) {
            current->v3d_filename = tok.read_string();
        }
        else if (tok.match("$Life:")) {
            current->life = tok.read_float();
        }
        else if (tok.match("$Material:")) {
            std::string mat = tok.read_string();
            current->material = parse_material_name(mat);
        }
        else if (tok.match("$Debris Filename:")) {
            current->debris_filename = tok.read_string();
        }
        else if (tok.match("$Debris Velocity:")) {
            current->debris_velocity = tok.read_float();
        }
        else if (tok.match("$Explode Anim:")) {
            current->explode_vclip = tok.read_string();
        }
        else if (tok.match("$Explode Anim Radius:")) {
            current->explode_radius = tok.read_float();
        }
        else if (tok.match("$Corpse Class Name:")) {
            current->corpse_class_name = tok.read_string();
        }
        else if (tok.match("$Damage Type Factor:")) {
            std::string type_name = tok.read_string();
            float factor = tok.read_float();
            int idx = parse_damage_type(type_name);
            if (idx >= 0 && idx < 11) {
                current->damage_type_factors[idx] = factor;
            }
        }
        else {
            tok.skip_line();
        }
    }

    xlog::info("clutter_tbl: parsed {} clutter classes", g_clutter_classes.size());
}

const ClutterClassInfo* clutter_tbl_find(const char* class_name)
{
    parse_clutter_tbl();
    auto it = g_clutter_classes.find(class_name);
    if (it != g_clutter_classes.end()) {
        xlog::info("clutter_tbl: found '{}' (life={}, material={}, debris='{}', corpse='{}')",
            class_name, it->second.life, it->second.material,
            it->second.debris_filename, it->second.corpse_class_name);
        return &it->second;
    }
    xlog::warn("clutter_tbl: class '{}' not found in {} entries", class_name, g_clutter_classes.size());
    return nullptr;
}
