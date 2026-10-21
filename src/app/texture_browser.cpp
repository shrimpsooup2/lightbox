#include "app/texture_browser.hpp"

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrlQuery>
#include <QVBoxLayout>

#include "app/background.hpp"
#include "app/property_widgets.hpp"
#include "app/theme.hpp"
#include "app/ui_helpers.hpp"
#include "core/log.hpp"
#include "import/importer.hpp"

namespace lb::app {
namespace {

constexpr int kAssetRole = Qt::UserRole; // index into results_
constexpr int kMaxShown = 120;
constexpr int kIconSize = 148;
constexpr const char* kSourceKey = "textures/source";
constexpr const char* kResolutionKey = "textures/resolution";

// Quick searches in the words architects use.
constexpr const char* kCategories[] = {"Brick",  "Concrete", "Plaster", "Wood",   "Planks",
                                       "Tiles",  "Stone",    "Marble",  "Metal",  "Paving",
                                       "Asphalt", "Gravel",  "Grass",   "Ground", "Roofing",
                                       "Fabric"};

QString sizeText(double width, double height) {
    if (width <= 0.0 || height <= 0.0) {
        return TextureBrowser::tr("size unknown, %1 m used")
            .arg(QLocale().toString(import::kDefaultTextureTileMeters));
    }
    const QLocale locale;
    if (std::abs(width - height) < 1e-6) {
        return TextureBrowser::tr("%1 m").arg(locale.toString(width, 'g', 3));
    }
    return TextureBrowser::tr("%1 × %2 m")
        .arg(locale.toString(width, 'g', 3), locale.toString(height, 'g', 3));
}

std::filesystem::path toPath(const QString& path) {
    return std::filesystem::path(path.toStdWString());
}

void removePath(const QString& path) {
    if (QFileInfo(path).isDir()) {
        QDir(path).removeRecursively();
    } else {
        QFile::remove(path);
    }
}

} // namespace

TextureBrowser::TextureBrowser(Scene& scene, Selection& selection, QWidget* parent)
    : QDialog(parent), scene_(scene), selection_(selection) {
    setWindowTitle(tr("Texture library"));
    resize(1000, 700);

    network_ = new QNetworkAccessManager(this);
    auto* cache = new QNetworkDiskCache(network_);
    cache->setCacheDirectory(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
                             QStringLiteral("/texture-library"));
    cache->setMaximumCacheSize(qint64{256} * 1024 * 1024);
    network_->setCache(cache);

    QSettings settings;
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);

    auto* top = new QHBoxLayout;
    top->setSpacing(8);
    sourceControl_ = new ui::SegmentedControl({tr("ambientCG"), tr("Poly Haven")}, this);
    sourceControl_->setCurrent(std::clamp(settings.value(kSourceKey, 0).toInt(), 0, 1));
    sourceControl_->setFixedWidth(240);
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Search textures, e.g. red brick, oak floor, concrete"));
    search_->setClearButtonEnabled(true);
    search_->addAction(theme::icon(QStringLiteral("search")), QLineEdit::LeadingPosition);
    resolution_ = new QComboBox(this);
    resolution_->addItems({QStringLiteral("1K"), QStringLiteral("2K"), QStringLiteral("4K")});
    resolution_->setCurrentText(settings.value(kResolutionKey, QStringLiteral("2K")).toString());
    resolution_->setToolTip(tr("Image size to download. 2K is sharp for most views; 4K for close-ups "
                               "(larger downloads, more memory)."));
    top->addWidget(sourceControl_);
    top->addWidget(search_, 1);
    top->addWidget(ui::label(tr("Resolution"), "property", this));
    top->addWidget(resolution_);
    layout->addLayout(top);

    auto* categories = new QHBoxLayout;
    categories->setSpacing(4);
    for (const char* category : kCategories) {
        auto* button = new QPushButton(tr(category), this);
        button->setProperty("compact", true);
        connect(button, &QPushButton::clicked, this, [this, category] {
            search_->setText(tr(category).toLower());
            search();
        });
        categories->addWidget(button);
    }
    categories->addStretch(1);
    layout->addLayout(categories);

    grid_ = new QListWidget(this);
    grid_->setViewMode(QListView::IconMode);
    grid_->setIconSize(QSize(kIconSize, kIconSize));
    grid_->setGridSize(QSize(kIconSize + 26, kIconSize + 50));
    grid_->setResizeMode(QListView::Adjust);
    grid_->setMovement(QListView::Static);
    grid_->setUniformItemSizes(true);
    grid_->setWordWrap(true);
    grid_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(grid_, 1);

