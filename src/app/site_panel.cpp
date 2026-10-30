#include "app/site_panel.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>

#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QPushButton>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>

#include "app/theme.hpp"
#include "app/ui_helpers.hpp"
#include "core/log.hpp"
#include "core/terrain.hpp"

namespace lb::app {
namespace {

constexpr int kRebuildDelayMs = 120; // while a slider moves, rebuild once it pauses
constexpr int kStrokeTickMs = 33;    // brush applications while the button is held (~30 per second)
constexpr std::size_t kMaxUndo = 24; // strokes that can be undone
constexpr int kRingPoints = 72;

struct ToolInfo {
    BrushMode mode;
    const char* name;
    const char* tip;
};
constexpr std::array kTools = {
    ToolInfo{BrushMode::Raise, QT_TR_NOOP("Raise"),
             QT_TR_NOOP("Push the ground up where you paint. Hold Shift to lower instead.")},
    ToolInfo{BrushMode::Lower, QT_TR_NOOP("Lower"),
             QT_TR_NOOP("Push the ground down: ponds, ditches, terraces. Shift raises instead.")},
    ToolInfo{BrushMode::Smooth, QT_TR_NOOP("Smooth"),
             QT_TR_NOOP("Even out bumps and rough edges.")},
    ToolInfo{BrushMode::Flatten, QT_TR_NOOP("Flatten"),
             QT_TR_NOOP("Level the ground to the height under the cursor when you start the "
                        "stroke: platforms and building pads.")},
    ToolInfo{BrushMode::Roughen, QT_TR_NOOP("Roughen"),
             QT_TR_NOOP("Add small bumps for a natural, uneven surface.")},
};

} // namespace

SitePanel::SitePanel(Scene& scene, Selection& selection, ViewportWindow& viewport, QWidget* parent)
    : QWidget(parent), scene_(scene), selection_(selection), viewport_(viewport) {
    QVBoxLayout* layout = ui::scrollPage(this);
    QWidget* page = layout->parentWidget();

    auto* ground = new ui::Section(tr("Ground"), page);
    hint_ = ui::label(QString(), "muted", ground);
    hint_->setWordWrap(true);
    ground->addWide(hint_);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(6);
    add_ = new QPushButton(theme::icon(QStringLiteral("trees")), tr("Add ground"), ground);
    add_->setProperty("accent", true);
    add_->setToolTip(tr("Makes ground around your building: flat under it, rolling hills beyond. "
                        "Then pick a grass material for it in Materials."));
    remove_ = new QPushButton(theme::icon(QStringLiteral("trash-2")), tr("Remove"), ground);
    remove_->setToolTip(tr("Remove the ground"));
    buttons->addWidget(add_, 1);
    buttons->addWidget(remove_);
    ground->addWide(buttons);
    layout->addWidget(ground);

    // Sculpt: pick a brush, then paint on the ground with the left mouse button.
    sculpt_ = new ui::Section(tr("Sculpt"), page);
    auto* toolGrid = new QGridLayout;
    toolGrid->setSpacing(6);
    for (std::size_t i = 0; i < kTools.size(); ++i) {
        auto* button = new QPushButton(tr(kTools[i].name), sculpt_);
        button->setProperty("compact", true);
        button->setCheckable(true);
        button->setToolTip(tr(kTools[i].tip));
        tools_[static_cast<std::size_t>(kTools[i].mode)] = button;
        toolGrid->addWidget(button, static_cast<int>(i) / 3, static_cast<int>(i) % 3);
        const BrushMode mode = kTools[i].mode;
        connect(button, &QPushButton::clicked, this, [this, mode](bool on) {
            setTool(on ? std::optional<BrushMode>(mode) : std::nullopt);
        });
    }
    sculpt_->addWide(toolGrid);
    brushSize_ = new ui::SliderField(2.0, 120.0, 0, tr(" m"), sculpt_);
    brushSize_->setValue(20.0);
    sculpt_->addRow(tr("Brush size"), brushSize_)
        .label->setToolTip(tr("Diameter of the ring on the ground."));
    brushStrength_ = new ui::SliderField(0.05, 1.0, 2, QString(), sculpt_);
    brushStrength_->setValue(0.5);
    sculpt_->addRow(tr("Strength"), brushStrength_)
        .label->setToolTip(tr("How fast the brush works. Lower is gentler and easier to control."));
    undo_ = new QPushButton(theme::icon(QStringLiteral("rotate-ccw")), tr("Undo stroke"), sculpt_);
    undo_->setToolTip(tr("Take back the last stroke (Ctrl+Z)"));
    sculpt_->addWide(undo_);
    sculptHint_ = ui::label(QString(), "muted", sculpt_);
    sculptHint_->setWordWrap(true);
    sculpt_->addWide(sculptHint_);
    layout->addWidget(sculpt_);

    shape_ = new ui::Section(tr("Shape"), page);
    sculptedNote_ = ui::label(
        tr("Shaped by hand. Remove the ground to generate a new one."), "muted", shape_);
    sculptedNote_->setWordWrap(true);
    shape_->addWide(sculptedNote_);
    size_ = new ui::SliderField(30.0, 600.0, 0, tr(" m"), shape_);
    shape_->addRow(tr("Size"), size_)
        .label->setToolTip(tr("Width of the square of ground, centered on your building."));
    hillHeight_ = new ui::SliderField(0.0, 12.0, 1, tr(" m"), shape_);
    shape_->addRow(tr("Hill height"), hillHeight_)
        .label->setToolTip(tr("How high the hills rise above the flat ground. 0 = flat."));
    hillSize_ = new ui::SliderField(5.0, 150.0, 0, tr(" m"), shape_);
    shape_->addRow(tr("Hill size"), hillSize_)
        .label->setToolTip(tr("Typical distance between hilltops: small for lumpy ground, large for "
                              "long gentle slopes."));
    newShape_ = new QPushButton(theme::icon(QStringLiteral("refresh-cw")), tr("New shape"), shape_);
    newShape_->setToolTip(tr("Try another landscape with the same settings"));
    shape_->addWide(newShape_);
    flat_ = new QCheckBox(tr("Flat under the building"), shape_);
    flat_->setToolTip(tr("Keeps the ground level under and right around the building, so it sits "
                         "on the ground instead of in a hill."));
    flat_->setChecked(true);
    shape_->addWide(flat_);
    blend_ = new ui::SliderField(1.0, 80.0, 0, tr(" m"), shape_);
    shape_->addRow(tr("Blend"), blend_)
        .label->setToolTip(tr("How far from the building the flat ground takes to rise into the "
                              "hills."));
    layout->addWidget(shape_);
    layout->addStretch(1);

    rebuildTimer_ = new QTimer(this);
    rebuildTimer_->setSingleShot(true);
    rebuildTimer_->setInterval(kRebuildDelayMs);
    connect(rebuildTimer_, &QTimer::timeout, this, &SitePanel::rebuild);
    strokeTimer_ = new QTimer(this);
    strokeTimer_->setInterval(kStrokeTickMs);
    connect(strokeTimer_, &QTimer::timeout, this, &SitePanel::tick);
    connect(add_, &QPushButton::clicked, this, &SitePanel::addGround);
    connect(remove_, &QPushButton::clicked, this, &SitePanel::removeGround);
    connect(newShape_, &QPushButton::clicked, this, [this] {
        seed_ = std::random_device{}() % 100000u + 1u;
        rebuildTimer_->start();
    });
    connect(flat_, &QCheckBox::toggled, this, [this] {
        updateControls();
        if (!updating_) {
            rebuildTimer_->start();
        }
    });
    for (ui::SliderField* slider : {size_, hillHeight_, hillSize_, blend_}) {
        connect(slider, &ui::SliderField::valueEdited, this, [this] { rebuildTimer_->start(); });
    }
    connect(undo_, &QPushButton::clicked, this, &SitePanel::undoStroke);
    auto* undoKey = new QShortcut(QKeySequence::Undo, this);
    undoKey->setContext(Qt::ApplicationShortcut); // works while the viewport has the focus
    connect(undoKey, &QShortcut::activated, this, [this] {
        if (isVisible()) {
            undoStroke();
        }
    });
    connect(&viewport_, &ViewportWindow::terrainBrush, this,
            [this](glm::vec3 origin, glm::vec3 direction, BrushPhase phase, bool invert) {
                onBrush(origin, direction, phase, invert);
            });
    listener_ = scene_.addListener([this](const SceneChange& change) {
        if (change.kind == SceneChange::Kind::EntityAdded ||
            change.kind == SceneChange::Kind::EntityRemoved) {
            updateControls();
        }
    });
    updateControls();
}

SitePanel::~SitePanel() {
    scene_.removeListener(listener_);
}

void SitePanel::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    setTool(std::nullopt); // other modes click to select and turn the view with the left button
}

