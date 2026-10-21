#include "app/import_panel.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include <glm/common.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/matrix.hpp>
#include <glm/trigonometric.hpp>

#include "app/background.hpp"
#include "app/property_widgets.hpp"
#include "app/theme.hpp"
#include "app/ui_helpers.hpp"
#include "app/viewport_window.hpp"
#include "core/image.hpp"
#include "core/log.hpp"
#include "core/placement.hpp"

namespace lb::app {
namespace {

constexpr auto kProgressInterval = std::chrono::milliseconds(50);
constexpr int kMaxListedWarnings = 20;
constexpr int kMaxVisibleModels = 6;
constexpr int kFileLabelWidth = 190;
const QString kLastDirectoryKey = QStringLiteral("import/lastDirectory");

std::filesystem::path toPath(const QString& path) {
    return std::filesystem::path(path.toStdU16String());
}

QString fromUtf8(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString patterns(std::span<const std::string_view> extensions) {
    QStringList list;
    for (const std::string_view extension : extensions) {
        list << QStringLiteral("*") + fromUtf8(extension);
    }
    return list.join(QLatin1Char(' '));
}

QString extensionList(std::span<const std::string_view> extensions) {
    QStringList list;
    for (const std::string_view extension : extensions) {
        list << fromUtf8(extension);
    }
    return list.join(QStringLiteral(" "));
}

bool sameUnit(double a, double b) {
    return std::abs(a - b) <= 1e-5 * std::max(std::abs(a), std::abs(b));
}

QString unitText(double metersPerUnit) {
    return QStringLiteral("1 unit = %1 m").arg(QLocale().toString(metersPerUnit, 'g', 6));
}

QString number(std::size_t n) {
    return QLocale().toString(static_cast<qulonglong>(n));
}

QString statsText(std::size_t objects, std::size_t triangles) {
    return QObject::tr("%1 · %2")
        .arg(ui::count(static_cast<int>(objects), QObject::tr("1 object"), QObject::tr("%1 objects")),
             triangles == 1 ? QObject::tr("1 triangle")
                            : QObject::tr("%1 triangles").arg(number(triangles)));
}

// Worker-thread state of one job's progress callback, used to throttle UI updates.
struct ProgressThrottle {
    std::chrono::steady_clock::time_point last;
    std::string stage;
};

struct ResourceSnapshot {
    std::unordered_set<ResourceId> meshes;
    std::unordered_set<ResourceId> materials;
    std::unordered_set<ResourceId> textures;
};

ResourceSnapshot snapshot(const Scene& scene) {
    ResourceSnapshot result;
    scene.forEachMesh([&](const MeshResource& r) { result.meshes.insert(r.id); });
    scene.forEachMaterial([&](const MaterialResource& r) { result.materials.insert(r.id); });
    scene.forEachTexture([&](const TextureResource& r) { result.textures.insert(r.id); });
    return result;
}

template <class Exists>
void removeUnused(Scene& scene, const std::vector<ResourceId>& ids, Exists exists) {
    for (const ResourceId id : ids) {
        if (exists(id) && scene.userCount(id) == 0) {
            scene.removeResource(id);
        }
    }
}

QString fileNames(const QStringList& paths) {
    if (paths.size() == 1) {
        return QFileInfo(paths.front()).fileName();
    }
    return ui::count(static_cast<int>(paths.size()), QObject::tr("1 file"), QObject::tr("%1 files"));
}

} // namespace

ImportPanel::ImportPanel(Scene& scene, Selection& selection, ViewportWindow& viewport,
                         QWidget* parent)
    : QWidget(parent), scene_(scene), selection_(selection), viewport_(viewport) {
    QVBoxLayout* layout = ui::scrollPage(this);
    QWidget* page = layout->parentWidget();

    // Actions first.
    auto* importModel =
        new QPushButton(theme::icon(QStringLiteral("download")), tr("Import model…"), page);
    importModel->setProperty("accent", true);
    importModel->setToolTip(tr("Add a model to the scene: SketchUp (export as Collada .dae), glTF, FBX, OBJ, KMZ, 3DS, "
                              "STL or PLY (Ctrl+I)."));
    layout->addWidget(importModel);
    auto* textureSet =
        new QPushButton(theme::icon(QStringLiteral("image-plus")), tr("Material from textures…"), page);
    textureSet->setToolTip(tr("Pick the images of one PBR set (color, normal, roughness…). They "
                              "become one material, applied to the selected objects."));
    layout->addWidget(textureSet);
    auto* hint = ui::label(tr("or drop files anywhere"), "muted", page);
    hint->setAlignment(Qt::AlignHCenter);
    hint->setToolTip(tr("Models: %1\nTextures: %2\nDrop several models to import them one after "
                        "another; dropped images become one material.")
                         .arg(extensionList(import::modelFileExtensions()),
                              extensionList(imageFileExtensions())));
    layout->addWidget(hint);

    // Progress, only while working.
    progressRow_ = new QWidget(page);
    auto* progressLayout = new QVBoxLayout(progressRow_);
    progressLayout->setContentsMargins(0, 4, 0, 4);
    progressLayout->setSpacing(4);
    progressStage_ = ui::label(QString(), "muted", progressRow_);
    progressStage_->setWordWrap(true);
    progressLayout->addWidget(progressStage_);
    auto* barRow = new QHBoxLayout;
    barRow->setSpacing(6);
    progressBar_ = new QProgressBar(progressRow_);
    progressBar_->setRange(0, 100);
    progressBar_->setTextVisible(false);
    barRow->addWidget(progressBar_, 1);
    cancelButton_ = new QPushButton(theme::icon(QStringLiteral("x")), tr("Cancel"), progressRow_);
    cancelButton_->setProperty("compact", true);
    cancelButton_->setToolTip(tr("Stop this import and skip the files still waiting."));
    barRow->addWidget(cancelButton_);
    progressLayout->addLayout(barRow);
    progressRow_->hide();
    layout->addWidget(progressRow_);
    layout->addSpacing(4);

    // Models imported this session.
    modelsSection_ = new ui::Section(tr("Models"), page);
    emptyHint_ = ui::label(tr("Models you import appear here."), "muted", modelsSection_);
    emptyHint_->setWordWrap(true);
    modelsSection_->addWide(emptyHint_);
    modelList_ = new QListWidget(modelsSection_);
    modelList_->setIconSize(QSize(theme::metrics().iconSize, theme::metrics().iconSize));
    modelList_->setSelectionMode(QAbstractItemView::SingleSelection);
    modelList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    modelList_->setToolTip(tr("Models imported in this session. Pick one to adjust its placement."));
    modelsSection_->addWide(modelList_);
    layout->addWidget(modelsSection_);

    // Placement of the picked model.
    placementSection_ = new ui::Section(tr("Placement"), page);
    units_ = new QComboBox(placementSection_);
    units_->setToolTip(tr("The length of one model unit. Changes the model's size, not its shape."));
    placementSection_->addRow(tr("Units"), units_)
        .label->setToolTip(tr("Pick the unit the model was drawn in if it looks too big or small."));
    upAxis_ = new ui::SegmentedControl({tr("Z up"), tr("Y up")}, placementSection_);
    placementSection_->addRow(tr("Up axis"), upAxis_)
        .label->setToolTip(tr("Choose Y up if the model lies on its back: some programs export "
                              "with Y pointing up."));
    auto* buttons = new QGridLayout;
    buttons->setSpacing(6);
    const auto addButton = [&](const char* icon, const QString& text, const QString& tip, int row,
                               int column) {
        auto* button =
            new QPushButton(theme::icon(QString::fromLatin1(icon)), text, placementSection_);
        button->setProperty("compact", true);
        button->setToolTip(tip);
        buttons->addWidget(button, row, column);
        return button;
    };
    QPushButton* frameButton =
        addButton("scan", tr("Frame"), tr("Fit the model into the view."), 0, 0);
    QPushButton* selectButton = addButton("mouse-pointer-2", tr("Select"),
                                          tr("Select the model to move it in Properties."), 0, 1);
    QPushButton* originButton =
        addButton("crosshair", tr("Place at origin"),
                  tr("Center the model on the origin and stand it on the ground."), 1, 0);
    QPushButton* removeButton = addButton("trash-2", tr("Remove"),
                                          tr("Delete the model and the materials and textures "
                                             "it brought in (unless other objects use them)."),
                                          1, 1);
    placementSection_->addWide(buttons);
    layout->addWidget(placementSection_);

    // What happened during the import.
    reportSection_ = new ui::Section(tr("Report"), page);
    reportFormat_ = ui::label(QString(), nullptr, reportSection_);
    reportSection_->addRow(tr("Format"), reportFormat_);
    reportFile_ = ui::label(QString(), nullptr, reportSection_);
    reportFile_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    reportSection_->addRow(tr("File"), reportFile_);
    reportStats_ = ui::label(QString(), nullptr, reportSection_);
    reportStats_->setWordWrap(true);
    reportSection_->addRow(tr("Contents"), reportStats_);
    reportTime_ = ui::label(QString(), nullptr, reportSection_);
    reportSection_->addRow(tr("Time"), reportTime_);
    reportUnits_ = ui::label(QString(), nullptr, reportSection_);
    reportUnits_->setWordWrap(true);
    reportSection_->addRow(tr("Units"), reportUnits_);
    reportWarnings_ = ui::label(QString(), "muted", reportSection_);
    reportWarnings_->setWordWrap(true);
    reportWarnings_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    reportSection_->addWide(reportWarnings_);
    layout->addWidget(reportSection_);
    layout->addStretch(1);

    connect(importModel, &QPushButton::clicked, this, &ImportPanel::chooseModels);
    connect(textureSet, &QPushButton::clicked, this, &ImportPanel::chooseTextureSet);
    connect(cancelButton_, &QPushButton::clicked, this, &ImportPanel::cancel);
    connect(modelList_, &QListWidget::currentRowChanged, this, [this] {
        if (!updating_) {
            refreshDetails();
        }
    });
    connect(units_, &QComboBox::activated, this,
            [this](int index) { setUnits(units_->itemData(index).toDouble()); });
    connect(upAxis_, &ui::SegmentedControl::currentEdited, this,
            [this](int index) { setYUp(index == 1); });
    connect(frameButton, &QPushButton::clicked, this, &ImportPanel::frameSelected);
    connect(selectButton, &QPushButton::clicked, this, [this] {
        if (const Record* record = selectedRecord()) {
            selection_.select(record->root);
        }
    });
    connect(originButton, &QPushButton::clicked, this, &ImportPanel::placeAtOrigin);
    connect(removeButton, &QPushButton::clicked, this, &ImportPanel::removeSelected);

    listener_ = scene_.addListener([this](const SceneChange& change) {
        if (change.kind == SceneChange::Kind::EntityRemoved) {
            // Models deleted elsewhere (outliner, Remove) drop out of the list.
            const auto gone = std::erase_if(
                records_, [this](const Record& r) { return scene_.entity(r.root) == nullptr; });
            if (gone > 0) {
                refreshList();
            }
        } else if (change.kind == SceneChange::Kind::TransformChanged && !updating_) {
            if (const Record* record = selectedRecord(); record && record->root == change.entity) {
                refreshDetails();
            }
        }
    });
    refreshList();
}

ImportPanel::~ImportPanel() {
    scene_.removeListener(listener_);
    if (cancel_) {
        cancel_->store(true); // the worker stops early; its result is dropped (panel is gone)
    }
}

QStringList ImportPanel::importablePaths(const QList<QUrl>& urls) {
    QStringList paths;
    for (const QUrl& url : urls) {
        if (url.isLocalFile()) {
            const QString path = url.toLocalFile();
            if (isImportable(path)) {
                paths << path;
            }
        }
    }
    return paths;
}

bool ImportPanel::isModelPath(const QString& path) {
    return import::isModelFile(toPath(path));
}

bool ImportPanel::isImportable(const QString& path) {
    const std::filesystem::path file = toPath(path);
    return import::isModelFile(file) || import::isTextureSource(file);
}

void ImportPanel::importFiles(const QStringList& paths, bool switchMode) {
    QStringList images;
    QStringList skipped;
    const std::optional<glm::vec2> dropPoint = viewport_.takeDropPoint();
    for (const QString& path : paths) {
        const std::filesystem::path file = toPath(path);
        if (isModelPath(path)) {
            queue_.push_back(Job{JobKind::Model, {path}, switchMode, dropPoint});
        } else if (isImageFile(file)) {
            images << path; // loose images together make one material
        } else if (import::isTextureSource(file)) {
            // A downloaded pack (zip or folder) is one material of its own.
            queue_.push_back(Job{JobKind::TextureSet, {path}, switchMode, {}});
        } else {
            skipped << QFileInfo(path).fileName();
        }
    }
    if (!images.isEmpty()) {
        queue_.push_back(Job{JobKind::TextureSet, images, switchMode, {}});
    }
    if (!skipped.isEmpty()) {
        emit statusMessage(tr("Skipped %1: LightBox can't import this kind of file yet.")
                               .arg(skipped.join(QStringLiteral(", "))));
    }
    startNext();
}

void ImportPanel::chooseModels() {
    QSettings settings;
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Import Model"), settings.value(kLastDirectoryKey).toString(),
        tr("3D models (%1);;All files (*)").arg(patterns(import::modelFileExtensions())));
    if (files.isEmpty()) {
        return;
    }
    settings.setValue(kLastDirectoryKey, QFileInfo(files.front()).absolutePath());
    importFiles(files);
}

void ImportPanel::chooseTextureSet() {
    QSettings settings;
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Material from Textures"), settings.value(kLastDirectoryKey).toString(),
        tr("Texture images or downloaded packs (%1 *.zip)").arg(patterns(imageFileExtensions())));
    if (files.isEmpty()) {
        return;
    }
    settings.setValue(kLastDirectoryKey, QFileInfo(files.front()).absolutePath());
    importFiles(files);
}

