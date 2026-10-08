#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include "alpine_color_picker.h"
#include "mfc_types.h"
#include "resources.h"

namespace
{

constexpr int palette_slot_count = 16;
constexpr int preset_columns = 16;
constexpr int preset_rows = 3;

// The 48 basic colours of the stock ChooseColor dialog.
const COLORREF preset_colors[preset_columns * preset_rows] = {
    RGB(0xFF, 0x80, 0x80), RGB(0xFF, 0xFF, 0x80), RGB(0x80, 0xFF, 0x80), RGB(0x00, 0xFF, 0x80),
    RGB(0x80, 0xFF, 0xFF), RGB(0x00, 0x80, 0xFF), RGB(0xFF, 0x80, 0xC0), RGB(0xFF, 0x80, 0xFF),
    RGB(0xFF, 0x00, 0x00), RGB(0xFF, 0xFF, 0x00), RGB(0x80, 0xFF, 0x00), RGB(0x00, 0xFF, 0x40),
    RGB(0x00, 0xFF, 0xFF), RGB(0x00, 0x80, 0xC0), RGB(0x80, 0x80, 0xC0), RGB(0xFF, 0x00, 0xFF),

    RGB(0x80, 0x40, 0x40), RGB(0xFF, 0x80, 0x40), RGB(0x00, 0xFF, 0x00), RGB(0x00, 0x80, 0x80),
    RGB(0x00, 0x40, 0x80), RGB(0x80, 0x80, 0xFF), RGB(0x80, 0x00, 0x40), RGB(0xFF, 0x00, 0x80),
    RGB(0x80, 0x00, 0x00), RGB(0xFF, 0x80, 0x00), RGB(0x00, 0x80, 0x00), RGB(0x00, 0x80, 0x40),
    RGB(0x00, 0x00, 0xFF), RGB(0x00, 0x00, 0xA0), RGB(0x80, 0x00, 0x80), RGB(0x80, 0x00, 0xFF),

    RGB(0x40, 0x00, 0x00), RGB(0x80, 0x40, 0x00), RGB(0x00, 0x40, 0x00), RGB(0x00, 0x40, 0x40),
    RGB(0x00, 0x00, 0x80), RGB(0x00, 0x00, 0x40), RGB(0x40, 0x00, 0x40), RGB(0x40, 0x00, 0x80),
    RGB(0x00, 0x00, 0x00), RGB(0x80, 0x80, 0x00), RGB(0x80, 0x80, 0x40), RGB(0x80, 0x80, 0x80),
    RGB(0x40, 0x80, 0x80), RGB(0xC0, 0xC0, 0xC0), RGB(0x40, 0x00, 0x40), RGB(0xFF, 0xFF, 0xFF),
};

struct ColorSurface
{
    HBITMAP bitmap;
    uint32_t* bits;
    int width;
    int height;
};

struct ColorPickerState
{
    COLORREF initial;
    COLORREF current;
    double hue;
    double sat;
    double val;
    COLORREF* custom;
    ColorSurface sv;
    double sv_hue;
    ColorSurface hue_bar;
    bool drag_sv;
    bool drag_hue;
    bool sampling;
    COLORREF sample_color;
};

// Each subclassed surface keeps its own original proc in its window data rather than sharing one.
WNDPROC surface_orig_wndproc(HWND ctl)
{
    return reinterpret_cast<WNDPROC>(GetWindowLongPtrA(ctl, GWLP_USERDATA));
}

LRESULT surface_default(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    WNDPROC orig = surface_orig_wndproc(hwnd);
    return orig ? CallWindowProcA(orig, hwnd, msg, wparam, lparam)
                : DefWindowProcA(hwnd, msg, wparam, lparam);
}

uint32_t to_dib_pixel(COLORREF color)
{
    return (static_cast<uint32_t>(GetRValue(color)) << 16) |
           (static_cast<uint32_t>(GetGValue(color)) << 8) |
           static_cast<uint32_t>(GetBValue(color));
}

void surface_free(ColorSurface& surface)
{
    if (surface.bitmap) {
        DeleteObject(surface.bitmap);
    }
    surface.bitmap = nullptr;
    surface.bits = nullptr;
    surface.width = 0;
    surface.height = 0;
}

bool surface_resize(ColorSurface& surface, int width, int height)
{
    if (width <= 0 || height <= 0) {
        return false;
    }
    surface_free(surface);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        return false;
    }
    surface.bitmap = bitmap;
    surface.bits = static_cast<uint32_t*>(bits);
    surface.width = width;
    surface.height = height;
    return true;
}

