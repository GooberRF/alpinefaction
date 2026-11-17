#pragma once

#include <windows.h>
#include <span>

struct DialogTooltip
{
    int control_id;
    const char* text;
};

// Hover tips for a dialog's controls, from WM_INITDIALOG; ids the dialog lacks are skipped. The tooltip
// is owned by the dialog, so it is destroyed with it. Tips also show over disabled controls and labels.
HWND alpine_dlg_add_tooltips(HWND hdlg, std::span<const DialogTooltip> tips);