void ImportPanel::startNext() {
    if (current_ || queue_.empty()) {
        return;
    }
    current_ = queue_.front();
    queue_.pop_front();
    const Job job = *current_;
    const std::uint64_t serial = ++jobSerial_;
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    jobTimer_.start();

    progressStage_->setText(job.kind == JobKind::Model
                                ? tr("Importing %1").arg(fileNames(job.paths))
                                : tr("Loading %1").arg(fileNames(job.paths)));
    progressBar_->setValue(0);
    cancelButton_->setEnabled(true);
    progressRow_->show();
    emit busyChanged(true);
    emit progressChanged(0);

    // Called on the worker thread: throttled there, then queued to the UI thread through the
    // application object, because this panel may be destroyed while the import runs.
    QPointer<ImportPanel> guard(this);
    auto throttle = std::make_shared<ProgressThrottle>();
    import::Progress progress = [guard, serial, throttle](float fraction, std::string_view stage) {
        const auto now = std::chrono::steady_clock::now();
        if (stage == throttle->stage && now - throttle->last < kProgressInterval) {
            return;
        }
        throttle->last = now;
        throttle->stage.assign(stage);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [guard, serial, fraction, text = fromUtf8(stage)] {
                if (guard) {
                    guard->onProgress(serial, fraction, text);
                }
            },
            Qt::QueuedConnection);
    };

    std::vector<std::filesystem::path> files;
    files.reserve(static_cast<std::size_t>(job.paths.size()));
    for (const QString& path : job.paths) {
        files.push_back(toPath(path));
    }
    runInBackground<import::ImportedModel>(
        this,
        [kind = job.kind, files = std::move(files), progress = std::move(progress),
         cancelFlag = cancel_] {
            if (kind == JobKind::Model) {
                return import::importModel(files.front(), progress, cancelFlag.get());
            }
            return import::importTextureSet(files, progress, cancelFlag.get());
        },
        [this](import::ImportedModel model) {
            const Job finished = *current_;
            std::exception_ptr error;
            if (cancel_->load()) {
                error = std::make_exception_ptr(import::ImportCancelled());
            } else {
                try {
                    if (finished.kind == JobKind::Model) {
                        addModel(finished, std::move(model));
                    } else {
                        addTextureSet(finished, model);
                    }
                } catch (...) {
                    error = std::current_exception();
                }
            }
            finishJob();
            if (error) {
                showFailure(finished, error);
            }
        },
        [this](std::exception_ptr error) {
            const Job failed = *current_;
            finishJob();
            showFailure(failed, error);
        });
}

