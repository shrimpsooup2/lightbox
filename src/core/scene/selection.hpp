#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

#include "core/scene/scene.hpp"

namespace lb {

// The set of selected entities of one scene, in selection order (the last one is the primary, shown in
// the inspector). Stays consistent with the scene: destroyed entities drop out automatically.
// Every effective change notifies listeners; setting the same selection again is silent.
class Selection {
public:
    using Listener = std::function<void()>;
    using ListenerId = std::uint64_t;

    explicit Selection(Scene& scene);
    ~Selection();
    Selection(const Selection&) = delete;
    Selection& operator=(const Selection&) = delete;

    // Replaces the selection. Unknown IDs and duplicates are ignored.
    void set(std::span<const EntityId> ids);
    void select(EntityId id); // replace with a single entity (none = clear)
    void clear();

    bool contains(EntityId id) const;
    bool empty() const { return ids_.empty(); }
    std::span<const EntityId> ids() const { return ids_; }
    EntityId primary() const { return ids_.empty() ? EntityId{} : ids_.back(); }

    ListenerId addListener(Listener listener);
    void removeListener(ListenerId id);

private:
    void notify();

    Scene& scene_;
    Scene::ListenerId sceneListener_ = 0;
    std::vector<EntityId> ids_;
    std::vector<std::pair<ListenerId, Listener>> listeners_;
    ListenerId nextListenerId_ = 1;
};

} // namespace lb
