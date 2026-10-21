#include "app/render_panel.hpp"

#include <algorithm>
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
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
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

    auto* renderImage =
        new QPushButton(theme::icon(QStringLiteral("sparkles")), tr("Render image…"), page);
    renderImage->setProperty("accent", true);
    renderImage->setToolTip(tr("Open the render screen: make the final ray-traced image of this "
                               "view in HD, Full HD or 4K and save it (Ctrl+Shift+R)."));
    connect(renderImage, &QPushButton::clicked, this, &RenderPanel::renderScreenRequested);
    layout->addWidget(renderImage);
    layout->addSpacing(6);

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

    // Ray tracing: the photoreal view (D-027).
    auto* rays = new ui::Section(tr("Ray tracing"), page);
    rayTraced_ = new QCheckBox(tr("Ray traced (photoreal)"), rays);
    rayTraced_->setToolTip(tr("Real reflections, glass, soft shadows and light bouncing around "
                              "rooms. The image refines while you watch; moving the camera shows "
                              "the quick preview, and it continues once you stop."));
    rays->addWide(rayTraced_);
    rayQuality_ = new ui::SegmentedControl({tr("Draft"), tr("Good"), tr("Final"), tr("Ultra")},
                                           rays);
    rays->addRow(tr("Quality"), rayQuality_)
        .label->setToolTip(tr("How clean the image gets: Draft 64, Good 256, Final 1024, Ultra "
                              "4096 samples per pixel. Every computer reaches the same quality; "
                              "slower ones just take longer."));
    raySpeed_ = new ui::SegmentedControl({tr("Gentle"), tr("Balanced"), tr("Fast")}, rays);
    rays->addRow(tr("Speed"), raySpeed_)
        .label->setToolTip(tr("How hard the graphics card works. Gentle keeps the computer cool and "
                              "quiet (good on battery); Fast finishes soonest."));
    rayProgress_ = new QProgressBar(rays);
    rayProgress_->setTextVisible(false);
    rayProgress_->setMaximumHeight(6);
    rays->addWide(rayProgress_);
    rayStatus_ = ui::label(QString(), "muted", rays);
    rayStatus_->setWordWrap(true);
    rays->addWide(rayStatus_);
    layout->addWidget(rays);

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
        // Qt gives push buttons a ~50 px minimum; five of them would widen every mode page,
        // because the mode panel's stack is as wide as its widest page.
        button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
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
    autoExposure_ = new QCheckBox(tr("Auto exposure"), image);
    autoExposure_->setToolTip(tr("Adjusts the brightness to the scene like a camera, so daylight, "
                                 "interiors and evening views all come out well exposed."));
    image->addWide(autoExposure_);
    exposure_ = new ui::SliderField(-4.0, 4.0, 1, tr(" EV"), image);
    image->addRow(tr("Exposure"), exposure_)
        .label->setToolTip(tr("Overall brightness in camera stops: +1 doubles the light. With auto "
                              "exposure, a correction on top of it."));
    toneMapping_ = new QComboBox(image);
    toneMapping_->addItem(tr("Filmic"));
    toneMapping_->addItem(tr("Neutral"));
    toneMapping_->addItem(tr("None"));
    toneMapping_->addItem(tr("AgX"));
    toneMapping_->setToolTip(tr("Filmic: rich contrast, soft highlights. Neutral: true material "
                                "colors. None: no highlight compression. AgX: gentle contrast; "
                                "very bright colors fade to white without shifting hue."));
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
    connect(autoExposure_, &QCheckBox::toggled, this,
            [this](bool on) { editLook([on](Look& l) { l.autoExposure = on; }); });

    // Quality and speed are preferences of this computer, not of the project.
    const QSettings settings;
    render::RayTracingSettings rayTracing = viewport_.rayTracing();
    rayTracing.quality = static_cast<render::RayQuality>(
        std::clamp(settings.value(QStringLiteral("render/rayQuality"), 1).toInt(), 0, 3));
    rayTracing.speed = static_cast<render::RaySpeed>(
        std::clamp(settings.value(QStringLiteral("render/raySpeed"), 1).toInt(), 0, 2));
    viewport_.setRayTracing(rayTracing);
    connect(rayTraced_, &QCheckBox::toggled, this, [this](bool on) {
        render::RayTracingSettings changed = viewport_.rayTracing();
        changed.enabled = on;
        viewport_.setRayTracing(changed);
    });
    connect(rayQuality_, &ui::SegmentedControl::currentEdited, this, [this](int index) {
        render::RayTracingSettings changed = viewport_.rayTracing();
        changed.quality = static_cast<render::RayQuality>(index);
        viewport_.setRayTracing(changed);
        QSettings().setValue(QStringLiteral("render/rayQuality"), index);
    });
    connect(raySpeed_, &ui::SegmentedControl::currentEdited, this, [this](int index) {
        render::RayTracingSettings changed = viewport_.rayTracing();
        changed.speed = static_cast<render::RaySpeed>(index);
        viewport_.setRayTracing(changed);
        QSettings().setValue(QStringLiteral("render/raySpeed"), index);
    });
    connect(&viewport_, &ViewportWindow::rayTracingChanged, this, &RenderPanel::refreshRayTracing);
    connect(&viewport_, &ViewportWindow::rayTracingProgress, this,
            [this](const render::RayTracingStatus& status) {
                rayStatus_->setText(rayStatusText(status));
                rayProgress_->setVisible(status.active && !status.preparing);
                rayProgress_->setRange(0, static_cast<int>(std::max(status.targetSamples, 1u)));
                rayProgress_->setValue(static_cast<int>(status.samples));
            });
    refreshRayTracing();
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
    autoExposure_->setChecked(look.autoExposure);
    updating_ = false;
}

