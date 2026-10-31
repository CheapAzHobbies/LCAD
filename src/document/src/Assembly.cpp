// SPDX-License-Identifier: GPL-3.0-or-later

// Components, occurrences, joints, and materials: the parts of the Document that make an
// assembly, and the placing of occurrences by their joints.

#include <lcad/document/Document.h>

#include "Rebuild.h"

#include <lcad/kernel/Error.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/SubShapes.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace lcad::document {

namespace {

constexpr std::size_t notFound = static_cast<std::size_t>(-1);
constexpr double pi = 3.14159265358979323846;

bool isBlank(const std::string& name) {
    return name.find_first_not_of(" \t\r\n") == std::string::npos;
}

void requireName(const std::string& name, const char* function) {
    if (isBlank(name)) {
        throw std::invalid_argument(std::string(function) + ": a name is needed");
    }
}

kernel::Placement turn(const kernel::Vector& axis, double degrees) {
    return kernel::rotation(kernel::Axis{{0.0, 0.0, 0.0}, axis}, degrees * pi / 180.0);
}

// A joint's values, kept within its limits.
std::array<double, 3> limited(const Joint& joint, std::array<double, 3> values) {
    for (std::size_t i = 0; i < values.size(); ++i) {
        const JointLimits& limits = joint.limits[i];
        if (limits.minimum.has_value()) {
            values[i] = std::max(values[i], *limits.minimum);
        }
        if (limits.maximum.has_value()) {
            values[i] = std::min(values[i], *limits.maximum);
        }
    }
    return values;
}

// How the moving side's frame stands to the fixed side's: stood off and turned, moved as far
// as the joint has, and turned over to face it unless flipped.
kernel::Placement linkOf(const Joint& joint, const std::array<double, 3>& values) {
    const kernel::Vector x{1.0, 0.0, 0.0};
    const kernel::Vector y{0.0, 1.0, 0.0};
    const kernel::Vector z{0.0, 0.0, 1.0};
    kernel::Placement link = kernel::translation({0.0, 0.0, joint.offset}) * turn(z, joint.angle);
    switch (joint.kind) {
    case JointKind::Rigid:
        break;
    case JointKind::Revolute:
        link = link * turn(z, values[0]);
        break;
    case JointKind::Slider:
        link = link * kernel::translation({0.0, 0.0, values[0]});
        break;
    case JointKind::Cylindrical:
        link = link * kernel::translation({0.0, 0.0, values[1]}) * turn(z, values[0]);
        break;
    case JointKind::PinSlot:
        link = link * kernel::translation({values[1], 0.0, 0.0}) * turn(z, values[0]);
        break;
    case JointKind::Planar:
        link = link * kernel::translation({values[0], values[1], 0.0}) * turn(z, values[2]);
        break;
    case JointKind::Ball:
        link = link * turn(z, values[2]) * turn(y, values[1]) * turn(x, values[0]);
        break;
    }
    if (!joint.flipped) {
        link = link * turn(x, 180.0);
    }
    return kernel::normalized(link);
}

// A joint that cannot hold. The message is a sentence for the user.
class JointFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

using Placements = std::unordered_map<ObjectId, kernel::Placement>;

struct Solution {
    Placements placements;
    std::unordered_map<ObjectId, std::string> errors;
};

kernel::Placement placementIn(const Document& document, const Placements& solved,
                              ObjectId occurrence) {
    const auto found = solved.find(occurrence);
    if (found != solved.end()) {
        return found->second;
    }
    const Occurrence* stored = document.findOccurrence(occurrence);
    return stored != nullptr ? stored->placement : kernel::Placement{};
}

// What a side is on, in words: "a face of Bracket:1".
std::string sideName(const Document& document, const JointSide& side) {
    if (side.path.empty()) {
        return "the assembly itself";
    }
    return document.occurrenceName(side.path.back());
}

// Where a side's frame is, in the coordinates of the first occurrence on its path, or of the
// joint's component for an empty path. The occurrences further down are placed already.
kernel::Placement sideFrame(const Document& document, const JointSide& side,
                            const Placements& solved) {
    kernel::Placement inner;
    for (std::size_t i = 1; i < side.path.size(); ++i) {
        inner = inner * placementIn(document, solved, side.path[i]);
    }
    if (side.kind == JointSide::Kind::Origin) {
        return inner;
    }
    const Body* body = document.findBody(side.body);
    if (body == nullptr || body->shape.isNull()) {
        throw JointFailure("The body it is placed on, on " + sideName(document, side) +
                           ", is gone.");
    }
    const kernel::NamedShape named{body->shape, body->faceNames};
    const bool face = side.kind == JointSide::Kind::Face;
    const int index =
        face ? kernel::findFace(named, side.name) : kernel::findEdge(named, side.name);
    if (index < 0) {
        throw JointFailure(
            std::string("The ") + (face ? "face" : "edge") + " it is placed on no longer exists: " +
            detail::describeName(document, side.name) + ", on " + sideName(document, side) + ".");
    }
    const kernel::Shape part = kernel::subShape(
        body->shape, face ? kernel::SubShapeKind::Face : kernel::SubShapeKind::Edge, index);
    const std::optional<kernel::Placement> frame = kernel::frameOn(part, side.snap);
    if (!frame.has_value()) {
        throw JointFailure(std::string("The ") + (face ? "face" : "edge") +
                           " it is placed on, on " + sideName(document, side) +
                           ", gives no direction to line up by.");
    }
    return inner * *frame;
}

// The components in an order in which each comes after every component inside it: the root
// last.
std::vector<ObjectId> innermostFirst(const Document& document) {
    std::vector<ObjectId> order;
    std::unordered_set<ObjectId> seen;
    std::function<void(ObjectId)> visit = [&](ObjectId component) {
        for (const ObjectId occurrence : document.occurrencesIn(component)) {
            const ObjectId inner = document.findOccurrence(occurrence)->component;
            if (seen.insert(inner).second) {
                visit(inner);
            }
        }
        order.push_back(component);
    };
    visit(ObjectId{});
    return order;
}

// Places the occurrences directly inside one component by its joints. Those inside them are
// placed already.
void solveComponent(const Document& document, ObjectId component, const std::vector<Joint>& joints,
                    Solution& solution) {
    // Where each is in the component; the invalid id is the component itself, which does not
    // move.
    Placements placed;
    placed[ObjectId{}] = kernel::Placement{};
    std::vector<ObjectId> children;
    for (const ObjectId id : document.occurrencesIn(component)) {
        const Occurrence* occurrence = document.findOccurrence(id);
        if (occurrence->suppressed) {
            continue;
        }
        children.push_back(id);
        if (occurrence->grounded) {
            placed[id] = occurrence->placement;
        }
    }
    struct Pending {
        const Joint* joint;
        ObjectId moving;
        ObjectId fixed;
        kernel::Placement movingFrame;
        kernel::Placement fixedFrame;
        kernel::Placement link;
    };
    std::vector<Pending> pending;
    for (const Joint& joint : joints) {
        if (joint.component != component || joint.suppressed) {
            continue;
        }
        try {
            for (const JointSide* side : {&joint.moving, &joint.fixed}) {
                for (const ObjectId id : side->path) {
                    const Occurrence* occurrence = document.findOccurrence(id);
                    if (occurrence == nullptr) {
                        throw JointFailure("A component it holds is gone.");
                    }
                    if (occurrence->suppressed) {
                        throw JointFailure(document.occurrenceName(id) + " is suppressed.");
                    }
                }
            }
            Pending entry{&joint,
                          joint.moving.path.empty() ? ObjectId{} : joint.moving.path.front(),
                          joint.fixed.path.empty() ? ObjectId{} : joint.fixed.path.front(),
                          sideFrame(document, joint.moving, solution.placements),
                          sideFrame(document, joint.fixed, solution.placements),
                          linkOf(joint, limited(joint, joint.values))};
            if (entry.moving == entry.fixed) {
                throw JointFailure(
                    entry.moving.isValid()
                        ? "Both of its sides are on " + document.occurrenceName(entry.moving) + "."
                        : "Both of its sides are on the assembly itself, which does not move.");
            }
            pending.push_back(entry);
        } catch (const JointFailure& failure) {
            solution.errors[joint.id] = failure.what();
        } catch (const kernel::Error& failure) {
            solution.errors[joint.id] = "Its frames cannot be found (" + failure.reason() + ").";
        }
    }
    while (!pending.empty()) {
        bool progress = false;
        for (auto entry = pending.begin(); entry != pending.end();) {
            const bool movingPlaced = placed.contains(entry->moving);
            const bool fixedPlaced = placed.contains(entry->fixed);
            if (!movingPlaced && !fixedPlaced) {
                ++entry;
                continue;
            }
            // The moving frame, turned over, lies on the fixed frame after the link:
            //   moving * movingFrame = fixed * fixedFrame * link
            if (fixedPlaced && !movingPlaced) {
                placed[entry->moving] =
                    kernel::normalized(placed[entry->fixed] * entry->fixedFrame * entry->link *
                                       kernel::inverse(entry->movingFrame));
            } else if (movingPlaced && !fixedPlaced) {
                // The side meant to move cannot: the other one moves instead.
                placed[entry->fixed] = kernel::normalized(
                    placed[entry->moving] * entry->movingFrame * kernel::inverse(entry->link) *
                    kernel::inverse(entry->fixedFrame));
            } else {
                const kernel::Placement wanted =
                    placed[entry->fixed] * entry->fixedFrame * entry->link;
                const kernel::Placement actual = placed[entry->moving] * entry->movingFrame;
                if (!kernel::samePlacement(wanted, actual, 1e-6)) {
                    const auto nameOf = [&document](ObjectId id) {
                        return id.isValid() ? document.occurrenceName(id)
                                            : std::string("the assembly itself");
                    };
                    solution.errors[entry->joint->id] =
                        "It cannot hold together with the other joints: " + nameOf(entry->moving) +
                        " and " + nameOf(entry->fixed) +
                        " are placed already, grounded or by them, and not where it would put "
                        "them.";
                }
            }
            entry = pending.erase(entry);
            progress = true;
        }
        if (!progress) {
            // Components joined to each other but to nothing that stays put: the one the first
            // joint moves another to stays where it was put.
            placed[pending.front().fixed] = placementIn(document, {}, pending.front().fixed);
        }
    }
    for (const ObjectId id : children) {
        solution.placements[id] = placementIn(document, placed, id);
    }
}

Solution solve(const Document& document, const std::vector<Joint>& joints) {
    Solution solution;
    for (const ObjectId component : innermostFirst(document)) {
        solveComponent(document, component, joints, solution);
    }
    return solution;
}

std::vector<Instance> instancesOf(const Document& document, const Placements& placements) {
    std::vector<Instance> instances{Instance{}};
    std::function<void(ObjectId, const std::vector<ObjectId>&, const kernel::Placement&, bool)>
        visit = [&](ObjectId component, const std::vector<ObjectId>& path,
                    const kernel::Placement& where, bool visible) {
            for (const ObjectId id : document.occurrencesIn(component)) {
                const Occurrence* occurrence = document.findOccurrence(id);
                if (occurrence->suppressed) {
                    continue;
                }
                Instance instance;
                instance.path = path;
                instance.path.push_back(id);
                instance.component = occurrence->component;
                instance.placement =
                    kernel::normalized(where * placementIn(document, placements, id));
                instance.visible = visible && occurrence->visible;
                instances.push_back(instance);
                visit(occurrence->component, instance.path, instance.placement, instance.visible);
            }
        };
    visit(ObjectId{}, {}, kernel::Placement{}, true);
    return instances;
}

// "plane:12" with 12 changed by `map`, and anything else as it is.
std::string remapped(const std::string& reference,
                     const std::unordered_map<ObjectId, ObjectId>& map) {
    for (const char* prefix : {"plane:", "axis:"}) {
        const std::string start(prefix);
        if (reference.rfind(start, 0) != 0) {
            continue;
        }
        try {
            const ObjectId id{std::stoull(reference.substr(start.size()))};
            const auto found = map.find(id);
            if (found != map.end()) {
                return start + std::to_string(found->second.value);
            }
        } catch (const std::exception&) {
        }
    }
    return reference;
}

// The construction plane or axis a reference names, or an invalid id.
ObjectId constructionNamed(const std::string& reference) {
    for (const char* prefix : {"plane:", "axis:"}) {
        const std::string start(prefix);
        if (reference.rfind(start, 0) == 0) {
            try {
                return ObjectId{std::stoull(reference.substr(start.size()))};
            } catch (const std::exception&) {
                return {};
            }
        }
    }
    return {};
}

} // namespace

