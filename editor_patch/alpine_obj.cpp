#include <windows.h>
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
        if (!group) continue;
        auto& kfs = *reinterpret_cast<VArray<DedObject*>*>(
            reinterpret_cast<uintptr_t>(group) + 0x1C);
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

// .rfg I/O helpers — operate on the stock bidirectional I/O context at CDedLevel+0x50
static void rfg_write_int32(void* ctx, int32_t val) {
    AddrCaller{0x004d1230}.this_call(ctx, val);
}
static void rfg_write_raw(void* ctx, const void* data, int size) {
    AddrCaller{0x004d13f0}.this_call(ctx, data, size);
}
static int32_t rfg_read_int32(void* ctx) {
    return AddrCaller{0x004d08f0}.this_call<int32_t>(ctx, 0, 0);
}
static bool rfg_read_raw(void* ctx, void* dst, int size) {
    AddrCaller{0x004d0f40}.this_call(ctx, dst, size, 0, 0);
    return AddrCaller{0x004d01f0}.this_call<int>(ctx) == 0;
}

// String I/O: int32 length + raw chars
static void rfg_write_vstring(void* ctx, const VString& s) {
    const char* str = s.c_str();
    int32_t len = static_cast<int32_t>(strlen(str));
    rfg_write_int32(ctx, len);
    if (len > 0) rfg_write_raw(ctx, str, len);
}
static void rfg_write_std_string(void* ctx, const std::string& s) {
    int32_t len = static_cast<int32_t>(s.size());
    rfg_write_int32(ctx, len);
    if (len > 0) rfg_write_raw(ctx, s.c_str(), len);
}
static std::string rfg_read_string(void* ctx) {
    int32_t len = rfg_read_int32(ctx);
    if (len <= 0 || len > 10000) return "";
    std::string result(len, '\0');
    if (!rfg_read_raw(ctx, result.data(), len)) return "";
    return result;
}

constexpr int32_t alpine_group_sentinel = 0x0AFB0001;

// Save hook: append Alpine objects after stock data, before file close.
// At 0x0041d11c: ESI = CDedLevel*, [ESI+0x50] = I/O context, about to close file.
CodeInjection alpine_group_save_hook{
    0x0041d11c,
    [](auto& regs) {
        auto* level = reinterpret_cast<CDedLevel*>(static_cast<uintptr_t>(regs.esi));
        void* io_ctx = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(level) + 0x50);

        // Collect selected Alpine objects
        auto& sel = level->selection;
        std::vector<DedMesh*> meshes;
        std::vector<DedNote*> notes;
        std::vector<DedCorona*> coronas;
        for (int i = 0; i < sel.size; i++) {
            auto* obj = sel.data_ptr[i];
            if (!obj) continue;
            if (obj->type == DedObjectType::DED_MESH)
                meshes.push_back(static_cast<DedMesh*>(obj));
            else if (obj->type == DedObjectType::DED_NOTE)
                notes.push_back(static_cast<DedNote*>(obj));
            else if (obj->type == DedObjectType::DED_CORONA)
                coronas.push_back(static_cast<DedCorona*>(obj));
        }

        if (meshes.empty() && notes.empty() && coronas.empty()) return;

        rfg_write_int32(io_ctx, alpine_group_sentinel);

        // Meshes
        rfg_write_int32(io_ctx, static_cast<int32_t>(meshes.size()));
        for (auto* m : meshes) {
            rfg_write_int32(io_ctx, m->uid);
            rfg_write_raw(io_ctx, &m->pos, sizeof(Vector3));
            rfg_write_raw(io_ctx, &m->orient, sizeof(Matrix3));
            rfg_write_vstring(io_ctx, m->script_name);
            rfg_write_vstring(io_ctx, m->mesh_filename);
            rfg_write_vstring(io_ctx, m->state_anim);
            rfg_write_raw(io_ctx, &m->collision_mode, 1);
            uint8_t num_ov = static_cast<uint8_t>(m->texture_overrides.size());
            rfg_write_raw(io_ctx, &num_ov, 1);
            for (const auto& ovr : m->texture_overrides) {
                rfg_write_raw(io_ctx, &ovr.slot, 1);
                rfg_write_std_string(io_ctx, ovr.filename);
            }
            rfg_write_int32(io_ctx, m->material);
            uint8_t is_cl = m->clutter_props.is_clutter ? 1 : 0;
            rfg_write_raw(io_ctx, &is_cl, 1);
            if (m->clutter_props.is_clutter) {
                auto& cp = m->clutter_props;
                rfg_write_raw(io_ctx, &cp.life, sizeof(float));
                rfg_write_std_string(io_ctx, cp.debris_filename);
                rfg_write_std_string(io_ctx, cp.explosion_vclip);
                rfg_write_raw(io_ctx, &cp.explosion_radius, sizeof(float));
                rfg_write_raw(io_ctx, &cp.debris_velocity, sizeof(float));
                for (int d = 0; d < 11; d++)
                    rfg_write_raw(io_ctx, &cp.damage_type_factors[d], sizeof(float));
                rfg_write_std_string(io_ctx, cp.corpse_filename);
                rfg_write_std_string(io_ctx, cp.corpse_state_anim);
                rfg_write_raw(io_ctx, &cp.corpse_collision, 1);
                rfg_write_raw(io_ctx, &cp.corpse_material, 1);
            }
        }

        // Notes
        rfg_write_int32(io_ctx, static_cast<int32_t>(notes.size()));
        for (auto* n : notes) {
            rfg_write_int32(io_ctx, n->uid);
            rfg_write_raw(io_ctx, &n->pos, sizeof(Vector3));
            rfg_write_raw(io_ctx, &n->orient, sizeof(Matrix3));
            rfg_write_vstring(io_ctx, n->script_name);
            rfg_write_int32(io_ctx, static_cast<int32_t>(n->notes.size()));
            for (const auto& text : n->notes) {
                rfg_write_std_string(io_ctx, text);
            }
        }

        // Coronas
        rfg_write_int32(io_ctx, static_cast<int32_t>(coronas.size()));
        for (auto* c : coronas) {
            rfg_write_int32(io_ctx, c->uid);
            rfg_write_raw(io_ctx, &c->pos, sizeof(Vector3));
            rfg_write_raw(io_ctx, &c->orient, sizeof(Matrix3));
            rfg_write_vstring(io_ctx, c->script_name);
            rfg_write_raw(io_ctx, &c->color_r, 1);
            rfg_write_raw(io_ctx, &c->color_g, 1);
            rfg_write_raw(io_ctx, &c->color_b, 1);
            rfg_write_raw(io_ctx, &c->color_a, 1);
            rfg_write_std_string(io_ctx, c->corona_bitmap);
            rfg_write_raw(io_ctx, &c->cone_angle, sizeof(float));
            rfg_write_raw(io_ctx, &c->intensity, sizeof(float));
            rfg_write_raw(io_ctx, &c->radius_distance, sizeof(float));
            rfg_write_raw(io_ctx, &c->radius_scale, sizeof(float));
            rfg_write_raw(io_ctx, &c->diminish_distance, sizeof(float));
            rfg_write_std_string(io_ctx, c->volumetric_bitmap);
            uint8_t has_vol = !c->volumetric_bitmap.empty() ? 1 : 0;
            rfg_write_raw(io_ctx, &has_vol, 1);
            if (has_vol) {
                rfg_write_raw(io_ctx, &c->volumetric_height, sizeof(float));
                rfg_write_raw(io_ctx, &c->volumetric_length, sizeof(float));
            }
        }

        xlog::info("[AlpineObj] Saved {} meshes, {} notes, {} coronas to group",
            meshes.size(), notes.size(), coronas.size());
    },
};

