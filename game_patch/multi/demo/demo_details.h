#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "demo_file.h"

struct DemoScoreRow
{
    uint8_t id = 0;
    std::string name;
    int team = -1; // 0 red, 1 blue, -1 unknown/non-team
    int16_t score = 0;
    uint8_t caps = 0;
    uint16_t ping = 0;
    bool has_stats = false; // saw at least one netgame_update entry for this player
};

struct DemoDetails
{
    bool readable = false; // open + header parse succeeded
    DemoHeaderInfo header;
    bool has_footer = false;
    uint32_t duration_ms = 0; // footer when present, else last record timestamp
    bool team_scores_known = false;
    int red_score = 0;
    int blue_score = 0;
    std::vector<DemoScoreRow> rows; // final roster, sorted by team then score desc
};

// Fully scans one demo (relative demo name, may contain subfolders) and reconstructs the
// final scoreboard from the recorded packet stream. Synchronous; per-round demo files
// are small so this is cheap enough to run on click.
DemoDetails demo_scan_details(const std::string& name);