    credit_ = ui::label(QString(), "muted", this);
    credit_->setTextFormat(Qt::RichText);
    credit_->setOpenExternalLinks(true);
    layout->addWidget(credit_);

    auto* footer = new QHBoxLayout;
    footer->setSpacing(8);
    status_ = ui::label(QString(), "muted", this);
    status_->setWordWrap(true);
    progress_ = new QProgressBar(this);
    progress_->setRange(0, 100);
    progress_->setFixedWidth(160);
    progress_->hide();
    cancel_ = new QPushButton(tr("Cancel"), this);
    cancel_->hide();
    use_ = new QPushButton(theme::icon(QStringLiteral("download")), tr("Add material"), this);
    use_->setProperty("accent", true);
    use_->setDefault(true);
    auto* close = new QPushButton(tr("Close"), this);
    footer->addWidget(status_, 1);
    footer->addWidget(progress_);
    footer->addWidget(cancel_);
    footer->addWidget(use_);
    footer->addWidget(close);
    layout->addLayout(footer);

    // Typing searches after a short pause; Enter searches at once.
    auto* debounce = new QTimer(this);
    debounce->setSingleShot(true);
    debounce->setInterval(400);
    connect(debounce, &QTimer::timeout, this, &TextureBrowser::search);
    connect(search_, &QLineEdit::textEdited, debounce, qOverload<>(&QTimer::start));
    connect(search_, &QLineEdit::returnPressed, this, [this, debounce] {
        debounce->stop();
        search();
    });
    connect(sourceControl_, &ui::SegmentedControl::currentEdited, this, [this](int index) {
        QSettings().setValue(kSourceKey, index);
        search();
    });
    connect(resolution_, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        QSettings().setValue(kResolutionKey, text);
        rebuildGrid(); // the "✓ in library" marks depend on the resolution
    });
    connect(grid_, &QListWidget::currentItemChanged, this, &TextureBrowser::updateActions);
    connect(grid_, &QListWidget::itemActivated, this, &TextureBrowser::useSelected);
    connect(use_, &QPushButton::clicked, this, &TextureBrowser::useSelected);
    connect(cancel_, &QPushButton::clicked, this, &TextureBrowser::cancelDownload);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    selectionListener_ = selection_.addListener([this] { updateActions(); });

    search();
}

TextureBrowser::~TextureBrowser() {
    selection_.removeListener(selectionListener_);
    cancelDownload();
}

TextureBrowser::Source TextureBrowser::source() const {
    return sourceControl_->current() == 1 ? Source::PolyHaven : Source::AmbientCg;
}

QString TextureBrowser::resolution() const {
    return resolution_->currentText();
}

QNetworkReply* TextureBrowser::get(const QUrl& url, bool preferCache) {
    QNetworkRequest request(url);
    // Poly Haven's API terms require a User-Agent that names the application.
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("LightBox/%1 (+https://github.com/shrimpsooup2/lightbox)")
                          .arg(QStringLiteral(LIGHTBOX_VERSION)));
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         preferCache ? QNetworkRequest::PreferCache
                                     : QNetworkRequest::PreferNetwork);
    request.setTransferTimeout(60000);
    return network_->get(request);
}

void TextureBrowser::search() {
    if (searchReply_) {
        searchReply_->abort();
    }
    ++searchSerial_;
    const QString text = search_->text().trimmed();
    if (source() == Source::AmbientCg) {
        credit_->setText(tr("Textures from <a href=\"https://ambientcg.com\">ambientCG</a> · CC0, "
                            "free for any use, no credit required"));
        searchAmbientCg(text);
    } else {
        credit_->setText(tr("Textures from <a href=\"https://polyhaven.com\">Poly Haven</a> · CC0, "
                            "free for any use · consider supporting them on Patreon"));
        searchPolyHaven(text);
    }
}

