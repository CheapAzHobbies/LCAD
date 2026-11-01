// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Error.h>
#include <lcad/kernel/Exchange.h>
#include <lcad/kernel/Measure.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/Plane.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/Stop.h>

#include <catch2/catch_test_macros.hpp>

#include <functional>

using namespace lcad::kernel;

namespace {

// Asks to stop for as long as it is in scope.
struct Stopping {
    Stopping() { requestStop(); }
    ~Stopping() { clearStop(); }
    Stopping(const Stopping&) = delete;
    Stopping& operator=(const Stopping&) = delete;
};

} // namespace

TEST_CASE("Asked to stop, geometry never comes to a wrong answer", "[kernel][stop]") {
    const NamedShape box = nameFaces(makeBox(40.0, 40.0, 40.0), "1/f");
    const NamedShape ball = nameFaces(makeSphere(25.0), "2/f");

    SECTION("It does not stay asked") {
        CHECK_FALSE(stopRequested());
        {
            const Stopping stopping;
            CHECK(stopRequested());
        }
        CHECK_FALSE(stopRequested());
    }
    SECTION("Without being asked, it all works") {
        CHECK(volume(combine(box, ball, Combination::Cut).shape) > 0.0);
        CHECK_FALSE(triangulate(box.shape, {0.1, 20.0}).triangles.empty());
    }
    SECTION("Combining") {
        const Stopping stopping;
        CHECK_THROWS_AS(combine(box, ball, Combination::Join), Error);
        CHECK_THROWS_AS(combine(box, ball, Combination::Cut), Error);
        CHECK_THROWS_AS(combine(box, ball, Combination::Intersect), Error);
    }
    SECTION("Rounding and chamfering look up only now and then, and either stop or are right") {
        const Stopping stopping;
        const auto stoppedOrRight = [&box](const std::function<NamedShape()>& make) {
            try {
                const double made = volume(make().shape);
                CHECK(made > 0.0);
                CHECK(made < volume(box.shape));
            } catch (const Error&) {
                SUCCEED("it stopped");
            }
        };
        stoppedOrRight([&box] { return fillet(box, {0}, 2.0, "3"); });
        stoppedOrRight([&box] { return chamfer(box, {0}, 2.0, "3"); });
        stoppedOrRight([&box] { return shell(box, {0}, 2.0, "3"); });
    }
    SECTION("And after it, the same work goes through") {
        {
            const Stopping stopping;
            CHECK_THROWS_AS(combine(box, ball, Combination::Cut), Error);
        }
        CHECK(volume(combine(box, ball, Combination::Cut).shape) > 0.0);
    }
}
