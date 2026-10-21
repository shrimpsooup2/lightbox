#include "app/materials_panel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <QCheckBox>
#include <QColorDialog>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPainter>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "app/background.hpp"
#include "app/property_widgets.hpp"
#include "app/texture_browser.hpp"
#include "app/texture_slot.hpp"
#include "app/theme.hpp"
#include "app/ui_helpers.hpp"
#include "core/image.hpp"
#include "import/importer.hpp"

namespace lb::app {
namespace {

constexpr int kIdRole = Qt::UserRole;
constexpr int kIconKeyRole = Qt::UserRole + 1; // what the row's icon was drawn from
constexpr int kListIconSize = 32;
constexpr int kMinListRows = 3;
constexpr int kMaxListRows = 8;
constexpr double kMinScale = 0.001;
constexpr double kMaxScale = 10000.0;

struct SurfacePreset {
    const char* name;
    float roughness;
    float metalness;
    const char* examples;
};

constexpr std::array kSurfacePresets = {
    SurfacePreset{QT_TR_NOOP("Matte"), 0.9f, 0.0f, QT_TR_NOOP("plaster, concrete, raw wood")},
    SurfacePreset{QT_TR_NOOP("Satin"), 0.5f, 0.0f, QT_TR_NOOP("painted walls, oiled wood")},
    SurfacePreset{QT_TR_NOOP("Glossy"), 0.15f, 0.0f, QT_TR_NOOP("lacquer, ceramic tiles")},
    SurfacePreset{QT_TR_NOOP("Metal"), 0.45f, 1.0f, QT_TR_NOOP("brushed steel, aluminium")},
    SurfacePreset{QT_TR_NOOP("Polished metal"), 0.08f, 1.0f, QT_TR_NOOP("chrome, polished brass")},
};

ResourceId idOf(const QTreeWidgetItem* item) {
    return item != nullptr ? ResourceId{item->data(0, kIdRole).toULongLong()} : ResourceId{};
}

int usageOf(const std::unordered_map<ResourceId, int>& usage, ResourceId id) {
    const auto it = usage.find(id);
    return it != usage.end() ? it->second : 0;
}

// Objects per material in one pass (Scene::userCount would walk all entities per material).
std::unordered_map<ResourceId, int> materialUsage(const Scene& scene) {
    std::unordered_map<ResourceId, int> usage;
    scene.traverse([&](const Entity& entity, const glm::mat4&) {
        if (entity.meshInstance && entity.meshInstance->material) {
            ++usage[entity.meshInstance->material];
        }
    });
    return usage;
}

QString objectsText(int users) {
    return users == 0 ? MaterialsPanel::tr("unused")
                      : ui::count(users, MaterialsPanel::tr("1 object"),
                                  MaterialsPanel::tr("%1 objects"));
}

void setUsage(QTreeWidgetItem* item, int users) {
    const QString text = objectsText(users);
    if (item->text(1) != text) {
        item->setText(1, text);
        item->setToolTip(0, users == 0 ? MaterialsPanel::tr("No object uses this material yet")
                                       : MaterialsPanel::tr("Used by %1: changing the material "
                                                            "changes all of them")
                                             .arg(text));
    }
}

// The base color map multiplied by the base color, as the renderer shows it.
QPixmap tinted(const QPixmap& thumbnail, const QColor& color) {
    if (color == QColor(Qt::white)) {
        return thumbnail;
    }
    QPixmap result = thumbnail;
    QPainter painter(&result);
    painter.setCompositionMode(QPainter::CompositionMode_Multiply);
    painter.fillRect(result.rect(), color);
    painter.setCompositionMode(QPainter::CompositionMode_DestinationIn); // restore transparency
    painter.drawPixmap(0, 0, thumbnail);
    painter.end();
    return result;
}

// Updates a number field from the document without emitting and without disturbing typing.
void setField(QDoubleSpinBox* field, double value) {
    if (field->hasFocus() && std::abs(field->value() - value) < 1e-9) {
        return;
    }
    const QSignalBlocker blocker(field);
    field->setValue(value);
}

QHBoxLayout* axisPair(QWidget* x, QWidget* y, QWidget* parent) {
    auto* row = new QHBoxLayout;
    row->setSpacing(4);
    row->addWidget(ui::label(QStringLiteral("X"), "axisX", parent));
    row->addWidget(x, 1);
    row->addSpacing(4);
    row->addWidget(ui::label(QStringLiteral("Y"), "axisY", parent));
    row->addWidget(y, 1);
    return row;
}

QString imageFilter() {
    QStringList patterns;
    for (std::string_view extension : imageFileExtensions()) {
        patterns << QLatin1Char('*') +
                        QString::fromLatin1(extension.data(),
                                            static_cast<qsizetype>(extension.size()));
    }
    return MaterialsPanel::tr("Images (%1)").arg(patterns.join(QLatin1Char(' ')));
}

// Texture sets: loose images, or a pack downloaded from a texture site (zip).
QString textureSetFilter() {
    QString filter = imageFilter();
    filter.insert(filter.size() - 1, QStringLiteral(" *.zip"));
    return MaterialsPanel::tr("Texture images or downloaded packs") +
           filter.mid(filter.indexOf(QLatin1Char('(')) - 1);
}

QString errorText(std::exception_ptr error) {
    try {
        std::rethrow_exception(std::move(error));
    } catch (const std::exception& e) {
        return QString::fromUtf8(e.what());
    } catch (...) {
        return MaterialsPanel::tr("Unknown error");
    }
}

} // namespace

MaterialsPanel::MaterialsPanel(Scene& scene, Selection& selection, QWidget* parent)
    : QWidget(parent), scene_(scene), selection_(selection) {
    thumbnails_ = new TextureThumbnails(scene_, this);
    QVBoxLayout* layout = ui::scrollPage(this);
    QWidget* page = layout->parentWidget();

    // Actions. The online library comes first: it is the quickest way to a good material.
    library_ = new QPushButton(theme::icon(QStringLiteral("download")), tr("Texture library…"), page);
    library_->setProperty("accent", true);
    library_->setToolTip(tr("Browse free CC0 textures from ambientCG and Poly Haven; one click "
                            "downloads a set and applies it at its real-world size"));
    layout->addWidget(library_);
    auto* actions = new QGridLayout;
    actions->setSpacing(6);
    create_ = new QPushButton(theme::icon(QStringLiteral("palette")), tr("New material"), page);
    create_->setProperty("compact", true);
    create_->setToolTip(tr("Add a plain material to the project"));
    fromTextures_ = new QPushButton(page);
    fromTextures_->setProperty("compact", true);
    fromTextures_->setToolTip(
        tr("Create a material from a texture set: select all of its images at once (color, normal, "
           "roughness… as downloaded from Poly Haven, ambientCG and similar sites)"));
    apply_ = new QPushButton(tr("Apply to selected"), page);
    apply_->setProperty("accent", true);
    apply_->setProperty("compact", true);
    selectUsers_ =
        new QPushButton(theme::icon(QStringLiteral("mouse-pointer-2")), tr("Select users"), page);
    selectUsers_->setProperty("compact", true);
    selectUsers_->setToolTip(tr("Select the objects that use this material"));
    delete_ = new QPushButton(theme::icon(QStringLiteral("trash-2")), QString(), page);
    delete_->setProperty("compact", true);
    actions->addWidget(create_, 0, 0);
    actions->addWidget(fromTextures_, 0, 1, 1, 2);
    actions->addWidget(apply_, 1, 0);
    actions->addWidget(selectUsers_, 1, 1);
    actions->addWidget(delete_, 1, 2);
    actions->setColumnStretch(0, 1);
    actions->setColumnStretch(1, 1);
    layout->addLayout(actions);
    progress_ = new QProgressBar(page);
    progress_->setRange(0, 100);
    progress_->setTextVisible(false);
    progress_->hide();
    layout->addWidget(progress_);
    notice_ = ui::label(QString(), "muted", page);
    notice_->setWordWrap(true);
    notice_->hide();
    layout->addWidget(notice_);
    setImporting(false);

    // Materials in the project.
    list_ = new QTreeWidget(page);
    list_->setColumnCount(2);
    list_->setHeaderHidden(true);
    list_->setRootIsDecorated(false);
    list_->setUniformRowHeights(true);
    list_->setIconSize(QSize(kListIconSize, kListIconSize));
    list_->header()->setStretchLastSection(false);
    list_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    list_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layout->addWidget(list_);
    noMaterial_ = ui::label(QString(), "muted", page);
    noMaterial_->setWordWrap(true);
    layout->addWidget(noMaterial_);

    // Base.
    base_ = new ui::Section(tr("Base"), page);
    name_ = new QLineEdit(base_);
    name_->setPlaceholderText(tr("Name"));
    base_->addRow(tr("Name"), name_);
    swatch_ = new QToolButton(base_);
    swatch_->setIconSize(QSize(48, 20));
    swatch_->setCursor(Qt::PointingHandCursor);
    swatch_->setToolTip(tr("Click to change the color"));
    auto* colorRow = new QHBoxLayout;
    colorRow->addWidget(swatch_);
    colorRow->addStretch();
    base_->addRow(tr("Color"), colorRow)
        .label->setToolTip(tr("Surface color. With a color map it tints the image: white shows "
                              "the image's own colors."));
    baseColorMap_ = new TextureSlotWidget(scene_, *thumbnails_, TextureSlotWidget::Kind::Color,
                                          tr("Color map"), base_);
    base_->addRow(tr("Color map"), baseColorMap_)
        .label->setToolTip(tr("Image with the surface colors (albedo, diffuse). Adding one sets "
                              "the color to white."));
    layout->addWidget(base_);

    // Surface.
    surface_ = new ui::Section(tr("Surface"), page);
    auto* presets = new QGridLayout;
    presets->setSpacing(4);
    for (int i = 0; i < static_cast<int>(kSurfacePresets.size()); ++i) {
        const SurfacePreset& preset = kSurfacePresets[static_cast<std::size_t>(i)];
        auto* button = new QPushButton(tr(preset.name), surface_);
        button->setProperty("compact", true);
        button->setToolTip(tr("Roughness %1, metalness %2: %3")
                               .arg(static_cast<double>(preset.roughness), 0, 'f', 2)
                               .arg(static_cast<double>(preset.metalness), 0, 'f', 0)
                               .arg(tr(preset.examples)));
        connect(button, &QPushButton::clicked, this, [this, i] { applyPreset(i); });
        presets->addWidget(button, i / 3, i % 3);
    }
    surface_->addWide(presets);
    roughness_ = new ui::SliderField(0.0, 1.0, 2, QString(), surface_);
    surface_->addRow(tr("Roughness"), roughness_)
        .label->setToolTip(tr("0 = polished and mirror-like, 1 = matte. Multiplies the "
                              "roughness map."));
    roughnessMap_ = new TextureSlotWidget(scene_, *thumbnails_, TextureSlotWidget::Kind::Scalar,
                                          tr("Roughness map"), surface_);
    surface_->addRow(tr("Map"), roughnessMap_)
        .label->setToolTip(tr("Roughness map: white = matte, black = glossy (turn on Invert for "
                              "a gloss map). Adding one sets Roughness to 1."));
    metalness_ = new ui::SliderField(0.0, 1.0, 2, QString(), surface_);
    surface_->addRow(tr("Metalness"), metalness_)
        .label->setToolTip(tr("1 for bare metal (steel, aluminium, brass), 0 for everything else. "
                              "Multiplies the metalness map."));
    metalnessMap_ = new TextureSlotWidget(scene_, *thumbnails_, TextureSlotWidget::Kind::Scalar,
                                          tr("Metalness map"), surface_);
    surface_->addRow(tr("Map"), metalnessMap_)
        .label->setToolTip(tr("Metalness map: white = metal. Adding one sets Metalness to 1."));
    layout->addWidget(surface_);

    // Relief.
    relief_ = new ui::Section(tr("Relief"), page);
    normalMap_ = new TextureSlotWidget(scene_, *thumbnails_, TextureSlotWidget::Kind::Color,
                                       tr("Normal map"), relief_);
    relief_->addRow(tr("Normal map"), normalMap_)
        .label->setToolTip(tr("The purple-blue image of a texture set: adds fine relief (joints, "
                              "grain) without extra geometry."));
    normalStrength_ = new ui::SliderField(0.0, 2.0, 2, QString(), relief_);
    relief_->addRow(tr("Strength"), normalStrength_)
        .label->setToolTip(tr("How pronounced the relief is: 0 = flat, 1 = as authored."));
    normalFlipY_ = new QCheckBox(tr("DirectX normal map (flip green)"), relief_);
    normalFlipY_->setToolTip(tr("Turn on when the relief looks inverted (dents instead of "
                                "ridges). Files named …NormalDX and Unreal/Quixel maps need it."));
    relief_->addWide(normalFlipY_);
    heightMap_ = new TextureSlotWidget(scene_, *thumbnails_, TextureSlotWidget::Kind::Scalar,
                                       tr("Height map"), relief_);
    relief_->addRow(tr("Height map"), heightMap_)
        .label->setToolTip(tr("Black-and-white image (white = high, often named Displacement or "
                              "Height): makes joints and gaps look truly recessed, with their own "
                              "shadows."));
    heightDepth_ = new ui::SliderField(0.0, 0.1, 3, tr(" m"), relief_);
    relief_->addRow(tr("Depth"), heightDepth_)
        .label->setToolTip(tr("How deep the height map's black parts are, e.g. 0.01–0.02 m for "
                              "brick joints, 0.05 m for cobbles. 0 = flat."));
    layout->addWidget(relief_);

    // Occlusion.
    occlusion_ = new ui::Section(tr("Occlusion"), page);
    occlusionMap_ = new TextureSlotWidget(scene_, *thumbnails_, TextureSlotWidget::Kind::Scalar,
                                          tr("Occlusion map"), occlusion_);
    occlusion_->addRow(tr("Map"), occlusionMap_)
        .label->setToolTip(tr("Ambient occlusion: darkens crevices in sky and fill light (not in "
                              "direct sun)."));
    layout->addWidget(occlusion_);

    // Glass and glow.
    glass_ = new ui::Section(tr("Glass && glow"), page);
    auto* glassPresets = new QGridLayout;
    glassPresets->setSpacing(4);
    struct GlassPreset {
        const char* name;
        const char* tip;
        float transmission;
        float roughness;
        glm::vec3 tint;
    };
    const GlassPreset glassPresetList[] = {
        {QT_TR_NOOP("Clear"), QT_TR_NOOP("Window glass"), 1.0f, 0.02f, {1.0f, 1.0f, 1.0f}},
        {QT_TR_NOOP("Frosted"), QT_TR_NOOP("Satin-etched glass, shower screens"), 1.0f, 0.35f,
         {1.0f, 1.0f, 1.0f}},
        {QT_TR_NOOP("Tinted"), QT_TR_NOOP("Grey solar-control glass"), 1.0f, 0.02f,
         {0.55f, 0.6f, 0.6f}},
        {QT_TR_NOOP("Solid"), QT_TR_NOOP("Not see-through"), 0.0f, -1.0f, {1.0f, 1.0f, 1.0f}},
    };
    int presetIndex = 0;
    for (const GlassPreset& preset : glassPresetList) {
        auto* button = new QPushButton(tr(preset.name), glass_);
        button->setProperty("compact", true);
        button->setToolTip(tr(preset.tip));
        connect(button, &QPushButton::clicked, this, [this, preset] {
            editMaterial([&](Material& m) {
                m.transmission = preset.transmission;
                if (preset.roughness >= 0.0f) {
                    m.roughness = preset.roughness;
                    m.metalness = 0.0f;
                    m.baseColor = preset.tint;
                    m.thinWalled = true;
                    m.ior = 1.5f;
                }
            });
        });
        glassPresets->addWidget(button, presetIndex / 4, presetIndex % 4);
        ++presetIndex;
    }
    glass_->addWide(glassPresets);
    transmission_ = new ui::SliderField(0.0, 1.0, 2, QString(), glass_);
    glass_->addRow(tr("See-through"), transmission_)
        .label->setToolTip(tr("0 = solid, 1 = clear glass. The color tints the light passing "
                              "through; roughness frosts it."));
    ior_ = new ui::SliderField(1.0, 2.5, 2, QString(), glass_);
    glass_->addRow(tr("Refraction"), ior_)
        .label->setToolTip(tr("Index of refraction: glass 1.5, water 1.33, acrylic 1.49. Higher "
                              "bends light more and reflects more."));
    thinWalled_ = new QCheckBox(tr("Thin pane (window glass)"), glass_);
    thinWalled_->setToolTip(tr("On for windows and glass walls modeled as single surfaces or thin "
                               "panes: light passes straight through. Off for solid glass objects "
                               "(vases, lenses), which bend the view behind them."));
    glass_->addWide(thinWalled_);
    emissionStrength_ = new ui::SliderField(0.0, 20000.0, 0, tr(" nits"), glass_);
    glass_->addRow(tr("Glow"), emissionStrength_)
        .label->setToolTip(tr("Makes the surface emit light (lamp shades, LED strips, screens), "
                              "in nits: a screen 300, a frosted bulb 50 000. 0 = off."));
    glowSwatch_ = new QToolButton(glass_);
    glowSwatch_->setIconSize(QSize(48, 20));
    glowSwatch_->setCursor(Qt::PointingHandCursor);
    glowSwatch_->setToolTip(tr("Click to change the glow color"));
    auto* glowRow = new QHBoxLayout;
    glowRow->addWidget(glowSwatch_);
    glowRow->addStretch();
    glass_->addRow(tr("Glow color"), glowRow);
    layout->addWidget(glass_);

    // Mapping.
    mapping_ = new ui::Section(tr("Mapping"), page);
    mappingMode_ = new ui::SegmentedControl({tr("UV"), tr("Box")}, mapping_);
    mappingMode_->setToolTip(tr("UV: the model's own texture coordinates from the modeling app.\n"
                                "Box: projected from three sides at real-world size; best for "
                                "CAD models without texture coordinates."));
    mapping_->addWide(mappingMode_);
    scaleX_ = ui::valueField(kMinScale, kMaxScale, 3, QString(), mapping_);
    scaleY_ = ui::valueField(kMinScale, kMaxScale, 3, QString(), mapping_);
    lockAspect_ = new QToolButton(mapping_);
    lockAspect_->setText(tr("Lock"));
    lockAspect_->setCheckable(true);
    lockAspect_->setChecked(true);
    lockAspect_->setToolTip(tr("Keep the X:Y proportion when changing one side"));
    QHBoxLayout* scaleRow = axisPair(scaleX_, scaleY_, mapping_);
    scaleRow->addWidget(lockAspect_);
    scaleLabel_ = mapping_->addRow(tr("Tile size"), scaleRow).label;
    // One click for common sizes; keeps the tile's proportions.
    auto* quick = new QHBoxLayout;
    quick->setSpacing(4);
    for (const double meters : {0.5, 1.0, 2.0, 3.0, 5.0}) {
        // Numbers only, so the row stays narrow; the row label says "m".
        auto* button = new QPushButton(QLocale().toString(meters), mapping_);
        button->setProperty("compact", true);
        button->setMinimumWidth(0);
        button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        button->setToolTip(tr("One texture tile covers %1 m").arg(QLocale().toString(meters)));
        connect(button, &QPushButton::clicked, this, [this, meters] {
            editMaterial([meters](Material& m) {
                const float aspect = m.textureScale.x > 0.0f ? m.textureScale.y / m.textureScale.x
                                                             : 1.0f;
                m.textureScale = glm::vec2(static_cast<float>(meters),
                                           static_cast<float>(meters) * aspect);
            });
        });
        quick->addWidget(button);
    }
    quickSizes_ = mapping_->addRow(tr("Quick size (m)"), quick);
    rotation_ = new ui::SliderField(0.0, 360.0, 0, QStringLiteral("°"), mapping_);
    mapping_->addRow(tr("Rotation"), rotation_)
        .label->setToolTip(tr("Turns the pattern, e.g. to run planks or tiles the other way."));
    variation_ = new ui::SliderField(0.0, 1.0, 2, QString(), mapping_);
    mapping_->addRow(tr("Variation"), variation_)
        .label->setToolTip(tr("Gentle changes in brightness across many tiles, so the texture "
                              "doesn't look like a repeating grid from a distance."));
    randomTiles_ = new QCheckBox(tr("Rotate each tile randomly"), mapping_);
    randomTiles_->setToolTip(tr("Turns and shifts every tile at random, blended softly at the "
                                "tile borders, so no repeat shows. Good for stone, concrete, "
                                "plaster, ground and grass; leave it off for bricks, planks and "
                                "anything with straight joints."));
    mapping_->addWide(randomTiles_);
    offsetX_ = ui::valueField(-1000.0, 1000.0, 2, QString(), mapping_);
    offsetY_ = ui::valueField(-1000.0, 1000.0, 2, QString(), mapping_);
    mapping_->addRow(tr("Offset"), axisPair(offsetX_, offsetY_, mapping_))
        .label->setToolTip(tr("Shifts the pattern, in tiles (0.5 = half a tile)."));
    layout->addWidget(mapping_);

    // Textures in the project.
    auto* textures = new ui::Section(tr("Textures in project"), page);
    textures_ = new TextureGrid(*thumbnails_, TextureGrid::Detail::Full, 3, textures);
    textures->addWide(textures_);
    textureCount_ = ui::label(QString(), "muted", textures);
    textureCount_->setWordWrap(true);
    removeUnused_ = new QPushButton(theme::icon(QStringLiteral("trash-2")),
                                    tr("Remove unused textures"), textures);
    removeUnused_->setProperty("compact", true);
    removeUnused_->setToolTip(tr("Remove textures that no material uses, to save memory"));
    auto* textureActions = new QHBoxLayout;
    textureActions->setSpacing(8);
    textureActions->addWidget(textureCount_, 1);
    textureActions->addWidget(removeUnused_);
    textures->addWide(textureActions);
    layout->addWidget(textures);
    layout->addStretch();

    connect(create_, &QPushButton::clicked, this, &MaterialsPanel::createMaterial);
    connect(fromTextures_, &QPushButton::clicked, this, &MaterialsPanel::createFromTextures);
    connect(library_, &QPushButton::clicked, this, &MaterialsPanel::openTextureLibrary);
    connect(apply_, &QPushButton::clicked, this, &MaterialsPanel::applyToSelection);
    connect(selectUsers_, &QPushButton::clicked, this, &MaterialsPanel::selectUsers);
    connect(delete_, &QPushButton::clicked, this, &MaterialsPanel::deleteMaterial);
    connect(list_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (!updating_) {
            showMaterial(idOf(item));
        }
    });
    connect(name_, &QLineEdit::editingFinished, this, &MaterialsPanel::applyName);
    connect(swatch_, &QToolButton::clicked, this, &MaterialsPanel::pickColor);
    connect(transmission_, &ui::SliderField::valueEdited, this, [this](double value) {
        editMaterial([value](Material& m) { m.transmission = static_cast<float>(value); });
    });
    connect(ior_, &ui::SliderField::valueEdited, this, [this](double value) {
        editMaterial([value](Material& m) { m.ior = static_cast<float>(value); });
    });
    connect(thinWalled_, &QCheckBox::clicked, this, [this](bool checked) {
        editMaterial([checked](Material& m) { m.thinWalled = checked; });
    });
    connect(emissionStrength_, &ui::SliderField::valueEdited, this, [this](double value) {
        editMaterial([value](Material& m) { m.emissionStrength = static_cast<float>(value); });
    });
    connect(glowSwatch_, &QToolButton::clicked, this, [this] {
        const MaterialResource* resource = scene_.material(current_);
        if (resource == nullptr) {
            return;
        }
        const QColor chosen = QColorDialog::getColor(
            ui::displayColor(resource->material.emissionColor), this, tr("Glow color"));
        if (chosen.isValid()) {
            editMaterial([&](Material& m) { m.emissionColor = ui::linearColor(chosen); });
        }
    });
    const std::pair<TextureSlotWidget*, TextureSlot Material::*> maps[] = {
        {baseColorMap_, &Material::baseColorMap}, {roughnessMap_, &Material::roughnessMap},
        {metalnessMap_, &Material::metalnessMap}, {normalMap_, &Material::normalMap},
        {occlusionMap_, &Material::occlusionMap}, {heightMap_, &Material::heightMap},
    };
    for (const auto& [widget, map] : maps) {
        connect(widget, &TextureSlotWidget::slotEdited, this,
                [this, map](const TextureSlot& edited) { editSlot(map, edited); });
    }
    connect(roughness_, &ui::SliderField::valueEdited, this, [this](double value) {
        editMaterial([value](Material& m) { m.roughness = static_cast<float>(value); });
    });
    connect(metalness_, &ui::SliderField::valueEdited, this, [this](double value) {
        editMaterial([value](Material& m) { m.metalness = static_cast<float>(value); });
    });
    connect(normalStrength_, &ui::SliderField::valueEdited, this, [this](double value) {
        editMaterial([value](Material& m) { m.normalStrength = static_cast<float>(value); });
    });
    connect(normalFlipY_, &QCheckBox::clicked, this, [this](bool checked) {
        editMaterial([checked](Material& m) { m.normalFlipY = checked; });
    });
    connect(heightDepth_, &ui::SliderField::valueEdited, this, [this](double value) {
        editMaterial([value](Material& m) { m.heightDepth = static_cast<float>(value); });
    });
    connect(mappingMode_, &ui::SegmentedControl::currentEdited, this, [this](int index) {
        editMaterial([index](Material& m) {
            m.mapping = index == 1 ? TextureMapping::Box : TextureMapping::UV;
        });
    });
    connect(scaleX_, &QDoubleSpinBox::valueChanged, this,
            [this](double value) { editScale(0, value); });
    connect(scaleY_, &QDoubleSpinBox::valueChanged, this,
            [this](double value) { editScale(1, value); });
    connect(rotation_, &ui::SliderField::valueEdited, this, [this](double value) {
        editMaterial(
            [value](Material& m) { m.textureRotationDegrees = static_cast<float>(value); });
    });
    connect(variation_, &ui::SliderField::valueEdited, this, [this](double value) {
        editMaterial([value](Material& m) { m.variation = static_cast<float>(value); });
    });
    connect(randomTiles_, &QCheckBox::clicked, this, [this](bool checked) {
        editMaterial([checked](Material& m) { m.randomTileRotation = checked; });
    });
    connect(offsetX_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        editMaterial([value](Material& m) { m.textureOffset.x = static_cast<float>(value); });
    });
    connect(offsetY_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        editMaterial([value](Material& m) { m.textureOffset.y = static_cast<float>(value); });
    });
    connect(textures_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        showTexturePreview(scene_, TextureGrid::idOf(item), this);
    });
    connect(removeUnused_, &QPushButton::clicked, this, &MaterialsPanel::removeUnusedTextures);
    connect(thumbnails_, &TextureThumbnails::thumbnailReady, this, [this](quint64 texture) {
        for (const auto& [id, item] : materialItems_) {
            const MaterialResource* material = scene_.material(id);
            if (material != nullptr && material->material.baseColorMap.texture.value == texture) {
                updateMaterialItem(item, *material);
            }
        }
    });

    sceneListener_ = scene_.addListener([this](const SceneChange& change) { onSceneChange(change); });
    selectionListener_ = selection_.addListener([this] {
        followSelection();
        updateActions();
    });
    rebuildMaterialList();
    rebuildTextures();
    followSelection();
    showMaterial(current_);
}