void TextureBrowser::searchAmbientCg(const QString& text) {
    QUrl url(QStringLiteral("https://ambientcg.com/api/v2/full_json"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("type"), QStringLiteral("Material"));
    query.addQueryItem(QStringLiteral("limit"), QString::number(kMaxShown));
    query.addQueryItem(QStringLiteral("sort"), QStringLiteral("Popular"));
    query.addQueryItem(QStringLiteral("include"),
                       QStringLiteral("downloadData,imageData,displayData,dimensionsData"));
    if (!text.isEmpty()) {
        query.addQueryItem(QStringLiteral("q"), text);
    }
    url.setQuery(query);
    setStatus(tr("Searching ambientCG…"));
    QNetworkReply* reply = get(url);
    searchReply_ = reply;
    const std::uint64_t serial = searchSerial_;
    connect(reply, &QNetworkReply::finished, this, [this, reply, serial] {
        reply->deleteLater();
        if (serial != searchSerial_) {
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            setStatus(tr("Can't reach ambientCG (%1). Check the internet connection.")
                          .arg(reply->errorString()),
                      true);
            return;
        }
        const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        std::vector<Asset> assets;
        for (const QJsonValue& value : root.value(QStringLiteral("foundAssets")).toArray()) {
            const QJsonObject item = value.toObject();
            Asset asset;
            asset.id = item.value(QStringLiteral("assetId")).toString();
            asset.name = item.value(QStringLiteral("displayName")).toString(asset.id);
            asset.category = item.value(QStringLiteral("displayCategory")).toString();
            // ambientCG gives the real-world size in centimeters (0 when unknown).
            asset.widthMeters = item.value(QStringLiteral("dimensionX")).toDouble() / 100.0;
            asset.heightMeters = item.value(QStringLiteral("dimensionY")).toDouble() / 100.0;
            asset.thumbnail = QUrl(item.value(QStringLiteral("previewImage"))
                                       .toObject()
                                       .value(QStringLiteral("256-PNG"))
                                       .toString());
            const QJsonArray downloads =
                item[QStringLiteral("downloadFolders")][QStringLiteral("default")]
                    [QStringLiteral("downloadFiletypeCategories")][QStringLiteral("zip")]
                    [QStringLiteral("downloads")]
                        .toArray();
            for (const QJsonValue& download : downloads) {
                const QJsonObject d = download.toObject();
                asset.zips.insert(d.value(QStringLiteral("attribute")).toString(),
                                  QUrl(d.value(QStringLiteral("downloadLink")).toString()));
            }
            if (!asset.id.isEmpty() && !asset.zips.isEmpty()) {
                assets.push_back(std::move(asset));
            }
        }
        const int total = root.value(QStringLiteral("numberOfResults"))
                              .toInt(static_cast<int>(assets.size()));
        showResults(std::move(assets), total);
    });
}

void TextureBrowser::searchPolyHaven(const QString& text) {
    if (polyHavenIndex_.empty()) {
        // The whole Poly Haven texture list is small (~900 entries): load once, filter locally.
        setStatus(tr("Loading Poly Haven…"));
        QNetworkReply* reply =
            get(QUrl(QStringLiteral("https://api.polyhaven.com/assets?t=textures")));
        searchReply_ = reply;
        const std::uint64_t serial = searchSerial_;
        connect(reply, &QNetworkReply::finished, this, [this, reply, serial] {
            reply->deleteLater();
            if (serial != searchSerial_) {
                return;
            }
            if (reply->error() != QNetworkReply::NoError) {
                setStatus(tr("Can't reach Poly Haven (%1). Check the internet connection.")
                              .arg(reply->errorString()),
                          true);
                return;
            }
            const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
            for (auto it = root.begin(); it != root.end(); ++it) {
                const QJsonObject item = it.value().toObject();
                Asset asset;
                asset.id = it.key();
                asset.name = item.value(QStringLiteral("name")).toString(asset.id);
                asset.category = item.value(QStringLiteral("category")).toString();
                for (const QString& key : {QStringLiteral("tags"), QStringLiteral("categories")}) {
                    for (const QJsonValue& word : item.value(key).toArray()) {
                        asset.keywords << word.toString().toLower();
                    }
                }
                asset.keywords << asset.category.toLower();
                // Poly Haven gives the real-world size in millimeters.
                const QJsonArray dimensions = item.value(QStringLiteral("dimensions")).toArray();
                if (dimensions.size() >= 2) {
                    asset.widthMeters = dimensions.at(0).toDouble() / 1000.0;
                    asset.heightMeters = dimensions.at(1).toDouble() / 1000.0;
                }
                // Qt here has no WebP plugin; the CDN serves the same thumbnail as PNG.
                QUrl thumbnailUrl(item.value(QStringLiteral("thumbnail_url")).toString());
                QUrlQuery thumbnailQuery(thumbnailUrl);
                thumbnailQuery.removeAllQueryItems(QStringLiteral("format"));
                thumbnailUrl.setQuery(thumbnailQuery);
                const QString thumbnail = thumbnailUrl.toString();
                asset.thumbnail =
                    QUrl(thumbnail.isEmpty()
                             ? QStringLiteral("https://cdn.polyhaven.com/asset_img/thumbs/%1.png"
                                              "?width=256&height=256")
                                   .arg(asset.id)
                             : thumbnail);
                asset.popularity = item.value(QStringLiteral("download_count")).toInt();
                polyHavenIndex_.push_back(std::move(asset));
            }
            std::sort(polyHavenIndex_.begin(), polyHavenIndex_.end(),
                      [](const Asset& a, const Asset& b) { return a.popularity > b.popularity; });
            if (!polyHavenIndex_.empty()) {
                searchPolyHaven(search_->text().trimmed());
            } else {
                setStatus(tr("Poly Haven sent an empty list. Try again later."), true);
            }
        });
        return;
    }
    const QStringList words = text.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    std::vector<Asset> matches;
    for (const Asset& asset : polyHavenIndex_) {
        const QString name = asset.name.toLower();
        const bool all = std::all_of(words.begin(), words.end(), [&](const QString& word) {
            return name.contains(word) ||
                   std::any_of(asset.keywords.begin(), asset.keywords.end(),
                               [&](const QString& keyword) { return keyword.contains(word); });
        });
        if (all) {
            matches.push_back(asset);
        }
    }
    const int total = static_cast<int>(matches.size());
    if (matches.size() > static_cast<std::size_t>(kMaxShown)) {
        matches.resize(kMaxShown);
    }
    showResults(std::move(matches), total);
}

void TextureBrowser::showResults(std::vector<Asset> assets, int total) {
    results_ = std::move(assets);
    total_ = total;
    grid_->clear(); // a new result list starts without a selection
    if (results_.empty()) {
        setStatus(tr("Nothing found. Try another word, like brick, oak or concrete."));
    } else if (total_ > static_cast<int>(results_.size())) {
        setStatus(tr("Showing %1 of %2. Type to narrow the search.")
                      .arg(results_.size())
                      .arg(total_));
    } else {
        setStatus(ui::count(static_cast<int>(results_.size()), tr("1 texture"),
                            tr("%1 textures")) +
                  tr(" · double-click one to use it"));
    }
    rebuildGrid();
}

void TextureBrowser::rebuildGrid() {
    const QListWidgetItem* current = grid_->currentItem();
    const int selectedRow = current != nullptr ? current->data(kAssetRole).toInt() : -1;
    grid_->clear();
    const QIcon placeholder = theme::icon(QStringLiteral("image"));
    for (std::size_t i = 0; i < results_.size(); ++i) {
        const Asset& asset = results_[i];
        const bool downloaded = !downloadedPath(asset).isEmpty();
        auto* item = new QListWidgetItem(
            placeholder, QStringLiteral("%1\n%2%3")
                             .arg(asset.name, sizeText(asset.widthMeters, asset.heightMeters),
                                  downloaded ? QStringLiteral("  ✓") : QString()));
        item->setData(kAssetRole, static_cast<int>(i));
        item->setToolTip(tr("%1\n%2 · tile size %3%4")
                             .arg(asset.name, asset.category,
                                  sizeText(asset.widthMeters, asset.heightMeters),
                                  downloaded ? tr("\nIn your library at %1, no download needed")
                                                   .arg(resolution())
                                             : QString()));
        grid_->addItem(item);
        if (static_cast<int>(i) == selectedRow) {
            grid_->setCurrentItem(item);
        }
        loadThumbnail(static_cast<int>(i));
    }
    updateActions();
}

void TextureBrowser::loadThumbnail(int row) {
    const QUrl url = results_[static_cast<std::size_t>(row)].thumbnail;
    if (!url.isValid() || url.isEmpty()) {
        return;
    }
    QNetworkReply* reply = get(url, true);
    const std::uint64_t serial = searchSerial_;
    connect(reply, &QNetworkReply::finished, this, [this, reply, row, serial] {
        reply->deleteLater();
        if (serial != searchSerial_ || reply->error() != QNetworkReply::NoError) {
            return;
        }
        QPixmap pixmap;
        if (!pixmap.loadFromData(reply->readAll())) {
            return;
        }
        if (QListWidgetItem* item = grid_->item(row)) {
            item->setIcon(QIcon(pixmap.scaled(kIconSize, kIconSize, Qt::KeepAspectRatioByExpanding,
                                              Qt::SmoothTransformation)));
        }
    });
}

void TextureBrowser::updateActions() {
    const bool working = download_ != nullptr || creating_;
    const auto objects = static_cast<int>(meshEntitiesIn(scene_, selection_.ids()).size());
    use_->setEnabled(grid_->currentItem() != nullptr && !working);
    use_->setText(objects > 0 ? tr("Apply to %1")
                                    .arg(ui::count(objects, tr("1 object"), tr("%1 objects")))
                              : tr("Add material"));
    use_->setToolTip(objects > 0
                         ? tr("Download the texture (if needed), make a material and give it to the "
                              "selected objects")
                         : tr("Download the texture (if needed) and add it as a material. Select "
                              "objects first to apply it right away."));
}

void TextureBrowser::setStatus(const QString& text, bool error) {
    status_->setText(text);
    status_->setToolTip(error ? text : QString());
    if (error) {
        log::warn("Texture library: {}", text.toStdString());
    }
}

void TextureBrowser::setBusy(bool busy) {
    progress_->setVisible(busy);
    progress_->setValue(0);
    cancel_->setVisible(busy && download_ != nullptr);
    updateActions();
}

QString TextureBrowser::libraryFolder() const {
    // Next to the logs (%LOCALAPPDATA%/LightBox, ~/.local/share/LightBox), independent of the
    // organization name Qt would add to AppLocalDataLocation.
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
           QStringLiteral("/LightBox/Library/Textures/") +
           (source() == Source::AmbientCg ? QStringLiteral("ambientCG")
                                          : QStringLiteral("PolyHaven"));
}

QString TextureBrowser::downloadedPath(const Asset& asset) const {
    const QString base = libraryFolder() + QLatin1Char('/') + asset.id + QLatin1Char('_');
    if (source() == Source::AmbientCg) {
        for (const QString& format : {QStringLiteral("JPG"), QStringLiteral("PNG")}) {
            const QString zip = base + resolution() + QLatin1Char('-') + format +
                                QStringLiteral(".zip");
            if (QFileInfo(zip).isFile()) {
                return zip;
            }
        }
        return {};
    }
    const QString folder = base + resolution().toLower();
    return QFileInfo(folder).isDir() ? folder : QString();
}

void TextureBrowser::writeAssetInfo(const Asset& asset) const {
    // Kept next to the download for the local library (1.10): what it is, where it came from.
    const bool ambient = source() == Source::AmbientCg;
    QJsonObject info;
    info.insert(QStringLiteral("id"), asset.id);
    info.insert(QStringLiteral("name"), asset.name);
    info.insert(QStringLiteral("category"), asset.category);
    info.insert(QStringLiteral("source"), ambient ? QStringLiteral("ambientCG")
                                                  : QStringLiteral("Poly Haven"));
    info.insert(QStringLiteral("url"), (ambient ? QStringLiteral("https://ambientcg.com/a/")
                                                : QStringLiteral("https://polyhaven.com/a/")) +
                                           asset.id);
    info.insert(QStringLiteral("license"), QStringLiteral("CC0-1.0"));
    info.insert(QStringLiteral("widthMeters"), asset.widthMeters);
    info.insert(QStringLiteral("heightMeters"), asset.heightMeters);
    QFile file(libraryFolder() + QLatin1Char('/') + asset.id + QStringLiteral(".json"));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(info).toJson());
    }
}

