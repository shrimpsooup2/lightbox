#include "app/start_screen.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>

#include <QAction>
#include <QByteArrayView>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QScrollBar>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QUrl>
#include <QVBoxLayout>

#include "app/background.hpp"
#include "app/project_controller.hpp"
#include "app/theme.hpp"
#include "app/ui_helpers.hpp"
#include "core/log.hpp"
#include "project/project_file.hpp"

namespace lb::app {
namespace {

enum Role {
    PathRole = Qt::UserRole + 1,
    SubtitleRole,
    ThumbnailRole, // QImage, null until loaded or when the project has none
    MissingRole,   // the file no longer exists
    SearchRole,    // name and path, what the search field matches
};

constexpr int kMinCardWidth = 220;
constexpr int kGap = 20;         // between cards
constexpr int kTextHeight = 62;  // name + subtitle below the thumbnail
constexpr int kTextPadding = 12; // inside the card
constexpr int kLogoWidth = 200;

std::filesystem::path toPath(const QString& path) {
    return std::filesystem::path(path.toStdU16String());
}

// "2 hours ago", "Yesterday", then a date.
QString relativeTime(qint64 unixSeconds) {
    const QDateTime when = QDateTime::fromSecsSinceEpoch(unixSeconds);
    const QDateTime now = QDateTime::currentDateTime();
    const qint64 seconds = when.secsTo(now);
    if (seconds < 60) {
        return QObject::tr("Just now");
    }
    if (seconds < 3600) {
        return ui::count(static_cast<int>(seconds / 60), QObject::tr("1 minute ago"),
                         QObject::tr("%1 minutes ago"));
    }
    if (seconds < 86400) {
        return ui::count(static_cast<int>(seconds / 3600), QObject::tr("1 hour ago"),
                         QObject::tr("%1 hours ago"));
    }
    const qint64 days = when.date().daysTo(now.date());
    if (days <= 1) {
        return QObject::tr("Yesterday");
    }
    if (days < 7) {
        return QObject::tr("%1 days ago").arg(days);
    }
    if (days < 30) {
        return ui::count(static_cast<int>(days / 7), QObject::tr("1 week ago"),
                         QObject::tr("%1 weeks ago"));
    }
    return QLocale().toString(when.date(), QStringLiteral("d MMM yyyy"));
}

// Part of an image that covers `target` (center crop, aspect kept).
QRectF coverSource(QSizeF image, QSizeF target) {
    const qreal scale = std::max(target.width() / image.width(), target.height() / image.height());
    const QSizeF visible = target / scale;
    return {(image.width() - visible.width()) / 2.0, (image.height() - visible.height()) / 2.0,
            visible.width(), visible.height()};
}

} // namespace

// Card details read off the UI thread.
struct ProjectCardInfo {
    bool exists = false;
    QString name;       // from the project; empty = use the file name
    qint64 savedAt = 0; // seconds since 1970; file time when the project has none
    QImage thumbnail;
    QString error; // the file exists but could not be read
};

namespace {

ProjectCardInfo readCard(const QString& path) {
    ProjectCardInfo card;
    const QFileInfo file(path);
    card.exists = file.exists();
    if (!card.exists) {
        return card;
    }
    card.savedAt = file.lastModified().toSecsSinceEpoch();
    try {
        const project::ProjectInfo info = project::readProjectInfo(toPath(path));
        card.name = QString::fromStdString(info.name);
        if (info.savedAtUnix > 0) {
            card.savedAt = info.savedAtUnix;
        }
        if (!info.thumbnailPng.empty()) {
            card.thumbnail = QImage::fromData(
                QByteArrayView(reinterpret_cast<const char*>(info.thumbnailPng.data()),
                               static_cast<qsizetype>(info.thumbnailPng.size())),
                "PNG");
        }
    } catch (const std::exception& e) {
        card.error = QString::fromUtf8(e.what());
    }
    return card;
}

} // namespace

// Paints a recent project as a card: 16:9 thumbnail, name, when it was saved. Colors come from the
// theme at paint time, so cards follow live theme switches.
class ProjectCardDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void setCellSize(QSize size) { cell_ = size; }

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return cell_; }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const theme::Palette& p = theme::palette();
        const qreal radius = theme::metrics().radius + 2;
        const QRectF card = QRectF(option.rect).adjusted(kGap / 2.0, kGap / 2.0, -kGap / 2.0,
                                                         -kGap / 2.0);
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool missing = index.data(MissingRole).toBool();

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        QPainterPath shape;
        shape.addRoundedRect(card, radius, radius);
        if (missing) {
            painter->setOpacity(0.5);
        }
        painter->fillPath(shape, hovered ? p.hover : p.raised);

        const QRectF thumbnail(card.left(), card.top(), card.width(), card.width() * 9.0 / 16.0);
        painter->save();
        painter->setClipPath(shape);
        const QImage image = index.data(ThumbnailRole).value<QImage>();
        if (image.isNull()) {
            painter->fillRect(thumbnail, p.window);
            const int iconSize = 40;
            const QRect iconRect(
                (thumbnail.center() - QPointF(iconSize / 2.0, iconSize / 2.0)).toPoint(),
                QSize(iconSize, iconSize));
            theme::icon(missing ? QStringLiteral("file-question") : QStringLiteral("box"))
                .paint(painter, iconRect);
        } else {
            painter->drawImage(thumbnail, image, coverSource(image.size(), thumbnail.size()));
        }
        painter->restore();
        painter->setPen(QPen(p.border, 1.0));
        painter->drawLine(thumbnail.bottomLeft(), thumbnail.bottomRight());

        const qreal textWidth = card.width() - 2 * kTextPadding;
        QFont nameFont = option.font;
        nameFont.setWeight(QFont::DemiBold);
        nameFont.setPointSizeF(option.font.pointSizeF() + 1.0);
        const QFontMetrics nameMetrics(nameFont);
        const QFontMetrics subtitleMetrics(option.font);
        const QRectF nameRect(card.left() + kTextPadding, thumbnail.bottom() + 10, textWidth,
                              nameMetrics.height());
        painter->setFont(nameFont);
        painter->setPen(p.text);
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                          nameMetrics.elidedText(index.data(Qt::DisplayRole).toString(),
                                                 Qt::ElideRight, static_cast<int>(textWidth)));
        const QRectF subtitleRect(nameRect.left(), nameRect.bottom() + 3, textWidth,
                                  subtitleMetrics.height());
        painter->setFont(option.font);
        painter->setPen(p.textMuted);
        painter->drawText(subtitleRect, Qt::AlignLeft | Qt::AlignVCenter,
                          subtitleMetrics.elidedText(index.data(SubtitleRole).toString(),
                                                     Qt::ElideRight, static_cast<int>(textWidth)));

        // Outline at full opacity so keyboard focus stays visible on missing projects too.
        painter->setOpacity(1.0);
        painter->setBrush(Qt::NoBrush);
        if (selected) {
            painter->setPen(QPen(p.accent, 2.0));
            painter->drawPath(shape);
        } else {
            painter->setPen(QPen(hovered ? p.borderStrong : p.border, 1.0));
            painter->drawPath(shape);
        }
        painter->restore();
    }

