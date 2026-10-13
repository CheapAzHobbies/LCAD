// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/kernel/Serialization.h>

#include <lcad/kernel/occt/Guard.h>
#include <lcad/kernel/occt/ShapeAccess.h>

#include <BinTools.hxx>
#include <BinTools_FormatVersion.hxx>

#include <sstream>
#include <stdexcept>

namespace lcad::kernel {

namespace {

// Pinned, never "current": OCCT 7.6, the oldest version LCAD supports, reads and writes
// version 4, and newer OCCT versions still read it. See docs/FILE_FORMAT.md.
const BinTools_FormatVersion brepFormatVersion = BinTools_FormatVersion_VERSION_4;

// Every file in this format starts with these bytes, whatever its version.
constexpr std::string_view brepSignature = "\nOpen CASCADE Topology V";

} // namespace

std::string toBrep(const Shape& shape) {
    if (shape.isNull()) {
        throw std::invalid_argument("toBrep: a null shape has no geometry to write");
    }
    return occt::guarded("write geometry", [&] {
        std::ostringstream stream(std::ios::out | std::ios::binary);
        const bool withTriangles = false;
        const bool withNormals = false;
        BinTools::Write(occt::unwrap(shape), stream, withTriangles, withNormals, brepFormatVersion);
        if (!stream.good()) {
            throw Error("write geometry", "the shape could not be encoded");
        }
        return std::move(stream).str();
    });
}

Shape fromBrep(std::string_view bytes) {
    // OCCT's reader reports data it does not recognize on its message channel and returns an
    // empty shape, so check the signature here and say what is wrong.
    if (!bytes.starts_with(brepSignature)) {
        throw Error("read geometry", "the data is not in the binary B-rep format");
    }
    return occt::guarded("read geometry", [&] {
        std::istringstream stream(std::string(bytes), std::ios::in | std::ios::binary);
        TopoDS_Shape shape;
        BinTools::Read(shape, stream);
        if (shape.IsNull() || stream.fail()) {
            throw Error("read geometry", "the data is incomplete or damaged");
        }
        return occt::wrap(shape);
    });
}

} // namespace lcad::kernel
