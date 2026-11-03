#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <xlog/xlog.h>
#include <common/utils/string-utils.h>
#include "editor_settings.h"
#include "file_dialogs.h"
#include "mfc_types.h"
#include "resources.h"
#include "sound_browser.h"
#include "sound_preview.h"
#include "sounds.h"
#include "vfs_browser.h"

namespace
{

constexpr UINT_PTR player_timer_id = 1;
constexpr UINT player_timer_ms = 30;
constexpr UINT decoded_message = WM_APP + 1;
// Music tracks run to a few megabytes; this leaves room for long ones.
constexpr std::size_t max_sound_file_bytes = 64u * 1024 * 1024;
constexpr int max_volume = 100;

// A decode runs on a thread of its own so a long track does not stall the dialog. The dialog drops its reference
// when the selection moves on, and the thread holds the last one until it finishes.
struct DecodeJob
{
    std::string name;
    std::vector<std::uint8_t> bytes;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> done{false};
    bool failed = false;
    DecodedSound result;
};

struct BrowserState
{
    std::vector<VfsBrowserSource> sources;
    std::vector<VfsBrowserFile> files;
    std::string initial;
    std::string selected;
    std::string result;
    bool rebuilding = false;
    int shown = 0;

    std::shared_ptr<DecodeJob> job;
    DecodedSound sound;
    bool sound_ready = false;
    std::size_t file_size = 0;
    bool play_when_ready = false;
    bool was_playing = false;
    std::int64_t playhead = -1;
    SoundPlayer player;
};

BrowserState* get_state(HWND hdlg)
{
    return reinterpret_cast<BrowserState*>(GetWindowLongPtrA(hdlg, GWLP_USERDATA));
}

bool is_checked(HWND hdlg, int id)
{
    return IsDlgButtonChecked(hdlg, id) == BST_CHECKED;
}

int current_volume()
{
    return std::clamp(g_editor_settings.sound_browser_volume, 0, max_volume);
}

void invalidate_wave(HWND hdlg)
{
    if (HWND wave = GetDlgItem(hdlg, IDC_SOUND_BROWSER_WAVE)) {
        InvalidateRect(wave, nullptr, FALSE);
    }
}

std::string format_time(double seconds)
{
    const auto tenths = static_cast<std::int64_t>(std::clamp(seconds, 0.0, 1.0e9) * 10.0);
    return std::format("{}:{:02}.{}", tenths / 600, tenths / 10 % 60, tenths % 10);
}

const char* channels_label(int channels)
{
    return channels == 1 ? "mono" : channels == 2 ? "stereo" : nullptr;
}

// ─── Info ────────────────────────────────────────────────────────────────────

void update_time(HWND hdlg, const BrowserState& st)
{
    std::string text;
    if (st.sound.pcm) {
        const double rate = st.sound.pcm->sample_rate;
        const double at = st.playhead >= 0 ? static_cast<double>(st.playhead) / rate : 0.0;
        text = format_time(at) + " / " + format_time(st.sound.seconds);
    }
    SetDlgItemTextA(hdlg, IDC_SOUND_BROWSER_TIME, text.c_str());
}

void update_play_button(HWND hdlg, const BrowserState& st)
{
    SetDlgItemTextA(hdlg, IDC_SOUND_BROWSER_PLAY, st.player.playing() ? "&Stop" : "&Play");
    EnableWindow(GetDlgItem(hdlg, IDC_SOUND_BROWSER_PLAY), st.sound.pcm || st.job);
}

void show_sound_info(HWND hdlg, const BrowserState& st)
{
    const DecodedSound& sound = st.sound;
    std::string format;
    std::string length;
    std::string status;
    if (st.job) {
        status = "Loading...";
    }
    else if (st.sound_ready) {
        format = sound.format;
        if (sound.sample_rate > 0) {
            format += std::format(", {} Hz", sound.sample_rate);
            if (sound.bits > 0) {
                format += std::format(", {}-bit", sound.bits);
            }
            const char* channels = channels_label(sound.channels);
            format += channels ? std::format(", {}", channels) : std::format(", {} channels", sound.channels);
        }
        if (sound.seconds > 0.0) {
            length = std::format("{:.2f} s, ", sound.seconds);
        }
        length += std::format("{} KB", (st.file_size + 1023) / 1024);
        status = sound.problem;
        if (status.empty() && sound.truncated && sound.pcm) {
            status = std::format("Only the first {} is previewed",
                                 format_time(static_cast<double>(sound.pcm->frames()) / sound.pcm->sample_rate));
        }
    }
    SetDlgItemTextA(hdlg, IDC_SOUND_BROWSER_NAME, st.selected.c_str());
    SetDlgItemTextA(hdlg, IDC_SOUND_BROWSER_FORMAT, format.c_str());
    SetDlgItemTextA(hdlg, IDC_SOUND_BROWSER_LENGTH, length.c_str());
    SetDlgItemTextA(hdlg, IDC_SOUND_BROWSER_STATUS, status.c_str());
    update_time(hdlg, st);
    update_play_button(hdlg, st);
    invalidate_wave(hdlg);
}

// ─── Playback ────────────────────────────────────────────────────────────────

void start_playback(HWND hdlg, BrowserState& st, std::size_t frame)
{
    if (!st.sound.pcm) {
        return;
    }
    st.player.set_loop(g_editor_settings.sound_browser_loop);
    st.player.set_volume(static_cast<float>(current_volume()) / max_volume);
    if (!st.player.play(st.sound.pcm, frame)) {
        SetDlgItemTextA(hdlg, IDC_SOUND_BROWSER_STATUS, "No sound device is available to preview on");
    }
    st.was_playing = st.player.playing();
    st.playhead = st.player.position();
    update_time(hdlg, st);
    update_play_button(hdlg, st);
    invalidate_wave(hdlg);
}

void stop_playback(HWND hdlg, BrowserState& st)
{
    st.player.stop();
    st.was_playing = false;
    st.playhead = -1;
    update_time(hdlg, st);
    update_play_button(hdlg, st);
    invalidate_wave(hdlg);
}

void toggle_playback(HWND hdlg, BrowserState& st)
{
    if (st.player.playing()) {
        stop_playback(hdlg, st);
    }
    else if (st.sound.pcm) {
        start_playback(hdlg, st, 0);
    }
    else if (st.job) {
        st.play_when_ready = true;
    }
}

void tick_playback(HWND hdlg, BrowserState& st)
{
    if (st.player.playing()) {
        st.player.pump();
    }
    const std::int64_t playhead = st.player.position();
    if (playhead != st.playhead) {
        st.playhead = playhead;
        update_time(hdlg, st);
        invalidate_wave(hdlg);
    }
    if (st.was_playing != st.player.playing()) {
        st.was_playing = st.player.playing();
        update_play_button(hdlg, st);
    }
}

void seek_to_cursor(HWND hdlg, BrowserState& st)
{
    HWND wave = GetDlgItem(hdlg, IDC_SOUND_BROWSER_WAVE);
    if (!wave || !st.sound.pcm) {
        return;
    }
    POINT pt;
    RECT rc;
    if (!GetCursorPos(&pt) || !ScreenToClient(wave, &pt) || !GetClientRect(wave, &rc) || rc.right <= 0) {
        return;
    }
    const std::uint64_t x = std::clamp<LONG>(pt.x, 0, rc.right - 1);
    start_playback(hdlg, st, static_cast<std::size_t>(x * st.sound.pcm->frames() / rc.right));
}

// ─── Decoding ────────────────────────────────────────────────────────────────

void cancel_decode(BrowserState& st)
{
    if (st.job) {
        st.job->cancelled = true;
        st.job.reset();
    }
}

void load_selection(HWND hdlg, BrowserState& st)
{
    cancel_decode(st);
    st.player.stop();
    st.sound = {};
    st.sound_ready = false;
    st.file_size = 0;
    st.was_playing = false;
    st.playhead = -1;
    if (st.selected.empty()) {
        show_sound_info(hdlg, st);
        return;
    }

    auto job = std::make_shared<DecodeJob>();
    job->name = st.selected;
    const int size = vfs_browser_file_size(st.selected);
    st.file_size = std::max(size, 0);
    st.sound_ready = true;
    if (size > 0 && static_cast<std::size_t>(size) > max_sound_file_bytes) {
        st.sound.problem = "The file is too large to preview";
    }
    // The engine's file system is not thread safe, so only the decode runs on the worker.
    else if (size <= 0 || !vfs_browser_read_file(st.selected, max_sound_file_bytes, job->bytes)) {
        st.sound.problem = "The file could not be read";
    }
    else {
        try {
            std::thread{[job, hdlg] {
                try {
                    job->result = sound_decode(job->name, job->bytes, job->cancelled);
                }
                catch (...) {
                    job->failed = true;
                }
                job->bytes = {};
                job->done.store(true, std::memory_order_release);
                if (!job->cancelled.load()) {
                    PostMessageA(hdlg, decoded_message, 0, reinterpret_cast<LPARAM>(job.get()));
                }
            }}.detach();
            st.job = std::move(job);
            st.sound_ready = false;
        }
        catch (...) {
            st.sound.problem = "The preview could not be started";
        }
    }
    show_sound_info(hdlg, st);
}

void on_decoded(HWND hdlg, BrowserState& st)
{
    const std::shared_ptr<DecodeJob> job = std::move(st.job);
    st.sound = job->failed ? DecodedSound{} : std::move(job->result);
    st.sound_ready = true;
    const bool play = st.play_when_ready || g_editor_settings.sound_browser_autoplay;
    st.play_when_ready = false;
    if (job->failed) {
        st.sound.problem = "Out of memory decoding the sound";
    }
    show_sound_info(hdlg, st);
    if (st.sound.pcm && play) {
        start_playback(hdlg, st, 0);
    }
}

// ─── Waveform ────────────────────────────────────────────────────────────────

void draw_wave_contents(HDC dc, const RECT& rc, HFONT font, const BrowserState& st)
{
    FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
    const int width = rc.right;
    const int height = rc.bottom;
    const auto& peaks = st.sound.peaks;
    if (!st.sound.pcm || peaks.empty()) {
        const char* text = st.job ? "Loading..." : (st.sound_ready ? "No preview" : "");
        HGDIOBJ old_font = font ? SelectObject(dc, font) : nullptr;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        RECT text_rc = rc;
        DrawTextA(dc, text, -1, &text_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (old_font) {
            SelectObject(dc, old_font);
        }
        return;
    }

    const int mid = height / 2;
    const int half = std::max(height / 2 - 1, 1);
    HPEN wave_pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_HIGHLIGHT));
    HGDIOBJ old_pen = SelectObject(dc, wave_pen);
    const std::uint64_t count = peaks.size();
    for (int x = 0; x < width; ++x) {
        const std::size_t begin = static_cast<std::size_t>(x * count / width);
        const std::size_t end = std::max(begin + 1, static_cast<std::size_t>((x + 1) * count / width));
        int low = 0;
        int high = 0;
        for (std::size_t i = begin; i < end && i < peaks.size(); ++i) {
            low = std::min<int>(low, peaks[i].first);
            high = std::max<int>(high, peaks[i].second);
        }
        MoveToEx(dc, x, mid - high * half / 32768, nullptr);
        LineTo(dc, x, mid - low * half / 32768 + 1);
    }
    SelectObject(dc, old_pen);
    DeleteObject(wave_pen);