void TextureBrowser::useSelected() {
    const QListWidgetItem* item = grid_->currentItem();
    if (item == nullptr || download_ != nullptr || creating_) {
        return;
    }
    const Asset asset = results_[static_cast<std::size_t>(item->data(kAssetRole).toInt())];
    if (const QString existing = downloadedPath(asset); !existing.isEmpty()) {
        createMaterial(asset, toPath(existing));
        return;
    }
    if (!QDir().mkpath(libraryFolder())) {
        setStatus(tr("Can't create the library folder %1.")
                      .arg(QDir::toNativeSeparators(libraryFolder())),
                  true);
        return;
    }
    if (source() == Source::AmbientCg) {
        downloadAmbientCg(asset);
    } else {
        downloadPolyHaven(asset);
    }
}

void TextureBrowser::downloadAmbientCg(const Asset& asset) {
    QString format = QStringLiteral("JPG"); // JPG sets are a third of the PNG size
    QUrl url = asset.zips.value(resolution() + QStringLiteral("-JPG"));
    if (!url.isValid() || url.isEmpty()) {
        format = QStringLiteral("PNG");
        url = asset.zips.value(resolution() + QStringLiteral("-PNG"));
    }
    if (!url.isValid() || url.isEmpty()) {
        setStatus(tr("%1 isn't available at %2. Pick another resolution.")
                      .arg(asset.name, resolution()),
                  true);
        return;
    }
    auto download = std::make_unique<Download>();
    download->asset = asset;
    download->source = Source::AmbientCg;
    download->finalPath = libraryFolder() + QLatin1Char('/') + asset.id + QLatin1Char('_') +
                          resolution() + QLatin1Char('-') + format + QStringLiteral(".zip");
    download->partialPath = download->finalPath + QStringLiteral(".part");
    download->files.emplace_back(url, download->partialPath);
    download_ = std::move(download);
    setBusy(true);
    downloadNext();
}

