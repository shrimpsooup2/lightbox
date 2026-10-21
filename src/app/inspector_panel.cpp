#include "app/inspector_panel.hpp"

#include <cmath>

#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include "app/ui_helpers.hpp"

namespace lb::app {
namespace {

using ui::label;
using ui::sectionHeader;

constexpr const char* kAxisRoles[] = {"axisX", "axisY", "axisZ"};
constexpr const char* kAxisNames[] = {"X", "Y", "Z"};

QDoubleSpinBox* numberField(double min, double max, int decimals, QWidget* parent) {
    auto* field = new QDoubleSpinBox(parent);
    field->setRange(min, max);
    field->setDecimals(decimals);
    field->setButtonSymbols(QAbstractSpinBox::NoButtons);
    field->setAccelerated(true);
    field->setKeyboardTracking(false); // apply on Enter/focus-out or wheel, not every keystroke
    field->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // The default size hint fits the widest value in range (e.g. "-1000000.000"); let the grid
    // share the panel width instead.
    field->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    field->setMinimumWidth(40);
    return field;
}

// One "label  X[ ] Y[ ] Z[ ]" row of the transform grid.
void addVectorRow(QGridLayout* grid, int row, const QString& title, std::array<QDoubleSpinBox*, 3>& fields,
                  double min, double max, int decimals, QWidget* parent) {
    grid->addWidget(label(title, "muted", parent), row, 0);
    for (int axis = 0; axis < 3; ++axis) {
        auto* cell = new QWidget(parent);
        auto* layout = new QHBoxLayout(cell);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        layout->addWidget(label(QString::fromLatin1(kAxisNames[axis]), kAxisRoles[axis], cell));
        fields[static_cast<std::size_t>(axis)] = numberField(min, max, decimals, cell);
        layout->addWidget(fields[static_cast<std::size_t>(axis)], 1);
        grid->addWidget(cell, row, axis + 1);
    }
}

} // namespace

InspectorPanel::InspectorPanel(Scene& scene, Selection& selection, QWidget* parent)
    : QWidget(parent), scene_(scene), selection_(selection) {
    setProperty("panel", true);
    stack_ = new QStackedWidget(this);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(stack_);

    // Empty state.
    auto* empty = new QWidget(stack_);
    empty->setProperty("panel", true);
    auto* emptyLayout = new QVBoxLayout(empty);
    emptyLayout->setContentsMargins(24, 24, 24, 24);
    emptyLayout->addStretch(1);
    auto* emptyTitle = label(tr("Nothing selected"), "heading", empty);
    emptyTitle->setAlignment(Qt::AlignCenter);
    auto* emptyHint =
        label(tr("Select an object in the Scene panel to see and edit its properties."), "muted",
              empty);
    emptyHint->setAlignment(Qt::AlignCenter);
    emptyHint->setWordWrap(true);
    emptyLayout->addWidget(emptyTitle);
    emptyLayout->addWidget(emptyHint);
    emptyLayout->addStretch(2);
    stack_->addWidget(empty);

    // Editor.
    auto* scroll = new QScrollArea(stack_);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* editor = new QWidget(scroll);
    editor->setProperty("panel", true);
    auto* layout = new QVBoxLayout(editor);
    layout->setContentsMargins(12, 10, 12, 12);
    layout->setSpacing(6);

    name_ = new QLineEdit(editor);
    name_->setProperty("flat", true);
    name_->setProperty("role", "title");
    kind_ = label(QString(), "muted", editor);
    layout->addWidget(name_);
    layout->addWidget(kind_);

    layout->addWidget(sectionHeader(tr("Transform"), editor));
    auto* transform = new QWidget(editor);
    auto* grid = new QGridLayout(transform);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(6);
    addVectorRow(grid, 0, tr("Position"), position_, -1.0e6, 1.0e6, 3, transform);
    addVectorRow(grid, 1, tr("Rotation"), rotation_, -180.0, 180.0, 1, transform);
    addVectorRow(grid, 2, tr("Scale"), scale_, 0.001, 1.0e4, 3, transform);
    for (QDoubleSpinBox* field : position_) {
        field->setToolTip(tr("Meters, relative to the parent"));
    }
    for (QDoubleSpinBox* field : rotation_) {
        field->setToolTip(tr("Degrees around the parent's axes"));
    }
    for (QDoubleSpinBox* field : scale_) {
        field->setToolTip(tr("Scale factor (1 = original size)"));
    }
    for (QDoubleSpinBox* field : rotation_) {
        field->setWrapping(true);
    }
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(2, 1);
    grid->setColumnStretch(3, 1);
    layout->addWidget(transform);

    materialSection_ = new QWidget(editor);
    auto* materialLayout = new QVBoxLayout(materialSection_);
    materialLayout->setContentsMargins(0, 0, 0, 0);
    materialLayout->setSpacing(6);
    materialLayout->addWidget(sectionHeader(tr("Material"), materialSection_));
    auto* materialRow = new QHBoxLayout;
    materialRow->setSpacing(10);
    swatch_ = new QToolButton(materialSection_);
    swatch_->setIconSize(QSize(36, 20));
    swatch_->setToolTip(tr("Change base color"));
    swatch_->setCursor(Qt::PointingHandCursor);
    materialName_ = label(QString(), nullptr, materialSection_);
    materialRow->addWidget(swatch_);
    materialRow->addWidget(materialName_, 1);
    materialLayout->addLayout(materialRow);
    materialUsage_ = label(QString(), "muted", materialSection_);
    materialUsage_->setWordWrap(true);
    materialLayout->addWidget(materialUsage_);
    layout->addWidget(materialSection_);

    geometrySection_ = new QWidget(editor);
    auto* geometryLayout = new QVBoxLayout(geometrySection_);
    geometryLayout->setContentsMargins(0, 0, 0, 0);
    geometryLayout->setSpacing(6);
    geometryLayout->addWidget(sectionHeader(tr("Geometry"), geometrySection_));
    geometryInfo_ = label(QString(), "muted", geometrySection_);
    geometryLayout->addWidget(geometryInfo_);
    layout->addWidget(geometrySection_);
    layout->addStretch(1);

    scroll->setWidget(editor);
    stack_->addWidget(scroll);

    connect(name_, &QLineEdit::editingFinished, this, &InspectorPanel::applyName);
    for (auto* group : {&position_, &rotation_, &scale_}) {
        for (QDoubleSpinBox* field : *group) {
            connect(field, &QDoubleSpinBox::valueChanged, this, &InspectorPanel::applyTransform);
        }
    }
    connect(swatch_, &QToolButton::clicked, this, &InspectorPanel::pickBaseColor);

    selectionListener_ = selection_.addListener([this] { showEntity(selection_.primary()); });
    sceneListener_ = scene_.addListener([this](const SceneChange& change) {
        if (updating_ || !current_) {
            return;
        }
        const bool aboutCurrent = change.entity == current_;
        const bool aboutMaterial = change.resource && change.resource == currentMaterial();
        if (aboutCurrent || aboutMaterial || change.kind == SceneChange::Kind::MeshInstanceChanged) {
            refresh(); // usage counts depend on other entities too
        }
    });
    showEntity(selection_.primary());
}

InspectorPanel::~InspectorPanel() {
    scene_.removeListener(sceneListener_);
    selection_.removeListener(selectionListener_);
}

ResourceId InspectorPanel::currentMaterial() const {
    const Entity* entity = scene_.entity(current_);
    return entity && entity->meshInstance ? entity->meshInstance->material : ResourceId{};
}

void InspectorPanel::showEntity(EntityId id) {
    current_ = scene_.entity(id) ? id : EntityId{};
    stack_->setCurrentIndex(current_ ? 1 : 0);
    refresh();
}

void InspectorPanel::refresh() {
    const Entity* entity = scene_.entity(current_);
    if (entity == nullptr) {
        return;
    }
    updating_ = true;
    if (!name_->hasFocus()) {
        name_->setText(QString::fromStdString(entity->name));
    }
    kind_->setText(entity->meshInstance ? tr("Object")
                   : entity->light
                       ? tr("Light · change it in Light mode")
                       : tr("Group · %1").arg(ui::count(static_cast<int>(entity->children.size()),
                                                        tr("1 item"), tr("%1 items"))));

    const Transform& t = entity->transform;
    const glm::vec3 euler = glm::degrees(glm::eulerAngles(t.rotation));
    for (int axis = 0; axis < 3; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        position_[i]->setValue(t.position[axis]);
        rotation_[i]->setValue(euler[axis]);
        scale_[i]->setValue(t.scale[axis]);
    }

    const bool hasMesh = entity->meshInstance.has_value();
    materialSection_->setVisible(hasMesh);
    geometrySection_->setVisible(hasMesh);
    if (hasMesh) {
        const MaterialResource* material = scene_.material(entity->meshInstance->material);
        const glm::vec3 color = material ? material->material.baseColor : Material{}.baseColor;
        swatch_->setIcon(ui::colorSwatch(ui::displayColor(color)));
        swatch_->setEnabled(material != nullptr);
        materialName_->setText(material ? QString::fromStdString(material->name)
                                        : tr("Default clay"));
        int users = 0;
        if (material) {
            scene_.traverse([&](const Entity& e, const glm::mat4&) {
                users += e.meshInstance && e.meshInstance->material == material->id ? 1 : 0;
            });
        }
        materialUsage_->setVisible(users > 1);
        materialUsage_->setText(
            tr("Shared by %1 objects. Changing it changes all of them.").arg(users));

        const MeshResource* mesh = scene_.mesh(entity->meshInstance->mesh);
        geometryInfo_->setText(mesh ? tr("%1 · %2 triangles · %3 vertices")
                                          .arg(QString::fromStdString(mesh->name))
                                          .arg(mesh->data.indices.size() / 3)
                                          .arg(mesh->data.positions.size())
                                    : QString());
    }
    updating_ = false;
}

void InspectorPanel::applyName() {
    const QString name = name_->text().trimmed();
    if (!current_ || name.isEmpty()) {
        refresh(); // restore the current name
        return;
    }
    updating_ = true;
    scene_.setName(current_, name.toStdString());
    updating_ = false;
}

void InspectorPanel::applyTransform() {
    if (updating_ || !current_) {
        return;
    }
    Transform t;
    glm::vec3 eulerDegrees;
    for (int axis = 0; axis < 3; ++axis) {
        const auto i = static_cast<std::size_t>(axis);
        t.position[axis] = static_cast<float>(position_[i]->value());
        eulerDegrees[axis] = static_cast<float>(rotation_[i]->value());
        t.scale[axis] = static_cast<float>(scale_[i]->value());
    }
    t.rotation = glm::quat(glm::radians(eulerDegrees));
    updating_ = true; // keep the fields exactly as typed (no Euler round trip while editing)
    scene_.setTransform(current_, t);
    updating_ = false;
}

void InspectorPanel::pickBaseColor() {
    const ResourceId materialId = currentMaterial();
    const MaterialResource* material = scene_.material(materialId);
    if (material == nullptr) {
        return;
    }
    const QColor chosen = QColorDialog::getColor(ui::displayColor(material->material.baseColor), this,
                                                 tr("Base color"));
    if (!chosen.isValid()) {
        return;
    }
    Material edited = material->material;
    edited.baseColor = ui::linearColor(chosen);
    scene_.setMaterial(materialId, edited);
}

} // namespace lb::app
