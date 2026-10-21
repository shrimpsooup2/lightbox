#pragma once

#include <functional>

#include <QImage>
#include <QWidget>

#include "app/property_widgets.hpp"
#include "core/scene/scene.hpp"
#include "render/types.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;

namespace lb::app {

class ViewportWindow;

// One line describing the ray-traced view's progress ("Refining: 37 of 256 samples · 0:42").
QString rayStatusText(const render::RayTracingStatus& status);

// Render mode panel: the way to the render screen (final images), look presets, the ray-traced
// view, camera lens and framing, image adjustments (exposure, tone mapping, color) and saving what
// the viewport shows. Views, the render queue and the Final (Cycles) engine join here later.
class RenderPanel : public QWidget {
    Q_OBJECT

public:
    RenderPanel(Scene& scene, ViewportWindow& viewport, QWidget* parent = nullptr);
    ~RenderPanel() override;

signals:
    // "Render image…": open the render screen.
    void renderScreenRequested();

private:
    void refreshLook();
    void refreshCamera();
    void refreshRayTracing();
    void editLook(const std::function<void(Look&)>& change);
    void saveImage();
    void copyImage();
    QImage capture();

    Scene& scene_;
    ViewportWindow& viewport_;
    Scene::ListenerId listener_ = 0;
    bool updating_ = false;

    ui::SliderField* lens_ = nullptr;
    QCheckBox* twoPoint_ = nullptr;

    ui::SliderField* exposure_ = nullptr;
    QComboBox* toneMapping_ = nullptr;
    ui::SliderField* contrast_ = nullptr;
    ui::SliderField* saturation_ = nullptr;
    ui::SliderField* warmth_ = nullptr;
    ui::SliderField* vignette_ = nullptr;
    QCheckBox* clay_ = nullptr;
    QCheckBox* autoExposure_ = nullptr;

    QCheckBox* rayTraced_ = nullptr;
    ui::SegmentedControl* rayQuality_ = nullptr;
    ui::SegmentedControl* raySpeed_ = nullptr;
    QLabel* rayStatus_ = nullptr;
    QProgressBar* rayProgress_ = nullptr;

    QLabel* status_ = nullptr;
};

} // namespace lb::app
