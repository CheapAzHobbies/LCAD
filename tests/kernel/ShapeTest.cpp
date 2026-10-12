// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/Shape.h>
#include <lcad/kernel/occt/ShapeAccess.h>

#include <catch2/catch_test_macros.hpp>

using namespace lcad::kernel;

TEST_CASE("A default-constructed Shape is null", "[kernel][shape]") {
    const Shape shape;

    CHECK(shape.isNull());
    CHECK(occt::unwrap(shape).IsNull());
}

TEST_CASE("Wrapping a null OCCT shape gives a null Shape", "[kernel][shape]") {
    CHECK(occt::wrap(TopoDS_Shape()).isNull());
}

TEST_CASE("Copies of a Shape refer to the same geometry", "[kernel][shape]") {
    const Shape original = makeBox(1.0, 2.0, 3.0);
    const Shape copy = original;

    CHECK_FALSE(copy.isNull());
    CHECK(occt::unwrap(copy).IsSame(occt::unwrap(original)));
}