MaterialsPanel::~MaterialsPanel() {
    if (cancelImport_) {
        cancelImport_->store(true);
    }
    scene_.removeListener(sceneListener_);
    selection_.removeListener(selectionListener_);
}

void MaterialsPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (dirty_ != 0) {
        flush(); // changes that arrived while another mode was shown
    }
}

// --- Refresh from the document -------------------------------------------------------------------

void MaterialsPanel::onSceneChange(const SceneChange& change) {
    const ResourceId id = change.resource;
    switch (change.kind) {
    case SceneChange::Kind::ResourceAdded:
        if (scene_.material(id) != nullptr) {
            markDirty(kMaterialList | kTextureUsage); // it may use existing textures
        } else if (scene_.texture(id) != nullptr) {
            markDirty(kTextureGrid);
        }
        break;
    case SceneChange::Kind::ResourceRemoved:
        if (materialItems_.contains(id) || id == current_) {
            markDirty(kMaterialList | kTextureUsage | kEditor);
        } else if (textureIds_.contains(id)) {
            markDirty(kTextureGrid);
        }
        break;
    case SceneChange::Kind::ResourceChanged:
        if (scene_.material(id) != nullptr) {
            changedMaterials_.insert(id);
            markDirty(kMaterialItems | kTextureUsage | (id == current_ ? kEditor : 0u));
        } else if (scene_.texture(id) != nullptr) {
            markDirty(kTextureGrid | kEditor); // renamed: names in the grid and the map slots
        }
        break;
    case SceneChange::Kind::MeshInstanceChanged:
    case SceneChange::Kind::EntityAdded:
    case SceneChange::Kind::EntityRemoved:
        markDirty(kUsage);
        break;
    default:
        break;
    }
}

