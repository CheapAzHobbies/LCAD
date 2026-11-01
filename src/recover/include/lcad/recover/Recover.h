// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/Document.h>
#include <lcad/kernel/Exchange.h>

#include <memory>
#include <string>
#include <vector>

// A mesh turned into a model that can be edited.
namespace lcad::recover {

/// How far a part of a mesh could be taken.
enum class Result {
    /// Sketches and features, as if it had been drawn in LCAD: fillets are Fillets, holes are
    /// Holes, and the sizes are dimensions.
    Features,
    /// A solid of exact faces under features: its fillets are Fillets and its holes are Holes, and
    /// what lies under them, which is no stack of sketches, takes the direct edits.
    FeaturesOnSolid,
    /// One solid of exact faces, which the direct edits work on, with no features.
    ExactSolid,
    /// One solid of flat facets, as the mesh has it.
    Facets,
    /// Not closed, so no solid.
    Failed,
};

/// One part of a mesh, and what became of it.
struct PartResult {
    /// What it is called: the name of the mesh, with a number if it has several parts.
    std::string name;
    Result result = Result::Failed;
    int pulls = 0;
    int holes = 0;
    int fillets = 0;
    int chamfers = 0;
    /// The share of its area on real surfaces rather than facets, from 0 to 1.
    double recognized = 0.0;
    /// Why it did not come out as features all the way, for the user. Empty if it did.
    std::string note;
};

/// What addMesh() did.
struct MeshResult {
    std::vector<PartResult> parts;
    /// The bodies it made.
    std::vector<document::ObjectId> bodies;
};

/// What a mesh comes to, worked out but not yet in any document.
class WorkedMesh {
public:
    struct Parts;
    explicit WorkedMesh(std::shared_ptr<const Parts> parts);
    [[nodiscard]] const Parts& parts() const { return *m_parts; }

private:
    std::shared_ptr<const Parts> m_parts;
};

/// Works out how far each part of a mesh can be taken, and checks each against the mesh: what does
/// not come to the same shape is not kept. This is the long part, and touches no document, so it can
/// go on a thread of its own. Throws kernel::Error when stopped (see Stop.h).
[[nodiscard]] WorkedMesh workOutMesh(const kernel::Mesh& mesh, const std::string& name);

/// Adds what was worked out to a document, as one undo step.
MeshResult addWorkedMesh(document::Document& document, const WorkedMesh& worked,
                         document::ObjectId component = {});

/// Both of those.
MeshResult addMesh(document::Document& document, const kernel::Mesh& mesh, const std::string& name,
                   document::ObjectId component = {});

} // namespace lcad::recover
