#pragma once

#include "../os/os.h"
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

inline struct RemoteServerCfgPopup {
private:
    using Self = RemoteServerCfgPopup;

public:
    bool is_active(this const Self& self);
    void reset(this Self& self);
    void add_content(this Self& self, std::string_view content);
    void toggle(this Self& self);
    void render(this Self& self);

    void finalize(this Self& self) {
        self.finalized = true;
    }

    void set_cfg_changed(this Self& self) {
        self.cfg_changed = true;
    }

    enum DisplayMode : uint8_t {
        DISPLAY_MODE_ALIGN_RIGHT_HIGHLIGHT_BOX = 0,
        DISPLAY_MODE_ALIGN_RIGHT_USE_LINE_SEPARATORS = 1,
        DISPLAY_MODE_ALIGN_RIGHT_COMPACT = 2,
        DISPLAY_MODE_ALIGN_LEFT_HIGHLIGHT_BOX = 3,
        DISPLAY_MODE_ALIGN_LEFT_USE_LINE_SEPARATORS = 4,
        DISPLAY_MODE_ALIGN_LEFT_COMPACT = 5,
        _DISPLAY_MODE_COUNT = 6,
    };

private:
    void add_line(this Self& self, std::string_view line);

    using KeyValue = std::pair<std::string, std::string>;
    using Line = std::variant<std::string, KeyValue>;

    std::vector<Line> lines{};
    std::string partial_line{};
    int last_key_down = 0;
    bool cfg_changed = false;
    bool need_restore_scroll = false;
    std::optional<float> saved_scroll{};
    HighResTimer page_up_timer{};
    HighResTimer page_down_timer{};
    bool finalized = false;
    bool active = false;
    struct {
        float current = 0.f;
        float target = 0.f;
        float velocity = 0.f;
    } scroll{};
} g_remote_server_cfg_popup{};