const char* toString(JointKind kind) {
    switch (kind) {
    case JointKind::Rigid:
        return "Rigid";
    case JointKind::Revolute:
        return "Revolute";
    case JointKind::Slider:
        return "Slider";
    case JointKind::Cylindrical:
        return "Cylindrical";
    case JointKind::PinSlot:
        return "Pin-Slot";
    case JointKind::Planar:
        return "Planar";
    case JointKind::Ball:
        return "Ball";
    }
    return "Rigid";
}

std::optional<JointKind> jointKindFromString(const std::string& name) {
    for (const JointKind kind :
         {JointKind::Rigid, JointKind::Revolute, JointKind::Slider, JointKind::Cylindrical,
          JointKind::PinSlot, JointKind::Planar, JointKind::Ball}) {
        if (name == toString(kind)) {
            return kind;
        }
    }
    return std::nullopt;
}

std::vector<JointMotion> motionsOf(JointKind kind) {
    switch (kind) {
    case JointKind::Rigid:
        return {};
    case JointKind::Revolute:
        return {{"Rotation", true}};
    case JointKind::Slider:
        return {{"Slide", false}};
    case JointKind::Cylindrical:
        return {{"Rotation", true}, {"Slide", false}};
    case JointKind::PinSlot:
        return {{"Rotation", true}, {"Slide", false}};
    case JointKind::Planar:
        return {{"Slide X", false}, {"Slide Y", false}, {"Rotation", true}};
    case JointKind::Ball:
        return {{"Pitch", true}, {"Yaw", true}, {"Roll", true}};
    }
    return {};
}

const std::vector<Material>& materialLibrary() {
    // Typical densities; a part printed hollow or with infill weighs less than its material.
    static const std::vector<Material> library{
        {"PLA", 1.24},
        {"PETG", 1.27},
        {"ABS", 1.04},
        {"ASA", 1.07},
        {"TPU", 1.21},
        {"Nylon", 1.14},
        {"Polycarbonate", 1.20},
        {"Resin", 1.15},
        {"Acrylic", 1.18},
        {"Acetal (POM)", 1.41},
        {"Carbon fiber composite", 1.55},
        {"Fiberglass (FR-4)", 1.85},
        {"Aluminum", 2.70},
        {"Steel", 7.85},
        {"Stainless steel", 8.00},
        {"Brass", 8.50},
        {"Copper", 8.96},
        {"Titanium", 4.43},
        {"Pine", 0.50},
        {"Oak", 0.75},
        {"Plywood", 0.60},
    };
    return library;
}

// ---------------------------------------------------------------------------------------------
// Reading

const Component* Document::findComponent(ObjectId id) const {
    const std::size_t index = componentIndexOf(id);
    return index == notFound ? nullptr : &m_components[index];
}

const Occurrence* Document::findOccurrence(ObjectId id) const {
    const std::size_t index = occurrenceIndexOf(id);
    return index == notFound ? nullptr : &m_occurrences[index];
}

const Joint* Document::findJoint(ObjectId id) const {
    const std::size_t index = jointIndexOf(id);
    return index == notFound ? nullptr : &m_joints[index];
}

std::vector<ObjectId> Document::occurrencesOf(ObjectId component) const {
    std::vector<ObjectId> found;
    for (const Occurrence& occurrence : m_occurrences) {
        if (occurrence.component == component) {
            found.push_back(occurrence.id);
        }
    }
    return found;
}

std::vector<ObjectId> Document::occurrencesIn(ObjectId component) const {
    std::vector<ObjectId> found;
    for (const Occurrence& occurrence : m_occurrences) {
        if (occurrence.parent == component) {
            found.push_back(occurrence.id);
        }
    }
    return found;
}