void SitePanel::updateControls() {
    const bool has = static_cast<bool>(findTerrain(scene_));
    const bool content = contentBounds(scene_).has_value();
    const bool sculpted = isSculpted(scene_);
    add_->setVisible(!has);
    add_->setEnabled(content);
    remove_->setVisible(has);
    sculpt_->setVisible(has);
    undo_->setEnabled(!undoStack_.empty() && !stroking_);
    shape_->setVisible(has);
    sculptedNote_->setVisible(sculpted);
    for (QWidget* control : std::initializer_list<QWidget*>{size_, hillHeight_, hillSize_, newShape_, flat_}) {
        control->setEnabled(!sculpted);
    }
    blend_->setEnabled(!sculpted && flat_->isChecked());
    if (!has && tool_) {
        setTool(std::nullopt);
    }
    sculptHint_->setText(
        tool_ ? tr("Hold the left mouse button and paint on the ground. Shift reverses Raise and "
                   "Lower. Middle drag turns the view, right drag pans, the wheel zooms.")
              : tr("Pick a brush to shape the ground."));
    hint_->setText(has ? tr("Sculpt the ground with the brushes below, and give it a grass "
                            "material in Materials.")
                       : content ? tr("Add ground around your building: flat under it, rolling "
                                      "hills beyond.")
                                 : tr("Import a model first (Import mode), then add ground "
                                      "around it."));
}

