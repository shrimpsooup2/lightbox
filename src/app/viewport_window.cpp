#include "app/viewport_window.hpp"

#include <algorithm>
#include <cmath>
#include <exception>

#include <QDropEvent>
#include <QElapsedTimer>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>
#include <QVulkanInstance>
#include <QWheelEvent>

#include "app/import_panel.hpp"
#include "core/log.hpp"
#include "core/solar.hpp"

namespace lb::app {
namespace {

constexpr float kOrbitRadiansPerPixel = 0.006f;

} // namespace

ViewportWindow::ViewportWindow(render::Renderer& renderer, QVulkanInstance& vulkan, Scene& scene,
                               render::SceneSync& sync)
    : renderer_(renderer), scene_(scene), sync_(sync) {
    setSurfaceType(QSurface::VulkanSurface);
    setVulkanInstance(&vulkan);
    sceneListener_ = scene_.addListener([this](const SceneChange&) { requestUpdate(); });
    frameScene();
}

ViewportWindow::~ViewportWindow() {
    scene_.removeListener(sceneListener_);
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
    } catch (const std::exception& e) {
        fail(e.what());
    }
}

render::FrameInput ViewportWindow::frameInput(QSize size) const {
    render::FrameInput input;
    input.view = camera_.viewMatrix();
    input.projection =
        camera_.projectionMatrix(static_cast<float>(size.width()) / static_cast<float>(size.height()));
    input.cameraPosition = camera_.position();
    input.draws = sync_.draws();
    input.materials = sync_.materials();
    input.showGrid = gridVisible_;

    const Environment& env = scene_.environment();
    input.lighting.sunDirection = env.sunDirection();
    input.lighting.sunColor =
        kelvinToLinearRgb(env.sun.temperatureKelvin) * std::max(env.sun.intensity, 0.0f);
    input.lighting.skyIntensity = env.sky.intensity;
    input.lighting.shadows = env.shadows.enabled;
    input.lighting.shadowSoftness = env.shadows.softness;

    const Look& look = scene_.look();
    input.look.exposureEv = look.exposureEv;
    input.look.toneMapping = static_cast<render::ToneMapping>(look.toneMapping);
    input.look.contrast = look.contrast;
    input.look.saturation = look.saturation;
    input.look.warmth = look.warmth;
    input.look.vignette = look.vignette;
    input.look.clay = look.clay;
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
            presenter_->render(frameInput(size), &captured);
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
}

// Stage 0 navigation: left/middle drag orbits, right drag or Shift+drag pans, wheel dollies.
// Stage 2.4 adds selectable navigation presets (SketchUp, Revit, Rhino, Blender).
void ViewportWindow::mouseMoveEvent(QMouseEvent* event) {
    const QPointF delta = event->position() - lastMousePosition_;
    lastMousePosition_ = event->position();

    const Qt::MouseButtons buttons = event->buttons();
    const bool orbitButtons = buttons & (Qt::LeftButton | Qt::MiddleButton);
    const bool pan = (buttons & Qt::RightButton) ||
                     (orbitButtons && (event->modifiers() & Qt::ShiftModifier));
    if (pan) {
        camera_.pan(static_cast<float>(delta.x()), static_cast<float>(delta.y()),
                    static_cast<float>(height()));
    } else if (orbitButtons) {
        camera_.orbit(static_cast<float>(-delta.x()) * kOrbitRadiansPerPixel,
                      static_cast<float>(delta.y()) * kOrbitRadiansPerPixel);
    } else {
        return;
    }
    requestUpdate();
}

void ViewportWindow::wheelEvent(QWheelEvent* event) {
    camera_.dolly(static_cast<float>(event->angleDelta().y()) / 120.0f);
    requestUpdate();
}

} // namespace lb::app