void ImportPanel::finishJob() {
    current_.reset();
    cancel_.reset();
    if (queue_.empty()) {
        progressRow_->hide();
        emit busyChanged(false);
    } else {
        startNext();
    }
}

void ImportPanel::onProgress(std::uint64_t job, float fraction, const QString& stage) {
    if (job != jobSerial_ || !current_ || (cancel_ && cancel_->load())) {
        return;
    }
    const int percent = std::clamp(static_cast<int>(std::lround(fraction * 100.0f)), 0, 100);
    progressBar_->setValue(percent);
    progressStage_->setText(QStringLiteral("%1 · %2").arg(fileNames(current_->paths), stage));
    emit progressChanged(percent);
}

void ImportPanel::cancel() {
    if (!cancel_) {
        return;
    }
    cancel_->store(true);
    queue_.clear();
    cancelButton_->setEnabled(false);
    progressStage_->setText(tr("Cancelling…"));
}

void ImportPanel::addModel(const Job& job, import::ImportedModel model) {
    const ResourceSnapshot before = snapshot(scene_);
    const import::PlacedModel placed = import::addToScene(scene_, model);

    Record record;
    record.root = placed.root;
    record.name = QString::fromStdString(model.name);
    record.format = QString::fromStdString(model.format);
    record.path = QDir::toNativeSeparators(job.paths.front());
    record.objects = placed.objects;
    record.triangles = placed.triangles;
    record.materialCount = placed.materials;
    record.textureCount = placed.textures;
    record.seconds = static_cast<double>(jobTimer_.elapsed()) / 1000.0;
    record.fileMetersPerUnit = model.metersPerUnit;
    record.unitsFromFile = model.unitsFromFile;
    for (const std::string& warning : model.warnings) {
        record.warnings << QString::fromStdString(warning);
        log::warn("Import {}: {}", model.name, warning);
    }
    scene_.forEachMesh([&](const MeshResource& r) {
        if (!before.meshes.contains(r.id)) {
            record.meshes.push_back(r.id);
        }
    });
    scene_.forEachMaterial([&](const MaterialResource& r) {
        if (!before.materials.contains(r.id)) {
            record.materials.push_back(r.id);
        }
    });
    scene_.forEachTexture([&](const TextureResource& r) {
        if (!before.textures.contains(r.id)) {
            record.textures.push_back(r.id);
        }
    });
    records_.push_back(std::move(record));
    const Record& added = records_.back();
    log::info("Imported {} ({}): {} objects, {} triangles, {} materials, {} textures in {:.2f} s",
              model.sourcePath, model.format, added.objects, added.triangles, added.materialCount,
              added.textureCount, added.seconds);

    refreshList();
    updating_ = true;
    modelList_->setCurrentRow(modelList_->count() - 1);
    updating_ = false;
    refreshDetails();

    // Stand it upright when the file can't say which way is up, then set it on the ground: where it
    // was dropped, else beside what is already there (1 m apart), else at the origin.
    bool stoodUp = false;
    if (!model.axesFromFile) {
        if (const std::optional<glm::quat> turn = uprightCorrection(scene_, added.root)) {
            Transform transform = scene_.entity(added.root)->transform;
            transform.rotation = glm::normalize(*turn * transform.rotation);
            scene_.setTransform(added.root, transform);
            stoodUp = true;
        }
    }
    glm::vec2 at(0.0f);
    if (job.dropPoint) {
        at = *job.dropPoint;
    } else {
        std::vector<EntityId> others;
        for (const EntityId root : scene_.roots()) {
            if (root != added.root) {
                others.push_back(root);
            }
        }
        const std::optional<Bounds> existing = worldBoundsOf(scene_, others);
        const std::optional<Bounds> own = worldBoundsOf(scene_, {&added.root, 1});
        if (existing && own) {
            at = glm::vec2(existing->max.x + 1.0f + own->size().x * 0.5f, existing->center().y);
        }
    }
    placeOnGround(scene_, added.root, at);

    selection_.select(added.root);
    if (const std::optional<Bounds> bounds = worldBoundsOf(scene_, {&added.root, 1})) {
        viewport_.frameBounds(*bounds);
    }
    QString message =
        tr("Imported %1: %2, %3 in %4 s")
            .arg(added.name,
                 ui::count(static_cast<int>(added.objects), tr("1 object"), tr("%1 objects")),
                 ui::count(static_cast<int>(added.materialCount), tr("1 material"),
                           tr("%1 materials")),
                 QLocale().toString(added.seconds, 'f', 1));
    if (stoodUp) {
        message += tr(" · stood upright (the file doesn't say which way is up)");
    }
    if (!added.warnings.isEmpty()) {
        message += tr(" · %1 (see Report)")
                       .arg(ui::count(static_cast<int>(added.warnings.size()), tr("1 warning"),
                                      tr("%1 warnings")));
    }
    emit statusMessage(message);
}

