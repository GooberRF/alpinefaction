#pragma once

#include <string>

// Tail cache for RED's GFace list appends. Live only inside a FaceListCacheWindow (level load and
// each Build Geometry tick) and outside any window procedure or message box that runs within it;
// everywhere else the hooked list helpers run their stock code. Closing a window empties the cache.
class FaceListCacheWindow
{
public:
    FaceListCacheWindow();
    ~FaceListCacheWindow();
    FaceListCacheWindow(const FaceListCacheWindow&) = delete;
    FaceListCacheWindow& operator=(const FaceListCacheWindow&) = delete;

private:
    // A window opened from inside a message handler (File > Open) is live itself.
    int outer_pause_depth_ = 0;
};

// Empty unless verify_face_list_cache is on: appends checked against a full walk, and how many disagreed.
std::string face_list_cache_verify_summary();
void face_list_cache_log_verify_summary();

void ApplyFaceListCachePatches();
