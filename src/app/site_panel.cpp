#include "app/site_panel.hpp"

#include <algorithm>
#include <random>

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include "app/theme.hpp"
#include "app/ui_helpers.hpp"
#include "core/log.hpp"
#include "core/terrain.hpp"

namespace lb::app {
namespace {

constexpr int kRebuildDelayMs = 120; // while a slider moves, rebuild once it pauses

} // namespace

SitePanel::SitePanel(Scene& scene, Selection& selection, QWidget* parent)
    : QWidget(parent), scene_(scene), selection_(selection) {
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

    shape_ = new ui::Section(tr("Shape"), page);
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

void SitePanel::updateControls() {
    const bool has = static_cast<bool>(findTerrain(scene_));
    const bool content = contentBounds(scene_).has_value();
    add_->setVisible(!has);
    add_->setEnabled(content);
    remove_->setVisible(has);
    newShape_->setVisible(has);
    shape_->setVisible(has);
    blend_->setEnabled(flat_->isChecked());
    hint_->setText(has ? tr("The ground is made. Change its shape below; give it a grass material "
                            "in Materials.")
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
    if (updating_ || !contentBounds(scene_)) {
        return;
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
    updateControls();
}

void SitePanel::removeGround() {
    rebuildTimer_->stop();
    removeTerrain(scene_);
    updateControls();
}

} // namespace lb::app
