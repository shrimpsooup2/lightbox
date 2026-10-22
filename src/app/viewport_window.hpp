#pragma once

#include <memory>
#include <future>
#include <optional>
#include <string>
#include <vector>

#include <QElapsedTimer>
#include <QImage>
#include <QPointF>
#include <QSet>
#include <QSize>
#include <QStringList>
#include <QWindow>

#include "core/environment_map.hpp"
#include "core/camera.hpp"
#include "core/gizmo.hpp"
#include "core/scene/scene.hpp"
#include "core/scene/selection.hpp"
#include "render/renderer.hpp"
#include "render/scene_sync.hpp"

class QTimer;
class QVulkanInstance;

namespace lb::app {

// What a left/middle drag turns around.
enum class OrbitMode {
    Model,  // the orbit target (center of the model after Frame)
    Cursor, // the point under the cursor when the drag starts
    Look,   // the eye: the camera stays put and looks around
};

// Native Vulkan window hosting one Live-engine viewport of a Scene. Embedded in the widget tree with
// QWidget::createWindowContainer. Renders on demand (input, resize, expose, document changes), not
// continuously. `scene` and `sync` must outlive the window.
class ViewportWindow : public QWindow {
    Q_OBJECT

public:
    ViewportWindow(render::Renderer& renderer, QVulkanInstance& vulkan, Scene& scene,
                   Selection& selection, render::SceneSync& sync);
    ~ViewportWindow() override;

    void frameScene();
    // Fits a world-space box into view, keeping the current orientation.
    void frameBounds(const Bounds& bounds);
    void resetView();
    void setGridVisible(bool visible);
    bool gridVisible() const { return gridVisible_; }

    // Camera lens and framing (Render mode ▸ Camera).
    const OrbitCamera& camera() const { return camera_; }
    void setFocalLength(float millimeters);
    void setTwoPointPerspective(bool enabled);
    void moveToEyeLevel();
    // Whole camera (orbit + lens), saved with projects. Setting it emits cameraChanged().
    CameraState cameraState() const { return camera_.state(); }
    void setCameraState(const CameraState& state);

    // Walkthrough: click where to stand (eye height 1.6 m), W A S D / arrows to move, drag to look,
    // Shift to run, Q/E down/up, Esc to stop. Outside walk mode a click selects the object under
    // the cursor (entityClicked).
    void setWalkMode(bool on);
    bool walkMode() const { return walkMode_; }
    void setOrbitMode(OrbitMode mode);
    OrbitMode orbitMode() const { return orbitMode_; }
    // Move/rotate handles on the selection (Import and Build modes).
    void setGizmoEnabled(bool enabled);
    // Outline around the selected objects (every mode but Render, which keeps the view clean).
    void setSelectionOutline(bool enabled);
    // Ray-traced view (on/off, quality, speed). Refines by itself after each change.
    void setRayTracing(const render::RayTracingSettings& settings);
    const render::RayTracingSettings& rayTracing() const { return rayTracing_; }
    // Ground position under the last drop of files on the viewport, once (then cleared).
    std::optional<glm::vec2> takeDropPoint();