void ImportPanel::addTextureSet(const Job& job, const import::ImportedModel& model) {
    const ResourceId material = import::addMaterialToScene(scene_, model);
    const MaterialResource* resource = scene_.material(material);
    const QString name = resource != nullptr ? QString::fromStdString(resource->name)
                                             : QString::fromStdString(model.name);
    for (const std::string& warning : model.warnings) {
        log::warn("Texture set {}: {}", model.name, warning);
    }

    // Selected objects (and everything inside selected groups, e.g. an imported model) get it.
    const auto applied = static_cast<int>(applyMaterial(scene_, selection_.ids(), material));

    const QString textures =
        ui::count(static_cast<int>(model.textures.size()), tr("1 texture"), tr("%1 textures"));
    QString message =
        applied > 0
            ? tr("Material %1 created from %2 and applied to %3")
                  .arg(name, textures, ui::count(applied, tr("1 object"), tr("%1 objects")))
            : tr("Material %1 created from %2").arg(name, textures);
    if (!model.warnings.empty()) {
        message += QStringLiteral(" · ") + QString::fromStdString(model.warnings.front());
    }
    log::info("Created material {} from {} images", name.toStdString(), job.paths.size());
    emit statusMessage(message);
    if (job.switchMode) {
        emit materialImported();
    }
}

