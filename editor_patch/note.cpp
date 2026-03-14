#include <windows.h>
#include <commctrl.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>
#include <vector>
#include <xlog/xlog.h>
#include "note.h"
#include "level.h"
#include "resources.h"
#include "vtypes.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

// ─── Globals ─────────────────────────────────────────────────────────────────

// Sprite bitmap handle for note icon
static int g_note_icon_handle = -1;

// Note clipboard for copy/paste
static std::vector<DedNote*> g_note_clipboard;

// ─── Sprite Loading ─────────────────────────────────────────────────────────

static void note_load_icon()
{
    if (g_note_icon_handle < 0) {
        g_note_icon_handle = bm_load("Icon_AFNote.tga", -1, 1);
        if (g_note_icon_handle >= 0) {
            xlog::info("[Note] Loaded note icon: handle={}", g_note_icon_handle);
        }
        else {
            xlog::warn("[Note] Failed to load note icon");
        }
    }
}

// ─── Cleanup ─────────────────────────────────────────────────────────────────

static void destroy_ded_note(DedNote* note)
{
    if (!note) return;
    note->field_4.free();
    note->script_name.free();
    note->class_name.free();
    delete note;
}

// ─── Serialization ──────────────────────────────────────────────────────────

void note_serialize_chunk(CDedLevel& level, rf::File& file)
{
    auto& notes = level.GetAlpineLevelProperties().note_objects;
    if (notes.empty()) return;

    auto start_pos = level.BeginRflSection(file, alpine_note_chunk_id);

    uint32_t count = static_cast<uint32_t>(notes.size());
    file.write<uint32_t>(count);

    for (auto* note : notes) {
        file.write<int32_t>(note->uid);
        file.write<float>(note->pos.x);
        file.write<float>(note->pos.y);
        file.write<float>(note->pos.z);
        file.write<float>(note->orient.rvec.x);
        file.write<float>(note->orient.rvec.y);
        file.write<float>(note->orient.rvec.z);
        file.write<float>(note->orient.uvec.x);
        file.write<float>(note->orient.uvec.y);
        file.write<float>(note->orient.uvec.z);
        file.write<float>(note->orient.fvec.x);
        file.write<float>(note->orient.fvec.y);
        file.write<float>(note->orient.fvec.z);
        write_rfl_string(file, note->script_name);
        uint32_t note_count = static_cast<uint32_t>(note->notes.size());
        file.write<uint32_t>(note_count);
        for (const auto& text : note->notes) {
            write_rfl_string(file, text);
        }
    }

    level.EndRflSection(file, start_pos);
}

