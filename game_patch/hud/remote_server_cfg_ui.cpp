#include "remote_server_cfg_ui.h"
#include "../graphics/gr.h"
#include "../input/input.h"
#include "../misc/alpine_settings.h"
#include "../multi/alpine_packets.h"
#include "../rf/gr/gr_font.h"
#include "../rf/level.h"
#include "../rf/os/console.h"
#include "../rf/os/frametime.h"
#include <common/utils/string-utils.h>
#include <format>
#include <utility>

static bool display_mode_is_compact() {
    return g_alpine_game_config.remote_server_cfg_display_mode
        == RemoteServerCfgPopup::DISPLAY_MODE_ALIGN_RIGHT_COMPACT
        || g_alpine_game_config.remote_server_cfg_display_mode
        == RemoteServerCfgPopup::DISPLAY_MODE_ALIGN_LEFT_COMPACT;
};

static bool display_mode_uses_line_separators() {
    return g_alpine_game_config.remote_server_cfg_display_mode
        == RemoteServerCfgPopup::DISPLAY_MODE_ALIGN_RIGHT_USE_LINE_SEPARATORS
        || g_alpine_game_config.remote_server_cfg_display_mode
        == RemoteServerCfgPopup::DISPLAY_MODE_ALIGN_LEFT_USE_LINE_SEPARATORS;
}

static bool display_mode_is_highlight_box() {
    return g_alpine_game_config.remote_server_cfg_display_mode
        == RemoteServerCfgPopup::DISPLAY_MODE_ALIGN_RIGHT_HIGHLIGHT_BOX
        || g_alpine_game_config.remote_server_cfg_display_mode
        == RemoteServerCfgPopup::DISPLAY_MODE_ALIGN_LEFT_HIGHLIGHT_BOX;
}

static bool display_mode_is_left_aligned() {
    return g_alpine_game_config.remote_server_cfg_display_mode
        == RemoteServerCfgPopup::DISPLAY_MODE_ALIGN_LEFT_HIGHLIGHT_BOX
        || g_alpine_game_config.remote_server_cfg_display_mode
        == RemoteServerCfgPopup::DISPLAY_MODE_ALIGN_LEFT_USE_LINE_SEPARATORS
        || g_alpine_game_config.remote_server_cfg_display_mode
        == RemoteServerCfgPopup::DISPLAY_MODE_ALIGN_LEFT_COMPACT;
}

void RemoteServerCfgPopup::reset(this Self& self) {
    self = Self{};
}

void RemoteServerCfgPopup::add_content(
    this Self& self,
    const std::string_view content
) {
    size_t i = 0;
    const size_t len = content.size();

    while (i < len) {
        const size_t new_line = content.find('\n', i);
        const bool complete = new_line != std::string_view::npos;
        std::string_view fragment = complete
            ? content.substr(i, new_line - i)
            : content.substr(i);

        if (!self.partial_line.empty() && !self.lines.empty()) {
            self.lines.pop_back();
        }

        self.partial_line += fragment;
        self.add_line(self.partial_line);

        if (complete && !self.partial_line.empty()) {
            self.partial_line.clear();
        }

        i = complete ? new_line + 1 : len;
    }
}

void RemoteServerCfgPopup::add_line(
    this Self& self,
    std::string_view line
) {
    const size_t new_line = line.find_first_of("\r\n");
    if (new_line != std::string_view::npos) {
        line = line.substr(0, new_line);
    }

    constexpr float REF_WIDTH  = 1280.f;
    constexpr float REF_HEIGHT = 800.f;
    const float scale_x = rf::gr::clip_width() / REF_WIDTH;
    const float scale_y = rf::gr::clip_height() / REF_HEIGHT;
    const float ui_scale = std::min(scale_x, scale_y);
    const int font_id = hud_get_default_font();
    const int w = static_cast<int>(680.f * ui_scale);
    const int content_w = w;
    const int line_w = content_w - 20 - static_cast<int>(50.f * ui_scale);
    const auto [space_w, space_h] = rf::gr::get_char_size(' ', font_id);

    const auto push_kv_line = [&] (
        std::string key,
        std::string value,
        const std::string_view key_suffix
    ) {
        const auto [key_w, key_h] = rf::gr::get_string_size(key, font_id);
        const auto [value_w, value_h] = rf::gr::get_string_size(value, font_id);
        int max_key_w = static_cast<int>(line_w * .6f);
        int max_value_w = line_w - max_key_w - space_w;
        if (key_w < max_key_w) {
            max_value_w = line_w - key_w - space_w;
        } else if (value_w < max_value_w) {
            max_key_w = line_w - value_w - space_w;
        }
        gr_fit_string(value, max_value_w, font_id);
        gr_fit_string(key, max_key_w, font_id, key_suffix);
        self.lines.push_back(std::pair{std::move(key), std::move(value)});
    };

    const size_t colon = line.find(":");
    const size_t arrow = line.find("->");
    if (colon != std::string::npos) {
        std::string key{line.substr(0, colon + 1)};
        std::string value{ltrim(line.substr(colon + 1))};
        push_kv_line(std::move(key), std::move(value), "- :");
    } else if (arrow != std::string::npos) {
        std::string key{rtrim(line.substr(0, arrow))};
        std::string value{ltrim(line.substr(arrow + 2))};
        key += " ->";
        push_kv_line(std::move(key), std::move(value), "- ->");
    } else {
        std::string text{line};
        gr_fit_string(text, line_w, font_id);
        // HACKFIX: We want to color `.toml` files.
        if (line.starts_with("    ") && line.contains(".toml")) {
            std::string empty_key{};
            self.lines.push_back(std::pair{std::move(empty_key), std::move(text)});
        } else {
            self.lines.push_back(std::move(text));
        }
    }
}

