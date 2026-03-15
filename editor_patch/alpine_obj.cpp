#include <windows.h>
#include <commctrl.h>
#include <cstdint>
#include <algorithm>
#include <xlog/xlog.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/AsmWriter.h>
#include "alpine_obj.h"
#include "mesh.h"
#include "note.h"
#include "mfc_types.h"
#include "level.h"
#include "vtypes.h"

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

            // Determine best Alpine hit
            DedObject* best_alpine = nullptr;
            if (best_mesh) {
                best_alpine = static_cast<DedObject*>(best_mesh);
            }
            if (best_note) {
                float note_pos[3] = {best_note->pos.x, best_note->pos.y, best_note->pos.z};
                float nsx = 0.0f, nsy = 0.0f;
                if (project_to_screen_2d(note_pos, &nsx, &nsy)) {
                    float ndx = nsx - click_x, ndy = nsy - click_y;
                    float note_dist = ndx * ndx + ndy * ndy;
                    if (!best_alpine || note_dist < mesh_dist_sq) {
                        best_alpine = static_cast<DedObject*>(best_note);
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
    },
};

// Track which Alpine object type the tree view is creating.
static int g_alpine_create_type = 0; // 0=Mesh, 2=Note

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

    xlog::info("[AlpineObj] Shared Alpine object patches applied");
}
