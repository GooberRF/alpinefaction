#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <cstdint>
#include <algorithm>
#include <map>
#include <set>
#include <vector>
#include <string>
#include <xlog/xlog.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/AsmWriter.h>
#include "alpine_obj.h"
#include "mesh.h"
#include "note.h"
#include "corona.h"
#include "mfc_types.h"
#include "level.h"
#include "vtypes.h"
#include "resources.h"

// ─── Utilities ──────────────────────────────────────────────────────────────

// Remove a specific object from a VArray selection
static void remove_from_selection(VArray<DedObject*>& sel, DedObject* obj)
{
    for (int i = 0; i < sel.size; i++) {
        if (sel.data_ptr[i] == obj) {
            for (int j = i; j < sel.size - 1; j++) {
                sel.data_ptr[j] = sel.data_ptr[j + 1];
            }
            sel.size--;
            return;
        }
    }
}

// ─── UID Generation ─────────────────────────────────────────────────────────

// Hook stock UID generator so it also considers Alpine object UIDs.
// Stock generate_uid scans brushes/objects for max UID and returns max+1,
// but doesn't know about Alpine objects.
FunHook<int()> alpine_generate_uid_hook{
    0x00484230,
    []() {
        int uid = alpine_generate_uid_hook.call_target();
        auto* level = CDedLevel::Get();
        if (level) {
            mesh_ensure_uid(uid);
            note_ensure_uid(uid);
            corona_ensure_uid(uid);
        }
        return uid;
    },
};

// ─── Properties Dialog Dispatch ──────────────────────────────────────────────

// Hook into object properties dialog dispatcher at the type switch point.
// At 0x0040200e: EAX = object type, ESI = CDedLevel*.
// For Alpine types, show the appropriate dialog and skip stock code.
CodeInjection alpine_properties_patch{
    0x0040200e,
    [](auto& regs) {
        if (regs.eax == static_cast<int>(DedObjectType::DED_NOTE)) {
            auto* level = reinterpret_cast<CDedLevel*>(static_cast<uintptr_t>(regs.esi));
            ShowNotePropertiesDialog(level);
            regs.eip = 0x00402293;
            return;
        }
        if (regs.eax == static_cast<int>(DedObjectType::DED_MESH)) {
            auto* level = reinterpret_cast<CDedLevel*>(static_cast<uintptr_t>(regs.esi));
            ShowMeshPropertiesForSelection(level);
            regs.eip = 0x00402293;
            return;
        }
        if (regs.eax == static_cast<int>(DedObjectType::DED_CORONA)) {
            auto* level = reinterpret_cast<CDedLevel*>(static_cast<uintptr_t>(regs.esi));
            ShowCoronaPropertiesDialog(level);
            regs.eip = 0x00402293;
            return;
        }
    },
};

// ─── Tree View ──────────────────────────────────────────────────────────────

// Hook the tree view population function (FUN_00440590) to add Alpine object sections.
// Inject just before the finalization call FUN_00442250.
// At this point: ESI = tree control (this+0x5c), EDI = panel object.
CodeInjection alpine_tree_patch{
    0x00441c89,
    [](auto& regs) {
        auto* level = CDedLevel::Get();
        if (!level) return;

        auto* tree = reinterpret_cast<EditorTreeCtrl*>(static_cast<uintptr_t>(regs.esi));
        int master_groups = *reinterpret_cast<int*>(regs.edi + 0x98);

        mesh_tree_populate(tree, master_groups, level);
        note_tree_populate(tree, master_groups, level);
        corona_tree_populate(tree, master_groups, level);
        tree->sort_children(master_groups);
    },
};

// ─── Object Picking ─────────────────────────────────────────────────────────

// Hook the object picking function FUN_0042ae80, just before it calls FUN_00423460
// (console display). This ensures Alpine objects are in the selection when the console
// display runs. At 0x0042aeea: EBX = CDedLevel*, stack has pick params.
CodeInjection alpine_pick_patch{
    0x0042aeea,
    [](auto& regs) {
        auto* level = reinterpret_cast<CDedLevel*>(static_cast<uintptr_t>(regs.ebx));
        if (!level) return;

        int param1 = *reinterpret_cast<int*>(regs.esp + 0x10);
        int param2 = *reinterpret_cast<int*>(regs.esp + 0x14);

        mesh_pick(level, param1, param2);
        note_pick(level, param1, param2);
        corona_pick(level, param1, param2);
    },
};

// Hook the click-pick handler FUN_0042bd10 at the CALL to FUN_0042b880 (ray-pick).
// We call stock ray-pick ourselves, then check Alpine objects if nothing found.
CodeInjection alpine_click_pick_patch{
    0x0042bd1f,
    [](auto& regs) {
        auto* level = reinterpret_cast<CDedLevel*>(static_cast<uintptr_t>(regs.esi));
        if (!level) return;

        uintptr_t esp_val = static_cast<uintptr_t>(regs.esp);
        uintptr_t p1 = *reinterpret_cast<uintptr_t*>(esp_val);
        uintptr_t p2 = *reinterpret_cast<uintptr_t*>(esp_val + 0x04);

        // Call stock ray-pick ourselves
        void* picked = level->pick_object(p1, p2);

        if (!picked) {
            auto* click_ptr = reinterpret_cast<float*>(p1);
            float click_x = click_ptr[0];
            float click_y = click_ptr[1];

            // Check mesh objects using bounding sphere
            float mesh_dist_sq = 1e30f;
            DedMesh* best_mesh = mesh_click_pick(level, click_x, click_y, &mesh_dist_sq);

            // Check note objects using fixed screen radius
            DedNote* best_note = note_click_pick(level, click_x, click_y);

            // Check corona objects using fixed screen radius
            DedCorona* best_corona = corona_click_pick(level, click_x, click_y);

            // Determine best Alpine hit
            DedObject* best_alpine = nullptr;
            float best_dist_sq = 1e30f;
            if (best_mesh) {
                best_alpine = static_cast<DedObject*>(best_mesh);
                best_dist_sq = mesh_dist_sq;
            }
            if (best_note) {
                float note_pos[3] = {best_note->pos.x, best_note->pos.y, best_note->pos.z};
                float nsx = 0.0f, nsy = 0.0f;
                if (project_to_screen_2d(note_pos, &nsx, &nsy)) {
                    float ndx = nsx - click_x, ndy = nsy - click_y;
                    float note_dist = ndx * ndx + ndy * ndy;
                    if (!best_alpine || note_dist < best_dist_sq) {
                        best_alpine = static_cast<DedObject*>(best_note);
                        best_dist_sq = note_dist;
                    }
                }
            }
            if (best_corona) {
                float corona_pos[3] = {best_corona->pos.x, best_corona->pos.y, best_corona->pos.z};
                float csx = 0.0f, csy = 0.0f;
                if (project_to_screen_2d(corona_pos, &csx, &csy)) {
                    float cdx = csx - click_x, cdy = csy - click_y;
                    float corona_dist = cdx * cdx + cdy * cdy;
                    if (!best_alpine || corona_dist < best_dist_sq) {
                        best_alpine = static_cast<DedObject*>(best_corona);
                        best_dist_sq = corona_dist;
                    }
                }
            }

            if (best_alpine) {
                uint8_t shift = *reinterpret_cast<uint8_t*>(esp_val + 0x18);
                if (!shift) {
                    level->deselect_all();
                }
                bool was_selected = false;
                if (shift) {
                    auto& sel = level->selection;
                    for (int i = 0; i < sel.size; i++) {
                        if (sel.data_ptr[i] == best_alpine) {
                            remove_from_selection(sel, best_alpine);
                            was_selected = true;
                            break;
                        }
                    }
                }
                if (!was_selected) {
                    level->select_object(best_alpine);
                }

                regs.esp = static_cast<int32_t>(esp_val + 8);
                regs.eip = 0x0042bd4f;
                return;
            }
        }

        // Stock object found, or nothing found at all — let stock code handle it
        regs.eax = reinterpret_cast<uintptr_t>(picked);
        regs.esp = static_cast<int32_t>(esp_val + 8);
        regs.eip = 0x0042bd24;
    },
};

