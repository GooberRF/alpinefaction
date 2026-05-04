#pragma once

#include "vtypes.h"
#include "textures.h"
#include <string>
#include <vector>

// Editor-side bitmap format extensions
constexpr int EDITOR_BM_TYPE_STB = 0x12;
constexpr int EDITOR_BM_TYPE_DDS = 0x10;
constexpr int EDITOR_BM_TYPE_ATX = 0x11;

// Format constants matching the stock engine values
constexpr int EDITOR_BM_FORMAT_8888_ARGB = 7;
constexpr int EDITOR_BM_FORMAT_888_RGB = 6;

void install_editor_bitmap_loader_hooks();

// Parse an ATX file and return the list of texture filenames it references
std::vector<std::string> parse_atx_dependencies(const char* atx_filename);
