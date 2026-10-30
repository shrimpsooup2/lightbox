#pragma once

#include <array>
#include <optional>
#include <vector>

#include <QElapsedTimer>
#include <QWidget>

#include "app/property_widgets.hpp"
#include "app/viewport_window.hpp"
#include "core/scene/scene.hpp"
#include "core/scene/selection.hpp"
#include "core/terrain_edit.hpp"

class QCheckBox;
class QLabel;
class QPushButton;
class QTimer;

namespace lb::app {

// Site mode panel (ROADMAP 5.1/5.2): ground around the building and Lumion-style brushes to shape
// it. "Add ground" makes rolling hills that are flat under the building (core/terrain.*); the
// brushes (Raise, Lower, Smooth, Flatten, Roughen) paint onto it with the left mouse button,
// working on the ground mesh itself (core/terrain_edit.*), so sculpting saves with the project.
// Once sculpted, the generator's sliders step aside; Undo steps back one stroke at a time.
class SitePanel : public QWidget {
    Q_OBJECT

public:
    SitePanel(Scene& scene, Selection& selection, ViewportWindow& viewport,
              QWidget* parent = nullptr);
    ~SitePanel() override;

protected:
    void hideEvent(QHideEvent* event) override;

private:
    // Fills the sliders from the content's size (first time) and makes the ground.
    void addGround();
    // Rebuilds the existing ground from the sliders (coalesced while a slider is dragged).
    void rebuild();
    void removeGround();
    void updateControls();
    ResourceId groundMaterial();

    // --- Sculpting ---
    void setTool(std::optional<BrushMode> tool);
    // Reads the ground's mesh into grid_ when it is not the one cached; false when it can't be
    // sculpted (no ground, moved, or not a grid).
    bool loadGrid();
    void onBrush(const glm::vec3& origin, const glm::vec3& direction, BrushPhase phase,
                 bool invert);
    void tick();
    void endStroke();
    // Writes `region` of grid_ into the scene's ground mesh.
    void commit(GridRegion region);
    void undoStroke();
    void updateRing(const std::optional<glm::vec3>& hit);
    Brush currentBrush() const;

    Scene& scene_;
    Selection& selection_;
    ViewportWindow& viewport_;
    Scene::ListenerId listener_ = 0;
    QTimer* rebuildTimer_ = nullptr;
    std::uint32_t seed_ = 1;
    bool updating_ = false;

    QLabel* hint_ = nullptr;
    QPushButton* add_ = nullptr;
    QPushButton* remove_ = nullptr;

    ui::Section* sculpt_ = nullptr;
    std::array<QPushButton*, 5> tools_{}; // indexed by BrushMode
    ui::SliderField* brushSize_ = nullptr;
    ui::SliderField* brushStrength_ = nullptr;
    QPushButton* undo_ = nullptr;
    QLabel* sculptHint_ = nullptr;
    std::optional<BrushMode> tool_;
    HeightGrid grid_;
    ResourceId gridMesh_; // the mesh grid_ was read from
    QTimer* strokeTimer_ = nullptr;
    QElapsedTimer strokeClock_;
    bool stroking_ = false;
    bool strokeChanged_ = false;
    bool invert_ = false;
    std::optional<float> flattenHeight_; // set by the first point of a Flatten stroke
    std::uint32_t strokeSeed_ = 1;
    glm::vec3 lastOrigin_{0.0f};
    glm::vec3 lastDirection_{0.0f, 0.0f, -1.0f};
    std::vector<float> strokeBefore_;
    std::vector<std::vector<float>> undoStack_;

    ui::Section* shape_ = nullptr;
    QLabel* sculptedNote_ = nullptr;
    ui::SliderField* size_ = nullptr;
    ui::SliderField* hillHeight_ = nullptr;
    ui::SliderField* hillSize_ = nullptr;
    QPushButton* newShape_ = nullptr;
    QCheckBox* flat_ = nullptr;
    ui::SliderField* blend_ = nullptr;
};

} // namespace lb::app
