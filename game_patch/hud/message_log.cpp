#include <patch_common/CodeInjection.h>
#include <patch_common/CallHook.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/MemUtils.h>
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"

static auto& message_log_bm_w = addr_as_ref<int>(0x006428C4);
static auto& message_log_bm_h = addr_as_ref<int>(0x006428C8);
static auto& message_log_bg_x = addr_as_ref<int>(0x006428CC);
static auto& message_log_bg_y = addr_as_ref<int>(0x006428D0);
static auto& message_log_entries_clip_h = addr_as_ref<int>(0x006425D4);
static auto& message_log_entries_clip_y = addr_as_ref<int>(0x006425D8);
static auto& message_log_entries_clip_w = addr_as_ref<int>(0x006425DC);
static auto& message_log_entries_clip_x = addr_as_ref<int>(0x006425E0);
static float message_log_scale_x;
static float message_log_scale_y;
static int message_log_no_messages_text_y;

CodeInjection message_log_init_injection{
    0x00454CD7,
    []() {
        message_log_scale_x = rf::gr::screen_width() / 640.0f;
        message_log_scale_y = rf::gr::screen_height() / 480.0f;

        auto clip_w = static_cast<float>(rf::gr::clip_width());
        auto clip_h = static_cast<float>(rf::gr::clip_height());
        message_log_bg_x = static_cast<int>((clip_w - message_log_bm_w * message_log_scale_x) / 2.0f);
        message_log_bg_y = static_cast<int>((clip_h - message_log_bm_h * message_log_scale_y) / 2.0f);
        message_log_entries_clip_x = message_log_bg_x + static_cast<int>(30.0f * message_log_scale_x);
        message_log_entries_clip_y = message_log_bg_y + static_cast<int>(41.0f * message_log_scale_y);
        message_log_entries_clip_w = static_cast<int>(313.0f * message_log_scale_x);
        message_log_entries_clip_h = static_cast<int>(296.0f * message_log_scale_y);
        message_log_no_messages_text_y = static_cast<int>(180.0f * message_log_scale_y);
    },
};

CallHook<void(int, int, int, int, int, int, int, int, int, bool, bool, rf::gr::Mode)> message_log_render_gr_bitmap_stretched_hook{
    0x004551F0,
    [](int bm_handle, int x, int y, int w, int h, int sx, int sy, int sw, int sh, bool flip_x, bool flip_y, rf::gr::Mode mode) {
        w = static_cast<int>(sw * message_log_scale_x);
        h = static_cast<int>(sh * message_log_scale_y);
        message_log_render_gr_bitmap_stretched_hook.call_target(bm_handle, x, y, w, h, sx, sy, sw, sh, flip_x, flip_y, mode);
    },
};


// ---------------------------------------------------------------------------
// Message log text layout
//
// Alpine scales the message log background and clip rectangle to the current
// resolution, but the text inside is still laid out with the original 640x480
// constants: the speaker name goes at x=5, the message body at x=70, and the body
// is split to a width of about 240. At 4K that is a 70 pixel name column inside an
// 1878 pixel box, so the speaker name runs straight into the message text.
//
// Separately, the per entry height stored at +0x106 when a message is added is
// (line count + 1) x gr_get_font_height(-1), i.e. measured with the *default* font,
// while the body is drawn with rfpc-medium.vf. Once a localization redirects the
// bitmap fonts to a TTF those differ, so entries overlap and the scroll arithmetic
// (0x00454F53 / 0x004550BC sum the same field) stops working. Fixing the stored
// value covers drawing and scrolling at once.
// ---------------------------------------------------------------------------

static auto& message_log_font = addr_as_ref<int>(0x006C66F0);

static float message_log_ui_scale()
{
    return rf::gr::screen_width() / 640.0f;
}

// The font the log body is drawn with. 0x006C66F0 is set when the log UI is created;
// before that (a message can arrive earlier) fall back to loading it by name. Do not
// call load_font on every entry: the .vf redirect logs a line on each call.
static int message_log_body_font()
{
    if (message_log_font) {
        return message_log_font;
    }
    static int fallback = rf::gr::load_font("rfpc-medium.vf");
    return fallback;
}

