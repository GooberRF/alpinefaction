#pragma once

#include <optional>
#include <string>
#include <utility>
#include "column_menu.h"
#include "../graphics/mesh_preview.h"

// A click on the path strip's player asks the next Multi screen to open with the panel seated.
void player_panel_request_seat(bool seat);
// Asked once by the Multi screen opening.
bool player_panel_take_seat_request();

// The multiplayer identity as a panel of a column menu: the name box, the character picks and the character's view.
// A name applies once its box lets go of the focus, a character once it is picked.
class PlayerPanel final : public ColumnPanel
{
public:
    [[nodiscard]] bool is_open() const override
    {
        return open_;
    }
    void open() override
    {
        set_open(true);
    }
    void close() override
    {
        set_open(false);
    }
    bool back() override
    {
        close();
        return true;
    }
    void build(UiContext& ctx, const MenuPanel& panel, const MenuFonts& fonts) override;
    void commit() override;

    // The host's enter: shut, and nothing held over from its last visit.
    void reset();
    // After each of the host's hit-test passes.
    void note_text_focus(UiId focus)
    {
        prev_text_focus_ = focus;
    }
    // Before any of the frame's 2D: the preview switches the render target. The draw pass puts it back while the
    // panel shows it.
    void render_preview();
    void release_preview();

private:
    void set_open(bool open);
    void show_character(int index, int team);
    void build_view(UiContext& ctx, const UiRect& r, const MenuFonts& fonts);
    void build_name_row(UiContext& ctx, const UiRect& r, const MenuFonts& fonts);
    void build_grid(UiContext& ctx, const UiRect& well, int tile_font);

    bool open_ = false;
    // Taken by the hit-test pass, acted on after it.
    int pending_character_ = -1;
    std::string name_;
    UiId prev_text_focus_ = 0;
    float grid_scroll_ = 0.0f;
    UiId grid_scrolled_to_ = 0;
    MeshPreview preview_;
    // What preview_ holds, so a character loads once per pick and one that fails is not retried.
    std::optional<std::pair<int, int>> preview_shown_; // character, team
    // Where the latest draw pass put the 3D view; empty while it shows none.
    UiRect preview_rect_{};
};
