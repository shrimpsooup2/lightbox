#pragma once

#include <map>
#include <memory>

#include <QByteArray>
#include <QMainWindow>
#include <QStringList>

#include "app/workspace.hpp"
#include "core/scene/scene.hpp"
#include "core/scene/selection.hpp"
#include "render/renderer.hpp"
#include "render/scene_sync.hpp"

class QAction;
class QActionGroup;
class QDockWidget;
class QLabel;
class QMenu;
class QProgressBar;
class QStackedWidget;
class QVulkanInstance;

namespace lb::app {

class ImportPanel;
class ProjectController;
class RenderScreen;
class StartScreen;
class ViewportWindow;

// Main application window showing one document (docs/WORKFLOW.md). Three pages: the start screen
// (recent projects, New / Open / Sample) while no project is open, the workspace, and the render
// screen (final images, D-029). The workspace has three panels, same in every mode: the current
// mode's panel on the left, Scene above Properties on the right, the viewport in the center. The
// header bar switches modes; each mode remembers how the user sized or moved the panels. `scene`
// must outlive the window.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(render::Renderer& renderer, QVulkanInstance& vulkan, Scene& scene,
               QWidget* parent = nullptr);
    ~MainWindow() override;

    void setMode(WorkspaceMode mode);
    // Saves the window, including the native Vulkan viewport, as a PNG.
    bool saveScreenshot(const QString& path);
    // Imports models (one after another) and turns images into one material, as if the files were
    // dropped on the window. With `switchMode`, shows Import mode for models and Materials mode for
    // a new texture material; without it the current mode stays (scripted startup with --mode).
    void importFiles(const QStringList& paths, bool switchMode = true);

    // Project commands. Each asks to save unsaved changes first and does nothing if the user
    // cancels. New projects open in Import mode; opened projects and the sample in the last mode.
    void newProject();
    void openSample();
    void openProject(const QString& path); // reads in the background; the workspace shows when done
    void showStartScreen();
    void showWorkspace();
    // The render screen replaces the workspace (docks hidden) until Back/Esc.
    void showRenderScreen();
    void closeRenderScreen();
    ProjectController& projects() { return *project_; }

protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void createActions();
    void createHeader();
    void createDocks(Scene& scene);
    void createMenus();
    void createStatusBar();
    QDockWidget* addPanel(const QString& title, const char* objectName, QWidget* content,
                          QLabel** titleLabel = nullptr);
    void showModePage(WorkspaceMode mode);
    void applyDefaultLayout();
    void restoreLayout(WorkspaceMode mode);
    void loadWorkspaces();
    void saveWorkspaces();
    void setThemeMode(bool dark);
    void updateThemeActions();
    void showAbout();

    void openProjectDialog();
    void closeProject();
    // A dropped `.lbox` opens; a `.hdr` panorama becomes the sky; models and images import
    // (starting a new project from the start screen).
    void openDroppedFiles(const QStringList& paths);
    void updateProjectChrome();
    void updateRecentMenu();
    void setStartScreenChrome(bool startScreen);

    render::Renderer& renderer_;
    Scene& scene_;
    std::unique_ptr<render::SceneSync> sceneSync_; // shared by all viewports of this scene
    std::unique_ptr<Selection> selection_;
    ViewportWindow* viewport_ = nullptr;
    QWidget* viewportContainer_ = nullptr;
    QLabel* frameLabel_ = nullptr;
    ImportPanel* importPanel_ = nullptr;
    QProgressBar* importProgress_ = nullptr; // status bar, visible while importing

    ProjectController* project_ = nullptr;
    QStackedWidget* pages_ = nullptr; // central: workspace, start screen or render screen
    QWidget* workspacePage_ = nullptr;
    StartScreen* startScreen_ = nullptr;
    bool onStartScreen_ = false;
    RenderScreen* renderScreen_ = nullptr;
    bool onRenderScreen_ = false;
    QLabel* projectLabel_ = nullptr;          // header: project name, "•" when modified
    QAction* projectLabelAction_ = nullptr;   // its header slot (hidden on the start screen)

    QDockWidget* modeDock_ = nullptr; // left: one page per mode
    QLabel* modeTitle_ = nullptr;
    QStackedWidget* modePages_ = nullptr;
    QDockWidget* sceneDock_ = nullptr;      // right, top
    QDockWidget* propertiesDock_ = nullptr; // right, bottom

    WorkspaceMode mode_ = WorkspaceMode::Build;
    std::map<WorkspaceMode, QByteArray> layouts_; // user-arranged layout per mode
    QByteArray defaultLayout_;                    // captured at startup, before show()
    std::map<WorkspaceMode, QAction*> modeActions_;
    QActionGroup* modeGroup_ = nullptr;
    QMenu* modeMenu_ = nullptr;
    QMenu* panelsMenu_ = nullptr;
    QMenu* recentMenu_ = nullptr;

    QAction* newProjectAction_ = nullptr;
    QAction* openProjectAction_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* saveAsAction_ = nullptr;
    QAction* closeProjectAction_ = nullptr;
    QAction* projectsAction_ = nullptr; // header: back to the start screen
    QAction* importAction_ = nullptr;
    QAction* textureSetAction_ = nullptr;
    QAction* renderImageAction_ = nullptr; // opens the render screen
    QAction* frameAction_ = nullptr;
    QAction* resetViewAction_ = nullptr;
    QAction* gridAction_ = nullptr;
    QAction* walkAction_ = nullptr;
    QAction* rayAction_ = nullptr; // ray traced view on/off
    QLabel* rayLabel_ = nullptr;   // its progress in the status bar
    QProgressBar* rayBar_ = nullptr;
    QActionGroup* orbitGroup_ = nullptr; // orbit modes (viewport toolbar, View menu)
    QAction* darkThemeAction_ = nullptr;
    QAction* lightThemeAction_ = nullptr;
    QAction* toggleThemeAction_ = nullptr;
};

} // namespace lb::app