// Height of one entry as actually rendered. The extra font height reproduces the
// blank line the original left between entries.
static int message_log_entry_height(const char* text)
{
    if (!text) {
        return 0;
    }
    const int font = message_log_body_font();
    return rf::gr::get_string_size(text, font).second + rf::gr::get_font_height(font);
}

// All four readers of the stored height, recomputed from the text that will actually
// be drawn. Patching the readers rather than the stored value makes this independent
// of where the value came from -- savegames carry it across sessions, so a value
// written by a different build would otherwise survive.
// Each site is a 7 byte `movsx <reg>, word ptr [<ptr> + 0x106]`; we set the
// destination register and resume after the instruction.

// Draw: is this entry within the visible area? (entry in esi -> eax)
CodeInjection message_log_height_visible_injection{
    0x00455315,
    [](auto& regs) {
        const char* text = regs.esi;
        regs.eax = message_log_entry_height(text);
        regs.eip = 0x0045531C;
    },
};

// Draw: advance y to the next entry. (entry in esi -> eax)
CodeInjection message_log_height_advance_injection{
    0x00455421,
    [](auto& regs) {
        const char* text = regs.esi;
        regs.eax = message_log_entry_height(text);
        regs.eip = 0x00455428;
    },
};

// Scroll: total height of the entries below the current position. (entry in eax -> eax)
CodeInjection message_log_height_scroll_sum_injection{
    0x00454F53,
    [](auto& regs) {
        const char* text = regs.eax;
        regs.eax = message_log_entry_height(text);
        regs.eip = 0x00454F5A;
    },
};

// Scroll: height of the last entry, to decide whether it fits. (entry in eax -> edx)
CodeInjection message_log_height_scroll_last_injection{
    0x004550BC,
    [](auto& regs) {
        const char* text = regs.eax;
        regs.edx = message_log_entry_height(text);
        regs.eip = 0x004550C3;
    },
};

// The engine splits the body to the unscaled 640x480 width and measures with the
// *default* font, while the body is drawn at 70 * scale with rfpc-medium.vf. Both
// have to match the real layout or lines come out too long and get clipped.
CallHook<int(int*, int*, char*, int, int, char, int)> message_log_add_split_str_hook{
    0x0043803F,
    [](int* len_array, int* offset_array, char* text, int max_w, int max_lines,
       char unk_char, int font_num) {
        static_cast<void>(max_w);
        static_cast<void>(font_num);
        // clip width 313 minus the 70 indent, minus a small right margin
        const int scaled_w = static_cast<int>(239.0f * message_log_ui_scale());
        return message_log_add_split_str_hook.call_target(
            len_array, offset_array, text, scaled_w, max_lines, unk_char,
            message_log_body_font());
    },
};

// Speaker name (x=5) and message body (x=70), both still in 640x480 coordinates.
CallHook<void(int, int, const char*, int, rf::gr::Mode)> message_log_name_string_hook{
    0x004553BA,
    [](int x, int y, const char* s, int font, rf::gr::Mode mode) {
        message_log_name_string_hook.call_target(
            static_cast<int>(x * message_log_ui_scale()), y, s, font, mode);
    },
};

CallHook<void(int, int, const char*, int, rf::gr::Mode)> message_log_body_string_hook{
    0x004553DD,
    [](int x, int y, const char* s, int font, rf::gr::Mode mode) {
        message_log_body_string_hook.call_target(
            static_cast<int>(x * message_log_ui_scale()), y, s, font, mode);
    },
};

void message_log_apply_patch()
{
    message_log_height_visible_injection.install();
    message_log_height_advance_injection.install();
    message_log_height_scroll_sum_injection.install();
    message_log_height_scroll_last_injection.install();
    message_log_add_split_str_hook.install();
    message_log_name_string_hook.install();
    message_log_body_string_hook.install();

    // Fix message log rendering in resolutions with ratio different than 4:3
    message_log_init_injection.install();
    message_log_render_gr_bitmap_stretched_hook.install();
    AsmWriter{0x00455299, 0x0045529F}.add(asm_regs::ecx, &message_log_no_messages_text_y);
}
