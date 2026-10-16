#pragma once

#include <memory>

#include <QImage>
#include <QPointF>
#include <QSize>
#include <QStringList>
#include <QWindow>

#include "core/camera.hpp"
#include "core/scene/scene.hpp"
#include "render/renderer.hpp"
#include "render/scene_sync.hpp"

class QVulkanInstance;

namespace lb::app {

// Native Vulkan window hosting one Live-engine viewport of a Scene. Embedded in the widget tree with
// QWidget::createWindowContainer. Renders on demand (input, resize, expose, document changes), not
// continuously. `scene` and `sync` must outlive the window.
class ViewportWindow : public QWindow {
    Q_OBJECT

public:
    ViewportWindow(render::Renderer& renderer, QVulkanInstance& vulkan, Scene& scene,
                   render::SceneSync& sync);
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

    // Renders a fresh frame and reads it back (blocking). Null if the viewport cannot render now.
    QImage grabFrame();

signals:
    // CPU time spent in Presenter::render (includes waiting for a free frame slot).
    void frameRendered(double milliseconds, QSize pixelSize);
    // Lens or two-point setting changed (by code or by the user).
    void cameraChanged();
    // The Live engine hit an unrecoverable error; the viewport stops rendering.
    void renderingFailed(const QString& message);
    // Local model or image files were dropped on the viewport (file paths).
    void filesDropped(const QStringList& paths);

protected:
    void exposeEvent(QExposeEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void renderFrame();
    render::FrameInput frameInput(QSize size) const;
    void fail(const char* what);
    QSize pixelSize() const;

    render::Renderer& renderer_;
    Scene& scene_;
    render::SceneSync& sync_;
    Scene::ListenerId sceneListener_ = 0;
    std::unique_ptr<render::Presenter> presenter_;
    OrbitCamera camera_;
    QPointF lastMousePosition_;
    bool gridVisible_ = true;
    bool failed_ = false;
};

} // namespace lb::app
