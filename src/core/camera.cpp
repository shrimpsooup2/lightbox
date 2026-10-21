#include "core/camera.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace lb {
namespace {

constexpr glm::vec3 kUp{0.0f, 0.0f, 1.0f};
constexpr float kMinDistance = 0.1f;
constexpr float kMaxDistance = 100000.0f;
// Keep a small margin from straight up/down so lookAt never degenerates.
constexpr float kMaxPitch = glm::half_pi<float>() - 0.01f;
constexpr float kSensorHeightMm = 24.0f; // 35 mm full-frame format

// Rodrigues' rotation of v about the unit vector k.
glm::vec3 rotateAbout(const glm::vec3& v, const glm::vec3& k, float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return v * c + glm::cross(k, v) * s + k * glm::dot(k, v) * (1.0f - c);
}

float focalToVerticalFov(float millimeters) {
    return 2.0f * std::atan(0.5f * kSensorHeightMm / millimeters);
}

} // namespace

OrbitCamera::OrbitCamera() {
    setFocalLength(30.0f);
    reset();
}

void OrbitCamera::reset() {
    target_ = glm::vec3(0.0f, 0.0f, 1.5f);
    distance_ = 25.0f;
    yaw_ = glm::radians(-55.0f);
    pitch_ = glm::radians(25.0f);
}

void OrbitCamera::setFocalLength(float millimeters) {
    focalLength_ = std::clamp(millimeters, 8.0f, 300.0f);
    verticalFov_ = focalToVerticalFov(focalLength_);
}

void OrbitCamera::moveToEyeLevel(float eyeHeight) {
    const glm::vec3 eye = position();
    pitch_ = 0.0f;
    target_.z = eyeHeight;
    // Keep standing roughly where the camera was, looking the same compass direction.
    const glm::vec3 offset{std::cos(yaw_), std::sin(yaw_), 0.0f};
    target_.x = eye.x - offset.x * distance_;
    target_.y = eye.y - offset.y * distance_;
}

void OrbitCamera::look(float deltaYaw, float deltaPitch) {
    const glm::vec3 eye = position();
    yaw_ = std::remainder(yaw_ + deltaYaw, glm::two_pi<float>());
    pitch_ = std::clamp(pitch_ + deltaPitch, -kMaxPitch, kMaxPitch);
    const float cosPitch = std::cos(pitch_);
    const glm::vec3 offset{cosPitch * std::cos(yaw_), cosPitch * std::sin(yaw_), std::sin(pitch_)};
    target_ = eye - offset * distance_;
}

void OrbitCamera::walk(float forwardMeters, float rightMeters, float upMeters) {
    // The camera looks from position() toward the target, i.e. along −offset.
    const glm::vec3 forward{-std::cos(yaw_), -std::sin(yaw_), 0.0f};
    const glm::vec3 right = glm::cross(forward, kUp);
    target_ += forward * forwardMeters + right * rightMeters + kUp * upMeters;
}

void OrbitCamera::orbit(float deltaYaw, float deltaPitch) {
    yaw_ = std::remainder(yaw_ + deltaYaw, glm::two_pi<float>());
    pitch_ = std::clamp(pitch_ + deltaPitch, -kMaxPitch, kMaxPitch);
}

// The change of (yaw, pitch) is a rotation of the whole camera frame: first about the horizontal
// axis (sin yaw, −cos yaw, 0), which raises the eye-from-target direction by the pitch change, then
// about world Z by the yaw change. Applying that rotation to the eye around the pivot keeps the
// pivot fixed relative to the camera.
void OrbitCamera::orbitAround(const glm::vec3& pivot, float deltaYaw, float deltaPitch) {
    const glm::vec3 axis{std::sin(yaw_), -std::cos(yaw_), 0.0f};
    const float oldPitch = pitch_;
    const glm::vec3 eye = position();
    orbit(deltaYaw, deltaPitch);
    const glm::vec3 relative = rotateAbout(rotateAbout(eye - pivot, axis, pitch_ - oldPitch), kUp,
                                           deltaYaw);
    const float cosPitch = std::cos(pitch_);
    const glm::vec3 offset{cosPitch * std::cos(yaw_), cosPitch * std::sin(yaw_), std::sin(pitch_)};
    target_ = pivot + relative - offset * distance_;
}

void OrbitCamera::pan(float deltaXPixels, float deltaYPixels, float viewportHeightPixels) {
    if (viewportHeightPixels <= 0.0f) {
        return;
    }
    const float worldPerPixel =
        2.0f * distance_ * std::tan(verticalFov_ * 0.5f) / viewportHeightPixels;
    const glm::vec3 forward = glm::normalize(target_ - position());
    const glm::vec3 right = glm::normalize(glm::cross(forward, kUp));
    const glm::vec3 up = glm::cross(right, forward);
    target_ += (-deltaXPixels * right + deltaYPixels * up) * worldPerPixel;
}

void OrbitCamera::dolly(float steps) {
    distance_ = std::clamp(distance_ * std::pow(0.85f, steps), kMinDistance, kMaxDistance);
}

void OrbitCamera::frame(const Bounds& bounds) {
    target_ = bounds.center();
    const float radius = std::max(glm::length(bounds.size()) * 0.5f, 0.5f);
    distance_ = std::clamp(radius / std::sin(verticalFov_ * 0.5f) * 1.1f, kMinDistance, kMaxDistance);
}

glm::vec3 OrbitCamera::position() const {
    const float cosPitch = std::cos(pitch_);
    const glm::vec3 offset{cosPitch * std::cos(yaw_), cosPitch * std::sin(yaw_), std::sin(pitch_)};
    return target_ + offset * distance_;
}

glm::mat4 OrbitCamera::viewMatrix() const {
    if (twoPoint_) {
        // Level camera: verticals stay vertical; projectionMatrix() shifts the image instead.
        const glm::vec3 eye = position();
        const glm::vec3 level{-std::cos(yaw_), -std::sin(yaw_), 0.0f};
        return glm::lookAtRH(eye, eye + level, kUp);
    }
    return glm::lookAtRH(position(), target_, kUp);
}

glm::mat4 OrbitCamera::projectionMatrix(float aspect) const {
    glm::mat4 m = infiniteReverseZPerspective(verticalFov_, aspect, nearPlane_);
    if (twoPoint_) {
        // Vertical lens shift that puts the orbit target back at the image center, like a
        // tilt-shift lens: clip.y gains shift * z_view.
        m[2][1] = std::tan(pitch_) / std::tan(verticalFov_ * 0.5f);
    }
    return m;
}

CameraState OrbitCamera::state() const {
    return {target_, distance_, yaw_, pitch_, focalLength_, twoPoint_};
}

void OrbitCamera::setState(const CameraState& state) {
    target_ = state.target;
    distance_ = std::clamp(state.distance, kMinDistance, kMaxDistance);
    yaw_ = std::remainder(state.yawRadians, glm::two_pi<float>());
    pitch_ = std::clamp(state.pitchRadians, -kMaxPitch, kMaxPitch);
    setFocalLength(state.focalLength);
    twoPoint_ = state.twoPoint;
}

glm::mat4 infiniteReverseZPerspective(float verticalFov, float aspect, float nearPlane) {
    const float f = 1.0f / std::tan(verticalFov * 0.5f);
    glm::mat4 m(0.0f);
    m[0][0] = f / aspect;
    m[1][1] = -f; // Vulkan clip space has +Y pointing down
    m[2][3] = -1.0f;
    m[3][2] = nearPlane;
    return m;
}

} // namespace lb
