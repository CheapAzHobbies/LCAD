// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Primitives.h>

#include <lcad/kernel/occt/Guard.h>
#include <lcad/kernel/occt/ShapeAccess.h>

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>

#include <cmath>
#include <stdexcept>
#include <string>

namespace lcad::kernel {

namespace {

[[noreturn]] void reject(const char* function, const std::string& problem) {
    throw std::invalid_argument(std::string(function) + ": " + problem);
}

void requireLength(double value, const char* function, const char* argument) {
    if (!std::isfinite(value) || value <= minimumLength) {
        reject(function, std::string(argument) + " must be a finite length greater than " +
                             std::to_string(minimumLength) + " mm, got " + std::to_string(value));
    }
}

// A radius that is allowed to be exactly zero.
void requireLengthOrZero(double value, const char* function, const char* argument) {
    if (value != 0.0) {
        requireLength(value, function, argument);
    }
}

} // namespace

Shape makeBox(double sizeX, double sizeY, double sizeZ) {
    requireLength(sizeX, "makeBox", "sizeX");
    requireLength(sizeY, "makeBox", "sizeY");
    requireLength(sizeZ, "makeBox", "sizeZ");
    return occt::guarded(
        "make box", [&] { return occt::wrap(BRepPrimAPI_MakeBox(sizeX, sizeY, sizeZ).Solid()); });
}

Shape makeCylinder(double radius, double height) {
    requireLength(radius, "makeCylinder", "radius");
    requireLength(height, "makeCylinder", "height");
    return occt::guarded("make cylinder", [&] {
        return occt::wrap(BRepPrimAPI_MakeCylinder(radius, height).Solid());
    });
}

Shape makeSphere(double radius) {
    requireLength(radius, "makeSphere", "radius");
    return occt::guarded("make sphere",
                         [&] { return occt::wrap(BRepPrimAPI_MakeSphere(radius).Solid()); });
}

Shape makeCone(double baseRadius, double topRadius, double height) {
    requireLengthOrZero(baseRadius, "makeCone", "baseRadius");
    requireLengthOrZero(topRadius, "makeCone", "topRadius");
    requireLength(height, "makeCone", "height");
    if (std::abs(baseRadius - topRadius) <= minimumLength) {
        reject("makeCone", "baseRadius and topRadius must differ, got " +
                               std::to_string(baseRadius) + " for both; use makeCylinder");
    }
    return occt::guarded("make cone", [&] {
        return occt::wrap(BRepPrimAPI_MakeCone(baseRadius, topRadius, height).Solid());
    });
}

Shape makeTorus(double majorRadius, double minorRadius) {
    requireLength(majorRadius, "makeTorus", "majorRadius");
    requireLength(minorRadius, "makeTorus", "minorRadius");
    if (majorRadius - minorRadius <= minimumLength) {
        reject("makeTorus", "minorRadius must be smaller than majorRadius, got " +
                                std::to_string(minorRadius) + " and " +
                                std::to_string(majorRadius));
    }
    return occt::guarded("make torus", [&] {
        return occt::wrap(BRepPrimAPI_MakeTorus(majorRadius, minorRadius).Solid());
    });
}

} // namespace lcad::kernel
