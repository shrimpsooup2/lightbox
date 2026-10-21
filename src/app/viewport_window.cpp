#include "app/viewport_window.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <exception>
#include <filesystem>

#include <QDropEvent>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>
#include <QTimer>
#include <QVulkanInstance>
#include <QWheelEvent>

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include "app/import_panel.hpp"
#include "core/log.hpp"
#include "core/raycast.hpp"
#include "core/solar.hpp"

namespace lb::app {
namespace {

constexpr float kOrbitRadiansPerPixel = 0.006f;
constexpr float kLookRadiansPerPixel = 0.004f;
constexpr qreal kClickSlopPixels = 4.0; // more movement than this is a drag, not a click
constexpr float kEyeHeight = 1.6f;      // meters
constexpr float kWalkSpeed = 1.5f;      // meters per second
constexpr float kRunSpeed = 4.5f;
// In walk mode the orbit target sits this far ahead, so leaving walk mode orbits around what is in
// front of you.
constexpr float kWalkPivotDistance = 5.0f;
constexpr float kLightPickPixels = 14.0f; // light icons: how close a click must be
constexpr float kWallClearance = 0.3f; // how close walking gets to a wall, meters

} // namespace

ViewportWindow::ViewportWindow(render::Renderer& renderer, QVulkanInstance& vulkan, Scene& scene,
                               Selection& selection, render::SceneSync& sync)
    : renderer_(renderer), scene_(scene), selection_(selection), sync_(sync) {
    setSurfaceType(QSurface::VulkanSurface);
    setVulkanInstance(&vulkan);
    sceneListener_ = scene_.addListener([this](const SceneChange&) { requestUpdate(); });
    selectionListener_ = selection_.addListener([this] {
        hoverHandle_ = GizmoHandle::None;
        requestUpdate(); // the gizmo follows the selection
    });
    walkTimer_ = new QTimer(this);
    walkTimer_->setInterval(16);
    connect(walkTimer_, &QTimer::timeout, this, &ViewportWindow::stepWalk);
    frameScene();
}

ViewportWindow::~ViewportWindow() {
    scene_.removeListener(sceneListener_);
    selection_.removeListener(selectionListener_);
    releaseSkyImage();
}

void ViewportWindow::releaseSkyImage() {
    if (skyImageTexture_.valid()) {
        renderer_.releaseTexture(skyImageTexture_);
    }
    skyImageTexture_ = {};
    skyImage_.reset();
    if (skyImageLoad_.valid()) {
        abandonedSkyLoads_.push_back(std::move(skyImageLoad_));
    }
}

void ViewportWindow::updateSkyImage() {
    const auto ready = [](const auto& future) {
        return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    };
    std::erase_if(abandonedSkyLoads_, ready);

    const std::string& wanted = scene_.environment().sky.image;
    if (wanted != skyImagePath_) {
        releaseSkyImage();
        skyImagePath_ = wanted;
        if (!wanted.empty()) {
            const std::filesystem::path path(std::u8string(wanted.begin(), wanted.end()));
            skyImageLoad_ = std::async(std::launch::async, [path] {
                return std::make_shared<const EnvironmentMap>(loadEnvironmentMap(path));
            });
        }
    }
    if (skyImageLoad_.valid()) {
        if (!ready(skyImageLoad_)) {
            QTimer::singleShot(100, this, [this] { requestUpdate(); }); // check again soon
            return;
        }
        try {
            std::shared_ptr<const EnvironmentMap> map = skyImageLoad_.get();
            skyImageTexture_ = renderer_.uploadEnvironmentMap(*map);
            skyImage_ = std::move(map);
            log::info("Sky image {}: {}x{}, sun {}", skyImagePath_, skyImage_->width,
                      skyImage_->height, skyImage_->hasSun ? "found" : "none (overcast)");
        } catch (const std::exception& e) {
            log::warn("Sky image {}: {}", skyImagePath_, e.what());
            emit skyImageFailed(QString::fromUtf8(e.what()));
        }
    }
}

void ViewportWindow::frameScene() {
    if (const std::optional<Bounds> bounds = scene_.worldBounds()) {
        camera_.frame(*bounds);
    }
    requestUpdate();
}

void ViewportWindow::frameBounds(const Bounds& bounds) {
    camera_.frame(bounds);
    requestUpdate();
}

void ViewportWindow::resetView() {
    camera_.reset();
    frameScene();
}

void ViewportWindow::setGridVisible(bool visible) {
    gridVisible_ = visible;
    requestUpdate();
}

void ViewportWindow::setFocalLength(float millimeters) {
    camera_.setFocalLength(millimeters);
    emit cameraChanged();
    requestUpdate();
}

void ViewportWindow::setTwoPointPerspective(bool enabled) {
    camera_.setTwoPointPerspective(enabled);
    emit cameraChanged();
    requestUpdate();
}

void ViewportWindow::moveToEyeLevel() {
    camera_.moveToEyeLevel();
    requestUpdate();
}

void ViewportWindow::setCameraState(const CameraState& state) {
    camera_.setState(state);
    emit cameraChanged();
    requestUpdate();
}

QSize ViewportWindow::pixelSize() const {
    const qreal ratio = devicePixelRatio();
    return {static_cast<int>(std::lround(width() * ratio)),
            static_cast<int>(std::lround(height() * ratio))};
}

void ViewportWindow::fail(const char* what) {
    failed_ = true;
    presenter_.reset();
    log::error("Viewport stopped: {}", what);
    emit renderingFailed(QString::fromUtf8(what));
}

void ViewportWindow::exposeEvent(QExposeEvent*) {
    if (!isExposed() || failed_) {
        return;
    }
    if (!presenter_) {
        const VkSurfaceKHR surface = QVulkanInstance::surfaceForWindow(this);
        if (surface == VK_NULL_HANDLE) {
            fail("Qt could not create a Vulkan surface for the viewport window");
            return;
        }
        try {
            const QSize size = pixelSize();
            presenter_ = renderer_.createPresenter(surface, static_cast<std::uint32_t>(size.width()),
                                                   static_cast<std::uint32_t>(size.height()));
        } catch (const std::exception& e) {
            fail(e.what());
            return;
        }
    }
    requestUpdate();
}

void ViewportWindow::resizeEvent(QResizeEvent*) {
    if (presenter_) {
        requestUpdate();
    }
}

bool ViewportWindow::event(QEvent* event) {
    switch (event->type()) {
    case QEvent::UpdateRequest:
        renderFrame();
        return true;
    case QEvent::PlatformSurface:
        // The swapchain must go before Qt destroys the VkSurfaceKHR.
        if (static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType() ==
            QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            presenter_.reset();
        }
        break;
    case QEvent::DragEnter:
    case QEvent::DragMove: {
        // QDragEnterEvent derives from QDragMoveEvent; both must be accepted for the drop to land.
        auto* drag = static_cast<QDragMoveEvent*>(event);
        if (drag->mimeData()->hasUrls() &&
            !ImportPanel::importablePaths(drag->mimeData()->urls()).isEmpty()) {
            drag->acceptProposedAction();
        } else {
            drag->ignore();
        }
        return true;
    }
    case QEvent::Drop: {
        auto* drop = static_cast<QDropEvent*>(event);
        const QStringList paths = ImportPanel::importablePaths(drop->mimeData()->urls());
        if (paths.isEmpty()) {
            drop->ignore();
            return true;
        }
        drop->acceptProposedAction();
        dropPoint_ = pointUnder(drop->position());
        emit filesDropped(paths);
        return true;
    }
    default:
        break;
    }
    return QWindow::event(event);
}

void ViewportWindow::renderFrame() {
    if (!presenter_ || !isExposed()) {
        return;
    }
    const QSize size = pixelSize();
    if (size.isEmpty()) {
        return;
    }

    try {
        sync_.update();
        updateSkyImage();
        presenter_->resize(static_cast<std::uint32_t>(size.width()),
                           static_cast<std::uint32_t>(size.height()));
        QElapsedTimer timer;
        timer.start();
        const bool presented = presenter_->render(frameInput(size));
        const double milliseconds = static_cast<double>(timer.nsecsElapsed()) / 1.0e6;
        if (presented) {
            emit frameRendered(milliseconds, size);
        } else {
            requestUpdate(); // swapchain was out of date; try again on the next tick
        }
        // The ray-traced view keeps refining by itself until it reaches its sample target.
        const render::RayTracingStatus status = presenter_->rayTracingStatus();
        if (status.active || lastRayStatus_.active) {
            emit rayTracingProgress(status);
        }
        lastRayStatus_ = status;
        if (status.active && !interacting_) {
            if (status.preparing) {
                QTimer::singleShot(100, this, [this] { requestUpdate(); });
            } else if (status.samples < status.targetSamples) {
                if (rayTracing_.speed == render::RaySpeed::Gentle) {
                    QTimer::singleShot(33, this, [this] { requestUpdate(); }); // ≤ 30 frames/s
                } else {
                    requestUpdate();
                }
            }
        }
    } catch (const std::exception& e) {
        fail(e.what());
    }
}

render::FrameInput ViewportWindow::frameInput(QSize size, bool overlays) const {
    render::FrameInput input;
    input.view = camera_.viewMatrix();
    input.projection =
        camera_.projectionMatrix(static_cast<float>(size.width()) / static_cast<float>(size.height()));
    input.cameraPosition = camera_.position();
    input.draws = sync_.draws();
    input.materials = sync_.materials();
    input.lights = sync_.lights();
    input.sceneVersion = sync_.version();
    input.rayTracing = rayTracing_;
    input.rayTracing.interacting = interacting_;
    if (rayTracing_.enabled) {
        input.rayScene = sync_.rayScene();
    }
    input.showGrid = gridVisible_;
    if (overlays) {
        appendLightIcons(input.overlay);
        if (const std::optional<Gizmo> gizmo = currentGizmo()) {
            gizmo->appendLines(input.overlay, drag_.active() ? drag_.handle() : hoverHandle_,
                               static_cast<float>(devicePixelRatio()));
        }
    }

    const Environment& env = scene_.environment();
    input.lighting.sunDirection = env.sunDirection();
    input.lighting.sunColor =
        kelvinToLinearRgb(env.sun.temperatureKelvin) * std::max(env.sun.intensity, 0.0f);
    input.lighting.skyIntensity = env.sky.intensity;
    input.lighting.shadows = env.shadows.enabled;
    input.lighting.shadowSoftness = env.shadows.softness;
    if (skyImage_) {
        // The sun comes from the image (direction and light), turned with it; Sun strength still
        // scales it, Sky light scales the whole image.
        input.lighting.skyImage = skyImage_;
        input.lighting.skyImageTexture = skyImageTexture_;
        input.lighting.skyImageRotationDegrees = env.sky.imageRotationDegrees;
        input.lighting.sunDirection =
            environmentToWorld(skyImage_->sunDirection, env.sky.imageRotationDegrees);
        // Irradiance E arrives as sunColor × 2π (see kRenderUnitsPerLux).
        input.lighting.sunColor = skyImage_->sunIrradiance *
                                  (std::max(env.sun.intensity, 0.0f) *
                                   std::max(env.sky.intensity, 0.0f) / 6.2831853f);
    }

    const Look& look = scene_.look();
    input.look.exposureEv = look.exposureEv;
    input.look.toneMapping = static_cast<render::ToneMapping>(look.toneMapping);
    input.look.contrast = look.contrast;
    input.look.saturation = look.saturation;
    input.look.warmth = look.warmth;
    input.look.vignette = look.vignette;
    input.look.clay = look.clay;
    input.look.autoExposure = look.autoExposure;
    return input;
}

render::FrameInput ViewportWindow::renderScreenInput(QSize size) {
    sync_.update();
    updateSkyImage();
    render::FrameInput input = frameInput(size, false);
    input.rayScene = sync_.rayScene();
    return input;
}

QImage ViewportWindow::grabFrame() {
    if (!presenter_ || !isExposed()) {
        return {};
    }
    const QSize size = pixelSize();
    if (size.isEmpty()) {
        return {};
    }
    render::CapturedImage captured;
    try {
        sync_.update();
        presenter_->resize(static_cast<std::uint32_t>(size.width()),
                           static_cast<std::uint32_t>(size.height()));
        // A swapchain recreation can swallow one attempt; a few retries cover it.
        for (int attempt = 0; attempt < 3 && captured.pixels.empty(); ++attempt) {
            presenter_->render(frameInput(size, false), &captured); // saved images: no gizmo
        }
    } catch (const std::exception& e) {
        fail(e.what());
        return {};
    }
    if (captured.pixels.empty()) {
        return {};
    }
    const QImage view(captured.pixels.data(), static_cast<int>(captured.width),
                      static_cast<int>(captured.height), static_cast<qsizetype>(captured.width) * 4,
                      captured.bgra ? QImage::Format_RGB32 : QImage::Format_RGBX8888);
    QImage image = view.copy(); // detach from `captured`
    image.setDevicePixelRatio(devicePixelRatio());
    return image;
}

void ViewportWindow::mousePressEvent(QMouseEvent* event) {
    lastMousePosition_ = event->position();
    pressPosition_ = event->position();
    dragged_ = false;
    setInteracting(true);
    if (event->button() == Qt::LeftButton) {
        if (const std::optional<Gizmo> gizmo = currentGizmo()) {
            const GizmoHandle handle =
                gizmo->pick(glm::vec2(event->position().x(), event->position().y()));
            glm::vec3 origin;
            glm::vec3 direction;
            if (handle != GizmoHandle::None && mouseRay(event->position(), origin, direction) &&
                drag_.begin(scene_, selection_.ids(), handle, *gizmo, origin, direction)) {
                dragged_ = true; // not a click: keep the selection
                requestUpdate();
                return;
            }
        }
    }
    if (!walkMode_ && orbitMode_ == OrbitMode::Cursor &&
        (event->buttons() & (Qt::LeftButton | Qt::MiddleButton))) {
        orbitPivot_ = pointUnder(event->position()).value_or(camera_.target());
    }
}

// Bottom center of the selection: objects stand on the ground, so they move and turn from there.
std::optional<Gizmo> ViewportWindow::currentGizmo() const {
    if (!gizmoEnabled_ || walkMode_ || selection_.empty() || width() <= 0 || height() <= 0) {
        return std::nullopt;
    }
    glm::vec3 origin;
    if (drag_.active()) {
        origin = drag_.origin();
    } else if (const std::optional<Bounds> bounds = worldBoundsOf(scene_, selection_.ids())) {
        origin = glm::vec3(bounds->center().x, bounds->center().y, bounds->min.z);
    } else {
        // Lights (no geometry): turn and move them around where they shine from.
        glm::vec3 sum(0.0f);
        int count = 0;
        for (const EntityId id : selection_.ids()) {
            if (const Entity* entity = scene_.entity(id); entity && entity->light) {
                sum += glm::vec3(scene_.worldMatrix(id)[3]);
                ++count;
            }
        }
        if (count == 0) {
            return std::nullopt;
        }
        origin = sum / static_cast<float>(count);
    }
    GizmoView view;
    const float aspect = static_cast<float>(width()) / static_cast<float>(height());
    view.viewProj = camera_.projectionMatrix(aspect) * camera_.viewMatrix();
    view.cameraPosition = camera_.position();
    view.verticalFov = camera_.verticalFov();
    view.viewportSize = glm::vec2(static_cast<float>(width()), static_cast<float>(height()));
    return Gizmo(origin, view);
}

void ViewportWindow::setGizmoEnabled(bool enabled) {
    gizmoEnabled_ = enabled;
    hoverHandle_ = GizmoHandle::None;
    requestUpdate();
}

std::optional<glm::vec3> ViewportWindow::pointUnder(QPointF position) const {
    glm::vec3 origin;
    glm::vec3 direction;
    if (!mouseRay(position, origin, direction)) {
        return std::nullopt;
    }
    if (const std::optional<RayHit> hit = raycast(scene_, origin, direction)) {
        return hit->point;
    }
    // Ground plane, unless it is so far away (near the horizon) that turning around it would fling
    // the camera.
    if (direction.z < -1e-4f) {
        const float t = -origin.z / direction.z;
        if (t < camera_.distance() * 4.0f) {
            return origin + direction * t;
        }
    }
    return std::nullopt;
}

std::optional<glm::vec2> ViewportWindow::takeDropPoint() {
    std::optional<glm::vec2> point;
    if (dropPoint_) {
        point = glm::vec2(dropPoint_->x, dropPoint_->y);
    }
    dropPoint_.reset();
    return point;
}

void ViewportWindow::setRayTracing(const render::RayTracingSettings& settings) {
    rayTracing_ = settings;
    rayTracing_.interacting = false;
    emit rayTracingChanged();
    requestUpdate();
}

void ViewportWindow::setInteracting(bool interacting) {
    if (interacting_ == interacting) {
        return;
    }
    interacting_ = interacting;
    requestUpdate(); // after the last move the ray-traced view picks up again
}

void ViewportWindow::setOrbitMode(OrbitMode mode) {
    orbitMode_ = mode;
}

// Left/middle drag turns the view (around the model's pivot, the point under the cursor, or the
// eye, per OrbitMode); right drag or Shift+drag pans; wheel dollies. Mouse presets of other apps
// (SketchUp, Revit, Rhino, Blender) are roadmap 2.4.
void ViewportWindow::mouseMoveEvent(QMouseEvent* event) {
    const QPointF delta = event->position() - lastMousePosition_;
    lastMousePosition_ = event->position();
    if ((event->position() - pressPosition_).manhattanLength() > kClickSlopPixels) {
        dragged_ = true;
    }

    const Qt::MouseButtons buttons = event->buttons();
    if (drag_.active()) {
        glm::vec3 origin;
        glm::vec3 direction;
        if (mouseRay(event->position(), origin, direction)) {
            drag_.update(origin, direction, (event->modifiers() & Qt::ShiftModifier) != 0);
        }
        return; // the scene change requests the frame
    }
    if (buttons == Qt::NoButton) {
        // Hover: highlight the handle under the cursor.
        const std::optional<Gizmo> gizmo = currentGizmo();
        const GizmoHandle handle =
            gizmo ? gizmo->pick(glm::vec2(event->position().x(), event->position().y()))
                  : GizmoHandle::None;
        if (handle != hoverHandle_) {
            hoverHandle_ = handle;
            if (handle == GizmoHandle::None) {
                unsetCursor();
            } else {
                setCursor(Qt::PointingHandCursor);
            }
            requestUpdate();
        }
        return;
    }
    if (walkMode_) {
        if (buttons & (Qt::LeftButton | Qt::RightButton | Qt::MiddleButton)) {
            // Turn the head: moving the mouse right looks right, up looks up.
            camera_.look(static_cast<float>(-delta.x()) * kLookRadiansPerPixel,
                         static_cast<float>(delta.y()) * kLookRadiansPerPixel);
            requestUpdate();
        }
        return;
    }
    const bool orbitButtons = buttons & (Qt::LeftButton | Qt::MiddleButton);
    const bool pan = (buttons & Qt::RightButton) ||
                     (orbitButtons && (event->modifiers() & Qt::ShiftModifier));
    if (pan) {
        camera_.pan(static_cast<float>(delta.x()), static_cast<float>(delta.y()),
                    static_cast<float>(height()));
    } else if (orbitButtons) {
        const float yaw = static_cast<float>(-delta.x());
        const float pitch = static_cast<float>(delta.y());
        switch (orbitMode_) {
        case OrbitMode::Model:
            camera_.orbit(yaw * kOrbitRadiansPerPixel, pitch * kOrbitRadiansPerPixel);
            break;
        case OrbitMode::Cursor:
            camera_.orbitAround(orbitPivot_, yaw * kOrbitRadiansPerPixel,
                                pitch * kOrbitRadiansPerPixel);
            break;
        case OrbitMode::Look:
            camera_.look(yaw * kLookRadiansPerPixel, pitch * kLookRadiansPerPixel);
            break;
        }
    } else {
        return;
    }
    requestUpdate();
}

void ViewportWindow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->buttons() == Qt::NoButton) {
        setInteracting(!walkKeys_.isEmpty());
    }
    if (drag_.active() && event->button() == Qt::LeftButton) {
        drag_.end();
        requestUpdate();
        return;
    }
    if (dragged_ || event->button() != Qt::LeftButton) {
        return;
    }
    if (walkMode_) {
        standAt(event->position());
        return;
    }
    // Light icons are small: they win over the geometry behind them.
    if (const EntityId light = lightUnder(event->position())) {
        emit entityClicked(light);
        return;
    }
    glm::vec3 origin;
    glm::vec3 direction;
    if (mouseRay(event->position(), origin, direction)) {
        const std::optional<RayHit> hit = raycast(scene_, origin, direction);
        emit entityClicked(hit ? hit->entity : EntityId{});
    }
}

