#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <cstddef>
#include <xlog/xlog.h>


// ======= Globals and utility =======
void load_af_options_config();
void load_level_info_config(const std::string& level_filename);

// ======= Alpine options =======
enum class AlpineOptionID
{
    GeomodEmitter_Default,
    GeomodEmitter_Driller,
    GeomodTexture_Ice,
    FirstLevelFilename,
    TrainingLevelFilename,
    DisableMultiplayerButton,
    DisableSingleplayerButtons,
    UseStockPlayersConfig,
    AssaultRifleAmmoColor,
    PrecisionRifleScopeColor,
    SniperRifleScopeColor,
    RailDriverFireGlowColor,
    RailDriverFireFlashColor,
    SumTrailerButtonAction,
    SumTrailerButtonURL,
    SumTrailerButtonBikFile,
    SumTrailerButtonLevelFile,
    PlayerEntityType,
    PlayerSuitEntityType,
    PlayerScientistEntityType,
    FallDamageLandMultiplier,
    FallDamageSlamMultiplier,
    MultiplayerWalkSpeed,
    MultiplayerCrouchWalkSpeed,
    WalkableSlopeThreshold,
    PlayerHeadlampColor,
    PlayerHeadlampIntensity,
    PlayerHeadlampRange,
    PlayerHeadlampRadius,
    RailDriverScannerColor,
    MultiTimerXOffset,
    MultiTimerYOffset,
    MultiTimerColor,
    DefaultThirdPerson,
    _optioncount // dummy for total count
};

constexpr std::size_t to_index(AlpineOptionID option_id)
{
    return static_cast<std::size_t>(option_id);
}

constexpr std::size_t af_option_count = to_index(AlpineOptionID::_optioncount);

// Variant type to represent all possible configuration option values
using OptionValue = std::variant<std::string, uint32_t, float, int, bool>;

// Metadata structure to store option information, including its parsing function
struct OptionMetadata
{
    AlpineOptionID id;
    std::string filename;
    std::function<std::optional<OptionValue>(const std::string&)> parse_function;
    bool apply_on_server = false; // will only be set on server if true (unnecessary for most)
};

// Main configuration structure for options
struct AlpineOptionsConfig
{
    // Store options in a map with their parsed values
    std::unordered_map<AlpineOptionID, OptionValue> options;
    std::array<bool, af_option_count> options_loaded = {}; // Track loaded options

    // Check if a specific option is loaded
    bool is_option_loaded(AlpineOptionID option_id) const
    {
        return options_loaded[static_cast<std::size_t>(option_id)];
    }
};

extern AlpineOptionsConfig g_alpine_options_config;

template<typename T>
inline T get_option_value(AlpineOptionID id)
{
    return std::get<T>(g_alpine_options_config.options.at(id));
}

// Helper function to retrieve an option or a default value
template<typename T>
inline T get_option_or_default(AlpineOptionID id, T default_value)
{
    return g_alpine_options_config.is_option_loaded(id) ? get_option_value<T>(id) : default_value;
}

// ======= Alpine level info ======= 
enum class AlpineLevelInfoID
{
    LightmapClampFloor,
    LightmapClampCeiling,
    IdealPlayerCount,
    AuthorContact,
    AuthorWebsite,
    Description,
    PlayerHeadlampColor,
    PlayerHeadlampIntensity,
    PlayerHeadlampRange,
    PlayerHeadlampRadius,
    ChatMap1,
    ChatMap2,
    ChatMap3,
    ChatMap4,
    ChatMap5,
    ChatMap6,
    ChatMap7,
    ChatMap8,
    ChatMap9,
    CraterTexturePPM,
    _optioncount       // dummy for total count
};

// Convert enum to index
constexpr std::size_t to_index(AlpineLevelInfoID option_id)
{
    return static_cast<std::size_t>(option_id);
}

constexpr std::size_t aflevel_option_count = to_index(AlpineLevelInfoID::_optioncount);

// Variant type to represent different configuration values
using LevelInfoValue = std::variant<std::string, uint32_t, float, int, bool>;

struct LevelInfoMetadata
{
    AlpineLevelInfoID id;
    std::function<std::optional<LevelInfoValue>(const std::string&)> parse_function;
};

// Main configuration structure for level info
struct AlpineLevelInfoConfig
{
    std::string current_level;
    std::unordered_map<AlpineLevelInfoID, LevelInfoValue> level_options;
    std::unordered_map<std::string, std::string> mesh_replacements; // stored lowercase

    void reset_for_level(const std::string& level)
    {
        current_level = level;
        level_options.clear();
        mesh_replacements.clear();
    }

    // Check if an option exists for a given level
    bool is_option_loaded(const std::string& level, AlpineLevelInfoID option_id) const
    {
        if (level != current_level) {
            return false; // should never happen
        }

        return level_options.find(option_id) != level_options.end();
    }
};

extern AlpineLevelInfoConfig g_alpine_level_info_config;

// Get an option value for a level
template<typename T>
inline T get_level_info_value(AlpineLevelInfoID id)
{
    return std::get<T>(g_alpine_level_info_config.level_options.at(id));
}

// Get an option value with a default fallback
template<typename T>
inline T get_level_info_or_default(const std::string& level, AlpineLevelInfoID id, T default_value)
{
    if (g_alpine_level_info_config.is_option_loaded(level, id)) {
        return get_level_info_value<T>(id);
    }
    return default_value;
}

bool is_p2t_fix_level(const std::string& filename);
bool is_known_run_level(const std::string& filename);
void open_url(const std::string& url);