    // Renders a fresh frame and reads it back (blocking). Null if the viewport cannot render now.
    QImage grabFrame();
    // Frame input of the current view for an offscreen render at `size` (the render screen): no
    // overlays, ray scene always prepared. Brings the scene mirror and the sky image up to date.
    render::FrameInput renderScreenInput(QSize size);
    // The scene's sky image is still being loaded (renders show the automatic sky until then).
    bool skyImageLoading() const { return skyImageLoad_.valid(); }

signals:
    // CPU time spent in Presenter::render (includes waiting for a free frame slot).
    void frameRendered(double milliseconds, QSize pixelSize);
    // Lens or two-point setting changed (by code or by the user).
    void cameraChanged();
    // The Live engine hit an unrecoverable error; the viewport stops rendering.
    void renderingFailed(const QString& message);
    // Local model or image files were dropped on the viewport (file paths).
    void filesDropped(const QStringList& paths);
    void walkModeChanged(bool on);
    // A click without dragging, outside walk mode: the mesh object or light under the cursor
    // (none = empty space); `add` when Shift or Ctrl was held (toggle it in the selection).
    void entityClicked(lb::EntityId entity, bool add);
    // A short how-to for the status bar.
    void hint(const QString& text);
    void rayTracingChanged();
    // After every frame while the ray-traced view is on (and once when it turns off).
    void rayTracingProgress(const lb::render::RayTracingStatus& status);
    // The scene's sky image could not be loaded (plain-language reason); the procedural sky stays.
    void skyImageFailed(const QString& message);

protected:
    void exposeEvent(QExposeEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    void renderFrame();
    // Follows Environment::sky.image: loads it on a worker thread, uploads it once ready.
    void updateSkyImage();
    void releaseSkyImage();
    render::FrameInput frameInput(QSize size, bool overlays = true) const;
    // The gizmo for the current selection in this view, when shown.
    std::optional<Gizmo> currentGizmo() const;
    void fail(const char* what);
    QSize pixelSize() const;
    // World-space ray through a point of the window (logical pixels): origin on the near plane.
    bool mouseRay(QPointF position, glm::vec3& origin, glm::vec3& direction) const;
    // The surface (or ground) point under a window position, if any.
    std::optional<glm::vec3> pointUnder(QPointF position) const;
    // Window position (logical pixels) of a world point; nullopt behind the camera.
    std::optional<glm::vec2> toWindow(const glm::vec3& world) const;
    // The light whose icon is under a window position, if any.
    EntityId lightUnder(QPointF position) const;
    void appendLightIcons(std::vector<OverlayLine>& out) const;
    void standAt(QPointF position);
    void stepWalk();
    // Moves the walking eye, stopping at walls and sliding along them.
    void walkBy(float forwardMeters, float rightMeters, float upMeters);
    // While the camera or objects move the raster view shows; ray tracing restarts afterwards.
    void setInteracting(bool interacting);

    render::Renderer& renderer_;
    Scene& scene_;
    Selection& selection_;
    render::SceneSync& sync_;
    Scene::ListenerId sceneListener_ = 0;
    Selection::ListenerId selectionListener_ = 0;
    GizmoDrag drag_;
    GizmoHandle hoverHandle_ = GizmoHandle::None;
    bool gizmoEnabled_ = false;
    bool outlineEnabled_ = true;
    // Entity ids of the selected mesh objects (groups expanded), sorted: the outline's input.
    // Updated when the selection or the scene changes, not per frame.
    std::vector<std::uint64_t> highlighted_;
    bool highlightDirty_ = true;
    render::RayTracingSettings rayTracing_;
    std::string skyImagePath_; // what skyImage_/skyImageLoad_ belong to
    std::future<std::shared_ptr<const EnvironmentMap>> skyImageLoad_;
    // Loads of images no longer wanted, kept until they finish (a std::async future blocks when
    // destroyed early).
    std::vector<std::future<std::shared_ptr<const EnvironmentMap>>> abandonedSkyLoads_;
    std::shared_ptr<const EnvironmentMap> skyImage_;
    render::TextureHandle skyImageTexture_;
    render::RayTracingStatus lastRayStatus_;
    bool interacting_ = false;
    std::unique_ptr<render::Presenter> presenter_;
    OrbitCamera camera_;
    QPointF lastMousePosition_;
    QPointF pressPosition_;
    bool dragged_ = false;
    bool walkMode_ = false;
    OrbitMode orbitMode_ = OrbitMode::Model;
    glm::vec3 orbitPivot_{0.0f};
    std::optional<glm::vec3> dropPoint_;
    QSet<int> walkKeys_;
    QTimer* walkTimer_ = nullptr;
    QElapsedTimer walkClock_;
    bool gridVisible_ = true;
    bool failed_ = false;
};

} // namespace lb::app
