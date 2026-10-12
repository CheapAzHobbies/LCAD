// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <utility>

namespace lcad::kernel {

/// Exact boundary-representation geometry: a solid, shell, face, wire, edge, vertex, or a
/// compound of them.
///
/// A Shape is a value. Copies are cheap and share the same geometry. A default-constructed
/// Shape is null.
///
/// The geometry kernel's own types never appear in this interface. Code that has to reach them
/// includes lcad/kernel/occt/ShapeAccess.h.
class Shape {
public:
    Shape() = default;

    [[nodiscard]] bool isNull() const { return m_impl == nullptr; }

    struct Impl;
    explicit Shape(std::shared_ptr<const Impl> impl) : m_impl(std::move(impl)) {}
    [[nodiscard]] const Impl* impl() const { return m_impl.get(); }

private:
    std::shared_ptr<const Impl> m_impl;
};

} // namespace lcad::kernel