void ImportPanel::showFailure(const Job& job, std::exception_ptr error) {
    const QString what = fileNames(job.paths);
    try {
        std::rethrow_exception(error);
    } catch (const import::ImportCancelled&) {
        emit statusMessage(tr("Import of %1 cancelled").arg(what));
    } catch (const std::exception& e) {
        log::error("Import of {} failed: {}", job.paths.join(QStringLiteral(", ")).toStdString(),
                   e.what());
        emit statusMessage(tr("Could not import %1").arg(what));
        QMessageBox::warning(this, tr("Import failed"),
                             tr("LightBox could not import %1.\n\n%2")
                                 .arg(what, QString::fromUtf8(e.what())));
    } catch (...) {
        log::error("Import of {} failed with an unknown error",
                   job.paths.join(QStringLiteral(", ")).toStdString());
        emit statusMessage(tr("Could not import %1").arg(what));
        QMessageBox::warning(this, tr("Import failed"),
                             tr("LightBox could not import %1 because of an unexpected error.")
                                 .arg(what));
    }
}

ImportPanel::Record* ImportPanel::selectedRecord() {
    const int row = modelList_->currentRow();
    if (row < 0 || row >= static_cast<int>(records_.size())) {
        return nullptr;
    }
    return &records_[static_cast<std::size_t>(row)];
}