ResourceId SitePanel::groundMaterial() {
    // Keeps the material the user gave the ground; a first ground gets a plain grassy green.
    if (const EntityId old = findTerrain(scene_)) {
        if (const ResourceId material = scene_.entity(old)->meshInstance->material) {
            return material;
        }
    }
    ResourceId found;
    scene_.forEachMaterial([&](const MaterialResource& m) {
        if (m.name == "Ground") {
            found = m.id;
        }
    });
    if (found) {
        return found;
    }
    Material material = materialWithColor({0.16f, 0.26f, 0.09f});
    material.roughness = 0.95f;
    return scene_.addMaterial("Ground", material);
}

void SitePanel::addGround() {
    const TerrainSettings settings = terrainAroundContent(scene_);
    updating_ = true;
    size_->setValue(settings.extent);
    hillHeight_->setValue(settings.hillHeight);
    hillSize_->setValue(settings.hillSize);
    blend_->setValue(settings.padBlend);
    flat_->setChecked(true);
    updating_ = false;
    seed_ = std::random_device{}() % 100000u + 1u;
    rebuild();
    if (const EntityId ground = findTerrain(scene_)) {
        selection_.select(ground); // ready for a material from the library
    }
}

void SitePanel::rebuild() {
    if (updating_ || isSculpted(scene_) || !contentBounds(scene_)) {
        return; // hand-sculpted ground is never regenerated
    }
    TerrainSettings settings = terrainAroundContent(scene_); // center, pad rectangle, base height
    settings.extent = static_cast<float>(size_->value());
    settings.cells = terrainCellsFor(settings.extent);
    settings.hillHeight = static_cast<float>(hillHeight_->value());
    settings.hillSize = static_cast<float>(hillSize_->value());
    settings.padBlend = static_cast<float>(blend_->value());
    settings.pad = flat_->isChecked();
    settings.seed = seed_;
    const bool wasSelected = findTerrain(scene_) && selection_.contains(findTerrain(scene_));
    const EntityId ground = setTerrain(scene_, settings, groundMaterial());
    log::info("Ground: {} m square, {} cells, hills {} m every {} m, {}", settings.extent,
              settings.cells, settings.hillHeight, settings.hillSize,
              settings.pad ? "flat under the building" : "no flat pad");
    if (wasSelected) {
        selection_.select(ground); // the old entity is gone: keep the new one selected
    }
    undoStack_.clear();
    gridMesh_ = {};
    updateControls();
}

void SitePanel::removeGround() {
    rebuildTimer_->stop();
    setTool(std::nullopt);
    removeTerrain(scene_);
    undoStack_.clear();
    gridMesh_ = {};
    updateControls();
}

// --- Sculpting -----------------------------------------------------------------------------------

Brush SitePanel::currentBrush() const {
    Brush brush;
    brush.mode = tool_.value_or(BrushMode::Raise);
    if (invert_ && brush.mode == BrushMode::Raise) {
        brush.mode = BrushMode::Lower;
    } else if (invert_ && brush.mode == BrushMode::Lower) {
        brush.mode = BrushMode::Raise;
    }
    brush.radius = static_cast<float>(brushSize_->value()) * 0.5f;
    brush.strength = static_cast<float>(brushStrength_->value());
    return brush;
}

void SitePanel::setTool(std::optional<BrushMode> tool) {
    if (tool && !loadGrid()) {
        tool = std::nullopt; // no ground, or one that was moved or isn't a grid
    }
    if (stroking_) {
        endStroke();
    }
    tool_ = tool;
    for (std::size_t i = 0; i < tools_.size(); ++i) {
        tools_[i]->setChecked(tool_ && static_cast<std::size_t>(*tool_) == i);
    }
    viewport_.setTerrainBrush(tool_.has_value());
    if (!tool_) {
        viewport_.setBrushRing({});
    }
    updateControls();
}

