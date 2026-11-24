#pragma once

#include <filesystem>
#include <string>
#include "asg_model.h"

// Whether the game in progress saves in the Alpine format, which is what the save hook writes
bool asg_game_saves_asg();
// Whether a loaded .svl continues in the Alpine format, keeping only its level. The player confirms it first.
bool asg_svl_load_converts();
inline constexpr const char* asg_svl_load_title = "LOAD ORIGINAL-FORMAT SAVE";
inline constexpr const char* asg_svl_load_text =
    "This save uses the original format. It will continue in the Alpine format: progress in the current level is "
    "kept, but levels visited before it will reset if you return to them.";
// The mod the game runs, or empty; a save made under another mod does not load
std::string asg_running_mod_name();
// Decodes and checks an .asg file, then starts loading its level. Shows the reason and returns false if it
// cannot be loaded; nothing has changed then.
bool asg_load_game(const std::filesystem::path& path);
// Makes the save the one the death popup offers to load, as stock's save and load menu do
void asg_set_last_save(const std::filesystem::path& path);
// From level_load, before the level's objects are created: picks the format of the game
void asg_level_load_begin(const char* state_filename);
// True while a load restores the level's objects. Objects it kills are not deaths in the game: Alpine's death hooks
// must not fire events or count them.
bool asg_restoring();
// From level_init_post: finishes restoring what needs the level's Alpine objects complete
void asg_level_init_post();
void asg_apply_patches();