// ─── Copy / Paste ───────────────────────────────────────────────────────────

// Hook the start of FUN_00412e20 to clear all Alpine clipboards.
CodeInjection alpine_copy_begin_hook{
    0x00412e20,
    [](auto& /*regs*/) {
        mesh_clear_clipboard();
        note_clear_clipboard();
        corona_clear_clipboard();
    },
};

// Hook the copy function FUN_00412e20 at 0x00412ea1 (MOV EBX,[EAX] + CMP = 6 bytes).
// For Alpine objects, stage a clone to the appropriate clipboard.
CodeInjection alpine_copy_hook{
    0x00412ea1,
    [](auto& regs) {
        auto* source = reinterpret_cast<DedObject*>(
            *reinterpret_cast<uintptr_t*>(static_cast<uintptr_t>(regs.eax)));
        if (source && source->type == DedObjectType::DED_MESH) {
            regs.ebx = reinterpret_cast<uintptr_t>(source);
            mesh_copy_object(source);
            regs.eip = 0x00412edb;
        }
        else if (source && source->type == DedObjectType::DED_NOTE) {
            regs.ebx = reinterpret_cast<uintptr_t>(source);
            note_copy_object(source);
            regs.eip = 0x00412edb;
        }
        else if (source && source->type == DedObjectType::DED_CORONA) {
            regs.ebx = reinterpret_cast<uintptr_t>(source);
            corona_copy_object(source);
            regs.eip = 0x00412edb;
        }
    },
};

// Wrapper for the paste function (FUN_00413050, called from Ctrl+V via thunk at 0x00448650).
// After the stock paste function processes clones, we create Alpine clones from clipboards.
static void __fastcall alpine_paste_wrapper(void* ecx_level, void* /*edx_unused*/)
{
    auto* level = reinterpret_cast<CDedLevel*>(ecx_level);
    if (!level) return;

    level->paste_objects();
    mesh_paste_objects(level);
    note_paste_objects(level);
    corona_paste_objects(level);
}

// ─── Delete / Cut ───────────────────────────────────────────────────────────

// Flags to detect delete and cut operations in FUN_0041be70.
static bool g_alpine_delete_mode = false;
static bool g_alpine_cut_mode = false;

// Hook FUN_0041bd00 to detect delete/cut mode before FUN_0041bbb0 processes selection items.
CodeInjection alpine_delete_mode_patch{
    0x0041bd00,
    [](auto& regs) {
        auto esp_val = static_cast<uintptr_t>(regs.esp);
        auto param_2 = *reinterpret_cast<int*>(esp_val + 4);
        auto* level = reinterpret_cast<CDedLevel*>(static_cast<uintptr_t>(regs.ecx));
        auto mode = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(level) + 0xf8);
        g_alpine_delete_mode = (mode == 4 && param_2 == 1);
        g_alpine_cut_mode = (mode == 4 && param_2 == 0);
    },
};

// Hook FUN_0041be70 to handle Alpine objects during cut finalization and delete.
CodeInjection alpine_paste_finalize_patch{
    0x0041be70,
    [](auto& regs) {
        auto esp_val = static_cast<uintptr_t>(regs.esp);
        auto* obj = reinterpret_cast<DedObject*>(
            *reinterpret_cast<uintptr_t*>(esp_val + 4));
        if (obj && obj->type == DedObjectType::DED_MESH) {
            if (g_alpine_delete_mode || g_alpine_cut_mode) {
                mesh_handle_delete_or_cut(obj);
            }
        }
        else if (obj && obj->type == DedObjectType::DED_NOTE) {
            if (g_alpine_delete_mode || g_alpine_cut_mode) {
                note_handle_delete_or_cut(obj);
            }
        }
        else if (obj && obj->type == DedObjectType::DED_CORONA) {
            if (g_alpine_delete_mode || g_alpine_cut_mode) {
                corona_handle_delete_or_cut(obj);
            }
        }
    },
};

// Hook the delete command handler (command ID 0x8018) at 0x00448690.
// Before stock code runs, remove Alpine objects from the selection and delete them.
CodeInjection alpine_delete_patch{
    0x00448690,
    [](auto& regs) {
        auto* level = CDedLevel::Get();
        if (!level) return;

        note_handle_delete_selection(level);
        mesh_handle_delete_selection(level);
        corona_handle_delete_selection(level);
    },
};

// ─── Object Mode Tree / Factory ─────────────────────────────────────────────

// Hook the Object mode tree view (FUN_00443610) to add Alpine type entries.
CodeInjection alpine_object_tree_patch{
    0x004442b7,
    [](auto& regs) {
        auto* tree = reinterpret_cast<EditorTreeCtrl*>(static_cast<uintptr_t>(regs.esi));
        mesh_tree_add_object_type(tree);
        note_tree_add_object_type(tree);
        corona_tree_add_object_type(tree);
        tree->sort_children(static_cast<int>(reinterpret_cast<intptr_t>(TVI_ROOT)));
    },
};

// Track which Alpine object type the tree view is creating.
static int g_alpine_create_type = 0; // 0=Mesh, 2=Note, 3=Corona

// Hook factory FUN_00442a40 to detect Alpine object types by tree item text.
int __fastcall alpine_factory_hooked(void* ecx_panel, void* /*edx*/, void* tree_item);
FunHook<decltype(alpine_factory_hooked)> alpine_factory_hook{
    0x00442a40,
    alpine_factory_hooked,
};
int __fastcall alpine_factory_hooked(void* ecx_panel, void* edx, void* tree_item)
{
    g_alpine_create_type = 0;

    HWND hTree = *reinterpret_cast<HWND*>(reinterpret_cast<uintptr_t>(ecx_panel) + 0x5c + 0x1c);
    if (hTree && tree_item) {
        char text[64] = {};
        TVITEMA tvi = {};
        tvi.mask = TVIF_TEXT;
        tvi.hItem = static_cast<HTREEITEM>(tree_item);
        tvi.pszText = text;
        tvi.cchTextMax = sizeof(text);
        TreeView_GetItem(hTree, &tvi);
        if (strcmp(text, "Note") == 0) {
            g_alpine_create_type = 2;
        }
        else if (strcmp(text, "Corona") == 0) {
            g_alpine_create_type = 3;
        }
    }

    return alpine_factory_hook.call_target(ecx_panel, edx, tree_item);
}

// Hook the object creation call site in FUN_004431c0. The factory returns NULL for
// unknown types. Create the appropriate Alpine object when NULL is returned.
CodeInjection alpine_create_object_patch{
    0x004432f5,
    [](auto& regs) {
        if (regs.eax == 0) {
            if (g_alpine_create_type == 2) {
                PlaceNewNoteObject();
            }
            else if (g_alpine_create_type == 3) {
                PlaceNewCoronaObject();
            }
            else {
                PlaceNewMeshObject();
            }
            g_alpine_create_type = 0;
            regs.eip = 0x004435be;
        }
    },
};

// ─── Rendering ──────────────────────────────────────────────────────────────

// Hook into the editor's 3D render function to render Alpine objects.
// Inject after the main object render loop in FUN_0041f6d0, before the icon pass.
CodeInjection alpine_render_patch{
    0x0041f9b2,
    [](auto& regs) {
        auto* level = CDedLevel::Get();
        if (!level) return;

        mesh_render(level);
        note_render(level);
        corona_render(level);
    },
};

// ─── Select Objects / Hide Objects ──────────────────────────────────────────

