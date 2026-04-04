#include "tbl.h"
#include <stdexcept>
#include "vtypes.h"
#include <xlog/xlog.h>

// ─── Generic helpers ────────────────────────────────────────────────────────

std::vector<char> tbl_read_file(const char* filename)
{
    rf::File file;
    if (file.open_mode(filename, 1, 0x98967f) < 0) {
        xlog::warn("tbl: failed to open '{}'", filename);
        return {};
    }

    int file_size = file.get_size();
    if (file_size <= 0) {
        xlog::warn("tbl: '{}' is empty or unreadable (size={})", filename, file_size);
        file.close();
        return {};
    }

    std::vector<char> buf(file_size);
    int bytes_read = file.read(buf.data(), file_size);
    file.close();
    if (bytes_read != file_size) {
        xlog::warn("tbl: '{}' short read ({}/{})", filename, bytes_read, file_size);
        return {};
    }
    return buf;
}

int tbl_parse_material(const std::string& name)
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

int tbl_parse_damage_type(const std::string& name)
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

// Clutter flag name to bit mapping
static int parse_clutter_flag(const std::string& name)
{
    static const struct { const char* name; int bit; } flags[] = {
        {"collectable",     CLUTTER_FLAG_COLLECTABLE},
        {"collide_weapon",  CLUTTER_FLAG_COLLIDE_WEAPON},
        {"collide_object",  CLUTTER_FLAG_COLLIDE_OBJECT},
        {"is_screen",       CLUTTER_FLAG_IS_SCREEN},
        {"shatters",        CLUTTER_FLAG_SHATTERS},
        {"has_alpha",       CLUTTER_FLAG_HAS_ALPHA},
        {"is_switch",       CLUTTER_FLAG_IS_SWITCH},
        {"can_carry",       CLUTTER_FLAG_CAN_CARRY},
        {"is_clock",        CLUTTER_FLAG_IS_CLOCK},
    };
    for (auto& f : flags) {
        if (_stricmp(name.c_str(), f.name) == 0) return f.bit;
    }
    return 0;
}

// ─── Clutter ────────────────────────────────────────────────────────────────

static CaseInsensitiveMap<ClutterClassInfo> g_clutter_classes;
static bool g_clutter_parsed = false;

static void parse_clutter_tbl()
{
    if (g_clutter_parsed) return;
    g_clutter_parsed = true;

    auto buf = tbl_read_file("clutter.tbl");
    if (buf.empty()) return;

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

        if (tok.match("$Flags:")) {
            // Format: (flag1, flag2, ...)
            current->flags = 0;
            if (tok.match("(")) {
                while (!tok.at_end()) {
                    if (tok.peek(")")) { tok.match(")"); break; }
                    std::string flag_name = tok.read_token();
                    if (flag_name.empty()) break; // no progress possible
                    current->flags |= parse_clutter_flag(flag_name);
                }
            }
        }
        else if (tok.match("$V3D Filename:")) {
            current->v3d_filename = tok.read_string();
        }
        else if (tok.match("$Life:")) {
            current->life = tok.read_float();
        }
        else if (tok.match("$Material:")) {
            current->material = tbl_parse_material(tok.read_string());
        }
        else if (tok.match("$Debris Filename:")) {
            current->debris_filename = tok.read_string();
        }
        else if (tok.match("$Debris Velocity:")) {
            current->debris_velocity = tok.read_float();
        }
        else if (tok.match("$Explode Anim Radius:")) {
            current->explode_radius = tok.read_float();
        }
        else if (tok.match("$Explode Anim:")) {
            current->explode_vclip = tok.read_string();
        }
        else if (tok.match("$Corpse Class Name:")) {
            current->corpse_class_name = tok.read_string();
        }
        else if (tok.match("$Damage Type Factor:")) {
            std::string type_name = tok.read_string();
            float factor = tok.read_float();
            int idx = tbl_parse_damage_type(type_name);
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
        return &it->second;
    }
    xlog::warn("clutter_tbl: class '{}' not found in {} entries", class_name, g_clutter_classes.size());
    return nullptr;
}
