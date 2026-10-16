#include <exception>
#include <filesystem>
#include <memory>

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QVulkanInstance>

#include "app/design_gallery.hpp"
#include "app/main_window.hpp"
#include "app/project_controller.hpp"
#include "app/theme.hpp"
#include "core/log.hpp"
#include "core/scene/scene.hpp"
#include "render/renderer.hpp"

namespace {

void routeQtMessages(QtMsgType type, const QMessageLogContext&, const QString& message) {
    const std::string text = message.toStdString();
    switch (type) {
    case QtDebugMsg:
        lb::log::debug("[qt] {}", text);
        break;
    case QtInfoMsg:
        lb::log::info("[qt] {}", text);
        break;
    case QtWarningMsg:
        lb::log::warn("[qt] {}", text);
        break;
    case QtCriticalMsg:
    case QtFatalMsg:
        lb::log::error("[qt] {}", text);
        break;
    }
}

void showStartupError(const QString& details) {
    QMessageBox::critical(nullptr, QStringLiteral("LightBox"),
                          QObject::tr("LightBox could not start its 3D engine.\n\n%1").arg(details));
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication::setApplicationName(QStringLiteral("LightBox"));
    QApplication::setOrganizationName(QStringLiteral("LightBox"));
    QApplication::setApplicationVersion(QStringLiteral(LIGHTBOX_VERSION));
    QApplication app(argc, argv);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("LightBox — open-source architectural rendering"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption validationOption(
        QStringLiteral("validation"),
        QStringLiteral("Enable Vulkan validation layers (requires the Vulkan SDK)."));
    const QCommandLineOption screenshotOption(
        QStringLiteral("screenshot"),
        QStringLiteral("Save a PNG of the main window shortly after startup, then quit."),
        QStringLiteral("file"));
    const QCommandLineOption screenshotDelayOption(
        QStringLiteral("screenshot-delay"),
        QStringLiteral("Milliseconds to wait before --screenshot (default 2000), e.g. to script UI "
                       "interaction first."),
        QStringLiteral("ms"), QStringLiteral("2000"));
    const QCommandLineOption quitAfterOption(
        QStringLiteral("quit-after"), QStringLiteral("Quit automatically after <ms> milliseconds."),
        QStringLiteral("ms"));
    const QCommandLineOption themeOption(
        QStringLiteral("theme"), QStringLiteral("Start with the 'dark' or 'light' theme."),
        QStringLiteral("name"));
    const QCommandLineOption galleryOption(
        QStringLiteral("gallery"),
        QStringLiteral("Open the design gallery (UI controls in the current theme) instead of the app."));
    const QCommandLineOption modeOption(
        QStringLiteral("mode"),
        QStringLiteral("Start in a workflow mode: import, build, materials, site, light, render. "
                       "Without --open/--new/--import, opens the sample project."),
        QStringLiteral("name"));
    const QCommandLineOption importOption(
        QStringLiteral("import"),
        QStringLiteral("Import a model (.gltf, .glb, .obj, .fbx) or texture image after startup, as "
                       "if dropped on the window. Repeatable: models import one after another, "
                       "images become one material. Without --open/--sample, starts a new "
                       "project. Keeps the --mode workflow mode if given."),
        QStringLiteral("file"));
    const QCommandLineOption openOption(
        QStringLiteral("open"), QStringLiteral("Open a LightBox project (.lbox) at startup."),
        QStringLiteral("file"));
    const QCommandLineOption newOption(
        QStringLiteral("new"), QStringLiteral("Start with a new, empty project (Import mode)."));
    const QCommandLineOption sampleOption(QStringLiteral("sample"),
                                          QStringLiteral("Start with the sample project."));
    parser.addOptions({validationOption, screenshotOption, screenshotDelayOption, quitAfterOption,
                       themeOption, galleryOption, modeOption, importOption, openOption, newOption,
                       sampleOption});
    parser.process(app);

    const QString logDirectory =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
        QStringLiteral("/logs");
    lb::log::setFile(std::filesystem::path(logDirectory.toStdWString()) / "lightbox.log");
    qInstallMessageHandler(routeQtMessages);
    lb::log::info("LightBox {} starting (Qt {})", LIGHTBOX_VERSION, qVersion());

    // Theme: command line wins over the saved preference; dark is the default.
    const QString themeName = parser.isSet(themeOption)
                                  ? parser.value(themeOption)
                                  : QSettings().value(QStringLiteral("ui/theme")).toString();
    lb::app::theme::apply(app, lb::app::theme::parseMode(themeName).value_or(lb::app::theme::Mode::Dark));
    app.setWindowIcon(lb::app::theme::appIcon());

    if (parser.isSet(galleryOption)) {
        lb::app::DesignGallery gallery;
        gallery.show();
        if (parser.isSet(screenshotOption)) {
            const QString path = parser.value(screenshotOption);
            QTimer::singleShot(parser.value(screenshotDelayOption).toInt(), &gallery, [&gallery, path] {
                QApplication::exit(gallery.saveScreenshot(path) ? 0 : 2);
            });
        }
        return app.exec();
    }

    std::unique_ptr<lb::render::Renderer> renderer;
    try {
        lb::render::RendererOptions options;
        options.enableValidation = parser.isSet(validationOption);
        renderer = std::make_unique<lb::render::Renderer>(options);
    } catch (const std::exception& e) {
        lb::log::error("Renderer initialization failed: {}", e.what());
        showStartupError(QString::fromUtf8(e.what()));
        return 1;
    }

    // Qt adopts the renderer's VkInstance only to create window surfaces.
    QVulkanInstance vulkan;
    vulkan.setVkInstance(renderer->vkInstance());
    if (!vulkan.create()) {
        lb::log::error("QVulkanInstance::create failed: {}", static_cast<int>(vulkan.errorCode()));
        showStartupError(QObject::tr("Qt could not use the Vulkan instance (error %1).")
                             .arg(static_cast<int>(vulkan.errorCode())));
        return 1;
    }

    int exitCode = 0;
    try {
        lb::Scene scene;
        lb::app::MainWindow window(*renderer, vulkan, scene);
        // Without a project flag LightBox opens on the start screen. Scripted runs pick a project:
        // --import starts a new one, --mode alone opens the sample (so screenshots show content).
        const bool importing = parser.isSet(importOption);
        if (parser.isSet(openOption)) {
            window.showStartScreen(); // stays visible if the project can't be opened
            window.openProject(QFileInfo(parser.value(openOption)).absoluteFilePath());
        } else if (parser.isSet(newOption) || (importing && !parser.isSet(sampleOption))) {
            window.newProject();
        } else if (parser.isSet(sampleOption) || parser.isSet(modeOption)) {
            window.openSample();
        } else {
            window.showStartScreen();
        }
        if (parser.isSet(modeOption)) {
            if (const auto mode = lb::app::workspaceModeFromKey(parser.value(modeOption))) {
                window.setMode(*mode);
            }
        }
        window.show();

        if (importing) {
            // After the window is shown, so the viewport exists and progress is visible; into an
            // opened project once it has loaded.
            QStringList files;
            for (const QString& file : parser.values(importOption)) {
                files << QFileInfo(file).absoluteFilePath();
            }
            const bool switchMode = !parser.isSet(modeOption);
            const auto startImport = [&window, files, switchMode] {
                window.importFiles(files, switchMode);
            };
            if (parser.isSet(openOption)) {
                QObject::connect(&window.projects(), &lb::app::ProjectController::projectOpened,
                                 &window, startImport, Qt::SingleShotConnection);
            } else {
                QTimer::singleShot(0, &window, startImport);
            }
        }
        if (parser.isSet(screenshotOption)) {
            const QString path = parser.value(screenshotOption);
            QTimer::singleShot(parser.value(screenshotDelayOption).toInt(), &window, [&window, path] {
                QApplication::exit(window.saveScreenshot(path) ? 0 : 2);
            });
        }
        if (parser.isSet(quitAfterOption)) {
            bool ok = false;
            const int ms = parser.value(quitAfterOption).toInt(&ok);
            if (ok && ms >= 0) {
                QTimer::singleShot(ms, &app, &QApplication::quit);
            }
        }
        exitCode = app.exec();
    } catch (const std::exception& e) {
        lb::log::error("Startup failed: {}", e.what());
        showStartupError(QString::fromUtf8(e.what()));
        return 1;
    }
    lb::log::info("LightBox exiting (code {})", exitCode);
    return exitCode;
}
