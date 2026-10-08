#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>
#include <patch_common/CallHook.h>
#include <xlog/xlog.h>
#include "bake_progress.h"
#include "headless_bake.h"
#include "level.h"
#include "resources.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{

constexpr std::size_t phase_count = static_cast<std::size_t>(BakePhase::count);

struct PhaseInfo
{
    const char* name;
    double fixed_seconds;
    double seconds_per_step;
};

// Rough costs measured on large test levels; the estimate rescales them by how the finished phases compared.
constexpr std::array<PhaseInfo, phase_count> phase_info{{
    {"Preparing the lightmap layout", 2.0, 0.0},
    {"Lighting surfaces", 0.0, 1.8e-3},
    {"Blending surface edges", 0.0, 7.0e-4},
    {"Smoothing surfaces", 0.0, 2.5e-5},
    {"Lighting movers", 0.0, 5.0e-3},
    {"Lighting terrain", 0.0, 5.0},
    {"Lighting overflow faces", 0.0, 1.6e-3},
    {"Encoding lightmaps", 0.0, 1.3e-6},
}};

enum class Status
{
    unlisted,
    pending,
    running,
    done,
    skipped,
};

struct Phase
{
    Status status = Status::unlisted;
    std::uint64_t total = 0;
    std::uint64_t done = 0;
    std::uint64_t expected = 0;
    ULONGLONG start = 0;
    ULONGLONG end = 0;
};

// Scoped by BakeProgressScope.
struct State
{
    bool active = false;
    bool cancelled = false;
    // Cancel was pressed once, at tick `armed_at`; a second, separate press confirms it
    bool armed = false;
    DWORD armed_at = 0;
    std::vector<std::pair<std::string, std::string>> deferred;
    HWND dlg = nullptr;
    std::vector<HWND> disabled;
    std::array<Phase, phase_count> phases{};
    // the status and time cells as last set, so unchanged cells are not redrawn
    std::array<std::array<std::string, 2>, phase_count> cells{};
    int current = -1;
    ULONGLONG start = 0;
    ULONGLONG last_refresh = 0;
};
State g_progress;

// Posted by the taskbar button's shift-right-click.
constexpr UINT wm_popup_system_menu = 0x0313;
constexpr DWORD confirm_window_ms = 4000;
constexpr const char* confirm_text = "Press Confirm Cancel to discard the bake and leave the level unlit";

std::size_t index_of(BakePhase phase)
{
    return static_cast<std::size_t>(phase);
}

double seconds_between(ULONGLONG from, ULONGLONG to)
{
    return to > from ? static_cast<double>(to - from) / 1000.0 : 0.0;
}

