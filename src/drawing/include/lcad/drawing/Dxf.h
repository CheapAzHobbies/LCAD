// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/drawing/Sheet.h>

#include <filesystem>
#include <string>

// DXF, the file other CAD programs and laser cutters read 2D geometry from.
namespace lcad::drawing {

/// A sheet as the text of an ASCII DXF file, in millimeters, in the R12 form that about every
/// program reads. Each kind of line has a layer of its own. Lines stay lines and circles stay
/// circles; other curves are polylines.
[[nodiscard]] std::string dxfText(const Sheet& sheet);

/// Writes dxfText() to a file. Throws std::runtime_error, with a sentence fit to show, if it
/// cannot be written.
void writeDxf(const Sheet& sheet, const std::filesystem::path& path);

} // namespace lcad::drawing
