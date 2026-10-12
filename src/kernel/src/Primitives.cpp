// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Primitives.h>

#include <lcad/kernel/occt/ShapeAccess.h>

#include <BRepPrimAPI_MakeBox.hxx>

#include <cmath>
#include <stdexcept>
#include <string>

namespace lcad::kernel {

namespace {

void requireLength(double value, const char* function, const char* argument) {
    if (!std::isfinite(value) || value <= minimumLength) {
        throw std::invalid_argument(
            std::string(function) + ": " + argument + " must be a finite length greater than " +
            std::to_string(minimumLength) + " mm, got " + std::to_string(value));
    }
}

} // namespace

Shape makeBox(double sizeX, double sizeY, double sizeZ) {
    requireLength(sizeX, "makeBox", "sizeX");
    requireLength(sizeY, "makeBox", "sizeY");
    requireLength(sizeZ, "makeBox", "sizeZ");
    return occt::wrap(BRepPrimAPI_MakeBox(sizeX, sizeY, sizeZ).Solid());
}

} // namespace lcad::kernel