std::string format_duration(double seconds)
{
    const auto s = static_cast<unsigned>(std::max(seconds, 0.0) + 0.5);
    char buf[32];
    if (s >= 3600) {
        std::snprintf(buf, sizeof(buf), "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    }
    else {
        std::snprintf(buf, sizeof(buf), "%u:%02u", s / 60, s % 60);
    }
    return buf;
}

double phase_estimate(std::size_t i)
{
    const Phase& p = g_progress.phases[i];
    const std::uint64_t steps = p.status == Status::pending ? p.expected : p.total;
    return phase_info[i].fixed_seconds + phase_info[i].seconds_per_step * static_cast<double>(steps);
}

double phase_fraction(const Phase& p)
{
    if (p.status == Status::done) {
        return 1.0;
    }
    return p.total ? std::min(1.0, static_cast<double>(p.done) / static_cast<double>(p.total)) : 0.0;
}

// The seconds left, negative while there is too little to go on; `overall` gets the fraction done.
double remaining_seconds(ULONGLONG now, double& overall)
{
    double done_actual = 0.0;
    double done_estimate = 0.0;
    // calibrated by the phases measured in steps; the layout's fixed guess says little about the rest
    for (std::size_t i = 0; i < phase_count; i++) {
        const Phase& p = g_progress.phases[i];
        if (p.status == Status::done && phase_info[i].seconds_per_step > 0.0) {
            done_actual += seconds_between(p.start, p.end);
            done_estimate += phase_estimate(i);
        }
    }
    const double scale = done_estimate > 0.5 ? done_actual / done_estimate : 1.0;
    double left = 0.0;
    double total = done_actual;
    bool known = done_estimate > 0.5;
    if (g_progress.current >= 0) {
        const auto i = static_cast<std::size_t>(g_progress.current);
        const Phase& p = g_progress.phases[i];
        const double elapsed = seconds_between(p.start, now);
        const double f = phase_fraction(p);
        double expected = std::max(elapsed, phase_estimate(i) * scale);
        if (f > 0.02 && elapsed > 2.0) {
            expected = elapsed / f;
            known = true;
        }
        left += expected - elapsed;
        total += expected;
    }
    for (std::size_t i = 0; i < phase_count; i++) {
        if (g_progress.phases[i].status == Status::pending) {
            const double expected = phase_estimate(i) * scale;
            left += expected;
            total += expected;
        }
    }
    overall = total > 0.0 ? std::clamp((total - left) / total, 0.0, 1.0) : 0.0;
    return known ? left : -1.0;
}

// The phase's row: listed phases appear in phase order.
int row_of(std::size_t i)
{
    int row = 0;
    for (std::size_t k = 0; k < i; k++) {
        row += g_progress.phases[k].status != Status::unlisted ? 1 : 0;
    }
    return row;
}

void set_cell(HWND list, int row, int column, const char* text)
{
    LVITEMA item{};
    item.iSubItem = column;
    item.pszText = const_cast<char*>(text);
    SendMessageA(list, LVM_SETITEMTEXTA, row, reinterpret_cast<LPARAM>(&item));
}

void insert_row(HWND list, std::size_t i)
{
    LVITEMA item{};
    item.mask = LVIF_TEXT;
    item.iItem = row_of(i);
    item.pszText = const_cast<char*>(phase_info[i].name);
    SendMessageA(list, LVM_INSERTITEMA, 0, reinterpret_cast<LPARAM>(&item));
}

void update_rows(ULONGLONG now)
{
    HWND list = GetDlgItem(g_progress.dlg, IDC_BAKE_PHASES);
    for (std::size_t i = 0; i < phase_count; i++) {
        const Phase& p = g_progress.phases[i];
        const char* status = "";
        std::string time;
        switch (p.status) {
        case Status::unlisted:
            continue;
        case Status::pending:
            status = g_progress.cancelled ? "cancelled" : "";
            break;
        case Status::running:
            status = g_progress.cancelled ? "cancelling" : "running";
            time = format_duration(seconds_between(p.start, now));
            break;
        case Status::done:
            status = "done";
            time = format_duration(seconds_between(p.start, p.end));
            break;
        case Status::skipped:
            status = "not needed";
            break;
        }
        auto& cells = g_progress.cells[i];
        if (cells[0] != status || cells[1] != time) {
            const int row = row_of(i);
            set_cell(list, row, 1, status);
            set_cell(list, row, 2, time.c_str());
            cells = {status, time};
        }
    }
}

// The list only shows the phases; a click on it or its scroll bar would start a capture loop that dispatches every
// window's messages.
bool list_click(const MSG& msg)
{
    const bool click = (msg.message >= WM_NCLBUTTONDOWN && msg.message <= WM_NCMBUTTONDBLCLK) ||
                       (msg.message >= WM_LBUTTONDOWN && msg.message <= WM_MBUTTONDBLCLK) ||
                       (msg.message >= WM_XBUTTONDOWN && msg.message <= WM_XBUTTONDBLCLK);
    if (!click) {
        return false;
    }
    HWND list = GetDlgItem(g_progress.dlg, IDC_BAKE_PHASES);
    return msg.hwnd == list || IsChild(list, msg.hwnd);
}

void pump_messages()
{
    MSG msg;
    // messages sent from other threads (WM_QUERYENDSESSION among them) wait for the bake; the dialog keeps every
    // modal loop that would dispatch them out of reach, but for holding down its close button
    constexpr UINT flags = PM_REMOVE | PM_QS_INPUT | PM_QS_PAINT | PM_QS_POSTMESSAGE;
    while (g_progress.dlg && PeekMessageA(&msg, g_progress.dlg, 0, 0, flags)) {
        // a held key repeats, and must not confirm the Cancel it armed
        const bool repeat =
            (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) && (HIWORD(msg.lParam) & KF_REPEAT);
        if (repeat || list_click(msg)) {
            continue;
        }
        if (!IsDialogMessageA(g_progress.dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
}

void refresh_window(bool force)
{
    if (!g_progress.dlg) {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (!force && now - g_progress.last_refresh < 100) {
        return;
    }
    g_progress.last_refresh = now;
    // the stock command re-enables its main frame before the alpine phases run
    for (HWND wnd : g_progress.disabled) {
        if (IsWindowEnabled(wnd)) {
            EnableWindow(wnd, FALSE);
        }
    }
    update_rows(now);
    if (g_progress.armed && GetTickCount() - g_progress.armed_at >= confirm_window_ms) {
        g_progress.armed = false;
        SetDlgItemTextA(g_progress.dlg, IDCANCEL, "Cancel");
    }
    std::string phase_text;
    double f = 0.0;
    if (g_progress.cancelled) {
        phase_text = "Cancelling...";
    }
    else if (g_progress.armed) {
        phase_text = confirm_text;
    }
    else if (g_progress.current >= 0) {
        const auto i = static_cast<std::size_t>(g_progress.current);
        const Phase& p = g_progress.phases[i];
        phase_text = phase_info[i].name;
        if (p.total) {
            phase_text += "   " + std::to_string(std::min(p.done, p.total)) + " of " + std::to_string(p.total);
        }
        f = phase_fraction(p);
    }
    SetDlgItemTextA(g_progress.dlg, IDC_BAKE_PHASE_TEXT, phase_text.c_str());
    SendDlgItemMessageA(g_progress.dlg, IDC_BAKE_PHASE_BAR, PBM_SETPOS, static_cast<WPARAM>(f * 1000.0), 0);
    double overall = 0.0;
    const double left = remaining_seconds(now, overall);
    SendDlgItemMessageA(g_progress.dlg, IDC_BAKE_OVERALL_BAR, PBM_SETPOS, static_cast<WPARAM>(overall * 1000.0), 0);
    const std::string time_text = "Elapsed " + format_duration(seconds_between(g_progress.start, now)) +
                                  "      Remaining " +
                                  (left < 0.0 || g_progress.cancelled ? std::string{"..."}
                                                                      : "about " + format_duration(left));
    SetDlgItemTextA(g_progress.dlg, IDC_BAKE_TIME_TEXT, time_text.c_str());
    pump_messages();
}

// Called from the bake's hooks, so nothing may unwind into the engine's frames.
void refresh(bool force)
{
    try {
        refresh_window(force);
    }
    catch (...) {
    }
}

// The first press arms Cancel, a second within a few seconds confirms it. A message box would run a modal loop
// that dispatches every window's messages in the middle of the stock passes.
void press_cancel(HWND hdlg)
{
    if (g_progress.cancelled) {
        return;
    }
    const DWORD now = GetTickCount();
    const DWORD since = now - g_progress.armed_at;
    // a double click is one press
    if (g_progress.armed && since < 700) {
        return;
    }
    if (!g_progress.armed || since >= confirm_window_ms) {
        g_progress.armed = true;
        g_progress.armed_at = now;
        SetDlgItemTextA(hdlg, IDCANCEL, "Confirm Cancel");
        SetDlgItemTextA(hdlg, IDC_BAKE_PHASE_TEXT, confirm_text);
        return;
    }
    g_progress.cancelled = true;
    g_progress.armed = false;
    SetDlgItemTextA(hdlg, IDCANCEL, "Cancel");
    EnableWindow(GetDlgItem(hdlg, IDCANCEL), FALSE);
    try {
        xlog::info("Lightmap: Calculate Lighting cancelled");
    }
    catch (...) {
    }
}

INT_PTR CALLBACK progress_proc(HWND hdlg, UINT msg, WPARAM wparam, LPARAM)
{
    switch (msg) {
    case WM_INITDIALOG:
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wparam) == IDCANCEL) {
            press_cancel(hdlg);
            return TRUE;
        }
        break;
    case WM_CONTEXTMENU:
    case wm_popup_system_menu:
        // the system menu runs a modal loop
        return TRUE;
    case WM_HELP:
        // the owner's help handler can open a message box
        return TRUE;
    case WM_SYSCOMMAND:
        // moving the window and its system menu run modal loops too
        switch (wparam & 0xfff0) {
        case SC_CLOSE:
            press_cancel(hdlg);
            return TRUE;
        case SC_MOVE:
        case SC_KEYMENU:
        case SC_MOUSEMENU:
            return TRUE;
        default:
            break;
        }
        break;
    default:
        break;
    }
    return FALSE;
}

BOOL CALLBACK disable_window(HWND wnd, LPARAM)
{
    if (IsWindowVisible(wnd) && IsWindowEnabled(wnd)) {
        try {
            g_progress.disabled.push_back(wnd);
        }
        catch (...) {
            return FALSE;
        }
        EnableWindow(wnd, FALSE);
    }
    return TRUE;
}

void open_window()
{
    // whatever is open beside the main frame, so a message handler can reach no editor command mid-bake
    EnumThreadWindows(GetCurrentThreadId(), disable_window, 0);
    g_progress.dlg = CreateDialogParamA(reinterpret_cast<HINSTANCE>(&__ImageBase),
                                        MAKEINTRESOURCEA(IDD_ALPINE_BAKE_PROGRESS), GetMainFrameHandle(),
                                        progress_proc, 0);
    if (!g_progress.dlg) {
        xlog::error("Lightmap: the Calculate Lighting progress window could not be created ({})", GetLastError());
        return;
    }
    HWND list = GetDlgItem(g_progress.dlg, IDC_BAKE_PHASES);
    SendMessageA(list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
    RECT rc{};
    GetClientRect(list, &rc);
    const int width = rc.right - rc.left;
    const int columns[3] = {width * 60 / 100, width * 22 / 100, width - width * 60 / 100 - width * 22 / 100};
    const char* titles[3] = {"Phase", "Status", "Time"};
    for (int c = 0; c < 3; c++) {
        LVCOLUMNA col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.cx = columns[c];
        col.pszText = const_cast<char*>(titles[c]);
        col.iSubItem = c;
        SendMessageA(list, LVM_INSERTCOLUMNA, c, reinterpret_cast<LPARAM>(&col));
    }
    for (std::size_t i = 0; i < phase_count; i++) {
        if (g_progress.phases[i].status != Status::unlisted) {
            insert_row(list, i);
        }
    }
    SendDlgItemMessageA(g_progress.dlg, IDC_BAKE_PHASE_BAR, PBM_SETRANGE32, 0, 1000);
    SendDlgItemMessageA(g_progress.dlg, IDC_BAKE_OVERALL_BAR, PBM_SETRANGE32, 0, 1000);
    ShowWindow(g_progress.dlg, SW_SHOW);
    SetForegroundWindow(g_progress.dlg);
    refresh(true);
}

void close_window()
{
    for (HWND wnd : g_progress.disabled) {
        EnableWindow(wnd, TRUE);
    }
    if (g_progress.dlg) {
        // the owner is enabled first, so the activation goes back to it rather than to another application
        DestroyWindow(g_progress.dlg);
        g_progress.dlg = nullptr;
    }
}

void finish_current()
{
    if (g_progress.current < 0) {
        return;
    }
    const auto i = static_cast<std::size_t>(g_progress.current);
    Phase& p = g_progress.phases[i];
    p.status = Status::done;
    p.end = GetTickCount64();
    g_progress.current = -1;
    if (headless_bake_active()) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "phase: %s, %llu steps in %.1fs", phase_info[i].name,
                      static_cast<unsigned long long>(p.done), seconds_between(p.start, p.end));
        try {
            headless_bake_note(buf);
        }
        catch (...) {
        }
    }
}

// The stock Calculate Lighting commands show their own "please wait" dialog over the progress window.
using ShowWindowCall = BOOL __fastcall(void* wnd, int edx, int cmd);
BOOL __fastcall stock_wait_dialog_show(void* wnd, int edx, int cmd);
CallHook<ShowWindowCall> stock_wait_dialog_show_hook{{0x00448f6b, 0x0044931b}, stock_wait_dialog_show};
BOOL __fastcall stock_wait_dialog_show(void* wnd, int edx, int cmd)
{
    return stock_wait_dialog_show_hook.call_target(wnd, edx, g_progress.dlg ? SW_HIDE : cmd);
}

using SetFocusCall = void* __fastcall(void* wnd, int edx);
void* __fastcall stock_wait_dialog_focus(void* wnd, int edx);
CallHook<SetFocusCall> stock_wait_dialog_focus_hook{{0x00448f8f, 0x0044933f}, stock_wait_dialog_focus};
void* __fastcall stock_wait_dialog_focus(void* wnd, int edx)
{
    return g_progress.dlg ? nullptr : stock_wait_dialog_focus_hook.call_target(wnd, edx);
}

} // namespace

BakeProgressScope::BakeProgressScope(unsigned phases)
{
    if (g_progress.active) {
        return;
    }
    owner_ = true;
    g_progress = State{};
    g_progress.active = true;
    g_progress.start = GetTickCount64();
    for (std::size_t i = 0; i < phase_count; i++) {
        if (phases & (1u << i)) {
            g_progress.phases[i].status = Status::pending;
        }
    }
    if (!headless_bake_active()) {
        try {
            open_window();
        }
        catch (const std::bad_alloc&) {
            close_window();
        }
    }
}

BakeProgressScope::~BakeProgressScope()
{
    if (!owner_) {
        return;
    }
    auto deferred = std::move(g_progress.deferred);
    try {
        finish_current();
        if (headless_bake_active()) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "phases took %.1fs", seconds_between(g_progress.start, GetTickCount64()));
            headless_bake_note(buf);
        }
    }
    catch (...) {
    }
    close_window();
    g_progress = State{};
    for (const auto& [caption, msg] : deferred) {
        MessageBoxA(GetMainFrameHandle(), msg.c_str(), caption.c_str(), MB_OK | MB_ICONWARNING);
    }
}

