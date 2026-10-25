#pragma once

#include <windows.h>

enum class EditorRendererKind
{
    d3d9,
    d3d11,
};

// The Alpine options in alpine_editor.ini, shown on the Alpine tab of the Preferences dialog unless noted.
struct EditorSettings
{
    // Takes effect when RED next starts
    EditorRendererKind renderer = EditorRendererKind::d3d11;
    bool texture_hover_outline = true;
    COLORREF texture_hover_outline_color = RGB(255, 220, 0);
    bool background_throttle = true;
    bool force_lod0 = true;
    // Level > Repair holes when building
    bool repair_build_holes = true;
    // Level > Rebuild with out-of-level areas
    bool rebuild_out_of_level = false;

    // View menu, Direct3D 11 only
    bool preview_level_fog = false;
    // Stock RED draws them in every render mode
    bool show_invisible_faces = true;
    bool show_back_faces = false;
    bool fullbright_meshes = false;
    bool show_decals = true;
    // A display mode beside the stock ones (view_display_mode), which picking one of them clears
    bool show_wireframe = false;
    bool wireframe_overlay = false;

    // Remembered from the Texture Browser rather than set in Preferences
    int texture_browser_display_size = 1;
    // the restored window rect in screen coordinates, empty until the browser is first shown
    RECT texture_browser_rect{};
    bool texture_browser_maximized = false;
};

extern EditorSettings g_editor_settings;

// The renderer this session runs on: alpine_editor.ini's choice, overridden by -d3d11 or -d3d9. Read at Init, before
// the Preferences load.
EditorRendererKind editor_session_renderer();

// graphics.cpp
void editor_apply_force_lod0();

void ApplyEditorSettingsPatches();
