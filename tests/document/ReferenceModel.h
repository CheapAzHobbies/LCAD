// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/Document.h>
#include <lcad/kernel/Primitives.h>
#include <lcad/kernel/Transform.h>

#include <memory>

namespace lcad::test {

/// The document behind tests/models/format-1/primitives-occt-*.lcad: one of each primitive,
/// each somewhere else, one hidden, one with a name outside ASCII, and one body deleted again
/// so that the ids have a gap. ReferenceModelTest.cpp states what a reader must find.
inline std::unique_ptr<document::Document> buildReferenceDocument() {
    using namespace lcad::kernel;
    auto document = std::make_unique<document::Document>();
    document->addBody("Box", makeBox(10.0, 20.0, 30.0));
    document->addBody("Cylinder", translated(makeCylinder(5.0, 12.0), 40.0, 0.0, 0.0));
    const document::ObjectId scrap = document->addBody("Scrap", makeBox(1.0, 1.0, 1.0));
    const document::ObjectId sphere =
        document->addBody("Kugel Ø14", translated(makeSphere(7.0), 0.0, 40.0, 0.0));
    document->addBody("Cone", translated(makeCone(6.0, 2.0, 9.0), 40.0, 40.0, 0.0));
    document->addBody("Torus", translated(makeTorus(10.0, 3.0), 0.0, 0.0, 40.0));
    document->removeBody(scrap);
    document->setBodyVisible(sphere, false);
    return document;
}

} // namespace lcad::test