std::optional<glm::vec2> ViewportWindow::toWindow(const glm::vec3& world) const {
    if (width() <= 0 || height() <= 0) {
        return std::nullopt;
    }
    const float aspect = static_cast<float>(width()) / static_cast<float>(height());
    const glm::vec4 clip =
        camera_.projectionMatrix(aspect) * camera_.viewMatrix() * glm::vec4(world, 1.0f);
    if (clip.w <= 1e-4f) {
        return std::nullopt;
    }
    return (glm::vec2(clip) / clip.w * 0.5f + 0.5f) *
           glm::vec2(static_cast<float>(width()), static_cast<float>(height()));
}

EntityId ViewportWindow::lightUnder(QPointF position) const {
    if (!gizmoEnabled_) {
        return {}; // icons are hidden: clicks go to the geometry
    }
    const glm::vec2 mouse(static_cast<float>(position.x()), static_cast<float>(position.y()));
    EntityId best;
    float bestDistance = kLightPickPixels;
    for (const render::LightParams& light : sync_.lights()) {
        if (const std::optional<glm::vec2> s = toWindow(light.position)) {
            const float distance = glm::length(*s - mouse);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = EntityId{light.entity};
            }
        }
    }
    return best;
}

// Lights have no geometry: small screen-sized icons show where they are and which way they shine
// (warm yellow, white when selected), in the modes that edit the scene (like the gizmo); Render
// mode keeps the view clean. Saved images leave them out.
void ViewportWindow::appendLightIcons(std::vector<OverlayLine>& out) const {
    if (!gizmoEnabled_ || sync_.lights().empty() || width() <= 0 || height() <= 0) {
        return;
    }
    const float scale = static_cast<float>(devicePixelRatio());
    const glm::mat4 view = camera_.viewMatrix();
    const glm::vec3 right(view[0][0], view[1][0], view[2][0]);
    const glm::vec3 up(view[0][1], view[1][1], view[2][1]);
    const float pixelsToMeters =
        2.0f * std::tan(camera_.verticalFov() * 0.5f) / static_cast<float>(height());
    constexpr int kSegments = 16;
    constexpr float kTwoPi = 6.2831853f;
    for (const render::LightParams& light : sync_.lights()) {
        const bool selected = selection_.contains(EntityId{light.entity});
        const glm::vec4 color = selected ? glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)
                                         : glm::vec4(1.0f, 0.82f, 0.35f, 0.95f);
        const float width = (selected ? 2.5f : 1.8f) * scale;
        const float pixel =
            std::max(glm::length(light.position - camera_.position()), 0.01f) * pixelsToMeters;
        const auto line = [&](const glm::vec3& a, const glm::vec3& b) {
            out.push_back({a, width + 2.0f * scale, b, width + 2.0f * scale,
                           glm::vec4(0.0f, 0.0f, 0.0f, 0.35f)});
            out.push_back({a, width, b, width, color});
        };
        const auto circle = [&](const glm::vec3& center, const glm::vec3& u, const glm::vec3& v,
                                float radius) {
            for (int i = 0; i < kSegments; ++i) {
                const float a0 = kTwoPi * static_cast<float>(i) / kSegments;
                const float a1 = kTwoPi * static_cast<float>(i + 1) / kSegments;
                line(center + (u * std::cos(a0) + v * std::sin(a0)) * radius,
                     center + (u * std::cos(a1) + v * std::sin(a1)) * radius);
            }
        };
        const glm::vec3 p = light.position;
        switch (light.kind) {
        case render::LightKind::Point:
            circle(p, right, up, 8.0f * pixel);
            for (int i = 0; i < 4; ++i) {
                const float a = kTwoPi * (static_cast<float>(i) + 0.5f) / 4.0f;
                const glm::vec3 d = right * std::cos(a) + up * std::sin(a);
                line(p + d * (11.0f * pixel), p + d * (16.0f * pixel));
            }
            break;
        case render::LightKind::Spot: {
            circle(p, right, up, 5.0f * pixel);
            const glm::vec3 axis = light.direction;
            const glm::vec3 side = glm::normalize(
                std::abs(axis.z) < 0.9f ? glm::cross(axis, glm::vec3(0.0f, 0.0f, 1.0f))
                                        : glm::cross(axis, glm::vec3(1.0f, 0.0f, 0.0f)));
            const glm::vec3 other = glm::cross(axis, side);
            const float length = 30.0f * pixel;
            const float sine = std::sqrt(std::max(1.0f - light.cosOuter * light.cosOuter, 0.0f));
            const float radius = length * std::min(sine / std::max(light.cosOuter, 0.2f), 1.5f);
            const glm::vec3 end = p + axis * length;
            circle(end, side, other, radius);
            for (int i = 0; i < 4; ++i) {
                const float a = kTwoPi * static_cast<float>(i) / 4.0f;
                line(p, end + (side * std::cos(a) + other * std::sin(a)) * radius);
            }
            break;
        }
        case render::LightKind::Area: {
            const glm::vec3 x = light.axisX * light.halfSize.x;
            const glm::vec3 y = light.axisY * light.halfSize.y;
            line(p - x - y, p + x - y);
            line(p + x - y, p + x + y);
            line(p + x + y, p - x + y);
            line(p - x + y, p - x - y);
            line(p, p + light.direction * (24.0f * pixel));
            break;
        }
        }
    }
}

