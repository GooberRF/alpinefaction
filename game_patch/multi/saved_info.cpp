#include "saved_info.h"
#include <fstream>
#include <format>
#include <cmath>
#include <string>
#include <toml++/toml.hpp>
#include <xlog/xlog.h>
#include <common/version/version.h>
#include <common/utils/string-utils.h>
#include "../rf/level.h"
#include "../rf/math/vector.h"
#include "../rf/file/file.h"
#include <windows.h>

namespace
{
    // Build "<RF root>\saved\<map>_saved.afl". Returns empty if no level is loaded or the path is too
    // long. When create_dir is set, the \saved\ directory is created if missing (used for writing).
    std::string saved_info_path(bool create_dir)
    {
        std::string map = std::string{get_filename_without_ext(rf::level.filename.c_str())};
        if (map.empty())
            return {};
        auto dir = std::format("{}\\saved", rf::root_path);
        if (dir.size() + map.size() + 16 > rf::max_path_len) {
            xlog::error("saved-info path is too long!");
            return {};
        }
        if (create_dir && !CreateDirectoryA(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            xlog::error("Failed to create saved directory, error {}", GetLastError());
            return {};
        }
        return std::format("{}\\{}_saved.afl", dir, map);
    }
}

std::optional<toml::table> saved_info_read()
{
    auto path = saved_info_path(false);
    if (path.empty())
        return std::nullopt;

    toml::table root;
    try {
        root = toml::parse_file(path);
    }
    catch (const toml::parse_error&) {
        return std::nullopt; // missing or malformed - start clean
    }

    if (const auto* header = root.get_as<toml::table>("header")) {
        const auto ver = (*header)["afl_version"].value<int64_t>().value_or(0);
        if (ver > AFL_VERSION) {
            xlog::warn("Ignoring saved-info {} written by a newer Alpine Faction (afl_version {})", path, ver);
            return std::nullopt;
        }
    }

    return root;
}

bool saved_info_write(const toml::table& sections)
{
    auto path = saved_info_path(true);
    if (path.empty())
        return false;

    toml::table root{
        {"header", toml::table{
            {"afl_version", AFL_VERSION},
            {"level", std::string{rf::level.filename.c_str()}},
        }},
    };
    for (auto&& [key, value] : sections) {
        if (key.str() == "header")
            continue; // reserved: saved_info owns the version/level header
        root.insert_or_assign(key, value);
    }

    std::ofstream file(path);
    if (!file.is_open()) {
        xlog::error("Failed to open saved-info file for write {}", path);
        return false;
    }
    file << root;
    return true;
}

bool saved_info_read_vec3(const toml::array* arr, rf::Vector3& out)
{
    if (!arr || arr->size() != 3)
        return false;
    // Accept both floating-point and integer TOML nodes (so a hand-edited `pos = [1,2,3]` isn't
    // silently dropped) and reject any non-finite (nan/inf) component.
    auto read_component = [](const toml::node* n, float& out_component) -> bool {
        if (!n)
            return false;
        double v;
        if (const auto d = n->value<double>())
            v = *d;
        else if (const auto i = n->value<int64_t>())
            v = static_cast<double>(*i);
        else
            return false;
        if (!std::isfinite(v))
            return false;
        out_component = static_cast<float>(v);
        return true;
    };
    return read_component(arr->get(0), out.x)
        && read_component(arr->get(1), out.y)
        && read_component(arr->get(2), out.z);
}
