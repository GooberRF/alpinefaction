#include <windows.h>
#include <commctrl.h>
#include "dialog_tooltips.h"

HWND alpine_dlg_add_tooltips(HWND hdlg, std::span<const DialogTooltip> tips)
{
    HWND tooltip =
        CreateWindowExA(WS_EX_TOPMOST, TOOLTIPS_CLASSA, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
                        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hdlg, nullptr, nullptr, nullptr);
    if (!tooltip) return nullptr;

    RECT wrap{0, 0, 180, 0};
    MapDialogRect(hdlg, &wrap);
    SendMessageA(tooltip, TTM_SETMAXTIPWIDTH, 0, wrap.right);

    for (const DialogTooltip& tip : tips) {
        HWND control = GetDlgItem(hdlg, tip.control_id);
        if (!control || !tip.text) continue;

        // The V2 size is the one both comctl32 5.8 and 6 accept.
        TOOLINFOA ti{};
        ti.cbSize = TTTOOLINFOA_V2_SIZE;
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = hdlg;
        ti.uId = reinterpret_cast<UINT_PTR>(control);
        ti.lpszText = const_cast<char*>(tip.text);
        SendMessageA(tooltip, TTM_ADDTOOLA, 0, reinterpret_cast<LPARAM>(&ti));

        // A disabled control, or a static without SS_NOTIFY, leaves the mouse to the dialog.
        ti.uFlags = TTF_SUBCLASS;
        ti.uId = static_cast<UINT_PTR>(tip.control_id);
        GetWindowRect(control, &ti.rect);
        MapWindowPoints(nullptr, hdlg, reinterpret_cast<POINT*>(&ti.rect), 2);
        SendMessageA(tooltip, TTM_ADDTOOLA, 0, reinterpret_cast<LPARAM>(&ti));
    }
    return tooltip;
}
