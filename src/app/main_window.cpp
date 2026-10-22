#include "app/main_window.hpp"

#include <algorithm>
#include <filesystem>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QGridLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QProgressBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "app/coming_soon.hpp"
#include "app/import_panel.hpp"
#include "app/inspector_panel.hpp"
#include "app/light_panel.hpp"
#include "app/materials_panel.hpp"
#include "app/outliner_panel.hpp"
#include "app/project_controller.hpp"
#include "app/render_panel.hpp"
#include "app/render_screen.hpp"
#include "app/start_screen.hpp"
#include "app/theme.hpp"
#include "app/ui_helpers.hpp"
#include "app/viewport_window.hpp"
#include "core/environment_map.hpp"
#include "core/log.hpp"

namespace lb::app {
namespace {

// Bump when docks are added/renamed or default sizes change, so stale saved layouts are ignored.
constexpr int kLayoutVersion = 6;

QString layoutKey(WorkspaceMode mode) {
    return QStringLiteral("workspace/layout/%1").arg(QLatin1String(workspaceModeInfo(mode).key));
}

// Local files a drop would open (`.lbox`) or import (models, images).
QStringList droppablePaths(const QList<QUrl>& urls) {
    QStringList paths;
    for (const QUrl& url : urls) {
        if (url.isLocalFile()) {
            const QString path = url.toLocalFile();
            if (ProjectController::isProjectFile(path) || ImportPanel::isImportable(path)) {
                paths << path;
            }
        }
    }
    return paths;
}

// Build mode has no tools yet; show how to get around instead of an empty panel.
QWidget* makeBuildPage(QWidget* parent) {
    QWidget* page = makeComingSoonPage(
        QStringLiteral("blocks"), QObject::tr("Asset library coming soon"),
        QObject::tr("You'll drag furniture, people, cars and trees straight into the scene. For now, "
                    "click an object to select it, then drag the arrows to move it, the rings to "
                    "turn it and the small squares to scale it (the middle one evenly)."),
        {}, parent);
    auto* layout = static_cast<QVBoxLayout*>(page->layout());
    layout->insertWidget(layout->count() - 1, ui::sectionHeader(QObject::tr("Moving around"), page));
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(6);
    const std::pair<QString, QString> controls[] = {
        {QObject::tr("Select"), QObject::tr("Click an object")},
        {QObject::tr("Move"), QObject::tr("Drag an arrow or the square")},
        {QObject::tr("Turn"), QObject::tr("Drag a ring")},
        {QObject::tr("Scale"), QObject::tr("Drag a small square")},
        {QObject::tr("Exact steps"), QObject::tr("Shift while dragging")},
        {QObject::tr("Orbit"), QObject::tr("Left or middle drag")},
        {QObject::tr("Pan"), QObject::tr("Right drag, or Shift + drag")},
        {QObject::tr("Zoom"), QObject::tr("Mouse wheel")},
        {QObject::tr("Walk"), QObject::tr("Footprints, then click")},
        {QObject::tr("Frame everything"), QObject::tr("F")},
        {QObject::tr("Reset the view"), QObject::tr("Home")},
    };
    int row = 0;
    for (const auto& [action, input] : controls) {
        grid->addWidget(ui::label(action, nullptr, page), row, 0);
        grid->addWidget(ui::label(input, "muted", page), row, 1);
        ++row;
    }
    grid->setColumnStretch(1, 1);
    layout->insertLayout(layout->count() - 1, grid);
    return page;
}

} // namespace

MainWindow::MainWindow(render::Renderer& renderer, QVulkanInstance& vulkan, Scene& scene,
                       QWidget* parent)
    : QMainWindow(parent),
      renderer_(renderer),
      scene_(scene),
      sceneSync_(std::make_unique<render::SceneSync>(scene, renderer)),
      selection_(std::make_unique<Selection>(scene)) {
    setWindowTitle(QStringLiteral("LightBox"));
    setDockOptions(QMainWindow::AnimatedDocks);
    resize(1440, 880);
    setAcceptDrops(true); // models, images and projects can be dropped anywhere on the window

    viewport_ = new ViewportWindow(renderer, vulkan, scene, *selection_, *sceneSync_);
    project_ = new ProjectController(scene, *viewport_, this);
    createActions();

    // Center: the workspace (viewport toolbar above the native Vulkan viewport; widgets cannot
    // overlap it) or the start screen.
    pages_ = new QStackedWidget(this);
    workspacePage_ = new QWidget(pages_);
    auto* centerLayout = new QVBoxLayout(workspacePage_);
    centerLayout->setContentsMargins(0, 0, 0, 0);
    centerLayout->setSpacing(0);
    auto* toolbar = new QToolBar(tr("Viewport"), workspacePage_);
    toolbar->setIconSize(QSize(theme::metrics().iconSize, theme::metrics().iconSize));
    toolbar->addAction(frameAction_);
    toolbar->addAction(resetViewAction_);
    toolbar->addSeparator();
    toolbar->addAction(gridAction_);
    toolbar->addAction(walkAction_);
    toolbar->addSeparator();
    toolbar->addActions(orbitGroup_->actions());
    toolbar->addSeparator();
    toolbar->addAction(rayAction_);
    centerLayout->addWidget(toolbar);
    viewportContainer_ = QWidget::createWindowContainer(viewport_, workspacePage_);
    viewportContainer_->setFocusPolicy(Qt::StrongFocus);
    viewportContainer_->setMinimumSize(320, 240);
    centerLayout->addWidget(viewportContainer_, 1);
    pages_->addWidget(workspacePage_);
    startScreen_ = new StartScreen(*project_, pages_);
    pages_->addWidget(startScreen_);
    renderScreen_ = new RenderScreen(renderer_, *viewport_, pages_);
    pages_->addWidget(renderScreen_);
    connect(renderScreen_, &RenderScreen::closeRequested, this, &MainWindow::closeRenderScreen);
    pages_->setCurrentWidget(workspacePage_);
    setCentralWidget(pages_);

    createHeader();
    createDocks(scene);
    createMenus();
    createStatusBar();
    loadWorkspaces();
    updateProjectChrome();

    connect(viewport_, &ViewportWindow::frameRendered, this, [this](double ms, QSize size) {
        frameLabel_->setText(QStringLiteral("%1 × %2  ·  %3 ms")
                                 .arg(size.width())
                                 .arg(size.height())
                                 .arg(ms, 0, 'f', 2));
    });
    connect(viewport_, &ViewportWindow::renderingFailed, this, [this](const QString& message) {
        QMessageBox::critical(this, QStringLiteral("LightBox"),
                              tr("The 3D viewport stopped because of a graphics error.\n\n%1")
                                  .arg(message));
    });
    connect(viewport_, &ViewportWindow::filesDropped, this, &MainWindow::openDroppedFiles);
    // Clicking an object in the viewport selects it (Materials then shows its material);
    // Shift/Ctrl+click adds or removes it, and keeps the selection on empty space.
    connect(viewport_, &ViewportWindow::entityClicked, this, [this](EntityId entity, bool add) {
        if (!add) {
            selection_->select(entity);
        } else if (entity) {
            selection_->toggle(entity);
        }
    });
    connect(viewport_, &ViewportWindow::hint, this, [this](const QString& text) {
        if (text.isEmpty()) {
            statusBar()->clearMessage();
        } else {
            statusBar()->showMessage(text);
        }
    });
    // Ray tracing progress in the status bar (it refines while the user works in any mode).
    rayLabel_ = new QLabel(this);
    rayLabel_->setProperty("muted", true);
    rayLabel_->hide();
    rayBar_ = new QProgressBar(this);
    rayBar_->setTextVisible(false);
    rayBar_->setFixedSize(120, 6);
    rayBar_->hide();
    statusBar()->addPermanentWidget(rayLabel_);
    statusBar()->addPermanentWidget(rayBar_);
    connect(viewport_, &ViewportWindow::rayTracingProgress, this,
            [this](const render::RayTracingStatus& status) {
                rayLabel_->setVisible(status.active);
                rayLabel_->setText(rayStatusText(status));
                const bool refining = status.active && !status.preparing &&
                                      status.samples < status.targetSamples;
                rayBar_->setVisible(refining);
                rayBar_->setRange(0, static_cast<int>(std::max(status.targetSamples, 1u)));
                rayBar_->setValue(static_cast<int>(status.samples));
            });
    connect(viewport_, &ViewportWindow::walkModeChanged, walkAction_, [this](bool on) {
        const QSignalBlocker block(walkAction_);
        walkAction_->setChecked(on);
    });
    connect(importPanel_, &ImportPanel::statusMessage, this,
            [this](const QString& text) { statusBar()->showMessage(text, 10000); });
    connect(viewport_, &ViewportWindow::skyImageFailed, this, [this](const QString& text) {
        statusBar()->showMessage(tr("The sky image could not be used: %1").arg(text), 15000);
    });
    connect(importPanel_, &ImportPanel::busyChanged, importProgress_, &QWidget::setVisible);
    connect(importPanel_, &ImportPanel::progressChanged, importProgress_, &QProgressBar::setValue);
    connect(importPanel_, &ImportPanel::materialImported, this,
            [this] { setMode(WorkspaceMode::Materials); });

    connect(project_, &ProjectController::projectChanged, this, &MainWindow::updateProjectChrome);
    connect(project_, &ProjectController::projectOpened, this, &MainWindow::showWorkspace);
    connect(project_, &ProjectController::statusMessage, this,
            [this](const QString& text) { statusBar()->showMessage(text, 10000); });
    connect(project_, &ProjectController::busyChanged, this, [this](bool busy) {
        if (busy) {
            setCursor(Qt::BusyCursor);
        } else {
            unsetCursor();
        }
    });
    connect(project_, &ProjectController::recentChanged, this, &MainWindow::updateRecentMenu);
    connect(startScreen_, &StartScreen::newProjectRequested, this, &MainWindow::newProject);
    connect(startScreen_, &StartScreen::openProjectRequested, this,
            &MainWindow::openProjectDialog);
    connect(startScreen_, &StartScreen::sampleRequested, this, &MainWindow::openSample);
    connect(startScreen_, &StartScreen::projectRequested, this, &MainWindow::openProject);
}

MainWindow::~MainWindow() {
    // Destroy panels and the viewport (and its swapchain) while the Selection and SceneSync they use
    // still exist; child widgets would otherwise outlive this class's members.
    delete modeDock_;
    delete sceneDock_;
    delete propertiesDock_;
    delete takeCentralWidget();
}

void MainWindow::createActions() {
    // Application-wide shortcuts so they also work while the native viewport window has focus.
    const auto projectAction = [this](const char* icon, const QString& text, const QKeySequence& key,
                                      const QString& tip) {
        auto* action = new QAction(text, this);
        if (icon != nullptr) {
            action->setIcon(theme::icon(QString::fromLatin1(icon)));
        }
        action->setShortcut(key);
        action->setShortcutContext(Qt::ApplicationShortcut);
        action->setToolTip(tip);
        return action;
    };
    newProjectAction_ = projectAction("plus", tr("&New Project"), QKeySequence(Qt::CTRL | Qt::Key_N),
                                      tr("Start an empty project (Ctrl+N)"));
    connect(newProjectAction_, &QAction::triggered, this, &MainWindow::newProject);
    openProjectAction_ =
        projectAction("folder-open", tr("&Open Project…"), QKeySequence(Qt::CTRL | Qt::Key_O),
                      tr("Open a LightBox project (Ctrl+O)"));
    connect(openProjectAction_, &QAction::triggered, this, &MainWindow::openProjectDialog);
    saveAction_ = projectAction("save", tr("&Save"), QKeySequence(Qt::CTRL | Qt::Key_S),
                                tr("Save the project (Ctrl+S)"));
    connect(saveAction_, &QAction::triggered, project_, &ProjectController::save);
    saveAsAction_ =
        projectAction(nullptr, tr("Save &As…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S),
                      tr("Save the project under a new name (Ctrl+Shift+S)"));
    connect(saveAsAction_, &QAction::triggered, project_, &ProjectController::saveAs);
    closeProjectAction_ =
        projectAction(nullptr, tr("&Close Project"), QKeySequence(Qt::CTRL | Qt::Key_W),
                      tr("Close the project and show all projects (Ctrl+W)"));
    connect(closeProjectAction_, &QAction::triggered, this, &MainWindow::closeProject);
    projectsAction_ = new QAction(theme::icon(QStringLiteral("layout-grid")), tr("Projects"), this);
    projectsAction_->setToolTip(tr("Projects: close this project and pick another (Ctrl+W)"));
    connect(projectsAction_, &QAction::triggered, this, &MainWindow::closeProject);

    importAction_ = new QAction(theme::icon(QStringLiteral("download")), tr("&Import Model…"), this);
    importAction_->setShortcut(Qt::CTRL | Qt::Key_I);
    importAction_->setShortcutContext(Qt::ApplicationShortcut);
    importAction_->setToolTip(tr("Import a model: Collada (.dae, from SketchUp), glTF, FBX, OBJ, KMZ, 3DS, STL or PLY (Ctrl+I)"));
    connect(importAction_, &QAction::triggered, this, [this] {
        if (onStartScreen_) {
            newProject(); // importing from the start screen starts a project for the model
            if (onStartScreen_) {
                return;
            }
        }
        setMode(WorkspaceMode::Import);
        importPanel_->chooseModels();
    });
    textureSetAction_ = new QAction(theme::icon(QStringLiteral("image-plus")),
                                    tr("&Material from Textures…"), this);
    textureSetAction_->setToolTip(tr("Make one material from a set of PBR texture images"));
    connect(textureSetAction_, &QAction::triggered, this,
            [this] { importPanel_->chooseTextureSet(); });
    renderImageAction_ =
        projectAction("sparkles", tr("&Render Image…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R),
                      tr("Make the final ray-traced image of the current view on the render "
                         "screen (Ctrl+Shift+R)"));
    connect(renderImageAction_, &QAction::triggered, this, &MainWindow::showRenderScreen);

    // The view actions only make sense in the workspace (disabled on the start screen).
    frameAction_ = new QAction(theme::icon(QStringLiteral("scan")), tr("&Frame Scene"), this);
    frameAction_->setShortcut(Qt::Key_F);
    frameAction_->setShortcutContext(Qt::ApplicationShortcut);
    frameAction_->setToolTip(tr("Frame scene (F)"));
    connect(frameAction_, &QAction::triggered, viewport_, &ViewportWindow::frameScene);

    resetViewAction_ = new QAction(theme::icon(QStringLiteral("rotate-ccw")), tr("&Reset View"), this);
    resetViewAction_->setShortcut(Qt::Key_Home);
    resetViewAction_->setShortcutContext(Qt::ApplicationShortcut);
    resetViewAction_->setToolTip(tr("Reset view (Home)"));
    connect(resetViewAction_, &QAction::triggered, viewport_, &ViewportWindow::resetView);

    gridAction_ = new QAction(theme::icon(QStringLiteral("grid-3x3")), tr("Show &Grid"), this);
    gridAction_->setCheckable(true);
    gridAction_->setChecked(true);
    gridAction_->setShortcut(Qt::Key_G);
    gridAction_->setShortcutContext(Qt::ApplicationShortcut);
    gridAction_->setToolTip(tr("Show ground grid (G)"));
    connect(gridAction_, &QAction::toggled, viewport_, &ViewportWindow::setGridVisible);

    walkAction_ = new QAction(theme::icon(QStringLiteral("footprints")), tr("&Walk"), this);
    walkAction_->setCheckable(true);
    walkAction_->setToolTip(tr("Walk through the design: click where you want to stand, then W A S "
                               "D or arrow keys to move and drag to look around (Esc stops)"));
    connect(walkAction_, &QAction::toggled, this, [this](bool on) {
        viewport_->setWalkMode(on);
        if (on) {
            viewportContainer_->setFocus();
        }
    });

    rayAction_ = new QAction(theme::icon(QStringLiteral("sparkles")), tr("&Ray Traced View"), this);
    rayAction_->setCheckable(true);
    rayAction_->setShortcut(Qt::Key_R);
    rayAction_->setShortcutContext(Qt::ApplicationShortcut);
    rayAction_->setToolTip(tr("Ray traced view (R): real reflections, glass, soft shadows and "
                              "bounced light. Refines while you watch; quality and speed in Render "
                              "mode"));
    connect(rayAction_, &QAction::toggled, this, [this](bool on) {
        render::RayTracingSettings settings = viewport_->rayTracing();
        if (settings.enabled != on) {
            settings.enabled = on;
            viewport_->setRayTracing(settings);
        }
    });
    connect(viewport_, &ViewportWindow::rayTracingChanged, rayAction_, [this] {
        const QSignalBlocker block(rayAction_);
        rayAction_->setChecked(viewport_->rayTracing().enabled);
    });

    // Orbit modes: what a left/middle drag turns around. Remembered between sessions.
    orbitGroup_ = new QActionGroup(this);
    struct OrbitChoice {
        OrbitMode mode;
        const char* icon;
        QString text;
        QString tip;
    };
    const OrbitChoice orbitChoices[] = {
        {OrbitMode::Model, "rotate-3d", tr("Orbit around the &Model"),
         tr("Orbit around the model: dragging turns the view around the middle of the model")},
        {OrbitMode::Cursor, "locate-fixed", tr("Orbit around the &Cursor"),
         tr("Orbit around the cursor: dragging turns the view around the point you grab, like "
            "SketchUp")},
        {OrbitMode::Look, "scan-eye", tr("&Look Around"),
         tr("Look around: the camera stays where it is and turns, like turning your head")},
    };
    const int savedOrbit = QSettings().value(QStringLiteral("viewport/orbitMode"), 0).toInt();
    for (const OrbitChoice& choice : orbitChoices) {
        auto* action = orbitGroup_->addAction(theme::icon(QString::fromLatin1(choice.icon)),
                                              choice.text);
        action->setCheckable(true);
        action->setToolTip(choice.tip);
        action->setData(static_cast<int>(choice.mode));
        action->setChecked(static_cast<int>(choice.mode) == savedOrbit);
        connect(action, &QAction::triggered, this, [this, mode = choice.mode] {
            viewport_->setOrbitMode(mode);
            QSettings().setValue(QStringLiteral("viewport/orbitMode"), static_cast<int>(mode));
        });
    }
    if (orbitGroup_->checkedAction() == nullptr) {
        orbitGroup_->actions().constFirst()->setChecked(true);
    }
    viewport_->setOrbitMode(static_cast<OrbitMode>(orbitGroup_->checkedAction()->data().toInt()));

    modeGroup_ = new QActionGroup(this);
    for (const WorkspaceModeInfo& info : workspaceModes()) {
        auto* action = modeGroup_->addAction(theme::icon(QString::fromLatin1(info.icon)), info.title);
        action->setCheckable(true);
        action->setShortcut(info.shortcut);
        action->setShortcutContext(Qt::ApplicationShortcut);
        action->setToolTip(QStringLiteral("%1 — %2 (%3)")
                               .arg(info.title, info.summary,
                                    info.shortcut.toString(QKeySequence::NativeText)));
        const WorkspaceMode mode = info.mode;
        connect(action, &QAction::triggered, this, [this, mode] { setMode(mode); });
        modeActions_[mode] = action;
    }

    auto* themes = new QActionGroup(this);
    darkThemeAction_ = themes->addAction(tr("&Dark"));
    lightThemeAction_ = themes->addAction(tr("&Light"));
    darkThemeAction_->setCheckable(true);
    lightThemeAction_->setCheckable(true);
    connect(darkThemeAction_, &QAction::triggered, this, [this] { setThemeMode(true); });
    connect(lightThemeAction_, &QAction::triggered, this, [this] { setThemeMode(false); });
    toggleThemeAction_ = new QAction(this);
    connect(toggleThemeAction_, &QAction::triggered, this,
            [this] { setThemeMode(theme::mode() != theme::Mode::Dark); });
    updateThemeActions();
}

void MainWindow::createHeader() {
    auto* header = new QToolBar(tr("Header"), this);
    header->setObjectName(QStringLiteral("header"));
    header->setMovable(false);
    header->setFloatable(false);
    header->setIconSize(QSize(16, 16));
    header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header->toggleViewAction()->setEnabled(false); // the mode bar is not optional
    addToolBar(Qt::TopToolBarArea, header);

    auto* brand = new QLabel(header);
    brand->setPixmap(theme::appIcon().pixmap(QSize(22, 22)));
    brand->setContentsMargins(4, 0, 6, 0);
    header->addWidget(brand);
    header->addAction(projectsAction_);
    if (auto* button = qobject_cast<QToolButton*>(header->widgetForAction(projectsAction_))) {
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    }
    projectLabel_ = new QLabel(header);
    projectLabel_->setProperty("role", "heading");
    projectLabel_->setContentsMargins(4, 0, 0, 0);
    projectLabelAction_ = header->addWidget(projectLabel_);

    auto* leftSpacer = new QWidget(header);
    leftSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    header->addWidget(leftSpacer);
    for (const WorkspaceModeInfo& info : workspaceModes()) {
        QAction* action = modeActions_.at(info.mode);
        header->addAction(action);
        if (QWidget* button = header->widgetForAction(action)) {
            button->setProperty("mode", true);
        }
    }
    auto* rightSpacer = new QWidget(header);
    rightSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    header->addWidget(rightSpacer);
    header->addAction(toggleThemeAction_);
    if (auto* button = qobject_cast<QToolButton*>(header->widgetForAction(toggleThemeAction_))) {
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    }
}

QDockWidget* MainWindow::addPanel(const QString& title, const char* objectName, QWidget* content,
                                  QLabel** titleLabel) {
    auto* dock = new QDockWidget(title, this);
    dock->setObjectName(QString::fromLatin1(objectName));
    // One clear title per panel, no close/float buttons. Panels can still be dragged by the title
    // (double-click floats them) and toggled from View ▸ Panels.
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    auto* label = new QLabel(title, dock);
    label->setProperty("role", "panelTitle");
    dock->setTitleBarWidget(label);
    if (titleLabel != nullptr) {
        *titleLabel = label;
    }
    content->setParent(dock);
    dock->setWidget(content);
    return dock;
}

void MainWindow::createDocks(Scene& scene) {
    modePages_ = new QStackedWidget;
    // Page order must match WorkspaceMode.
    importPanel_ = new ImportPanel(scene, *selection_, *viewport_);
    modePages_->addWidget(importPanel_);
    modePages_->addWidget(makeBuildPage(nullptr));
    modePages_->addWidget(new MaterialsPanel(scene, *selection_));
    modePages_->addWidget(makeComingSoonPage(
        QStringLiteral("trees"), tr("Site tools are coming soon"),
        tr("Shape the ground around your building, plant the garden and add water."),
        {tr("Terrain, paths and building pads"), tr("Trees, hedges and groundcover"),
         tr("Pools, fountains, ponds and streams")}));
    modePages_->addWidget(new LightPanel(scene, *selection_, *viewport_));
    auto* renderPanel = new RenderPanel(scene, *viewport_);
    connect(renderPanel, &RenderPanel::renderScreenRequested, this, &MainWindow::showRenderScreen);
    modePages_->addWidget(renderPanel);

    modeDock_ = addPanel(tr("Build"), "dock.mode", modePages_, &modeTitle_);
    sceneDock_ = addPanel(tr("Scene"), "dock.scene", new OutlinerPanel(scene, *selection_));
    propertiesDock_ =
        addPanel(tr("Properties"), "dock.properties", new InspectorPanel(scene, *selection_));
}

void MainWindow::showModePage(WorkspaceMode mode) {
    modePages_->setCurrentIndex(static_cast<int>(mode));
    modeTitle_->setText(workspaceModeInfo(mode).title);
    modeDock_->setWindowTitle(workspaceModeInfo(mode).title);
    modeActions_.at(mode)->setChecked(true);
    // Moving things is for placing models, arranging the design and aiming lights, not for
    // picking materials.
    viewport_->setGizmoEnabled(mode == WorkspaceMode::Import || mode == WorkspaceMode::Build ||
                               mode == WorkspaceMode::Light);
    // The selection outline helps everywhere except when framing the final image.
    viewport_->setSelectionOutline(mode != WorkspaceMode::Render);
}

void MainWindow::applyDefaultLayout() {
    for (QDockWidget* dock : {modeDock_, sceneDock_, propertiesDock_}) {
        dock->setFloating(false);
        removeDockWidget(dock);
    }
    addDockWidget(Qt::LeftDockWidgetArea, modeDock_);
    addDockWidget(Qt::RightDockWidgetArea, sceneDock_);
    splitDockWidget(sceneDock_, propertiesDock_, Qt::Vertical);
    for (QDockWidget* dock : {modeDock_, sceneDock_, propertiesDock_}) {
        dock->show();
    }
    // The mode panel holds dense property rows (D-019); 360 px fits a label column, a slider and
    // a number field, or a texture slot with its channel and invert controls.
    resizeDocks({modeDock_, sceneDock_}, {360, 300}, Qt::Horizontal);
    resizeDocks({sceneDock_, propertiesDock_}, {400, 400}, Qt::Vertical);
}

void MainWindow::restoreLayout(WorkspaceMode mode) {
    const auto saved = layouts_.find(mode);
    if (saved == layouts_.end() || !restoreState(saved->second, kLayoutVersion)) {
        layouts_.erase(mode);
        restoreState(defaultLayout_, kLayoutVersion);
    }
}

void MainWindow::setMode(WorkspaceMode mode) {
    closeRenderScreen(); // switching modes means editing again
    showModePage(mode);
    if (mode == mode_) {
        return;
    }
    // On the start screen the docks are hidden; the layout is saved on leaving the workspace and
    // restored by showWorkspace().
    if (!onStartScreen_) {
        layouts_[mode_] = saveState(kLayoutVersion);
    }
    mode_ = mode;
    if (!onStartScreen_) {
        restoreLayout(mode);
    }
    QSettings().setValue(QStringLiteral("workspace/current"),
                         QString::fromLatin1(workspaceModeInfo(mode).key));
}

void MainWindow::loadWorkspaces() {
    // Capture the default arrangement while the window is still hidden; later rearrangements always
    // go through restoreState(), which Qt applies reliably to a visible window.
    applyDefaultLayout();
    defaultLayout_ = saveState(kLayoutVersion);

    const QSettings settings;
    restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray());
    for (const WorkspaceModeInfo& info : workspaceModes()) {
        const QByteArray state = settings.value(layoutKey(info.mode)).toByteArray();
        if (!state.isEmpty()) {
            layouts_[info.mode] = state;
        }
    }
    mode_ = workspaceModeFromKey(settings.value(QStringLiteral("workspace/current")).toString())
                .value_or(WorkspaceMode::Build);
    showModePage(mode_);
    restoreLayout(mode_);
}

void MainWindow::saveWorkspaces() {
    if (!onStartScreen_ && !onRenderScreen_) { // those pages hide the docks
        layouts_[mode_] = saveState(kLayoutVersion);
    }
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    for (const auto& [mode, state] : layouts_) {
        settings.setValue(layoutKey(mode), state);
    }
}

void MainWindow::createMenus() {
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(newProjectAction_);
    file->addAction(openProjectAction_);
    recentMenu_ = file->addMenu(tr("Open &Recent"));
    recentMenu_->setToolTipsVisible(true);
    updateRecentMenu();
    file->addSeparator();
    file->addAction(saveAction_);
    file->addAction(saveAsAction_);
    file->addAction(closeProjectAction_);
    file->addSeparator();
    file->addAction(importAction_);
    file->addAction(textureSetAction_);
    file->addSeparator();
    file->addAction(renderImageAction_);
    file->addSeparator();
    QAction* quit = file->addAction(tr("E&xit"), this, &QWidget::close);
    quit->setShortcut(QKeySequence::Quit);

    QMenu* view = menuBar()->addMenu(tr("&View"));
    modeMenu_ = view->addMenu(tr("&Mode"));
    for (const WorkspaceModeInfo& info : workspaceModes()) {
        modeMenu_->addAction(modeActions_.at(info.mode));
    }
    panelsMenu_ = view->addMenu(tr("&Panels"));
    for (QDockWidget* dock : {modeDock_, sceneDock_, propertiesDock_}) {
        QAction* toggle = dock->toggleViewAction();
        if (dock == modeDock_) {
            toggle->setText(tr("Mode panel"));
        }
        panelsMenu_->addAction(toggle);
    }
    panelsMenu_->addSeparator();
    panelsMenu_->addAction(tr("&Reset Layout"), this, [this] {
        layouts_.erase(mode_);
        restoreLayout(mode_);
    });
    view->addSeparator();
    view->addAction(frameAction_);
    view->addAction(resetViewAction_);
    view->addAction(gridAction_);
    view->addAction(walkAction_);
    view->addSeparator();
    view->addActions(orbitGroup_->actions());
    view->addAction(rayAction_);
    view->addSeparator();
    QMenu* themeMenu = view->addMenu(tr("&Theme"));
    themeMenu->addAction(darkThemeAction_);
    themeMenu->addAction(lightThemeAction_);

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About LightBox"), this, &MainWindow::showAbout);
}

void MainWindow::createStatusBar() {
    const render::DeviceInfo& gpu = renderer_.deviceInfo();
    auto* gpuLabel = new QLabel(QStringLiteral("%1  ·  Vulkan %2  ·  %3× MSAA")
                                    .arg(QString::fromStdString(gpu.name),
                                         QString::fromStdString(gpu.vulkanVersion))
                                    .arg(gpu.msaaSamples));
    gpuLabel->setToolTip(tr("Driver %1 · %2 MiB device memory · hardware ray tracing: %3")
                             .arg(QString::fromStdString(gpu.driverVersion))
                             .arg(gpu.deviceLocalBytes >> 20)
                             .arg(gpu.hardwareRayTracing ? tr("yes") : tr("no")));
    frameLabel_ = new QLabel;
    importProgress_ = new QProgressBar;
    importProgress_->setRange(0, 100);
    importProgress_->setTextVisible(false);
    importProgress_->setFixedWidth(120);
    importProgress_->setToolTip(tr("Importing… (details and Cancel in Import mode)"));
    importProgress_->hide();
    statusBar()->addWidget(gpuLabel);
    statusBar()->addPermanentWidget(importProgress_);
    statusBar()->addPermanentWidget(frameLabel_);
}

void MainWindow::setThemeMode(bool dark) {
    const theme::Mode next = dark ? theme::Mode::Dark : theme::Mode::Light;
    theme::apply(*qApp, next);
    QSettings().setValue(QStringLiteral("ui/theme"), theme::modeName(next));
    updateThemeActions();
}

void MainWindow::updateThemeActions() {
    const bool dark = theme::mode() == theme::Mode::Dark;
    darkThemeAction_->setChecked(dark);
    lightThemeAction_->setChecked(!dark);
    toggleThemeAction_->setIcon(theme::icon(dark ? QStringLiteral("sun") : QStringLiteral("moon")));
    toggleThemeAction_->setText(dark ? tr("Switch to light theme") : tr("Switch to dark theme"));
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (!project_->maybeSave()) {
        event->ignore();
        return;
    }
    saveWorkspaces();
    QMainWindow::closeEvent(event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls() && !droppablePaths(event->mimeData()->urls()).isEmpty()) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent* event) {
    const QStringList paths = droppablePaths(event->mimeData()->urls());
    if (paths.isEmpty()) {
        return;
    }
    event->acceptProposedAction();
    openDroppedFiles(paths);
}

void MainWindow::openDroppedFiles(const QStringList& paths) {
    for (const QString& path : paths) {
        if (ProjectController::isProjectFile(path)) {
            openProject(path); // one project at a time; the first dropped wins
            return;
        }
    }
    if (onStartScreen_) {
        newProject();
        if (onStartScreen_) {
            return; // the user kept the current project open
        }
    }
    // A sky panorama (.hdr) becomes the sky; everything else is imported.
    QStringList imports;
    for (const QString& path : paths) {
        if (isEnvironmentMapFile(std::filesystem::path(path.toStdWString()))) {
            Environment environment = scene_.environment();
            environment.sky.image = QFileInfo(path).absoluteFilePath().toStdString();
            scene_.setEnvironment(environment);
            setMode(WorkspaceMode::Light);
            statusBar()->showMessage(tr("Sky image: %1").arg(QFileInfo(path).fileName()), 10000);
        } else {
            imports << path;
        }
    }
    if (!imports.isEmpty()) {
        importFiles(imports);
    }
}

void MainWindow::importFiles(const QStringList& paths, bool switchMode) {
    if (switchMode && std::any_of(paths.begin(), paths.end(), &ImportPanel::isModelPath)) {
        setMode(WorkspaceMode::Import);
    }
    importPanel_->importFiles(paths, switchMode);
}

void MainWindow::newProject() {
    if (!project_->maybeSave()) {
        return;
    }
    project_->newProject(); // projectOpened() shows the workspace
    setMode(WorkspaceMode::Import);
}

void MainWindow::openSample() {
    if (project_->maybeSave()) {
        project_->openSample();
    }
}

void MainWindow::openProject(const QString& path) {
    if (project_->maybeSave()) {
        project_->open(path);
    }
}

void MainWindow::openProjectDialog() {
    const QString path = project_->chooseProjectFile();
    if (!path.isEmpty()) {
        openProject(path);
    }
}

void MainWindow::closeProject() {
    if (!project_->maybeSave()) {
        return;
    }
    showStartScreen();
    project_->closeProject();
}

void MainWindow::showStartScreen() {
    if (onStartScreen_) {
        return;
    }
    if (onRenderScreen_) {
        // Its docks are already hidden and the layout was saved when it opened.
        renderScreen_->stop();
        onRenderScreen_ = false;
    } else if (isVisible()) {
        // Remember the workspace arrangement before the docks go away. Before the first show()
        // the saved layout from loadWorkspaces() is still current, and a hidden window's state
        // would not be.
        layouts_[mode_] = saveState(kLayoutVersion);
    }
    onStartScreen_ = true;
    for (QDockWidget* dock : {modeDock_, sceneDock_, propertiesDock_}) {
        dock->hide();
    }
    pages_->setCurrentWidget(startScreen_);
    setStartScreenChrome(true);
}

void MainWindow::showWorkspace() {
    if (!onStartScreen_) {
        return;
    }
    onStartScreen_ = false;
    pages_->setCurrentWidget(workspacePage_);
    showModePage(mode_);
    restoreLayout(mode_); // also brings the docks back
    setStartScreenChrome(false);
    viewportContainer_->setFocus();
}

void MainWindow::showRenderScreen() {
    if (onStartScreen_ || onRenderScreen_) {
        return;
    }
    if (isVisible()) {
        layouts_[mode_] = saveState(kLayoutVersion);
    }
    onRenderScreen_ = true;
    for (QDockWidget* dock : {modeDock_, sceneDock_, propertiesDock_}) {
        dock->hide();
    }
    statusBar()->hide(); // the render screen shows its own progress
    pages_->setCurrentWidget(renderScreen_);
    renderScreen_->prepare(project_->name());
}

void MainWindow::closeRenderScreen() {
    if (!onRenderScreen_) {
        return;
    }
    renderScreen_->stop();
    onRenderScreen_ = false;
    pages_->setCurrentWidget(workspacePage_);
    showModePage(mode_);
    restoreLayout(mode_); // also brings the docks back
    statusBar()->show();
    viewportContainer_->setFocus();
}

void MainWindow::setStartScreenChrome(bool startScreen) {
    // The header keeps the brand and the theme switch; mode bar, project name and Projects button
    // only mean something inside a project.
    modeGroup_->setVisible(!startScreen);
    modeGroup_->setEnabled(!startScreen);
    projectsAction_->setVisible(!startScreen);
    projectLabelAction_->setVisible(!startScreen);
    for (QAction* action : {saveAction_, saveAsAction_, closeProjectAction_, textureSetAction_,
                            frameAction_, resetViewAction_, gridAction_, walkAction_,
                            rayAction_, renderImageAction_}) {
        action->setEnabled(!startScreen);
    }
    modeMenu_->menuAction()->setEnabled(!startScreen);
    panelsMenu_->menuAction()->setEnabled(!startScreen);
    statusBar()->setVisible(!startScreen);
    updateProjectChrome();
}

void MainWindow::updateProjectChrome() {
    const QString name = project_->name();
    const bool dirty = project_->isDirty();
    projectLabel_->setText(dirty ? tr("%1 •").arg(name) : name);
    projectLabel_->setToolTip(project_->isUntitled() ? tr("Not saved yet")
                                                     : QDir::toNativeSeparators(project_->path()));
    if (onStartScreen_) {
        setWindowModified(false);
        setWindowTitle(QStringLiteral("LightBox"));
    } else {
        setWindowTitle(tr("%1[*] — LightBox").arg(name));
        setWindowModified(dirty);
    }
}

void MainWindow::updateRecentMenu() {
    recentMenu_->clear();
    const QStringList recent = project_->recentProjects();
    if (recent.isEmpty()) {
        recentMenu_->addAction(tr("No recent projects"))->setEnabled(false);
    }
    int number = 1;
    for (const QString& path : recent) {
        QString name = QFileInfo(path).completeBaseName();
        name.replace(QLatin1Char('&'), QStringLiteral("&&"));
        const QString text = number <= 9 ? QStringLiteral("&%1  %2").arg(number).arg(name) : name;
        QAction* action = recentMenu_->addAction(text, this, [this, path] { openProject(path); });
        action->setToolTip(QDir::toNativeSeparators(path));
        ++number;
    }
    recentMenu_->addSeparator();
    QAction* clear = recentMenu_->addAction(tr("&Clear Recent Projects"), project_,
                                            &ProjectController::clearRecent);
    clear->setEnabled(!recent.isEmpty());
}

void MainWindow::showAbout() {
    const render::DeviceInfo& gpu = renderer_.deviceInfo();
    QMessageBox box(this);
    box.setWindowTitle(tr("About LightBox"));
    box.setIconPixmap(QPixmap(QStringLiteral(":/branding/lightbox-logo-dark.png"))
                          .scaledToWidth(220, Qt::SmoothTransformation));
    box.setText(tr("<h3>LightBox %1</h3>"
                   "<p>Free, open-source architectural rendering.</p>"
                   "<p>MIT License · "
                   "<a href=\"https://github.com/shrimpsooup2/lightbox\">github.com/shrimpsooup2/"
                   "lightbox</a></p>"
                   "<p>Live engine: %2 (Vulkan %3)</p>")
                    .arg(QStringLiteral(LIGHTBOX_VERSION), QString::fromStdString(gpu.name),
                         QString::fromStdString(gpu.vulkanVersion)));
    box.exec();
}

bool MainWindow::saveScreenshot(const QString& path) {
    // The native Vulkan viewport is invisible to QWidget::grab(), so the window chrome is grabbed
    // from the widget tree and a read-back viewport frame is painted into the viewport's area.
    // Works even when the window is covered by other windows. The start screen has no viewport.
    QImage viewportImage;
    if (!onStartScreen_) {
        viewportImage = viewport_->grabFrame();
        if (viewportImage.isNull()) {
            log::error("Screenshot failed: the viewport could not produce a frame");
            return false;
        }
    }
    QPixmap pixmap = grab();
    if (!viewportImage.isNull()) {
        QPainter painter(&pixmap);
        painter.drawImage(
            QRect(viewportContainer_->mapTo(this, QPoint(0, 0)), viewportContainer_->size()),
            viewportImage);
    }
    const bool saved = pixmap.save(path, "PNG");
    if (saved) {
        log::info("Screenshot saved to {}", path.toStdString());
    } else {
        log::error("Could not save screenshot to {}", path.toStdString());
    }
    return saved;
}

} // namespace lb::app