private:
    QSize cell_{kMinCardWidth + kGap, kMinCardWidth * 9 / 16 + kTextHeight + kGap};
};

StartScreen::StartScreen(ProjectController& projects, QWidget* parent)
    : QWidget(parent), projects_(projects) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Left: brand and the ways to start.
    auto* sidebar = new QWidget(this);
    sidebar->setProperty("role", "startSidebar");
    sidebar->setAttribute(Qt::WA_StyledBackground);
    sidebar->setFixedWidth(300);
    auto* side = new QVBoxLayout(sidebar);
    side->setContentsMargins(28, 32, 28, 24);
    side->setSpacing(10);
    logo_ = new QLabel(sidebar);
    side->addWidget(logo_, 0, Qt::AlignLeft);
    side->addSpacing(28);
    const auto addAction = [&](const char* icon, const QString& text, const QString& tip) {
        auto* button = new QPushButton(theme::icon(QString::fromLatin1(icon)), text, sidebar);
        button->setProperty("role", "startAction");
        button->setIconSize(QSize(18, 18));
        button->setToolTip(tip);
        side->addWidget(button);
        return button;
    };
    QPushButton* newButton = addAction("plus", tr("New project"),
                                       tr("Start an empty project and import your model (Ctrl+N)."));
    newButton->setProperty("accent", true);
    QPushButton* openButton =
        addAction("folder-open", tr("Open project…"), tr("Open a LightBox project file (Ctrl+O)."));
    QPushButton* sampleButton = addAction(
        "house", tr("Sample project"), tr("Explore LightBox with a ready-made clay house."));
    auto* hint = ui::label(tr("Or drop a model anywhere to start a new project."), "muted", sidebar);
    hint->setWordWrap(true);
    side->addSpacing(4);
    side->addWidget(hint);
    side->addStretch(1);
    side->addWidget(
        ui::label(QStringLiteral("LightBox %1").arg(QStringLiteral(LIGHTBOX_VERSION)), "muted",
                  sidebar));
    layout->addWidget(sidebar);

    // Right: recent projects.
    auto* recent = new QWidget(this);
    auto* content = new QVBoxLayout(recent);
    content->setContentsMargins(40 - kGap / 2, 36, 40 - kGap / 2, 16);
    content->setSpacing(12);
    auto* titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(kGap / 2, 0, kGap / 2, 0);
    titleRow->addWidget(ui::label(tr("Recent projects"), "display", recent));
    titleRow->addStretch(1);
    search_ = new QLineEdit(recent);
    search_->setPlaceholderText(tr("Search projects"));
    search_->setClearButtonEnabled(true);
    search_->addAction(theme::icon(QStringLiteral("search")), QLineEdit::LeadingPosition);
    search_->setFixedWidth(280);
    search_->setAcceptDrops(false); // dropped files go to the window (new project / open)
    titleRow->addWidget(search_);
    content->addLayout(titleRow);

    model_ = new QStandardItemModel(this);
    filter_ = new QSortFilterProxyModel(this);
    filter_->setSourceModel(model_);
    filter_->setFilterRole(SearchRole);
    filter_->setFilterCaseSensitivity(Qt::CaseInsensitive);

    pages_ = new QStackedWidget(recent);
    grid_ = new QListView(pages_);
    grid_->setProperty("role", "projectGrid");
    grid_->setViewMode(QListView::IconMode);
    grid_->setFlow(QListView::LeftToRight);
    grid_->setWrapping(true);
    grid_->setResizeMode(QListView::Adjust);
    grid_->setMovement(QListView::Static);
    grid_->setUniformItemSizes(true);
    grid_->setSpacing(0);
    grid_->setSelectionMode(QAbstractItemView::SingleSelection);
    grid_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    grid_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    grid_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    grid_->verticalScrollBar()->setSingleStep(24);
    grid_->setMouseTracking(true);
    grid_->viewport()->setAttribute(Qt::WA_Hover);
    grid_->setContextMenuPolicy(Qt::CustomContextMenu);
    grid_->setModel(filter_);
    delegate_ = new ProjectCardDelegate(grid_);
    grid_->setItemDelegate(delegate_);
    grid_->installEventFilter(this);
    pages_->addWidget(grid_);

    noMatches_ = ui::label(QString(), "muted", pages_);
    noMatches_->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    noMatches_->setContentsMargins(0, 40, 0, 0);
    pages_->addWidget(noMatches_);

    emptyPage_ = new QWidget(pages_);
    auto* empty = new QVBoxLayout(emptyPage_);
    empty->setSpacing(14);
    empty->addStretch(2);
    auto* emptyText =
        ui::label(tr("No projects yet. Start one, and it will appear here once you save it."),
                  "muted", emptyPage_);
    emptyText->setAlignment(Qt::AlignHCenter);
    empty->addWidget(emptyText);
    auto* emptyNew = new QPushButton(theme::icon(QStringLiteral("plus")), tr("New project"),
                                     emptyPage_);
    emptyNew->setProperty("accent", true);
    empty->addWidget(emptyNew, 0, Qt::AlignHCenter);
    empty->addStretch(3);
    pages_->addWidget(emptyPage_);
    content->addWidget(pages_, 1);
    layout->addWidget(recent, 1);

    connect(newButton, &QPushButton::clicked, this, &StartScreen::newProjectRequested);
    connect(emptyNew, &QPushButton::clicked, this, &StartScreen::newProjectRequested);
    connect(openButton, &QPushButton::clicked, this, &StartScreen::openProjectRequested);
    connect(sampleButton, &QPushButton::clicked, this, &StartScreen::sampleRequested);
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) {
        filter_->setFilterFixedString(text.trimmed());
        updateVisibility();
    });
    connect(search_, &QLineEdit::returnPressed, this, [this] {
        // Enter in the search field moves into the results, so a second Enter opens the first.
        if (filter_->rowCount() > 0) {
            grid_->setFocus();
            grid_->setCurrentIndex(filter_->index(0, 0));
        }
    });
    connect(grid_, &QListView::activated, this, &StartScreen::openIndex);
    connect(grid_, &QListView::customContextMenuRequested, this, &StartScreen::showContextMenu);
    connect(&projects_, &ProjectController::recentChanged, this, &StartScreen::refresh);

    updateLogo();
    refresh();
}