void ImportPanel::refreshList() {
    const Record* current = selectedRecord();
    const EntityId currentRoot = current != nullptr ? current->root : EntityId{};

    updating_ = true;
    modelList_->clear();
    int currentRow = -1;
    for (const Record& record : records_) {
        auto* item = new QListWidgetItem(
            theme::icon(QStringLiteral("file-box")),
            QStringLiteral("%1\n%2 · %3")
                .arg(record.name, record.format, statsText(record.objects, record.triangles)),
            modelList_);
        item->setToolTip(record.path);
        if (record.root == currentRoot) {
            currentRow = modelList_->count() - 1;
        }
    }
    if (currentRow < 0 && !records_.empty()) {
        currentRow = static_cast<int>(records_.size()) - 1;
    }
    modelList_->setCurrentRow(currentRow);
    updating_ = false;

    // Size the list to its rows (it sits inside the scrolling panel).
    const int rows = std::min(modelList_->count(), kMaxVisibleModels);
    if (rows > 0) {
        modelList_->setFixedHeight(rows * modelList_->sizeHintForRow(0) +
                                   2 * modelList_->frameWidth() + 2);
    }
    modelList_->setVisible(!records_.empty());
    emptyHint_->setVisible(records_.empty());
    refreshDetails();
}

void ImportPanel::refreshDetails() {
    const Record* record = selectedRecord();
    const Entity* root = record != nullptr ? scene_.entity(record->root) : nullptr;
    placementSection_->setVisible(root != nullptr);
    reportSection_->setVisible(root != nullptr);
    if (root == nullptr) {
        return;
    }

    // Units: the common choices plus the file's own unit when it is none of them. The current
    // choice is read back from the root's scale, so edits made in Properties show up here.
    const double scale = static_cast<double>(root->transform.scale.x);
    const bool uniform = root->transform.scale.x == root->transform.scale.y &&
                         root->transform.scale.x == root->transform.scale.z;
    units_->clear();
    bool fileUnitListed = false;
    for (const import::UnitChoice& choice : import::unitChoices()) {
        units_->addItem(fromUtf8(choice.name), choice.metersPerUnit);
        fileUnitListed = fileUnitListed || sameUnit(choice.metersPerUnit, record->fileMetersPerUnit);
    }
    if (!fileUnitListed) {
        units_->addItem(record->unitsFromFile
                            ? tr("From file (%1)").arg(unitText(record->fileMetersPerUnit))
                            : tr("Guessed (%1)").arg(unitText(record->fileMetersPerUnit)),
                        record->fileMetersPerUnit);
    }
    int unitIndex = -1;
    for (int i = 0; i < units_->count() && uniform; ++i) {
        if (sameUnit(units_->itemData(i).toDouble(), scale)) {
            unitIndex = i;
            break;
        }
    }
    units_->setCurrentIndex(unitIndex);
    upAxis_->setCurrent(record->yUp ? 1 : 0);

    reportFormat_->setText(record->format);
    reportFile_->setText(
        reportFile_->fontMetrics().elidedText(record->path, Qt::ElideMiddle, kFileLabelWidth));
    reportFile_->setToolTip(record->path);
    reportStats_->setText(
        tr("%1\n%2, %3")
            .arg(statsText(record->objects, record->triangles),
                 ui::count(static_cast<int>(record->materialCount), tr("1 material"),
                           tr("%1 materials")),
                 ui::count(static_cast<int>(record->textureCount), tr("1 texture"),
                           tr("%1 textures"))));
    reportTime_->setText(tr("%1 s").arg(QLocale().toString(record->seconds, 'f', 2)));
    reportUnits_->setText(record->unitsFromFile
                              ? tr("Units from file (%1)").arg(unitText(record->fileMetersPerUnit))
                              : tr("Guessed — check (%1)").arg(unitText(record->fileMetersPerUnit)));
    reportUnits_->setToolTip(record->unitsFromFile
                                 ? tr("The file states its unit.")
                                 : tr("The file doesn't state its unit; LightBox guessed it from "
                                      "the model's size. Change Units above if the size is off."));
    if (record->warnings.isEmpty()) {
        reportWarnings_->setText(tr("No warnings."));
    } else {
        QStringList lines;
        for (int i = 0; i < record->warnings.size() && i < kMaxListedWarnings; ++i) {
            lines << QStringLiteral("• ") + record->warnings[i];
        }
        if (record->warnings.size() > kMaxListedWarnings) {
            lines << tr("…and %1 more").arg(record->warnings.size() - kMaxListedWarnings);
        }
        reportWarnings_->setText(lines.join(QLatin1Char('\n')));
    }
}