bool ViewportWindow::mouseRay(QPointF position, glm::vec3& origin, glm::vec3& direction) const {
    if (width() <= 0 || height() <= 0) {
        return false;
    }
    const float aspect = static_cast<float>(width()) / static_cast<float>(height());
    const glm::mat4 inverse =
        glm::inverse(camera_.projectionMatrix(aspect) * camera_.viewMatrix());
    // Vulkan NDC: y points down, so the window's y maps directly. Reverse-Z: depth 1 is the near
    // plane, 0.5 lies twice as far along the same ray.
    const float x = static_cast<float>(position.x() / width()) * 2.0f - 1.0f;
    const float y = static_cast<float>(position.y() / height()) * 2.0f - 1.0f;
    const glm::vec4 nearPoint = inverse * glm::vec4(x, y, 1.0f, 1.0f);
    const glm::vec4 farther = inverse * glm::vec4(x, y, 0.5f, 1.0f);
    origin = glm::vec3(nearPoint) / nearPoint.w;
    direction = glm::normalize(glm::vec3(farther) / farther.w - origin);
    return true;
}

// Click to walk there: on a floor, roof or the ground you stand on the spot; on a wall you stand
// 1.5 m in front of it, on whatever floor is below, looking at it. Eye height 1.6 m.
void ViewportWindow::standAt(QPointF position) {
    glm::vec3 origin;
    glm::vec3 direction;
    if (!mouseRay(position, origin, direction)) {
        return;
    }
    glm::vec3 point;
    glm::vec3 normal;
    if (const std::optional<RayHit> hit = raycast(scene_, origin, direction)) {
        point = hit->point;
        normal = hit->normal;
    } else if (direction.z < -1e-4f) {
        point = origin + direction * (-origin.z / direction.z); // the ground plane
        normal = {0.0f, 0.0f, 1.0f};
    } else {
        emit hint(tr("Click on the ground, a floor or a wall to stand there."));
        return;
    }

    const glm::vec3 eyeNow = camera_.position();
    glm::vec3 eye;
    glm::vec3 facing;
    if (normal.z > 0.6f) {
        eye = point + glm::vec3(0.0f, 0.0f, kEyeHeight);
        facing = point - eyeNow; // keep looking the way you went
    } else {
        glm::vec3 out(normal.x, normal.y, 0.0f);
        out = glm::length(out) > 1e-4f ? glm::normalize(out) : -glm::normalize(direction);
        const glm::vec3 spot = point + out * 1.5f;
        float floor = 0.0f;
        if (const std::optional<RayHit> below =
                raycast(scene_, spot + glm::vec3(0.0f, 0.0f, 0.5f), {0.0f, 0.0f, -1.0f})) {
            floor = below->point.z;
        }
        eye = glm::vec3(spot.x, spot.y, floor + kEyeHeight);
        facing = -out;
    }
    facing.z = 0.0f;
    if (glm::length(facing) < 1e-3f) {
        facing = camera_.target() - eyeNow;
        facing.z = 0.0f;
    }
    if (glm::length(facing) < 1e-3f) {
        facing = {0.0f, 1.0f, 0.0f};
    }
    facing = glm::normalize(facing);

    // The camera looks from position() = target + offset · distance toward the target.
    CameraState state = camera_.state();
    state.yawRadians = std::atan2(-facing.y, -facing.x);
    state.pitchRadians = 0.0f;
    state.distance = kWalkPivotDistance;
    state.target = eye + facing * kWalkPivotDistance;
    camera_.setState(state);
    requestUpdate();
}