// Load hook: read Alpine objects after stock data, before file close.
// At 0x0041d2e4: ESI = CDedLevel*, [ESI+0x50] = I/O context, stock deserialization done.
CodeInjection alpine_group_load_hook{
    0x0041d2e4,
    [](auto& regs) {
        auto* level = reinterpret_cast<CDedLevel*>(static_cast<uintptr_t>(regs.esi));
        void* io_ctx = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(level) + 0x50);
        auto& props = level->GetAlpineLevelProperties();

        // Try to read sentinel — returns 0 on EOF/error (stock .rfg files)
        int32_t sentinel = rfg_read_int32(io_ctx);
        if (sentinel != alpine_group_sentinel) return;

        // Meshes
        int32_t mesh_count = rfg_read_int32(io_ctx);
        if (mesh_count < 0 || mesh_count > 10000) return;
        for (int32_t i = 0; i < mesh_count; i++) {
            auto* mesh = new DedMesh();
            memset(static_cast<DedObject*>(mesh), 0, sizeof(DedObject));
            mesh->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
            mesh->type = DedObjectType::DED_MESH;
            mesh->collision_mode = 2;

            mesh->uid = rfg_read_int32(io_ctx);
            if (!rfg_read_raw(io_ctx, &mesh->pos, sizeof(Vector3))) { DestroyDedMesh(mesh); return; }
            if (!rfg_read_raw(io_ctx, &mesh->orient, sizeof(Matrix3))) { DestroyDedMesh(mesh); return; }
            std::string sname = rfg_read_string(io_ctx);
            mesh->script_name.assign_0(sname.c_str());
            std::string mfn = rfg_read_string(io_ctx);
            mesh->mesh_filename.assign_0(mfn.c_str());
            std::string sa = rfg_read_string(io_ctx);
            mesh->state_anim.assign_0(sa.c_str());
            rfg_read_raw(io_ctx, &mesh->collision_mode, 1);
            uint8_t num_ov = 0;
            rfg_read_raw(io_ctx, &num_ov, 1);
            for (uint8_t oi = 0; oi < num_ov; oi++) {
                uint8_t slot = 0;
                rfg_read_raw(io_ctx, &slot, 1);
                std::string tex = rfg_read_string(io_ctx);
                if (!tex.empty()) mesh->texture_overrides.push_back({slot, std::move(tex)});
            }
            mesh->material = rfg_read_int32(io_ctx);
            uint8_t is_cl = 0;
            rfg_read_raw(io_ctx, &is_cl, 1);
            mesh->clutter_props.is_clutter = (is_cl != 0);
            if (mesh->clutter_props.is_clutter) {
                auto& cp = mesh->clutter_props;
                rfg_read_raw(io_ctx, &cp.life, sizeof(float));
                cp.debris_filename = rfg_read_string(io_ctx);
                cp.explosion_vclip = rfg_read_string(io_ctx);
                rfg_read_raw(io_ctx, &cp.explosion_radius, sizeof(float));
                rfg_read_raw(io_ctx, &cp.debris_velocity, sizeof(float));
                for (int d = 0; d < 11; d++)
                    rfg_read_raw(io_ctx, &cp.damage_type_factors[d], sizeof(float));
                cp.corpse_filename = rfg_read_string(io_ctx);
                cp.corpse_state_anim = rfg_read_string(io_ctx);
                rfg_read_raw(io_ctx, &cp.corpse_collision, 1);
                rfg_read_raw(io_ctx, &cp.corpse_material, 1);
            }
            mesh->vmesh = nullptr;
            mesh->vmesh_load_failed = false;

            props.mesh_objects.push_back(mesh);
            level->master_objects.add(static_cast<DedObject*>(mesh));
            level->select_object(static_cast<DedObject*>(mesh));
        }

        // Notes
        int32_t note_count = rfg_read_int32(io_ctx);
        if (note_count < 0 || note_count > 10000) return;
        for (int32_t i = 0; i < note_count; i++) {
            auto* note = new DedNote();
            memset(static_cast<DedObject*>(note), 0, sizeof(DedObject));
            note->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
            note->type = DedObjectType::DED_NOTE;

            note->uid = rfg_read_int32(io_ctx);
            if (!rfg_read_raw(io_ctx, &note->pos, sizeof(Vector3))) { delete note; return; }
            if (!rfg_read_raw(io_ctx, &note->orient, sizeof(Matrix3))) { delete note; return; }
            std::string sn = rfg_read_string(io_ctx);
            note->script_name.assign_0(sn.c_str());
            int32_t ncount = rfg_read_int32(io_ctx);
            if (ncount < 0 || ncount > 10000) ncount = 0;
            for (int32_t ni = 0; ni < ncount; ni++) {
                note->notes.push_back(rfg_read_string(io_ctx));
            }

            props.note_objects.push_back(note);
            level->master_objects.add(static_cast<DedObject*>(note));
            level->select_object(static_cast<DedObject*>(note));
        }

        // Coronas
        int32_t corona_count = rfg_read_int32(io_ctx);
        if (corona_count < 0 || corona_count > 10000) return;
        for (int32_t i = 0; i < corona_count; i++) {
            auto* corona = new DedCorona();
            memset(static_cast<DedObject*>(corona), 0, sizeof(DedObject));
            corona->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
            corona->type = DedObjectType::DED_CORONA;

            corona->uid = rfg_read_int32(io_ctx);
            if (!rfg_read_raw(io_ctx, &corona->pos, sizeof(Vector3))) { DestroyDedCorona(corona); return; }
            if (!rfg_read_raw(io_ctx, &corona->orient, sizeof(Matrix3))) { DestroyDedCorona(corona); return; }
            std::string sn = rfg_read_string(io_ctx);
            corona->script_name.assign_0(sn.c_str());
            rfg_read_raw(io_ctx, &corona->color_r, 1);
            rfg_read_raw(io_ctx, &corona->color_g, 1);
            rfg_read_raw(io_ctx, &corona->color_b, 1);
            rfg_read_raw(io_ctx, &corona->color_a, 1);
            corona->corona_bitmap = rfg_read_string(io_ctx);
            rfg_read_raw(io_ctx, &corona->cone_angle, sizeof(float));
            rfg_read_raw(io_ctx, &corona->intensity, sizeof(float));
            rfg_read_raw(io_ctx, &corona->radius_distance, sizeof(float));
            rfg_read_raw(io_ctx, &corona->radius_scale, sizeof(float));
            rfg_read_raw(io_ctx, &corona->diminish_distance, sizeof(float));
            corona->volumetric_bitmap = rfg_read_string(io_ctx);
            uint8_t has_vol = 0;
            rfg_read_raw(io_ctx, &has_vol, 1);
            if (has_vol) {
                rfg_read_raw(io_ctx, &corona->volumetric_height, sizeof(float));
                rfg_read_raw(io_ctx, &corona->volumetric_length, sizeof(float));
            }

            props.corona_objects.push_back(corona);
            level->master_objects.add(static_cast<DedObject*>(corona));
            level->select_object(static_cast<DedObject*>(corona));
        }

        xlog::info("[AlpineObj] Loaded {} meshes, {} notes, {} coronas from group",
            mesh_count, note_count, corona_count);
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
    alpine_group_save_hook.install();
    alpine_group_load_hook.install();

    xlog::info("[AlpineObj] Shared Alpine object patches applied");
}