void ImportPanel::setUnits(double metersPerUnit) {
    const Record* record = selectedRecord();
    const Entity* root = record != nullptr ? scene_.entity(record->root) : nullptr;
    if (root == nullptr || metersPerUnit <= 0.0) {
        return;
    }
    Transform transform = root->transform;
    transform.scale = glm::vec3(static_cast<float>(metersPerUnit));
    scene_.setTransform(record->root, transform);
}

void ImportPanel::setYUp(bool yUp) {
    Record* record = selectedRecord();
    const Entity* root = record != nullptr ? scene_.entity(record->root) : nullptr;
    if (root == nullptr || record->yUp == yUp) {
        return;
    }
    // +90° about X turns the file's +Y into +Z. The model keeps its footprint center and stays on
    // the same ground level, so the turn doesn't send it underground.
    const std::optional<Bounds> before = worldBoundsOf(scene_, {&record->root, 1});
    Transform transform = root->transform;
    const float angle = glm::radians(yUp ? 90.0f : -90.0f);
    transform.rotation =
        glm::normalize(glm::angleAxis(angle, glm::vec3(1.0f, 0.0f, 0.0f)) * transform.rotation);
    record->yUp = yUp;
    updating_ = true;
    scene_.setTransform(record->root, transform);
    if (const std::optional<Bounds> after = worldBoundsOf(scene_, {&record->root, 1});
        before && after) {
        const glm::vec3 offset(before->center().x - after->center().x,
                               before->center().y - after->center().y,
                               before->min.z - after->min.z);
        moveBy(scene_, record->root, offset);
    }
    updating_ = false;
    refreshDetails();
}

