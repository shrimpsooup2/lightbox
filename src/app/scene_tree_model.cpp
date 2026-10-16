#include "app/scene_tree_model.hpp"

#include <algorithm>
#include <cstdint>

#include "app/theme.hpp"
#include "app/ui_helpers.hpp"

namespace lb::app {

static_assert(sizeof(quintptr) >= sizeof(std::uint64_t), "entity IDs are stored in index internal IDs");

SceneTreeModel::SceneTreeModel(Scene& scene, QObject* parent)
    : QAbstractItemModel(parent),
      scene_(scene),
      objectIcon_(theme::icon(QStringLiteral("box"))),
      groupIcon_(theme::icon(QStringLiteral("folder"))) {
    listener_ = scene_.addListener([this](const SceneChange& change) {
        switch (change.kind) {
        case SceneChange::Kind::EntityAdded:
        case SceneChange::Kind::EntityRemoved:
        case SceneChange::Kind::EntityReparented:
            // Structural edits are rare (user actions, imports); a reset keeps the model simple.
            // The outliner restores expansion and selection afterwards.
            beginResetModel();
            endResetModel();
            break;
        case SceneChange::Kind::EntityRenamed:
        case SceneChange::Kind::MeshInstanceChanged: {
            const QModelIndex index = indexOf(change.entity);
            emit dataChanged(index, index);
            break;
        }
        default:
            break;
        }
    });
}

SceneTreeModel::~SceneTreeModel() {
    scene_.removeListener(listener_);
}

std::span<const EntityId> SceneTreeModel::childrenOf(EntityId id) const {
    if (!id) {
        return scene_.roots();
    }
    const Entity* entity = scene_.entity(id);
    return entity ? std::span<const EntityId>(entity->children) : std::span<const EntityId>{};
}

EntityId SceneTreeModel::entityAt(const QModelIndex& index) const {
    return index.isValid() ? EntityId{static_cast<std::uint64_t>(index.internalId())} : EntityId{};
}

QModelIndex SceneTreeModel::indexOf(EntityId id) const {
    const Entity* entity = scene_.entity(id);
    if (entity == nullptr) {
        return {};
    }
    const std::span<const EntityId> siblings = childrenOf(entity->parent);
    const auto it = std::find(siblings.begin(), siblings.end(), id);
    if (it == siblings.end()) {
        return {};
    }
    return createIndex(static_cast<int>(it - siblings.begin()), 0, static_cast<quintptr>(id.value));
}

QModelIndex SceneTreeModel::index(int row, int column, const QModelIndex& parent) const {
    const std::span<const EntityId> siblings = childrenOf(entityAt(parent));
    if (column != 0 || row < 0 || static_cast<std::size_t>(row) >= siblings.size()) {
        return {};
    }
    return createIndex(row, 0, static_cast<quintptr>(siblings[static_cast<std::size_t>(row)].value));
}

QModelIndex SceneTreeModel::parent(const QModelIndex& child) const {
    const Entity* entity = scene_.entity(entityAt(child));
    return entity && entity->parent ? indexOf(entity->parent) : QModelIndex{};
}

int SceneTreeModel::rowCount(const QModelIndex& parent) const {
    if (parent.column() > 0) {
        return 0;
    }
    return static_cast<int>(childrenOf(entityAt(parent)).size());
}

int SceneTreeModel::columnCount(const QModelIndex&) const {
    return 1;
}

QVariant SceneTreeModel::data(const QModelIndex& index, int role) const {
    const Entity* entity = scene_.entity(entityAt(index));
    if (entity == nullptr) {
        return {};
    }
    switch (role) {
    case Qt::DisplayRole:
    case Qt::EditRole:
        return QString::fromStdString(entity->name);
    case Qt::DecorationRole:
        return entity->meshInstance ? objectIcon_ : groupIcon_;
    case Qt::ToolTipRole:
        if (entity->meshInstance) {
            const MeshResource* mesh = scene_.mesh(entity->meshInstance->mesh);
            const MaterialResource* material = scene_.material(entity->meshInstance->material);
            return tr("Object · mesh %1 · material %2")
                .arg(mesh ? QString::fromStdString(mesh->name) : tr("none"),
                     material ? QString::fromStdString(material->name) : tr("default"));
        }
        return tr("Group · %1").arg(ui::count(static_cast<int>(entity->children.size()),
                                              tr("1 item"), tr("%1 items")));
    default:
        return {};
    }
}

bool SceneTreeModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    const EntityId id = entityAt(index);
    const QString name = value.toString().trimmed();
    if (role != Qt::EditRole || !id || name.isEmpty()) {
        return false;
    }
    scene_.setName(id, name.toStdString());
    return true;
}

Qt::ItemFlags SceneTreeModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable;
}

} // namespace lb::app
