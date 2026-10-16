#pragma once

#include <functional>

#include <QImage>
#include <QWidget>

#include "app/property_widgets.hpp"
#include "core/scene/scene.hpp"

class QCheckBox;
class QComboBox;
class QLabel;

namespace lb::app {

class ViewportWindow;

// Render mode panel: look presets, camera lens and framing, image adjustments (exposure, tone
// mapping, color) and saving what the viewport shows. Views, the render queue and the Final (Cycles)
// engine join here in later stages.
class RenderPanel : public QWidget {
    Q_OBJECT

public:
    RenderPanel(Scene& scene, ViewportWindow& viewport, QWidget* parent = nullptr);
    ~RenderPanel() override;

private:
    void refreshLook();
    void refreshCamera();
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

    QLabel* status_ = nullptr;
};

} // namespace lb::app
