// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/kernel/Shape.h>

#include <TopoDS_Shape.hxx>

namespace lcad::kernel {

struct Shape::Impl {
    TopoDS_Shape shape;
};

} // namespace lcad::kernel