void build_sv_surface(ColorPickerState& state)
{
    const int width = state.sv.width;
    const int height = state.sv.height;
    for (int y = 0; y < height; ++y) {
        const double v = height > 1 ? 1.0 - static_cast<double>(y) / (height - 1) : 1.0;
        uint32_t* row = state.sv.bits + static_cast<size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            const double s = width > 1 ? static_cast<double>(x) / (width - 1) : 0.0;
            row[x] = to_dib_pixel(alpine_hsv_to_rgb(state.hue, s, v));
        }
    }
    state.sv_hue = state.hue;
}

void build_hue_surface(ColorSurface& surface)
{
    for (int y = 0; y < surface.height; ++y) {
        const uint32_t pixel = to_dib_pixel(
            alpine_hsv_to_rgb(360.0 * y / surface.height, 1.0, 1.0));
        uint32_t* row = surface.bits + static_cast<size_t>(y) * surface.width;
        for (int x = 0; x < surface.width; ++x) {
            row[x] = pixel;
        }
    }
}

bool ensure_sv_surface(ColorPickerState& state, int width, int height)
{
    bool rebuild = false;
    if (!state.sv.bitmap || state.sv.width != width || state.sv.height != height) {
        if (!surface_resize(state.sv, width, height)) {
            return false;
        }
        rebuild = true;
    }
    if (rebuild || state.sv_hue != state.hue) {
        build_sv_surface(state);
    }
    return true;
}

bool ensure_hue_surface(ColorPickerState& state, int width, int height)
{
    if (!state.hue_bar.bitmap || state.hue_bar.width != width || state.hue_bar.height != height) {
        if (!surface_resize(state.hue_bar, width, height)) {
            return false;
        }
        build_hue_surface(state.hue_bar);
    }
    return true;
}

void blit_surface(HDC hdc, const ColorSurface& surface)
{
    HDC mem_dc = CreateCompatibleDC(hdc);
    if (!mem_dc) {
        return;
    }
    HGDIOBJ old_bitmap = SelectObject(mem_dc, surface.bitmap);
    BitBlt(hdc, 0, 0, surface.width, surface.height, mem_dc, 0, 0, SRCCOPY);
    SelectObject(mem_dc, old_bitmap);
    DeleteDC(mem_dc);
}

void fill_solid(HDC hdc, const RECT& rect, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    if (!brush) {
        return;
    }
    FillRect(hdc, &rect, brush);
    DeleteObject(brush);
}

void draw_sv_marker(HDC hdc, int x, int y)
{
    HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    HPEN dark = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    HPEN light = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HGDIOBJ old_pen = SelectObject(hdc, dark);
    Ellipse(hdc, x - 5, y - 5, x + 6, y + 6);
    SelectObject(hdc, light);
    Ellipse(hdc, x - 4, y - 4, x + 5, y + 5);
    SelectObject(hdc, old_pen);
    SelectObject(hdc, old_brush);
    DeleteObject(dark);
    DeleteObject(light);
}