void MaterialsPanel::markDirty(unsigned flags) {
    dirty_ |= flags;
    // Hidden panels catch up in showEvent; visible ones once per event-loop turn, so an import
    // adding thousands of objects or a dragged slider costs one refresh, not one per change.
    if (!flushScheduled_ && isVisible()) {
        flushScheduled_ = true;
        QTimer::singleShot(0, this, &MaterialsPanel::flush);
    }
}

void MaterialsPanel::flush() {
    flushScheduled_ = false;
    unsigned dirty = std::exchange(dirty_, 0u);
    const auto changed = std::exchange(changedMaterials_, {});
    if ((dirty & kMaterialList) == 0 && (dirty & kMaterialItems) != 0) {
        for (ResourceId id : changed) {
            const auto item = materialItems_.find(id);
            const MaterialResource* material = scene_.material(id);
            if (item == materialItems_.end() || material == nullptr) {
                continue;
            }
            if (item->second->text(0) != QString::fromStdString(material->name)) {
                dirty |= kMaterialList; // renamed: the list order changes
                break;
            }
            updateMaterialItem(item->second, *material);
        }
    }
    if ((dirty & kMaterialList) != 0) {
        rebuildMaterialList();
    } else if ((dirty & kUsage) != 0) {
        updateMaterialUsage();
    }
    if ((dirty & kTextureGrid) != 0) {
        rebuildTextures();
    } else if ((dirty & kTextureUsage) != 0) {
        refreshTextureUsage();
    }
    if ((dirty & (kEditor | kMaterialList)) != 0) {
        showMaterial(current_);
    } else if ((dirty & kUsage) != 0) {
        updateActions();
    }
}

