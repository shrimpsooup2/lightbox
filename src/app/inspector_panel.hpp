#pragma once

#include <array>

#include <QWidget>

#include "core/scene/scene.hpp"
#include "core/scene/selection.hpp"

class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QStackedWidget;
class QToolButton;

namespace lb::app {

// "Properties" panel: edits the primary selected entity (name, transform, material color) and shows
// mesh information. Hand-built for Stage 0; the reflected property system (2.6) will generate it.
class InspectorPanel : public QWidget {
    Q_OBJECT

public:
    InspectorPanel(Scene& scene, Selection& selection, QWidget* parent = nullptr);
    ~InspectorPanel() override;

private:
    void showEntity(EntityId id);
    void refresh();
    void applyName();
    void applyTransform();
    void pickBaseColor();
    ResourceId currentMaterial() const;

    Scene& scene_;
    Selection& selection_;
    Scene::ListenerId sceneListener_ = 0;
    Selection::ListenerId selectionListener_ = 0;
    EntityId current_;
    bool updating_ = false;

    QStackedWidget* stack_ = nullptr;
    QLineEdit* name_ = nullptr;
    QLabel* kind_ = nullptr;
    std::array<QDoubleSpinBox*, 3> position_{};
    std::array<QDoubleSpinBox*, 3> rotation_{};
    std::array<QDoubleSpinBox*, 3> scale_{};
    QWidget* materialSection_ = nullptr;
    QToolButton* swatch_ = nullptr;
    QLabel* materialName_ = nullptr;
    QLabel* materialUsage_ = nullptr;
    QWidget* geometrySection_ = nullptr;
    QLabel* geometryInfo_ = nullptr;
};

} // namespace lb::app
