#include "app/texture_slot.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollBar>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidgetAction>

#include "app/background.hpp"
#include "app/property_widgets.hpp"
#include "app/theme.hpp"
#include "app/ui_helpers.hpp"
#include "core/image.hpp"
#include "import/importer.hpp"

namespace lb::app {
namespace {

constexpr int kIdRole = Qt::UserRole;
// Thumbnail building per event-loop turn: long enough to make progress, short enough to keep input
// and painting smooth.
constexpr qint64 kThumbnailSliceMs = 8;
constexpr int kSlotThumbSize = 40;

QString imageFilter() {
    QStringList patterns;
    for (std::string_view extension : imageFileExtensions()) {
        patterns << QLatin1Char('*') +
                        QString::fromLatin1(extension.data(),
                                            static_cast<qsizetype>(extension.size()));
    }
    return QObject::tr("Images (%1)").arg(patterns.join(QLatin1Char(' ')));
}

// Folder of the last image chosen in any slot (per session), so picking a set of maps one by one
// doesn't start from the same place every time.
QString& lastImageDirectory() {
    static QString directory;
    return directory;
}

std::filesystem::path toPath(const QString& path) {
    return std::filesystem::path(path.toStdWString());
}

QString sizeText(const ImageData& image) {
    return QStringLiteral("%1 × %2").arg(image.width).arg(image.height);
}

QString usageText(int users) {
    return users == 0 ? QObject::tr("unused")
                      : ui::count(users, QObject::tr("1 material"), QObject::tr("%1 materials"));
}

QString sourceText(const TextureResource& texture) {
    return texture.sourcePath.empty()
               ? QObject::tr("Embedded in an imported model")
               : QDir::toNativeSeparators(QString::fromStdString(texture.sourcePath));
}

ResourceId textureFrom(const QMimeData* mime) {
    if (!mime->hasFormat(QLatin1String(kTextureMimeType))) {
        return {};
    }
    bool ok = false;
    const qulonglong value = mime->data(QLatin1String(kTextureMimeType)).toULongLong(&ok);
    return ok ? ResourceId{value} : ResourceId{};
}

QString imageFileFrom(const QMimeData* mime) {
    if (!mime->hasUrls()) {
        return {};
    }
    for (const QUrl& url : mime->urls()) {
        if (url.isLocalFile()) {
            const QString path = url.toLocalFile();
            if (isImageFile(toPath(path))) {
                return path;
            }
        }
    }
    return {};
}

QPixmap makeThumbnail(const ImageData& image) {
    constexpr int size = TextureThumbnails::kSize;
    const QImage scaled = textureImage(image, size);
    QImage canvas(size, size, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    if (!scaled.isNull()) {
        // Small textures are enlarged to fill the square; large ones arrive already scaled.
        const QSize fitted = scaled.size().scaled(size, size, Qt::KeepAspectRatio);
        const QRect target(QPoint((size - fitted.width()) / 2, (size - fitted.height()) / 2),
                           fitted);
        QPainter painter(&canvas);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(target, scaled);
    }
    return QPixmap::fromImage(canvas);
}

} // namespace

// --- TextureThumbnails ---------------------------------------------------------------------------

TextureThumbnails::TextureThumbnails(Scene& scene, QObject* parent)
    : QObject(parent), scene_(scene) {
    listener_ = scene_.addListener([this](const SceneChange& change) {
        if (change.kind == SceneChange::Kind::ResourceChanged ||
            change.kind == SceneChange::Kind::ResourceRemoved) {
            cache_.erase(change.resource);
        }
    });
}

TextureThumbnails::~TextureThumbnails() {
    scene_.removeListener(listener_);
}

QPixmap TextureThumbnails::thumbnail(ResourceId texture) {
    if (const auto it = cache_.find(texture); it != cache_.end()) {
        return it->second;
    }
    const TextureResource* resource = scene_.texture(texture);
    if (resource == nullptr) {
        return {};
    }
    return cache_.emplace(texture, makeThumbnail(resource->image)).first->second;
}

QPixmap TextureThumbnails::thumbnailLater(ResourceId texture) {
    if (const auto it = cache_.find(texture); it != cache_.end()) {
        return it->second;
    }
    if (scene_.texture(texture) != nullptr && queued_.insert(texture).second) {
        queue_.push_back(texture);
        if (!scheduled_) {
            scheduled_ = true;
            QTimer::singleShot(0, this, &TextureThumbnails::buildQueued);
        }
    }
    return {};
}

void TextureThumbnails::buildQueued() {
    scheduled_ = false;
    QElapsedTimer clock;
    clock.start();
    while (!queue_.empty() && clock.elapsed() < kThumbnailSliceMs) {
        const ResourceId id = queue_.front();
        queue_.pop_front();
        queued_.erase(id);
        if (scene_.texture(id) == nullptr) {
            continue; // removed while waiting
        }
        thumbnail(id);
        emit thumbnailReady(id.value);
    }
    if (!queue_.empty()) {
        scheduled_ = true;
        QTimer::singleShot(0, this, &TextureThumbnails::buildQueued);
    }
}

QIcon thumbnailIcon(const QPixmap& pixmap) {
    QIcon icon(pixmap);
    icon.addPixmap(pixmap, QIcon::Selected);
    return icon;
}

QImage textureImage(const ImageData& image, int maxSide) {
    if (image.empty() || maxSide <= 0 ||
        image.pixels.size() < std::size_t{image.width} * image.height * 4) {
        return {};
    }
    const int width = static_cast<int>(image.width);
    const int height = static_cast<int>(image.height);
    // Wraps the texture's pixels without copying; every path below returns a detached image.
    const QImage view(image.pixels.data(), width, height, static_cast<qsizetype>(width) * 4,
                      QImage::Format_RGBA8888);
    if (width <= maxSide && height <= maxSide) {
        return view.copy();
    }
    const QSize target =
        QSize(width, height).scaled(maxSide, maxSide, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
    // Smooth scaling reads every source pixel; a nearest-neighbor pass down to 4× the target first
    // keeps 4K textures in the low milliseconds at nearly the same quality.
    const QSize intermediate(target.width() * 4, target.height() * 4);
    if (width > intermediate.width() && height > intermediate.height()) {
        return view.scaled(intermediate, Qt::IgnoreAspectRatio, Qt::FastTransformation)
            .scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    return view.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

std::unordered_map<ResourceId, int> textureUsage(const Scene& scene) {
    std::unordered_map<ResourceId, int> usage;
    scene.forEachMaterial([&](const MaterialResource& resource) {
        const Material& m = resource.material;
        const std::array<ResourceId, 5> maps = {m.baseColorMap.texture, m.roughnessMap.texture,
                                                m.metalnessMap.texture, m.normalMap.texture,
                                                m.occlusionMap.texture};
        // A material using one texture for several maps (packed ORM) counts once, like
        // Scene::userCount.
        for (auto it = maps.begin(); it != maps.end(); ++it) {
            if (*it && std::find(maps.begin(), it, *it) == it) {
                ++usage[*it];
            }
        }
    });
    return usage;
}

void showTexturePreview(const Scene& scene, ResourceId id, QWidget* parent) {
    const TextureResource* texture = scene.texture(id);
    if (texture == nullptr) {
        return;
    }
    auto* dialog = new QDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QString::fromStdString(texture->name));
    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(16, 16, 16, 12);
    layout->setSpacing(6);

    int maxSide = 720;
    qreal ratio = 1.0;
    if (const QScreen* screen = parent != nullptr ? parent->screen() : nullptr) {
        maxSide = std::min(maxSide, screen->availableGeometry().height() * 2 / 3);
        ratio = screen->devicePixelRatio();
    }
    QPixmap pixmap = QPixmap::fromImage(
        textureImage(texture->image, static_cast<int>(static_cast<qreal>(maxSide) * ratio)));
    pixmap.setDevicePixelRatio(ratio);
    auto* image = new QLabel(dialog);
    image->setProperty("role", "texturePreview");
    image->setAlignment(Qt::AlignCenter);
    image->setPixmap(pixmap);
    layout->addWidget(image, 1);

    layout->addWidget(ui::label(QString::fromStdString(texture->name), "heading", dialog));
    const int users = static_cast<int>(scene.userCount(id));
    layout->addWidget(ui::label(QObject::tr("%1 pixels · %2")
                                    .arg(sizeText(texture->image), usageText(users)),
                                "muted", dialog));
    auto* source = ui::label(sourceText(*texture), "muted", dialog);
    source->setWordWrap(true);
    source->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(source);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog->show();
}

// --- TextureGrid ---------------------------------------------------------------------------------

TextureGrid::TextureGrid(TextureThumbnails& thumbnails, Detail detail, int maxRows, QWidget* parent)
    : QListWidget(parent), thumbnails_(thumbnails), detail_(detail), maxRows_(maxRows) {
    setProperty("role", "textureGrid");
    setViewMode(QListView::IconMode);
    setMovement(QListView::Static);
    setResizeMode(QListView::Adjust);
    setFlow(QListView::LeftToRight);
    setWrapping(true);
    setUniformItemSizes(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    setDefaultDropAction(Qt::CopyAction);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setTextElideMode(Qt::ElideMiddle);

    const bool full = detail_ == Detail::Full;
    const int iconSide = full ? 64 : 48;
    const int lines = full ? 3 : 1;
    setIconSize(QSize(iconSide, iconSide));
    setGridSize(QSize(iconSide + 28, iconSide + 14 + lines * fontMetrics().lineSpacing()));

    connect(&thumbnails_, &TextureThumbnails::thumbnailReady, this, [this](quint64 value) {
        const auto it = items_.find(ResourceId{value});
        if (it != items_.end()) {
            it->second->setIcon(thumbnailIcon(thumbnails_.thumbnail(it->first)));
        }
    });
}

ResourceId TextureGrid::idOf(const QListWidgetItem* item) {
    return item != nullptr ? ResourceId{item->data(kIdRole).toULongLong()} : ResourceId{};
}

void TextureGrid::rebuild(const Scene& scene) {
    std::vector<const TextureResource*> textures;
    scene.forEachTexture([&](const TextureResource& texture) { textures.push_back(&texture); });
    std::sort(textures.begin(), textures.end(), [](const auto* a, const auto* b) {
        return a->name != b->name ? a->name < b->name : a->id < b->id;
    });
    const auto usage =
        detail_ == Detail::Full ? textureUsage(scene) : std::unordered_map<ResourceId, int>{};
    const ResourceId current = idOf(currentItem());
    const QIcon placeholder = theme::icon(QStringLiteral("image"));

    setUpdatesEnabled(false);
    clear();
    items_.clear();
    for (const TextureResource* texture : textures) {
        auto* item = new QListWidgetItem(this);
        item->setData(kIdRole, QVariant::fromValue<qulonglong>(texture->id.value));
        const QPixmap pixmap = thumbnails_.thumbnailLater(texture->id);
        item->setIcon(pixmap.isNull() ? placeholder : thumbnailIcon(pixmap));
        const auto users = usage.find(texture->id);
        describe(item, *texture, users != usage.end() ? users->second : 0);
        items_.emplace(texture->id, item);
    }
    selectTexture(current);
    setUpdatesEnabled(true);
    fitHeight();
}

void TextureGrid::updateUsage(const Scene& scene) {
    if (detail_ != Detail::Full) {
        return;
    }
    const auto usage = textureUsage(scene);
    for (const auto& [id, item] : items_) {
        if (const TextureResource* texture = scene.texture(id)) {
            const auto users = usage.find(id);
            describe(item, *texture, users != usage.end() ? users->second : 0);
        }
    }
}

void TextureGrid::selectTexture(ResourceId texture) {
    const auto it = items_.find(texture);
    if (it == items_.end()) {
        clearSelection();
        setCurrentItem(nullptr);
        return;
    }
    setCurrentItem(it->second);
    scrollToItem(it->second);
}

void TextureGrid::describe(QListWidgetItem* item, const TextureResource& texture,
                           int users) const {
    const QString name = QString::fromStdString(texture.name);
    const QString elided =
        fontMetrics().elidedText(name, Qt::ElideMiddle, gridSize().width() - 8);
    const QString text = detail_ == Detail::Full ? elided + QLatin1Char('\n') +
                                                       sizeText(texture.image) + QLatin1Char('\n') +
                                                       usageText(users)
                                                 : elided;
    if (item->text() != text) {
        item->setText(text);
    }
    item->setToolTip(QObject::tr("%1\n%2 pixels · %3\n%4\n\nDrag onto a map to use it")
                         .arg(name, sizeText(texture.image), usageText(users),
                              sourceText(texture)));
}

QStringList TextureGrid::mimeTypes() const {
    return {QLatin1String(kTextureMimeType)};
}

QMimeData* TextureGrid::mimeData(const QList<QListWidgetItem*>& items) const {
    auto* mime = new QMimeData;
    if (!items.isEmpty()) {
        mime->setData(QLatin1String(kTextureMimeType),
                      QByteArray::number(static_cast<qulonglong>(idOf(items.first()).value)));
    }
    return mime;
}

void TextureGrid::resizeEvent(QResizeEvent* event) {
    QListWidget::resizeEvent(event);
    if (event->size().width() != event->oldSize().width()) {
        fitHeight();
    }
}

void TextureGrid::fitHeight() {
    const QSize cell = gridSize();
    const int columns = std::max(1, viewport()->width() / cell.width());
    const int rows = std::max(1, (count() + columns - 1) / columns);
    // Past maxRows a third of the next row peeks out, so it is obvious the grid scrolls.
    const int visible = rows <= maxRows_ ? rows * cell.height()
                                         : maxRows_ * cell.height() + cell.height() / 3;
    setFixedHeight(visible + 2 * frameWidth() + 4);
}

// --- TextureSlotWidget ---------------------------------------------------------------------------

TextureSlotWidget::TextureSlotWidget(Scene& scene, TextureThumbnails& thumbnails, Kind kind,
                                     const QString& title, QWidget* parent)
    : QWidget(parent), scene_(scene), thumbnails_(thumbnails), kind_(kind), title_(title) {
    setAcceptDrops(true);
    setAttribute(Qt::WA_StyledBackground); // paints the drop highlight from the stylesheet
    setProperty("role", "textureSlot");

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    thumb_ = new QToolButton(this);
    thumb_->setProperty("role", "textureThumb");
    thumb_->setFixedSize(kSlotThumbSize, kSlotThumbSize);
    thumb_->setCursor(Qt::PointingHandCursor);
    thumb_->setToolTip(tr("%1: click for options, or drop an image file or a project texture here")
                           .arg(title_));
    layout->addWidget(thumb_, 0, Qt::AlignTop);

    auto* text = new QVBoxLayout;
    text->setContentsMargins(0, 0, 0, 0);
    text->setSpacing(2);
    text->addStretch();
    name_ = new QLabel(this);
    name_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    name_->installEventFilter(this);
    text->addWidget(name_);
    auto* details = new QHBoxLayout;
    details->setSpacing(6);
    info_ = ui::label(QString(), "muted", this);
    info_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    details->addWidget(info_, 1);
    if (kind_ == Kind::Scalar) {
        channel_ = new QComboBox(this);
        channel_->addItems({QStringLiteral("R"), QStringLiteral("G"), QStringLiteral("B"),
                            QStringLiteral("A")});
        channel_->setToolTip(tr("Image channel that drives this setting. Packed maps hold several "
                                "in one image (glTF/ORM: G = roughness, B = metalness, R = "
                                "occlusion)."));
        invert_ = new QCheckBox(tr("Invert"), this);
        invert_->setToolTip(tr("Use 1 − value, e.g. a gloss map driving roughness"));
        details->addWidget(channel_);
        details->addWidget(invert_);
        connect(channel_, &QComboBox::activated, this, [this](int index) {
            slot_.channel = index;
            emit slotEdited(slot_);
        });
        connect(invert_, &QCheckBox::clicked, this, [this](bool checked) {
            slot_.invert = checked;
            emit slotEdited(slot_);
        });
    }
    text->addLayout(details);
    text->addStretch();
    layout->addLayout(text, 1);

    clear_ = new QToolButton(this);
    clear_->setIcon(theme::icon(QStringLiteral("x")));
    clear_->setIconSize(QSize(14, 14));
    clear_->setToolTip(tr("Remove the map from this material (the texture stays in the project)"));
    layout->addWidget(clear_, 0, Qt::AlignTop);

    connect(thumb_, &QToolButton::clicked, this, &TextureSlotWidget::showMenu);
    connect(clear_, &QToolButton::clicked, this, [this] { assign({}); });
    refresh();
}

void TextureSlotWidget::setSlot(const lb::TextureSlot& slot) {
    const TextureResource* texture = scene_.texture(slot.texture);
    if (slot == slot_ && (texture == nullptr || QString::fromStdString(texture->name) == fullName_)) {
        return; // nothing visible changed (common while a slider of the same material is dragged)
    }
    slot_ = slot;
    refresh();
}

void TextureSlotWidget::cancelLoading() {
    ++loadGeneration_;
    if (!loadingName_.isEmpty()) {
        loadingName_.clear();
        refresh();
    }
}

void TextureSlotWidget::showMenu() {
    QMenu menu(this);
    QAction* choose =
        menu.addAction(theme::icon(QStringLiteral("folder-open")), tr("Choose image…"));

    int textureCount = 0;
    scene_.forEachTexture([&](const TextureResource&) { ++textureCount; });
    QMenu* project =
        menu.addMenu(theme::icon(QStringLiteral("layout-grid")), tr("Use texture in project"));
    ResourceId picked;
    if (textureCount == 0) {
        project->setEnabled(false);
    } else {
        auto* grid = new TextureGrid(thumbnails_, TextureGrid::Detail::Name, 4, project);
        grid->setFixedWidth(grid->gridSize().width() * std::min(textureCount, 4) +
                            grid->verticalScrollBar()->sizeHint().width() +
                            2 * grid->frameWidth() + 4);
        grid->rebuild(scene_);
        grid->selectTexture(slot_.texture);
        auto* action = new QWidgetAction(project);
        action->setDefaultWidget(grid);
        project->addAction(action);
        connect(grid, &QListWidget::itemClicked, &menu,
                [&picked, project, &menu](QListWidgetItem* item) {
                    picked = TextureGrid::idOf(item);
                    project->hide();
                    menu.hide(); // ends exec()
                });
    }

    QAction* show = menu.addAction(theme::icon(QStringLiteral("eye")), tr("Show texture"));
    show->setEnabled(scene_.texture(slot_.texture) != nullptr);
    menu.addSeparator();
    QAction* clear = menu.addAction(theme::icon(QStringLiteral("x")), tr("Clear"));
    clear->setEnabled(static_cast<bool>(slot_.texture) || !loadingName_.isEmpty());

    QAction* chosen = menu.exec(thumb_->mapToGlobal(QPoint(0, thumb_->height() + 2)));
    if (picked) {
        assign(picked);
    } else if (chosen == choose) {
        chooseFile();
    } else if (chosen == show) {
        showTexturePreview(scene_, slot_.texture, this);
    } else if (chosen == clear) {
        assign({});
    }
}

void TextureSlotWidget::chooseFile() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose %1").arg(title_.toLower()), lastImageDirectory(), imageFilter());
    if (path.isEmpty()) {
        return;
    }
    lastImageDirectory() = QFileInfo(path).absolutePath();
    loadFile(path);
}

void TextureSlotWidget::loadFile(const QString& path) {
    const std::uint64_t generation = ++loadGeneration_;
    const QFileInfo info(path);
    loadingName_ = info.fileName();
    refresh();
    const std::filesystem::path file = toPath(info.absoluteFilePath());
    runInBackground<ImageData>(
        this,
        [file] { return downscaleToFit(loadImage(file), import::kMaxTextureSize); },
        [this, generation, info](ImageData image) {
            if (generation != loadGeneration_) {
                return; // cancelled, or replaced by another pick
            }
            loadingName_.clear();
            const ResourceId texture = scene_.addTexture(
                info.fileName().toStdString(), std::move(image),
                QDir::toNativeSeparators(info.absoluteFilePath()).toStdString());
            assign(texture);
        },
        [this, generation](std::exception_ptr error) {
            if (generation != loadGeneration_) {
                return;
            }
            loadingName_.clear();
            refresh();
            QString message;
            try {
                std::rethrow_exception(error);
            } catch (const std::exception& e) {
                message = QString::fromUtf8(e.what());
            } catch (...) {
                message = tr("The image could not be read.");
            }
            QMessageBox::warning(this, tr("Could not load image"), message);
        });
}

void TextureSlotWidget::assign(ResourceId texture) {
    ++loadGeneration_; // a pick replaces an image still loading
    loadingName_.clear();
    slot_.texture = texture;
    refresh();
    emit slotEdited(slot_);
}

void TextureSlotWidget::refresh() {
    const TextureResource* texture = scene_.texture(slot_.texture);
    const bool loading = !loadingName_.isEmpty();
    if (loading) {
        thumb_->setIcon(theme::icon(QStringLiteral("refresh-cw")));
        thumb_->setIconSize(QSize(18, 18));
        fullName_ = tr("Loading %1…").arg(loadingName_);
        info_->setText(tr("Decoding image"));
        name_->setToolTip(QString());
    } else if (texture != nullptr) {
        thumb_->setIcon(thumbnailIcon(thumbnails_.thumbnail(slot_.texture)));
        thumb_->setIconSize(QSize(kSlotThumbSize - 4, kSlotThumbSize - 4));
        fullName_ = QString::fromStdString(texture->name);
        info_->setText(sizeText(texture->image));
        name_->setToolTip(fullName_ + QLatin1Char('\n') + sourceText(*texture));
    } else {
        thumb_->setIcon(theme::icon(QStringLiteral("image-plus")));
        thumb_->setIconSize(QSize(18, 18));
        fullName_ = tr("None");
        info_->clear();
        name_->setToolTip(QString());
    }
    ui::setStyleProperty(name_, "role", texture != nullptr || loading ? QString()
                                                                      : QStringLiteral("muted"));
    info_->setVisible(texture != nullptr || loading);
    const bool scalarOptions = kind_ == Kind::Scalar && texture != nullptr && !loading;
    if (channel_ != nullptr) {
        channel_->setCurrentIndex(std::clamp(slot_.channel, 0, 3));
        channel_->setVisible(scalarOptions);
        invert_->setChecked(slot_.invert);
        invert_->setVisible(scalarOptions);
    }
    clear_->setVisible(texture != nullptr || loading);
    updateName();
}

void TextureSlotWidget::updateName() {
    name_->setText(
        name_->fontMetrics().elidedText(fullName_, Qt::ElideMiddle, std::max(0, name_->width())));
}

void TextureSlotWidget::setDropHighlight(bool on) {
    ui::setStyleProperty(this, "dropTarget", on);
}

void TextureSlotWidget::dragEnterEvent(QDragEnterEvent* event) {
    const QMimeData* mime = event->mimeData();
    if (scene_.texture(textureFrom(mime)) != nullptr || !imageFileFrom(mime).isEmpty()) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
        setDropHighlight(true);
    }
}

void TextureSlotWidget::dragLeaveEvent(QDragLeaveEvent* event) {
    QWidget::dragLeaveEvent(event);
    setDropHighlight(false);
}

void TextureSlotWidget::dropEvent(QDropEvent* event) {
    setDropHighlight(false);
    const QMimeData* mime = event->mimeData();
    // Copy, never move: a move would make the texture grid remove the dragged item.
    event->setDropAction(Qt::CopyAction);
    if (const ResourceId texture = textureFrom(mime); scene_.texture(texture) != nullptr) {
        event->accept();
        assign(texture);
    } else if (const QString path = imageFileFrom(mime); !path.isEmpty()) {
        event->accept();
        lastImageDirectory() = QFileInfo(path).absolutePath();
        loadFile(path);
    }
}

bool TextureSlotWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == name_ && event->type() == QEvent::Resize) {
        updateName(); // the label's width follows the layout, not only this widget's size
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace lb::app
