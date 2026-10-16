#pragma once

#include <cstdint>

#include <QString>
#include <QWidget>

class QEvent;
class QLabel;
class QLineEdit;
class QListView;
class QModelIndex;
class QPoint;
class QShowEvent;
class QSortFilterProxyModel;
class QStackedWidget;
class QStandardItemModel;

namespace lb::app {

class ProjectController;
class ProjectCardDelegate;
struct ProjectCardInfo;

// What LightBox shows when no project is open (D-022, docs/WORKFLOW.md §5): New / Open / Sample on
// the left, the recent projects as thumbnail cards on the right. Card details and thumbnails are
// read on worker threads, so the screen appears at once. Emits requests; the main window decides
// (it asks to save the current project first). `projects` must outlive the screen.
class StartScreen : public QWidget {
    Q_OBJECT

public:
    explicit StartScreen(ProjectController& projects, QWidget* parent = nullptr);

    // Rebuilds the cards from the recent list (also done on ProjectController::recentChanged).
    void refresh();

signals:
    void newProjectRequested();
    void openProjectRequested(); // show the Open dialog
    void sampleRequested();
    void projectRequested(const QString& path);

protected:
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void updateLogo();
    void updateGrid();
    void updateVisibility();
    void applyCard(const QString& path, const ProjectCardInfo& info);
    void openIndex(const QModelIndex& index);
    void showContextMenu(const QPoint& position);

    ProjectController& projects_;
    QString logoKey_;              // logo file and pixel ratio currently shown
    std::uint64_t generation_ = 0; // card loads from an older refresh are dropped

    QLabel* logo_ = nullptr;
    QLineEdit* search_ = nullptr;
    QStackedWidget* pages_ = nullptr; // grid, no matches, empty state
    QWidget* emptyPage_ = nullptr;
    QLabel* noMatches_ = nullptr;
    QListView* grid_ = nullptr;
    QStandardItemModel* model_ = nullptr;
    QSortFilterProxyModel* filter_ = nullptr;
    ProjectCardDelegate* delegate_ = nullptr;
};

} // namespace lb::app
