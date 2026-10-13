// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Error.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/Serialization.h>
#include <lcad/kernel/Transform.h>

#include <Tolerances.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <stdexcept>
#include <string>

using namespace lcad::kernel;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using lcad::test::exactPropertyTolerance;
using lcad::test::lengthTolerance;

namespace {

void checkSameMeasurements(const Shape& a, const Shape& b) {
    // The bytes hold the exact geometry, so the copy measures the same to rounding.
    CHECK_THAT(volume(b), WithinRel(volume(a), exactPropertyTolerance));
    CHECK_THAT(surfaceArea(b), WithinRel(surfaceArea(a), exactPropertyTolerance));
    const TopologyCount ca = countTopology(a);
    const TopologyCount cb = countTopology(b);
    CHECK(cb.solids == ca.solids);
    CHECK(cb.shells == ca.shells);
    CHECK(cb.faces == ca.faces);
    CHECK(cb.wires == ca.wires);
    CHECK(cb.edges == ca.edges);
    CHECK(cb.vertices == ca.vertices);
    const auto ba = boundingBox(a);
    const auto bb = boundingBox(b);
    REQUIRE(ba.has_value());
    REQUIRE(bb.has_value());
    CHECK_THAT(bb->min.x, WithinAbs(ba->min.x, lengthTolerance));
    CHECK_THAT(bb->min.y, WithinAbs(ba->min.y, lengthTolerance));
    CHECK_THAT(bb->min.z, WithinAbs(ba->min.z, lengthTolerance));
    CHECK_THAT(bb->max.x, WithinAbs(ba->max.x, lengthTolerance));
    CHECK_THAT(bb->max.y, WithinAbs(ba->max.y, lengthTolerance));
    CHECK_THAT(bb->max.z, WithinAbs(ba->max.z, lengthTolerance));
}

} // namespace

TEST_CASE("A shape written to bytes reads back as the same geometry", "[kernel][serialization]") {
    const int which = GENERATE(0, 1, 2, 3, 4, 5);
    CAPTURE(which);
    Shape original;
    switch (which) {
    case 0:
        original = makeBox(10.0, 20.0, 30.0);
        break;
    case 1:
        original = makeCylinder(5.0, 12.0);
        break;
    case 2:
        original = makeSphere(7.0);
        break;
    case 3:
        original = makeCone(6.0, 2.0, 9.0);
        break;
    case 4:
        original = makeTorus(10.0, 3.0);
        break;
    default:
        // A moved shape: its position has to survive too.
        original = translated(makeBox(4.0, 5.0, 6.0), 100.0, -20.0, 3.0);
        break;
    }

    const std::string bytes = toBrep(original);
    const Shape copy = fromBrep(bytes);

    REQUIRE_FALSE(copy.isNull());
    CHECK(isValid(copy));
    checkSameMeasurements(original, copy);
}

TEST_CASE("Writing the same shape twice gives the same bytes", "[kernel][serialization]") {
    const Shape box = makeBox(1.0, 2.0, 3.0);

    CHECK(toBrep(box) == toBrep(box));
}

TEST_CASE("A null shape cannot be written", "[kernel][serialization]") {
    CHECK_THROWS_AS(toBrep(Shape()), std::invalid_argument);
}

TEST_CASE("Bytes that are not geometry are refused", "[kernel][serialization]") {
    CHECK_THROWS_AS(fromBrep(""), Error);
    CHECK_THROWS_AS(fromBrep("this is not a shape"), Error);
    CHECK_THROWS_AS(fromBrep(std::string(4096, '\0')), Error);

    try {
        (void)fromBrep("PK\x03\x04 a zip file, say");
        FAIL("garbage was accepted");
    } catch (const Error& error) {
        CHECK(error.operation() == "read geometry");
        CHECK(error.reason() == "the data is not in the binary B-rep format");
    }
}

TEST_CASE("Geometry cut short is refused, wherever the cut falls", "[kernel][serialization]") {
    const std::string bytes = toBrep(makeCylinder(5.0, 12.0));
    REQUIRE(bytes.size() > 200);

    std::size_t accepted = 0;
    for (std::size_t keep = 0; keep < bytes.size(); ++keep) {
        try {
            (void)fromBrep(std::string_view(bytes).substr(0, keep));
            ++accepted;
            UNSCOPED_INFO("accepted a prefix of " << keep << " of " << bytes.size() << " bytes");
        } catch (const Error&) {
        }
    }
    CHECK(accepted == 0);
}

TEST_CASE("A valid signature followed by something else is refused", "[kernel][serialization]") {
    const std::string bytes = toBrep(makeBox(1.0, 2.0, 3.0));
    // The signature line ends at the second line break.
    const std::size_t headerEnd = bytes.find('\n', 1);
    REQUIRE(headerEnd != std::string::npos);
    const std::string header = bytes.substr(0, headerEnd + 1);
    CAPTURE(header);

    CHECK_THROWS_AS(fromBrep(header), Error);
    CHECK_THROWS_AS(fromBrep(header + std::string(256, '\0')), Error);
    CHECK_THROWS_AS(fromBrep(header + "Locations 0\nand then nothing useful"), Error);
}