void ImportPanel::frameSelected() {
    if (const Record* record = selectedRecord()) {
        if (const std::optional<Bounds> bounds = worldBoundsOf(scene_, {&record->root, 1})) {
            viewport_.frameBounds(*bounds);
        }
    }
}

void ImportPanel::placeAtOrigin() {
    const Record* record = selectedRecord();
    if (record == nullptr) {
        return;
    }
    placeOnGround(scene_, record->root, glm::vec2(0.0f));
}

void ImportPanel::removeSelected() {
    const Record* selected = selectedRecord();
    if (selected == nullptr) {
        return;
    }
    // Copy first: destroying the root drops the record through the scene listener.
    const Record record = *selected;
    if (scene_.entity(record.root) != nullptr) {
        scene_.destroyEntity(record.root);
    }
    // Meshes, then materials, then textures, so each is unused once its users are gone. Anything
    // the user has since used elsewhere stays.
    removeUnused(scene_, record.meshes,
                 [this](ResourceId id) { return scene_.mesh(id) != nullptr; });
    removeUnused(scene_, record.materials,
                 [this](ResourceId id) { return scene_.material(id) != nullptr; });
    removeUnused(scene_, record.textures,
                 [this](ResourceId id) { return scene_.texture(id) != nullptr; });
    std::erase_if(records_, [&](const Record& r) { return r.root == record.root; });
    refreshList();
    emit statusMessage(tr("Removed %1").arg(record.name));
}

} // namespace lb::app
