#include <windows.h>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include "face_list_cache.h"
#include "level.h"
#include "vtypes.h"

namespace
{

struct TailEntry
{
    std::uintptr_t head;
    std::uintptr_t tail;
    int count;
};

// Scoped by FaceListCacheWindow and CachePause below.
int g_window_depth = 0;
int g_pause_depth = 0;

bool cache_live()
{
    return g_window_depth > 0 && g_pause_depth == 0;
}

// The stock face list the list helpers take as `this`, e.g. GSolid::face_list_head and face_list_count.
struct FaceListHeader
{
    std::uintptr_t head;
    int count;
};
static_assert(offsetof(FaceListHeader, count) == offsetof(GSolid, face_list_count) - offsetof(GSolid, face_list_head));
static_assert(offsetof(FaceListHeader, count) == offsetof(GRoom, face_list_count) - offsetof(GRoom, face_list_head));

std::uintptr_t& list_head(std::uintptr_t list)
{
    return reinterpret_cast<FaceListHeader*>(list)->head;
}

int& list_count(std::uintptr_t list)
{
    return reinterpret_cast<FaceListHeader*>(list)->count;
}

template<std::uintptr_t NextOffset>
struct FaceList
{
    static inline std::unordered_map<std::uintptr_t, TailEntry> tails;

    static std::uintptr_t& next(std::uintptr_t face)
    {
        return *reinterpret_cast<std::uintptr_t*>(face + NextOffset);
    }

    static void remember(std::uintptr_t list, const TailEntry& entry)
    {
        try {
            tails.insert_or_assign(list, entry);
        }
        catch (...) {
            tails.clear();
        }
    }

    // Same list edits as the stock helper.
    static void append(std::uintptr_t list, std::uintptr_t face)
    {
        std::uintptr_t& head = list_head(list);
        int& count = list_count(list);
        count++;
        if (!head) {
            head = face;
            next(face) = 0;
            remember(list, {face, face, count});
            return;
        }
        std::uintptr_t tail = head;
        if (auto it = tails.find(list); it != tails.end() && it->second.head == head && it->second.count == count - 1) {
            tail = it->second.tail;
        }
        while (next(tail)) {
            tail = next(tail);
        }
        next(face) = next(tail);
        next(tail) = face;
        remember(list, {head, face, count});
    }

    // FUN_004a97b0's edit: the face becomes the head, the tail stays.
    static void prepend(std::uintptr_t list, std::uintptr_t face)
    {
        std::uintptr_t& head = list_head(list);
        int& count = list_count(list);
        const std::uintptr_t old_head = head;
        count++;
        head = face;
        next(face) = old_head;
        auto it = tails.find(list);
        if (!old_head) {
            remember(list, {face, face, count});
        }
        else if (it != tails.end()) {
            if (it->second.head == old_head && it->second.count == count - 1) {
                it->second.head = face;
                it->second.count = count;
            }
            else {
                tails.erase(it);
            }
        }
    }