std::string Document::occurrenceName(ObjectId id) const {
    const Occurrence* occurrence = findOccurrence(id);
    if (occurrence == nullptr) {
        return {};
    }
    const Component* component = findComponent(occurrence->component);
    const std::vector<ObjectId> all = occurrencesOf(occurrence->component);
    const auto number = std::find(all.begin(), all.end(), id) - all.begin() + 1;
    return (component != nullptr ? component->name : std::string("Component")) + ":" +
           std::to_string(number);
}

ObjectId Document::componentOf(ObjectId item) const {
    if (const Body* body = findBody(item)) {
        return body->component;
    }
    if (const SketchItem* sketch = findSketch(item)) {
        return sketch->component;
    }
    if (const Feature* feature = findFeature(item)) {
        return feature->component;
    }
    if (const Joint* joint = findJoint(item)) {
        return joint->component;
    }
    return {};
}

std::string Document::uniqueComponentName(std::string_view base) const {
    for (int number = 1;; ++number) {
        const std::string candidate = std::string(base) + " " + std::to_string(number);
        const bool taken =
            std::any_of(m_components.begin(), m_components.end(),
                        [&candidate](const Component& c) { return c.name == candidate; });
        if (!taken) {
            return candidate;
        }
    }
}

std::string Document::uniqueJointName(std::string_view base) const {
    for (int number = 1;; ++number) {
        const std::string candidate = std::string(base) + " " + std::to_string(number);
        const bool taken =
            std::any_of(m_joints.begin(), m_joints.end(),
                        [&candidate](const Joint& j) { return j.name == candidate; });
        if (!taken) {
            return candidate;
        }
    }
}

std::string Document::jointError(ObjectId id) const {
    const auto found = m_jointErrors.find(id);
    return found == m_jointErrors.end() ? std::string() : found->second;
}

kernel::Placement Document::placementOf(ObjectId occurrence) const {
    return placementIn(*this, m_solved, occurrence);
}

kernel::Placement Document::placementOf(const std::vector<ObjectId>& path) const {
    kernel::Placement where;
    for (const ObjectId occurrence : path) {
        where = where * placementOf(occurrence);
    }
    return kernel::normalized(where);
}

std::vector<Instance>
Document::instancesWith(const std::map<ObjectId, std::array<double, 3>>& values) const {
    std::vector<Joint> joints = m_joints;
    for (Joint& joint : joints) {
        if (const auto found = values.find(joint.id); found != values.end()) {
            joint.values = found->second;
        }
    }
    return instancesOf(*this, solve(*this, joints).placements);
}

JointTrial Document::tryJoint(const Joint& joint) const {
    JointTrial trial;
    if (const std::string problem = jointProblem(joint); !problem.empty()) {
        trial.error = problem;
        trial.instances = m_instances;
        return trial;
    }
    std::vector<Joint> joints = m_joints;
    Joint tried = joint;
    if (!tried.id.isValid()) {
        // An id no object has, to tell its result apart.
        tried.id = ObjectId{m_nextId};
    }
    const auto same = std::find_if(joints.begin(), joints.end(),
                                   [&tried](const Joint& j) { return j.id == tried.id; });
    if (same != joints.end()) {
        *same = tried;
    } else {
        joints.push_back(tried);
    }
    const Solution solution = solve(*this, joints);
    if (const auto error = solution.errors.find(tried.id); error != solution.errors.end()) {
        trial.error = error->second;
    }
    trial.instances = instancesOf(*this, solution.placements);
    return trial;
}

