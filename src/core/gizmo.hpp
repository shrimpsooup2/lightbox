#pragma once

#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "core/overlay.hpp"
#include "core/scene/scene.hpp"

namespace lb {

// Move, rotate and scale handles for the selection: arrows along X/Y/Z, a square for moving on the
// ground, rings for turning about X/Y/Z, small solid squares past the rings for stretching along
// X/Y/Z and a white square in the middle for even scaling. Sized to stay the same on screen.
enum class GizmoHandle {
    None,
    MoveX,
    MoveY,
    MoveZ,
    MovePlane,
    RotateX,
    RotateY,
    RotateZ,
    ScaleX,
    ScaleY,
    ScaleZ,
    ScaleUniform,
};

// One view of the gizmo: positions and sizes in window pixels (what mouse events use).
struct GizmoView {
    glm::mat4 viewProj{1.0f};
    glm::vec3 cameraPosition{0.0f};
    float verticalFov = 1.0f;
    glm::vec2 viewportSize{1.0f};
};

class Gizmo {
public:
    static constexpr float kArrowPixels = 110.0f;

    Gizmo(const glm::vec3& origin, const GizmoView& view);

    const glm::vec3& origin() const { return origin_; }
    float size() const { return size_; } // arrow length in meters

    // The handle under a window position (within a few pixels), or None.
    GizmoHandle pick(const glm::vec2& mouse) const;
    // Lines to draw; `highlight` (hovered or dragged) is drawn in the highlight color. Widths are in
    // window pixels times `pixelScale` (the device pixel ratio).
    void appendLines(std::vector<OverlayLine>& out, GizmoHandle highlight, float pixelScale) const;

private:
    std::optional<glm::vec2> toScreen(const glm::vec3& world) const;
    glm::vec3 ringPoint(GizmoHandle ring, float angle) const;
    glm::vec3 scaleHandlePoint(GizmoHandle handle) const;

    glm::vec3 origin_;
    float size_;
    GizmoView view_;
};

// A move, rotate or scale drag on a selection. Keeps the transforms from the start, so each update
// applies the total change (no drift). Rays are world-space mouse rays.
class GizmoDrag {
public:
    // False when the handle cannot be dragged from this view (e.g. an arrow pointing at the eye).
    bool begin(Scene& scene, std::span<const EntityId> ids, GizmoHandle handle, const Gizmo& gizmo,
               const glm::vec3& rayOrigin, const glm::vec3& rayDirection);
    // `snap`: moves in 0.1 m steps, turns in 15° steps, scales in 10 % steps.
    void update(const glm::vec3& rayOrigin, const glm::vec3& rayDirection, bool snap);
    void end() { entities_.clear(); }

    bool active() const { return !entities_.empty(); }
    GizmoHandle handle() const { return handle_; }
    // Where the gizmo is now (moves with the selection).
    glm::vec3 origin() const { return origin_ + moved_; }
    glm::vec3 moved() const { return moved_; }   // meters so far
    float turned() const { return turned_; }     // radians so far
    glm::vec3 scaled() const { return scaled_; } // factor per world axis so far

private:
    std::optional<float> measure(const glm::vec3& rayOrigin, const glm::vec3& rayDirection) const;
    std::optional<glm::vec3> planeHit(const glm::vec3& rayOrigin,
                                      const glm::vec3& rayDirection) const;
    void apply();

    Scene* scene_ = nullptr;
    std::vector<std::pair<EntityId, Transform>> entities_;
    GizmoHandle handle_ = GizmoHandle::None;
    glm::vec3 origin_{0.0f};
    float size_ = 1.0f; // gizmo arrow length at the start: even-scaling sensitivity
    glm::vec3 axis_{0.0f};
    glm::vec3 right_{1.0f, 0.0f, 0.0f}; // even scaling: screen-right direction in the world
    glm::vec3 facing_{0.0f};            // even scaling: normal of the plane facing the camera
    float start_ = 0.0f; // axis position (move, scale), angle (rotate), right offset (even scale)
    glm::vec3 startPoint_{0.0f}; // ground-plane hit at the start (MovePlane)
    float lastAngle_ = 0.0f;
    float unwrapped_ = 0.0f;
    glm::vec3 moved_{0.0f};
    float turned_ = 0.0f;
    glm::vec3 scaled_{1.0f};
};

} // namespace lb
