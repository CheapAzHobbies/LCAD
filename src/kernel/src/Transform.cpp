// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Transform.h>

#include <lcad/kernel/occt/Guard.h>
#include <lcad/kernel/occt/ShapeAccess.h>

#include <TopLoc_Location.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <stdexcept>

namespace lcad::kernel {

Shape translated(const Shape& shape, double dx, double dy, double dz) {
    if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz)) {
        throw std::invalid_argument("translated: the distances must be finite numbers");
    }
    if (shape.isNull()) {
        return shape;
    }
    return occt::guarded("move shape", [&] {
        gp_Trsf transform;
        transform.SetTranslation(gp_Vec(dx, dy, dz));
        return occt::wrap(occt::unwrap(shape).Moved(TopLoc_Location(transform)));
    });
}

} // namespace lcad::kernel
