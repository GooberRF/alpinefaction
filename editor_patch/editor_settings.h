#pragma once

#include <windows.h>

// The Alpine options in alpine_editor.ini, shown on the Alpine tab of the Preferences dialog unless noted.
struct EditorSettings
{
    bool texture_hover_outline = true;
    COLORREF texture_hover_outline_color = RGB(255, 220, 0);
    bool background_throttle = true;
    bool force_lod0 = true;
    // Level > Repair holes when building
    bool repair_build_holes = true;
    // Level > Rebuild with out-of-level areas
    bool rebuild_out_of_level = false;

    // Remembered from the Texture Browser rather than set in Preferences
    int texture_browser_display_size = 1;
    // the restored window rect in screen coordinates, empty until the browser is first shown
    RECT texture_browser_rect{};
    bool texture_browser_maximized = false;
};

extern EditorSettings g_editor_settings;

// graphics.cpp
void editor_apply_force_lod0();

void ApplyEditorSettingsPatches();