void TextureBrowser::downloadPolyHaven(const Asset& asset) {
    auto download = std::make_unique<Download>();
    download->asset = asset;
    download->source = Source::PolyHaven;
    download->finalPath =
        libraryFolder() + QLatin1Char('/') + asset.id + QLatin1Char('_') + resolution().toLower();
    download->partialPath = download->finalPath + QStringLiteral(".part");
    download_ = std::move(download);
    setBusy(true);
    setStatus(tr("Getting the files of %1…").arg(asset.name));

    QNetworkReply* reply =
        get(QUrl(QStringLiteral("https://api.polyhaven.com/files/%1").arg(asset.id)));
    downloadReply_ = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (!download_) {
            return; // cancelled
        }
        if (reply->error() != QNetworkReply::NoError) {
            const QString error = reply->errorString();
            download_.reset();
            setBusy(false);
            setStatus(tr("Can't reach Poly Haven (%1).").arg(error), true);
            return;
        }
        const QJsonObject files = QJsonDocument::fromJson(reply->readAll()).object();
        const QString res = resolution().toLower();
        const auto pick = [&](const QString& map) {
            const QJsonObject formats = files[map][res].toObject();
            for (const QString& format : {QStringLiteral("jpg"), QStringLiteral("png")}) {
                const QString url = formats[format][QStringLiteral("url")].toString();
                if (!url.isEmpty()) {
                    return QUrl(url);
                }
            }
            return QUrl();
        };
        Download& d = *download_;
        const bool hasRough = files.contains(QStringLiteral("Rough"));
        const bool hasAo = files.contains(QStringLiteral("AO"));
        QStringList maps = {QStringLiteral("Diffuse"), QStringLiteral("nor_gl"),
                            QStringLiteral("Rough"),   QStringLiteral("AO"),
                            QStringLiteral("Metal"),   QStringLiteral("Displacement")};
        if (!hasRough || !hasAo) {
            maps << QStringLiteral("arm"); // packed AO/roughness/metal fills what's missing
        }
        for (const QString& map : maps) {
            if (const QUrl url = pick(map); url.isValid() && !url.isEmpty()) {
                d.files.emplace_back(url, d.partialPath + QLatin1Char('/') +
                                              QFileInfo(url.path()).fileName());
            }
        }
        if (d.files.empty()) {
            const QString name = d.asset.name;
            download_.reset();
            setBusy(false);
            setStatus(tr("%1 has no textures at %2. Pick another resolution.")
                          .arg(name, resolution()),
                      true);
            return;
        }
        removePath(d.partialPath);
        QDir().mkpath(d.partialPath);
        downloadNext();
    });
}