    if (st.playhead >= 0) {
        const std::uint64_t frames = std::max<std::size_t>(st.sound.pcm->frames(), 1);
        const int x = static_cast<int>(static_cast<std::uint64_t>(st.playhead) * width / frames);
        HPEN head_pen = CreatePen(PS_SOLID, 1, RGB(220, 0, 0));
        old_pen = SelectObject(dc, head_pen);
        MoveToEx(dc, x, 0, nullptr);
        LineTo(dc, x, height);
        SelectObject(dc, old_pen);
        DeleteObject(head_pen);
    }
}

// Drawn off screen first: the playhead repaints the whole control many times a second.
void draw_wave(const DRAWITEMSTRUCT& dis, const BrowserState& st)
{
    const int width = dis.rcItem.right - dis.rcItem.left;
    const int height = dis.rcItem.bottom - dis.rcItem.top;
    if (width <= 0 || height <= 0) {
        return;
    }
    HDC mem = CreateCompatibleDC(dis.hDC);
    HBITMAP bitmap = mem ? CreateCompatibleBitmap(dis.hDC, width, height) : nullptr;
    if (!bitmap) {
        if (mem) {
            DeleteDC(mem);
        }
        return;
    }
    HGDIOBJ old_bitmap = SelectObject(mem, bitmap);
    const RECT rc{0, 0, width, height};
    const auto font = reinterpret_cast<HFONT>(SendMessageA(dis.hwndItem, WM_GETFONT, 0, 0));
    draw_wave_contents(mem, rc, font, st);
    BitBlt(dis.hDC, dis.rcItem.left, dis.rcItem.top, width, height, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(mem);
}

// ─── Dialog ──────────────────────────────────────────────────────────────────

void rebuild_tree(HWND hdlg, BrowserState& st, const std::string& select_name)
{
    HWND tree = GetDlgItem(hdlg, IDC_SOUND_BROWSER_TREE);
    if (!tree) {
        return;
    }
    const std::string filter = vfs_browser_filter_text(GetDlgItem(hdlg, IDC_SOUND_BROWSER_FILTER));
    st.shown = vfs_browser_rebuild_tree(tree, st.sources, st.files, nullptr, filter, select_name, false, nullptr,
                                        st.rebuilding);
    const std::string count = std::format("{} of {} sounds", st.shown, st.files.size());
    SetDlgItemTextA(hdlg, IDC_SOUND_BROWSER_COUNT, count.c_str());
}

void accept_selection(HWND hdlg, BrowserState& st)
{
    if (st.selected.empty()) {
        return;
    }
    st.result = st.selected;
    EndDialog(hdlg, IDOK);
}

void on_selection_changed(HWND hdlg, BrowserState& st)
{
    HWND tree = GetDlgItem(hdlg, IDC_SOUND_BROWSER_TREE);
    const int index = vfs_browser_item_file_index(tree, TreeView_GetSelection(tree));
    const bool have_file = index >= 0 && index < static_cast<int>(st.files.size());
    const std::string name = have_file ? st.files[index].name : std::string{};
    EnableWindow(GetDlgItem(hdlg, IDOK), !name.empty());
    const std::string source = have_file ? vfs_browser_source_text(st.sources[st.files[index].source]) : std::string{};
    SetDlgItemTextA(hdlg, IDC_SOUND_BROWSER_SOURCE, source.c_str());
    // The same name in another folder or packfile is the same file to the engine.
    if (name == st.selected) {
        return;
    }
    st.selected = name;
    st.play_when_ready = false;
    load_selection(hdlg, st);
}

INT_PTR sound_browser_message(HWND hdlg, UINT msg, WPARAM wparam, LPARAM lparam)
{
    BrowserState* st = get_state(hdlg);

    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowLongPtrA(hdlg, GWLP_USERDATA, lparam);
        auto& state = *reinterpret_cast<BrowserState*>(lparam);
        alpine_center_dialog_on_owner(hdlg);
        SendDlgItemMessage(hdlg, IDC_SOUND_BROWSER_FILTER, EM_SETLIMITTEXT, 64, 0);
        CheckDlgButton(hdlg, IDC_SOUND_BROWSER_LOOP,
                       g_editor_settings.sound_browser_loop ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_SOUND_BROWSER_AUTOPLAY,
                       g_editor_settings.sound_browser_autoplay ? BST_CHECKED : BST_UNCHECKED);
        SendDlgItemMessage(hdlg, IDC_SOUND_BROWSER_VOLUME, TBM_SETRANGE, TRUE, MAKELPARAM(0, max_volume));
        SendDlgItemMessage(hdlg, IDC_SOUND_BROWSER_VOLUME, TBM_SETPOS, TRUE, current_volume());
        bool listing_failed = false;
        try {
            vfs_browser_collect(state.sources, {&state.files}, [](const std::string& name) {
                const auto ext = get_ext_from_filename(name);
                return (string_iequals(ext, "wav") || string_iequals(ext, "ogg")) ? 0 : -1;
            });
        }
        catch (...) {
            state.sources.clear();
            state.files.clear();
            listing_failed = true;
        }
        rebuild_tree(hdlg, state, state.initial);
        on_selection_changed(hdlg, state);
        show_sound_info(hdlg, state);
        if (listing_failed) {
            xlog::error("[SoundBrowser] Out of memory listing the file system");
            SetDlgItemTextA(hdlg, IDC_SOUND_BROWSER_STATUS, "Out of memory listing sounds");
        }
        SetTimer(hdlg, player_timer_id, player_timer_ms, nullptr);
        SetFocus(GetDlgItem(hdlg, IDC_SOUND_BROWSER_TREE));
        return FALSE;
    }
    case WM_TIMER:
        if (st && wparam == player_timer_id) {
            tick_playback(hdlg, *st);
            return TRUE;
        }
        break;
    case decoded_message:
        // A stale post may carry the address of a freed job that a newer one now reuses; `done` makes that harmless.
        if (st && st->job && reinterpret_cast<DecodeJob*>(lparam) == st->job.get()
            && st->job->done.load(std::memory_order_acquire)) {
            on_decoded(hdlg, *st);
        }
        return TRUE;
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        if (st && dis && dis->CtlID == IDC_SOUND_BROWSER_WAVE) {
            draw_wave(*dis, *st);
            return TRUE;
        }
        break;
    }
    case WM_HSCROLL:
        if (st && reinterpret_cast<HWND>(lparam) == GetDlgItem(hdlg, IDC_SOUND_BROWSER_VOLUME)) {
            g_editor_settings.sound_browser_volume = static_cast<int>(
                SendDlgItemMessage(hdlg, IDC_SOUND_BROWSER_VOLUME, TBM_GETPOS, 0, 0));
            st->player.set_volume(static_cast<float>(current_volume()) / max_volume);
            return TRUE;
        }
        break;
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lparam);
        if (!st || !hdr || hdr->idFrom != IDC_SOUND_BROWSER_TREE) {
            break;
        }
        // Both fall through to the default handler, which expands and collapses folders.
        if (hdr->code == TVN_SELCHANGEDA || hdr->code == TVN_SELCHANGEDW) {
            if (!st->rebuilding) {
                on_selection_changed(hdlg, *st);
            }
        }
        else if (hdr->code == NM_DBLCLK && vfs_browser_dblclk_file_index(hdr->hwndFrom) >= 0) {
            accept_selection(hdlg, *st);
        }
        break;
    }
    case WM_COMMAND: {
        if (!st) {
            break;
        }
        const int id = LOWORD(wparam);
        const int code = HIWORD(wparam);
        switch (id) {
        case IDC_SOUND_BROWSER_FILTER:
            if (code == EN_CHANGE) {
                rebuild_tree(hdlg, *st, st->selected);
                on_selection_changed(hdlg, *st);
                return TRUE;
            }
            break;
        case IDC_SOUND_BROWSER_WAVE:
            if (code == STN_CLICKED || code == STN_DBLCLK) {
                seek_to_cursor(hdlg, *st);
                return TRUE;
            }
            break;
        case IDC_SOUND_BROWSER_PLAY:
            toggle_playback(hdlg, *st);
            return TRUE;
        case IDC_SOUND_BROWSER_LOOP:
            g_editor_settings.sound_browser_loop = is_checked(hdlg, IDC_SOUND_BROWSER_LOOP);
            st->player.set_loop(g_editor_settings.sound_browser_loop);
            return TRUE;
        case IDC_SOUND_BROWSER_AUTOPLAY:
            g_editor_settings.sound_browser_autoplay = is_checked(hdlg, IDC_SOUND_BROWSER_AUTOPLAY);
            if (g_editor_settings.sound_browser_autoplay && !st->player.playing()) {
                start_playback(hdlg, *st, 0);
            }
            return TRUE;
        case IDOK:
            accept_selection(hdlg, *st);
            return TRUE;
        case IDCANCEL:
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        default:
            break;
        }
        break;
    }
    case WM_DESTROY:
        KillTimer(hdlg, player_timer_id);
        if (st) {
            cancel_decode(*st);
            st->player.stop();
        }
        break;
    default:
        break;
    }
    return FALSE;
}

