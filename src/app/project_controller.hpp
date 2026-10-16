#pragma once

#include <cstdint>

#include <QObject>
#include <QString>
#include <QStringList>

#include "core/scene/scene.hpp"

class QWidget;

namespace lb::app {

class ViewportWindow;

// The project behind the main window (D-022): which `.lbox` file the scene belongs to, whether it has
// unsaved changes, and the recent projects list. New, sample and opened documents are installed here;
// saving writes the scene, the viewport camera and a viewport thumbnail. UI thread only. `scene` and
// `viewport` must outlive the controller; `window` parents its dialogs.
class ProjectController : public QObject {
    Q_OBJECT

public:
    ProjectController(Scene& scene, ViewportWindow& viewport, QWidget* window);
    ~ProjectController() override;

    QString name() const { return name_; }
    QString path() const { return path_; } // absolute; empty while untitled (never saved)
    bool isUntitled() const { return path_.isEmpty(); }
    bool isDirty() const { return dirty_; }
    bool isBusy() const { return loadSerial_ != finishedSerial_; } // a project is being read

    // Each installs a document and emits projectOpened(). They don't ask to save the current one:
    // callers run maybeSave() first.
    void newProject();
    void openSample();
    // Reads the file on a worker thread, then installs it. Failures leave the current document as it
    // is and are explained in a message box; a missing file can be removed from the recent list.
    void open(const QString& path);
    // Back to an empty, untitled, unmodified document without emitting projectOpened() (the window
    // shows the start screen).
    void closeProject();

    // Writes the project (untitled → saveAs()). False when the user cancelled or saving failed.
    bool save();
    bool saveAs();
    // Asks Save / Don't Save / Cancel when there are unsaved changes. False means "stop": the user
    // cancelled or the save failed.
    bool maybeSave();
    // Open dialog for `.lbox` files; empty when cancelled.
    QString chooseProjectFile();

    // Most recently opened or saved first (absolute paths, at most 12).
    QStringList recentProjects() const;
    void removeRecent(const QString& path);
    void clearRecent();

    static bool isProjectFile(const QString& path);

signals:
    void projectChanged(); // name, path or dirty flag
    void projectOpened();  // a new, sample or opened document was installed
    void busyChanged(bool busy);
    void recentChanged();
    void statusMessage(const QString& text); // one line for the status bar

private:
    // Puts a document in place without marking the project as changed. `install` mutates the scene.
    template <class Install>
    void installDocument(Install install);
    void setDocument(const QString& path, const QString& name);
    bool writeTo(const QString& path, const QString& name);
    void addRecent(const QString& path);
    void storeRecent(const QStringList& paths);
    void cancelLoad();
    void finishLoad(std::uint64_t serial);

    Scene& scene_;
    ViewportWindow& viewport_;
    QWidget* window_;
    Scene::ListenerId listener_ = 0;
    QString path_;
    QString name_;
    bool dirty_ = false;
    bool installing_ = false;
    // Loads in flight are numbered; a result whose number is no longer current is dropped (another
    // document was installed meanwhile).
    std::uint64_t loadSerial_ = 0;
    std::uint64_t finishedSerial_ = 0;
};

} // namespace lb::app