void draw_hue_marker(HDC hdc, int width, int y)
{
    RECT outer = {0, y - 2, width, y + 3};
    FrameRect(hdc, &outer, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    RECT inner = {1, y - 1, width - 1, y + 2};
    FrameRect(hdc, &inner, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
}

void paint_sv_field(ColorPickerState& state, HWND ctl, HDC hdc)
{
    RECT rect;
    GetClientRect(ctl, &rect);
    if (!ensure_sv_surface(state, static_cast<int>(rect.right), static_cast<int>(rect.bottom))) {
        FillRect(hdc, &rect, GetSysColorBrush(COLOR_BTNFACE));
        return;
    }
    blit_surface(hdc, state.sv);
    const int x = rect.right > 1 ? static_cast<int>(state.sat * (rect.right - 1) + 0.5) : 0;
    const int y = rect.bottom > 1 ? static_cast<int>((1.0 - state.val) * (rect.bottom - 1) + 0.5) : 0;
    draw_sv_marker(hdc, x, y);
}

void paint_hue_bar(ColorPickerState& state, HWND ctl, HDC hdc)
{
    RECT rect;
    GetClientRect(ctl, &rect);
    if (!ensure_hue_surface(state, static_cast<int>(rect.right), static_cast<int>(rect.bottom))) {
        FillRect(hdc, &rect, GetSysColorBrush(COLOR_BTNFACE));
        return;
    }
    blit_surface(hdc, state.hue_bar);
    const int height = static_cast<int>(rect.bottom);
    const int y = std::clamp(static_cast<int>(state.hue * height / 360.0), 0, height - 1);
    draw_hue_marker(hdc, static_cast<int>(rect.right), y);
}

void paint_compare(const ColorPickerState& state, HWND ctl, HDC hdc)
{
    RECT rect;
    GetClientRect(ctl, &rect);
    const int mid = rect.right / 2;
    RECT left = {0, 0, mid, rect.bottom};
    RECT right = {mid, 0, rect.right, rect.bottom};
    fill_solid(hdc, left, state.initial);
    fill_solid(hdc, right, state.current);
    FrameRect(hdc, &rect, GetSysColorBrush(COLOR_BTNSHADOW));
}

void paint_swatches(HDC hdc, const RECT& rect, const COLORREF* colors, int columns, int rows)
{
    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            RECT cell = {rect.right * column / columns, rect.bottom * row / rows,
                         rect.right * (column + 1) / columns, rect.bottom * (row + 1) / rows};
            fill_solid(hdc, cell, colors[row * columns + column] & 0x00FFFFFF);
            FrameRect(hdc, &cell, GetSysColorBrush(COLOR_BTNSHADOW));
        }
    }
}

// -1 when the control has no area to divide.
int swatch_index(const RECT& rect, POINT pt, int columns, int rows)
{
    if (rect.right <= 0 || rect.bottom <= 0) {
        return -1;
    }
    const int column = std::clamp<int>(pt.x * columns / rect.right, 0, columns - 1);
    const int row = std::clamp<int>(pt.y * rows / rect.bottom, 0, rows - 1);
    return row * columns + column;
}

void paint_palette(const ColorPickerState& state, HWND ctl, HDC hdc)
{
    RECT rect;
    GetClientRect(ctl, &rect);
    if (!state.custom) {
        FillRect(hdc, &rect, GetSysColorBrush(COLOR_BTNFACE));
        return;
    }
    paint_swatches(hdc, rect, state.custom, palette_slot_count, 1);
}

void paint_presets(HWND ctl, HDC hdc)
{
    RECT rect;
    GetClientRect(ctl, &rect);
    paint_swatches(hdc, rect, preset_colors, preset_columns, preset_rows);
}

void paint_sample(const ColorPickerState& state, HWND ctl, HDC hdc)
{
    RECT rect;
    GetClientRect(ctl, &rect);
    fill_solid(hdc, rect, state.sampling ? state.sample_color : state.current);
    FrameRect(hdc, &rect, GetSysColorBrush(COLOR_BTNSHADOW));
}

void refresh_fields(HWND hdlg, const ColorPickerState& state)
{
    SetDlgItemInt(hdlg, IDC_COLORPICK_RED, GetRValue(state.current), FALSE);
    SetDlgItemInt(hdlg, IDC_COLORPICK_GREEN, GetGValue(state.current), FALSE);
    SetDlgItemInt(hdlg, IDC_COLORPICK_BLUE, GetBValue(state.current), FALSE);
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", GetRValue(state.current),
                  GetGValue(state.current), GetBValue(state.current));
    SetDlgItemTextA(hdlg, IDC_COLORPICK_HEX, buffer);
}

void invalidate_surface(HWND hdlg, int id)
{
    if (HWND ctl = GetDlgItem(hdlg, id)) {
        InvalidateRect(ctl, nullptr, FALSE);
    }
}

void refresh_all(HWND hdlg, const ColorPickerState& state)
{
    refresh_fields(hdlg, state);
    invalidate_surface(hdlg, IDC_COLORPICK_SV_FIELD);
    invalidate_surface(hdlg, IDC_COLORPICK_HUE_BAR);
    invalidate_surface(hdlg, IDC_COLORPICK_COMPARE);
    invalidate_surface(hdlg, IDC_COLORPICK_PALETTE);
    invalidate_surface(hdlg, IDC_COLORPICK_SAMPLE);
}

