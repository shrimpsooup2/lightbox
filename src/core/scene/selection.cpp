#include "core/scene/selection.hpp"

#include <algorithm>

namespace lb {

Selection::Selection(Scene& scene) : scene_(scene) {
    sceneListener_ = scene_.addListener([this](const SceneChange& change) {
        if (change.kind != SceneChange::Kind::EntityRemoved || !contains(change.entity)) {
            return;
        }
        std::erase(ids_, change.entity);
        notify();
    });
}

Selection::~Selection() {
    scene_.removeListener(sceneListener_);
}

void Selection::set(std::span<const EntityId> ids) {
    std::vector<EntityId> next;
    next.reserve(ids.size());
    for (EntityId id : ids) {
        if (scene_.entity(id) != nullptr && std::find(next.begin(), next.end(), id) == next.end()) {
            next.push_back(id);
        }
    }
    if (next == ids_) {
        return;
    }
    ids_ = std::move(next);
    notify();
}

void Selection::select(EntityId id) {
    if (id) {
        set(std::span<const EntityId>(&id, 1));
    } else {
        clear();
    }
}

void Selection::clear() {
    set({});
}

void Selection::toggle(EntityId id) {
    if (!id || scene_.entity(id) == nullptr) {
        return;
    }
    std::vector<EntityId> next(ids_.begin(), ids_.end());
    if (const auto it = std::find(next.begin(), next.end(), id); it != next.end()) {
        next.erase(it);
    } else {
        next.push_back(id); // the newest becomes the primary
    }
    set(next);
}

bool Selection::contains(EntityId id) const {
    return std::find(ids_.begin(), ids_.end(), id) != ids_.end();
}

Selection::ListenerId Selection::addListener(Listener listener) {
    const ListenerId id = nextListenerId_++;
    listeners_.emplace_back(id, std::move(listener));
    return id;
}

void Selection::removeListener(ListenerId id) {
    std::erase_if(listeners_, [id](const auto& entry) { return entry.first == id; });
}

void Selection::notify() {
    const auto listeners = listeners_;
    for (const auto& [id, listener] : listeners) {
        listener();
    }
}

} // namespace lb
