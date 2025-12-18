#pragma once

#include "../os/os.h"
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

inline struct RemoteServerCfgPopup {
public:

    static bool uses_line_separators();
    static bool is_compact();
    static bool is_highlight_box();
    static bool is_left_aligned();
    bool is_active(this const RemoteServerCfgPopup& self);
    void reset(this RemoteServerCfgPopup& self);
    void add_content(this RemoteServerCfgPopup& self, std::string_view content);
    void toggle(this RemoteServerCfgPopup& self);
    void render(this RemoteServerCfgPopup& self);

    void finalize(this RemoteServerCfgPopup& self) {
        self.finalized = true;
    }

    void set_cfg_changed(this RemoteServerCfgPopup& self) {
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
    void add_line(this RemoteServerCfgPopup& self, std::string_view line);

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
