#pragma once

#include <span>

#include <QAbstractItemModel>
#include <QIcon>

#include "core/scene/scene.hpp"

namespace lb::app {

// Qt item model over the Scene hierarchy (one column: name). Rows mirror Entity::children order and
// update live from scene notifications. Names are editable. Index internal IDs are entity IDs.
class SceneTreeModel : public QAbstractItemModel {
    Q_OBJECT

public:
    explicit SceneTreeModel(Scene& scene, QObject* parent = nullptr);
    ~SceneTreeModel() override;

    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    EntityId entityAt(const QModelIndex& index) const;
    QModelIndex indexOf(EntityId id) const;

private:
    std::span<const EntityId> childrenOf(EntityId id) const;

    Scene& scene_;
    Scene::ListenerId listener_ = 0;
    QIcon objectIcon_;
    QIcon groupIcon_;
};

} // namespace lb::app