// Greys carry no hue, so keep the hue bar where the user left it instead of snapping to red.
void set_current_from_rgb(ColorPickerState& state, COLORREF color)
{
    double h;
    double s;
    double v;
    alpine_rgb_to_hsv(color & 0x00FFFFFF, h, s, v);
    if (s > 0.0) {
        state.hue = h;
    }
    state.sat = s;
    state.val = v;
    state.current = color & 0x00FFFFFF;
}

void set_current_from_hsv(ColorPickerState& state)
{
    state.current = alpine_hsv_to_rgb(state.hue, state.sat, state.val);
}

bool commit_rgb_fields(HWND hdlg, ColorPickerState& state)
{
    static const int ids[3] = {IDC_COLORPICK_RED, IDC_COLORPICK_GREEN, IDC_COLORPICK_BLUE};
    int values[3] = {};
    char buffer[32];
    for (int i = 0; i < 3; ++i) {
        GetDlgItemTextA(hdlg, ids[i], buffer, static_cast<int>(sizeof(buffer)));
        if (!alpine_color_parse_component(buffer, values[i])) {
            refresh_fields(hdlg, state);
            return false;
        }
    }
    const COLORREF color = RGB(values[0], values[1], values[2]);
    if (color != state.current) {
        set_current_from_rgb(state, color);
    }
    refresh_all(hdlg, state);
    return true;
}

bool commit_hex_field(HWND hdlg, ColorPickerState& state)
{
    char buffer[32];
    GetDlgItemTextA(hdlg, IDC_COLORPICK_HEX, buffer, static_cast<int>(sizeof(buffer)));
    COLORREF color = 0;
    if (!alpine_color_parse_hex(buffer, color)) {
        refresh_fields(hdlg, state);
        return false;
    }
    if (color != state.current) {
        set_current_from_rgb(state, color);
    }
    refresh_all(hdlg, state);
    return true;
}

void show_sample_controls(HWND hdlg, bool visible)
{
    const int mode = visible ? SW_SHOW : SW_HIDE;
    if (HWND ctl = GetDlgItem(hdlg, IDC_COLORPICK_SAMPLE)) {
        ShowWindow(ctl, mode);
    }
    if (HWND ctl = GetDlgItem(hdlg, IDC_COLORPICK_SAMPLE_INFO)) {
        ShowWindow(ctl, mode);
    }
}

void sample_screen_under_cursor(HWND hdlg, ColorPickerState& state)
{
    POINT pt;
    if (!GetCursorPos(&pt)) {
        return;
    }
    HDC screen_dc = GetDC(nullptr);
    if (!screen_dc) {
        return;
    }
    const COLORREF color = GetPixel(screen_dc, pt.x, pt.y);
    ReleaseDC(nullptr, screen_dc);
    if (color == CLR_INVALID) {
        return;
    }
    state.sample_color = color & 0x00FFFFFF;
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%ld,%ld  #%02X%02X%02X", pt.x, pt.y,
                  GetRValue(state.sample_color), GetGValue(state.sample_color),
                  GetBValue(state.sample_color));
    SetDlgItemTextA(hdlg, IDC_COLORPICK_SAMPLE_INFO, buffer);
    invalidate_surface(hdlg, IDC_COLORPICK_SAMPLE);
}

void sampling_begin(HWND hdlg, ColorPickerState& state)
{
    if (state.sampling) {
        return;
    }
    state.sampling = true;
    state.sample_color = state.current;
    show_sample_controls(hdlg, true);
    SetCapture(hdlg);
    SetCursor(LoadCursorA(nullptr, IDC_CROSS));
    sample_screen_under_cursor(hdlg, state);
}

void sampling_end(HWND hdlg, ColorPickerState& state, bool accept)
{
    if (!state.sampling) {
        return;
    }
    state.sampling = false;
    if (GetCapture() == hdlg) {
        ReleaseCapture();
    }
    SetDlgItemTextA(hdlg, IDC_COLORPICK_SAMPLE_INFO, "");
    show_sample_controls(hdlg, false);
    if (accept) {
        set_current_from_rgb(state, state.sample_color);
    }
    refresh_all(hdlg, state);
}

