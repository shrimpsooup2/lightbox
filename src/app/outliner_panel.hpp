#pragma once

#include <unordered_set>

#include <QWidget>

#include "core/scene/scene.hpp"
#include "core/scene/selection.hpp"

class QLineEdit;
class QSortFilterProxyModel;
class QTreeView;

namespace lb::app {

class SceneTreeModel;

// "Scene" panel: searchable tree of the document's entities, two-way synced with the Selection.
class OutlinerPanel : public QWidget {
    Q_OBJECT

public:
    OutlinerPanel(Scene& scene, Selection& selection, QWidget* parent = nullptr);
    ~OutlinerPanel() override;

private:
    void pushSelectionToTree();
    void pullSelectionFromTree();
    void restoreExpansion();

    Selection& selection_;
    Selection::ListenerId selectionListener_ = 0;
    SceneTreeModel* model_ = nullptr;
    QSortFilterProxyModel* proxy_ = nullptr;
    QTreeView* tree_ = nullptr;
    QLineEdit* search_ = nullptr;
    std::unordered_set<EntityId> expanded_;
    bool syncing_ = false;
};

} // namespace lb::app