void StartScreen::refresh() {
    const std::uint64_t generation = ++generation_;
    model_->clear();
    for (const QString& path : projects_.recentProjects()) {
        const QString name = QFileInfo(path).completeBaseName();
        auto* item = new QStandardItem(name);
        item->setEditable(false);
        item->setData(path, PathRole);
        item->setData(name + QLatin1Char('\n') + QDir::toNativeSeparators(path), SearchRole);
        item->setToolTip(QDir::toNativeSeparators(path));
        model_->appendRow(item);
        runInBackground<ProjectCardInfo>(
            this, [path] { return readCard(path); },
            [this, generation, path](ProjectCardInfo info) {
                if (generation == generation_) {
                    applyCard(path, info);
                }
            },
            [path](std::exception_ptr) {
                // readCard reports read errors in its result; anything else is a bug worth a log line.
                log::warn("Could not read the start screen card for {}", path.toStdString());
            });
    }
    updateVisibility();
}

void StartScreen::applyCard(const QString& path, const ProjectCardInfo& info) {
    for (int row = 0; row < model_->rowCount(); ++row) {
        QStandardItem* item = model_->item(row);
        if (item->data(PathRole).toString() != path) {
            continue;
        }
        const QString nativePath = QDir::toNativeSeparators(path);
        if (!info.name.isEmpty()) {
            item->setText(info.name);
        }
        item->setData(item->text() + QLatin1Char('\n') + nativePath, SearchRole);
        item->setData(!info.exists, MissingRole);
        item->setData(info.thumbnail, ThumbnailRole);
        if (!info.exists) {
            item->setData(tr("File not found"), SubtitleRole);
            item->setToolTip(tr("%1\nThe file was moved, renamed or deleted.").arg(nativePath));
        } else if (!info.error.isEmpty()) {
            item->setData(tr("Can't read this project"), SubtitleRole);
            item->setToolTip(QStringLiteral("%1\n%2").arg(nativePath, info.error));
        } else {
            item->setData(relativeTime(info.savedAt), SubtitleRole);
            item->setToolTip(nativePath);
        }
        return;
    }
}

