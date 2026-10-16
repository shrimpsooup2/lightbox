#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace lb {

// Strongly typed, persistent 64-bit identifier. Zero means "none". IDs are random (see Scene), so they
// stay unique when content from different projects or re-imports is merged.
template <class Tag>
struct Id {
    std::uint64_t value = 0;

    constexpr explicit operator bool() const { return value != 0; }
    friend constexpr auto operator<=>(const Id&, const Id&) = default;
};

using EntityId = Id<struct EntityTag>;
using ResourceId = Id<struct ResourceTag>;

} // namespace lb

template <class Tag>
struct std::hash<lb::Id<Tag>> {
    std::size_t operator()(lb::Id<Tag> id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value);
    }
};
