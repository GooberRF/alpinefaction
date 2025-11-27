#pragma once

#include <functional>
#include <common/utils/string-utils.h>

enum GameLang
{
    LANG_EN = 0,
    LANG_GR = 1,
    LANG_FR = 2,
};

void vpackfile_apply_patches();
GameLang get_installed_game_lang();
bool is_modded_game();
void vpackfile_find_matching_files(const StringMatcher& query, std::function<void(const char*)> result_consumer);
void vpackfile_disable_overriding();

// suppress unnecessary missing asset warnings for asset files referenced by but missing from the stock game
inline bool is_known_missing_asset(std::string_view filename) {
    static constexpr std::string_view suppressed_filenames[] = {
        "bigboom.vbm",
        "fp_shotgun_reload.wav",
        "laser loop.wav",
    };

    for (const auto suppressed : suppressed_filenames) {
        if (filename.size() != suppressed.size()) {
            continue;
        }

        bool matches = true;
        for (size_t i = 0; i < filename.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(filename[i])) !=
                std::tolower(static_cast<unsigned char>(suppressed[i]))) {
                matches = false;
                break;
            }
        }

        if (matches) {
            return true;
        }
    }

    return false;
}
