#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include "level.h"
#include "selection.h"

// Hash-based replacements for RED's quadratic selection routines

namespace
{

// Stands in for stock's linear find, append and remove-first-occurrence on selection arrays for the length of one
// operation, then writes each array back in one pass. An array reads as its kept originals followed by its kept
// appends, which is where stock's own sequence of edits leaves every element. Nothing is written before apply(), so
// an allocation failure leaves the arrays for stock to redo the operation on.
template<typename T>
class SelectionEdits
{
public:
    int size(VArray<T*>& array) const
    {
        const auto it = entries_.find(&array);
        return it != entries_.end() ? it->second.live : array.size;
    }

    bool contains(VArray<T*>& array, T* item)
    {
        Entry& e = entry(array);
        const auto original = e.originals_left.find(item);
        if (original != e.originals_left.end() && original->second > 0) {
            return true;
        }
        const auto appended = e.appended_at.find(item);
        return appended != e.appended_at.end() && !appended->second.empty();
    }

    void append(VArray<T*>& array, T* item)
    {
        Entry& e = entry(array);
        e.appended_at[item].push_back(e.appended.size());
        e.appended.push_back(item);
        e.appended_live.push_back(true);
        ++e.live;
    }

    void add(VArray<T*>& array, T* item)
    {
        if (!contains(array, item)) {
            append(array, item);
        }
    }

    void remove(VArray<T*>& array, T* item)
    {
        Entry& e = entry(array);
        const auto original = e.originals_left.find(item);
        if (original != e.originals_left.end() && original->second > 0) {
            --original->second;
            ++e.originals_removed[item];
            --e.live;
            return;
        }
        const auto appended = e.appended_at.find(item);
        if (appended != e.appended_at.end() && !appended->second.empty()) {
            e.appended_live[appended->second.front()] = false;
            appended->second.erase(appended->second.begin());
            --e.live;
        }
    }

    void clear(VArray<T*>& array)
    {
        Entry& e = entry(array);
        for (auto& [item, left] : e.originals_left) {
            e.originals_removed[item] += left;
            left = 0;
        }
        e.appended.clear();
        e.appended_live.clear();
        e.appended_at.clear();
        e.live = 0;
    }

    void apply()
    {
        for (auto& [array, e] : entries_) {
            if (!e.originals_removed.empty()) {
                int kept = 0;
                for (int i = 0; i < array->size; ++i) {
                    T* item = array->data_ptr[i];
                    const auto removed = e.originals_removed.find(item);
                    if (removed != e.originals_removed.end() && removed->second > 0) {
                        --removed->second;
                        continue;
                    }
                    array->data_ptr[kept++] = item;
                }
                array->size = kept;
            }
            for (size_t i = 0; i < e.appended.size(); ++i) {
                if (e.appended_live[i]) {
                    array->push_back(e.appended[i]);
                }
            }
        }
        entries_.clear();
    }

private:
    struct Entry
    {
        std::unordered_map<T*, int> originals_left;
        std::unordered_map<T*, int> originals_removed;
        std::vector<T*> appended;
        std::vector<bool> appended_live;
        std::unordered_map<T*, std::vector<size_t>> appended_at;
        int live = 0;
    };

    Entry& entry(VArray<T*>& array)
    {
        auto [it, fresh] = entries_.try_emplace(&array);
        if (fresh) {
            for (int i = 0; i < array.size; ++i) {
                ++it->second.originals_left[array.data_ptr[i]];
            }
            it->second.live = array.size;
        }
        return it->second;
    }

    std::unordered_map<VArray<T*>*, Entry> entries_;
};

// Never unwinds into engine frames
template<typename Fast, typename Stock>
void or_stock(Fast&& fast, Stock&& stock)
{
    try {
        fast();
    }
    catch (...) {
        stock();
    }
}

// The brush and level faces sharing a face id, in the order 0x0042C4E0 and 0x0042C530 visit them
class FacesById
{
public:
    using Matches = std::vector<std::pair<GSolid*, GFace*>>;

    explicit FacesById(CDedLevel& level) : level_{level} {}