// Nothing the dialog allocates may unwind into USER32's frames, so it is all contained here.
INT_PTR CALLBACK SoundBrowserDialogProc(HWND hdlg, UINT msg, WPARAM wparam, LPARAM lparam)
{
    try {
        return sound_browser_message(hdlg, msg, wparam, lparam);
    }
    catch (...) {
        xlog::error("[SoundBrowser] Out of memory handling message {:#x}", msg);
        return FALSE;
    }
}

bool browse_sound(HWND parent, std::string& filename)
{
    // Files added to user_maps\sounds while the editor runs are listed too.
    reload_custom_sounds();
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    BrowserState state;
    state.initial = filename;
    const INT_PTR result =
        DialogBoxParamA(reinterpret_cast<HINSTANCE>(&__ImageBase), MAKEINTRESOURCEA(IDD_ALPINE_SOUND_BROWSER), parent,
                        SoundBrowserDialogProc, reinterpret_cast<LPARAM>(&state));
    cancel_decode(state);
    state.player.stop();
    if (result == -1) {
        xlog::error("[SoundBrowser] The browser could not be opened ({})", GetLastError());
    }
    if (result == IDOK && !state.result.empty()) {
        filename = state.result;
        return true;
    }
    return false;
}

} // namespace

BOOL alpine_sound_browser_answer(OPENFILENAMEA* ofn)
{
    // No exception may unwind into the RED handler's SEH frames.
    try {
        // The field holds a bare name, though the request carries whatever was typed there.
        const std::string current{ofn->lpstrFile, strnlen(ofn->lpstrFile, ofn->nMaxFile)};
        const std::size_t name_start = current.find_last_of("\\/:");
        std::string filename = name_start == std::string::npos ? current : current.substr(name_start + 1);
        const bool picked = browse_sound(ofn->hwndOwner, filename);
        const bool has_title = ofn->lpstrFileTitle && ofn->nMaxFileTitle;
        const bool fits = picked && filename.size() < ofn->nMaxFile
                          && (!has_title || filename.size() < ofn->nMaxFileTitle);
        if (picked && !fits) {
            xlog::warn("[SoundBrowser] '{}' is too long for the field", filename);
        }
        // The handlers read the name back from lpstrFileTitle. The ambient sound one takes a cancel as OK too, so a
        // cancel hands back the field's own text.
        if (has_title) {
            alpine_file_dialog_copy_bounded((fits ? filename : current).c_str(), ofn->lpstrFileTitle,
                                            ofn->nMaxFileTitle);
        }
        if (!fits) {
            return FALSE;
        }
        alpine_file_dialog_copy_bounded(filename.c_str(), ofn->lpstrFile, ofn->nMaxFile);
        alpine_file_dialog_name_offsets(ofn->lpstrFile, ofn->nFileOffset, ofn->nFileExtension);
        return TRUE;
    }
    catch (...) {
        xlog::error("[SoundBrowser] Out of memory answering a sound file request");
        return FALSE;
    }
}
