#include "core/gizmo.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/matrix.hpp>
#include <glm/vec4.hpp>

namespace lb {
namespace {

constexpr float kShaftEnd = 0.78f;   // arrow shaft, as a fraction of the arrow length
constexpr float kRingRadius = 1.25f; // rings sit outside the arrows
constexpr float kPlaneNear = 0.22f;  // ground-move square, fraction of the arrow length
constexpr float kPlaneFar = 0.42f;
constexpr int kRingSegments = 64;
constexpr float kPickPixels = 8.0f;
constexpr float kMoveSnap = 0.1f; // meters
constexpr float kTurnSnap = glm::pi<float>() / 12.0f; // 15°
constexpr float kScaleAt = 1.5f;      // axis scale squares, past the rings
constexpr float kCubeHalf = 0.065f;   // half side of a scale square, fraction of the arrow length
constexpr float kSquarePickPixels = 11.0f;
constexpr float kScaleSnap = 0.1f;    // 10 % steps
constexpr float kMinScale = 0.01f;
constexpr float kMaxScale = 100.0f;

constexpr glm::vec4 kHighlight{1.0f, 0.82f, 0.2f, 1.0f};
constexpr glm::vec4 kShadow{0.0f, 0.0f, 0.0f, 0.35f};

glm::vec3 axisOf(GizmoHandle handle) {
    switch (handle) {
    case GizmoHandle::MoveX:
    case GizmoHandle::RotateX:
    case GizmoHandle::ScaleX:
        return {1.0f, 0.0f, 0.0f};
    case GizmoHandle::MoveY:
    case GizmoHandle::RotateY:
    case GizmoHandle::ScaleY:
        return {0.0f, 1.0f, 0.0f};
    default:
        return {0.0f, 0.0f, 1.0f};
    }
}

glm::vec4 colorOf(GizmoHandle handle) {
    switch (handle) {
    case GizmoHandle::MoveX:
    case GizmoHandle::RotateX:
    case GizmoHandle::ScaleX:
        return {0.93f, 0.26f, 0.26f, 1.0f};
    case GizmoHandle::MoveY:
    case GizmoHandle::RotateY:
    case GizmoHandle::ScaleY:
        return {0.42f, 0.82f, 0.24f, 1.0f};
    case GizmoHandle::MovePlane:
    case GizmoHandle::ScaleUniform:
        return {0.92f, 0.92f, 0.92f, 1.0f};
    default:
        return {0.25f, 0.52f, 1.0f, 1.0f};
    }
}

bool isRotate(GizmoHandle handle) {
    return handle == GizmoHandle::RotateX || handle == GizmoHandle::RotateY ||
           handle == GizmoHandle::RotateZ;
}

bool isScaleAxis(GizmoHandle handle) {
    return handle == GizmoHandle::ScaleX || handle == GizmoHandle::ScaleY ||
           handle == GizmoHandle::ScaleZ;
}

bool isScale(GizmoHandle handle) {
    return isScaleAxis(handle) || handle == GizmoHandle::ScaleUniform;
}

// In-plane basis (u, w) with u × w = axis, so atan2 measures counter-clockwise about the axis.
std::pair<glm::vec3, glm::vec3> ringBasis(const glm::vec3& axis) {
    if (axis.x > 0.5f) {
        return {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    }
    if (axis.y > 0.5f) {
        return {{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}};
    }
    return {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
}

float segmentDistance(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b) {
    const glm::vec2 ab = b - a;
    const float lengthSquared = glm::dot(ab, ab);
    const float t = lengthSquared > 0.0f ? std::clamp(glm::dot(p - a, ab) / lengthSquared, 0.0f, 1.0f)
                                         : 0.0f;
    return glm::length(p - (a + ab * t));
}

float cross2(const glm::vec2& a, const glm::vec2& b) {
    return a.x * b.y - a.y * b.x;
}

std::optional<glm::vec3> rayPlane(const glm::vec3& point, const glm::vec3& normal,
                                  const glm::vec3& rayOrigin, const glm::vec3& rayDirection,
                                  float minCosine) {
    const float denom = glm::dot(rayDirection, normal);
    if (std::abs(denom) < minCosine) {
        return std::nullopt;
    }
    const float t = glm::dot(point - rayOrigin, normal) / denom;
    if (t < 0.0f) {
        return std::nullopt;
    }
    return rayOrigin + rayDirection * t;
}

float snapped(float value, float step) {
    return std::round(value / step) * step;
}

} // namespace

Gizmo::Gizmo(const glm::vec3& origin, const GizmoView& view) : origin_(origin), view_(view) {
    const float distance = std::max(glm::length(origin - view.cameraPosition), 0.01f);
    const float metersPerPixel =
        2.0f * distance * std::tan(view.verticalFov * 0.5f) / std::max(view.viewportSize.y, 1.0f);
    size_ = kArrowPixels * metersPerPixel;
}

std::optional<glm::vec2> Gizmo::toScreen(const glm::vec3& world) const {
    const glm::vec4 clip = view_.viewProj * glm::vec4(world, 1.0f);
    if (clip.w <= 1e-4f) {
        return std::nullopt;
    }
    // Vulkan clip space has y down, like window coordinates.
    return (glm::vec2(clip) / clip.w * 0.5f + 0.5f) * view_.viewportSize;
}

glm::vec3 Gizmo::ringPoint(GizmoHandle ring, float angle) const {
    const auto [u, w] = ringBasis(axisOf(ring));
    return origin_ + (u * std::cos(angle) + w * std::sin(angle)) * (kRingRadius * size_);
}

GizmoHandle Gizmo::pick(const glm::vec2& mouse) const {
    // The small solid squares come first: they sit on top of the arrows and rings.
    GizmoHandle nearest = GizmoHandle::None;
    float nearestDistance = kSquarePickPixels;
    for (const GizmoHandle handle : {GizmoHandle::ScaleUniform, GizmoHandle::ScaleX,
                                     GizmoHandle::ScaleY, GizmoHandle::ScaleZ}) {
        if (const std::optional<glm::vec2> s = toScreen(scaleHandlePoint(handle))) {
            const float d = glm::length(mouse - *s);
            if (d < nearestDistance) {
                nearestDistance = d;
                nearest = handle;
            }
        }
    }
    if (nearest != GizmoHandle::None) {
        return nearest;
    }
    // The ground square wins when the cursor is inside it.
    const glm::vec3 corners[4] = {
        origin_ + glm::vec3(kPlaneNear, kPlaneNear, 0.0f) * size_,
        origin_ + glm::vec3(kPlaneFar, kPlaneNear, 0.0f) * size_,
        origin_ + glm::vec3(kPlaneFar, kPlaneFar, 0.0f) * size_,
        origin_ + glm::vec3(kPlaneNear, kPlaneFar, 0.0f) * size_,
    };
    glm::vec2 screen[4];
    bool visible = true;
    for (int i = 0; i < 4; ++i) {
        const std::optional<glm::vec2> s = toScreen(corners[i]);
        visible = visible && s.has_value();
        screen[i] = s.value_or(glm::vec2(0.0f));
    }
    if (visible) {
        int positive = 0;
        for (int i = 0; i < 4; ++i) {
            positive += cross2(screen[(i + 1) % 4] - screen[i], mouse - screen[i]) > 0.0f ? 1 : 0;
        }
        if (positive == 0 || positive == 4) {
            return GizmoHandle::MovePlane;
        }
    }

    GizmoHandle best = GizmoHandle::None;
    float bestDistance = kPickPixels;
    const auto consider = [&](GizmoHandle handle, const glm::vec3& a, const glm::vec3& b) {
        const std::optional<glm::vec2> sa = toScreen(a);
        const std::optional<glm::vec2> sb = toScreen(b);
        if (sa && sb) {
            const float d = segmentDistance(mouse, *sa, *sb);
            if (d < bestDistance) {
                bestDistance = d;
                best = handle;
            }
        }
    };
    for (const GizmoHandle arrow : {GizmoHandle::MoveX, GizmoHandle::MoveY, GizmoHandle::MoveZ}) {
        // The part near the center belongs to the square and the other arrows.
        consider(arrow, origin_ + axisOf(arrow) * (0.15f * size_), origin_ + axisOf(arrow) * size_);
    }
    for (const GizmoHandle ring :
         {GizmoHandle::RotateX, GizmoHandle::RotateY, GizmoHandle::RotateZ}) {
        for (int i = 0; i < kRingSegments; ++i) {
            const float step = glm::two_pi<float>() / kRingSegments;
            consider(ring, ringPoint(ring, step * static_cast<float>(i)),
                     ringPoint(ring, step * static_cast<float>(i + 1)));
        }
    }
    return best;
}

void Gizmo::appendLines(std::vector<OverlayLine>& out, GizmoHandle highlight,
                        float pixelScale) const {
    // Each line gets a soft dark outline underneath, so handles read on bright and dark scenes.
    const auto line = [&](const glm::vec3& a, const glm::vec3& b, float widthA, float widthB,
                          GizmoHandle handle) {
        out.push_back({a, (widthA + 2.0f) * pixelScale, b, (widthB + 2.0f) * pixelScale, kShadow});
        out.push_back({a, widthA * pixelScale, b, widthB * pixelScale,
                       handle == highlight ? kHighlight : colorOf(handle)});
    };
    for (const GizmoHandle ring :
         {GizmoHandle::RotateX, GizmoHandle::RotateY, GizmoHandle::RotateZ}) {
        const float step = glm::two_pi<float>() / kRingSegments;
        for (int i = 0; i < kRingSegments; ++i) {
            line(ringPoint(ring, step * static_cast<float>(i)),
                 ringPoint(ring, step * static_cast<float>(i + 1)), 2.5f, 2.5f, ring);
        }
    }
    const glm::vec3 corners[4] = {
        origin_ + glm::vec3(kPlaneNear, kPlaneNear, 0.0f) * size_,
        origin_ + glm::vec3(kPlaneFar, kPlaneNear, 0.0f) * size_,
        origin_ + glm::vec3(kPlaneFar, kPlaneFar, 0.0f) * size_,
        origin_ + glm::vec3(kPlaneNear, kPlaneFar, 0.0f) * size_,
    };
    for (int i = 0; i < 4; ++i) {
        line(corners[i], corners[(i + 1) % 4], 2.0f, 2.0f, GizmoHandle::MovePlane);
    }
    for (const GizmoHandle arrow : {GizmoHandle::MoveX, GizmoHandle::MoveY, GizmoHandle::MoveZ}) {
        const glm::vec3 axis = axisOf(arrow);
        line(origin_, origin_ + axis * (kShaftEnd * size_), 3.0f, 3.0f, arrow);
        line(origin_ + axis * (kShaftEnd * size_), origin_ + axis * size_, 13.0f, 0.0f, arrow);
    }
    // Scale squares: drawn facing the camera (a short horizontal segment, as wide as it is long).
    glm::vec3 right = glm::cross(origin_ - view_.cameraPosition, glm::vec3(0.0f, 0.0f, 1.0f));
    right = glm::length(right) > 1e-6f ? glm::normalize(right) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 half = right * (kCubeHalf * size_);
    const float side = 2.0f * kCubeHalf * kArrowPixels;
    for (const GizmoHandle handle : {GizmoHandle::ScaleX, GizmoHandle::ScaleY, GizmoHandle::ScaleZ}) {
        const glm::vec3 center = scaleHandlePoint(handle);
        line(origin_ + axisOf(handle) * (kRingRadius * size_), center, 1.5f, 1.5f, handle);
        line(center - half, center + half, side, side, handle);
    }
    line(origin_ - half, origin_ + half, side, side, GizmoHandle::ScaleUniform);
}

glm::vec3 Gizmo::scaleHandlePoint(GizmoHandle handle) const {
    return handle == GizmoHandle::ScaleUniform ? origin_
                                               : origin_ + axisOf(handle) * (kScaleAt * size_);
}

bool GizmoDrag::begin(Scene& scene, std::span<const EntityId> ids, GizmoHandle handle,
                      const Gizmo& gizmo, const glm::vec3& rayOrigin,
                      const glm::vec3& rayDirection) {
    entities_.clear();
    if (handle == GizmoHandle::None) {
        return false;
    }
    scene_ = &scene;
    handle_ = handle;
    origin_ = gizmo.origin();
    size_ = gizmo.size();
    axis_ = axisOf(handle);
    moved_ = glm::vec3(0.0f);
    turned_ = 0.0f;
    unwrapped_ = 0.0f;
    scaled_ = glm::vec3(1.0f);
    if (handle == GizmoHandle::MovePlane) {
        const std::optional<glm::vec3> hit = planeHit(rayOrigin, rayDirection);
        if (!hit) {
            return false;
        }
        startPoint_ = *hit;
    } else {
        if (handle == GizmoHandle::ScaleUniform) {
            // Even scaling follows the mouse left/right on a plane facing the camera.
            facing_ = rayDirection;
            right_ = glm::cross(rayDirection, glm::vec3(0.0f, 0.0f, 1.0f));
            right_ = glm::length(right_) > 1e-3f ? glm::normalize(right_)
                                                 : glm::vec3(1.0f, 0.0f, 0.0f);
        }
        const std::optional<float> start = measure(rayOrigin, rayDirection);
        if (!start || (isScaleAxis(handle) && std::abs(*start) < 0.05f * size_)) {
            return false;
        }
        start_ = *start;
        lastAngle_ = *start;
    }
    for (const EntityId id : outermostEntities(scene, ids)) {
        entities_.emplace_back(id, scene.entity(id)->transform);
    }
    return !entities_.empty();
}

// Moves and axis scaling: position along the axis line closest to the ray. Turns: angle of the
// ray's hit on the ring's plane. Even scaling: the hit's offset to the right on the plane facing the
// camera. Nullopt when the ray runs (nearly) along the axis or the plane.
std::optional<float> GizmoDrag::measure(const glm::vec3& rayOrigin,
                                        const glm::vec3& rayDirection) const {
    if (handle_ == GizmoHandle::ScaleUniform) {
        const std::optional<glm::vec3> hit =
            rayPlane(origin_, facing_, rayOrigin, rayDirection, 1e-3f);
        return hit ? std::optional<float>(glm::dot(*hit - origin_, right_)) : std::nullopt;
    }
    if (isRotate(handle_)) {
        const std::optional<glm::vec3> hit = rayPlane(origin_, axis_, rayOrigin, rayDirection, 0.08f);
        if (!hit) {
            return std::nullopt;
        }
        const auto [u, w] = ringBasis(axis_);
        const glm::vec3 v = *hit - origin_;
        return std::atan2(glm::dot(v, w), glm::dot(v, u));
    }
    const float b = glm::dot(axis_, rayDirection);
    const float denom = 1.0f - b * b;
    if (denom < 1e-3f) {
        return std::nullopt;
    }
    const glm::vec3 w0 = rayOrigin - origin_;
    return (glm::dot(axis_, w0) - b * glm::dot(rayDirection, w0)) / denom;
}

std::optional<glm::vec3> GizmoDrag::planeHit(const glm::vec3& rayOrigin,
                                             const glm::vec3& rayDirection) const {
    return rayPlane(origin_, {0.0f, 0.0f, 1.0f}, rayOrigin, rayDirection, 1e-3f);
}

void GizmoDrag::update(const glm::vec3& rayOrigin, const glm::vec3& rayDirection, bool snap) {
    if (!active()) {
        return;
    }
    if (handle_ == GizmoHandle::MovePlane) {
        const std::optional<glm::vec3> hit = planeHit(rayOrigin, rayDirection);
        if (!hit) {
            return;
        }
        moved_ = glm::vec3(hit->x - startPoint_.x, hit->y - startPoint_.y, 0.0f);
        if (snap) {
            moved_ = glm::vec3(snapped(moved_.x, kMoveSnap), snapped(moved_.y, kMoveSnap), 0.0f);
        }
    } else {
        const std::optional<float> value = measure(rayOrigin, rayDirection);
        if (!value) {
            return;
        }
        if (isRotate(handle_)) {
            unwrapped_ += std::remainder(*value - lastAngle_, glm::two_pi<float>());
            lastAngle_ = *value;
            turned_ = snap ? snapped(unwrapped_, kTurnSnap) : unwrapped_;
        } else if (isScale(handle_)) {
            // Axis: the handle follows the mouse (twice as far out = twice as long). Even: one
            // gizmo length to the right doubles, to the left halves.
            float factor = handle_ == GizmoHandle::ScaleUniform
                               ? std::exp2((*value - start_) / size_)
                               : *value / start_;
            factor = std::clamp(factor, kMinScale, kMaxScale);
            if (snap) {
                factor = std::max(snapped(factor, kScaleSnap), kScaleSnap);
            }
            scaled_ = handle_ == GizmoHandle::ScaleUniform
                          ? glm::vec3(factor)
                          : glm::vec3(1.0f) + axis_ * (factor - 1.0f);
        } else {
            const float distance = *value - start_;
            moved_ = axis_ * (snap ? snapped(distance, kMoveSnap) : distance);
        }
    }
    apply();
}

void GizmoDrag::apply() {
    const glm::quat turn = glm::angleAxis(turned_, axis_);
    for (const auto& [id, start] : entities_) {
        const Entity* entity = scene_->entity(id);
        if (entity == nullptr) {
            continue; // deleted during the drag
        }
        const glm::mat4 parentWorld =
            entity->parent ? scene_->worldMatrix(entity->parent) : glm::mat4(1.0f);
        const glm::mat4 toParent = glm::inverse(parentWorld);
        const glm::mat3 parentLinear(parentWorld);
        const glm::quat parentRotation = glm::quat_cast(
            glm::mat3(glm::normalize(parentLinear[0]), glm::normalize(parentLinear[1]),
                      glm::normalize(parentLinear[2])));
        const glm::vec3 world(parentWorld * glm::vec4(start.position, 1.0f));
        Transform transform = start;
        if (isRotate(handle_)) {
            const glm::vec3 turned = origin_ + turn * (world - origin_);
            transform.position = glm::vec3(toParent * glm::vec4(turned, 1.0f));
            transform.rotation = glm::normalize(glm::inverse(parentRotation) * turn *
                                                parentRotation * start.rotation);
        } else if (isScale(handle_)) {
            // Positions spread out from the gizmo origin along world axes.
            const glm::vec3 spread = origin_ + (world - origin_) * scaled_;
            transform.position = glm::vec3(toParent * glm::vec4(spread, 1.0f));
            if (handle_ == GizmoHandle::ScaleUniform) {
                transform.scale = start.scale * scaled_.x;
            } else {
                // A turned object stretches along its own axis closest to the dragged one.
                const glm::quat rotation = parentRotation * start.rotation;
                int closest = 0;
                float best = -1.0f;
                for (int i = 0; i < 3; ++i) {
                    glm::vec3 local(0.0f);
                    local[i] = 1.0f;
                    const float alignment = std::abs(glm::dot(rotation * local, axis_));
                    if (alignment > best) {
                        best = alignment;
                        closest = i;
                    }
                }
                transform.scale[closest] *= glm::dot(scaled_, axis_);
            }
        } else {
            transform.position += glm::vec3(toParent * glm::vec4(moved_, 0.0f));
        }
        scene_->setTransform(id, transform);
    }
}

} // namespace lb
