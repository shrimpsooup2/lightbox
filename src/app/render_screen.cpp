#include "app/render_screen.hpp"

#include <algorithm>
#include <array>
#include <exception>

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#include "app/property_widgets.hpp"
#include "app/render_panel.hpp"
#include "app/theme.hpp"
#include "app/ui_helpers.hpp"
#include "app/viewport_window.hpp"
#include "core/log.hpp"

namespace lb::app {
namespace {

struct OutputSize {
    int width; // 0: the viewport's size
    int height;
};
constexpr std::array kSizes = {OutputSize{0, 0}, OutputSize{1280, 720}, OutputSize{1920, 1080},
                               OutputSize{3840, 2160}};
constexpr int kDefaultSize = 2;    // Full HD
constexpr int kDefaultQuality = 1; // Good
constexpr int kDefaultSpeed = 2;   // Fast: the user is waiting for this image
// How often the screen shows the image filling in (each look reads the image back from the GPU).
constexpr int kPreviewIntervalMs = 1000;

const QString kSizeKey = QStringLiteral("renderScreen/size");
const QString kQualityKey = QStringLiteral("renderScreen/quality");
const QString kSpeedKey = QStringLiteral("renderScreen/speed");
const QString kDirectoryKey = QStringLiteral("renderScreen/lastDirectory");

QImage toImage(const render::CapturedImage& captured) {
    const QImage view(captured.pixels.data(), static_cast<int>(captured.width),
                      static_cast<int>(captured.height), static_cast<qsizetype>(captured.width) * 4,
                      captured.bgra ? QImage::Format_RGB32 : QImage::Format_RGBX8888);
    return view.copy(); // detach from `captured`
}

} // namespace

// The image scaled to fit (never enlarged), or a hint before the first render.
class RenderScreen::Preview : public QWidget {
public:
    explicit Preview(QWidget* parent) : QWidget(parent) { setMinimumSize(320, 200); }

    void setImage(QImage image) {
        image_ = std::move(image);
        update();
    }
    const QImage& image() const { return image_; }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const theme::Palette& palette = theme::palette();
        painter.fillRect(rect(), palette.window);
        if (image_.isNull()) {
            painter.setPen(palette.textMuted);
            painter.drawText(rect(), Qt::AlignCenter,
                             tr("Press Render to make the final image of the current view."));
            return;
        }
        const QRect area = rect().adjusted(24, 24, -24, -24);
        const QSize logical = image_.size() / devicePixelRatioF();
        const QSize size = logical.width() <= area.width() && logical.height() <= area.height()
                               ? logical
                               : logical.scaled(area.size(), Qt::KeepAspectRatio);
        const QRect target(area.center() - QPoint(size.width() / 2, size.height() / 2), size);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(target, image_);
        painter.setPen(palette.border);
        painter.drawRect(target.adjusted(-1, -1, 0, 0));
    }

private:
    QImage image_;
};

