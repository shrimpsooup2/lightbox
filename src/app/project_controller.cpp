#include "app/project_controller.hpp"

#include <exception>
#include <filesystem>
#include <utility>
#include <vector>

#include <QBuffer>
#include <QByteArray>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>

#include "app/background.hpp"
#include "app/viewport_window.hpp"
#include "core/camera.hpp"
#include "core/log.hpp"
#include "core/sample_scene.hpp"
#include "project/project_file.hpp"

namespace lb::app {
namespace {

constexpr const char* kRecentKey = "projects/recent";
constexpr qsizetype kMaxRecent = 12;
constexpr QSize kThumbnailSize(480, 270);

QString extension() {
    return QString::fromLatin1(project::kFileExtension.data(),
                               static_cast<qsizetype>(project::kFileExtension.size()));
}

std::filesystem::path toPath(const QString& path) {
    return std::filesystem::path(path.toStdU16String());
}

Qt::CaseSensitivity pathCase() {
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

QString normalized(const QString& path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

// Documents/LightBox Projects: where Save As starts for new projects.
QString defaultFolder() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
        .filePath(QStringLiteral("LightBox Projects"));
}

// Project names become suggested file names; characters Windows forbids in names are replaced.
QString fileNameFor(const QString& name) {
    QString result = name.trimmed();
    for (QChar& c : result) {
        if (QStringLiteral("<>:\"/\\|?*").contains(c) || c.unicode() < 0x20) {
            c = QLatin1Char('-');
        }
    }
    return result.isEmpty() ? QObject::tr("Project") : result;
}

// Start-screen preview: the current viewport frame, center-cropped to 16:9.
std::vector<std::uint8_t> thumbnailPng(const QImage& frame) {
    if (frame.isNull()) {
        return {};
    }
    const int width = frame.width();
    const int height = frame.height();
    QRect crop(0, 0, width, height);
    if (width * 9 > height * 16) {
        const int cropWidth = height * 16 / 9;
        crop = QRect((width - cropWidth) / 2, 0, cropWidth, height);
    } else {
        const int cropHeight = width * 9 / 16;
        crop = QRect(0, (height - cropHeight) / 2, width, cropHeight);
    }
    QImage thumbnail =
        frame.copy(crop).scaled(kThumbnailSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    thumbnail.setDevicePixelRatio(1.0);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!thumbnail.save(&buffer, "PNG")) {
        log::warn("Could not encode the project thumbnail");
        return {};
    }
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.constData());
    return std::vector<std::uint8_t>(data, data + bytes.size());
}

// Plain-language reason a project could not be opened. ProjectError messages already name the file.
QString openErrorText(const QString& path, const std::exception_ptr& error) {
    const QString file = QFileInfo(path).fileName();
    try {
        std::rethrow_exception(error);
    } catch (const project::ProjectError& e) {
        return QString::fromUtf8(e.what());
    } catch (const std::exception& e) {
        return QObject::tr("“%1” could not be opened because it is damaged.\n\n%2")
            .arg(file, QString::fromUtf8(e.what()));
    } catch (...) {
        return QObject::tr("“%1” could not be opened.").arg(file);
    }
}

class WaitCursor {
public:
    WaitCursor() { QGuiApplication::setOverrideCursor(Qt::WaitCursor); }
    ~WaitCursor() { QGuiApplication::restoreOverrideCursor(); }
    WaitCursor(const WaitCursor&) = delete;
    WaitCursor& operator=(const WaitCursor&) = delete;
};

} // namespace

ProjectController::ProjectController(Scene& scene, ViewportWindow& viewport, QWidget* window)
    : QObject(window), scene_(scene), viewport_(viewport), window_(window),
      name_(tr("Untitled project")) {
    listener_ = scene_.addListener([this](const SceneChange&) {
        if (!installing_ && !dirty_) {
            dirty_ = true;
            emit projectChanged();
        }
    });
}

ProjectController::~ProjectController() {
    scene_.removeListener(listener_);
}

template <class Install>
void ProjectController::installDocument(Install install) {
    installing_ = true;
    try {
        install();
    } catch (...) {
        installing_ = false;
        throw;
    }
    installing_ = false;
}

void ProjectController::setDocument(const QString& path, const QString& name) {
    path_ = path;
    name_ = name;
    dirty_ = false;
    emit projectChanged();
}

void ProjectController::newProject() {
    cancelLoad();
    installDocument([this] { scene_.clear(); });
    setDocument({}, tr("Untitled project"));
    viewport_.setCameraState(OrbitCamera().state());
    emit projectOpened();
}

void ProjectController::openSample() {
    cancelLoad();
    installDocument([this] {
        scene_.clear();
        addSampleMassing(scene_);
    });
    setDocument({}, tr("Sample house"));
    viewport_.setCameraState(OrbitCamera().state());
    viewport_.frameScene();
    emit projectOpened();
}

void ProjectController::closeProject() {
    cancelLoad();
    installDocument([this] { scene_.clear(); });
    setDocument({}, tr("Untitled project"));
}

void ProjectController::open(const QString& file) {
    const QString path = normalized(file);
    const QString fileName = QFileInfo(path).fileName();
    if (!QFileInfo::exists(path)) {
        const bool listed = recentProjects().contains(path, pathCase());
        if (!listed) {
            QMessageBox::warning(window_, tr("Open Project"),
                                 tr("LightBox can't find “%1”.").arg(QDir::toNativeSeparators(path)));
            return;
        }
        const auto answer = QMessageBox::question(
            window_, tr("Open Project"),
            tr("LightBox can't find “%1”. It may have been moved, renamed or deleted.\n\nRemove it "
               "from the recent projects?")
                .arg(QDir::toNativeSeparators(path)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer == QMessageBox::Yes) {
            removeRecent(path);
        }
        return;
    }

    const bool wasBusy = isBusy();
    const std::uint64_t serial = ++loadSerial_;
    if (!wasBusy) {
        emit busyChanged(true);
    }
    emit statusMessage(tr("Opening %1…").arg(fileName));
    log::info("Opening project {}", path.toStdString());
    runInBackground<project::LoadedProject>(
        this, [file = toPath(path)] { return project::loadProject(file); },
        [this, serial, path, fileName](project::LoadedProject loaded) {
            if (serial != loadSerial_ || !isBusy()) {
                return; // another document was installed meanwhile
            }
            std::exception_ptr error;
            try {
                installDocument([&] { scene_.replaceContents(std::move(loaded.scene)); });
            } catch (...) {
                error = std::current_exception();
            }
            finishLoad(serial);
            if (error) {
                const QString text = openErrorText(path, error);
                log::error("Could not install project {}: {}", path.toStdString(),
                           text.toStdString());
                QMessageBox::warning(window_, tr("Open Project"), text);
                return;
            }
            const QString name = loaded.info.name.empty()
                                     ? QFileInfo(path).completeBaseName()
                                     : QString::fromStdString(loaded.info.name);
            setDocument(path, name);
            if (loaded.info.camera) {
                viewport_.setCameraState(*loaded.info.camera);
            } else {
                viewport_.setCameraState(OrbitCamera().state());
                viewport_.frameScene();
            }
            addRecent(path);
            log::info("Opened project {} (format {})", path.toStdString(), loaded.formatVersion);
            emit statusMessage(tr("Opened %1").arg(fileName));
            emit projectOpened();
        },
        [this, serial, path](std::exception_ptr error) {
            if (serial != loadSerial_ || !isBusy()) {
                return;
            }
            finishLoad(serial);
            const QString text = openErrorText(path, error);
            log::error("Could not open project {}: {}", path.toStdString(), text.toStdString());
            emit statusMessage(QString());
            QMessageBox::warning(window_, tr("Open Project"), text);
        });
}

void ProjectController::cancelLoad() {
    if (isBusy()) {
        finishedSerial_ = loadSerial_;
        emit busyChanged(false);
    }
}

void ProjectController::finishLoad(std::uint64_t serial) {
    finishedSerial_ = serial;
    emit busyChanged(false);
}

bool ProjectController::save() {
    if (isUntitled()) {
        return saveAs();
    }
    return writeTo(path_, name_);
}

bool ProjectController::saveAs() {
    QString folder;
    if (isUntitled()) {
        folder = defaultFolder();
        QDir().mkpath(folder);
    } else {
        folder = QFileInfo(path_).absolutePath();
    }
    const QString suggested = QDir(folder).filePath(fileNameFor(name_) + extension());
    QString file = QFileDialog::getSaveFileName(window_, tr("Save Project As"), suggested,
                                                tr("LightBox project (*%1)").arg(extension()));
    if (file.isEmpty()) {
        return false;
    }
    if (!isProjectFile(file)) {
        file += extension();
    }
    return writeTo(normalized(file), QFileInfo(file).completeBaseName());
}

bool ProjectController::writeTo(const QString& path, const QString& name) {
    project::ProjectInfo info;
    info.name = name.toStdString();
    info.camera = viewport_.cameraState();
    QString error;
    {
        // Saving runs on the UI thread for now (D-022); the cursor says the pause is expected.
        const WaitCursor wait;
        info.thumbnailPng = thumbnailPng(viewport_.grabFrame());
        try {
            project::saveProject(scene_, info, toPath(path));
        } catch (const std::exception& e) {
            error = QString::fromUtf8(e.what());
            if (error.isEmpty()) {
                error = tr("Unknown error.");
            }
        }
    }
    if (!error.isEmpty()) {
        log::error("Could not save project {}: {}", path.toStdString(), error.toStdString());
        QMessageBox::warning(window_, tr("Save Project"),
                             tr("LightBox could not save “%1”.\n\n%2")
                                 .arg(QDir::toNativeSeparators(path), error));
        return false;
    }
    log::info("Saved project {}", path.toStdString());
    setDocument(path, name);
    addRecent(path);
    emit statusMessage(tr("Saved %1").arg(QDir::toNativeSeparators(path)));
    return true;
}

bool ProjectController::maybeSave() {
    if (!dirty_) {
        return true;
    }
    QMessageBox box(QMessageBox::Warning, tr("Unsaved Changes"),
                    tr("Save changes to “%1” before closing?").arg(name_),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, window_);
    box.setInformativeText(tr("Your changes will be lost if you don't save them."));
    box.button(QMessageBox::Discard)->setText(tr("Don't Save"));
    box.setDefaultButton(QMessageBox::Save);
    switch (box.exec()) {
    case QMessageBox::Save:
        return save();
    case QMessageBox::Discard:
        return true;
    default:
        return false;
    }
}

QString ProjectController::chooseProjectFile() {
    QString folder;
    if (!isUntitled()) {
        folder = QFileInfo(path_).absolutePath();
    } else if (const QStringList recent = recentProjects(); !recent.isEmpty()) {
        folder = QFileInfo(recent.front()).absolutePath();
    } else if (QFileInfo::exists(defaultFolder())) {
        folder = defaultFolder();
    } else {
        folder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    }
    return QFileDialog::getOpenFileName(window_, tr("Open Project"), folder,
                                        tr("LightBox project (*%1)").arg(extension()));
}

QStringList ProjectController::recentProjects() const {
    return QSettings().value(kRecentKey).toStringList();
}

void ProjectController::addRecent(const QString& path) {
    const QString file = normalized(path);
    QStringList list = recentProjects();
    list.removeIf([&](const QString& p) { return p.compare(file, pathCase()) == 0; });
    list.prepend(file);
    if (list.size() > kMaxRecent) {
        list.resize(kMaxRecent);
    }
    storeRecent(list);
}

void ProjectController::removeRecent(const QString& path) {
    const QString file = normalized(path);
    QStringList list = recentProjects();
    if (list.removeIf([&](const QString& p) { return p.compare(file, pathCase()) == 0; }) > 0) {
        storeRecent(list);
    }
}

void ProjectController::clearRecent() {
    storeRecent({});
}

void ProjectController::storeRecent(const QStringList& paths) {
    QSettings().setValue(kRecentKey, paths);
    emit recentChanged();
}

bool ProjectController::isProjectFile(const QString& path) {
    return path.endsWith(extension(), Qt::CaseInsensitive);
}

} // namespace lb::app