void MaterialsPanel::rebuildMaterialList() {
    std::vector<const MaterialResource*> materials;
    scene_.forEachMaterial([&](const MaterialResource& m) { materials.push_back(&m); });
    std::sort(materials.begin(), materials.end(), [](const auto* a, const auto* b) {
        return a->name != b->name ? a->name < b->name : a->id < b->id;
    });
    materialUsage_ = materialUsage(scene_);

    QList<QTreeWidgetItem*> items;
    items.reserve(static_cast<qsizetype>(materials.size()));
    materialItems_.clear();
    const QColor muted = theme::palette().textMuted;
    for (const MaterialResource* material : materials) {
        auto* item = new QTreeWidgetItem;
        item->setData(0, kIdRole, QVariant::fromValue<qulonglong>(material->id.value));
        item->setForeground(1, muted);
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        updateMaterialItem(item, *material);
        setUsage(item, usageOf(materialUsage_, material->id));
        materialItems_.emplace(material->id, item);
        items.push_back(item);
    }
    updating_ = true;
    list_->setUpdatesEnabled(false);
    list_->clear();
    list_->addTopLevelItems(items);
    selectCurrentItem();
    list_->setUpdatesEnabled(true);
    updating_ = false;
    fitMaterialList();
}

void MaterialsPanel::updateMaterialItem(QTreeWidgetItem* item, const MaterialResource& resource) {
    const QString name = QString::fromStdString(resource.name);
    if (item->text(0) != name) {
        item->setText(0, name);
    }
    // The icon is redrawn only when what it shows changed, not on every roughness slider step.
    const QColor color = ui::displayColor(resource.material.baseColor);
    const ResourceId texture = resource.material.baseColorMap.texture;
    const QPixmap thumbnail = texture ? thumbnails_->thumbnailLater(texture) : QPixmap();
    const QString key = QStringLiteral("%1:%2:%3")
                            .arg(texture.value)
                            .arg(color.rgba())
                            .arg(thumbnail.isNull() ? 0 : 1);
    if (item->data(0, kIconKeyRole).toString() == key) {
        return;
    }
    item->setData(0, kIconKeyRole, key);
    item->setIcon(0, thumbnail.isNull()
                         ? ui::colorSwatch(color, QSize(kListIconSize, kListIconSize))
                         : thumbnailIcon(tinted(thumbnail, color)));
}