RenderScreen::RenderScreen(render::Renderer& renderer, ViewportWindow& viewport, QWidget* parent)
    : QWidget(parent), renderer_(renderer), viewport_(viewport) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    preview_ = new Preview(this);
    layout->addWidget(preview_, 1);

    auto* sidebar = new QWidget(this);
    sidebar->setProperty("role", "renderSidebar");
    sidebar->setAttribute(Qt::WA_StyledBackground);
    sidebar->setFixedWidth(300);
    auto* side = new QVBoxLayout(sidebar);
    side->setContentsMargins(20, 20, 20, 20);
    side->setSpacing(8);
    side->addWidget(ui::label(tr("Render"), "display", sidebar));
    auto* intro = ui::label(tr("The ray tracer makes the final image of the current view at the "
                               "size you choose. It keeps going while you wait here; the editing "
                               "view rests meanwhile."),
                            "muted", sidebar);
    intro->setWordWrap(true);
    side->addWidget(intro);
    side->addSpacing(8);

    QSettings settings;
    side->addWidget(ui::label(tr("Size"), "section", sidebar));
    size_ = new ui::SegmentedControl({tr("View"), tr("HD"), tr("Full HD"), tr("4K")}, sidebar);
    size_->setToolTip(tr("View: the size of the editing view. HD 1280 × 720, Full HD 1920 × 1080, "
                         "4K 3840 × 2160. Four times the pixels take four times as long."));
    size_->setCurrent(std::clamp(settings.value(kSizeKey, kDefaultSize).toInt(), 0,
                                 static_cast<int>(kSizes.size()) - 1));
    side->addWidget(size_);
    sizeNote_ = ui::label(QString(), "muted", sidebar);
    side->addWidget(sizeNote_);

    side->addWidget(ui::label(tr("Quality"), "section", sidebar));
    quality_ = new ui::SegmentedControl({tr("Draft"), tr("Good"), tr("Final"), tr("Ultra")}, sidebar);
    quality_->setToolTip(tr("How clean the image gets: Draft 64, Good 256, Final 1024, Ultra 4096 "
                            "samples per pixel. Every computer reaches the same quality; slower "
                            "ones just take longer."));
    quality_->setCurrent(std::clamp(settings.value(kQualityKey, kDefaultQuality).toInt(), 0, 3));
    side->addWidget(quality_);

    side->addWidget(ui::label(tr("Speed"), "section", sidebar));
    speed_ = new ui::SegmentedControl({tr("Gentle"), tr("Balanced"), tr("Fast")}, sidebar);
    speed_->setToolTip(tr("How hard the graphics card works. Fast finishes soonest; Gentle keeps "
                          "the computer responsive for other work."));
    speed_->setCurrent(std::clamp(settings.value(kSpeedKey, kDefaultSpeed).toInt(), 0, 2));
    side->addWidget(speed_);
    side->addSpacing(12);

    renderButton_ = new QPushButton(sidebar);
    renderButton_->setProperty("accent", true);
    side->addWidget(renderButton_);
    progress_ = new QProgressBar(sidebar);
    progress_->setTextVisible(false);
    progress_->setFixedHeight(6);
    progress_->hide();
    side->addWidget(progress_);
    status_ = ui::label(QString(), "muted", sidebar);
    status_->setWordWrap(true);
    side->addWidget(status_);
    saveButton_ = new QPushButton(theme::icon(QStringLiteral("save")), tr("Save image…"), sidebar);
    saveButton_->setToolTip(tr("Save the image at full size as PNG or JPEG (also while it is still "
                               "refining)."));
    side->addWidget(saveButton_);
    side->addStretch(1);
    auto* back = new QPushButton(theme::icon(QStringLiteral("chevron-right")),
                                 tr("Back to editing"), sidebar);
    back->setToolTip(tr("Return to the project (Esc). A render in progress stops."));
    side->addWidget(back);
    layout->addWidget(sidebar);

    timer_ = new QTimer(this);
    timer_->setInterval(0); // each step waits for the GPU, which paces the loop
    connect(timer_, &QTimer::timeout, this, &RenderScreen::step);
    connect(renderButton_, &QPushButton::clicked, this, [this] {
        if (rendering_) {
            finish(tr("Stopped. Save the image as it is, or press Render to start again."), true);
        } else {
            start();
        }
    });
    connect(saveButton_, &QPushButton::clicked, this, &RenderScreen::saveImage);
    connect(back, &QPushButton::clicked, this, &RenderScreen::closeRequested);
    connect(size_, &ui::SegmentedControl::currentEdited, this, [this](int index) {
        QSettings().setValue(kSizeKey, index);
        updateControls();
        if (rendering_) {
            start(); // a new size is a new image
        }
    });
    // Quality and speed apply to a render in progress (a new quality restarts it by itself).
    connect(quality_, &ui::SegmentedControl::currentEdited, this,
            [](int index) { QSettings().setValue(kQualityKey, index); });
    connect(speed_, &ui::SegmentedControl::currentEdited, this,
            [](int index) { QSettings().setValue(kSpeedKey, index); });
    updateControls();
}

RenderScreen::~RenderScreen() = default;

void RenderScreen::prepare(const QString& projectName) {
    projectName_ = projectName;
    updateControls();
    setFocus();
}

void RenderScreen::stop() {
    if (rendering_) {
        finish(tr("Stopped."), true);
    }
    presenter_.reset();
}

void RenderScreen::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        emit closeRequested();
        return;
    }
    QWidget::keyPressEvent(event);
}

QSize RenderScreen::outputSize() const {
    const OutputSize& size = kSizes[static_cast<std::size_t>(size_->current())];
    if (size.width > 0) {
        return {size.width, size.height};
    }
    const QSize view = viewport_.size() * viewport_.devicePixelRatio();
    return view.isEmpty() ? QSize(1280, 720) : view;
}

render::FrameInput RenderScreen::frameInput() {
    render::FrameInput input = viewport_.renderScreenInput(outputSize());
    input.rayTracing.enabled = true;
    input.rayTracing.quality = static_cast<render::RayQuality>(quality_->current());
    input.rayTracing.speed = static_cast<render::RaySpeed>(speed_->current());
    input.rayTracing.interacting = false;
    return input;
}

