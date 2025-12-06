#pragma once

#include "../rf/input.h"
#include <variant>
#include <vector>
#include <optional>

enum class ChatMenuType : int
{
    None,
    Comms,
    Taunts,
    Commands,
    Spectate
};

void hud_apply_patches();
int hud_get_small_font();
int hud_get_default_font();
int hud_get_large_font();
bool hud_weapons_is_double_ammo();
void draw_hud_vote_notification(std::string vote_type);
void remove_hud_vote_notification();
void stop_draw_respawn_timer_notification();
void draw_respawn_timer_notification(bool can_respawn, bool force_respawn, int spawn_delay);
void draw_hud_ready_notification(bool draw);
void set_local_pre_match_active(bool set_active);
void multi_hud_level_init();
void multi_hud_on_local_spawn();
void multi_hud_reset_run_gt_timer(bool triggered_by_respawn_key);
void multi_hud_update_timer_color();
void toggle_chat_menu(ChatMenuType state);
bool get_chat_menu_is_active();
void hud_render_draw_chat_menu();
void chat_menu_action_handler(rf::Key key);
void build_local_player_spectators_strings();

inline struct RemoteServerCfgPopup {
public:
    void reset();
    void add_content(std::string_view content);
    bool is_active();
    void toggle();
    void render();
    bool is_compact();
    bool uses_line_separators();
    bool is_highlight_box();
    bool is_left_aligned();

    void finalize() {
        m_finalized = true;
    }

    void set_cfg_changed() {
        m_cfg_changed = true;
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
    void add_line(std::string_view line);

    using Line = std::variant<std::string, std::pair<std::string, std::string>>;

    std::vector<Line> m_lines{};
    std::string m_partial_line{};
    int m_last_key_down = 0;
    bool m_cfg_changed = false;
    bool m_need_restore_scroll = false;
    std::optional<float> m_saved_scroll{};
    bool m_finalized = false;
    bool m_is_active = false;
    struct {
        float current = 0.f;
        float target = 0.f;
        float velocity = 0.f;
    } m_scroll{};
} g_remote_server_cfg_popup{};