void ViewportWindow::setWalkMode(bool on) {
    if (walkMode_ == on) {
        return;
    }
    walkMode_ = on;
    walkKeys_.clear();
    walkTimer_->stop();
    if (on) {
        setCursor(Qt::CrossCursor);
        requestActivate(); // keys go to the viewport right away
        emit hint(tr("Walk: click where you want to stand. W A S D or arrow keys move, drag to "
                     "look around, Shift runs, Q/E go down/up, Esc stops."));
    } else {
        unsetCursor();
        emit hint(QString());
    }
    emit walkModeChanged(on);
}

void ViewportWindow::keyPressEvent(QKeyEvent* event) {
    if (!walkMode_) {
        QWindow::keyPressEvent(event);
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        setWalkMode(false);
        return;
    }
    switch (event->key()) {
    case Qt::Key_W: case Qt::Key_A: case Qt::Key_S: case Qt::Key_D:
    case Qt::Key_Up: case Qt::Key_Down: case Qt::Key_Left: case Qt::Key_Right:
    case Qt::Key_Q: case Qt::Key_E:
        if (!event->isAutoRepeat()) {
            walkKeys_.insert(event->key());
            if (!walkTimer_->isActive()) {
                walkClock_.restart();
                walkTimer_->start();
            }
        }
        event->accept();
        return;
    default:
        QWindow::keyPressEvent(event);
    }
}

