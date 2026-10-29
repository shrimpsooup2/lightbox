#pragma once

#include <QWidget>

#include "app/property_widgets.hpp"
#include "core/scene/scene.hpp"
#include "core/scene/selection.hpp"

class QCheckBox;
class QLabel;
class QPushButton;
class QTimer;

namespace lb::app {

// Site mode panel (ROADMAP 5.1): ground around the building. "Add ground" makes rolling hills that
// are flat under the building and blend out around it (core/terrain.*); the sliders reshape it live
// and *New shape* tries another landscape. Paths, pads, planting and water join here later.
class SitePanel : public QWidget {
    Q_OBJECT

public:
    SitePanel(Scene& scene, Selection& selection, QWidget* parent = nullptr);
    ~SitePanel() override;

private:
    // Fills the sliders from the content's size (first time) and makes the ground.
    void addGround();
    // Rebuilds the existing ground from the sliders (coalesced while a slider is dragged).
    void rebuild();
    void removeGround();
    void updateControls();
    ResourceId groundMaterial();

    Scene& scene_;
    Selection& selection_;
    Scene::ListenerId listener_ = 0;
    QTimer* rebuildTimer_ = nullptr;
    std::uint32_t seed_ = 1;
    bool updating_ = false;

    QLabel* hint_ = nullptr;
    QPushButton* add_ = nullptr;
    QPushButton* remove_ = nullptr;
    ui::Section* shape_ = nullptr;
    ui::SliderField* size_ = nullptr;
    ui::SliderField* hillHeight_ = nullptr;
    ui::SliderField* hillSize_ = nullptr;
    QPushButton* newShape_ = nullptr;
    QCheckBox* flat_ = nullptr;
    ui::SliderField* blend_ = nullptr;
};

} // namespace lb::app