    template<typename Remove>
    static void remove(std::uintptr_t list, std::uintptr_t face, Remove&& stock_remove)
    {
        const std::uintptr_t head_before = list_head(list);
        const int count_before = list_count(list);
        stock_remove();
        auto it = tails.find(list);
        if (it == tails.end()) {
            return;
        }
        TailEntry& entry = it->second;
        if (entry.tail == face || entry.head != head_before || entry.count != count_before) {
            tails.erase(it);
            return;
        }
        entry.head = list_head(list);
        entry.count = list_count(list);
    }
};

using SolidFaces = FaceList<offsetof(GFace, next_solid)>;
using BBoxFaces = FaceList<offsetof(GFace, next_bbox)>;
using RoomFaces = FaceList<offsetof(GFace, next_room)>;

void clear_all()
{
    if (!SolidFaces::tails.empty()) {
        SolidFaces::tails.clear();
    }
    if (!BBoxFaces::tails.empty()) {
        BBoxFaces::tails.clear();
    }
    if (!RoomFaces::tails.empty()) {
        RoomFaces::tails.clear();
    }
}

void forget(std::uintptr_t list)
{
    SolidFaces::tails.erase(list);
    BBoxFaces::tails.erase(list);
    RoomFaces::tails.erase(list);
}

// The six helpers are thiscall on the list header with the face as the only stack argument.
using ListOp = void __fastcall(std::uintptr_t list, int edx, std::uintptr_t face);

void __fastcall solid_append(std::uintptr_t list, int edx, std::uintptr_t face);
FunHook<ListOp> solid_append_hook{0x00499560, solid_append};
void __fastcall solid_append(std::uintptr_t list, int edx, std::uintptr_t face)
{
    if (cache_live()) {
        SolidFaces::append(list, face);
    }
    else {
        solid_append_hook.call_target(list, edx, face);
    }
}

void __fastcall bbox_append(std::uintptr_t list, int edx, std::uintptr_t face);
FunHook<ListOp> bbox_append_hook{0x0048e630, bbox_append};
void __fastcall bbox_append(std::uintptr_t list, int edx, std::uintptr_t face)
{
    if (cache_live()) {
        BBoxFaces::append(list, face);
    }
    else {
        bbox_append_hook.call_target(list, edx, face);
    }
}

void __fastcall room_append(std::uintptr_t list, int edx, std::uintptr_t face);
FunHook<ListOp> room_append_hook{0x00486b30, room_append};
void __fastcall room_append(std::uintptr_t list, int edx, std::uintptr_t face)
{
    if (cache_live()) {
        RoomFaces::append(list, face);
    }
    else {
        room_append_hook.call_target(list, edx, face);
    }
}

void __fastcall solid_remove(std::uintptr_t list, int edx, std::uintptr_t face);
FunHook<ListOp> solid_remove_hook{0x00486bd0, solid_remove};
void __fastcall solid_remove(std::uintptr_t list, int edx, std::uintptr_t face)
{
    if (cache_live()) {
        SolidFaces::remove(list, face, [&] { solid_remove_hook.call_target(list, edx, face); });
    }
    else {
        solid_remove_hook.call_target(list, edx, face);
    }
}

void __fastcall bbox_remove(std::uintptr_t list, int edx, std::uintptr_t face);
FunHook<ListOp> bbox_remove_hook{0x0048e670, bbox_remove};
void __fastcall bbox_remove(std::uintptr_t list, int edx, std::uintptr_t face)
{
    if (cache_live()) {
        BBoxFaces::remove(list, face, [&] { bbox_remove_hook.call_target(list, edx, face); });
    }
    else {
        bbox_remove_hook.call_target(list, edx, face);
    }
}

void __fastcall room_remove(std::uintptr_t list, int edx, std::uintptr_t face);
FunHook<ListOp> room_remove_hook{0x00486b70, room_remove};
void __fastcall room_remove(std::uintptr_t list, int edx, std::uintptr_t face)
{
    if (cache_live()) {
        RoomFaces::remove(list, face, [&] { room_remove_hook.call_target(list, edx, face); });
    }
    else {
        room_remove_hook.call_target(list, edx, face);
    }
}

// FUN_004a97b0 prepends to a +0x54 list (the CSG's cloned faces).
void __fastcall solid_prepend(std::uintptr_t list, int edx, std::uintptr_t face);
FunHook<ListOp> solid_prepend_hook{0x004a97b0, solid_prepend};
void __fastcall solid_prepend(std::uintptr_t list, int edx, std::uintptr_t face)
{
    if (cache_live()) {
        SolidFaces::prepend(list, face);
    }
    else {
        solid_prepend_hook.call_target(list, edx, face);
    }
}

// The CSG end's whole-list moves end by emptying the global CSG list, which drops the whole cache
// (csg_take_faces covers the begin move); emptying any other header drops just its entry.
void __fastcall list_clear(std::uintptr_t list, int edx);
FunHook<void __fastcall(std::uintptr_t, int)> list_clear_hook{0x004afd80, list_clear};
void __fastcall list_clear(std::uintptr_t list, int edx)
{
    list_clear_hook.call_target(list, edx);
    if (list == reinterpret_cast<std::uintptr_t>(&csg_face_list_head)) {
        clear_all();
    }
    else {
        forget(list);
    }
}

void __fastcall csg_take_faces(std::uintptr_t list, int edx);
CallHook<void __fastcall(std::uintptr_t, int)> csg_take_faces_hook{0x004a7121, csg_take_faces};
void __fastcall csg_take_faces(std::uintptr_t list, int edx)
{
    clear_all();
    csg_take_faces_hook.call_target(list, edx);
}

// FUN_0042f030: the level reader behind CDedDoc::LoadSaveLevel (thiscall, two stack arguments).
int __fastcall level_read(void* self, int edx, void* level, const char* path);
FunHook<int __fastcall(void*, int, void*, const char*)> level_read_hook{0x0042f030, level_read};
int __fastcall level_read(void* self, int edx, void* level, const char* path)
{
    FaceListCacheWindow window;
    return level_read_hook.call_target(self, edx, level, path);
}

// Message handlers and message boxes can reach any editor command, so the cache is off inside them.
struct CachePause
{
    CachePause()
    {
        g_pause_depth++;
        clear_all();
    }
    ~CachePause()
    {
        g_pause_depth--;
    }
    CachePause(const CachePause&) = delete;
    CachePause& operator=(const CachePause&) = delete;
};

decltype(&MessageBoxA) g_message_box = nullptr;

int WINAPI message_box_paused(HWND wnd, LPCSTR text, LPCSTR caption, UINT type)
{
    CachePause pause;
    return g_message_box(wnd, text, caption, type);
}

// AfxCallWndProc: every message an MFC window receives, sent or dispatched, goes through it.
LRESULT __stdcall mfc_call_wnd_proc(void* wnd, HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
FunHook<LRESULT __stdcall(void*, HWND, UINT, WPARAM, LPARAM)> mfc_call_wnd_proc_hook{0x005302d7, mfc_call_wnd_proc};
LRESULT __stdcall mfc_call_wnd_proc(void* wnd, HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (g_window_depth == 0) {
        return mfc_call_wnd_proc_hook.call_target(wnd, hwnd, msg, wparam, lparam);
    }
    CachePause pause;
    return mfc_call_wnd_proc_hook.call_target(wnd, hwnd, msg, wparam, lparam);
}

} // namespace

FaceListCacheWindow::FaceListCacheWindow()
{
    clear_all();
    outer_pause_depth_ = std::exchange(g_pause_depth, 0);
    g_window_depth++;
}

FaceListCacheWindow::~FaceListCacheWindow()
{
    g_window_depth--;
    g_pause_depth = outer_pause_depth_;
    clear_all();
}

void ApplyFaceListCachePatches()
{
    solid_append_hook.install();
    bbox_append_hook.install();
    room_append_hook.install();
    solid_remove_hook.install();
    bbox_remove_hook.install();
    room_remove_hook.install();
    solid_prepend_hook.install();
    list_clear_hook.install();
    csg_take_faces_hook.install();
    level_read_hook.install();
    mfc_call_wnd_proc_hook.install();
    g_message_box = addr_as_ref<decltype(&MessageBoxA)>(red_message_box_iat_slot);
    write_mem_ptr(red_message_box_iat_slot, &message_box_paused);
}
