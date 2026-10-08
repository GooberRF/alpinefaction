#pragma once

#include <windows.h>
#include <algorithm>
#include <cmath>

enum class AlpineColorPickerResult
{
    ok,
    cancelled,
    unavailable, // dialog resource missing or the picker is already open
};

// Shared 16-slot swatch array (CMainFrame::custom_colors) every color site writes through.
COLORREF* alpine_shared_custom_colors();

// Opens the Alpine color picker. custom_colors may be null (palette row is hidden).
AlpineColorPickerResult alpine_pick_color_ex(HWND parent, COLORREF& color, COLORREF* custom_colors);

// As above, but falls back to the stock ChooseColor dialog when the picker is unavailable.
bool alpine_pick_color(HWND parent, COLORREF& color, COLORREF* custom_colors);

inline int alpine_color_round_u8(double value)
{
    return std::clamp(static_cast<int>(value + 0.5), 0, 255);
}

// h in [0, 360), s and v in [0, 1]. Paired with alpine_hsv_to_rgb this round-trips every
// COLORREF byte-exactly and keeps the RGB/hex fields from drifting while dragging.
inline void alpine_rgb_to_hsv(COLORREF color, double& h, double& s, double& v)
{
    const int r = GetRValue(color);
    const int g = GetGValue(color);
    const int b = GetBValue(color);
    const int max_c = std::max(r, std::max(g, b));
    const int min_c = std::min(r, std::min(g, b));
    const int delta = max_c - min_c;

    v = max_c / 255.0;
    s = max_c ? static_cast<double>(delta) / max_c : 0.0;
    if (delta == 0) {
        h = 0.0;
        return;
    }
    double hue;
    if (max_c == r) {
        hue = 60.0 * static_cast<double>(g - b) / delta;
    }
    else if (max_c == g) {
        hue = 60.0 * (2.0 + static_cast<double>(b - r) / delta);
    }
    else {
        hue = 60.0 * (4.0 + static_cast<double>(r - g) / delta);
    }
    if (hue < 0.0) {
        hue += 360.0;
    }
    h = hue;
}

inline COLORREF alpine_hsv_to_rgb(double h, double s, double v)
{
    h = std::fmod(h, 360.0);
    if (h < 0.0) {
        h += 360.0;
    }
    s = std::clamp(s, 0.0, 1.0);
    v = std::clamp(v, 0.0, 1.0);

    const double v_max = v * 255.0;
    const double v_min = v_max * (1.0 - s);
    const double sector = h / 60.0;
    int index = static_cast<int>(std::floor(sector));
    index = std::clamp(index, 0, 5);
    const double f = sector - index;
    const double rising = v_min + (v_max - v_min) * f;
    const double falling = v_max - (v_max - v_min) * f;

    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    switch (index) {
    case 0: r = v_max; g = rising; b = v_min; break;
    case 1: r = falling; g = v_max; b = v_min; break;
    case 2: r = v_min; g = v_max; b = rising; break;
    case 3: r = v_min; g = falling; b = v_max; break;
    case 4: r = rising; g = v_min; b = v_max; break;
    default: r = v_max; g = v_min; b = falling; break;
    }
    return RGB(alpine_color_round_u8(r), alpine_color_round_u8(g), alpine_color_round_u8(b));
}

// Total functions: leave the output untouched and return false for anything that is not an
// exact match, so a garbled field can only ever cause a silent revert.
inline bool alpine_color_parse_component(const char* text, int& out)
{
    if (!text) {
        return false;
    }
    const char* p = text;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p < '0' || *p > '9') {
        return false;
    }
    int value = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        if (value > 255) {
            return false;
        }
        ++p;
    }
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p) {
        return false;
    }
    out = value;
    return true;
}

inline bool alpine_color_parse_hex(const char* text, COLORREF& out)
{
    if (!text) {
        return false;
    }
    const char* p = text;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p == '#') {
        ++p;
    }
    unsigned value = 0;
    for (int i = 0; i < 6; ++i) {
        const char c = p[i];
        unsigned digit;
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned>(c - '0');
        }
        else if (c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned>(c - 'a') + 10;
        }
        else if (c >= 'A' && c <= 'F') {
            digit = static_cast<unsigned>(c - 'A') + 10;
        }
        else {
            return false;
        }
        value = (value << 4) | digit;
    }
    p += 6;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p) {
        return false;
    }
    out = RGB((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
    return true;
}
