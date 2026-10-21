#pragma once

#include <functional>

#include <QWidget>

#include "app/property_widgets.hpp"
#include "core/scene/scene.hpp"
#include "core/scene/selection.hpp"

class QCheckBox;
class QComboBox;
class QDateEdit;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSlider;
class QTimeEdit;

namespace lb::app {

class ViewportWindow;

// Light mode panel: time-of-day presets, artificial lights (add bulbs, spots and panels; edit the
// selected one), the sky (automatic, or a sky image whose sun takes over), the sun (manual
// direction, or real position from site location, date and time), light strength and shadows.
class LightPanel : public QWidget {
    Q_OBJECT

public:
    LightPanel(Scene& scene, Selection& selection, ViewportWindow& viewport,
               QWidget* parent = nullptr);
    ~LightPanel() override;

private:
    void refresh();
    void edit(const std::function<void(Environment&)>& change);
    void applyPreset(int index);
    void addLight(LightType type);
    void refreshLight();
    void editLight(const std::function<void(Light&)>& change);
    EntityId selectedLight() const;

    Scene& scene_;
    Selection& selection_;
    ViewportWindow& viewport_;
    Scene::ListenerId listener_ = 0;
    Selection::ListenerId selectionListener_ = 0;
    bool updating_ = false;

    QPushButton* skyImageButton_ = nullptr;
    QPushButton* skyClearButton_ = nullptr;
    ui::PropertyRow skyRotationRow_;
    ui::SliderField* skyRotation_ = nullptr;
    QLabel* sunFollowsSky_ = nullptr;

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

    // The selected artificial light.
    QLabel* lightHint_ = nullptr;
    QWidget* lightTypeRow_ = nullptr;
    ui::SegmentedControl* lightType_ = nullptr;
    ui::PropertyRow lumensRow_;
    ui::SliderField* lumens_ = nullptr;
    ui::PropertyRow temperatureRow_;
    ui::SliderField* lightTemperature_ = nullptr;
    ui::PropertyRow radiusRow_;
    ui::SliderField* radius_ = nullptr;
    ui::PropertyRow widthRow_;
    ui::SliderField* width_ = nullptr;
    ui::PropertyRow heightRow_;
    ui::SliderField* height_ = nullptr;
    ui::PropertyRow angleRow_;
    ui::SliderField* spotAngle_ = nullptr;
    ui::PropertyRow softnessRow_;
    ui::SliderField* spotSoftness_ = nullptr;
};

} // namespace lb::app
