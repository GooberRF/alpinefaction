#pragma once

#include <functional>
#include <string>
#include <vector>

namespace fflink::achievements {

// Result of an achievement HTTP exchange (sync or push).
// Both endpoints share the same on-the-wire response shape: "key,uid,uid,uid,...".
struct UnlockedAchievementsResult
{
    bool ok = false;                 // false on transport, HTTP, or parse failure
    int returned_key = 0;            // first comma-separated token from the response
    std::vector<int> unlocked_uids;  // remaining tokens, parsed as integers
    std::string error_detail;        // human-readable failure cause when ok == false
};

// Initial sync (HTTP GET /afachievement/v1/initial/{token}).
// Fires asynchronously; on_main is invoked on the main thread via fflink::do_frame().
void sync_async(std::string token, std::function<void(UnlockedAchievementsResult)> on_main);

// Update push (HTTP POST /afachievement/v1/update/{token}, body: "key={key},{payload}").
// payload is the comma-separated list of "uid=count", "kill=...", "use=..." entries
// produced by the achievement manager. The client prepends "key=N," before sending.
// Fires asynchronously; on_main is invoked on the main thread via fflink::do_frame().
void push_async(std::string token, int key, std::string payload,
                std::function<void(UnlockedAchievementsResult)> on_main);

} // namespace fflink::achievements
