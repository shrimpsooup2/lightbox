#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "core/mesh.hpp"

namespace lb {

// Everything needed to put an OrbitCamera back where it was (saved with projects).
struct CameraState {
    glm::vec3 target{0.0f};
    float distance = 25.0f;
    float yawRadians = 0.0f;
    float pitchRadians = 0.0f;
    float focalLength = 30.0f; // mm, 35 mm full-frame equivalent
    bool twoPoint = false;

    bool operator==(const CameraState&) const = default;
};

// Orbit camera around a target point, in world conventions (meters, right-handed, Z-up).
// Produces Vulkan-style matrices: Y flipped in clip space, reverse-Z with an infinite far plane.
class OrbitCamera {
public:
    OrbitCamera();

    // Rotate around the target. Positive yaw turns counter-clockwise seen from above.
    void orbit(float deltaYaw, float deltaPitch);
    // Same turn, but around any world point (e.g. the point under the cursor): the eye and the
    // view rotate together, so that point stays where it is on screen. The orbit target moves along.
    void orbitAround(const glm::vec3& pivot, float deltaYaw, float deltaPitch);
    // Move the target in the view plane by a screen-space delta in pixels.
    void pan(float deltaXPixels, float deltaYPixels, float viewportHeightPixels);
    // Move toward (positive steps) or away from the target, exponentially.
    void dolly(float steps);
    // Fit a bounding box into view, keeping the current orientation.
    void frame(const Bounds& bounds);
    // Resets orientation and position; keeps lens settings.
    void reset();

    // Lens in 35 mm full-frame equivalent focal length (8–300 mm); sets the vertical field of view.
    void setFocalLength(float millimeters);
    float focalLength() const { return focalLength_; }
    // Two-point perspective: the camera stays level so vertical lines stay vertical, and the image is
    // shifted (like a shift lens) to keep the target framed.
    void setTwoPointPerspective(bool enabled) { twoPoint_ = enabled; }
    bool twoPointPerspective() const { return twoPoint_; }
    // Stand at `eyeHeight` meters looking level, from about where the camera is now.
    void moveToEyeLevel(float eyeHeight = 1.6f);
    // Walkthrough: turn the view around the eye (not the orbit target), and move the eye. Forward
    // and right stay on the ground plane, so looking up or down never changes the walking height.
    void look(float deltaYaw, float deltaPitch);
    void walk(float forwardMeters, float rightMeters, float upMeters);

    CameraState state() const;
    // Restores a saved state; values are clamped to the valid ranges.
    void setState(const CameraState& state);

    glm::vec3 position() const;
    glm::vec3 target() const { return target_; }
    float distance() const { return distance_; }
    float verticalFov() const { return verticalFov_; }

    glm::mat4 viewMatrix() const;
    glm::mat4 projectionMatrix(float aspect) const;

private:
    glm::vec3 target_{0.0f};
    float distance_ = 1.0f;
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
    float focalLength_ = 30.0f;
    float verticalFov_ = 0.0f;
    float nearPlane_ = 0.05f;
    bool twoPoint_ = false;
};

// Infinite reverse-Z perspective for Vulkan clip space: depth 1 at `nearPlane`, 0 at infinity.
glm::mat4 infiniteReverseZPerspective(float verticalFov, float aspect, float nearPlane);

} // namespace lb