void MaterialsPanel::updateMaterialUsage() {
    materialUsage_ = materialUsage(scene_);
    for (const auto& [id, item] : materialItems_) {
        setUsage(item, usageOf(materialUsage_, id));
    }
}

void MaterialsPanel::fitMaterialList() {
    const int count = list_->topLevelItemCount();
    const int rowHeight =
        std::max(count > 0 ? list_->sizeHintForRow(0) : 0, kListIconSize + 6);
    const int rows = std::clamp(count, kMinListRows, kMaxListRows);
    list_->setFixedHeight(rows * rowHeight + 2 * list_->frameWidth() + 2);
}

void MaterialsPanel::selectCurrentItem() {
    const bool wasUpdating = std::exchange(updating_, true);
    if (const auto it = materialItems_.find(current_); it != materialItems_.end()) {
        list_->setCurrentItem(it->second);
        list_->scrollToItem(it->second);
    } else {
        list_->clearSelection();
        list_->setCurrentItem(nullptr);
    }
    updating_ = wasUpdating;
}

void MaterialsPanel::rebuildTextures() {
    textures_->rebuild(scene_);
    textureIds_.clear();
    scene_.forEachTexture([&](const TextureResource& texture) { textureIds_.insert(texture.id); });
    textureUsage_ = textureUsage(scene_);
    textures_->setVisible(!textureIds_.empty());
    updateTextureActions();
}

