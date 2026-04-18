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
        else if (tok.match("$Glare:")) {
            current->glare_name = tok.read_string();
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

// ─── Effects (Glares) ──────────────────────────────────────────────────────

static CaseInsensitiveMap<GlareClassInfo> g_glare_classes;
static bool g_glare_parsed = false;

static void parse_effects_tbl()
{
    if (g_glare_parsed) return;
    g_glare_parsed = true;

    auto buf = tbl_read_file("effects.tbl");
    if (buf.empty()) return;

    TblTokenizer tok(buf.data(), buf.size());

    // Find #Glares section
    while (!tok.at_end()) {
        if (tok.match("#Glares")) break;
        tok.skip_line();
    }

    GlareClassInfo* current = nullptr;

    while (!tok.at_end()) {
        if (tok.peek("#End")) break;

        if (tok.match("$Name:")) {
            std::string name = tok.read_string();
            if (!name.empty()) {
                current = &g_glare_classes[name];
                current->name = name;
            }
            continue;
        }

        if (!current) {
            tok.skip_line();
            continue;
        }

        if (tok.match("$Light Color:")) {
            // Format: {R, G, B}
            tok.match("{");
            current->color_r = static_cast<uint8_t>(tok.read_int());
            tok.match(",");
            current->color_g = static_cast<uint8_t>(tok.read_int());
            tok.match(",");
            current->color_b = static_cast<uint8_t>(tok.read_int());
            tok.match("}");
        }
        else if (tok.match("$Corona Bitmap:")) {
            current->corona_bitmap = tok.read_string();
        }
        else if (tok.match("$Cone Angle:")) {
            current->cone_angle = tok.read_float();
        }
        else if (tok.match("$Intensity:")) {
            current->intensity = tok.read_float();
        }
        else if (tok.match("$Radius Distance Factor:")) {
            current->radius_distance = tok.read_float();
        }
        else if (tok.match("$Radius Scale Factor:")) {
            current->radius_scale = tok.read_float();
        }
        else if (tok.match("$Diminish Distance:")) {
            current->diminish_distance = tok.read_float();
        }
        else if (tok.match("$Volumetric Bitmap:")) {
            current->volumetric_bitmap = tok.read_string();
        }
        else if (tok.match("$Volumetric Height:")) {
            current->volumetric_height = tok.read_float();
        }
        else if (tok.match("$Volumetric Length:")) {
            current->volumetric_length = tok.read_float();
        }
        else if (tok.match("$Reflection Bitmap:")) {
            tok.read_string(); // read and discard — DedCorona has no reflection field
        }
        else {
            tok.skip_line();
        }
    }

    xlog::info("effects_tbl: parsed {} glare entries", g_glare_classes.size());
}

const GlareClassInfo* glare_tbl_find(const char* glare_name)
{
    parse_effects_tbl();
    auto it = g_glare_classes.find(glare_name);
    if (it != g_glare_classes.end()) {
        return &it->second;
    }
    xlog::warn("effects_tbl: glare '{}' not found in {} entries", glare_name, g_glare_classes.size());
    return nullptr;
}

// ─── Entities ──────────────────────────────────────────────────────────────

static CaseInsensitiveMap<EntityClassInfo> g_entity_classes;
static bool g_entity_parsed = false;

static void parse_entity_tbl()
{
    if (g_entity_parsed) return;
    g_entity_parsed = true;

    auto buf = tbl_read_file("entity.tbl");
    if (buf.empty()) return;

    TblTokenizer tok(buf.data(), buf.size());

    // Find #Entity Classes section
    while (!tok.at_end()) {
        if (tok.match("#Entity Classes")) break;
        tok.skip_line();
    }

    EntityClassInfo* current = nullptr;

    while (!tok.at_end()) {
        if (tok.peek("#End")) break;

        if (tok.match("$Name:")) {
            std::string name = tok.read_string();
            if (!name.empty()) {
                current = &g_entity_classes[name];
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
        else if (tok.match("$Flags:") || tok.match("$Flags2:")) {
            // Parse flag list: ("flag1" "flag2" ...)
            if (tok.match("(")) {
                while (!tok.at_end()) {
                    if (tok.peek(")")) { tok.match(")"); break; }
                    std::string flag = tok.read_string();
                    if (flag.empty()) { tok.skip_line(); break; }
                    if (_stricmp(flag.c_str(), "no_collide") == 0)
                        current->no_collide = true;
                    else if (_stricmp(flag.c_str(), "collide_player") == 0)
                        current->collide_player = true;
                }
            }
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
        else if (tok.match("$Explode Anim Radius:")) {
            current->explode_radius = tok.read_float();
        }
        else if (tok.match("$Explode Anim:")) {
            current->explode_vclip = tok.read_string();
        }
        else if (tok.match("$Corpse V3D Filename:")) {
            current->corpse_v3d_filename = tok.read_string();
        }
        else if (tok.match("$Damage Type Factor:")) {
            std::string type_name = tok.read_string();
            float factor = tok.read_float();
            int idx = tbl_parse_damage_type(type_name);
            if (idx >= 0 && idx < 11) {
                current->damage_type_factors[idx] = factor;
            }
        }
        else if (tok.match("$Thruster VFX")) {
            // Format: $Thruster VFX N: filename.vfx
            tok.read_int(); // consume N
            tok.match(":");
            std::string vfx = tok.read_string();
            if (!vfx.empty()) {
                current->thruster_vfx_names.push_back(vfx);
            }
        }
        else if (tok.match("$Corona (Glare)")) {
            // Format: $Corona (Glare) N: glare_name
            // Skip the number and colon, read the glare name
            tok.read_int(); // consume N
            tok.match(":");
            std::string glare_name = tok.read_string();
            if (!glare_name.empty()) {
                current->corona_glare_names.push_back(glare_name);
            }
        }
        else if (tok.match("+State:")) {
            // Format: +State: "state_name" "anim_filename"
            std::string state_name = tok.read_string();
            std::string anim_file = tok.read_string();
            if (_stricmp(state_name.c_str(), "stand") == 0 && current->stand_anim.empty()) {
                current->stand_anim = anim_file;
            }
        }
        else {
            tok.skip_line();
        }
    }

    xlog::info("entity_tbl: parsed {} entity classes", g_entity_classes.size());
}

const EntityClassInfo* entity_tbl_find(const char* class_name)
{
    parse_entity_tbl();
    auto it = g_entity_classes.find(class_name);
    if (it != g_entity_classes.end()) {
        return &it->second;
    }
    xlog::warn("entity_tbl: class '{}' not found in {} entries", class_name, g_entity_classes.size());
    return nullptr;
}