bool SitePanel::loadGrid() {
    const EntityId ground = findTerrain(scene_);
    if (!ground) {
        return false;
    }
    const Entity* entity = scene_.entity(ground);
    if (!(entity->transform == Transform{})) {
        return false; // the grid is in world space only while the ground sits where it was made
    }
    const ResourceId meshId = entity->meshInstance->mesh;
    if (meshId == gridMesh_ && grid_.cells > 0) {
        return true;
    }
    const MeshResource* mesh = scene_.mesh(meshId);
    std::optional<HeightGrid> grid = mesh != nullptr ? heightGridFromMesh(mesh->data) : std::nullopt;
    if (!grid) {
        return false;
    }
    grid_ = std::move(*grid);
    gridMesh_ = meshId;
    undoStack_.clear(); // a different ground: its history is not ours
    return true;
}

void SitePanel::updateRing(const std::optional<glm::vec3>& hit) {
    if (!hit || !tool_) {
        viewport_.setBrushRing({});
        return;
    }
    const float radius = static_cast<float>(brushSize_->value()) * 0.5f;
    std::vector<glm::vec3> ring;
    ring.reserve(kRingPoints);
    for (int i = 0; i < kRingPoints; ++i) {
        const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / kRingPoints;
        const glm::vec2 p = glm::vec2(*hit) + radius * glm::vec2(std::cos(angle), std::sin(angle));
        ring.emplace_back(p, grid_.heightAt(p) + 0.1f); // just above the surface
    }
    viewport_.setBrushRing(std::move(ring));
}

void SitePanel::onBrush(const glm::vec3& origin, const glm::vec3& direction, BrushPhase phase,
                        bool invert) {
    if (!tool_ || !loadGrid()) {
        return;
    }
    lastOrigin_ = origin;
    lastDirection_ = direction;
    invert_ = invert;
    const std::optional<glm::vec3> hit = grid_.intersect(origin, direction);
    switch (phase) {
    case BrushPhase::Hover:
        updateRing(hit);
        break;
    case BrushPhase::Begin:
        updateRing(hit);
        stroking_ = true;
        strokeChanged_ = false;
        flattenHeight_.reset();
        strokeBefore_ = grid_.heights;
        strokeSeed_ = std::random_device{}() % 100000u + 1u;
        strokeClock_.start();
        strokeTimer_->start();
        updateControls();
        break;
    case BrushPhase::Drag:
        updateRing(hit); // the timer applies the brush at the latest position
        break;
    case BrushPhase::End:
        endStroke();
        updateRing(hit);
        break;
    }
}

void SitePanel::tick() {
    if (!stroking_ || !tool_) {
        return;
    }
    const double seconds = std::min(static_cast<double>(strokeClock_.restart()) / 1000.0, 0.1);
    const std::optional<glm::vec3> hit = grid_.intersect(lastOrigin_, lastDirection_);
    if (!hit) {
        return; // the cursor is off the ground
    }
    if (!flattenHeight_) {
        flattenHeight_ = hit->z; // Flatten levels to where the stroke began
    }
    const GridRegion region = applyBrush(grid_, currentBrush(), glm::vec2(*hit),
                                         static_cast<float>(seconds), *flattenHeight_, strokeSeed_);
    if (region.empty()) {
        return;
    }
    strokeChanged_ = true;
    commit(region);
}

void SitePanel::commit(GridRegion region) {
    const MeshResource* mesh = scene_.mesh(gridMesh_);
    if (mesh == nullptr) {
        return;
    }
    MeshData data = mesh->data;
    writeHeights(data, grid_, region);
    scene_.setMeshData(gridMesh_, std::move(data));
}

void SitePanel::endStroke() {
    strokeTimer_->stop();
    if (!stroking_) {
        return;
    }
    stroking_ = false;
    if (strokeChanged_) {
        undoStack_.push_back(std::move(strokeBefore_));
        if (undoStack_.size() > kMaxUndo) {
            undoStack_.erase(undoStack_.begin());
        }
        markSculpted(scene_); // from now on the generator's sliders leave the ground alone
    }
    strokeBefore_.clear();
    updateControls();
}

void SitePanel::undoStroke() {
    if (stroking_ || undoStack_.empty() || !loadGrid()) {
        return;
    }
    grid_.heights = std::move(undoStack_.back());
    undoStack_.pop_back();
    commit(wholeGrid(grid_));
    updateControls();
}

} // namespace lb::app