void MaterialsPanel::refreshTextureUsage() {
    auto usage = textureUsage(scene_);
    if (usage == textureUsage_) {
        return; // the usual case while editing values of one material
    }
    textureUsage_ = std::move(usage);
    textures_->updateUsage(scene_);
    updateTextureActions();
}

void MaterialsPanel::updateTextureActions() {
    const int total = static_cast<int>(textureIds_.size());
    int unused = 0;
    for (ResourceId id : textureIds_) {
        unused += textureUsage_.contains(id) ? 0 : 1;
    }
    if (total == 0) {
        textureCount_->setText(tr("No textures yet"));
        textureCount_->setToolTip(tr("Drop an image on a map above, use From textures…, or import "
                                     "a model with textures"));
    } else {
        textureCount_->setText(unused == 0 ? ui::count(total, tr("1 texture"), tr("%1 textures"))
                                           : tr("%1, %2 unused")
                                                 .arg(ui::count(total, tr("1 texture"),
                                                                tr("%1 textures")))
                                                 .arg(unused));
        textureCount_->setToolTip(tr("Double-click a texture to preview it; drag it onto a map "
                                     "to use it"));
    }
    removeUnused_->setVisible(unused > 0);
}

// --- Selected material -----------------------------------------------------------------------------

void MaterialsPanel::showMaterial(ResourceId id) {
    const MaterialResource* resource = scene_.material(id);
    current_ = resource != nullptr ? id : ResourceId{};
    if (current_ != shown_) {
        // An image still decoding for the previous material must not land on this one.
        for (TextureSlotWidget* map :
             {baseColorMap_, roughnessMap_, metalnessMap_, normalMap_, occlusionMap_, heightMap_}) {
            map->cancelLoading();
        }
        shown_ = current_;
    }
    for (QWidget* section : {static_cast<QWidget*>(base_), static_cast<QWidget*>(surface_),
                             static_cast<QWidget*>(relief_), static_cast<QWidget*>(occlusion_),
                             static_cast<QWidget*>(glass_), static_cast<QWidget*>(mapping_)}) {
        section->setVisible(resource != nullptr);
    }
    noMaterial_->setVisible(resource == nullptr);
    if (resource == nullptr) {
        noMaterial_->setText(materialItems_.empty()
                                 ? tr("No materials yet: create one above or import a model.")
                                 : tr("Pick a material above, or select an object in the scene."));
        updateActions();
        return;
    }

    const Material& m = resource->material;
    if (!name_->hasFocus()) {
        name_->setText(QString::fromStdString(resource->name));
    }
    swatch_->setIcon(ui::colorSwatch(ui::displayColor(m.baseColor), QSize(48, 20)));
    baseColorMap_->setSlot(m.baseColorMap);
    roughness_->setValue(m.roughness);
    roughnessMap_->setSlot(m.roughnessMap);
    metalness_->setValue(m.metalness);
    metalnessMap_->setSlot(m.metalnessMap);
    normalMap_->setSlot(m.normalMap);
    normalStrength_->setValue(m.normalStrength);
    normalFlipY_->setChecked(m.normalFlipY);
    occlusionMap_->setSlot(m.occlusionMap);
    heightMap_->setSlot(m.heightMap);
    heightDepth_->setValue(static_cast<double>(m.heightDepth));
    transmission_->setValue(static_cast<double>(m.transmission));
    ior_->setValue(static_cast<double>(m.ior));
    thinWalled_->setChecked(m.thinWalled);
    emissionStrength_->setValue(static_cast<double>(m.emissionStrength));
    glowSwatch_->setIcon(ui::colorSwatch(ui::displayColor(m.emissionColor), QSize(48, 20)));
    showMapping(m);
    updateActions();
}

