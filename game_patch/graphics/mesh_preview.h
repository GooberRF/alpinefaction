#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "../rf/math/matrix.h"
#include "../rf/math/vector.h"

namespace rf
{
    struct VMesh;
}

// A mesh drawn along with the previewed one, placed in that mesh's own space.
struct MeshPreviewPart
{
    std::string filename;
    rf::Vector3 pos;
    rf::Matrix3 orient;
    // Instead of pos / orient: a prop point of the previewed mesh the part is held at as it animates, with the
    // part's own prop `grip` on it, as an entity holds its weapon (0x00418E60).
    int follow_prop = -1;
    std::string grip;
};

// A slowly turning view of one mesh, drawn into a render target that 2D UI then blits. It brings its own camera
// and lights, and puts back the level lighting state it borrows.
// The owner calls release() while the engine is still up; nothing is freed at DLL unload.
class MeshPreview
{
public:
    // Asked once the mesh has loaded, for the parts that sit on it.
    using PartsFn = std::function<std::vector<MeshPreviewPart>(rf::VMesh* mesh)>;

    MeshPreview() = default;
    MeshPreview(const MeshPreview&) = delete;
    MeshPreview& operator=(const MeshPreview&) = delete;

    // A static (.v3d / .v3m), animated (.vfx) or character (.v3c / .vcm) mesh; an empty name, or one that does not
    // load, shows nothing. `zoom` above 1 frames it closer than its box.
    void set_mesh(const std::string& filename, const PartsFn& parts = {}, float zoom = 1.0f);
    // A character mesh (.v3c / .vcm), dressed by `dress` once it has loaded and held in its looping state
    // animation `state_anim`, as an entity stands. Loaded afresh on every call, as set_mesh is, and it starts
    // facing the camera.
    void set_character(const std::string& filename, const std::string& state_anim,
                       const std::function<void(rf::VMesh*)>& dress = {}, const PartsFn& parts = {});
    // Turns the mesh by this frame's step and draws it at `w` x `h`. It switches the render target, so it
    // runs between the frame's 2D drawing, never inside it.
    void render(int w, int h);
    // render() sized for a preview well `w` x `h`, inside its 1 px edge; nothing for a well too small to hold it.
    void render_in_well(int w, int h);
    // The finished picture, or -1 when there is none.
    [[nodiscard]] int bitmap() const
    {
        return rendered_ ? target_bm_ : -1;
    }
    void release();

private:
    struct LoadedPart
    {
        rf::VMesh* mesh; // one of part_meshes_
        rf::Vector3 pos;
        rf::Matrix3 orient;
        int follow_prop;
        int grip; // on `mesh`
    };

    void release_mesh();
    void release_target();

    rf::VMesh* mesh_ = nullptr;
    std::vector<rf::VMesh*> part_meshes_; // each loaded once, freed by release_mesh
    std::vector<LoadedPart> parts_;
    int target_bm_ = -1;
    int target_w_ = 0;
    int target_h_ = 0;
    bool rendered_ = false;
    float yaw_ = 0.0f;
    float zoom_ = 1.0f;
    int action_ = -1; // a character's state animation
    std::optional<std::pair<rf::Vector3, rf::Vector3>> bounds_; // the box framed, from the first render
};
