// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace lcad::document {

/// Identifies one object (so far: a body) within its document. An id is never reused, not even
/// after its object is deleted, and it is what the file format refers to objects by.
struct ObjectId {
    std::uint64_t value = 0;

    /// Whether this refers to an object at all. A default-constructed id does not.
    [[nodiscard]] bool isValid() const { return value != 0; }

    friend auto operator<=>(const ObjectId&, const ObjectId&) = default;
};

} // namespace lcad::document

template <>
struct std::hash<lcad::document::ObjectId> {
    std::size_t operator()(const lcad::document::ObjectId& id) const noexcept {
        return std::hash<std::uint64_t>()(id.value);
    }
};
