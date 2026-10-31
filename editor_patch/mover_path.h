#pragma once

#include <windows.h>
#include <cstddef>
#include <common/mover_path.h>

struct BrushNode;
struct CDedLevel;
struct DedObject;
struct EditorViewport;
struct GroupEntry;
namespace rf
{
    struct File;
}

void ApplyMoverPathPatches();

void mover_path_serialize_chunk(CDedLevel& level, rf::File& file);
void mover_path_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len);

// A .rfg stores the chunk with keyframe indices, since stock renumbers colliding uids on import. A group save hands
// each group to mover_path_group_saved, in file order, then writes them; import resolves the indices against the
// moving groups from `first_new_group` on.
void mover_path_group_save_begin();
void mover_path_group_saved(const GroupEntry* group);
void mover_path_group_serialize_chunk(CDedLevel& level, rf::File& file);
void mover_path_group_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len, int first_new_group);

// A keyframe's data, or the defaults; setting the defaults forgets the keyframe.
mover_path::KeyframeData mover_path_keyframe_data(const DedObject& keyframe);
void mover_path_set_keyframe_data(const DedObject& keyframe, const mover_path::KeyframeData& data);
void mover_path_keyframe_mirrored(const DedObject& keyframe, int axis);
void mover_path_uid_generated(int uid);

GroupEntry* find_moving_group_from_selection();

void mover_path_dialog_init(HWND hdlg);
void mover_path_dialog_command(HWND hdlg, int id);
void mover_path_dialog_ok(HWND hdlg);

bool mover_path_view_lbutton_down(EditorViewport* view, UINT flags, int x, int y);
void mover_path_view_lbutton_up();
bool mover_path_view_rbutton_down();
bool mover_path_view_mbutton_down();
bool mover_path_dragging();
bool mover_path_holds_view_focus();
void mover_path_cancel_drag();
void mover_path_idle();
void mover_path_level_reset();

void mover_path_preview_open();
void mover_path_preview_close();
bool mover_path_preview_shown();
bool mover_path_preview_hides(const BrushNode& brush);
void mover_path_render(CDedLevel* level);