ColorPickerState* get_state(HWND hdlg)
{
    return reinterpret_cast<ColorPickerState*>(GetWindowLongPtrA(hdlg, GWLP_USERDATA));
}

void update_sv_from_point(ColorPickerState& state, HWND ctl, POINT pt)
{
    RECT rect;
    GetClientRect(ctl, &rect);
    const int width = static_cast<int>(rect.right);
    const int height = static_cast<int>(rect.bottom);
    const int x = std::clamp(static_cast<int>(pt.x), 0, std::max(width - 1, 0));
    const int y = std::clamp(static_cast<int>(pt.y), 0, std::max(height - 1, 0));
    state.sat = width > 1 ? static_cast<double>(x) / (width - 1) : 0.0;
    state.val = height > 1 ? 1.0 - static_cast<double>(y) / (height - 1) : 1.0;
    set_current_from_hsv(state);
}

void update_hue_from_point(ColorPickerState& state, HWND ctl, POINT pt)
{
    RECT rect;
    GetClientRect(ctl, &rect);
    const int height = static_cast<int>(rect.bottom);
    const int y = std::clamp(static_cast<int>(pt.y), 0, std::max(height - 1, 0));
    state.hue = height > 0 ? 360.0 * y / height : 0.0;
    set_current_from_hsv(state);
}

LRESULT CALLBACK ColorSurfaceProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    HWND hdlg = GetParent(hwnd);
    ColorPickerState* state = hdlg ? get_state(hdlg) : nullptr;
    const int id = GetDlgCtrlID(hwnd);
    if (!state) {
        return surface_default(hwnd, msg, wparam, lparam);
    }

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (hdc) {
            // The Static class is CS_PARENTDC, so BeginPaint hands back a DC clipped to the
            // PARENT's client area: without this the markers' overhang paints onto the dialog.
            RECT client;
            GetClientRect(hwnd, &client);
            IntersectClipRect(hdc, client.left, client.top, client.right, client.bottom);
            switch (id) {
            case IDC_COLORPICK_SV_FIELD: paint_sv_field(*state, hwnd, hdc); break;
            case IDC_COLORPICK_HUE_BAR: paint_hue_bar(*state, hwnd, hdc); break;
            case IDC_COLORPICK_COMPARE: paint_compare(*state, hwnd, hdc); break;
            case IDC_COLORPICK_PALETTE: paint_palette(*state, hwnd, hdc); break;
            case IDC_COLORPICK_PRESETS: paint_presets(hwnd, hdc); break;
            case IDC_COLORPICK_SAMPLE: paint_sample(*state, hwnd, hdc); break;
            default: break;
            }
            EndPaint(hwnd, &ps);
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (id == IDC_COLORPICK_SV_FIELD || id == IDC_COLORPICK_HUE_BAR) {
            SetCursor(LoadCursorA(nullptr, IDC_CROSS));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (id == IDC_COLORPICK_SV_FIELD) {
            state->drag_sv = true;
            SetCapture(hwnd);
            update_sv_from_point(*state, hwnd, pt);
            refresh_all(hdlg, *state);
            return 0;
        }
        if (id == IDC_COLORPICK_HUE_BAR) {
            state->drag_hue = true;
            SetCapture(hwnd);
            update_hue_from_point(*state, hwnd, pt);
            refresh_all(hdlg, *state);
            return 0;
        }
        if (id == IDC_COLORPICK_COMPARE) {
            RECT rect;
            GetClientRect(hwnd, &rect);
            if (pt.x < rect.right / 2) {
                set_current_from_rgb(*state, state->initial);
                refresh_all(hdlg, *state);
            }
            return 0;
        }
        if (id == IDC_COLORPICK_PALETTE && state->custom) {
            RECT rect;
            GetClientRect(hwnd, &rect);
            const int index = swatch_index(rect, pt, palette_slot_count, 1);
            if (index >= 0) {
                set_current_from_rgb(*state, state->custom[index]);
                refresh_all(hdlg, *state);
            }
            return 0;
        }
        if (id == IDC_COLORPICK_PRESETS) {
            RECT rect;
            GetClientRect(hwnd, &rect);
            const int index = swatch_index(rect, pt, preset_columns, preset_rows);
            if (index >= 0) {
                set_current_from_rgb(*state, preset_colors[index]);
                refresh_all(hdlg, *state);
            }
            return 0;
        }
        break;
    }
    case WM_RBUTTONDOWN:
        // Presets are read only, so only the custom row stores.
        if (id == IDC_COLORPICK_PALETTE && state->custom) {
            RECT rect;
            GetClientRect(hwnd, &rect);
            POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            const int index = swatch_index(rect, pt, palette_slot_count, 1);
            if (index >= 0) {
                state->custom[index] = state->current;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        break;
    case WM_MOUSEMOVE: {
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (id == IDC_COLORPICK_SV_FIELD && state->drag_sv) {
            update_sv_from_point(*state, hwnd, pt);
            refresh_all(hdlg, *state);
            return 0;
        }
        if (id == IDC_COLORPICK_HUE_BAR && state->drag_hue) {
            update_hue_from_point(*state, hwnd, pt);
            refresh_all(hdlg, *state);
            return 0;
        }
        break;
    }
    case WM_LBUTTONUP:
        if (state->drag_sv || state->drag_hue) {
            state->drag_sv = false;
            state->drag_hue = false;
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        state->drag_sv = false;
        state->drag_hue = false;
        break;
    case WM_NCDESTROY:
        if (WNDPROC orig = surface_orig_wndproc(hwnd)) {
            SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig));
        }
        break;
    default:
        break;
    }
    return surface_default(hwnd, msg, wparam, lparam);
}