void note_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len)
{
    auto& notes = level.GetAlpineLevelProperties().note_objects;
    std::size_t remaining = chunk_len;

    rf::File::ChunkGuard chunk_guard{file, remaining};

    auto read_bytes = [&](void* dst, std::size_t n) -> bool {
        if (remaining < n) return false;
        int got = file.read(dst, n);
        if (got != static_cast<int>(n) || file.error()) {
            if (got > 0) remaining -= got;
            return false;
        }
        remaining -= n;
        return true;
    };

    uint32_t count = 0;
    if (!read_bytes(&count, sizeof(count))) return;
    if (count > 10000) count = 10000;

    for (uint32_t i = 0; i < count; i++) {
        auto* note = new DedNote();
        memset(static_cast<DedObject*>(note), 0, sizeof(DedObject));
        note->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
        note->type = DedObjectType::DED_NOTE;

        if (!read_bytes(&note->uid, sizeof(note->uid))) { destroy_ded_note(note); return; }
        if (!read_bytes(&note->pos.x, sizeof(float))) { destroy_ded_note(note); return; }
        if (!read_bytes(&note->pos.y, sizeof(float))) { destroy_ded_note(note); return; }
        if (!read_bytes(&note->pos.z, sizeof(float))) { destroy_ded_note(note); return; }
        if (!read_bytes(&note->orient.rvec.x, sizeof(float))) { destroy_ded_note(note); return; }
        if (!read_bytes(&note->orient.rvec.y, sizeof(float))) { destroy_ded_note(note); return; }
        if (!read_bytes(&note->orient.rvec.z, sizeof(float))) { destroy_ded_note(note); return; }
        if (!read_bytes(&note->orient.uvec.x, sizeof(float))) { destroy_ded_note(note); return; }
        if (!read_bytes(&note->orient.uvec.y, sizeof(float))) { destroy_ded_note(note); return; }
        if (!read_bytes(&note->orient.uvec.z, sizeof(float))) { destroy_ded_note(note); return; }
        if (!read_bytes(&note->orient.fvec.x, sizeof(float))) { destroy_ded_note(note); return; }
        if (!read_bytes(&note->orient.fvec.y, sizeof(float))) { destroy_ded_note(note); return; }
        if (!read_bytes(&note->orient.fvec.z, sizeof(float))) { destroy_ded_note(note); return; }

        std::string sname = read_rfl_string(file, remaining);
        note->script_name.assign_0(sname.c_str());

        uint32_t note_count = 0;
        if (!read_bytes(&note_count, sizeof(note_count))) { destroy_ded_note(note); return; }
        if (note_count > 10000) note_count = 10000;
        for (uint32_t ni = 0; ni < note_count; ni++) {
            std::string text = read_rfl_string(file, remaining);
            note->notes.push_back(std::move(text));
        }

        notes.push_back(note);
        level.master_objects.add(static_cast<DedObject*>(note));
    }

    xlog::info("[Note] Loaded {} note objects", notes.size());
}

// ─── Properties Dialog ──────────────────────────────────────────────────────

static std::vector<DedNote*> g_selected_notes;

static void note_populate_list(HWND hdlg, DedNote* note)
{
    HWND hlist = GetDlgItem(hdlg, IDC_NOTE_LIST);
    ListView_DeleteAllItems(hlist);

    for (int i = 0; i < static_cast<int>(note->notes.size()); i++) {
        char idx_buf[16];
        snprintf(idx_buf, sizeof(idx_buf), "%d", i);

        LVITEMA lvi = {};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = i;
        lvi.iSubItem = 0;
        lvi.pszText = idx_buf;
        ListView_InsertItem(hlist, &lvi);

        std::string preview = note->notes[i];
        for (auto& c : preview) {
            if (c == '\n' || c == '\r') c = ' ';
        }
        if (preview.size() > 80) {
            preview = preview.substr(0, 77) + "...";
        }
        ListView_SetItemText(hlist, i, 1, const_cast<char*>(preview.c_str()));
    }
}

