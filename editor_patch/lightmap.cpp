#include <memory>
#include <cstdint>
#include <patch_common/CodeInjection.h>
#include <patch_common/MemUtils.h>
#include <patch_common/ShortTypes.h>

// Max lights that can be processed per face (shadow mask buffer limit).
// Faces with more lights than this get the pink fill safety fallback.
static constexpr int max_shadow_masks = 1024;
static void* shadow_mask_ptrs[max_shadow_masks];
static std::unique_ptr<uint8_t[]> shadow_mask_pool;

// Scene light object pool: replaces the stock 1100-entry pool at 0x006FB248
// Each entry is sizeof(rf::gr::Light) = 0x10C bytes (see game_patch/rf/gr/gr_light.h)
// todo: shared header between editor and game for this and other common structs
static constexpr int max_scene_lights = 8192;

// Per-face light list: replaces the stock ~1100-entry array at 0x006F9FF8
// Must be at least max_scene_lights since all scene lights could affect a single face
// 0x00488810 writes to this array with no bounds check
static void* face_light_list[max_scene_lights];
static constexpr int light_entry_size = 0x10C;
alignas(16) static uint8_t light_pool[max_scene_lights * light_entry_size];

// Dummy light entry for out-of-bounds handle access.
// reads return all zeros (type=LT_NONE). Prevents memory corruption when the
// stock code calls handle_to_ptr with handle=-1 (allocation failure).
alignas(16) static uint8_t dummy_light[light_entry_size] = {};

CodeInjection light_handle_to_pointer_injection{
    0x00487a00,
    [](auto& regs) {
        int handle = *reinterpret_cast<int*>(regs.esp + 4);
        if (handle >= 0 && handle < max_scene_lights) {
            regs.eax = reinterpret_cast<uintptr_t>(light_pool) +
                        static_cast<unsigned>(handle) * light_entry_size;
        }
        else {
            regs.eax = reinterpret_cast<uintptr_t>(dummy_light);
        }
        regs.eip = 0x00487a15; // jump to RET
    },
};

CodeInjection lightmap_light_limit_injection{
    0x004AC608,
    [](auto& regs) {
        int light_count = regs.edi;
        if (light_count >= max_shadow_masks) {
            regs.eip = 0x004AC9E4; // pink fill safety fallback
        }
        else {
            regs.eip = 0x004AC611; // normal lightmap processing
        }
    },
};

