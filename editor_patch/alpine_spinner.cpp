#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "alpine_spinner.h"

namespace
{

constexpr UINT_PTR spinner_subclass_id = 1;
constexpr char spinner_prop_name[] = "AlpineSpinner";

struct SpinnerState
{
    HWND edit = nullptr;
    float step = 1.0f;
    float min_v = 0.0f;
    float max_v = 1.0f;
    int decimals = 2;
    bool pressed = false;
    bool dragging = false;
    float start_value = 0.0f;
    int start_y = 0;
};

SpinnerState* spinner_state(HWND spin)
{
    return static_cast<SpinnerState*>(GetPropA(spin, spinner_prop_name));
}

float spinner_read(const SpinnerState* st)
{
    char buf[32] = {};
    GetWindowTextA(st->edit, buf, sizeof(buf));
    const double v = std::atof(buf);
    return std::isfinite(v) ? static_cast<float>(v) : 0.0f;
}

void spinner_write(const SpinnerState* st, float value)
{
    if (!std::isfinite(value)) value = st->min_v;
    value = std::clamp(value, st->min_v, st->max_v);
    char buf[32];
    if (st->decimals <= 0) {
        std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::lround(value)));
    }
    else {
        std::snprintf(buf, sizeof(buf), "%.*f", st->decimals, static_cast<double>(value));
    }
    // A horizontal drag produces a stream of WM_MOUSEMOVE at the same value, and every
    // SetWindowTextA of it would still fire EN_CHANGE and repaint - which on a dialog with a live
    // viewport preview means re-solving and redrawing for no change at all.
    char current[32] = {};
    GetWindowTextA(st->edit, current, sizeof(current));
    if (std::strcmp(current, buf) == 0) {
        return;
    }
    SetWindowTextA(st->edit, buf);
}

// Every message is forwarded: the up-down keeps its native click-step, autorepeat and arrow keys,
// and only learns that the cursor left its rect because the move reaches it as well.
LRESULT CALLBACK spinner_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR,
                                       DWORD_PTR)
{
    auto* st = spinner_state(hwnd);
    switch (msg) {
    case WM_LBUTTONDOWN:
        if (st) {
            st->pressed = true;
            st->dragging = false;
            st->start_value = spinner_read(st);
            st->start_y = GET_Y_LPARAM(lp);
        }
        break;
    case WM_MOUSEMOVE:
        if (st && st->pressed) {
            const int y = GET_Y_LPARAM(lp);
            if (!st->dragging) {
                POINT pt{GET_X_LPARAM(lp), y};
                RECT rc{};
                GetClientRect(hwnd, &rc);
                if (!PtInRect(&rc, pt)) {
                    // Re-baselined where the drag actually starts, so leaving the arrows does not
                    // jump the value by the pixels already travelled (or by the click-step the
                    // press itself applied).
                    st->dragging = true;
                    st->start_value = spinner_read(st);
                    st->start_y = y;
                }
            }
            if (st->dragging) {
                spinner_write(st,
                              st->start_value + static_cast<float>(st->start_y - y) * st->step);
            }
        }
        break;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (st) {
            st->pressed = false;
            st->dragging = false;
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, spinner_subclass_proc, spinner_subclass_id);
        RemovePropA(hwnd, spinner_prop_name);
        delete st;
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

} // namespace

void alpine_spinner_init(HWND hdlg, int idc_edit, int idc_spin, float step, float min_v,
                         float max_v, int decimals)
{
    HWND spin = GetDlgItem(hdlg, idc_spin);
    HWND edit = GetDlgItem(hdlg, idc_edit);
    if (!spin || !edit) return;

    // The spinner never moves itself (UDN_DELTAPOS is refused), so its own range only has to be
    // wide enough that both arrows always report a delta. Accel matches stock: 1 unit, 10 while
    // held.
    SendMessage(spin, UDM_SETRANGE32, static_cast<WPARAM>(-0x10000), static_cast<LPARAM>(0x10000));
    SendMessage(spin, UDM_SETPOS32, 0, 0);
    UDACCEL accel[2] = {{0, 1}, {1, 10}};
    SendMessage(spin, UDM_SETACCEL, 2, reinterpret_cast<LPARAM>(accel));
    SendMessage(spin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(edit), 0);

    auto* st = spinner_state(spin);
    if (!st) {
        st = new SpinnerState();
        if (!SetPropA(spin, spinner_prop_name, st)) {
            delete st;
            return;
        }
        // Without the subclass nothing ever reaches WM_NCDESTROY to free the state, and the prop
        // would outlive this call pointing at it.
        if (!SetWindowSubclass(spin, spinner_subclass_proc, spinner_subclass_id, 0)) {
            RemovePropA(spin, spinner_prop_name);
            delete st;
            return;
        }
    }
    st->edit = edit;
    st->step = std::isfinite(step) && step > 0.0f ? step : 1.0f;
    st->min_v = std::min(min_v, max_v);
    st->max_v = std::max(min_v, max_v);
    st->decimals = std::clamp(decimals, 0, 6);
}

void alpine_spinner_init_int(HWND hdlg, int idc_edit, int idc_spin, int step, int min_v, int max_v)
{
    alpine_spinner_init(hdlg, idc_edit, idc_spin, static_cast<float>(step),
                        static_cast<float>(min_v), static_cast<float>(max_v), 0);
}

bool alpine_spinner_handle_notify(HWND hdlg, LPARAM lp)
{
    auto* nm = reinterpret_cast<NMHDR*>(lp);
    if (!nm || nm->code != UDN_DELTAPOS) return false;
    auto* st = spinner_state(nm->hwndFrom);
    if (!st) return false;

    const int delta = reinterpret_cast<NMUPDOWN*>(lp)->iDelta;
    spinner_write(st, spinner_read(st) + static_cast<float>(delta) * st->step);
    SetWindowLongPtr(hdlg, DWLP_MSGRESULT, 1); // refuse the spinner's own position change
    return true;
}