static INT_PTR CALLBACK NoteDialogProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    static DedNote* s_note = nullptr;

    switch (msg) {
    case WM_INITDIALOG: {
        if (g_selected_notes.empty()) {
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        s_note = g_selected_notes[0];

        SetDlgItemTextA(hdlg, IDC_NOTE_SCRIPT_NAME, s_note->script_name.c_str());

        HWND hlist = GetDlgItem(hdlg, IDC_NOTE_LIST);
        ListView_SetExtendedListViewStyle(hlist, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        LVCOLUMNA col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 30;
        col.pszText = const_cast<char*>("#");
        ListView_InsertColumn(hlist, 0, &col);

        col.cx = 210;
        col.pszText = const_cast<char*>("Preview");
        ListView_InsertColumn(hlist, 1, &col);

        note_populate_list(hdlg, s_note);
        return TRUE;
    }
    case WM_NOTIFY: {
        auto* nmhdr = reinterpret_cast<NMHDR*>(lp);
        if (nmhdr->idFrom == IDC_NOTE_LIST && nmhdr->code == LVN_ITEMCHANGED) {
            auto* nmlv = reinterpret_cast<NMLISTVIEW*>(lp);
            if ((nmlv->uNewState & LVIS_SELECTED) && !(nmlv->uOldState & LVIS_SELECTED)) {
                int sel = nmlv->iItem;
                if (sel >= 0 && sel < static_cast<int>(s_note->notes.size())) {
                    SetDlgItemTextA(hdlg, IDC_NOTE_TEXT, s_note->notes[sel].c_str());
                }
            }
            else if (!(nmlv->uNewState & LVIS_SELECTED) && (nmlv->uOldState & LVIS_SELECTED)) {
                // Item deselected — clear text box if nothing else is selected
                HWND hlist = GetDlgItem(hdlg, IDC_NOTE_LIST);
                if (ListView_GetNextItem(hlist, -1, LVNI_SELECTED) < 0) {
                    SetDlgItemTextA(hdlg, IDC_NOTE_TEXT, "(empty)");
                }
            }
        }
        return FALSE;
    }
    case WM_COMMAND:
        // Auto-commit script name as the user types
        if (HIWORD(wp) == EN_CHANGE && LOWORD(wp) == IDC_NOTE_SCRIPT_NAME) {
            char buf[256] = {};
            GetDlgItemTextA(hdlg, IDC_NOTE_SCRIPT_NAME, buf, sizeof(buf));
            for (auto* n : g_selected_notes) {
                n->script_name.assign_0(buf);
            }
            return TRUE;
        }
        // Auto-commit text edits to the selected note entry as the user types
        if (HIWORD(wp) == EN_CHANGE && LOWORD(wp) == IDC_NOTE_TEXT && s_note) {
            HWND hlist = GetDlgItem(hdlg, IDC_NOTE_LIST);
            int sel = ListView_GetNextItem(hlist, -1, LVNI_SELECTED);
            if (sel >= 0 && sel < static_cast<int>(s_note->notes.size())) {
                int len = GetWindowTextLengthA(GetDlgItem(hdlg, IDC_NOTE_TEXT));
                std::string text(len + 1, '\0');
                if (len > 0) {
                    int got = GetDlgItemTextA(hdlg, IDC_NOTE_TEXT, text.data(), len + 1);
                    text.resize(got);
                } else {
                    text.clear();
                }
                s_note->notes[sel] = text;
                // Update list preview for this item
                std::string preview = text;
                for (auto& c : preview) {
                    if (c == '\n' || c == '\r') c = ' ';
                }
                if (preview.size() > 80) {
                    preview = preview.substr(0, 77) + "...";
                }
                ListView_SetItemText(hlist, sel, 1, const_cast<char*>(preview.c_str()));
            }
            return TRUE;
        }
        switch (LOWORD(wp)) {
        case IDC_NOTE_ADD: {
            int len = GetWindowTextLengthA(GetDlgItem(hdlg, IDC_NOTE_TEXT));
            std::string text(len + 1, '\0');
            if (len > 0) {
                int got = GetDlgItemTextA(hdlg, IDC_NOTE_TEXT, text.data(), len + 1);
                text.resize(got);
            } else {
                text.clear();
            }
            if (text.empty()) {
                text = "(empty)";
            }
            s_note->notes.push_back(text);
            note_populate_list(hdlg, s_note);
            HWND hlist = GetDlgItem(hdlg, IDC_NOTE_LIST);
            int last = static_cast<int>(s_note->notes.size()) - 1;
            ListView_SetItemState(hlist, last, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(hlist, last, FALSE);
            return TRUE;
        }
        case IDC_NOTE_REMOVE: {
            HWND hlist = GetDlgItem(hdlg, IDC_NOTE_LIST);
            int sel = ListView_GetNextItem(hlist, -1, LVNI_SELECTED);
            if (sel >= 0 && sel < static_cast<int>(s_note->notes.size())) {
                s_note->notes.erase(s_note->notes.begin() + sel);
                note_populate_list(hdlg, s_note);
                SetDlgItemTextA(hdlg, IDC_NOTE_TEXT, "");
                if (sel >= static_cast<int>(s_note->notes.size())) {
                    sel = static_cast<int>(s_note->notes.size()) - 1;
                }
                if (sel >= 0) {
                    ListView_SetItemState(hlist, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                }
            }
            return TRUE;
        }
        case IDOK:
        case IDCANCEL:
            EndDialog(hdlg, IDOK);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void ShowNotePropertiesDialog(CDedLevel* level)
{
    auto& sel = level->selection;
    g_selected_notes.clear();
    for (int i = 0; i < sel.get_size(); i++) {
        DedObject* obj = sel[i];
        if (obj && obj->type == DedObjectType::DED_NOTE) {
            g_selected_notes.push_back(static_cast<DedNote*>(obj));
        }
    }

    if (!g_selected_notes.empty()) {
        DialogBoxParam(
            reinterpret_cast<HINSTANCE>(&__ImageBase),
            MAKEINTRESOURCE(IDD_ALPINE_NOTE_PROPERTIES),
            GetActiveWindow(),
            NoteDialogProc,
            0
        );
    }

    g_selected_notes.clear();
}

// ─── Object Lifecycle ───────────────────────────────────────────────────────

void PlaceNewNoteObject()
{
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto* note = new DedNote();
    memset(static_cast<DedObject*>(note), 0, sizeof(DedObject));
    note->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
    note->type = DedObjectType::DED_NOTE;

    note->script_name.assign_0("Note");

    void* viewport = get_active_viewport();
    if (viewport) {
        void* view_data = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(viewport) + 0x54);
        if (view_data) {
            auto* cam_pos = reinterpret_cast<Vector3*>(reinterpret_cast<uintptr_t>(view_data) + 0x28);
            note->pos = *cam_pos;
        }
    }

    note->orient.rvec = {1.0f, 0.0f, 0.0f};
    note->orient.uvec = {0.0f, 1.0f, 0.0f};
    note->orient.fvec = {0.0f, 0.0f, 1.0f};

    note->uid = generate_uid();

    level->GetAlpineLevelProperties().note_objects.push_back(note);
    level->master_objects.add(static_cast<DedObject*>(note));

    level->clear_selection();
    level->add_to_selection(static_cast<DedObject*>(note));
    level->update_console_display();
}

DedNote* CloneNoteObject(DedNote* source, bool add_to_level)
{
    if (!source) return nullptr;

    auto* note = new DedNote();
    memset(static_cast<DedObject*>(note), 0, sizeof(DedObject));
    note->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
    note->type = DedObjectType::DED_NOTE;

    note->pos = source->pos;
    note->orient = source->orient;
    note->script_name.assign_0(source->script_name.c_str());
    note->notes = source->notes;

    note->uid = generate_uid();

    if (add_to_level) {
        auto* level = CDedLevel::Get();
        if (level) {
            level->GetAlpineLevelProperties().note_objects.push_back(note);
            level->master_objects.add(static_cast<DedObject*>(note));
        }
    }

    return note;
}

void DeleteNoteObject(DedNote* note)
{
    if (!note) return;
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto& notes = level->GetAlpineLevelProperties().note_objects;
    auto it = std::find(notes.begin(), notes.end(), note);
    if (it != notes.end()) {
        notes.erase(it);
    }
    level->master_objects.remove_by_value(static_cast<DedObject*>(note));
    destroy_ded_note(note);
}

// ─── Handler functions called from mesh.cpp hooks ───────────────────────────

void note_render(CDedLevel* level)
{
    auto& notes = level->GetAlpineLevelProperties().note_objects;
    if (notes.empty()) return;

    note_load_icon();

    float cam_param = *reinterpret_cast<float*>(0x014cf7e0);

    for (auto* note : notes) {
        if (note->hidden_in_editor) continue;

        bool selected = false;
        auto& sel = level->selection;
        for (int i = 0; i < sel.size; i++) {
            if (sel.data_ptr[i] == static_cast<DedObject*>(note)) {
                selected = true;
                break;
            }
        }

        if (selected) {
            set_draw_color(0xff, 0x00, 0x00, 0xff);
        }
        else {
            set_draw_color(0xff, 0xff, 0xff, 0xff);
        }

        if (g_note_icon_handle >= 0) {
            gr_set_bitmap(g_note_icon_handle, -1);
        }

        gr_render_billboard(&note->pos, 0, 0.25f, cam_param);
    }
}

void note_pick(CDedLevel* level, int param1, int param2)
{
    auto& notes = level->GetAlpineLevelProperties().note_objects;
    for (auto* note : notes) {
        if (note->hidden_in_editor) continue;
        bool hit = level->hit_test_point(param1, param2, &note->pos);
        if (hit) {
            level->select_object(static_cast<DedObject*>(note));
        }
    }
}

DedNote* note_click_pick(CDedLevel* level, float click_x, float click_y)
{
    auto& notes = level->GetAlpineLevelProperties().note_objects;
    float best_dist_sq = 1e30f;
    DedNote* best_note = nullptr;

    for (auto* note : notes) {
        if (note->hidden_in_editor) continue;

        float center_pos[3] = {note->pos.x, note->pos.y, note->pos.z};
        float screen_cx = 0.0f, screen_cy = 0.0f;
        if (!project_to_screen_2d(center_pos, &screen_cx, &screen_cy))
            continue;

        constexpr float screen_radius_sq = 400.0f; // 20px

        float dx = screen_cx - click_x;
        float dy = screen_cy - click_y;
        float dist_sq = dx * dx + dy * dy;
        if (dist_sq <= screen_radius_sq && dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            best_note = note;
        }
    }

    return best_note;
}

void note_tree_populate(EditorTreeCtrl* tree, int master_groups, CDedLevel* level)
{
    auto& notes = level->GetAlpineLevelProperties().note_objects;

    char buf[64];
    snprintf(buf, sizeof(buf), "Notes (%d)", static_cast<int>(notes.size()));
    int parent = tree->insert_item(buf, master_groups, 0xffff0002);

    for (auto* note : notes) {
        const char* name = note->script_name.c_str();
        if (!name || name[0] == '\0') {
            name = "(unnamed note)";
        }
        int child = tree->insert_item(name, parent, 0xffff0002);
        tree->set_item_data(child, note->uid);
    }
}

void note_tree_add_object_type(EditorTreeCtrl* tree)
{
    tree->insert_item("Note", 0xffff0000, 0xffff0002);
}

bool note_copy_object(DedObject* source)
{
    if (!source || source->type != DedObjectType::DED_NOTE) return false;
    auto* staged = CloneNoteObject(static_cast<DedNote*>(source), false);
    if (staged) {
        g_note_clipboard.push_back(staged);
        return true;
    }
    return false;
}

void note_paste_objects(CDedLevel* level)
{
    for (auto* staged : g_note_clipboard) {
        auto* clone = CloneNoteObject(staged, true);
        if (clone) {
            level->add_to_selection(static_cast<DedObject*>(clone));
        }
    }
}

void note_clear_clipboard()
{
    for (auto* note : g_note_clipboard) {
        destroy_ded_note(note);
    }
    g_note_clipboard.clear();
}

void note_handle_delete_or_cut(DedObject* obj)
{
    if (!obj || obj->type != DedObjectType::DED_NOTE) return;
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto& note_objects = level->GetAlpineLevelProperties().note_objects;
    auto it = std::find(note_objects.begin(), note_objects.end(), static_cast<DedNote*>(obj));
    if (it != note_objects.end()) {
        note_objects.erase(it);
    }
}

void note_handle_delete_selection(CDedLevel* level)
{
    auto& sel = level->selection;
    for (int i = sel.size - 1; i >= 0; i--) {
        DedObject* obj = sel.data_ptr[i];
        if (obj && obj->type == DedObjectType::DED_NOTE) {
            for (int j = i; j < sel.size - 1; j++) {
                sel.data_ptr[j] = sel.data_ptr[j + 1];
            }
            sel.size--;
            DeleteNoteObject(static_cast<DedNote*>(obj));
        }
    }
}

void note_ensure_uid(int& uid)
{
    auto* level = CDedLevel::Get();
    if (!level) return;
    for (auto* n : level->GetAlpineLevelProperties().note_objects) {
        if (n->uid >= uid) uid = n->uid + 1;
    }
}