void ViewportWindow::keyReleaseEvent(QKeyEvent* event) {
    if (walkMode_ && !event->isAutoRepeat() && walkKeys_.remove(event->key())) {
        event->accept();
        return;
    }
    QWindow::keyReleaseEvent(event);
}

void ViewportWindow::focusOutEvent(QFocusEvent* event) {
    walkKeys_.clear(); // a key released while another window had focus would stick otherwise
    walkTimer_->stop();
    QWindow::focusOutEvent(event);
}

void ViewportWindow::stepWalk() {
    if (walkKeys_.isEmpty()) {
        walkTimer_->stop();
        setInteracting(QGuiApplication::mouseButtons() != Qt::NoButton);
        return;
    }
    setInteracting(true);
    const float seconds =
        std::min(static_cast<float>(walkClock_.restart()) / 1000.0f, 0.1f); // no jumps after stalls
    const bool run = (QGuiApplication::queryKeyboardModifiers() & Qt::ShiftModifier) != 0;
    const float step = (run ? kRunSpeed : kWalkSpeed) * seconds;
    const auto held = [this](int a, int b) { return walkKeys_.contains(a) || walkKeys_.contains(b); };
    const float forward = (held(Qt::Key_W, Qt::Key_Up) ? 1.0f : 0.0f) -
                          (held(Qt::Key_S, Qt::Key_Down) ? 1.0f : 0.0f);
    const float right = (held(Qt::Key_D, Qt::Key_Right) ? 1.0f : 0.0f) -
                        (held(Qt::Key_A, Qt::Key_Left) ? 1.0f : 0.0f);
    const float up = (walkKeys_.contains(Qt::Key_E) ? 1.0f : 0.0f) -
                     (walkKeys_.contains(Qt::Key_Q) ? 1.0f : 0.0f);
    walkBy(forward * step, right * step, up * step);
    requestUpdate();
}