void RenderPanel::refreshRayTracing() {
    const render::RayTracingSettings& settings = viewport_.rayTracing();
    const QSignalBlocker block(rayTraced_);
    rayTraced_->setChecked(settings.enabled);
    rayQuality_->setCurrent(static_cast<int>(settings.quality));
    raySpeed_->setCurrent(static_cast<int>(settings.speed));
    if (!settings.enabled) {
        rayStatus_->setText(tr("Off: the viewport shows the quick preview."));
        rayProgress_->hide();
    }
}

QString rayStatusText(const render::RayTracingStatus& status) {
    if (!status.supported) {
        return QObject::tr("This graphics card or driver can't run the ray tracer (it needs "
                           "non-uniform texture indexing). Updating the driver may help.");
    }
    if (!status.active) {
        return QObject::tr("Off: the viewport shows the quick preview.");
    }
    if (status.preparing) {
        return QObject::tr("Preparing the scene for ray tracing…");
    }
    const auto clock = [](double seconds) {
        const auto total = static_cast<int>(seconds + 0.5);
        return total >= 3600 ? QStringLiteral("%1:%2:%3")
                                   .arg(total / 3600)
                                   .arg(total / 60 % 60, 2, 10, QLatin1Char('0'))
                                   .arg(total % 60, 2, 10, QLatin1Char('0'))
                             : QStringLiteral("%1:%2").arg(total / 60).arg(total % 60, 2, 10,
                                                                          QLatin1Char('0'));
    };
    if (status.samples >= status.targetSamples) {
        return QObject::tr("Done: %1 samples in %2.")
            .arg(status.targetSamples)
            .arg(clock(status.seconds));
    }
    // Every sample costs about the same, so the time so far predicts the rest.
    QString left = QObject::tr("estimating time…");
    if (status.samples >= 2) {
        const double perSample = status.seconds / status.samples;
        left = QObject::tr("about %1 left")
                   .arg(clock(perSample * (status.targetSamples - status.samples)));
    }
    return QObject::tr("Refining: %1 of %2 samples · %3 · %4")
        .arg(status.samples)
        .arg(status.targetSamples)
        .arg(clock(status.seconds))
        .arg(left);
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
