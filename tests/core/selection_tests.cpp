#include <vector>

#include <doctest/doctest.h>

#include "core/color.hpp"
#include "core/scene/scene.hpp"
#include "core/scene/selection.hpp"

using namespace lb;

namespace {

std::vector<EntityId> idsOf(const Selection& selection) {
    return {selection.ids().begin(), selection.ids().end()};
}

} // namespace

TEST_CASE("selection keeps order, ignores unknown ids and duplicates") {
    Scene scene;
    const EntityId a = scene.createEntity("a");
    const EntityId b = scene.createEntity("b");
    Selection selection(scene);

    const EntityId requested[] = {b, EntityId{42}, a, b};
    selection.set(requested);
    CHECK(idsOf(selection) == std::vector<EntityId>{b, a});
    CHECK(selection.primary() == a);
}

TEST_CASE("selection drops destroyed entities, including descendants") {
    Scene scene;
    const EntityId group = scene.createEntity("group");
    const EntityId child = scene.createEntity("child", group);
    const EntityId other = scene.createEntity("other");
    Selection selection(scene);
    const EntityId requested[] = {child, other};
    selection.set(requested);

    int notifications = 0;
    selection.addListener([&] { ++notifications; });
    scene.destroyEntity(group);
    CHECK(idsOf(selection) == std::vector<EntityId>{other});
    CHECK(notifications == 1);
}

TEST_CASE("selection notifies only on real changes") {
    Scene scene;
    const EntityId a = scene.createEntity("a");
    Selection selection(scene);
    int notifications = 0;
    const Selection::ListenerId listener = selection.addListener([&] { ++notifications; });

    selection.select(a);
    selection.select(a); // same selection: silent
    selection.clear();
    selection.clear(); // already empty: silent
    CHECK(notifications == 2);

    selection.removeListener(listener);
    selection.select(a);
    CHECK(notifications == 2);
}

TEST_CASE("sRGB transfer functions round-trip and hit reference points") {
    CHECK(srgbToLinear(0.0f) == doctest::Approx(0.0f));
    CHECK(srgbToLinear(1.0f) == doctest::Approx(1.0f));
    CHECK(srgbToLinear(0.5f) == doctest::Approx(0.21404f).epsilon(1e-4));
    CHECK(linearToSrgb(0.18f) == doctest::Approx(0.46135f).epsilon(1e-4)); // 18% grey
    for (float v : {0.001f, 0.02f, 0.3f, 0.75f, 0.99f}) {
        CHECK(linearToSrgb(srgbToLinear(v)) == doctest::Approx(v).epsilon(1e-5));
    }
    CHECK(srgbToLinear(1.5f) == doctest::Approx(1.0f)); // clamped
}