void ViewportWindow::walkBy(float forwardMeters, float rightMeters, float upMeters) {
    const float yaw = camera_.state().yawRadians;
    const glm::vec3 forward{-std::cos(yaw), -std::sin(yaw), 0.0f};
    const glm::vec3 right = glm::cross(forward, glm::vec3(0.0f, 0.0f, 1.0f));
    glm::vec3 move = forward * forwardMeters + right * rightMeters +
                     glm::vec3(0.0f, 0.0f, upMeters);
    // Rays at eye and waist height; a hit within reach removes the part of the move that goes into
    // the surface, so you slide along walls (two passes handle corners).
    const glm::vec3 eye = camera_.position();
    for (int pass = 0; pass < 2; ++pass) {
        const float length = glm::length(move);
        if (length < 1e-6f) {
            break;
        }
        const glm::vec3 direction = move / length;
        std::optional<RayHit> nearest;
        for (const float drop : {0.0f, 0.8f}) {
            const std::optional<RayHit> hit =
                raycast(scene_, eye - glm::vec3(0.0f, 0.0f, drop), direction,
                        length + kWallClearance);
            if (hit && (!nearest || hit->distance < nearest->distance)) {
                nearest = hit;
            }
        }
        if (!nearest) {
            break;
        }
        move -= nearest->normal * glm::dot(move, nearest->normal);
    }
    camera_.walk(glm::dot(move, forward), glm::dot(move, right), move.z);
}

void ViewportWindow::wheelEvent(QWheelEvent* event) {
    const float notches = static_cast<float>(event->angleDelta().y()) / 120.0f;
    if (walkMode_) {
        walkBy(notches * 0.5f, 0.0f, 0.0f); // half a step per notch
    } else {
        camera_.dolly(notches);
    }
    requestUpdate();
}

} // namespace lb::app