void StartScreen::updateVisibility() {
    const bool any = model_->rowCount() > 0;
    search_->setVisible(any);
    if (!any) {
        pages_->setCurrentWidget(emptyPage_);
    } else if (filter_->rowCount() == 0) {
        noMatches_->setText(tr("No projects match “%1”.").arg(search_->text().trimmed()));
        pages_->setCurrentWidget(noMatches_);
    } else {
        pages_->setCurrentWidget(grid_);
    }
}

void StartScreen::updateLogo() {
    // The "-dark" export has a light wordmark, made for dark backgrounds.
    const QString file = theme::mode() == theme::Mode::Dark
                             ? QStringLiteral(":/branding/lightbox-logo-dark.png")
                             : QStringLiteral(":/branding/lightbox-logo.png");
    const qreal ratio = devicePixelRatioF();
    const QString key = QStringLiteral("%1@%2").arg(file).arg(ratio);
    if (key == logoKey_) {
        return;
    }
    logoKey_ = key;
    QPixmap logo = QPixmap(file).scaledToWidth(static_cast<int>(kLogoWidth * ratio),
                                               Qt::SmoothTransformation);
    logo.setDevicePixelRatio(ratio);
    logo_->setPixmap(logo);
}

void StartScreen::showEvent(QShowEvent* event) {
    updateLogo(); // the screen (and its pixel ratio) is known now
    QWidget::showEvent(event);
}

void StartScreen::updateGrid() {
    // Columns of at least kMinCardWidth that fill the width exactly. The scroll bar's width is always
    // reserved, so it appearing or disappearing doesn't change the layout (and can't oscillate).
    const int scrollBar = grid_->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, grid_);
    const int available = grid_->width() - 2 * grid_->frameWidth() - scrollBar - 4;
    const int columns = std::max(1, available / (kMinCardWidth + kGap));
    const int cellWidth = std::max(kMinCardWidth + kGap, available / columns);
    const int cardWidth = cellWidth - kGap;
    const QSize cell(cellWidth, cardWidth * 9 / 16 + kTextHeight + kGap);
    if (cell != grid_->gridSize()) {
        delegate_->setCellSize(cell);
        grid_->setGridSize(cell);
    }
}

void StartScreen::openIndex(const QModelIndex& index) {
    if (index.isValid()) {
        emit projectRequested(index.data(PathRole).toString());
    }
}

void StartScreen::showContextMenu(const QPoint& position) {
    const QModelIndex index = grid_->indexAt(position);
    if (!index.isValid()) {
        return;
    }
    const QString path = index.data(PathRole).toString();
    const QString folder = QFileInfo(path).absolutePath();
    QMenu menu(this);
    menu.addAction(theme::icon(QStringLiteral("folder-open")), tr("Open"), this,
                   [this, card = QPersistentModelIndex(index)] { openIndex(card); });
    QAction* show = menu.addAction(theme::icon(QStringLiteral("folder")), tr("Show in Folder"), this,
                                   [folder] { QDesktopServices::openUrl(QUrl::fromLocalFile(folder)); });
    show->setEnabled(QFileInfo(folder).isDir());
    menu.addSeparator();
    menu.addAction(theme::icon(QStringLiteral("x")), tr("Remove from List"), this,
                   [this, path] { projects_.removeRecent(path); });
    menu.exec(grid_->viewport()->mapToGlobal(position));
}

void StartScreen::changeEvent(QEvent* event) {
    if (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange) {
        updateLogo();
    }
    QWidget::changeEvent(event);
}

bool StartScreen::eventFilter(QObject* watched, QEvent* event) {
    if (watched == grid_ && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        updateGrid();
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace lb::app
