#include "app/render_panel.hpp"

#include <array>

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

#include "app/theme.hpp"
#include "app/ui_helpers.hpp"
#include "app/viewport_window.hpp"
#include "core/log.hpp"

namespace lb::app {
namespace {

struct LookPreset {
    const char* name;
    Look look;
};

// Starting points; every value stays adjustable below.
const std::array kLookPresets = {
    LookPreset{"Natural", Look{}},
    LookPreset{"Bright", Look{0.6f, ToneMapping::Neutral, 0.95f, 1.05f, 0.0f, 0.05f, false}},
    LookPreset{"Soft", Look{0.3f, ToneMapping::Filmic, 0.8f, 0.9f, 0.1f, 0.1f, false}},
    LookPreset{"Dramatic", Look{-0.3f, ToneMapping::Filmic, 1.3f, 1.15f, 0.0f, 0.35f, false}},
    LookPreset{"Warm", Look{0.1f, ToneMapping::Filmic, 1.05f, 1.1f, 0.45f, 0.2f, false}},
    LookPreset{"Clay", Look{0.3f, ToneMapping::Neutral, 1.0f, 1.0f, 0.0f, 0.1f, true}},
};

constexpr std::array kLenses = {18, 24, 35, 50, 85};

} // namespace

RenderPanel::RenderPanel(Scene& scene, ViewportWindow& viewport, QWidget* parent)
    : QWidget(parent), scene_(scene), viewport_(viewport) {
    QVBoxLayout* layout = ui::scrollPage(this);
    QWidget* page = layout->parentWidget();

    auto* presetGrid = new QGridLayout;
    presetGrid->setSpacing(6);
    for (int i = 0; i < static_cast<int>(kLookPresets.size()); ++i) {
        auto* button = new QPushButton(tr(kLookPresets[i].name), page);
        button->setProperty("compact", true);
        const Look look = kLookPresets[static_cast<std::size_t>(i)].look;
        connect(button, &QPushButton::clicked, this, [this, look] { scene_.setLook(look); });
        presetGrid->addWidget(button, i / 3, i % 3);
    }
    layout->addLayout(presetGrid);
    layout->addSpacing(4);

    // Camera.
    auto* camera = new ui::Section(tr("Camera"), page);
    lens_ = new ui::SliderField(12.0, 200.0, 0, tr(" mm"), camera);
    camera->addRow(tr("Lens"), lens_)
        .label->setToolTip(tr("Focal length (full-frame equivalent). Short lenses show more of a "
                              "room; long lenses flatten perspective."));
    auto* lensRow = new QHBoxLayout;
    lensRow->setSpacing(4);
    for (int mm : kLenses) {
        auto* button = new QPushButton(QString::number(mm), camera);
        button->setProperty("compact", true);
        button->setToolTip(tr("%1 mm lens").arg(mm));
        connect(button, &QPushButton::clicked, this,
                [this, mm] { viewport_.setFocalLength(static_cast<float>(mm)); });
        lensRow->addWidget(button);
    }
    camera->addRow(QString(), lensRow);
    twoPoint_ = new QCheckBox(tr("Two-point perspective"), camera);
    twoPoint_->setToolTip(tr("Keeps vertical lines vertical, like an architectural shift lens."));
    camera->addWide(twoPoint_);
    auto* cameraButtons = new QHBoxLayout;
    cameraButtons->setSpacing(6);
    auto* eyeLevel = new QPushButton(theme::icon(QStringLiteral("eye")), tr("Eye level"), camera);
    eyeLevel->setProperty("compact", true);
    eyeLevel->setToolTip(tr("Stand at 1.6 m and look straight ahead."));
    auto* frameAll = new QPushButton(theme::icon(QStringLiteral("scan")), tr("Frame all"), camera);
    frameAll->setProperty("compact", true);
    cameraButtons->addWidget(eyeLevel);
    cameraButtons->addWidget(frameAll);
    camera->addWide(cameraButtons);
    layout->addWidget(camera);

    // Image adjustments.
    auto* image = new ui::Section(tr("Image"), page);
    exposure_ = new ui::SliderField(-4.0, 4.0, 1, tr(" EV"), image);
    image->addRow(tr("Exposure"), exposure_)
        .label->setToolTip(tr("Overall brightness in camera stops: +1 doubles the light."));
    toneMapping_ = new QComboBox(image);
    toneMapping_->addItem(tr("Filmic"));
    toneMapping_->addItem(tr("Neutral"));
    toneMapping_->addItem(tr("None"));
    toneMapping_->setToolTip(tr("Filmic: rich contrast, soft highlights. Neutral: true material "
                                "colors. None: no highlight compression."));
    image->addRow(tr("Tone"), toneMapping_);
    contrast_ = new ui::SliderField(0.5, 1.5, 2, QString(), image);
    image->addRow(tr("Contrast"), contrast_);
    saturation_ = new ui::SliderField(0.0, 2.0, 2, QString(), image);
    image->addRow(tr("Saturation"), saturation_);
    warmth_ = new ui::SliderField(-1.0, 1.0, 2, QString(), image);
    image->addRow(tr("Warmth"), warmth_)
        .label->setToolTip(tr("White balance: negative is cooler (bluer), positive warmer."));
    vignette_ = new ui::SliderField(0.0, 1.0, 2, QString(), image);
    image->addRow(tr("Vignette"), vignette_);
    clay_ = new QCheckBox(tr("Clay model (ignore materials)"), image);
    image->addWide(clay_);
    layout->addWidget(image);

    // Output.
    auto* output = new ui::Section(tr("Save image"), page);
    auto* save = new QPushButton(theme::icon(QStringLiteral("image")), tr("Save image…"), output);
    save->setProperty("accent", true);
    auto* copy = new QPushButton(theme::icon(QStringLiteral("copy")), tr("Copy"), output);
    auto* outputButtons = new QHBoxLayout;
    outputButtons->setSpacing(6);
    outputButtons->addWidget(save, 1);
    outputButtons->addWidget(copy);
    output->addWide(outputButtons);
    status_ = ui::label(tr("Saves exactly what the viewport shows, at its size."), "muted", output);
    status_->setWordWrap(true);
    output->addWide(status_);
    layout->addWidget(output);
    layout->addStretch(1);

    connect(lens_, &ui::SliderField::valueEdited, this,
            [this](double mm) { viewport_.setFocalLength(static_cast<float>(mm)); });
    connect(twoPoint_, &QCheckBox::toggled, this, [this](bool on) {
        if (!updating_) {
            viewport_.setTwoPointPerspective(on);
        }
    });
    connect(eyeLevel, &QPushButton::clicked, this, [this] { viewport_.moveToEyeLevel(); });
    connect(frameAll, &QPushButton::clicked, this, [this] { viewport_.frameScene(); });
    connect(&viewport_, &ViewportWindow::cameraChanged, this, &RenderPanel::refreshCamera);

    const auto connectSlider = [this](ui::SliderField* field, auto apply) {
        connect(field, &ui::SliderField::valueEdited, this, [this, apply](double value) {
            editLook([&](Look& look) { apply(look, static_cast<float>(value)); });
        });
    };
    connectSlider(exposure_, [](Look& l, float v) { l.exposureEv = v; });
    connectSlider(contrast_, [](Look& l, float v) { l.contrast = v; });
    connectSlider(saturation_, [](Look& l, float v) { l.saturation = v; });
    connectSlider(warmth_, [](Look& l, float v) { l.warmth = v; });
    connectSlider(vignette_, [](Look& l, float v) { l.vignette = v; });
    connect(toneMapping_, &QComboBox::currentIndexChanged, this, [this](int index) {
        editLook([index](Look& l) { l.toneMapping = static_cast<ToneMapping>(index); });
    });
    connect(clay_, &QCheckBox::toggled, this,
            [this](bool on) { editLook([on](Look& l) { l.clay = on; }); });
    connect(save, &QPushButton::clicked, this, &RenderPanel::saveImage);
    connect(copy, &QPushButton::clicked, this, &RenderPanel::copyImage);

    listener_ = scene_.addListener([this](const SceneChange& change) {
        if (change.kind == SceneChange::Kind::LookChanged) {
            refreshLook();
        }
    });
    refreshLook();
    refreshCamera();
}

RenderPanel::~RenderPanel() {
    scene_.removeListener(listener_);
}

void RenderPanel::editLook(const std::function<void(Look&)>& change) {
    if (updating_) {
        return;
    }
    Look look = scene_.look();
    change(look);
    scene_.setLook(look);
}

void RenderPanel::refreshLook() {
    const Look& look = scene_.look();
    updating_ = true;
    exposure_->setValue(look.exposureEv);
    toneMapping_->setCurrentIndex(static_cast<int>(look.toneMapping));
    contrast_->setValue(look.contrast);
    saturation_->setValue(look.saturation);
    warmth_->setValue(look.warmth);
    vignette_->setValue(look.vignette);
    clay_->setChecked(look.clay);
    updating_ = false;
}

void RenderPanel::refreshCamera() {
    updating_ = true;
    lens_->setValue(viewport_.camera().focalLength());
    twoPoint_->setChecked(viewport_.camera().twoPointPerspective());
    updating_ = false;
}

QImage RenderPanel::capture() {
    QImage image = viewport_.grabFrame();
    if (image.isNull()) {
        status_->setText(tr("The viewport could not produce an image right now."));
    }
    return image;
}

void RenderPanel::saveImage() {
    const QImage image = capture();
    if (image.isNull()) {
        return;
    }
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save image"), QDir(folder).filePath(QStringLiteral("LightBox render.png")),
        tr("PNG image (*.png);;JPEG image (*.jpg *.jpeg)"));
    if (path.isEmpty()) {
        return;
    }
    const bool jpeg = QFileInfo(path).suffix().startsWith(QLatin1String("jp"), Qt::CaseInsensitive);
    if (image.save(path, nullptr, jpeg ? 95 : -1)) {
        status_->setText(tr("Saved %1 × %2 px to %3")
                             .arg(image.width())
                             .arg(image.height())
                             .arg(QDir::toNativeSeparators(path)));
        log::info("Saved viewport image {}", path.toStdString());
    } else {
        status_->setText(tr("Could not write %1").arg(QDir::toNativeSeparators(path)));
    }
}

void RenderPanel::copyImage() {
    const QImage image = capture();
    if (image.isNull()) {
        return;
    }
    QGuiApplication::clipboard()->setImage(image);
    status_->setText(tr("Copied %1 × %2 px image to the clipboard.")
                         .arg(image.width())
                         .arg(image.height()));
}

} // namespace lb::app