    const Matches& find(int face_id)
    {
        if (!built_) {
            build();
        }
        const auto it = faces_.find(face_id);
        return it != faces_.end() ? it->second : none_;
    }

private:
    void build()
    {
        built_ = true;
        const auto add_solid = [&](GSolid* solid) {
            if (!solid) return;
            solid->for_each_face([&](GFace& face) {
                faces_[face.face_id].emplace_back(solid, &face);
                return false;
            });
        };
        level_.for_each_brush([&](BrushNode& brush) {
            add_solid(static_cast<GSolid*>(brush.geometry));
        });
        add_solid(level_.solid);
    }

    CDedLevel& level_;
    bool built_ = false;
    std::unordered_map<int, Matches> faces_;
    Matches none_;
};

// 0x0042C4E0: adds every face sharing the id, and refuses id -1
bool select_faces_by_id(SelectionEdits<GFace>& edits, FacesById& faces_by_id, int face_id)
{
    if (face_id == -1) return false;
    for (const auto& [owner, match] : faces_by_id.find(face_id)) {
        edits.add(owner->face_selection, match);
    }
    return true;
}

// 0x0042C530: removes every face sharing the id, -1 included
void deselect_faces_by_id(SelectionEdits<GFace>& edits, FacesById& faces_by_id, int face_id)
{
    for (const auto& [owner, match] : faces_by_id.find(face_id)) {
        edits.remove(owner->face_selection, match);
    }
}

// A face without corners counts as inside
bool face_in_marquee(CDedLevel& level, int corner_a, int corner_b, const BrushNode& brush, const GFace& face)
{
    const GFaceVertex* fv = face.edge_loop;
    if (!fv) return true;
    do {
        const Vector3 world = brush.to_world_stock(fv->vertex->pos);
        if (!level.hit_test_point(corner_a, corner_b, &world)) {
            return false;
        }
        fv = fv->next;
    } while (fv && fv != face.edge_loop);
    return true;
}

FunHook<void __fastcall(CDedLevel*, void*, int, int, char)> face_marquee_select_hook{
    0x0042AF30,
    [](CDedLevel* level, void* edx, int corner_a, int corner_b, char add) FASTCALL_LAMBDA {
        or_stock(
            [&] {
                if (!add) {
                    level->deselect_all_faces();
                }
                const bool by_id = level->edit_mode == DedEditMode::Texture;
                SelectionEdits<GFace> edits;
                FacesById faces_by_id{*level};
                std::unordered_set<int> added_ids;
                level->for_each_brush([&](BrushNode& brush) {
                    auto* solid = static_cast<GSolid*>(brush.geometry);
                    if (!brush.pickable() || !solid) return;
                    solid->for_each_face([&](GFace& face) {
                        if (!face_in_marquee(*level, corner_a, corner_b, brush, face)) {
                            return false;
                        }
                        if (!by_id) {
                            edits.add(solid->face_selection, &face);
                            return false;
                        }
                        // Stock stops at a face without an id and moves on to the next brush
                        if (face.face_id == -1) {
                            return true;
                        }
                        if (added_ids.insert(face.face_id).second) {
                            select_faces_by_id(edits, faces_by_id, face.face_id);
                        }
                        return false;
                    });
                });
                edits.apply();
            },
            [&] { face_marquee_select_hook.call_target(level, edx, corner_a, corner_b, add); });
    },
};

FunHook<void __fastcall(CDedLevel*, void*, int, int)> face_marquee_deselect_hook{
    0x0042B3E0,
    [](CDedLevel* level, void* edx, int corner_a, int corner_b) FASTCALL_LAMBDA {
        or_stock(
            [&] {
                const bool by_id = level->edit_mode == DedEditMode::Texture;
                SelectionEdits<GFace> edits;
                FacesById faces_by_id{*level};
                std::vector<GFace*> hits;
                // Stock repeats an id's removal for every hit, which is a no-op once none of its faces is selected
                std::unordered_set<int> cleared_ids;
                // Unlike select, stock deselect checks no brush state and lets face id -1 through
                level->for_each_brush([&](BrushNode& brush) {
                    auto* solid = static_cast<GSolid*>(brush.geometry);
                    if (!solid || solid->face_selection.size == 0) return;
                    hits.clear();
                    solid->for_each_face([&](GFace& face) {
                        if (edits.contains(solid->face_selection, &face)
                            && face_in_marquee(*level, corner_a, corner_b, brush, face)) {
                            hits.push_back(&face);
                        }
                        return false;
                    });
                    for (GFace* face : hits) {
                        if (!by_id) {
                            edits.remove(solid->face_selection, face);
                            continue;
                        }
                        if (cleared_ids.count(face->face_id) != 0) continue;
                        deselect_faces_by_id(edits, faces_by_id, face->face_id);
                        const auto& matches = faces_by_id.find(face->face_id);
                        if (std::none_of(matches.begin(), matches.end(), [&](const auto& match) {
                                return edits.contains(match.first->face_selection, match.second);
                            })) {
                            cleared_ids.insert(face->face_id);
                        }
                    }
                });
                edits.apply();
            },
            [&] { face_marquee_deselect_hook.call_target(level, edx, corner_a, corner_b); });
    },
};

FunHook<void __fastcall(CDedLevel*, void*, int, int, char)> vertex_marquee_select_hook{
    0x0042B050,
    [](CDedLevel* level, void* edx, int corner_a, int corner_b, char add) FASTCALL_LAMBDA {
        or_stock(
            [&] {
                if (!add) {
                    level->deselect_all_vertices();
                }
                SelectionEdits<GVertex> edits;
                level->for_each_brush([&](BrushNode& brush) {
                    auto* solid = static_cast<GSolid*>(brush.geometry);
                    if (!brush.pickable() || !solid) return;
                    for (int i = 0; i < solid->vertices.size; ++i) {
                        GVertex* vertex = solid->vertices.data_ptr[i];
                        const Vector3 world = brush.to_world_stock(vertex->pos);
                        if (level->hit_test_point(corner_a, corner_b, &world)) {
                            edits.add(solid->vertex_selection, vertex);
                        }
                    }
                });
                edits.apply();
            },
            [&] { vertex_marquee_select_hook.call_target(level, edx, corner_a, corner_b, add); });
    },
};

FunHook<void __fastcall(CDedLevel*, void*, int, int)> vertex_marquee_deselect_hook{
    0x0042B580,
    [](CDedLevel* level, void*, int corner_a, int corner_b) FASTCALL_LAMBDA {
        level->for_each_brush([&](BrushNode& brush) {
            auto* solid = static_cast<GSolid*>(brush.geometry);
            if (!solid) return;
            auto& selection = solid->vertex_selection;
            int kept = 0;
            for (int i = 0; i < selection.size; ++i) {
                GVertex* vertex = selection.data_ptr[i];
                const Vector3 world = brush.to_world_stock(vertex->pos);
                if (!level->hit_test_point(corner_a, corner_b, &world)) {
                    selection.data_ptr[kept++] = vertex;
                }
            }
            selection.size = kept;
        });
    },
};

FunHook<void __fastcall(CDedLevel*)> face_invert_hook{
    0x0042E920,
    [](CDedLevel* level) FASTCALL_LAMBDA {
        or_stock(
            [&] {
                SelectionEdits<GFace> edits;
                level->for_each_brush([&](BrushNode& brush) {
                    auto* solid = static_cast<GSolid*>(brush.geometry);
                    if (!solid || edits.size(solid->face_selection) == 0) return;
                    solid->for_each_face([&](GFace& face) {
                        if (edits.contains(solid->face_selection, &face)) {
                            edits.remove(solid->face_selection, &face);
                        }
                        else {
                            edits.append(solid->face_selection, &face);
                        }
                        return false;
                    });
                });
                edits.apply();
            },
            [&] { face_invert_hook.call_target(level); });
    },
};

FunHook<void __fastcall(CDedLevel*)> texture_invert_hook{
    0x0042E9B0,
    [](CDedLevel* level) FASTCALL_LAMBDA {
        or_stock(
            [&] {
                SelectionEdits<GFace> edits;
                FacesById faces_by_id{*level};
                level->for_each_brush([&](BrushNode& brush) {
                    auto* solid = static_cast<GSolid*>(brush.geometry);
                    if (!solid || edits.size(solid->face_selection) == 0) return;
                    solid->for_each_face([&](GFace& face) {
                        if (edits.contains(solid->face_selection, &face)) {
                            deselect_faces_by_id(edits, faces_by_id, face.face_id);
                        }
                        else {
                            select_faces_by_id(edits, faces_by_id, face.face_id);
                        }
                        return false;
                    });
                });
                edits.apply();
            },
            [&] { texture_invert_hook.call_target(level); });
    },
};

FunHook<void __fastcall(CDedLevel*)> vertex_invert_hook{
    0x0042EA40,
    [](CDedLevel* level) FASTCALL_LAMBDA {
        or_stock(
            [&] {
                SelectionEdits<GVertex> edits;
                level->for_each_brush([&](BrushNode& brush) {
                    auto* solid = static_cast<GSolid*>(brush.geometry);
                    if (!solid || edits.size(solid->vertex_selection) == 0) return;
                    for (int i = 0; i < solid->vertices.size; ++i) {
                        GVertex* vertex = solid->vertices.data_ptr[i];
                        if (edits.contains(solid->vertex_selection, vertex)) {
                            edits.remove(solid->vertex_selection, vertex);
                        }
                        else {
                            edits.append(solid->vertex_selection, vertex);
                        }
                    }
                });
                edits.apply();
            },
            [&] { vertex_invert_hook.call_target(level); });
    },
};

// Shift+S: every face of each brush with a selected face. Face mode's per-brush fill (0x00490E30) is linear.
FunHook<void __fastcall(CDedLevel*)> select_brush_faces_hook{
    0x0042D840,
    [](CDedLevel* level) FASTCALL_LAMBDA {
        if (level->edit_mode != DedEditMode::Texture) {
            select_brush_faces_hook.call_target(level);
            return;
        }
        or_stock(
            [&] {
                SelectionEdits<GFace> edits;
                FacesById faces_by_id{*level};
                if (level->solid) {
                    edits.clear(level->solid->face_selection);
                }
                // A brush whose faces an earlier brush selected by id counts as selected, so ids repeat
                level->for_each_brush([&](BrushNode& brush) {
                    auto* solid = static_cast<GSolid*>(brush.geometry);
                    if (!brush.pickable() || !solid || edits.size(solid->face_selection) <= 0) return;
                    edits.clear(solid->face_selection);
                    solid->for_each_face([&](GFace& face) {
                        return !select_faces_by_id(edits, faces_by_id, face.face_id);
                    });
                });
                edits.apply();
            },
            [&] { select_brush_faces_hook.call_target(level); });
    },
};

// Shift+D in Texture mode (inline in the view key handler): the faces of each brush with a selected face whose
// texture a selected face uses. Stock also gathers the selected face ids, which it never reads.
CodeInjection select_same_texture_faces_injection{
    0x0047C3A0,
    [](auto& regs) {
        auto* level = CDedLevel::Get();
        if (!level) return;
        try {
            std::unordered_set<int> textures;
            std::vector<GSolid*> solids;
            level->for_each_brush([&](BrushNode& brush) {
                auto* solid = static_cast<GSolid*>(brush.geometry);
                if (!brush.pickable() || !solid || solid->face_selection.size <= 0) return;
                solids.push_back(solid);
                for (int i = 0; i < solid->face_selection.size; ++i) {
                    textures.insert(solid->face_selection.data_ptr[i]->bitmap_id);
                }
            });
            SelectionEdits<GFace> edits;
            FacesById faces_by_id{*level};
            std::unordered_set<int> added_ids;
            for (GSolid* solid : solids) {
                solid->for_each_face([&](GFace& face) {
                    if (textures.count(face.bitmap_id) != 0 && added_ids.insert(face.face_id).second) {
                        select_faces_by_id(edits, faces_by_id, face.face_id);
                    }
                    return false;
                });
            }
            edits.apply();
            regs.eip = 0x0047C4B3;
        }
        catch (...) {
            // Stock's block runs instead
        }
    },
};

// Drawing a solid's vertex marks finds each vertex in its selection (0x0042AB70 at 0x0049A8FE); a draw changes no
// selection, so for the length of one draw (0x0049A760) the answers come from one set
class DrawnVertexSelection
{
public:
    explicit DrawnVertexSelection(const VArray<GVertex*>& selection) : selection_{selection} {}

