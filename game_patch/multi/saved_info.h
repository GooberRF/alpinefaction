#pragma once

#include <optional>
#include <toml++/toml.hpp>

namespace rf { struct Vector3; }
std::optional<toml::table> saved_info_read();
bool saved_info_write(const toml::table& sections);
bool saved_info_read_vec3(const toml::array* arr, rf::Vector3& out);