void MaterialsPanel::showMapping(const Material& m) {
    const bool box = m.mapping == TextureMapping::Box;
    mappingMode_->setCurrent(box ? 1 : 0);
    const QString suffix = box ? tr(" m") : QStringLiteral("×");
    for (QDoubleSpinBox* field : {scaleX_, scaleY_}) {
        if (field->suffix() != suffix) {
            field->setSuffix(suffix);
        }
    }
    scaleLabel_->setText(box ? tr("Tile size") : tr("Repeat"));
    quickSizes_.setVisible(box);
    scaleLabel_->setToolTip(box ? tr("Real-world size of one texture tile, e.g. 0.6 m for a "
                                     "paving slab photo.")
                                : tr("How often the texture repeats per unit of the model's "
                                     "texture coordinates."));
    setField(scaleX_, static_cast<double>(m.textureScale.x));
    setField(scaleY_, static_cast<double>(m.textureScale.y));
    rotation_->setValue(static_cast<double>(m.textureRotationDegrees));
    variation_->setValue(static_cast<double>(m.variation));
    randomTiles_->setChecked(m.randomTileRotation);
    setField(offsetX_, static_cast<double>(m.textureOffset.x));
    setField(offsetY_, static_cast<double>(m.textureOffset.y));
}

void MaterialsPanel::followSelection() {
    // The primary selection, or the first object inside it when a group is selected.
    const EntityId primary = selection_.primary();
    ResourceId material;
    for (const EntityId id : meshEntitiesIn(scene_, std::span<const EntityId>(&primary, 1))) {
        if (const Entity* entity = scene_.entity(id); entity->meshInstance->material) {
            material = entity->meshInstance->material;
            break;
        }
    }
    if (!material || material == current_ || scene_.material(material) == nullptr) {
        return;
    }
    current_ = material;
    selectCurrentItem();
    showMaterial(material);
}

void MaterialsPanel::updateActions() {
    const auto selectedObjects = static_cast<int>(meshEntitiesIn(scene_, selection_.ids()).size());
    const bool hasMaterial = static_cast<bool>(current_);
    apply_->setEnabled(hasMaterial && selectedObjects > 0);
    apply_->setText(selectedObjects > 1 ? tr("Apply to %1 selected").arg(selectedObjects)
                                        : tr("Apply to selected"));
    apply_->setToolTip(selectedObjects > 0
                           ? tr("Give the selected objects this material")
                           : tr("First select objects in the scene or in the Scene panel"));
    const int users = hasMaterial ? usageOf(materialUsage_, current_) : 0;
    selectUsers_->setEnabled(users > 0);
    delete_->setEnabled(hasMaterial && users == 0);
    delete_->setToolTip(!hasMaterial ? tr("Delete material")
                        : users == 0 ? tr("Delete this unused material")
                                     : tr("Only materials no object uses can be deleted"));
}

// --- Edits ---------------------------------------------------------------------------------------

void MaterialsPanel::editMaterial(const std::function<void(Material&)>& change) {
    const MaterialResource* resource = scene_.material(current_);
    if (resource == nullptr) {
        return;
    }
    Material edited = resource->material;
    change(edited);
    scene_.setMaterial(current_, edited);
}

void MaterialsPanel::editSlot(TextureSlot Material::*map, const TextureSlot& slot) {
    if (slot.texture && scene_.texture(slot.texture) == nullptr) {
        return; // removed meanwhile
    }
    editMaterial([&](Material& m) {
        const bool added = !(m.*map) && static_cast<bool>(slot);
        m.*map = slot;
        // Values multiply their map: start from neutral so the new map shows as authored.
        if (added && map == &Material::baseColorMap) {
            m.baseColor = glm::vec3(1.0f);
        } else if (added && map == &Material::roughnessMap) {
            m.roughness = 1.0f;
        } else if (added && map == &Material::metalnessMap) {
            m.metalness = 1.0f;
        }
    });
}

void MaterialsPanel::editScale(int axis, double value) {
    editMaterial([&](Material& m) {
        const float size = static_cast<float>(value);
        glm::vec2 scale = m.textureScale;
        const int other = 1 - axis;
        if (lockAspect_->isChecked() && scale[axis] > 0.0f) {
            scale[other] = std::clamp(scale[other] * size / scale[axis],
                                      static_cast<float>(kMinScale), static_cast<float>(kMaxScale));
        }
        scale[axis] = size;
        m.textureScale = scale;
    });
}

void MaterialsPanel::applyName() {
    const QString name = name_->text().trimmed();
    if (!current_ || name.isEmpty()) {
        showMaterial(current_);
        return;
    }
    scene_.setResourceName(current_, name.toStdString());
}