void subclass_surface(HWND hdlg, int id)
{
    HWND ctl = GetDlgItem(hdlg, id);
    if (!ctl) {
        return;
    }
    WNDPROC prev = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(ctl, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ColorSurfaceProc)));
    if (prev != ColorSurfaceProc) {
        SetWindowLongPtrA(ctl, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(prev));
    }
}

void init_dialog(HWND hdlg, ColorPickerState& state)
{
    subclass_surface(hdlg, IDC_COLORPICK_SV_FIELD);
    subclass_surface(hdlg, IDC_COLORPICK_HUE_BAR);
    subclass_surface(hdlg, IDC_COLORPICK_COMPARE);
    subclass_surface(hdlg, IDC_COLORPICK_PALETTE);
    subclass_surface(hdlg, IDC_COLORPICK_PRESETS);
    subclass_surface(hdlg, IDC_COLORPICK_SAMPLE);

    SendDlgItemMessage(hdlg, IDC_COLORPICK_RED, EM_SETLIMITTEXT, 3, 0);
    SendDlgItemMessage(hdlg, IDC_COLORPICK_GREEN, EM_SETLIMITTEXT, 3, 0);
    SendDlgItemMessage(hdlg, IDC_COLORPICK_BLUE, EM_SETLIMITTEXT, 3, 0);
    SendDlgItemMessage(hdlg, IDC_COLORPICK_HEX, EM_SETLIMITTEXT, 9, 0);

    if (!state.custom) {
        static const int palette_ids[] = {IDC_COLORPICK_PALETTE_LABEL, IDC_COLORPICK_PALETTE,
                                          IDC_COLORPICK_HINT};
        for (int id : palette_ids) {
            if (HWND ctl = GetDlgItem(hdlg, id)) {
                ShowWindow(ctl, SW_HIDE);
            }
        }
    }
    show_sample_controls(hdlg, false);
    refresh_fields(hdlg, state);
}