std::optional<Material> Document::findMaterial(std::string_view name) const {
    for (const Material& material : m_materials) {
        if (material.name == name) {
            return material;
        }
    }
    for (const Material& material : materialLibrary()) {
        if (material.name == name) {
            return material;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// Checks

std::size_t Document::componentIndexOf(ObjectId id) const {
    for (std::size_t i = 0; i < m_components.size(); ++i) {
        if (m_components[i].id == id) {
            return i;
        }
    }
    return notFound;
}

std::size_t Document::occurrenceIndexOf(ObjectId id) const {
    for (std::size_t i = 0; i < m_occurrences.size(); ++i) {
        if (m_occurrences[i].id == id) {
            return i;
        }
    }
    return notFound;
}

std::size_t Document::jointIndexOf(ObjectId id) const {
    for (std::size_t i = 0; i < m_joints.size(); ++i) {
        if (m_joints[i].id == id) {
            return i;
        }
    }
    return notFound;
}

std::size_t Document::materialIndexOf(const std::string& name) const {
    for (std::size_t i = 0; i < m_materials.size(); ++i) {
        if (m_materials[i].name == name) {
            return i;
        }
    }
    return notFound;
}

bool Document::contains(ObjectId outer, ObjectId inner) const {
    if (outer == inner) {
        return true;
    }
    std::unordered_set<ObjectId> seen;
    std::vector<ObjectId> open{outer};
    while (!open.empty()) {
        const ObjectId component = open.back();
        open.pop_back();
        for (const Occurrence& occurrence : m_occurrences) {
            if (occurrence.parent != component || !occurrence.parent.isValid()) {
                continue;
            }
            if (occurrence.component == inner) {
                return true;
            }
            if (seen.insert(occurrence.component).second) {
                open.push_back(occurrence.component);
            }
        }
    }
    // The root holds everything.
    return !outer.isValid();
}

void Document::requireOwnComponent(ObjectId component, const char* function) const {
    if (!component.isValid()) {
        return;
    }
    const Component* found = findComponent(component);
    if (found == nullptr) {
        throw std::invalid_argument(std::string(function) + ": there is no component with id " +
                                    std::to_string(component.value));
    }
    if (!found->link.empty()) {
        throw std::invalid_argument(std::string(function) + ": " + found->name +
                                    " is linked from another document; change it there");
    }
}

ObjectId Document::componentForFeature(const Feature& feature) const {
    const bool makes =
        (feature.kind == FeatureKind::Pull || feature.kind == FeatureKind::Revolve) &&
        feature.operation == Operation::NewBody;
    if (!makes) {
        if (const Body* body = findBody(feature.body)) {
            return body->component;
        }
    }
    if (const SketchItem* sketch = findSketch(feature.sketch)) {
        return sketch->component;
    }
    for (const std::string& reference : {feature.reference, feature.reference2}) {
        if (const Feature* construction = findFeature(constructionNamed(reference))) {
            return construction->component;
        }
    }
    return feature.component;
}

std::string Document::componentProblem(const Feature& feature) const {
    const auto where = [this](ObjectId component) {
        const Component* found = findComponent(component);
        return found != nullptr ? found->name : std::string("the assembly itself");
    };
    const auto check = [&](ObjectId component, const std::string& what) -> std::string {
        if (component == feature.component) {
            return {};
        }
        return what + " is part of " + where(component) + ", and " + feature.name +
               " would be part of " + where(feature.component) +
               ": a feature works within one component.";
    };
    const bool makes =
        (feature.kind == FeatureKind::Pull || feature.kind == FeatureKind::Revolve) &&
        feature.operation == Operation::NewBody;
    if (const Body* body = findBody(feature.body); body != nullptr && !makes) {
        if (!body->linkKey.empty()) {
            return body->name + " is linked from another document; change it there.";
        }
        if (std::string problem = check(body->component, body->name); !problem.empty()) {
            return problem;
        }
    }
    if (const SketchItem* sketch = findSketch(feature.sketch)) {
        if (std::string problem = check(sketch->component, sketch->name); !problem.empty()) {
            return problem;
        }
    }
    if (const Body* limit = findBody(feature.extentBody)) {
        if (std::string problem = check(limit->component, limit->name); !problem.empty()) {
            return problem;
        }
    }
    for (const std::string& reference : {feature.reference, feature.reference2}) {
        if (const Feature* construction = findFeature(constructionNamed(reference))) {
            if (std::string problem = check(construction->component, construction->name);
                !problem.empty()) {
                return problem;
            }
        }
    }
    return {};
}

std::string Document::jointProblem(const Joint& joint) const {
    if (joint.component.isValid()) {
        const Component* component = findComponent(joint.component);
        if (component == nullptr) {
            return "it is part of a component the document does not have";
        }
        if (!component->link.empty()) {
            return component->name + " is linked from another document";
        }
    }
    for (const JointSide* side : {&joint.moving, &joint.fixed}) {
        // Each occurrence on the path is inside the component before it.
        ObjectId inside = joint.component;
        for (const ObjectId id : side->path) {
            const Occurrence* occurrence = findOccurrence(id);
            if (occurrence == nullptr || occurrence->parent != inside) {
                return "a side of it does not lie in the component it is part of";
            }
            inside = occurrence->component;
        }
        if (side->kind != JointSide::Kind::Origin) {
            const Body* body = findBody(side->body);
            if (body == nullptr || body->component != inside) {
                return "a side of it is on a body that is not where its path leads";
            }
            if (side->name.empty()) {
                return "a side of it names no face or edge";
            }
        }
    }
    for (std::size_t i = 0; i < joint.values.size(); ++i) {
        const JointLimits& limits = joint.limits[i];
        if (!std::isfinite(joint.values[i]) ||
            (limits.minimum.has_value() && limits.maximum.has_value() &&
             *limits.minimum > *limits.maximum)) {
            return "its values or limits are not numbers, or a least is more than a most";
        }
    }
    if (!std::isfinite(joint.offset) || !std::isfinite(joint.angle)) {
        return "its offset and angle have to be numbers";
    }
    return {};
}

// ---------------------------------------------------------------------------------------------
// Editing

void Document::inOneStep(const std::string& label, const std::function<void()>& edits) {
    if (m_open) {
        edits();
        return;
    }
    beginTransaction(label);
    try {
        edits();
    } catch (...) {
        rollbackTransaction();
        throw;
    }
    commitTransaction();
}

ObjectId Document::addComponent(std::string name, ObjectId parent,
                                const kernel::Placement& placement) {
    requireName(name, "addComponent");
    requireOwnComponent(parent, "addComponent");
    if (!kernel::isRigid(placement)) {
        throw std::invalid_argument("addComponent: a placement only turns and moves");
    }
    const ObjectId id{m_nextId++};
    const std::string label = "New Component";
    inOneStep(label, [&] {
        Edit edit{};
        edit.kind = Edit::Kind::AddComponent;
        edit.id = id;
        edit.index = m_components.size();
        edit.component = Component{id, std::move(name), {}, {}};
        record(std::move(edit), label);
        addOccurrence(id, parent, placement);
    });
    return id;
}

ObjectId Document::addOccurrence(ObjectId component, ObjectId parent,
                                 const kernel::Placement& placement) {
    if (findComponent(component) == nullptr) {
        throw std::invalid_argument("addOccurrence: there is no component with id " +
                                    std::to_string(component.value));
    }
    requireOwnComponent(parent, "addOccurrence");
    if (contains(component, parent)) {
        throw std::invalid_argument("addOccurrence: " + findComponent(component)->name +
                                    " cannot be put inside itself");
    }
    if (!kernel::isRigid(placement)) {
        throw std::invalid_argument("addOccurrence: a placement only turns and moves");
    }
    Edit edit{};
    edit.kind = Edit::Kind::AddOccurrence;
    edit.id = ObjectId{m_nextId++};
    edit.index = m_occurrences.size();
    edit.occurrence = Occurrence{edit.id, component, parent, kernel::normalized(placement)};
    const ObjectId id = edit.id;
    record(std::move(edit), "Insert " + findComponent(component)->name);
    return id;
}

void Document::removeOccurrence(ObjectId id) {
    const Occurrence* found = findOccurrence(id);
    if (found == nullptr) {
        throw std::invalid_argument("removeOccurrence: there is no occurrence with id " +
                                    std::to_string(id.value));
    }
    const Occurrence occurrence = *found;
    inOneStep("Delete " + occurrenceName(id), [&] {
        // The joints that hold it, or anything inside it, go with it.
        std::vector<ObjectId> holding;
        for (const Joint& joint : m_joints) {
            for (const JointSide* side : {&joint.moving, &joint.fixed}) {
                if (std::find(side->path.begin(), side->path.end(), id) != side->path.end()) {
                    holding.push_back(joint.id);
                    break;
                }
            }
        }
        for (const ObjectId joint : holding) {
            removeJoint(joint);
        }
        Edit edit{};
        edit.kind = Edit::Kind::RemoveOccurrence;
        edit.id = id;
        edit.index = occurrenceIndexOf(id);
        edit.occurrence = occurrence;
        record(std::move(edit), "Delete Occurrence");
        if (occurrencesOf(occurrence.component).empty()) {
            removeComponentContents(occurrence.component);
        }
    });
}

void Document::removeComponentContents(ObjectId component) {
    for (const ObjectId inner : occurrencesIn(component)) {
        if (findOccurrence(inner) != nullptr) {
            removeOccurrence(inner);
        }
    }
    std::vector<ObjectId> joints;
    for (const Joint& joint : m_joints) {
        if (joint.component == component) {
            joints.push_back(joint.id);
        }
    }
    for (const ObjectId joint : joints) {
        removeJoint(joint);
    }
    // Features last applied first, then the bodies and sketches they were made from.
    std::vector<ObjectId> features;
    for (const ObjectId item : m_timeline) {
        const Feature* feature = findFeature(item);
        if (feature != nullptr && feature->component == component) {
            features.push_back(item);
        }
    }
    for (auto feature = features.rbegin(); feature != features.rend(); ++feature) {
        if (findFeature(*feature) != nullptr) {
            removeFeature(*feature);
        }
    }
    std::vector<ObjectId> bodies;
    for (const Body& body : m_bodies) {
        if (body.component == component) {
            bodies.push_back(body.id);
        }
    }
    for (const ObjectId body : bodies) {
        if (findBody(body) != nullptr) {
            removeBody(body);
        }
    }
    std::vector<ObjectId> sketches;
    for (const SketchItem& sketch : m_sketches) {
        if (sketch.component == component) {
            sketches.push_back(sketch.id);
        }
    }
    for (const ObjectId sketch : sketches) {
        removeSketch(sketch);
    }
    Edit edit{};
    edit.kind = Edit::Kind::RemoveComponent;
    edit.id = component;
    edit.index = componentIndexOf(component);
    edit.component = m_components[edit.index];
    record(std::move(edit), "Delete Component");
}

void Document::renameComponent(ObjectId id, std::string name) {
    const Component* component = findComponent(id);
    if (component == nullptr) {
        throw std::invalid_argument("renameComponent: there is no component with id " +
                                    std::to_string(id.value));
    }
    requireName(name, "renameComponent");
    if (component->name == name) {
        return;
    }
    Edit edit{};
    edit.kind = Edit::Kind::ChangeComponent;
    edit.id = id;
    edit.component = *component;
    edit.newComponent = *component;
    edit.newComponent.name = std::move(name);
    record(std::move(edit), "Rename Component");
}

namespace {

// The edit of one occurrence: what it was, and what it becomes.
template <typename Change>
void changeOccurrence(const Document& document, ObjectId id, const char* function, Change change,
                      Occurrence& before, Occurrence& after) {
    const Occurrence* occurrence = document.findOccurrence(id);
    if (occurrence == nullptr) {
        throw std::invalid_argument(std::string(function) + ": there is no occurrence with id " +
                                    std::to_string(id.value));
    }
    before = *occurrence;
    after = *occurrence;
    change(after);
}

} // namespace

void Document::setOccurrencePlacement(ObjectId id, const kernel::Placement& placement) {
    if (!kernel::isRigid(placement)) {
        throw std::invalid_argument("setOccurrencePlacement: a placement only turns and moves");
    }
    Edit edit{};
    edit.kind = Edit::Kind::ChangeOccurrence;
    edit.id = id;
    changeOccurrence(
        *this, id, "setOccurrencePlacement",
        [&placement](Occurrence& o) { o.placement = kernel::normalized(placement); },
        edit.occurrence, edit.newOccurrence);
    if (edit.occurrence == edit.newOccurrence) {
        return;
    }
    record(std::move(edit), "Move " + occurrenceName(id));
}

void Document::setOccurrenceGrounded(ObjectId id, bool grounded) {
    Edit edit{};
    edit.kind = Edit::Kind::ChangeOccurrence;
    edit.id = id;
    changeOccurrence(
        *this, id, "setOccurrenceGrounded", [grounded](Occurrence& o) { o.grounded = grounded; },
        edit.occurrence, edit.newOccurrence);
    if (edit.occurrence == edit.newOccurrence) {
        return;
    }
    // A component that is grounded where joints put it stays there.
    if (grounded) {
        edit.newOccurrence.placement = placementOf(id);
    }
    record(std::move(edit), (grounded ? "Ground " : "Unground ") + occurrenceName(id));
}

void Document::setOccurrenceVisible(ObjectId id, bool visible) {
    Edit edit{};
    edit.kind = Edit::Kind::ChangeOccurrence;
    edit.id = id;
    changeOccurrence(
        *this, id, "setOccurrenceVisible", [visible](Occurrence& o) { o.visible = visible; },
        edit.occurrence, edit.newOccurrence);
    if (edit.occurrence == edit.newOccurrence) {
        return;
    }
    record(std::move(edit), (visible ? "Show " : "Hide ") + occurrenceName(id));
}

void Document::setOccurrenceSuppressed(ObjectId id, bool suppressed) {
    Edit edit{};
    edit.kind = Edit::Kind::ChangeOccurrence;
    edit.id = id;
    changeOccurrence(
        *this, id, "setOccurrenceSuppressed",
        [suppressed](Occurrence& o) { o.suppressed = suppressed; }, edit.occurrence,
        edit.newOccurrence);
    if (edit.occurrence == edit.newOccurrence) {
        return;
    }
    record(std::move(edit), (suppressed ? "Suppress " : "Restore ") + occurrenceName(id));
}

void Document::moveOccurrence(ObjectId id, ObjectId parent) {
    const Occurrence* occurrence = findOccurrence(id);
    if (occurrence == nullptr) {
        throw std::invalid_argument("moveOccurrence: there is no occurrence with id " +
                                    std::to_string(id.value));
    }
    if (occurrence->parent == parent) {
        return;
    }
    requireOwnComponent(parent, "moveOccurrence");
    if (contains(occurrence->component, parent)) {
        throw std::invalid_argument(occurrenceName(id) + " cannot be put inside itself.");
    }
    for (const Joint& joint : m_joints) {
        for (const JointSide* side : {&joint.moving, &joint.fixed}) {
            if (std::find(side->path.begin(), side->path.end(), id) != side->path.end()) {
                throw std::invalid_argument(joint.name + " holds " + occurrenceName(id) +
                                            " where it is. Delete the joint first.");
            }
        }
    }
    // Where it is now, and where the new parent is, through the first occurrence of each.
    const auto firstPath = [this](ObjectId target, bool ofComponent) -> std::vector<ObjectId> {
        for (const Instance& instance : m_instances) {
            if (ofComponent ? instance.component == target
                            : (!instance.path.empty() && instance.path.back() == target)) {
                return instance.path;
            }
        }
        return {};
    };
    const kernel::Placement now = placementOf(firstPath(id, false));
    const kernel::Placement parentAt =
        parent.isValid() ? placementOf(firstPath(parent, true)) : kernel::Placement{};
    Edit edit{};
    edit.kind = Edit::Kind::ChangeOccurrence;
    edit.id = id;
    edit.occurrence = *occurrence;
    edit.newOccurrence = *occurrence;
    edit.newOccurrence.parent = parent;
    edit.newOccurrence.placement = kernel::normalized(kernel::inverse(parentAt) * now);
    record(std::move(edit), "Move " + occurrenceName(id));
}

ObjectId Document::makeComponent(std::string name, const std::vector<ObjectId>& bodyIds) {
    requireName(name, "makeComponent");
    if (bodyIds.empty()) {
        throw std::invalid_argument("makeComponent: no bodies are given");
    }
    const Body* first = findBody(bodyIds.front());
    if (first == nullptr) {
        throw std::invalid_argument("makeComponent: there is no body with id " +
                                    std::to_string(bodyIds.front().value));
    }
    const ObjectId from = first->component;
    std::unordered_set<ObjectId> bodies;
    for (const ObjectId id : bodyIds) {
        const Body* body = findBody(id);
        if (body == nullptr) {
            throw std::invalid_argument("makeComponent: there is no body with id " +
                                        std::to_string(id.value));
        }
        if (!body->linkKey.empty()) {
            throw std::invalid_argument(body->name + " is linked from another document.");
        }
        if (body->component != from) {
            throw std::invalid_argument(
                "The bodies are parts of different components; choose bodies of one.");
        }
        bodies.insert(id);
    }
    // What moves: the features that make or change the bodies, the sketches and the
    // construction geometry they use, and the sketches drawn on the bodies.
    std::unordered_set<ObjectId> features;
    std::unordered_set<ObjectId> sketches;
    for (const Feature& feature : m_features) {
        if (bodies.contains(feature.body) && feature.component == from) {
            features.insert(feature.id);
        }
    }
    // Construction geometry used by what moves moves too; repeat until nothing more is added.
    for (bool grew = true; grew;) {
        grew = false;
        const auto take = [&](ObjectId construction) {
            const Feature* found = findFeature(construction);
            if (found != nullptr && found->component == from &&
                features.insert(construction).second) {
                grew = true;
            }
        };
        for (const ObjectId id : std::vector<ObjectId>(features.begin(), features.end())) {
            const Feature* feature = findFeature(id);
            take(constructionNamed(feature->reference));
            take(constructionNamed(feature->reference2));
            if (const SketchItem* sketch = findSketch(feature->sketch)) {
                sketches.insert(sketch->id);
            }
        }
        for (const SketchItem& sketch : m_sketches) {
            if (bodies.contains(sketch.attachedTo)) {
                sketches.insert(sketch.id);
            }
        }
        for (const ObjectId id : std::vector<ObjectId>(sketches.begin(), sketches.end())) {
            take(findSketch(id)->attachedTo);
        }
    }
    // Nothing that stays may use what moves, and nothing that moves may use what stays.
    for (const Feature& feature : m_features) {
        if (feature.component != from) {
            continue;
        }
        const bool moves = features.contains(feature.id);
        const auto conflict = [&](ObjectId used, const std::string& usedName) {
            const bool usedMoves =
                bodies.contains(used) || features.contains(used) || sketches.contains(used);
            if (used.isValid() && usedMoves != moves) {
                throw std::invalid_argument(feature.name + " uses " + usedName +
                                            (moves ? ", which stays" : ", which moves") +
                                            ". Choose the bodies that " + (moves ? "it" : "they") +
                                            " belong to together.");
            }
        };
        if (const SketchItem* sketch = findSketch(feature.sketch)) {
            conflict(sketch->id, sketch->name);
        }
        if (const Body* limit = findBody(feature.extentBody)) {
            conflict(limit->id, limit->name);
        }
        if (const Body* body = findBody(feature.body); body != nullptr && !moves) {
            conflict(body->id, body->name);
        }
        for (const std::string& reference : {feature.reference, feature.reference2}) {
            if (const Feature* construction = findFeature(constructionNamed(reference))) {
                conflict(construction->id, construction->name);
            }
        }
    }
    for (const SketchItem& sketch : m_sketches) {
        if (sketch.component != from || !sketch.attachedTo.isValid()) {
            continue;
        }
        const bool moves = sketches.contains(sketch.id);
        const bool usedMoves =
            bodies.contains(sketch.attachedTo) || features.contains(sketch.attachedTo);
        if (moves != usedMoves) {
            throw std::invalid_argument(sketch.name + " is drawn on something that " +
                                        (moves ? "stays" : "moves") +
                                        ". Choose the bodies it belongs to together.");
        }
    }

    const ObjectId component{m_nextId++};
    inOneStep("Make Component", [&] {
        Edit add{};
        add.kind = Edit::Kind::AddComponent;
        add.id = component;
        add.index = m_components.size();
        add.component = Component{component, std::move(name), {}, {}};
        record(std::move(add), "Make Component");
        addOccurrence(component, from, kernel::Placement{});
        for (const ObjectId id : bodyIds) {
            Edit edit{};
            edit.kind = Edit::Kind::ChangeBody;
            edit.id = id;
            edit.body = *findBody(id);
            edit.newBody = edit.body;
            edit.newBody.component = component;
            record(std::move(edit), "Make Component");
        }
        for (const ObjectId id : sketches) {
            Edit edit{};
            edit.kind = Edit::Kind::ChangeSketchComponent;
            edit.id = id;
            edit.oldComponent = from;
            edit.newComponentId = component;
            record(std::move(edit), "Make Component");
        }
        for (const ObjectId id : features) {
            Edit edit{};
            edit.kind = Edit::Kind::ChangeFeature;
            edit.id = id;
            edit.feature = *findFeature(id);
            edit.newFeature = edit.feature;
            edit.newFeature.component = component;
            record(std::move(edit), "Make Component");
        }
    });
    return component;
}

ObjectId Document::copyComponent(ObjectId occurrenceId) {
    const Occurrence* found = findOccurrence(occurrenceId);
    if (found == nullptr) {
        throw std::invalid_argument("copyComponent: there is no occurrence with id " +
                                    std::to_string(occurrenceId.value));
    }
    const Occurrence original = *found;
    const Component source = *findComponent(original.component);
    // The copy's name: "Bracket copy", "Bracket copy 2".
    std::string name = source.name + " copy";
    for (int number = 2; std::any_of(m_components.begin(), m_components.end(),
                                     [&name](const Component& c) { return c.name == name; });
         ++number) {
        name = source.name + " copy " + std::to_string(number);
    }

    // A new id for everything copied, so that the copies refer to each other.
    std::unordered_map<ObjectId, ObjectId> ids;
    const auto fresh = [&](ObjectId old) {
        const ObjectId id{m_nextId++};
        ids[old] = id;
        return id;
    };
    const auto map = [&ids](ObjectId old) {
        const auto mapped = ids.find(old);
        return mapped != ids.end() ? mapped->second : old;
    };
    const ObjectId component = fresh(source.id);
    std::vector<ObjectId> items;
    for (const ObjectId item : m_timeline) {
        if (componentOf(item) == source.id) {
            items.push_back(item);
            fresh(item);
        }
    }
    for (const Body& body : m_bodies) {
        if (body.component == source.id && (body.madeBy.isValid() || !body.linkKey.empty())) {
            fresh(body.id);
        }
    }
    for (const ObjectId item : items) {
        for (const ObjectId piece : piecesOf(item)) {
            if (!ids.contains(piece)) {
                fresh(piece);
            }
        }
    }
    std::vector<ObjectId> inner = occurrencesIn(source.id);
    for (const ObjectId id : inner) {
        fresh(id);
    }
    std::vector<ObjectId> joints;
    for (const Joint& joint : m_joints) {
        if (joint.component == source.id) {
            joints.push_back(joint.id);
            fresh(joint.id);
        }
    }

    ObjectId copy;
    suspendRebuild();
    try {
        inOneStep("Copy " + source.name, [&] {
            Edit add{};
            add.kind = Edit::Kind::AddComponent;
            add.id = component;
            add.index = m_components.size();
            add.component = source;
            add.component.id = component;
            add.component.name = name;
            record(std::move(add), "Copy Component");
            // Linked bodies, which are not in the timeline.
            for (const Body& body : std::vector<Body>(m_bodies)) {
                if (body.component != source.id || body.linkKey.empty()) {
                    continue;
                }
                Edit edit{};
                edit.kind = Edit::Kind::AddBody;
                edit.id = map(body.id);
                edit.index = m_bodies.size();
                edit.body = body;
                edit.body.id = edit.id;
                edit.body.component = component;
                edit.oldMarker = edit.newMarker = m_marker;
                record(std::move(edit), "Copy Component");
            }
            // A solid of a feature that is not being copied comes along as it is.
            for (const Body& body : std::vector<Body>(m_bodies)) {
                if (body.component != source.id || body.piece == 0 || body.shape.isNull() ||
                    std::find(items.begin(), items.end(), body.madeBy) != items.end()) {
                    continue;
                }
                Edit edit{};
                edit.kind = Edit::Kind::AddBody;
                edit.id = map(body.id);
                edit.place = placeForNew();
                markersForAdding(edit);
                edit.index = m_bodies.size();
                edit.body = body;
                edit.body.id = edit.id;
                edit.body.component = component;
                edit.body.source = body.shape;
                edit.body.madeBy = {};
                edit.body.piece = 0;
                record(std::move(edit), "Copy Component");
            }
            for (const ObjectId item : items) {
                Edit edit{};
                edit.id = map(item);
                edit.place = placeForNew();
                markersForAdding(edit);
                if (const SketchItem* sketch = findSketch(item)) {
                    edit.kind = Edit::Kind::AddSketch;
                    edit.index = m_sketches.size();
                    edit.sketch = *sketch;
                    edit.sketch.id = edit.id;
                    edit.sketch.component = component;
                    edit.sketch.attachedTo = map(sketch->attachedTo);
                } else if (const Feature* feature = findFeature(item)) {
                    edit.kind = Edit::Kind::AddFeature;
                    edit.feature = *feature;
                    edit.feature.id = edit.id;
                    edit.feature.component = component;
                    edit.feature.tag = detail::tagOf(*feature);
                    edit.feature.body = map(feature->body);
                    edit.feature.sketch = map(feature->sketch);
                    edit.feature.extentBody = map(feature->extentBody);
                    edit.feature.reference = remapped(feature->reference, ids);
                    edit.feature.reference2 = remapped(feature->reference2, ids);
                    for (ObjectId& copied : edit.feature.sources) {
                        copied = map(copied);
                    }
                    if (const Body* made = findBody(feature->body);
                        made != nullptr && made->madeBy == feature->id) {
                        edit.hasBody = true;
                        edit.index = m_bodies.size();
                        edit.body = *made;
                        edit.body.id = map(made->id);
                        edit.body.madeBy = edit.id;
                        edit.body.component = component;
                        edit.body.shape = {};
                    }
                } else if (const Body* body = findBody(item)) {
                    edit.kind = Edit::Kind::AddBody;
                    edit.index = m_bodies.size();
                    edit.body = *body;
                    edit.body.id = edit.id;
                    edit.body.component = component;
                    edit.body.tag = detail::tagOf(*body);
                }
                record(std::move(edit), "Copy Component");
                // The other solids it leaves. One kept elsewhere has no place in the copy.
                for (const ObjectId piece : piecesOf(item)) {
                    Edit held{};
                    held.kind = Edit::Kind::AddBody;
                    held.id = map(piece);
                    held.index = m_bodies.size();
                    held.body = *findBody(piece);
                    held.body.id = held.id;
                    held.body.madeBy = map(item);
                    held.body.discarded = held.body.discarded || held.body.component != source.id;
                    held.body.component = component;
                    held.body.shape = {};
                    record(std::move(held), "Copy Component");
                }
            }
            for (const ObjectId id : inner) {
                Edit edit{};
                edit.kind = Edit::Kind::AddOccurrence;
                edit.id = map(id);
                edit.index = m_occurrences.size();
                edit.occurrence = *findOccurrence(id);
                edit.occurrence.id = edit.id;
                edit.occurrence.parent = component;
                record(std::move(edit), "Copy Component");
            }
            for (const ObjectId id : joints) {
                Edit edit{};
                edit.kind = Edit::Kind::AddJoint;
                edit.id = map(id);
                edit.index = m_joints.size();
                edit.joint = *findJoint(id);
                edit.joint.id = edit.id;
                edit.joint.component = component;
                for (JointSide* side : {&edit.joint.moving, &edit.joint.fixed}) {
                    if (!side->path.empty()) {
                        side->path.front() = map(side->path.front());
                    } else {
                        side->body = map(side->body);
                    }
                }
                record(std::move(edit), "Copy Component");
            }
            Edit placeCopy{};
            placeCopy.kind = Edit::Kind::AddOccurrence;
            placeCopy.id = ObjectId{m_nextId++};
            placeCopy.index = m_occurrences.size();
            placeCopy.occurrence =
                Occurrence{placeCopy.id, component, original.parent, placementOf(occurrenceId)};
            copy = placeCopy.id;
            record(std::move(placeCopy), "Copy Component");
        });
    } catch (...) {
        resumeRebuild();
        throw;
    }
    resumeRebuild();
    return copy;
}

ObjectId Document::addLinkedComponent(std::string name, std::string link, LinkedContent content,
                                      ObjectId parent, const kernel::Placement& placement) {
    requireName(name, "addLinkedComponent");
    if (isBlank(link)) {
        throw std::invalid_argument("addLinkedComponent: a link is needed");
    }
    requireOwnComponent(parent, "addLinkedComponent");
    const ObjectId component{m_nextId++};
    const std::string label = "Insert " + name;
    inOneStep(label, [&] {
        Edit add{};
        add.kind = Edit::Kind::AddComponent;
        add.id = component;
        add.index = m_components.size();
        add.component = Component{component, std::move(name), std::move(link), content.stamp};
        record(std::move(add), label);
        for (LinkedBody& linked : content.bodies) {
            Edit edit{};
            edit.kind = Edit::Kind::AddBody;
            edit.id = ObjectId{m_nextId++};
            edit.index = m_bodies.size();
            edit.body = Body{edit.id, linked.name.empty() ? std::string("Body") : linked.name, true,
                             linked.shape};
            edit.body.source = linked.shape;
            edit.body.component = component;
            edit.body.material = linked.material;
            edit.body.linkKey = linked.key;
            edit.body.sourceFaceNames = std::move(linked.faceNames);
            edit.oldMarker = edit.newMarker = m_marker;
            record(std::move(edit), label);
        }
        addOccurrence(component, parent, placement);
    });
    return component;
}

void Document::updateLinkedComponent(ObjectId id, LinkedContent content) {
    const Component* component = findComponent(id);
    if (component == nullptr || component->link.empty()) {
        throw std::invalid_argument("updateLinkedComponent: there is no linked component with "
                                    "id " +
                                    std::to_string(id.value));
    }
    const std::string label = "Update " + component->name;
    inOneStep(label, [&] {
        std::unordered_set<std::string> keys;
        for (LinkedBody& linked : content.bodies) {
            keys.insert(linked.key);
            const auto same = std::find_if(m_bodies.begin(), m_bodies.end(), [&](const Body& b) {
                return b.component == id && b.linkKey == linked.key;
            });
            if (same == m_bodies.end()) {
                Edit edit{};
                edit.kind = Edit::Kind::AddBody;
                edit.id = ObjectId{m_nextId++};
                edit.index = m_bodies.size();
                edit.body = Body{edit.id, linked.name.empty() ? std::string("Body") : linked.name,
                                 true, linked.shape};
                edit.body.source = linked.shape;
                edit.body.component = id;
                edit.body.material = linked.material;
                edit.body.linkKey = linked.key;
                edit.body.sourceFaceNames = linked.faceNames;
                edit.oldMarker = edit.newMarker = m_marker;
                record(std::move(edit), label);
                continue;
            }
            Edit edit{};
            edit.kind = Edit::Kind::ChangeBody;
            edit.id = same->id;
            edit.body = *same;
            edit.newBody = *same;
            edit.newBody.name = linked.name.empty() ? same->name : linked.name;
            edit.newBody.source = linked.shape;
            edit.newBody.sourceFaceNames = linked.faceNames;
            if (!linked.material.empty()) {
                edit.newBody.material = linked.material;
            }
            record(std::move(edit), label);
        }
        std::vector<ObjectId> gone;
        for (const Body& body : m_bodies) {
            if (body.component == id && !keys.contains(body.linkKey)) {
                gone.push_back(body.id);
            }
        }
        for (const ObjectId body : gone) {
            removeBody(body);
        }
        Edit stamp{};
        stamp.kind = Edit::Kind::ChangeComponent;
        stamp.id = id;
        stamp.component = *findComponent(id);
        stamp.newComponent = stamp.component;
        stamp.newComponent.linkStamp = content.stamp;
        record(std::move(stamp), label);
    });
}

void Document::setComponentLink(ObjectId id, std::string link) {
    const Component* component = findComponent(id);
    if (component == nullptr || component->link.empty()) {
        throw std::invalid_argument("setComponentLink: there is no linked component with id " +
                                    std::to_string(id.value));
    }
    if (isBlank(link)) {
        throw std::invalid_argument("setComponentLink: a link is needed");
    }
    if (component->link == link) {
        return;
    }
    Edit edit{};
    edit.kind = Edit::Kind::ChangeComponent;
    edit.id = id;
    edit.component = *component;
    edit.newComponent = *component;
    edit.newComponent.link = std::move(link);
    record(std::move(edit), "Relink " + component->name);
}

ObjectId Document::addJoint(Joint joint) {
    if (const std::string problem = jointProblem(joint); !problem.empty()) {
        throw std::invalid_argument("addJoint: " + problem);
    }
    joint.id = ObjectId{m_nextId++};
    if (isBlank(joint.name)) {
        joint.name = uniqueJointName(toString(joint.kind));
    }
    joint.values = limited(joint, joint.values);
    Edit edit{};
    edit.kind = Edit::Kind::AddJoint;
    edit.id = joint.id;
    edit.index = m_joints.size();
    edit.joint = std::move(joint);
    const ObjectId id = edit.id;
    record(std::move(edit), "Joint");
    return id;
}

void Document::changeJoint(ObjectId id, Joint definition) {
    const Joint* joint = findJoint(id);
    if (joint == nullptr) {
        throw std::invalid_argument("changeJoint: there is no joint with id " +
                                    std::to_string(id.value));
    }
    definition.id = id;
    if (isBlank(definition.name)) {
        definition.name = joint->name;
    }
    if (const std::string problem = jointProblem(definition); !problem.empty()) {
        throw std::invalid_argument("changeJoint: " + problem);
    }
    definition.values = limited(definition, definition.values);
    if (definition == *joint) {
        return;
    }
    Edit edit{};
    edit.kind = Edit::Kind::ChangeJoint;
    edit.id = id;
    edit.joint = *joint;
    edit.newJoint = std::move(definition);
    const std::string label = edit.joint.name == edit.newJoint.name ? "Edit " + edit.joint.name
                                                                    : std::string("Rename Joint");
    record(std::move(edit), label);
}

void Document::removeJoint(ObjectId id) {
    const std::size_t index = jointIndexOf(id);
    if (index == notFound) {
        throw std::invalid_argument("removeJoint: there is no joint with id " +
                                    std::to_string(id.value));
    }
    const Joint joint = m_joints[index];
    // What it holds stays where it is, rather than going back to where it was put by hand.
    std::vector<std::pair<ObjectId, kernel::Placement>> held;
    for (const JointSide* side : {&joint.moving, &joint.fixed}) {
        if (!side->path.empty() && findOccurrence(side->path.front()) != nullptr) {
            held.emplace_back(side->path.front(), placementOf(side->path.front()));
        }
    }
    inOneStep("Delete " + joint.name, [&] {
        Edit edit{};
        edit.kind = Edit::Kind::RemoveJoint;
        edit.id = id;
        edit.index = index;
        edit.joint = joint;
        record(std::move(edit), "Delete " + joint.name);
        for (const auto& [occurrence, now] : held) {
            if (findOccurrence(occurrence) == nullptr) {
                continue;
            }
            Edit keep{};
            keep.kind = Edit::Kind::ChangeOccurrence;
            keep.id = occurrence;
            keep.occurrence = *findOccurrence(occurrence);
            keep.newOccurrence = keep.occurrence;
            keep.newOccurrence.placement = now;
            if (!(keep.occurrence == keep.newOccurrence)) {
                record(std::move(keep), "Delete " + joint.name);
            }
        }
    });
}

void Document::setBodyMaterial(ObjectId id, std::string material) {
    const Body* body = findBody(id);
    if (body == nullptr) {
        throw std::invalid_argument("setBodyMaterial: there is no body with id " +
                                    std::to_string(id.value));
    }
    if (!material.empty() && !findMaterial(material).has_value()) {
        throw std::invalid_argument("setBodyMaterial: there is no material called \"" + material +
                                    "\"");
    }
    if (body->material == material) {
        return;
    }
    Edit edit{};
    edit.kind = Edit::Kind::ChangeBody;
    edit.id = id;
    edit.body = *body;
    edit.newBody = *body;
    edit.newBody.material = std::move(material);
    record(std::move(edit), "Set Material");
}

void Document::addMaterial(Material material) {
    if (isBlank(material.name) || materialIndexOf(material.name) != notFound) {
        throw std::invalid_argument("addMaterial: a material needs a name no other of the "
                                    "document's has");
    }
    if (!std::isfinite(material.density) || material.density <= 0.0) {
        throw std::invalid_argument("addMaterial: the density has to be more than zero");
    }
    Edit edit{};
    edit.kind = Edit::Kind::AddMaterial;
    edit.index = m_materials.size();
    edit.material = std::move(material);
    record(std::move(edit), "Add Material");
}

void Document::changeMaterial(const std::string& name, Material material) {
    const std::size_t index = materialIndexOf(name);
    if (index == notFound) {
        throw std::invalid_argument("changeMaterial: the document has no material called \"" +
                                    name + "\"");
    }
    if (isBlank(material.name) ||
        (material.name != name && materialIndexOf(material.name) != notFound)) {
        throw std::invalid_argument("changeMaterial: a material needs a name no other of the "
                                    "document's has");
    }
    if (!std::isfinite(material.density) || material.density <= 0.0) {
        throw std::invalid_argument("changeMaterial: the density has to be more than zero");
    }
    if (m_materials[index] == material) {
        return;
    }
    inOneStep("Change Material", [&] {
        Edit edit{};
        edit.kind = Edit::Kind::ChangeMaterial;
        edit.index = index;
        edit.material = m_materials[index];
        edit.newMaterial = material;
        record(std::move(edit), "Change Material");
        if (material.name == name) {
            return;
        }
        for (const Body& body : std::vector<Body>(m_bodies)) {
            if (body.material == name) {
                Edit rename{};
                rename.kind = Edit::Kind::ChangeBody;
                rename.id = body.id;
                rename.body = body;
                rename.newBody = body;
                rename.newBody.material = material.name;
                record(std::move(rename), "Change Material");
            }
        }
    });
}

void Document::removeMaterial(const std::string& name) {
    const std::size_t index = materialIndexOf(name);
    if (index == notFound) {
        throw std::invalid_argument("removeMaterial: the document has no material called \"" +
                                    name + "\"");
    }
    inOneStep("Delete Material", [&] {
        // A body of a material LCAD knows by the same name keeps it.
        const bool known = std::any_of(materialLibrary().begin(), materialLibrary().end(),
                                       [&name](const Material& m) { return m.name == name; });
        if (!known) {
            for (const Body& body : std::vector<Body>(m_bodies)) {
                if (body.material == name) {
                    Edit clear{};
                    clear.kind = Edit::Kind::ChangeBody;
                    clear.id = body.id;
                    clear.body = body;
                    clear.newBody = body;
                    clear.newBody.material.clear();
                    record(std::move(clear), "Delete Material");
                }
            }
        }
        Edit edit{};
        edit.kind = Edit::Kind::RemoveMaterial;
        edit.index = materialIndexOf(name);
        edit.material = m_materials[edit.index];
        record(std::move(edit), "Delete Material");
    });
}

// ---------------------------------------------------------------------------------------------
// Applying and undoing

void Document::applyAssembly(const Edit& edit, bool forward) {
    const auto at = [](auto& list, std::size_t index) {
        return list.begin() + static_cast<std::ptrdiff_t>(std::min(index, list.size()));
    };
    // Adding forward is removing backward, and the other way round.
    const auto addOrRemove = [&](auto& list, const auto& item, bool add) {
        if (add) {
            list.insert(at(list, edit.index), item);
        } else {
            list.erase(at(list, edit.index));
        }
    };
    switch (edit.kind) {
    case Edit::Kind::AddComponent:
    case Edit::Kind::RemoveComponent:
        addOrRemove(m_components, edit.component,
                    (edit.kind == Edit::Kind::AddComponent) == forward);
        m_resolve = true;
        m_assemblyChanged = true;
        break;
    case Edit::Kind::ChangeComponent:
        m_components[componentIndexOf(edit.id)] = forward ? edit.newComponent : edit.component;
        m_assemblyChanged = true;
        break;
    case Edit::Kind::AddOccurrence:
    case Edit::Kind::RemoveOccurrence:
        addOrRemove(m_occurrences, edit.occurrence,
                    (edit.kind == Edit::Kind::AddOccurrence) == forward);
        m_resolve = true;
        m_assemblyChanged = true;
        break;
    case Edit::Kind::ChangeOccurrence:
        m_occurrences[occurrenceIndexOf(edit.id)] = forward ? edit.newOccurrence : edit.occurrence;
        m_resolve = true;
        m_assemblyChanged = true;
        break;
    case Edit::Kind::AddJoint:
    case Edit::Kind::RemoveJoint:
        addOrRemove(m_joints, edit.joint, (edit.kind == Edit::Kind::AddJoint) == forward);
        m_resolve = true;
        m_assemblyChanged = true;
        break;
    case Edit::Kind::ChangeJoint:
        m_joints[jointIndexOf(edit.id)] = forward ? edit.newJoint : edit.joint;
        m_resolve = true;
        m_assemblyChanged = true;
        break;
    case Edit::Kind::ChangeBody: {
        const Body& to = forward ? edit.newBody : edit.body;
        Body& body = m_bodies[indexOf(edit.id)];
        const bool renamed = body.name != to.name;
        const bool shown = body.visible != to.visible;
        const bool material = body.material != to.material;
        const bool moved = body.component != to.component;
        const bool reshaped = body.source.impl() != to.source.impl() ||
                              body.sourceFaceNames != to.sourceFaceNames || body.tag != to.tag ||
                              body.discarded != to.discarded;
        body.name = to.name;
        body.visible = to.visible;
        body.component = to.component;
        body.material = to.material;
        body.tag = to.tag;
        body.linkKey = to.linkKey;
        body.source = to.source;
        body.sourceFaceNames = to.sourceFaceNames;
        body.discarded = to.discarded;
        if (reshaped) {
            touch(edit.id);
        }
        if (renamed) {
            notify(Change::Kind::BodyRenamed, edit.id);
        }
        if (shown) {
            notify(Change::Kind::BodyVisibilityChanged, edit.id);
        }
        if (material) {
            notify(Change::Kind::MaterialsChanged, edit.id);
        }
        if (moved) {
            m_resolve = true;
            m_assemblyChanged = true;
        }
        break;
    }
    case Edit::Kind::ChangeSketchComponent:
        m_sketches[sketchIndexOf(edit.id)].component =
            forward ? edit.newComponentId : edit.oldComponent;
        m_assemblyChanged = true;
        notify(Change::Kind::SketchGeometryChanged, edit.id);
        break;
    case Edit::Kind::AddMaterial:
    case Edit::Kind::RemoveMaterial:
        addOrRemove(m_materials, edit.material, (edit.kind == Edit::Kind::AddMaterial) == forward);
        notify(Change::Kind::MaterialsChanged);
        break;
    case Edit::Kind::ChangeMaterial:
        m_materials[std::min(edit.index, m_materials.size() - 1)] =
            forward ? edit.newMaterial : edit.material;
        notify(Change::Kind::MaterialsChanged);
        break;
    default:
        break;
    }
}

void Document::solveAssembly() {
    m_resolve = false;
    Solution solution = solve(*this, m_joints);
    std::vector<Instance> instances = instancesOf(*this, solution.placements);
    const bool errorsChanged = solution.errors != m_jointErrors;
    bool moved = instances.size() != m_instances.size();
    for (std::size_t i = 0; !moved && i < instances.size(); ++i) {
        moved = instances[i].path != m_instances[i].path ||
                instances[i].visible != m_instances[i].visible ||
                !kernel::samePlacement(instances[i].placement, m_instances[i].placement, 0.0);
    }
    m_solved = std::move(solution.placements);
    m_jointErrors = std::move(solution.errors);
    m_instances = std::move(instances);
    const bool structure = m_assemblyChanged || errorsChanged;
    m_assemblyChanged = false;
    if (structure) {
        notify(Change::Kind::AssemblyChanged);
    }
    if (moved) {
        notify(Change::Kind::PlacementsChanged);
    }
}

} // namespace lcad::document
