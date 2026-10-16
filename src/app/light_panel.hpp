#pragma once

#include <functional>

#include <QWidget>

#include "app/property_widgets.hpp"
#include "core/scene/scene.hpp"

class QCheckBox;
class QComboBox;
class QDateEdit;
class QDoubleSpinBox;
class QLabel;
class QSlider;
class QTimeEdit;

namespace lb::app {

// Light mode panel: time-of-day presets, then the sun (manual direction, or real position from site
// location, date and time), light strength and shadows. Edits the scene Environment, which the
// viewport reads every frame. Sky colors stay automatic; Environments (5.x) will own them.
class LightPanel : public QWidget {
    Q_OBJECT

public:
    explicit LightPanel(Scene& scene, QWidget* parent = nullptr);
    ~LightPanel() override;

private:
    void refresh();
    void edit(const std::function<void(Environment&)>& change);
    void applyPreset(int index);

    Scene& scene_;
    Scene::ListenerId listener_ = 0;
    bool updating_ = false;

    ui::SegmentedControl* mode_ = nullptr;
    ui::PropertyRow azimuthRow_;
    ui::PropertyRow elevationRow_;
    ui::PropertyRow dateRow_;
    ui::PropertyRow timeRow_;
    ui::SliderField* azimuth_ = nullptr;
    ui::SliderField* elevation_ = nullptr;
    QDateEdit* date_ = nullptr;
    QSlider* timeSlider_ = nullptr;
    QTimeEdit* time_ = nullptr;
    QLabel* readout_ = nullptr;

    ui::Section* location_ = nullptr;
    QComboBox* city_ = nullptr;
    QDoubleSpinBox* latitude_ = nullptr;
    QDoubleSpinBox* longitude_ = nullptr;
    QDoubleSpinBox* utcOffset_ = nullptr;
    ui::SliderField* north_ = nullptr;

    ui::SliderField* intensity_ = nullptr;
    ui::SliderField* temperature_ = nullptr;
    ui::SliderField* skyLight_ = nullptr;
    QCheckBox* shadows_ = nullptr;
    ui::SliderField* softness_ = nullptr;
};

} // namespace lb::app
