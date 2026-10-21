// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/kernel/Transform.h>

#include <gp_Trsf.hxx>

namespace lcad::kernel::occt {

/// A placement as OCCT's transformation, for modules that hand transformations to OCCT
/// themselves. The placement has to be rigid (see isRigid()); it is made exactly so first.
[[nodiscard]] inline gp_Trsf toTrsf(const Placement& placement) {
    const Placement p = normalized(placement);
    gp_Trsf transform;
    // The columns are where X, Y, and Z go; the last one is the move.
    transform.SetValues(p.xAxis.x, p.yAxis.x, p.zAxis.x, p.origin.x, p.xAxis.y, p.yAxis.y,
                        p.zAxis.y, p.origin.y, p.xAxis.z, p.yAxis.z, p.zAxis.z, p.origin.z);
    return transform;
}

} // namespace lcad::kernel::occt