void MaterialsPanel::pickColor() {
    const MaterialResource* resource = scene_.material(current_);
    if (resource == nullptr) {
        return;
    }
    const QColor chosen = QColorDialog::getColor(ui::displayColor(resource->material.baseColor),
                                                 this, tr("Material color"));
    if (chosen.isValid()) {
        editMaterial([&](Material& m) { m.baseColor = ui::linearColor(chosen); });
    }
}

void MaterialsPanel::applyPreset(int index) {
    const SurfacePreset& preset = kSurfacePresets[static_cast<std::size_t>(index)];
    editMaterial([&](Material& m) {
        m.roughness = preset.roughness;
        m.metalness = preset.metalness;
    });
}

void MaterialsPanel::selectUsers() {
    std::vector<EntityId> users;
    scene_.traverse([&](const Entity& entity, const glm::mat4&) {
        if (entity.meshInstance && entity.meshInstance->material == current_) {
            users.push_back(entity.id);
        }
    });
    selection_.set(users);
}

void MaterialsPanel::applyToSelection() {
    if (!current_) {
        return;
    }
    applyMaterial(scene_, selection_.ids(), current_);
}

void MaterialsPanel::createMaterial() {
    notice_->hide();
    std::unordered_set<std::string> names;
    scene_.forEachMaterial([&](const MaterialResource& m) { names.insert(m.name); });
    std::string name;
    for (std::size_t n = names.size() + 1; name.empty() || names.contains(name); ++n) {
        name = tr("Material %1").arg(n).toStdString();
    }
    current_ = scene_.addMaterial(std::move(name), Material{});
    flush(); // show it now, so the name can be typed right away
    base_->setExpanded(true);
    name_->setFocus();
    name_->selectAll();
}

void MaterialsPanel::createFromTextures() {
    if (cancelImport_) {
        cancelImport_->store(true); // the button reads "Cancel" while a set is loading
        return;
    }
    static QString lastDirectory;
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Choose the images of a texture set, or a downloaded pack (.zip)"), lastDirectory,
        textureSetFilter());
    if (files.isEmpty()) {
        return;
    }
    lastDirectory = QFileInfo(files.first()).absolutePath();
    notice_->hide();

    // A pack (zip) is one material by itself; loose images together make one material.
    std::vector<std::filesystem::path> paths;
    for (const QString& file : files) {
        const bool zip = file.endsWith(QLatin1String(".zip"), Qt::CaseInsensitive);
        if (zip) {
            paths.assign(1, std::filesystem::path(file.toStdWString()));
            break;
        }
        paths.emplace_back(file.toStdWString());
    }
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    cancelImport_ = cancel;
    setImporting(true);
    QPointer<QProgressBar> bar(progress_);
    import::Progress progress = [bar](float fraction, std::string_view) {
        const int percent = static_cast<int>(std::lround(fraction * 100.0f));
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [bar, percent] {
                if (bar) {
                    bar->setValue(percent);
                }
            },
            Qt::QueuedConnection);
    };

    runInBackground<import::ImportedModel>(
        this,
        [paths, progress, cancel] {
            return import::importTextureSet(paths, progress, cancel.get());
        },
        [this](import::ImportedModel model) {
            cancelImport_.reset();
            setImporting(false);
            ResourceId material;
            try {
                material = import::addMaterialToScene(scene_, model);
            } catch (const std::exception& e) {
                QMessageBox::warning(this, tr("Could not create the material"),
                                     QString::fromUtf8(e.what()));
                return;
            }
            current_ = material;
            flush();
            if (!model.warnings.empty()) {
                QStringList lines;
                for (const std::string& warning : model.warnings) {
                    lines << QString::fromStdString(warning);
                }
                notice_->setText(lines.size() == 1
                                     ? lines.first()
                                     : tr("%1 (+%2 more)").arg(lines.first()).arg(lines.size() - 1));
                notice_->setToolTip(lines.join(QLatin1Char('\n')));
                notice_->show();
            }
        },
        [this](std::exception_ptr error) {
            cancelImport_.reset();
            setImporting(false);
            try {
                std::rethrow_exception(error);
            } catch (const import::ImportCancelled&) {
                return;
            } catch (...) {
                QMessageBox::warning(this, tr("Could not load the texture set"),
                                     errorText(std::current_exception()));
            }
        });
}

void MaterialsPanel::openTextureLibrary() {
    if (!browser_) {
        browser_ = new TextureBrowser(scene_, selection_, window());
        browser_->setAttribute(Qt::WA_DeleteOnClose);
        connect(browser_, &TextureBrowser::materialAdded, this,
                [this](ResourceId material, const QString& message) {
                    current_ = material;
                    flush();
                    notice_->setText(message);
                    notice_->setToolTip(message);
                    notice_->show();
                });
    }
    browser_->show();
    browser_->raise();
    browser_->activateWindow();
}

void MaterialsPanel::setImporting(bool importing) {
    fromTextures_->setIcon(theme::icon(importing ? QStringLiteral("x")
                                                 : QStringLiteral("image-plus")));
    fromTextures_->setText(importing ? tr("Cancel loading") : tr("From textures…"));
    progress_->setValue(0);
    progress_->setVisible(importing);
}

void MaterialsPanel::deleteMaterial() {
    if (current_ && scene_.userCount(current_) == 0) {
        const ResourceId doomed = current_;
        current_ = {};
        scene_.removeResource(doomed);
    }
}

void MaterialsPanel::removeUnusedTextures() {
    const auto usage = textureUsage(scene_);
    std::vector<ResourceId> unused;
    scene_.forEachTexture([&](const TextureResource& texture) {
        if (!usage.contains(texture.id)) {
            unused.push_back(texture.id);
        }
    });
    if (unused.empty()) {
        return;
    }
    const int count = static_cast<int>(unused.size());
    const auto answer = QMessageBox::question(
        this, tr("Remove unused textures"),
        tr("Remove %1 that no material uses?")
            .arg(ui::count(count, tr("1 texture"), tr("%1 textures"))));
    if (answer != QMessageBox::Yes) {
        return;
    }
    for (ResourceId id : unused) {
        if (scene_.texture(id) != nullptr && scene_.userCount(id) == 0) {
            scene_.removeResource(id);
        }
    }
}

} // namespace lb::app
