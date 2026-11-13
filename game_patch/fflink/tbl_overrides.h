#pragma once

#include <nlohmann/json.hpp>

// Snapshot of server-side weapons.tbl / entity.tbl modifications, reported once in the
// afstats game_start event. Values are captured at launch right after the engine parses
// each tbl and never change afterwards.
namespace afstats {

// Deviations from stock, or a null json when nothing deviates, this is not a dedicated
// server, or a client-required TC mod is loaded.
const nlohmann::json& get_tbl_overrides();

// Install the tbl parse hooks and register the debug command. Called from fflink::do_patch().
void tbl_overrides_do_patch();

} // namespace afstats
