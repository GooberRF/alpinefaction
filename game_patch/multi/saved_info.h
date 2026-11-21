#pragma once

#include <optional>
#include <string>
#include <toml++/toml.hpp>

namespace rf { struct Vector3; }
// "<RF root>\saved", created when `create_dir` is set. Empty if it could not be created.
std::string saved_dir_path(bool create_dir);
std::optional<toml::table> saved_info_read();
bool saved_info_write(const toml::table& sections);
bool saved_info_read_vec3(const toml::array* arr, rf::Vector3& out);
