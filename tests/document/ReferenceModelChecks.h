// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/Document.h>
#include <lcad/kernel/Measure.h>

#include <Tolerances.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <string>

namespace lcad::test {

struct ExpectedBody {
    std::uint64_t id;
    std::string name;
    bool visible;
    int faces;
    double volume;
    double area;
    kernel::Point min;
    kernel::Point max;
};

inline void checkBody(const document::Document& document, const ExpectedBody& expected) {
    using Catch::Matchers::WithinAbs;
    using Catch::Matchers::WithinRel;
    INFO("body \"" << expected.name << "\"");
    const document::Body* body = document.findBody(document::ObjectId{expected.id});
    REQUIRE(body != nullptr);
    CHECK(body->name == expected.name);
    CHECK(body->visible == expected.visible);
    CHECK(kernel::isValid(body->shape));
    const kernel::TopologyCount count = kernel::countTopology(body->shape);
    CHECK(count.solids == 1);
    CHECK(count.faces == expected.faces);
    CHECK_THAT(kernel::volume(body->shape), WithinRel(expected.volume, curvedPropertyTolerance));
    CHECK_THAT(kernel::surfaceArea(body->shape), WithinRel(expected.area, curvedPropertyTolerance));
    const auto bounds = kernel::boundingBox(body->shape);
    REQUIRE(bounds.has_value());
    CHECK_THAT(bounds->min.x, WithinAbs(expected.min.x, lengthTolerance));
    CHECK_THAT(bounds->min.y, WithinAbs(expected.min.y, lengthTolerance));
    CHECK_THAT(bounds->min.z, WithinAbs(expected.min.z, lengthTolerance));
    CHECK_THAT(bounds->max.x, WithinAbs(expected.max.x, lengthTolerance));
    CHECK_THAT(bounds->max.y, WithinAbs(expected.max.y, lengthTolerance));
    CHECK_THAT(bounds->max.z, WithinAbs(expected.max.z, lengthTolerance));
}

/// What a correct reader finds in the reference document (ReferenceModel.h). The numbers are
/// the closed-form values for each primitive, not values recorded from a run.
inline void checkReferenceDocument(const document::Document& document) {
    const double pi = 3.14159265358979323846;
    REQUIRE(document.bodies().size() == 5);
    // Creation order, with id 3 missing: that body was deleted before saving.
    CHECK(document.bodies()[0].id == document::ObjectId{1});
    CHECK(document.bodies()[1].id == document::ObjectId{2});
    CHECK(document.bodies()[2].id == document::ObjectId{4});
    CHECK(document.bodies()[3].id == document::ObjectId{5});
    CHECK(document.bodies()[4].id == document::ObjectId{6});
    CHECK(document.nextObjectId() == 7);

    checkBody(document, {1, "Box", true, 6, 6000.0, 2200.0, {0.0, 0.0, 0.0}, {10.0, 20.0, 30.0}});
    checkBody(
        document,
        {2, "Cylinder", true, 3, 300.0 * pi, 170.0 * pi, {35.0, -5.0, 0.0}, {45.0, 5.0, 12.0}});
    checkBody(document, {4,
                         "Kugel Ø14",
                         false,
                         1,
                         4.0 / 3.0 * pi * 343.0,
                         196.0 * pi,
                         {-7.0, 33.0, -7.0},
                         {7.0, 47.0, 7.0}});
    const double slant = std::sqrt(16.0 + 81.0);
    checkBody(document, {5,
                         "Cone",
                         true,
                         3,
                         156.0 * pi,
                         pi * 8.0 * slant + pi * 36.0 + pi * 4.0,
                         {34.0, 34.0, 0.0},
                         {46.0, 46.0, 9.0}});
    checkBody(document, {6,
                         "Torus",
                         true,
                         1,
                         180.0 * pi * pi,
                         120.0 * pi * pi,
                         {-13.0, -13.0, 37.0},
                         {13.0, 13.0, 43.0}});
}

} // namespace lcad::test
