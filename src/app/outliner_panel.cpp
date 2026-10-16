#include "app/outliner_panel.hpp"

#include <vector>

#include <QItemSelection>
#include <QLineEdit>
#include <QSortFilterProxyModel>
#include <QTreeView>
#include <QVBoxLayout>

#include "app/scene_tree_model.hpp"
#include "app/theme.hpp"

namespace lb::app {

OutlinerPanel::OutlinerPanel(Scene& scene, Selection& selection, QWidget* parent)
    : QWidget(parent), selection_(selection) {
    setProperty("panel", true);

    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Search scene"));
    search_->setClearButtonEnabled(true);
    search_->addAction(theme::icon(QStringLiteral("search")), QLineEdit::LeadingPosition);

    model_ = new SceneTreeModel(scene, this);
    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setRecursiveFilteringEnabled(true);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);

    tree_ = new QTreeView(this);
    tree_->setModel(proxy_);
    tree_->setHeaderHidden(true);
    tree_->setUniformRowHeights(true);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::DoubleClicked);
    tree_->setIconSize(QSize(16, 16));
    tree_->setIndentation(16);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);
    layout->addWidget(search_);
    layout->addWidget(tree_, 1);

    connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) {
        proxy_->setFilterFixedString(text);
        if (text.isEmpty()) {
            restoreExpansion();
        } else {
            tree_->expandAll(); // show every match in context
        }
    });
    connect(tree_, &QTreeView::expanded, this, [this](const QModelIndex& index) {
        if (search_->text().isEmpty()) {
            expanded_.insert(model_->entityAt(proxy_->mapToSource(index)));
        }
    });
    connect(tree_, &QTreeView::collapsed, this, [this](const QModelIndex& index) {
        if (search_->text().isEmpty()) {
            expanded_.erase(model_->entityAt(proxy_->mapToSource(index)));
        }
    });
    connect(tree_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { pullSelectionFromTree(); });
    connect(model_, &QAbstractItemModel::modelReset, this, [this] {
        restoreExpansion();
        pushSelectionToTree();
    });
    selectionListener_ = selection_.addListener([this] { pushSelectionToTree(); });

    // Start with every group open: imported models are browsed top-down.
    std::vector<QModelIndex> pending{QModelIndex{}};
    while (!pending.empty()) {
        const QModelIndex parentIndex = pending.back();
        pending.pop_back();
        for (int row = 0; row < model_->rowCount(parentIndex); ++row) {
            const QModelIndex child = model_->index(row, 0, parentIndex);
            if (model_->rowCount(child) > 0) {
                expanded_.insert(model_->entityAt(child));
                pending.push_back(child);
            }
        }
    }
    restoreExpansion();
}

OutlinerPanel::~OutlinerPanel() {
    selection_.removeListener(selectionListener_);
}

void OutlinerPanel::restoreExpansion() {
    const bool wasSyncing = syncing_;
    syncing_ = true;
    std::erase_if(expanded_, [this](EntityId id) { return !model_->indexOf(id).isValid(); });
    for (EntityId id : expanded_) {
        tree_->expand(proxy_->mapFromSource(model_->indexOf(id)));
    }
    syncing_ = wasSyncing;
}

void OutlinerPanel::pushSelectionToTree() {
    if (syncing_) {
        return;
    }
    syncing_ = true;
    QItemSelection rows;
    for (EntityId id : selection_.ids()) {
        const QModelIndex index = proxy_->mapFromSource(model_->indexOf(id));
        if (index.isValid()) {
            rows.select(index, index);
        }
    }
    tree_->selectionModel()->select(rows, QItemSelectionModel::ClearAndSelect |
                                              QItemSelectionModel::Rows);
    const QModelIndex primary = proxy_->mapFromSource(model_->indexOf(selection_.primary()));
    if (primary.isValid()) {
        tree_->selectionModel()->setCurrentIndex(primary, QItemSelectionModel::NoUpdate);
        tree_->scrollTo(primary);
    }
    syncing_ = false;
}

void OutlinerPanel::pullSelectionFromTree() {
    if (syncing_) {
        return;
    }
    std::vector<EntityId> ids;
    for (const QModelIndex& index : tree_->selectionModel()->selectedRows()) {
        ids.push_back(model_->entityAt(proxy_->mapToSource(index)));
    }
    // Keep the clicked row last so it becomes the primary selection shown in the inspector.
    const EntityId current = model_->entityAt(proxy_->mapToSource(tree_->currentIndex()));
    if (std::erase(ids, current) > 0) {
        ids.push_back(current);
    }
    syncing_ = true;
    selection_.set(ids);
    syncing_ = false;
}

} // namespace lb::app