void RenderScreen::start() {
    const QSize size = outputSize();
    try {
        // A fresh presenter: a new image even when nothing changed since the last render.
        presenter_.reset();
        presenter_ = renderer_.createOffscreenPresenter(static_cast<std::uint32_t>(size.width()),
                                                        static_cast<std::uint32_t>(size.height()));
    } catch (const std::exception& e) {
        status_->setText(tr("The render could not start: %1").arg(QString::fromUtf8(e.what())));
        return;
    }
    log::info("Render screen: {}x{}", size.width(), size.height());
    rendering_ = true;
    sincePreview_.invalidate();
    progress_->setValue(0);
    progress_->show();
    status_->setText(tr("Preparing…"));
    timer_->start();
    updateControls();
}

void RenderScreen::step() {
    if (!presenter_) {
        return;
    }
    const bool preview = !sincePreview_.isValid() || sincePreview_.elapsed() >= kPreviewIntervalMs;
    render::CapturedImage captured;
    try {
        presenter_->render(frameInput(), preview ? &captured : nullptr);
    } catch (const std::exception& e) {
        finish(tr("Rendering stopped: %1").arg(QString::fromUtf8(e.what())), false);
        return;
    }
    if (preview && !captured.pixels.empty()) {
        preview_->setImage(toImage(captured));
        sincePreview_.restart();
        updateControls();
    }
    const render::RayTracingStatus status = presenter_->rayTracingStatus();
    if (!status.supported) {
        finish(rayStatusText(status), false);
        return;
    }
    progress_->setRange(0, static_cast<int>(std::max(status.targetSamples, 1u)));
    progress_->setValue(static_cast<int>(status.samples));
    status_->setText(viewport_.skyImageLoading() ? tr("Loading the sky image…")
                                                 : rayStatusText(status));
    if (!status.preparing && status.targetSamples > 0 && status.samples >= status.targetSamples) {
        finish(rayStatusText(status), true); // the last samples are in: read the finished image
    }
}

void RenderScreen::finish(const QString& message, bool capture) {
    timer_->stop();
    if (capture && presenter_) {
        render::CapturedImage captured;
        try {
            presenter_->render(frameInput(), &captured);
        } catch (const std::exception& e) {
            log::warn("Render screen: final read-back failed: {}", e.what());
        }
        if (!captured.pixels.empty()) {
            preview_->setImage(toImage(captured));
        }
    }
    presenter_.reset(); // a 4K render holds hundreds of MB of GPU memory
    rendering_ = false;
    status_->setText(message);
    updateControls();
}

void RenderScreen::saveImage() {
    const QImage image = preview_->image();
    if (image.isNull()) {
        return;
    }
    QSettings settings;
    const QString folder = settings
                               .value(kDirectoryKey, QStandardPaths::writableLocation(
                                                         QStandardPaths::PicturesLocation))
                               .toString();
    QString base = projectName_.isEmpty() ? QStringLiteral("LightBox") : projectName_;
    base.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")), QStringLiteral("-"));
    const QString suggested =
        QDir(folder).filePath(QStringLiteral("%1 %2.png")
                                  .arg(base, QDateTime::currentDateTime().toString(
                                                 QStringLiteral("yyyy-MM-dd HHmm"))));
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Save Render"), suggested, tr("PNG image (*.png);;JPEG image (*.jpg *.jpeg)"));
    if (path.isEmpty()) {
        return;
    }
    settings.setValue(kDirectoryKey, QFileInfo(path).absolutePath());
    const QString suffix = QFileInfo(path).suffix().toLower();
    const bool jpeg = suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg");
    if (image.save(path, nullptr, jpeg ? 95 : -1)) {
        status_->setText(tr("Saved %1.").arg(QDir::toNativeSeparators(path)));
    } else {
        status_->setText(tr("Could not save %1. Check the folder and file name.")
                             .arg(QDir::toNativeSeparators(path)));
    }
}

void RenderScreen::updateControls() {
    const QSize size = outputSize();
    sizeNote_->setText(tr("%1 × %2 pixels").arg(size.width()).arg(size.height()));
    renderButton_->setText(rendering_ ? tr("Stop") : tr("Render"));
    renderButton_->setIcon(theme::icon(rendering_ ? QStringLiteral("x")
                                                  : QStringLiteral("sparkles")));
    progress_->setVisible(rendering_);
    saveButton_->setEnabled(!preview_->image().isNull());
}

} // namespace lb::app
