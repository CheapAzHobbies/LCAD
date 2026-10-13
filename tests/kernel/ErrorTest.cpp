// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Error.h>
#include <lcad/kernel/occt/Guard.h>

#include <Standard_DomainError.hxx>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>

using namespace lcad::kernel;

TEST_CASE("A kernel error says what was attempted and why it failed", "[kernel][error]") {
    const Error error("fillet edge", "the radius is larger than the face");

    CHECK(error.operation() == "fillet edge");
    CHECK(error.reason() == "the radius is larger than the face");
    CHECK(std::string(error.what()) == "fillet edge: the radius is larger than the face");
}

TEST_CASE("An OCCT exception leaves the kernel as a kernel error", "[kernel][error]") {
    try {
        occt::guarded("test operation", []() -> int { throw Standard_DomainError("bad value"); });
        FAIL("guarded() swallowed the exception");
    } catch (const Error& error) {
        CHECK(error.operation() == "test operation");
        CHECK(error.reason() == "Standard_DomainError: bad value");
    }
}

TEST_CASE("guarded() passes results and other exceptions through", "[kernel][error]") {
    CHECK(occt::guarded("test operation", [] { return 42; }) == 42);
    CHECK_THROWS_AS(
        occt::guarded("test operation", []() -> int { throw std::invalid_argument("mine"); }),
        std::invalid_argument);
}
