#pragma once

#include <windows.h>
#include <commdlg.h>

// Answers a GetOpenFileNameA request with the sound browser, returning the picked file's bare name.
BOOL alpine_sound_browser_answer(OPENFILENAMEA* ofn);