void TextureBrowser::downloadNext() {
    if (!download_) {
        return;
    }
    Download& d = *download_;
    if (d.next >= d.files.size()) {
        removePath(d.finalPath); // a stale, incomplete earlier copy
        const bool moved = QFileInfo(d.partialPath).isDir()
                               ? QDir().rename(d.partialPath, d.finalPath)
                               : QFile::rename(d.partialPath, d.finalPath);
        if (!moved) {
            removePath(d.partialPath);
            download_.reset();
            setBusy(false);
            setStatus(tr("Can't save the download to the library folder."), true);
            return;
        }
        writeAssetInfo(d.asset);
        const Asset asset = d.asset;
        const QString path = d.finalPath;
        download_.reset();
        setBusy(false);
        createMaterial(asset, toPath(path));
        return;
    }
    const auto& [url, target] = d.files[d.next];
    auto file = std::make_shared<QFile>(target);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        removePath(d.partialPath);
        download_.reset();
        setBusy(false);
        setStatus(tr("Can't write %1.").arg(QDir::toNativeSeparators(target)), true);
        return;
    }
    setStatus(d.files.size() > 1 ? tr("Downloading %1 (%2 of %3)…")
                                       .arg(d.asset.name)
                                       .arg(d.next + 1)
                                       .arg(d.files.size())
                                 : tr("Downloading %1…").arg(d.asset.name));
    QNetworkReply* reply = get(url);
    downloadReply_ = reply;
    connect(reply, &QNetworkReply::readyRead, this, [reply, file] { file->write(reply->readAll()); });
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
                if (!download_ || total <= 0) {
                    return;
                }
                const double part = static_cast<double>(received) / static_cast<double>(total);
                const double overall = (static_cast<double>(download_->next) + part) /
                                       static_cast<double>(download_->files.size());
                progress_->setValue(static_cast<int>(std::lround(overall * 100.0)));
            });
    connect(reply, &QNetworkReply::finished, this, [this, reply, file] {
        reply->deleteLater();
        file->write(reply->readAll());
        file->close();
        if (!download_) {
            file->remove(); // cancelled
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            const QString error = reply->errorString();
            removePath(download_->partialPath);
            download_.reset();
            setBusy(false);
            setStatus(tr("Download failed: %1").arg(error), true);
            return;
        }
        ++download_->next;
        downloadNext();
    });
}