void bake_progress_phase(BakePhase phase, std::uint64_t total)
{
    const auto i = index_of(phase);
    if (!g_progress.active || i >= phase_count || g_progress.current == static_cast<int>(i)) {
        return;
    }
    finish_current();
    Phase& p = g_progress.phases[i];
    if (p.status == Status::unlisted && g_progress.dlg) {
        p.status = Status::pending;
        insert_row(GetDlgItem(g_progress.dlg, IDC_BAKE_PHASES), i);
    }
    p.status = Status::running;
    p.total = total == bake_progress_expected_steps ? p.expected : total;
    p.done = 0;
    p.start = GetTickCount64();
    g_progress.current = static_cast<int>(i);
    refresh(true);
}

void bake_progress_expect(BakePhase phase, std::uint64_t total)
{
    const auto i = index_of(phase);
    if (g_progress.active && i < phase_count) {
        g_progress.phases[i].expected = total;
    }
}

void bake_progress_skip(BakePhase phase)
{
    const auto i = index_of(phase);
    if (g_progress.active && i < phase_count && g_progress.phases[i].status == Status::pending) {
        g_progress.phases[i].status = Status::skipped;
        refresh(true);
    }
}

void bake_progress_step(std::uint64_t steps)
{
    if (!g_progress.active || g_progress.current < 0) {
        return;
    }
    g_progress.phases[static_cast<std::size_t>(g_progress.current)].done += steps;
    refresh(false);
}

bool bake_progress_active()
{
    return g_progress.active;
}

BakePhase bake_progress_current()
{
    return g_progress.current < 0 ? BakePhase::count : static_cast<BakePhase>(g_progress.current);
}

bool bake_progress_cancelled()
{
    return g_progress.cancelled;
}

void bake_progress_defer_message(const char* caption, const std::string& msg)
{
    try {
        g_progress.deferred.emplace_back(caption, msg);
    }
    catch (...) {
    }
}

void ApplyBakeProgressPatches()
{
    stock_wait_dialog_show_hook.install();
    stock_wait_dialog_focus_hook.install();
}