    // -1 when the stock find should answer
    int find(const VArray<GVertex*>& selection, GVertex* vertex)
    {
        constexpr int stock_find_max_size = 16;
        if (&selection != &selection_ || selection.size <= stock_find_max_size || failed_) return -1;
        if (!vertices_) {
            try {
                vertices_.emplace(selection.data_ptr, selection.data_ptr + selection.size);
            }
            catch (...) {
                vertices_.reset();
                failed_ = true;
                return -1;
            }
        }
        return vertices_->count(vertex) != 0 ? 1 : 0;
    }

private:
    const VArray<GVertex*>& selection_;
    std::optional<std::unordered_set<GVertex*>> vertices_;
    bool failed_ = false;
};

DrawnVertexSelection* g_drawn_vertex_selection = nullptr;

FunHook<void __fastcall(GSolid*, void*, char, int, char, const Vector3*, const Matrix3*, int, int, int)> solid_draw_hook{
    0x0049A760,
    [](GSolid* solid, void* edx, char outline, int face_outlines, char vertex_marks, const Vector3* pos,
       const Matrix3* orient, int r, int g, int b) FASTCALL_LAMBDA {
        if (!vertex_marks) {
            solid_draw_hook.call_target(solid, edx, outline, face_outlines, vertex_marks, pos, orient, r, g, b);
            return;
        }
        DrawnVertexSelection drawn{solid->vertex_selection};
        DrawnVertexSelection* const outer = std::exchange(g_drawn_vertex_selection, &drawn);
        solid_draw_hook.call_target(solid, edx, outline, face_outlines, vertex_marks, pos, orient, r, g, b);
        g_drawn_vertex_selection = outer;
    },
};

// Its caller only tests the sign
CallHook<int __fastcall(VArray<GVertex*>*, void*, GVertex*)> solid_draw_vertex_selected_hook{
    0x0049A8FE,
    [](VArray<GVertex*>* selection, void* edx, GVertex* vertex) FASTCALL_LAMBDA {
        const int found = g_drawn_vertex_selection ? g_drawn_vertex_selection->find(*selection, vertex) : -1;
        if (found < 0) {
            return solid_draw_vertex_selected_hook.call_target(selection, edx, vertex);
        }
        return found ? 0 : -1;
    },
};

// Brush mode logs each selected brush's index in the brush list, which stock found by walking the list per brush
CodeInjection brush_selection_log_injection{
    0x0043F7E0,
    [](auto& regs) {
        auto* level = reinterpret_cast<CDedLevel*>(static_cast<uintptr_t>(regs.ebx));
        const auto& selected = addr_as_ref<VArray<BrushNode*>>(static_cast<uintptr_t>(regs.esp) + 0x18);
        try {
            std::unordered_map<const BrushNode*, int> list_index;
            int count = 0;
            level->for_each_brush([&](BrushNode& brush) {
                list_index.emplace(&brush, count++);
            });
            for (int i = 0; i < selected.size; ++i) {
                BrushNode* brush = selected.data_ptr[i];
                auto* solid = static_cast<GSolid*>(brush->geometry);
                const Vector3 size = solid ? solid->vertex_bbox_size() : Vector3{};
                const auto it = list_index.find(brush);
                const int index = it != list_index.end() ? it->second : count - 1;
                LogDlg_Append(GetLogDlg(), "%d: h=%0.2f w=%0.2f d=%0.2f t=%d\n", brush->uid, size.y, size.x,
                              size.z, index);
            }
        }
        catch (...) {
            return; // only the map can fail, before any line, so stock's loop runs instead
        }
        regs.eip = 0x0043F883;
    },
};

} // namespace

void ApplySelectionPatches()
{
    face_marquee_select_hook.install();
    face_marquee_deselect_hook.install();
    vertex_marquee_select_hook.install();
    vertex_marquee_deselect_hook.install();
    face_invert_hook.install();
    texture_invert_hook.install();
    vertex_invert_hook.install();
    select_brush_faces_hook.install();
    select_same_texture_faces_injection.install();
    solid_draw_hook.install();
    solid_draw_vertex_selected_hook.install();
    brush_selection_log_injection.install();
}