INT_PTR CALLBACK ColorPickerDialogProc(HWND hdlg, UINT msg, WPARAM wparam, LPARAM lparam)
{
    ColorPickerState* state = get_state(hdlg);

    switch (msg) {
    case WM_INITDIALOG:
        SetWindowLongPtrA(hdlg, GWLP_USERDATA, lparam);
        alpine_center_dialog_on_owner(hdlg);
        init_dialog(hdlg, *reinterpret_cast<ColorPickerState*>(lparam));
        return TRUE;
    case WM_SETCURSOR:
        if (state && state->sampling) {
            SetCursor(LoadCursorA(nullptr, IDC_CROSS));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE:
        if (state && state->sampling) {
            sample_screen_under_cursor(hdlg, *state);
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        if (state && state->sampling) {
            sample_screen_under_cursor(hdlg, *state);
            sampling_end(hdlg, *state, true);
            return TRUE;
        }
        break;
    case WM_RBUTTONDOWN:
        if (state && state->sampling) {
            sampling_end(hdlg, *state, false);
            return TRUE;
        }
        break;
    case WM_CAPTURECHANGED:
        if (state && state->sampling && reinterpret_cast<HWND>(lparam) != hdlg) {
            sampling_end(hdlg, *state, false);
        }
        break;
    case WM_COMMAND: {
        if (!state) {
            break;
        }
        const int id = LOWORD(wparam);
        const int code = HIWORD(wparam);
        if (state->sampling) {
            if (id == IDCANCEL) {
                sampling_end(hdlg, *state, false);
            }
            return TRUE;
        }
        switch (id) {
        case IDC_COLORPICK_RED:
        case IDC_COLORPICK_GREEN:
        case IDC_COLORPICK_BLUE:
            if (code == EN_KILLFOCUS) {
                commit_rgb_fields(hdlg, *state);
                return TRUE;
            }
            break;
        case IDC_COLORPICK_HEX:
            if (code == EN_KILLFOCUS) {
                commit_hex_field(hdlg, *state);
                return TRUE;
            }
            break;
        case IDC_COLORPICK_EYEDROPPER:
            if (code == BN_CLICKED) {
                sampling_begin(hdlg, *state);
                return TRUE;
            }
            break;
        case IDOK: {
            HWND focus = GetFocus();
            const int focus_id = focus ? GetDlgCtrlID(focus) : 0;
            if (focus_id == IDC_COLORPICK_RED || focus_id == IDC_COLORPICK_GREEN ||
                focus_id == IDC_COLORPICK_BLUE) {
                commit_rgb_fields(hdlg, *state);
            }
            else if (focus_id == IDC_COLORPICK_HEX) {
                commit_hex_field(hdlg, *state);
            }
            EndDialog(hdlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        default:
            break;
        }
        break;
    }
    default:
        break;
    }
    return FALSE;
}

} // namespace

COLORREF* alpine_shared_custom_colors()
{
    static COLORREF fallback[palette_slot_count] = {};
    return g_main_frame ? g_main_frame->custom_colors : fallback;
}

AlpineColorPickerResult alpine_pick_color_ex(HWND parent, COLORREF& color, COLORREF* custom_colors)
{
    static bool is_open = false;
    if (is_open) {
        return AlpineColorPickerResult::unavailable;
    }

    HINSTANCE instance = reinterpret_cast<HINSTANCE>(&__ImageBase);
    if (!FindResourceA(instance, MAKEINTRESOURCEA(IDD_ALPINE_COLOR_PICKER),
                       reinterpret_cast<LPCSTR>(RT_DIALOG))) {
        return AlpineColorPickerResult::unavailable;
    }

    ColorPickerState state = {};
    state.initial = color & 0x00FFFFFF;
    state.current = state.initial;
    alpine_rgb_to_hsv(state.current, state.hue, state.sat, state.val);
    state.sv_hue = -1.0;
    state.custom = custom_colors;
    state.sample_color = state.current;

    is_open = true;
    const INT_PTR result = DialogBoxParamA(instance, MAKEINTRESOURCEA(IDD_ALPINE_COLOR_PICKER),
                                           parent, ColorPickerDialogProc,
                                           reinterpret_cast<LPARAM>(&state));
    is_open = false;

    surface_free(state.sv);
    surface_free(state.hue_bar);

    if (result == IDOK) {
        color = state.current & 0x00FFFFFF;
        return AlpineColorPickerResult::ok;
    }
    if (result == IDCANCEL) {
        return AlpineColorPickerResult::cancelled;
    }
    return AlpineColorPickerResult::unavailable;
}

bool alpine_pick_color(HWND parent, COLORREF& color, COLORREF* custom_colors)
{
    switch (alpine_pick_color_ex(parent, color, custom_colors)) {
    case AlpineColorPickerResult::ok:
        return true;
    case AlpineColorPickerResult::cancelled:
        return false;
    default:
        break;
    }

    CHOOSECOLORA cc = {};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = parent;
    cc.rgbResult = color;
    cc.lpCustColors = custom_colors ? custom_colors : alpine_shared_custom_colors();
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (!ChooseColorA(&cc)) {
        return false;
    }
    color = cc.rgbResult;
    return true;
}