void ApplyLightmapPatches()
{
    // Fix pink lightmaps when a face is affected by >= 64 lights
    // Allocate shadow mask buffers on the heap
    shadow_mask_pool = std::make_unique<uint8_t[]>(max_shadow_masks * 0x1000);
    for (int i = 0; i < max_shadow_masks; i++) {
        shadow_mask_ptrs[i] = &shadow_mask_pool[i * 0x1000];
    }

    // Replace light handle-to-pointer with bounds-checked version
    // Prevents corruption on handle=-1, which occurs when trying to add/edit lights after max_scene_lights
    // Original: ptr = pool_base + handle * 0x10C (no validation)
    light_handle_to_pointer_injection.install();

    // Replace the >= 64 limit check with new limit
    lightmap_light_limit_injection.install();

    // Redirect mask buffer array references from old 64-entry array (0x0057CE78) to new array
    write_mem_ptr(0x004AC7A0 + 4, shadow_mask_ptrs);
    write_mem_ptr(0x004AC888 + 1, shadow_mask_ptrs);

    // Expand per-face light list from 1100 entries (0x006F9FF8)
    write_mem_ptr(0x004887C9, face_light_list);
    write_mem_ptr(0x0048899E, face_light_list);
    write_mem_ptr(0x00488B6F, face_light_list);
    write_mem_ptr(0x00488C52, face_light_list);
    write_mem_ptr(0x00488C59, face_light_list);
    write_mem_ptr(0x00488CB7, face_light_list);
    write_mem_ptr(0x0048911D, face_light_list);
    write_mem_ptr(0x0050364B, face_light_list);
    write_mem_ptr(0x00488C09, face_light_list);
    write_mem_ptr(0x00489519, face_light_list);
    write_mem_ptr(0x00505B26, face_light_list);
    write_mem_ptr(0x00489E59, face_light_list);
    write_mem_ptr(0x005025E8, face_light_list);
    write_mem_ptr(0x0050459C, face_light_list);

    // Expand scene light object pool from 1100 entries (0x006FB248)
    // Redirect pool base address references
    // 0x00487A11 omitted because it's inside 0x00487a00 and avoided by light_handle_to_pointer_injection
    write_mem_ptr(0x00486CA0, light_pool);
    write_mem_ptr(0x00487045, light_pool);
    write_mem_ptr(0x00487A85, light_pool);
    write_mem_ptr(0x00487ABA, light_pool);
    // Redirect pool base+4 (prev pointer field) references
    write_mem_ptr(0x00487A7F, light_pool + 4);
    write_mem_ptr(0x00487AB4, light_pool + 4);
    // Redirect pool base+8 (type/active field) reference
    write_mem_ptr(0x00487A24, light_pool + 8);
    // Redirect pool base+0xC (data field) reference
    write_mem_ptr(0x0048A464, light_pool + 0xC);
    // Update scan end limit
    auto scan_end = reinterpret_cast<uintptr_t>(light_pool + 8) + max_scene_lights * light_entry_size;
    write_mem<uint32_t>(0x00487A34, static_cast<uint32_t>(scan_end));
    // Update count limit
    write_mem<uint32_t>(0x00487A41, max_scene_lights);
    // Update zeroing loop count
    write_mem<uint32_t>(0x00487077, max_scene_lights * light_entry_size / 4);

    // Fix lightmap surface group ID overflow crash (face+0x36 is a signed short)
    // When >32767 lightmap surface groups exist (typically >~45000 faces), the signed
    // short wraps negative. MOVSX sign-extends it to a negative 32-bit index, causing
    // out-of-bounds array access and heap corruption.
    // Fix: patch all MOVSX reads of face+0x36 to MOVZX (0x0FBF -> 0x0FB7), raising the
    // limit from 32767 to 65535 groups. Also fix sentinel checks and group comparisons.

    // MOVSX -> MOVZX for face+0x36 reads from memory (14 sites)
    // Each MOVSX word ptr [reg+0x36] is encoded 0F BF; change second byte to B7 for MOVZX
    write_mem<u8>(0x004aa900, 0xB7); // FUN_004aa610: lightmap surface grouping
    write_mem<u8>(0x004aa929, 0xB7);
    write_mem<u8>(0x004aa946, 0xB7);
    write_mem<u8>(0x004aa99d, 0xB7);
    write_mem<u8>(0x004aab06, 0xB7);
    write_mem<u8>(0x004aad59, 0xB7); // FUN_004aabf0: batch lightmap calculator
    write_mem<u8>(0x004aad8a, 0xB7);
    write_mem<u8>(0x004ad241, 0xB7); // FUN_004ad160: shadow geometry
    write_mem<u8>(0x004ad27a, 0xB7);
    write_mem<u8>(0x004ae26c, 0xB7); // FUN_004ae050: shadow calculation
    write_mem<u8>(0x004ae73a, 0xB7); // FUN_004ae360: shadow calculation
    write_mem<u8>(0x004ae81f, 0xB7);
    write_mem<u8>(0x004aed4e, 0xB7);
    write_mem<u8>(0x004a45b1, 0xB7); // solid_write: level file export

    // MOVSX -> MOVZX for face+0x36 reads from register (5 sites)
    // These follow 16-bit MOV AX,[face+0x36] and sign-extend AX to 32-bit before use as index
    write_mem<u8>(0x0048778d, 0xB7); // FUN_004872e0: lightmap surface container lookup
    write_mem<u8>(0x00487864, 0xB7);
    write_mem<u8>(0x0049b955, 0xB7); // FUN_0049b550: geo-cache build
    write_mem<u8>(0x0049bea0, 0xB7);
    write_mem<u8>(0x0049bed3, 0xB7);

    // Fix sentinel checks: JLE -> JE (0x7E -> 0x74)
    // Original: CMP AX,0xFFFF; JLE skip — skips all negative values (wrong for groups > 32767)
    // Fixed: CMP AX,0xFFFF; JE skip — skips only the 0xFFFF "no group" sentinel
    write_mem<u8>(0x00487781, 0x74); // FUN_004872e0
    write_mem<u8>(0x00487858, 0x74);
    write_mem<u8>(0x0049b94b, 0x74); // FUN_0049b550

    // Note: CMP AX,BX; JLE at 0x0049be96 and 0x0049bec9 left as signed — BX can be
    // 0xFFFF ("no batch" sentinel), and unsigned JBE would cause all faces to be skipped.
    // Signed comparison is correct for group IDs 0-32767 and safe for the sentinel case.
}