static const char* get_type_display_name(DedObjectType type)
{
    switch (type) {
        case DedObjectType::DED_CLUTTER:            return "Clutter";
        case DedObjectType::DED_ENTITY:             return "Entity";
        case DedObjectType::DED_ITEM:               return "Item";
        case DedObjectType::DED_RESPAWN_POINT:      return "Respawn Point";
        case DedObjectType::DED_TRIGGER:            return "Trigger";
        case DedObjectType::DED_AMBIENT_SOUND:      return "Ambient Sound";
        case DedObjectType::DED_LIGHT:              return "Light";
        case DedObjectType::DED_GEO_REGION:         return "Geo Region";
        case DedObjectType::DED_NAV_POINT:          return "Nav Point";
        case DedObjectType::DED_EVENT:              return "Event";
        case DedObjectType::DED_CUTSCENE_CAMERA:    return "Cutscene Camera";
        case DedObjectType::DED_CUTSCENE_PATH_NODE: return "Cutscene Path Node";
        case DedObjectType::DED_PARTICLE_EMITTER:   return "Particle Emitter";
        case DedObjectType::DED_GAS_REGION:         return "Gas Region";
        case DedObjectType::DED_ROOM_EFFECT:        return "Room Effect";
        case DedObjectType::DED_EAX_EFFECT:         return "EAX Effect";
        case DedObjectType::DED_CLIMBING_REGION:    return "Climbing Region";
        case DedObjectType::DED_DECAL:              return "Decal";
        case DedObjectType::DED_BOLT_EMITTER:       return "Bolt Emitter";
        case DedObjectType::DED_TARGET:             return "Target";
        case DedObjectType::DED_KEYFRAME:           return "Keyframe";
        case DedObjectType::DED_PUSH_REGION:        return "Push Region";
        case DedObjectType::DED_MESH:               return "Mesh";
        case DedObjectType::DED_NOTE:               return "Note";
        case DedObjectType::DED_CORONA:             return "Corona";
        default:                                    return "Unknown";
    }
}

// ─── Select Objects / Show-Hide Objects (matches stock two-panel layout) ─────

// Type filter entries — alphabetical, matching stock + Alpine types
static const struct { DedObjectType type; const char* label; } g_type_filters[] = {
    {DedObjectType::DED_AMBIENT_SOUND,    "Ambient Sounds"},
    {DedObjectType::DED_BOLT_EMITTER,     "Bolt Emitters"},
    {DedObjectType::DED_CLIMBING_REGION,  "Climbing Regions"},
    {DedObjectType::DED_CLUTTER,          "Clutter"},
    {DedObjectType::DED_CORONA,           "Coronas"},
    {DedObjectType::DED_CUTSCENE_CAMERA,  "Cutscene Cameras"},
    {DedObjectType::DED_DECAL,            "Decals"},
    {DedObjectType::DED_EAX_EFFECT,       "EAX Effects"},
    {DedObjectType::DED_ENTITY,           "Entities"},
    {DedObjectType::DED_EVENT,            "Events"},
    {DedObjectType::DED_GAS_REGION,       "Gas Regions"},
    {DedObjectType::DED_GEO_REGION,       "Geo Regions"},
    {DedObjectType::DED_ITEM,             "Items"},
    {DedObjectType::DED_KEYFRAME,         "Keyframes"},
    {DedObjectType::DED_LIGHT,            "Lights"},
    {DedObjectType::DED_MESH,             "Meshes"},
    {DedObjectType::DED_NAV_POINT,        "Nav Points"},
    {DedObjectType::DED_NOTE,             "Notes"},
    {DedObjectType::DED_PARTICLE_EMITTER, "Particle Emitters"},
    {DedObjectType::DED_PUSH_REGION,      "Push Regions"},
    {DedObjectType::DED_RESPAWN_POINT,    "Respawns"},
    {DedObjectType::DED_ROOM_EFFECT,      "Room Effects"},
    {DedObjectType::DED_TARGET,           "Targets"},
    {DedObjectType::DED_TRIGGER,          "Triggers"},
};
constexpr int g_num_type_filters = sizeof(g_type_filters) / sizeof(g_type_filters[0]);

// Persistent filter checkbox state across dialog invocations (within same session)
static bool g_filter_state_initialized = false;
static bool g_filter_states[30] = {}; // indexed by g_type_filters position

struct ObjectEntry {
    DedObject* obj;
    std::string text; // "CLASS_NAME : SCRIPT_NAME (UID)"
    DedObjectType type;
    bool initially_visible; // for Hide mode: was not hidden when dialog opened
    bool initially_selected; // for Select mode: was selected when dialog opened
};

struct TypeFilterDialogData {
    const char* title;
    bool hide_mode;     // false = Select (multi-select), true = Show/Hide (checkboxes)
    bool updating_checks; // guard against recursive LVN_ITEMCHANGED
    CDedLevel* level;
    std::vector<ObjectEntry> all_objects;
    HWND filter_cbs[30]; // checkbox HWNDs for "Show In List"
    int sort_mode;       // 0 = name, 1 = type
    std::vector<DedObject*> result_objects;
};

// Collect all objects from master_objects + moving_groups
static void collect_all_objects(CDedLevel* level, bool include_hidden,
    std::vector<ObjectEntry>& out)
{
    out.clear();

    // Build set of currently selected objects for pre-selection
    std::set<DedObject*> selected_set;
    auto& sel = level->selection;
    for (int i = 0; i < sel.size; i++)
        selected_set.insert(sel.data_ptr[i]);

    auto add_obj = [&](DedObject* obj) {
        if (!obj) return;
        if (!include_hidden && obj->hidden_in_editor) return;
        char buf[256];
        const char* cls = obj->class_name.c_str();
        const char* scr = obj->script_name.c_str();
        if (cls[0] && scr[0])
            snprintf(buf, sizeof(buf), "%s : %s (%d)", cls, scr, obj->uid);
        else if (cls[0])
            snprintf(buf, sizeof(buf), "%s (%d)", cls, obj->uid);
        else if (scr[0])
            snprintf(buf, sizeof(buf), "%s : %s (%d)", get_type_display_name(obj->type), scr, obj->uid);
        else
            snprintf(buf, sizeof(buf), "%s (%d)", get_type_display_name(obj->type), obj->uid);
        out.push_back({obj, buf, obj->type, !obj->hidden_in_editor, selected_set.count(obj) > 0});
    };

    auto& master = level->master_objects;
    for (int i = 0; i < master.size; i++)
        add_obj(master.data_ptr[i]);

    auto& mg = level->moving_groups;
    for (int i = 0; i < mg.size; i++) {
        auto* group = mg.data_ptr[i];
        if (!group || !group->keyframes) continue;
        auto& kfs = *group->keyframes;
        for (int j = 0; j < kfs.size; j++)
            add_obj(kfs.data_ptr[j]);
    }
}