void TextureBrowser::cancelDownload() {
    if (!download_) {
        return;
    }
    const QString partial = download_->partialPath;
    download_.reset();
    if (downloadReply_) {
        downloadReply_->abort();
    }
    removePath(partial);
    setBusy(false);
    setStatus(tr("Download cancelled."));
}

void TextureBrowser::createMaterial(const Asset& asset, const std::filesystem::path& source) {
    creating_ = true;
    setBusy(true);
    setStatus(tr("Making the material %1…").arg(asset.name));
    runInBackground<import::ImportedModel>(
        this,
        [source] {
            return import::importTextureSet(std::span<const std::filesystem::path>(&source, 1));
        },
        [this, asset](import::ImportedModel model) {
            creating_ = false;
            setBusy(false);
            if (model.materials.empty()) {
                setStatus(tr("%1 has no usable texture maps.").arg(asset.name), true);
                return;
            }
            import::ImportedMaterial& material = model.materials.front();
            material.name = asset.name.toStdString();
            const bool sized = asset.widthMeters > 0.0 && asset.heightMeters > 0.0;
            if (sized) {
                material.material.textureScale = glm::vec2(static_cast<float>(asset.widthMeters),
                                                           static_cast<float>(asset.heightMeters));
            }
            ResourceId id;
            try {
                id = import::addMaterialToScene(scene_, model);
            } catch (const std::exception& e) {
                setStatus(QString::fromUtf8(e.what()), true);
                return;
            }
            const auto applied = static_cast<int>(applyMaterial(scene_, selection_.ids(), id));
            const QString size = sized ? sizeText(asset.widthMeters, asset.heightMeters)
                                       : sizeText(import::kDefaultTextureTileMeters,
                                                  import::kDefaultTextureTileMeters);
            const QString message =
                applied > 0
                    ? tr("Applied %1 to %2 · tile %3")
                          .arg(asset.name, ui::count(applied, tr("1 object"), tr("%1 objects")),
                               size)
                    : tr("Added material %1 · tile %2 · select objects and press Apply")
                          .arg(asset.name, size);
            setStatus(message);
            log::info("Texture library: {}", message.toStdString());
            rebuildGrid(); // "✓ in library" marks
            emit materialAdded(id, message);
        },
        [this, asset](std::exception_ptr error) {
            creating_ = false;
            setBusy(false);
            try {
                std::rethrow_exception(error);
            } catch (const std::exception& e) {
                setStatus(tr("Could not make %1: %2").arg(asset.name, QString::fromUtf8(e.what())),
                          true);
            }
        });
}

} // namespace lb::app