bool RemoteServerCfgPopup::is_active(this const Self& self) {
    return self.active;
}

void RemoteServerCfgPopup::toggle(this Self& self) {
    if (!self.active && self.cfg_changed) {
        const float saved_scroll = self.scroll.current;
        self.reset();
        self.need_restore_scroll = true;
        self.saved_scroll.emplace(saved_scroll);
    }

    self.active = !self.active;

    if (self.active && self.lines.empty()) {
        af_send_server_cfg_request();
    }
}

void RemoteServerCfgPopup::render(this Self& self) {
    constexpr float REF_WIDTH  = 1280.f;
    constexpr float REF_HEIGHT = 800.f;
    const float scale_x = rf::gr::clip_width() / REF_WIDTH;
    const float scale_y = rf::gr::clip_height() / REF_HEIGHT;
    const float ui_scale = std::min(scale_x, scale_y);

    const int font_id = hud_get_default_font();
    const int label_font_id = font_id;

    int separator_h = display_mode_is_compact() ? 0 : 1;
    int line_factor = display_mode_is_compact() ? 2 : 6;

    constexpr rf::Key DISPLAY_MODE_KEY = rf::KEY_BACKSP;

    if (rf::key_get_and_reset_down_counter(DISPLAY_MODE_KEY)
        && !rf::console::console_is_visible()) {
        const int delta = rf::key_is_down(rf::KEY_LSHIFT)
            || rf::key_is_down(rf::KEY_RSHIFT)
            ? -1
            : 1;
        const auto value = std::to_underlying(
            g_alpine_game_config.remote_server_cfg_display_mode
        );
        g_alpine_game_config.remote_server_cfg_display_mode =
            static_cast<Self::DisplayMode>(
                (value + delta + Self::_DISPLAY_MODE_COUNT)
                    % Self::_DISPLAY_MODE_COUNT
            );

        const int new_separator_h = display_mode_is_compact() ? 0 : 1;
        const int new_line_factor = display_mode_is_compact() ? 2 : 6;
        const int old_line_h = rf::gr::get_font_height(font_id) + line_factor;
        const int old_total_h = (self.lines.size() + 1)
            * (old_line_h + separator_h)
            + separator_h;
        const int new_line_h =  rf::gr::get_font_height(font_id) + new_line_factor;
        const int new_total_h = (self.lines.size() + 1)
            * (new_line_h + new_separator_h)
            + new_separator_h;

        const float ratio_current = old_total_h > 0
            ? self.scroll.current / static_cast<float>(old_total_h)
            : 0.f;
        const float ratio_target = old_total_h > 0
            ? self.scroll.target / static_cast<float>(old_total_h)
            : 0.f;
        self.scroll.current = ratio_current * new_total_h;
        self.scroll.target = ratio_target * new_total_h;

        const int label_h = 10 + rf::gr::get_font_height(label_font_id) + 10;
        const int h = static_cast<int>(500.f * ui_scale)
            / (new_line_h + new_separator_h)
            * (new_line_h + new_separator_h)
            + label_h
            * 2
            + separator_h;
        const float max_scroll = static_cast<float>(
            std::max(0, new_total_h - h + label_h * 2)
        );

        self.scroll.current = std::clamp(self.scroll.current, 0.f, max_scroll);
        self.scroll.target = std::clamp(self.scroll.target, 0.f, max_scroll);

        separator_h = new_separator_h;
        line_factor = new_line_factor;
    }

    const int line_h = rf::gr::get_font_height(font_id) + line_factor;
    const int line_pad_h = line_factor / 2;
    const int total_h = (self.lines.size() + 1)
        * (line_h + separator_h)
        + separator_h;
    const int label_h = 10 + rf::gr::get_font_height(label_font_id) + 10;
    const int w = static_cast<int>(680.f * ui_scale);
    const int x = (rf::gr::clip_width() - w) / 2;
    const int h = static_cast<int>(500.f * ui_scale)
        / (line_h + separator_h)
        * (line_h + separator_h)
        + label_h
        * 2
        + separator_h;
    const int y = (rf::gr::clip_height() - h) / 2;
    const float max_scroll = static_cast<float>(
        std::max(0, total_h - h + label_h * 2)
    );

    int clip_x = 0, clip_y = 0, clip_w = 0, clip_h = 0;
    rf::gr::get_clip(&clip_x, &clip_y, &clip_w, &clip_h);

    if (rf::mouse_dz != 0) {
        constexpr float NUM_LINES = 2.f;
        self.scroll.target += (rf::mouse_dz > 0 ? -NUM_LINES : NUM_LINES)
            * (static_cast<float>(line_h + separator_h));
    }

    if (!rf::console::console_is_visible()) {
        const int page_h = h - label_h * 2 - separator_h;
        constexpr int PAGE_UP_DOWN_GRACE_PERIOD_MS = 500;
        constexpr int PAGE_UP_DOWN_WAIT_TIME_MS = 100;
        if (rf::key_is_down(rf::KEY_PAGEUP)
            && (!self.page_up_timer.valid() || self.page_up_timer.elapsed())
        ) {
            self.scroll.target
                -= static_cast<float>(page_h - (line_h + separator_h));
            const int wait_time = self.page_up_timer.valid()
                ? PAGE_UP_DOWN_WAIT_TIME_MS
                : PAGE_UP_DOWN_GRACE_PERIOD_MS;
            self.page_up_timer.set(std::chrono::milliseconds{wait_time});
        }
        if (rf::key_is_down(rf::KEY_PAGEDOWN)
            && (!self.page_down_timer.valid() || self.page_down_timer.elapsed())
        ) {
            self.scroll.target
                += static_cast<float>(page_h - (line_h + separator_h));
            const int wait_time = self.page_down_timer.valid()
                ? PAGE_UP_DOWN_WAIT_TIME_MS
                : PAGE_UP_DOWN_GRACE_PERIOD_MS;
            self.page_down_timer.set(std::chrono::milliseconds{wait_time});
        }
        if (!rf::key_is_down(rf::KEY_PAGEUP) && self.page_up_timer.valid()) {
            self.page_up_timer.invalidate();
        }
        if (!rf::key_is_down(rf::KEY_PAGEDOWN) && self.page_down_timer.valid()) {
            self.page_down_timer.invalidate();
        }

        constexpr float KEY_SCROLL_SPEED = 600.f;
        if (rf::key_is_down(rf::KEY_UP)) {
            self.scroll.target -= KEY_SCROLL_SPEED * rf::frametime;
            self.last_key_down = 1;
        } else if (rf::key_is_down(rf::KEY_DOWN)) {
            self.scroll.target += KEY_SCROLL_SPEED * rf::frametime;
            self.last_key_down = -1;
        }
    }

    if (!rf::key_is_down(rf::KEY_UP)
        && !rf::key_is_down(rf::KEY_DOWN)
        && self.last_key_down) {
        const float line_and_sep_h = static_cast<float>(line_h + separator_h);
        float rem = std::fmod(self.scroll.target, line_and_sep_h);
        if (std::fabs(rem) > 1e-3f) {
            if (rem < 0.f) {
                rem += line_and_sep_h;
            }
            if (self.last_key_down < 0) {
                self.scroll.target += line_and_sep_h - rem;
            } else {
                self.scroll.target -= rem;
            }
        }
        self.last_key_down = 0;
    }

    if (self.need_restore_scroll
        && self.saved_scroll.has_value()
        && self.finalized) {
        self.scroll.target = std::clamp(self.saved_scroll.value(), 0.f, max_scroll);
        self.scroll.current = self.scroll.target;
        self.saved_scroll.reset();
        self.need_restore_scroll = false;
    }

    self.scroll.target = std::clamp(self.scroll.target, 0.f, max_scroll);

    const float delta = std::fabs(self.scroll.target - self.scroll.current);
    // HACKFIX: If `delta` falls below 1.0, it can cause a final stutter.
    constexpr int MIN_DELTA = 1;
    if (delta >= static_cast<float>(MIN_DELTA)) {
        const auto smooth_cd = [] (
            const float from,
            const float to,
            float &vel,
            const float smooth_time
        ) {
            const float omega = 2.f / smooth_time;
            const float x = omega * rf::frametime;
            const float exp = 1.f / (1.f + x + .48f * x * x + .235f * x * x * x);
            const float change = from - to;
            const float tmp = (vel + omega * change) * rf::frametime;
            vel = (vel - omega * tmp) * exp;
            return to + (change + tmp) * exp;
        };
        self.scroll.current = smooth_cd(
            self.scroll.current,
            self.scroll.target,
            self.scroll.velocity,
            .1f
        );
    }

    if (!rf::console::console_is_visible()) {
        if (rf::key_get_and_reset_down_counter(rf::KEY_HOME)
            && std::lround(self.scroll.current) > MIN_DELTA) {
            self.scroll.target = .0f;
        } else if (rf::key_get_and_reset_down_counter(rf::KEY_END)
            && std::lround(self.scroll.current)
                < std::lround(max_scroll - static_cast<float>(MIN_DELTA))) {
            self.scroll.target = max_scroll;
        }
    }

    rf::gr::set_color(0, 0, 0, 128);
    // rf::gr::rect(x, y + label_h, w, h - label_h * 2);
    rf::gr::rect(x, y, w, h);
    // rf::gr::set_color(100, 100, 100, 30);
    // rf::gr::rect(x, y, w, label_h);
    // rf::gr::set_color(255, 200, 100, 255);

    constexpr std::string_view BASE_TEXT = "REMOTE SERVER CONFIG";
    constexpr std::string_view SEPARATOR_TEXT = " | ";
    constexpr std::string_view OUTDATED_TEXT = "OUTDATED";
    const auto [base_w, base_h] = rf::gr::get_string_size(BASE_TEXT, label_font_id);
    int total_w = base_w;
    if (self.cfg_changed) {
        const auto [sep_w, sep_h]
            = rf::gr::get_string_size(SEPARATOR_TEXT, label_font_id);
        const auto [out_w, out_h]
            = rf::gr::get_string_size(OUTDATED_TEXT, label_font_id);
        total_w += sep_w + out_w;
    }
    const int center_x = x + w / 2;
    const int left_x = center_x - total_w / 2;
    rf::gr::set_color(255, 255, 255, 255);
    rf::gr::string_aligned(
        rf::gr::ALIGN_LEFT,
        left_x,
        y + 10,
        BASE_TEXT.data(),
        label_font_id
    );
    if (self.cfg_changed) {
        rf::gr::set_color(255, 255, 255, 255);
        rf::gr::string_aligned(
            rf::gr::ALIGN_LEFT,
            rf::gr::current_string_x,
            y + 10,
            SEPARATOR_TEXT.data(),
            label_font_id
        );
        rf::gr::set_color(255, 80, 80, 255);
        rf::gr::string_aligned(
            rf::gr::ALIGN_LEFT,
            rf::gr::current_string_x,
            y + 10,
            OUTDATED_TEXT.data(),
            label_font_id
        );
    }

    const int content_x = x;
    const int content_y = y + label_h;
    const int content_w = w;
    const int content_h = h - label_h * 2;
    rf::gr::set_clip(0, content_y, rf::gr::clip_width(), content_h);

    int line_y = std::lround(-self.scroll.current);

    if (display_mode_uses_line_separators()) {
        rf::gr::set_color(180, 180, 180, 64);
        rf::gr::set_clip(0, content_y - MIN_DELTA, rf::gr::clip_width(), content_h);
        rf::gr::rect(content_x, line_y + MIN_DELTA, content_w, separator_h);
        rf::gr::set_clip(0, content_y, rf::gr::clip_width(), content_h);
        line_y += separator_h;
    }

    constexpr auto GET_DISPLAY_MODE_MSG = [] (const rf::Key key) {
        rf::String key_name{};
        rf::control_config_get_key_name(&key_name, key);
        return std::format("Press {} to Cycle Display Modes", key_name);
    };
    static const std::string display_mode_msg = GET_DISPLAY_MODE_MSG(DISPLAY_MODE_KEY);
    rf::gr::set_color(180, 180, 180, 255);
    rf::gr::string_aligned(
        rf::gr::ALIGN_CENTER,
        content_x + content_w / 2,
        line_y + line_pad_h,
        display_mode_msg.c_str(),
        font_id
    );

    line_y += line_h;

    for (const auto& line : self.lines) {
        if (line_y + line_h + separator_h < 0) {
            line_y += line_h + separator_h;
            continue;
        } else if (line_y >= content_h) {
            break;
        }

        if (display_mode_uses_line_separators()) {
            rf::gr::set_color(180, 180, 180, 64);
            rf::gr::set_clip(
                0,
                content_y - MIN_DELTA,
                rf::gr::clip_width(),
                content_h + MIN_DELTA * 2
            );
            rf::gr::rect(content_x, line_y + MIN_DELTA, content_w, separator_h);
            rf::gr::set_clip(0, content_y, rf::gr::clip_width(), content_h);
        } else if (display_mode_is_highlight_box()) {
            rf::gr::set_color(180, 180, 180, 64);
            rf::gr::rect(content_x, line_y, content_w, separator_h);
        }

        line_y += separator_h;

        rf::gr::set_color(255, 255, 255, 255);
        const int line_w = content_w - 20 - static_cast<int>(50.f * ui_scale);
        if (std::holds_alternative<Self::KeyValue>(line)) {
            const auto& [key, value] =
                std::get<Self::KeyValue>(line);
            rf::gr::string_aligned(
                rf::gr::ALIGN_LEFT,
                content_x + 20,
                line_y + line_pad_h,
                key.c_str(),
                font_id
            );
            if (value == "true") {
                rf::gr::set_color(0, 220, 130, 255);
            } else if (value == "false") {
                rf::gr::set_color(255, 80, 80, 255);
            } else {
                rf::gr::set_color(100, 200, 255, 255);
            }
            if (display_mode_is_left_aligned()) {
                const auto [space_w, space_h] = rf::gr::get_char_size(' ', font_id);
                rf::gr::string_aligned(
                    rf::gr::ALIGN_LEFT,
                    rf::gr::current_string_x + space_w,
                    line_y + line_pad_h,
                    value.c_str(),
                    font_id
                );
            } else {
                rf::gr::string_aligned(
                    rf::gr::ALIGN_RIGHT,
                    content_x + content_w - static_cast<int>(50.f * ui_scale),
                    line_y + line_pad_h,
                    value.c_str(),
                    font_id
                );
            }
        } else if (std::holds_alternative<std::string>(line)) {
            const std::string& text = std::get<std::string>(line);
            if (text.starts_with(rf::level.filename.c_str())) {
                rf::gr::set_color(255, 0, 255, 255);
            }
            rf::gr::string_aligned(
                rf::gr::ALIGN_LEFT,
                content_x + 20,
                line_y + line_pad_h,
                text.c_str(),
                font_id
            );
        }
        line_y += line_h;
    }

    if (display_mode_uses_line_separators()) {
        rf::gr::set_color(180, 180, 180, 64);
        rf::gr::set_clip(0, content_y + MIN_DELTA, rf::gr::clip_width(), content_h);
        rf::gr::rect(content_x, line_y - MIN_DELTA, content_w, separator_h);
        rf::gr::set_clip(0, content_y, rf::gr::clip_width(), content_h);
    } else if (display_mode_is_highlight_box()) {
        rf::gr::set_color(255, 255, 0, 255);
        rf::gr::rect(content_x, 0, content_w, separator_h);
        rf::gr::rect(content_x, content_h - separator_h, content_w, separator_h);
    }

    if (total_h > content_h) {
        const float scroll_ratio = self.scroll.current / max_scroll;
        const float scroll_bar_height = static_cast<float>(content_h)
            * static_cast<float>(content_h)
            / total_h;
        const float scroll_bar_y = scroll_ratio
            * (static_cast<float>(content_h) - scroll_bar_height);
        const int bar_x = content_x + content_w - 6;
        const int bar_w = 6;
        // rf::gr::set_color(100, 100, 100, 128);
        // rf::gr::rect(bar_x, y, bar_w, h);
        // rf::gr::set_color(200, 200, 200, 128);
        rf::gr::set_color(100, 255, 200, 255);
        rf::gr::rect(
            bar_x,
            std::lround(scroll_bar_y),
            bar_w,
            std::lround(scroll_bar_height)
        );
    }

    rf::gr::set_clip(clip_x, clip_y, clip_w, clip_h);

    const rf::String key = get_action_bind_name(
        get_af_control(rf::AlpineControlConfigAction::AF_ACTION_REMOTE_SERVER_CFG)
    );
    rf::gr::set_color(255, 255, 255, 255);
    rf::gr::string_aligned(
        rf::gr::ALIGN_CENTER,
        x + w / 2,
        y + h - label_h + 10,
        std::format("Press {} to Close", key).c_str(),
        label_font_id
    );
}