// Check if a type is visible in the "Show In List" filter
static bool is_type_filtered_in(TypeFilterDialogData* data, DedObjectType type)
{
    for (int i = 0; i < g_num_type_filters; i++) {
        if (g_type_filters[i].type == type)
            return SendMessage(data->filter_cbs[i], BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    return true; // types not in filter list are always shown
}

// Rebuild the left object list based on current filters and sort mode
static void refresh_object_list(HWND hwnd, TypeFilterDialogData* data)
{
    HWND list = GetDlgItem(hwnd, IDC_TYPE_FILTER_LIST);

    // Save check/selection states before clearing
    std::map<DedObject*, bool> check_states;
    std::map<DedObject*, bool> sel_states;
    {
        int count = ListView_GetItemCount(list);
        for (int i = 0; i < count; i++) {
            LVITEM lvi = {};
            lvi.mask = LVIF_PARAM;
            lvi.iItem = i;
            ListView_GetItem(list, &lvi);
            if (!lvi.lParam) continue;
            auto* obj = reinterpret_cast<DedObject*>(lvi.lParam);
            if (data->hide_mode)
                check_states[obj] = ListView_GetCheckState(list, i) != 0;
            else
                sel_states[obj] = (ListView_GetItemState(list, i, LVIS_SELECTED) & LVIS_SELECTED) != 0;
        }
    }

    // Filter
    std::vector<const ObjectEntry*> filtered;
    for (auto& e : data->all_objects) {
        if (is_type_filtered_in(data, e.type))
            filtered.push_back(&e);
    }

    // Sort
    if (data->sort_mode == 0) {
        std::sort(filtered.begin(), filtered.end(),
            [](auto* a, auto* b) { return a->text < b->text; });
    } else {
        std::sort(filtered.begin(), filtered.end(), [](auto* a, auto* b) {
            if (a->type != b->type) return a->type < b->type;
            return a->text < b->text;
        });
    }

    // Repopulate
    SendMessage(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    for (size_t i = 0; i < filtered.size(); i++) {
        LVITEM item = {};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<char*>(filtered[i]->text.c_str());
        item.lParam = reinterpret_cast<LPARAM>(filtered[i]->obj);
        ListView_InsertItem(list, &item);

        if (data->hide_mode) {
            // Restore check state
            auto it = check_states.find(filtered[i]->obj);
            bool checked = (it != check_states.end()) ? it->second : filtered[i]->initially_visible;
            ListView_SetCheckState(list, static_cast<int>(i), checked);
        } else {
            // Restore selection state, or use initial selection from editor
            auto it = sel_states.find(filtered[i]->obj);
            bool selected = (it != sel_states.end()) ? it->second : filtered[i]->initially_selected;
            if (selected)
                ListView_SetItemState(list, static_cast<int>(i), LVIS_SELECTED, LVIS_SELECTED);
        }
    }
    SendMessage(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);
}

// ─── ListView subclass for click-drag multi-select ──────────────────────────
// Stock editor used CListBox with LBS_EXTENDEDSEL which natively supports
// click-drag selection. ListView doesn't support this, so we subclass to
// handle mouse drag and select the range from anchor to current item.

static WNDPROC g_orig_listview_proc = nullptr;
static int g_drag_anchor = -1;
static bool g_drag_active = false;
static int g_drag_last_idx = -1;
static UINT_PTR g_scroll_timer = 0;
static constexpr UINT_PTR SCROLL_TIMER_ID = 9001;
static constexpr int SCROLL_INTERVAL_MS = 50;

static void drag_update_selection(HWND hwnd, int current_idx)
{
    if (current_idx < 0 || g_drag_anchor < 0 || current_idx == g_drag_last_idx) return;
    g_drag_last_idx = current_idx;

    int lo = std::min(g_drag_anchor, current_idx);
    int hi = std::max(g_drag_anchor, current_idx);
    int count = ListView_GetItemCount(hwnd);

    SendMessage(hwnd, WM_SETREDRAW, FALSE, 0);
    for (int i = 0; i < count; i++) {
        UINT want = (i >= lo && i <= hi) ? LVIS_SELECTED : 0;
        ListView_SetItemState(hwnd, i, want, LVIS_SELECTED);
    }
    SendMessage(hwnd, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);

    ListView_EnsureVisible(hwnd, current_idx, FALSE);
}

static int drag_hit_test_with_scroll(HWND hwnd, int y)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int count = ListView_GetItemCount(hwnd);
    if (y < rc.top) {
        int top = ListView_GetTopIndex(hwnd);
        return std::max(0, top - 1);
    }
    if (y >= rc.bottom) {
        int top = ListView_GetTopIndex(hwnd);
        int per_page = ListView_GetCountPerPage(hwnd);
        return std::min(count - 1, top + per_page);
    }
    LVHITTESTINFO ht = {};
    ht.pt = {rc.left + (rc.right - rc.left) / 2, y};
    return ListView_HitTest(hwnd, &ht);
}

static LRESULT CALLBACK ListViewDragSelectProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_LBUTTONDOWN: {
        if (wparam & (MK_SHIFT | MK_CONTROL)) break;
        LVHITTESTINFO ht = {};
        ht.pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        int idx = ListView_HitTest(hwnd, &ht);
        if (idx >= 0) {
            // Let checkbox clicks pass through to default handler
            if (ht.flags & LVHT_ONITEMSTATEICON)
                break;

            int count = ListView_GetItemCount(hwnd);
            SendMessage(hwnd, WM_SETREDRAW, FALSE, 0);
            for (int i = 0; i < count; i++)
                ListView_SetItemState(hwnd, i, 0, LVIS_SELECTED);
            ListView_SetItemState(hwnd, idx, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            SendMessage(hwnd, WM_SETREDRAW, TRUE, 0);
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);

            g_drag_anchor = idx;
            g_drag_last_idx = idx;
            g_drag_active = true;
            SetCapture(hwnd);
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE: {
        if (!g_drag_active || !(wparam & MK_LBUTTON)) break;
        int y = GET_Y_LPARAM(lparam);
        int idx = drag_hit_test_with_scroll(hwnd, y);
        drag_update_selection(hwnd, idx);

        RECT rc;
        GetClientRect(hwnd, &rc);
        if (y < rc.top || y >= rc.bottom) {
            if (!g_scroll_timer)
                g_scroll_timer = SetTimer(hwnd, SCROLL_TIMER_ID, SCROLL_INTERVAL_MS, nullptr);
        } else if (g_scroll_timer) {
            KillTimer(hwnd, SCROLL_TIMER_ID);
            g_scroll_timer = 0;
        }
        return 0;
    }
    case WM_TIMER: {
        if (wparam == SCROLL_TIMER_ID && g_drag_active) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            int idx = drag_hit_test_with_scroll(hwnd, pt.y);
            drag_update_selection(hwnd, idx);
            return 0;
        }
        break;
    }
    case WM_LBUTTONUP:
        if (g_drag_active) {
            g_drag_active = false;
            if (g_scroll_timer) { KillTimer(hwnd, SCROLL_TIMER_ID); g_scroll_timer = 0; }
            ReleaseCapture();
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        g_drag_active = false;
        if (g_scroll_timer) { KillTimer(hwnd, SCROLL_TIMER_ID); g_scroll_timer = 0; }
        break;
    }
    return CallWindowProc(g_orig_listview_proc, hwnd, msg, wparam, lparam);
}

static INT_PTR CALLBACK TypeFilterDlgProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    auto* data = reinterpret_cast<TypeFilterDialogData*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_INITDIALOG: {
        data = reinterpret_cast<TypeFilterDialogData*>(lparam);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, lparam);
        SetWindowText(hwnd, data->title);

        // Configure ListView
        HWND list = GetDlgItem(hwnd, IDC_TYPE_FILTER_LIST);
        // Add LVS_SHOWSELALWAYS so selections remain visible when buttons have focus
        LONG style = GetWindowLong(list, GWL_STYLE);
        SetWindowLong(list, GWL_STYLE, style | LVS_SHOWSELALWAYS);
        DWORD ex_style = LVS_EX_FULLROWSELECT;
        if (data->hide_mode) ex_style |= LVS_EX_CHECKBOXES;
        ListView_SetExtendedListViewStyle(list, ex_style);

        // Subclass for click-drag multi-select (replicates LBS_EXTENDEDSEL behavior)
        g_orig_listview_proc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtr(list, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ListViewDragSelectProc)));
        g_drag_active = false;
        g_drag_anchor = -1;
        g_drag_last_idx = -1;
        g_scroll_timer = 0;

        RECT lr;
        GetClientRect(list, &lr);
        LVCOLUMN col = {};
        col.mask = LVCF_WIDTH;
        col.cx = lr.right - lr.left - GetSystemMetrics(SM_CXVSCROLL);
        ListView_InsertColumn(list, 0, &col);

        // Initialize persistent filter state on first use
        if (!g_filter_state_initialized) {
            for (int i = 0; i < g_num_type_filters; i++)
                g_filter_states[i] = true;
            g_filter_state_initialized = true;
        }

        // Create "Show In List" checkboxes dynamically (positions in DLU, converted to pixels)
        HFONT font = reinterpret_cast<HFONT>(SendMessage(hwnd, WM_GETFONT, 0, 0));
        for (int i = 0; i < g_num_type_filters; i++) {
            RECT rc = {230, static_cast<LONG>(12 + i * 9),
                        230 + 108, static_cast<LONG>(12 + i * 9 + 8)};
            MapDialogRect(hwnd, &rc);
            data->filter_cbs[i] = CreateWindow(
                "BUTTON", g_type_filters[i].label,
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                hwnd,
                reinterpret_cast<HMENU>(static_cast<uintptr_t>(IDC_TYPE_FILTER_CB_BASE + i)),
                nullptr, nullptr);
            SendMessage(data->filter_cbs[i], WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
            SendMessage(data->filter_cbs[i], BM_SETCHECK,
                g_filter_states[i] ? BST_CHECKED : BST_UNCHECKED, 0);
        }

        // Create "Check All" and "Uncheck All" buttons below the groupbox bottom border
        {
            RECT rc1 = {230, 294, 230 + 52, 294 + 14};
            RECT rc2 = {284, 294, 284 + 52, 294 + 14};
            MapDialogRect(hwnd, &rc1);
            MapDialogRect(hwnd, &rc2);
            HWND btn_check = CreateWindow(
                "BUTTON", "Check All", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                rc1.left, rc1.top, rc1.right - rc1.left, rc1.bottom - rc1.top,
                hwnd, reinterpret_cast<HMENU>(static_cast<uintptr_t>(IDC_TYPE_FILTER_CHECK_ALL)),
                nullptr, nullptr);
            SendMessage(btn_check, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
            HWND btn_uncheck = CreateWindow(
                "BUTTON", "Uncheck All", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                rc2.left, rc2.top, rc2.right - rc2.left, rc2.bottom - rc2.top,
                hwnd, reinterpret_cast<HMENU>(static_cast<uintptr_t>(IDC_TYPE_FILTER_UNCHECK_ALL)),
                nullptr, nullptr);
            SendMessage(btn_uncheck, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        }

        // Sort by Name initially
        data->sort_mode = 0;
        CheckRadioButton(hwnd, IDC_TYPE_FILTER_SORT_NAME, IDC_TYPE_FILTER_SORT_TYPE,
            IDC_TYPE_FILTER_SORT_NAME);

        if (data->hide_mode) {
            // Hide Jump To / View From in Hide mode
            ShowWindow(GetDlgItem(hwnd, IDC_TYPE_FILTER_JUMP_TO), SW_HIDE);
            ShowWindow(GetDlgItem(hwnd, IDC_TYPE_FILTER_VIEW_FROM), SW_HIDE);
            // Rename buttons for hide mode
            SetDlgItemText(hwnd, IDC_TYPE_FILTER_ALL, "Unhide All");
            SetDlgItemText(hwnd, IDC_TYPE_FILTER_INVERT, "Invert Visibility");
            // Create "Hide All" button below "Unhide All"
            RECT rc_hide = {350, 68, 350 + 55, 68 + 14};
            MapDialogRect(hwnd, &rc_hide);
            HWND btn_hide_all = CreateWindow(
                "BUTTON", "Hide All", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                rc_hide.left, rc_hide.top, rc_hide.right - rc_hide.left, rc_hide.bottom - rc_hide.top,
                hwnd, reinterpret_cast<HMENU>(static_cast<uintptr_t>(IDC_TYPE_FILTER_HIDE_ALL)),
                nullptr, nullptr);
            SendMessage(btn_hide_all, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
            // Move "Invert Visibility" down to make room
            RECT rc_invert = {350, 86, 350 + 55, 86 + 14};
            MapDialogRect(hwnd, &rc_invert);
            SetWindowPos(GetDlgItem(hwnd, IDC_TYPE_FILTER_INVERT), nullptr,
                rc_invert.left, rc_invert.top, rc_invert.right - rc_invert.left,
                rc_invert.bottom - rc_invert.top, SWP_NOZORDER);
        }

        // Collect objects and populate
        collect_all_objects(data->level, data->hide_mode, data->all_objects);
        refresh_object_list(hwnd, data);

        return TRUE;
    }

    case WM_COMMAND: {
        int id = LOWORD(wparam);
        int notify = HIWORD(wparam);

        // "Show In List" checkbox toggled → refresh list
        if (id >= IDC_TYPE_FILTER_CB_BASE &&
            id < IDC_TYPE_FILTER_CB_BASE + g_num_type_filters &&
            notify == BN_CLICKED) {
            refresh_object_list(hwnd, data);
            return TRUE;
        }

        // Sort radio changed
        if ((id == IDC_TYPE_FILTER_SORT_NAME || id == IDC_TYPE_FILTER_SORT_TYPE) &&
            notify == BN_CLICKED) {
            data->sort_mode = (id == IDC_TYPE_FILTER_SORT_TYPE) ? 1 : 0;
            refresh_object_list(hwnd, data);
            return TRUE;
        }

        switch (id) {
        case IDC_TYPE_FILTER_JUMP_TO:
        case IDC_TYPE_FILTER_VIEW_FROM: {
            // Move camera to/near the first selected object
            HWND list = GetDlgItem(hwnd, IDC_TYPE_FILTER_LIST);
            int sel_idx = ListView_GetNextItem(list, -1, LVNI_SELECTED);
            if (sel_idx >= 0) {
                LVITEM lvi = {};
                lvi.mask = LVIF_PARAM;
                lvi.iItem = sel_idx;
                ListView_GetItem(list, &lvi);
                if (lvi.lParam) {
                    auto* obj = reinterpret_cast<DedObject*>(lvi.lParam);
                    auto* vp = get_active_viewport();
                    if (vp && vp->view_data) {
                        if (id == IDC_TYPE_FILTER_VIEW_FROM) {
                            vp->view_data->camera_pos = obj->pos;
                            vp->view_data->camera_orient = obj->orient;
                        } else {
                            // Position camera 5 units behind the object
                            auto& vd = *vp->view_data;
                            auto& fwd = vd.camera_orient.fvec;
                            vd.camera_pos.x = obj->pos.x - fwd.x * 5.0f;
                            vd.camera_pos.y = obj->pos.y - fwd.y * 5.0f;
                            vd.camera_pos.z = obj->pos.z - fwd.z * 5.0f;
                        }
                    }
                }
            }
            // Auto-close dialog with OK
            SendMessage(hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED),
                reinterpret_cast<LPARAM>(GetDlgItem(hwnd, IDOK)));
            return TRUE;
        }
        case IDOK: {
            HWND list = GetDlgItem(hwnd, IDC_TYPE_FILTER_LIST);
            data->result_objects.clear();
            int count = ListView_GetItemCount(list);
            for (int i = 0; i < count; i++) {
                bool include;
                if (data->hide_mode)
                    include = true; // collect ALL objects, check state determines visibility
                else
                    include = (ListView_GetItemState(list, i, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                if (!include) continue;
                LVITEM lvi = {};
                lvi.mask = LVIF_PARAM;
                lvi.iItem = i;
                ListView_GetItem(list, &lvi);
                if (lvi.lParam) {
                    if (data->hide_mode) {
                        // Store check state in the object: checked = visible, unchecked = hidden
                        auto* obj = reinterpret_cast<DedObject*>(lvi.lParam);
                        bool checked = ListView_GetCheckState(list, i) != 0;
                        obj->hidden_in_editor = !checked;
                        if (obj->hidden_in_editor) {
                            remove_from_selection(data->level->selection, obj);
                            if (obj->type == DedObjectType::DED_LIGHT) {
                                *reinterpret_cast<uint8_t*>(
                                    reinterpret_cast<uintptr_t>(obj) + 0x3C) = 1;
                            }
                        }
                    } else {
                        data->result_objects.push_back(reinterpret_cast<DedObject*>(lvi.lParam));
                    }
                }
            }
            // Save filter state for next invocation
            for (int i = 0; i < g_num_type_filters; i++)
                g_filter_states[i] = SendMessage(data->filter_cbs[i], BM_GETCHECK, 0, 0) == BST_CHECKED;
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            // Save filter state even on cancel
            for (int i = 0; i < g_num_type_filters; i++)
                g_filter_states[i] = SendMessage(data->filter_cbs[i], BM_GETCHECK, 0, 0) == BST_CHECKED;
            EndDialog(hwnd, IDCANCEL);
            return TRUE;

        case IDC_TYPE_FILTER_HIDE_ALL: {
            HWND list = GetDlgItem(hwnd, IDC_TYPE_FILTER_LIST);
            int count = ListView_GetItemCount(list);
            data->updating_checks = true;
            for (int i = 0; i < count; i++)
                ListView_SetCheckState(list, i, FALSE);
            data->updating_checks = false;
            return TRUE;
        }
        case IDC_TYPE_FILTER_ALL: {
            HWND list = GetDlgItem(hwnd, IDC_TYPE_FILTER_LIST);
            int count = ListView_GetItemCount(list);
            if (data->hide_mode) {
                data->updating_checks = true;
                for (int i = 0; i < count; i++)
                    ListView_SetCheckState(list, i, TRUE);
                data->updating_checks = false;
            } else {
                for (int i = 0; i < count; i++)
                    ListView_SetItemState(list, i, LVIS_SELECTED, LVIS_SELECTED);
            }
            return TRUE;
        }
        case IDC_TYPE_FILTER_INVERT: {
            HWND list = GetDlgItem(hwnd, IDC_TYPE_FILTER_LIST);
            int count = ListView_GetItemCount(list);
            if (data->hide_mode) {
                data->updating_checks = true;
                for (int i = 0; i < count; i++)
                    ListView_SetCheckState(list, i, !ListView_GetCheckState(list, i));
                data->updating_checks = false;
            } else {
                for (int i = 0; i < count; i++) {
                    UINT s = ListView_GetItemState(list, i, LVIS_SELECTED);
                    ListView_SetItemState(list, i, s ^ LVIS_SELECTED, LVIS_SELECTED);
                }
            }
            return TRUE;
        }
        case IDC_TYPE_FILTER_CHECK_ALL:
            for (int i = 0; i < g_num_type_filters; i++)
                SendMessage(data->filter_cbs[i], BM_SETCHECK, BST_CHECKED, 0);
            refresh_object_list(hwnd, data);
            return TRUE;
        case IDC_TYPE_FILTER_UNCHECK_ALL:
            for (int i = 0; i < g_num_type_filters; i++)
                SendMessage(data->filter_cbs[i], BM_SETCHECK, BST_UNCHECKED, 0);
            refresh_object_list(hwnd, data);
            return TRUE;
        } // switch id
        break;
    } // WM_COMMAND

    case WM_NOTIFY: {
        auto* nmhdr = reinterpret_cast<NMHDR*>(lparam);
        if (nmhdr->idFrom == IDC_TYPE_FILTER_LIST && nmhdr->code == LVN_ITEMCHANGED) {
            auto* nmlv = reinterpret_cast<NMLISTVIEW*>(lparam);
            // Detect checkbox state change (LVIS_STATEIMAGEMASK tracks check state)
            if (data && data->hide_mode && !data->updating_checks &&
                (nmlv->uChanged & LVIF_STATE) &&
                (nmlv->uNewState & LVIS_STATEIMAGEMASK) != (nmlv->uOldState & LVIS_STATEIMAGEMASK))
            {
                // Only propagate if the changed item is selected
                HWND list = nmhdr->hwndFrom;
                if (ListView_GetItemState(list, nmlv->iItem, LVIS_SELECTED) & LVIS_SELECTED) {
                    bool new_checked = ((nmlv->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
                    data->updating_checks = true;
                    int count = ListView_GetItemCount(list);
                    for (int i = 0; i < count; i++) {
                        if (i != nmlv->iItem &&
                            (ListView_GetItemState(list, i, LVIS_SELECTED) & LVIS_SELECTED))
                        {
                            ListView_SetCheckState(list, i, new_checked);
                        }
                    }
                    data->updating_checks = false;
                }
            }
        }
        break;
    } // WM_NOTIFY
    }
    return FALSE;
}

void alpine_select_objects(CDedLevel* level)
{
    TypeFilterDialogData data = {};
    data.title = "Select Objects";
    data.hide_mode = false;
    data.level = level;

    HINSTANCE hinst = GetModuleHandle("AlpineEditor.dll");
    if (!hinst) hinst = GetModuleHandle(nullptr);

    INT_PTR result = DialogBoxParam(hinst, MAKEINTRESOURCE(IDD_TYPE_FILTER),
        GetMainFrameHandle(), TypeFilterDlgProc, reinterpret_cast<LPARAM>(&data));

    if (result != IDOK) return;

    level->deselect_all();
    for (auto* obj : data.result_objects)
        level->select_object(obj);
    level->update_console_display();
}

void alpine_hide_objects(CDedLevel* level)
{
    TypeFilterDialogData data = {};
    data.title = "Show/Hide Objects";
    data.hide_mode = true;
    data.level = level;

    HINSTANCE hinst = GetModuleHandle("AlpineEditor.dll");
    if (!hinst) hinst = GetModuleHandle(nullptr);

    // OK handler applies visibility directly to objects
    DialogBoxParam(hinst, MAKEINTRESOURCE(IDD_TYPE_FILTER),
        GetMainFrameHandle(), TypeFilterDlgProc, reinterpret_cast<LPARAM>(&data));
}

// ─── Group Save / Load (.rfg) ───────────────────────────────────────────────
// The rfg I/O context at CDedLevel+0x50 is an rf::File*, so we reuse the same
// chunk serializers/deserializers used for .rfl level files. This ensures the
// binary format for Alpine objects is identical in both .rfl and .rfg files.

// Global vectors to collect Alpine objects during group serialization.
// FUN_00435630 (per-group serializer) has a switch on obj->type that handles types 0..0x16.
// Alpine types (0x17=DED_MESH, 0x18=DED_NOTE, 0x19=DED_CORONA) fall through and are silently
// dropped. We hook the switch's overflow check (JA at 0x00435a82) to collect them here.
static std::vector<DedMesh*> g_group_save_meshes;
static std::vector<DedNote*> g_group_save_notes;
static std::vector<DedCorona*> g_group_save_coronas;

// Brush UIDs captured in serialization order during group save.
// Used to write brush metadata (geoable/breakable flags) to the .rfg brush group chunk.
static std::vector<int> g_group_save_brush_uids;

// Tracks moving_groups.size before stock group load, so we can find new group entries afterward.
static int g_moving_groups_size_before_load = 0;

// Hook at 0x0041ce4f: beginning of save function, clears collection vectors.
CodeInjection alpine_group_save_clear_hook{
    0x0041ce4f,
    [](auto& regs) {
        g_group_save_meshes.clear();
        g_group_save_notes.clear();
        g_group_save_coronas.clear();
        g_group_save_brush_uids.clear();
    },
};

// Hook at 0x004358af: inside FUN_00435630's brush serialization loop.
// At this point ECX = BrushNode* (loaded at 0x004358ad from VArray element).
// EBP = loop index. Captures each brush's UID in serialization order.
CodeInjection alpine_group_brush_save_capture_hook{
    0x004358af,
    [](auto& regs) {
        auto* brush = reinterpret_cast<BrushNode*>(static_cast<uintptr_t>(regs.ecx));
        g_group_save_brush_uids.push_back(brush->uid);
    },
};

// Hook at 0x00435a82: replaces `JA 0x00435be1` in the type switch of FUN_00435630.
// For types > 0x16, collects Alpine objects and skips to loop continue (0x00435be1).
// For types <= 0x16, falls through to the jump table at 0x00435a88 (original behavior).
CodeInjection alpine_group_type_collect_hook{
    0x00435a82,
    [](auto& regs) {
        auto type = static_cast<int>(regs.ecx); // ECX = obj->type (set at 0x00435a7c)
        if (type > 0x16) {
            auto* obj = reinterpret_cast<DedObject*>(static_cast<uintptr_t>(regs.eax));
            if (type == static_cast<int>(DedObjectType::DED_MESH))
                g_group_save_meshes.push_back(static_cast<DedMesh*>(obj));
            else if (type == static_cast<int>(DedObjectType::DED_NOTE))
                g_group_save_notes.push_back(static_cast<DedNote*>(obj));
            else if (type == static_cast<int>(DedObjectType::DED_CORONA))
                g_group_save_coronas.push_back(static_cast<DedCorona*>(obj));
            regs.eip = 0x00435be1; // skip to loop continue
        }
        // types <= 0x16 fall through to jump table at 0x00435a88
    },
};

// Save hook: append Alpine object chunks after stock data, before file close.
// At 0x0041d11c: ESI = CDocument*, [ESI+0x50] = rf::File*, about to close file.
CodeInjection alpine_group_save_hook{
    0x0041d11c,
    [](auto& regs) {
        // ESI = CDocument*; CDedLevel is at CDocument + 0x60
        auto* level = reinterpret_cast<CDedLevel*>(static_cast<uintptr_t>(regs.esi) + 0x60);
        auto* file = reinterpret_cast<rf::File*>(
            *reinterpret_cast<void**>(static_cast<uintptr_t>(regs.esi) + 0x50));
        auto& props = level->GetAlpineLevelProperties();

        // Temporarily swap the level's object vectors with the collected group objects,
        // then call the same chunk serializers used for .rfl files, then restore.
        if (!g_group_save_meshes.empty()) {
            auto saved = std::move(props.mesh_objects);
            props.mesh_objects.assign(g_group_save_meshes.begin(), g_group_save_meshes.end());
            mesh_serialize_chunk(*level, *file);
            props.mesh_objects = std::move(saved);
        }

        if (!g_group_save_notes.empty()) {
            auto saved = std::move(props.note_objects);
            props.note_objects.assign(g_group_save_notes.begin(), g_group_save_notes.end());
            note_serialize_chunk(*level, *file);
            props.note_objects = std::move(saved);
        }

        if (!g_group_save_coronas.empty()) {
            auto saved = std::move(props.corona_objects);
            props.corona_objects.assign(g_group_save_coronas.begin(), g_group_save_coronas.end());
            corona_serialize_chunk(*level, *file);
            props.corona_objects = std::move(saved);
        }

        // Write brush group metadata chunk: which brushes (by serialization index) are
        // geoable/breakable. This enables .rfg round-tripping of brush properties.
        if (!g_group_save_brush_uids.empty()) {
            // Build entries for brushes that have alpine properties
            std::vector<BrushGroupEntry> entries;

            for (uint32_t i = 0; i < g_group_save_brush_uids.size(); i++) {
                int uid = g_group_save_brush_uids[i];
                uint8_t flags = 0;
                uint8_t material = 0;

                // Check geoable
                if (std::find(props.geoable_brush_uids.begin(),
                              props.geoable_brush_uids.end(), uid)
                    != props.geoable_brush_uids.end()) {
                    flags |= 1;
                }

                // Check breakable
                auto bit = std::find(props.breakable_brush_uids.begin(),
                                     props.breakable_brush_uids.end(), uid);
                if (bit != props.breakable_brush_uids.end()) {
                    flags |= 2;
                    auto idx = std::distance(props.breakable_brush_uids.begin(), bit);
                    if (idx < static_cast<int>(props.breakable_materials.size()))
                        material = props.breakable_materials[idx];
                }

                if (flags != 0)
                    entries.push_back({i, flags, material});
            }

            if (!entries.empty()) {
                // Write chunk: chunk_id (int32) + chunk_size (int32) + data
                // Format: version (uint32) + entry_size (uint16) + entry_count (uint32) + entries
                constexpr uint32_t chunk_version = 1;
                constexpr uint16_t entry_size = sizeof(BrushGroupEntry);
                uint32_t entry_count = static_cast<uint32_t>(entries.size());
                int32_t data_size = static_cast<int32_t>(
                    sizeof(chunk_version) + sizeof(entry_size) + sizeof(entry_count)
                    + entry_count * entry_size);

                file->write<int32_t>(alpine_brush_group_chunk_id);
                file->write<int32_t>(data_size);
                file->write<uint32_t>(chunk_version);
                file->write<uint16_t>(entry_size);
                file->write<uint32_t>(entry_count);
                for (auto& e : entries) {
                    file->write<uint32_t>(e.brush_index);
                    file->write<uint8_t>(e.flags);
                    file->write<uint8_t>(e.material);
                }

                xlog::info("[AlpineObj] Saved {} brush property entries to group", entry_count);
            }
        }

        xlog::info("[AlpineObj] Saved {} meshes, {} notes, {} coronas to group",
            g_group_save_meshes.size(), g_group_save_notes.size(), g_group_save_coronas.size());

        g_group_save_meshes.clear();
        g_group_save_notes.clear();
        g_group_save_coronas.clear();
        g_group_save_brush_uids.clear();
    },
};

// Pre-load hook: capture moving_groups.size before stock group load runs.
// At 0x0041d2d8: ESI = CDocument*, about to call FUN_00438340.
// Instruction: MOV EDX, [ESI+0x50] (3 bytes)
CodeInjection alpine_group_pre_load_hook{
    0x0041d2d8,
    [](auto& regs) {
        auto* level = reinterpret_cast<CDedLevel*>(static_cast<uintptr_t>(regs.esi) + 0x60);
        g_moving_groups_size_before_load = level->moving_groups.size;
    },
};

// Load hook: read Alpine object chunks after stock data.
// At 0x0041d2e4: ESI = CDocument*, stock deserialization done.
// Stock group load (FUN_00438340) deselects all, loads objects, selects each loaded
// object, and creates a User-Defined Group entry in moving_groups. Alpine objects are
// loaded after stock code finishes, so we must also add them to the selection and to
// the newly created group entry's objects VArray (at group_entry + 0x10).
CodeInjection alpine_group_load_hook{
    0x0041d2e4,
    [](auto& regs) {
        // ESI = CDocument*; CDedLevel is at CDocument + 0x60
        auto* level = reinterpret_cast<CDedLevel*>(static_cast<uintptr_t>(regs.esi) + 0x60);
        auto* file = reinterpret_cast<rf::File*>(
            *reinterpret_cast<void**>(static_cast<uintptr_t>(regs.esi) + 0x50));
        auto& props = level->GetAlpineLevelProperties();

        // Track which objects existed before loading so we can select only the new ones
        auto mesh_start = props.mesh_objects.size();
        auto note_start = props.note_objects.size();
        auto corona_start = props.corona_objects.size();

        // Brush group entries parsed from the .rfg brush metadata chunk.
        std::vector<BrushGroupEntry> brush_group_entries;

        // Read chunks in the same format as .rfl files: chunk_id (int32) + chunk_size (int32) + data
        while (true) {
            int32_t chunk_id = 0;
            if (file->read(&chunk_id, 4) != 4) break;
            int32_t chunk_size = 0;
            if (file->read(&chunk_size, 4) != 4) break;
            if (chunk_size < 0 || chunk_size > 10000000) break;

            if (chunk_id == alpine_mesh_chunk_id) {
                mesh_deserialize_chunk(*level, *file, chunk_size);
            }
            else if (chunk_id == alpine_note_chunk_id) {
                note_deserialize_chunk(*level, *file, chunk_size);
            }
            else if (chunk_id == alpine_corona_chunk_id) {
                corona_deserialize_chunk(*level, *file, chunk_size);
            }
            else if (chunk_id == alpine_brush_group_chunk_id) {
                // Read brush metadata chunk, tracking remaining bytes to stay within chunk bounds
                int32_t remaining = chunk_size;

                uint32_t version = 0;
                if (remaining < 4 || file->read(&version, 4) != 4) { file->seek(std::max(remaining, 0), rf::File::seek_cur); break; }
                remaining -= 4;
                if (version < 1) { file->seek(remaining, rf::File::seek_cur); break; }

                uint16_t entry_size = 0;
                if (remaining < 2 || file->read(&entry_size, 2) != 2) { file->seek(std::max(remaining, 0), rf::File::seek_cur); break; }
                remaining -= 2;
                if (entry_size < sizeof(BrushGroupEntry)) { file->seek(remaining, rf::File::seek_cur); break; }

                uint32_t entry_count = 0;
                if (remaining < 4 || file->read(&entry_count, 4) != 4) { file->seek(std::max(remaining, 0), rf::File::seek_cur); break; }
                remaining -= 4;

                // Validate entry_count against remaining chunk bytes
                if (entry_count > static_cast<uint32_t>(remaining) / entry_size)
                    entry_count = static_cast<uint32_t>(remaining) / entry_size;
                if (entry_count > 10000) entry_count = 10000;

                uint32_t read_count = 0;
                brush_group_entries.resize(entry_count);
                for (uint32_t i = 0; i < entry_count; i++) {
                    uint32_t brush_index = 0;
                    uint8_t flags = 0;
                    uint8_t material = 0;
                    if (file->read(&brush_index, 4) != 4) break;
                    if (file->read(&flags, 1) != 1) break;
                    if (file->read(&material, 1) != 1) break;
                    remaining -= sizeof(BrushGroupEntry);
                    brush_group_entries[i] = {brush_index, flags, material};
                    read_count++;
                    // Skip unknown trailing bytes per entry for forward compat
                    int extra = entry_size - sizeof(BrushGroupEntry);
                    if (extra > 0) {
                        file->seek(extra, rf::File::seek_cur);
                        remaining -= extra;
                    }
                }
                brush_group_entries.resize(read_count);
                // Skip any remaining bytes in the chunk
                if (remaining > 0)
                    file->seek(remaining, rf::File::seek_cur);
                xlog::info("[AlpineObj] Read {} brush property entries from group", read_count);
            }
            else {
                // Unknown chunk — stop reading
                break;
            }
        }

        int meshes_loaded = static_cast<int>(props.mesh_objects.size() - mesh_start);
        int notes_loaded = static_cast<int>(props.note_objects.size() - note_start);
        int coronas_loaded = static_cast<int>(props.corona_objects.size() - corona_start);
        bool has_brush_props = !brush_group_entries.empty();

        if (!meshes_loaded && !notes_loaded && !coronas_loaded && !has_brush_props)
            return;

        // Assign new unique UIDs to imported Alpine objects (group import must not
        // reuse UIDs from the file — stock FUN_004365c0 does the same for stock types).
        // generate_uid (vtypes.h, hooked by alpine_generate_uid_hook) considers all objects.
        for (auto i = mesh_start; i < props.mesh_objects.size(); i++)
            static_cast<DedObject*>(props.mesh_objects[i])->uid = generate_uid();
        for (auto i = note_start; i < props.note_objects.size(); i++)
            static_cast<DedObject*>(props.note_objects[i])->uid = generate_uid();
        for (auto i = corona_start; i < props.corona_objects.size(); i++)
            static_cast<DedObject*>(props.corona_objects[i])->uid = generate_uid();

        // Add newly loaded Alpine objects to the selection so they move with the
        // other stock objects when the user places the imported group.
        for (auto i = mesh_start; i < props.mesh_objects.size(); i++)
            level->add_to_selection(static_cast<DedObject*>(props.mesh_objects[i]));
        for (auto i = note_start; i < props.note_objects.size(); i++)
            level->add_to_selection(static_cast<DedObject*>(props.note_objects[i]));
        for (auto i = corona_start; i < props.corona_objects.size(); i++)
            level->add_to_selection(static_cast<DedObject*>(props.corona_objects[i]));

        // Refresh console display to include newly selected Alpine objects
        // (stock FUN_00423460 runs inside FUN_00438340, before our hook loads them)
        level->update_console_display();

        // Add Alpine objects to the User-Defined Group entry created by the stock
        // group load. The stock loader (FUN_004365c0) populates each GroupEntry's objects
        // VArray from the file; we add Alpine objects to the same VArray so they appear
        // in the group.
        //
        // Limitation: our Alpine chunk format doesn't encode per-group membership, so
        // if multiple groups were imported at once, Alpine objects are added to the first
        // new user-defined group only. This matches the typical single-group import case.
        auto& mg = level->moving_groups;
        int new_groups = mg.size - g_moving_groups_size_before_load;
        // Safety cap: never iterate more than 16 new entries to guard against corruption
        if (new_groups > 0 && new_groups <= 16) {
            for (int gi = g_moving_groups_size_before_load; gi < mg.size; gi++) {
                auto* entry = mg.data_ptr[gi];
                if (!entry) continue;
                if (!entry->is_user_defined()) continue;
                for (auto i = mesh_start; i < props.mesh_objects.size(); i++)
                    entry->objects.push_back(static_cast<DedObject*>(props.mesh_objects[i]));
                for (auto i = note_start; i < props.note_objects.size(); i++)
                    entry->objects.push_back(static_cast<DedObject*>(props.note_objects[i]));
                for (auto i = corona_start; i < props.corona_objects.size(); i++)
                    entry->objects.push_back(static_cast<DedObject*>(props.corona_objects[i]));

                // Apply brush group properties: map serialization index → final brush UID
                // via the group entry's brushes VArray (same order as serialized).
                if (!brush_group_entries.empty()) {
                    auto& brushes = entry->brushes;
                    int applied = 0;
                    for (auto& bge : brush_group_entries) {
                        if (bge.brush_index >= static_cast<uint32_t>(brushes.size))
                            continue;
                        auto* brush = brushes.data_ptr[bge.brush_index];
                        if (!brush) continue;
                        int uid = brush->uid;

                        if (bge.flags & 1) { // geoable
                            if (std::find(props.geoable_brush_uids.begin(),
                                          props.geoable_brush_uids.end(), uid)
                                == props.geoable_brush_uids.end()) {
                                props.geoable_brush_uids.push_back(uid);
                            }
                        }
                        if (bge.flags & 2) { // breakable
                            if (std::find(props.breakable_brush_uids.begin(),
                                          props.breakable_brush_uids.end(), uid)
                                == props.breakable_brush_uids.end()) {
                                props.breakable_brush_uids.push_back(uid);
                                props.breakable_room_uids.push_back(0); // computed at .rfl save time
                                props.breakable_materials.push_back(bge.material);
                            }
                        }
                        applied++;
                    }
                    xlog::info("[AlpineObj] Applied {} brush properties from group", applied);
                }

                break; // Only add to the first user-defined group
            }
        }

        xlog::info("[AlpineObj] Loaded {} meshes, {} notes, {} coronas from group",
            meshes_loaded, notes_loaded, coronas_loaded);
    },
};

// ─── Install ────────────────────────────────────────────────────────────────

void ApplyAlpineObjectPatches()
{
    alpine_properties_patch.install();
    alpine_generate_uid_hook.install();
    alpine_tree_patch.install();
    alpine_pick_patch.install();
    alpine_click_pick_patch.install();
    alpine_copy_begin_hook.install();
    alpine_copy_hook.install();
    AsmWriter(0x00448659).jmp(alpine_paste_wrapper);
    alpine_delete_mode_patch.install();
    alpine_paste_finalize_patch.install();
    alpine_delete_patch.install();
    alpine_object_tree_patch.install();
    alpine_factory_hook.install();
    alpine_create_object_patch.install();
    alpine_render_patch.install();
    alpine_group_save_clear_hook.install();
    alpine_group_brush_save_capture_hook.install();
    alpine_group_type_collect_hook.install();
    alpine_group_save_hook.install();
    alpine_group_pre_load_hook.install();
    alpine_group_load_hook.install();

    xlog::info("[AlpineObj] Shared Alpine object patches applied");
}
