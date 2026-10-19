// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/kernel/Check.h>
#include <lcad/kernel/Exchange.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/Transform.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

namespace lcad::test {

// The STEP file that tests/models/step/ keeps one copy of per OCCT version: two parts, a plate
// 60 by 40 by 10 with a hole of radius 5 through it at (30, 20), and a pin of radius 4 and
// length 25 standing at (100, 0).
inline std::vector<kernel::NamedBody> referenceStepBodies() {
    using namespace lcad::kernel;
    const NamedShape box = nameFaces(makeBox(60.0, 40.0, 10.0), "b");
    const NamedShape hole = nameFaces(translated(makeCylinder(5.0, 12.0), 30.0, 20.0, -1.0), "h");
    return {{"Bracket", combine(box, hole, Combination::Cut).shape},
            {"Pin", translated(makeCylinder(4.0, 25.0), 100.0, 0.0, 0.0)}};
}

// What a reader must find in it.
inline void checkReferenceStep(const kernel::ImportResult& read) {
    using Catch::Matchers::WithinAbs;
    using Catch::Matchers::WithinRel;
    const double pi = 3.14159265358979323846;
    CHECK(read.notes.empty());
    REQUIRE(read.bodies.size() == 2);
    CHECK(read.bodies[0].name == "Bracket");
    CHECK(read.bodies[1].name == "Pin");
    for (const kernel::NamedBody& body : read.bodies) {
        CHECK(kernel::checkGeometry(body.shape).empty());
    }
    CHECK_THAT(kernel::volume(read.bodies[0].shape),
               WithinRel((60.0 * 40.0 - pi * 25.0) * 10.0, 1e-9));
    CHECK_THAT(kernel::volume(read.bodies[1].shape), WithinRel(pi * 16.0 * 25.0, 1e-9));
    const auto pin = kernel::boundingBox(read.bodies[1].shape);
    REQUIRE(pin.has_value());
    CHECK_THAT(pin->min.x, WithinAbs(96.0, 1e-6));
    CHECK_THAT(pin->max.z, WithinAbs(25.0, 1e-6));
    // Exact surfaces, not facets: the plate's faces are the box's six and the hole's wall.
    CHECK(kernel::countTopology(read.bodies[0].shape).faces == 7);
}

} // namespace lcad::test
