// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/document/Document.h>

#include "Rebuild.h"

#include <lcad/core/Expression.h>
#include <lcad/kernel/Measure.h>
#include <lcad/sketch/Edit.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace lcad::document {

namespace {

constexpr std::size_t notFound = static_cast<std::size_t>(-1);

constexpr std::size_t atTheEnd = static_cast<std::size_t>(-1);

bool isBlank(const std::string& name) {
    return name.find_first_not_of(" \t\r\n") == std::string::npos;
}

void requireName(const std::string& name, const char* function) {
    if (isBlank(name)) {
        throw std::invalid_argument(std::string(function) + ": a name is needed");
    }
}

// Whether a feature makes a body of its own, as against changing one that is there.
bool makesBody(const Feature& feature) {
    return (feature.kind == FeatureKind::Pull || feature.kind == FeatureKind::Revolve) &&
           feature.operation == Operation::NewBody;
}

bool usesSketch(const Feature& feature) {
    return feature.kind == FeatureKind::Pull || feature.kind == FeatureKind::Revolve ||
           feature.kind == FeatureKind::Hole;
}

// A construction plane placed against an origin plane needs no body.
bool needsBody(const Feature& feature) {
    return !((feature.kind == FeatureKind::Plane || feature.kind == FeatureKind::Axis) &&
             !feature.body.isValid());
}

// A construction plane or axis a reference names ("plane:12", "axis:14"), or an invalid id.
ObjectId constructionIn(const std::string& reference) {
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

bool samePlace(const kernel::Plane& a, const kernel::Plane& b) {
    const auto near = [](double x, double y) { return std::abs(x - y) < 1e-12; };
    return near(a.origin.x, b.origin.x) && near(a.origin.y, b.origin.y) &&
           near(a.origin.z, b.origin.z) && near(a.normal.x, b.normal.x) &&
           near(a.normal.y, b.normal.y) && near(a.normal.z, b.normal.z) &&
           near(a.xDirection.x, b.xDirection.x) && near(a.xDirection.y, b.xDirection.y) &&
           near(a.xDirection.z, b.xDirection.z);
}

} // namespace

Document::Document() = default;

Document::Document(DocumentState state)
    : m_bodies(std::move(state.bodies)), m_sketches(std::move(state.sketches)),
      m_features(std::move(state.features)), m_parameters(std::move(state.parameters)),
      m_timeline(std::move(state.timeline)), m_marker(state.marker), m_nextId(state.nextId) {
    std::unordered_set<ObjectId> seen;
    const auto claim = [this, &seen](ObjectId id, const char* what) {
        if (!id.isValid() || id.value >= m_nextId) {
            throw std::invalid_argument(std::string("Document: ") + what + " id " +
                                        std::to_string(id.value) +
                                        " is outside the ids the document has handed out");
        }
        if (!seen.insert(id).second) {
            throw std::invalid_argument("Document: two objects share the id " +
                                        std::to_string(id.value));
        }
    };
    for (const SketchItem& sketch : m_sketches) {
        claim(sketch.id, "sketch");
        requireName(sketch.name, "Document");
    }
    for (const Feature& feature : m_features) {
        claim(feature.id, "feature");
        requireName(feature.name, "Document");
    }
    std::unordered_set<std::string> parameterNames;
    for (const Parameter& parameter : m_parameters) {
        claim(parameter.id, "parameter");
        if (!core::isValidName(parameter.name) || !parameterNames.insert(parameter.name).second) {
            throw std::invalid_argument("Document: \"" + parameter.name +
                                        "\" cannot be the name of a parameter, or is taken twice");
        }
    }
    for (Body& body : m_bodies) {
        claim(body.id, "body");
        requireName(body.name, "Document");
        if (body.madeBy.isValid()) {
            const Feature* maker = findFeature(body.madeBy);
            if (maker == nullptr || !makesBody(*maker) || maker->body != body.id) {
                throw std::invalid_argument("Document: body \"" + body.name +
                                            "\" names a feature that does not make it");
            }
            body.shape = {};
            body.source = {};
            continue;
        }
        if (body.shape.isNull()) {
            throw std::invalid_argument("Document: body \"" + body.name + "\" has no geometry");
        }
        body.source = body.shape;
    }
    for (const SketchItem& sketch : m_sketches) {
        if (!sketch.attachedTo.isValid()) {
            continue;
        }
        const Feature* plane = findFeature(sketch.attachedTo);
        const bool onPlane =
            plane != nullptr && plane->kind == FeatureKind::Plane && sketch.attachedFace.empty();
        const bool onFace = findBody(sketch.attachedTo) != nullptr && !sketch.attachedFace.empty();
        if (!onPlane && !onFace) {
            throw std::invalid_argument("Document: sketch \"" + sketch.name +
                                        "\" is attached to something the document does not have");
        }
    }
    for (const Feature& feature : m_features) {
        if (!needsBody(feature)) {
            continue;
        }
        const Body* body = findBody(feature.body);
        if (body == nullptr || (makesBody(feature) && body->madeBy != feature.id)) {
            throw std::invalid_argument("Document: feature \"" + feature.name +
                                        "\" names a body the document does not have");
        }
    }

    // The timeline holds every body no feature makes, every sketch, and every feature, once.
    std::vector<ObjectId> everything;
    for (const Body& body : m_bodies) {
        if (!body.madeBy.isValid()) {
            everything.push_back(body.id);
        }
    }
    for (const SketchItem& sketch : m_sketches) {
        everything.push_back(sketch.id);
    }
    for (const Feature& feature : m_features) {
        everything.push_back(feature.id);
    }
    if (m_timeline.empty()) {
        m_timeline = everything;
    } else {
        std::vector<ObjectId> given = m_timeline;
        std::sort(given.begin(), given.end());
        std::sort(everything.begin(), everything.end());
        if (given != everything) {
            throw std::invalid_argument(
                "Document: the timeline does not list every body, sketch, and feature once");
        }
    }
    if (const std::string problem = orderProblem(m_timeline); !problem.empty()) {
        throw std::invalid_argument("Document: " + problem);
    }
    m_rebuildAll = true;
    rebuild();
}

Document::~Document() = default;

const Body* Document::findBody(ObjectId id) const {
    const std::size_t index = indexOf(id);
    return index == notFound ? nullptr : &m_bodies[index];
}

std::string Document::uniqueBodyName(std::string_view base) const {
    const auto taken = [this](const std::string& name) {
        return std::any_of(m_bodies.begin(), m_bodies.end(),
                           [&name](const Body& body) { return body.name == name; });
    };
    std::string candidate(base);
    for (int number = 2; taken(candidate); ++number) {
        candidate = std::string(base) + " " + std::to_string(number);
    }
    return candidate;
}

ObjectId Document::addBody(std::string name, kernel::Shape shape) {
    requireName(name, "addBody");
    if (!kernel::isValid(shape)) {
        throw std::invalid_argument("addBody: the shape for \"" + name +
                                    "\" is empty or not valid geometry");
    }
    Edit edit{};
    edit.kind = Edit::Kind::AddBody;
    edit.id = ObjectId{m_nextId};
    edit.index = m_bodies.size();
    edit.body = Body{edit.id, std::move(name), true, shape};
    edit.body.source = std::move(shape);
    edit.place = placeForNew();
    markersForAdding(edit);
    const ObjectId id = edit.id;
    ++m_nextId;
    record(std::move(edit), "Add Body");
    return id;
}

void Document::removeBody(ObjectId id) {
    const std::size_t index = indexOf(id);
    if (index == notFound) {
        throw std::invalid_argument("removeBody: there is no body with id " +
                                    std::to_string(id.value));
    }
    // What features make of a body goes with the features. Take those away first, the last
    // one applied first, as one step.
    std::vector<ObjectId> acting;
    for (const ObjectId item : m_timeline) {
        const Feature* feature = findFeature(item);
        if (feature != nullptr && feature->body == id) {
            acting.push_back(item);
        }
    }
    const bool madeByFeature = m_bodies[index].madeBy.isValid();
    const bool ownStep = !m_open && (!acting.empty() || madeByFeature);
    if (ownStep) {
        beginTransaction("Delete Body");
    }
    try {
        for (auto feature = acting.rbegin(); feature != acting.rend(); ++feature) {
            removeFeature(*feature);
        }
        if (!madeByFeature) {
            Edit edit{};
            edit.kind = Edit::Kind::RemoveBody;
            edit.id = id;
            edit.body = m_bodies[indexOf(id)];
            edit.index = indexOf(id);
            edit.place = placeOf(id);
            markersForRemoving(edit, edit.place);
            record(std::move(edit), "Delete Body");
        }
    } catch (...) {
        if (ownStep) {
            rollbackTransaction();
        }
        throw;
    }
    if (ownStep) {
        commitTransaction();
    }
}

void Document::renameBody(ObjectId id, std::string name) {
    const std::size_t index = indexOf(id);
    if (index == notFound) {
        throw std::invalid_argument("renameBody: there is no body with id " +
                                    std::to_string(id.value));
    }
    requireName(name, "renameBody");
    if (m_bodies[index].name == name) {
        return;
    }
    Edit edit{};
    edit.kind = Edit::Kind::RenameBody;
    edit.id = id;
    edit.index = index;
    edit.oldName = m_bodies[index].name;
    edit.newName = std::move(name);
    record(std::move(edit), "Rename Body");
}

void Document::setBodyVisible(ObjectId id, bool visible) {
    const std::size_t index = indexOf(id);
    if (index == notFound) {
        throw std::invalid_argument("setBodyVisible: there is no body with id " +
                                    std::to_string(id.value));
    }
    if (m_bodies[index].visible == visible) {
        return;
    }
    Edit edit{};
    edit.kind = Edit::Kind::SetBodyVisible;
    edit.id = id;
    edit.index = index;
    edit.oldVisible = m_bodies[index].visible;
    edit.newVisible = visible;
    record(std::move(edit), visible ? "Show Body" : "Hide Body");
}

const SketchItem* Document::findSketch(ObjectId id) const {
    const std::size_t index = sketchIndexOf(id);
    return index == notFound ? nullptr : &m_sketches[index];
}

std::string Document::uniqueSketchName(std::string_view base) const {
    for (int number = 1;; ++number) {
        const std::string candidate = std::string(base) + " " + std::to_string(number);
        const bool taken = std::any_of(
            m_sketches.begin(), m_sketches.end(),
            [&candidate](const SketchItem& sketch) { return sketch.name == candidate; });
        if (!taken) {
            return candidate;
        }
    }
}

ObjectId Document::addSketch(std::string name, const kernel::Plane& plane, ObjectId attachedTo,
                             std::string attachedFace) {
    requireName(name, "addSketch");
    if (attachedTo.isValid()) {
        const Feature* onPlane = findFeature(attachedTo);
        const bool toPlane =
            onPlane != nullptr && onPlane->kind == FeatureKind::Plane && attachedFace.empty();
        const bool toFace = findBody(attachedTo) != nullptr && !attachedFace.empty();
        if (!toPlane && !toFace) {
            throw std::invalid_argument("addSketch: it is attached to something the document "
                                        "does not have");
        }
    }
    Edit edit{};
    edit.kind = Edit::Kind::AddSketch;
    edit.id = ObjectId{m_nextId};
    edit.index = m_sketches.size();
    edit.sketch = SketchItem{edit.id,    std::move(name),         true, plane, sketch::Sketch(),
                             attachedTo, std::move(attachedFace), {}};
    edit.place = placeForNew();
    markersForAdding(edit);
    const ObjectId id = edit.id;
    ++m_nextId;
    record(std::move(edit), "Create Sketch");
    return id;
}

void Document::removeSketch(ObjectId id) {
    const std::size_t index = sketchIndexOf(id);
    if (index == notFound) {
        throw std::invalid_argument("removeSketch: there is no sketch with id " +
                                    std::to_string(id.value));
    }
    Edit edit{};
    edit.kind = Edit::Kind::RemoveSketch;
    edit.id = id;
    edit.index = index;
    edit.sketch = m_sketches[index];
    edit.place = placeOf(id);
    markersForRemoving(edit, edit.place);
    record(std::move(edit), "Delete Sketch");
}

void Document::renameSketch(ObjectId id, std::string name) {
    const std::size_t index = sketchIndexOf(id);
    if (index == notFound) {
        throw std::invalid_argument("renameSketch: there is no sketch with id " +
                                    std::to_string(id.value));
    }
    requireName(name, "renameSketch");
    if (m_sketches[index].name == name) {
        return;
    }
    Edit edit{};
    edit.kind = Edit::Kind::RenameSketch;
    edit.id = id;
    edit.oldName = m_sketches[index].name;
    edit.newName = std::move(name);
    record(std::move(edit), "Rename Sketch");
}

void Document::setSketchVisible(ObjectId id, bool visible) {
    const std::size_t index = sketchIndexOf(id);
    if (index == notFound) {
        throw std::invalid_argument("setSketchVisible: there is no sketch with id " +
                                    std::to_string(id.value));
    }
    if (m_sketches[index].visible == visible) {
        return;
    }
    Edit edit{};
    edit.kind = Edit::Kind::SetSketchVisible;
    edit.id = id;
    edit.oldVisible = m_sketches[index].visible;
    edit.newVisible = visible;
    record(std::move(edit), visible ? "Show Sketch" : "Hide Sketch");
}

void Document::setSketchGeometry(ObjectId id, sketch::Sketch geometry, std::string label) {
    const std::size_t index = sketchIndexOf(id);
    if (index == notFound) {
        throw std::invalid_argument("setSketchGeometry: there is no sketch with id " +
                                    std::to_string(id.value));
    }
    Edit edit{};
    edit.kind = Edit::Kind::SetSketchGeometry;
    edit.id = id;
    edit.sketch.geometry = m_sketches[index].geometry;
    edit.newGeometry = std::move(geometry);
    record(std::move(edit), label.empty() ? std::string("Edit Sketch") : label);
}

// ---------------------------------------------------------------------------------------------
// Features and the timeline

const Feature* Document::findFeature(ObjectId id) const {
    const std::size_t index = featureIndexOf(id);
    return index == notFound ? nullptr : &m_features[index];
}

std::string Document::uniqueFeatureName(std::string_view base) const {
    for (int number = 1;; ++number) {
        const std::string candidate = std::string(base) + " " + std::to_string(number);
        const bool taken =
            std::any_of(m_features.begin(), m_features.end(),
                        [&candidate](const Feature& feature) { return feature.name == candidate; });
        if (!taken) {
            return candidate;
        }
    }
}

ObjectId Document::addFeature(Feature feature) {
    if (usesSketch(feature) && findSketch(feature.sketch) == nullptr) {
        throw std::invalid_argument("addFeature: there is no sketch with id " +
                                    std::to_string(feature.sketch.value));
    }
    const bool makes = makesBody(feature);
    if (!makes && needsBody(feature) && findBody(feature.body) == nullptr) {
        throw std::invalid_argument("addFeature: there is no body with id " +
                                    std::to_string(feature.body.value));
    }
    Edit edit{};
    edit.kind = Edit::Kind::AddFeature;
    feature.id = ObjectId{m_nextId++};
    if (isBlank(feature.name)) {
        feature.name = uniqueFeatureName(toString(feature.kind));
    }
    for (std::vector<sketch::EntityId>& outline : feature.regions) {
        std::sort(outline.begin(), outline.end());
    }
    if (makes) {
        feature.body = ObjectId{m_nextId++};
        edit.hasBody = true;
        edit.body = Body{feature.body, uniqueBodyName("Body"), true, {}};
        edit.body.madeBy = feature.id;
        edit.index = m_bodies.size();
    }
    edit.id = feature.id;
    edit.place = placeForNew();
    markersForAdding(edit);
    const std::string label = toString(feature.kind);
    edit.feature = std::move(feature);
    const ObjectId id = edit.id;
    record(std::move(edit), label);
    return id;
}

void Document::changeFeature(ObjectId id, Feature definition) {
    const std::size_t index = featureIndexOf(id);
    if (index == notFound) {
        throw std::invalid_argument("changeFeature: there is no feature with id " +
                                    std::to_string(id.value));
    }
    const Feature& old = m_features[index];
    definition.id = old.id;
    if (isBlank(definition.name)) {
        definition.name = old.name;
    }
    if (usesSketch(definition) && findSketch(definition.sketch) == nullptr) {
        throw std::invalid_argument("changeFeature: there is no sketch with id " +
                                    std::to_string(definition.sketch.value));
    }
    for (std::vector<sketch::EntityId>& outline : definition.regions) {
        std::sort(outline.begin(), outline.end());
    }
    Edit edit{};
    edit.kind = Edit::Kind::ChangeFeature;
    edit.id = id;
    const bool made = makesBody(old);
    const bool makes = makesBody(definition);
    if (made && makes) {
        definition.body = old.body;
    } else if (made) {
        // It joins or cuts another body now, and the body it made goes.
        if (findBody(definition.body) == nullptr || definition.body == old.body) {
            throw std::invalid_argument("changeFeature: a body to work on is needed");
        }
        edit.bodyChange = -1;
        edit.index = indexOf(old.body);
        edit.body = m_bodies[edit.index];
    } else if (makes) {
        definition.body = ObjectId{m_nextId++};
        edit.bodyChange = 1;
        edit.index = m_bodies.size();
        edit.body = Body{definition.body, uniqueBodyName("Body"), true, {}};
        edit.body.madeBy = id;
    } else if (needsBody(definition) && findBody(definition.body) == nullptr) {
        throw std::invalid_argument("changeFeature: there is no body with id " +
                                    std::to_string(definition.body.value));
    }
    if (definition == old) {
        return;
    }
    edit.feature = old;
    edit.newFeature = std::move(definition);
    const std::string label = "Edit " + edit.feature.name;
    record(std::move(edit), label);
}

void Document::removeFeature(ObjectId id) {
    const std::size_t index = featureIndexOf(id);
    if (index == notFound) {
        throw std::invalid_argument("removeFeature: there is no feature with id " +
                                    std::to_string(id.value));
    }
    Edit edit{};
    edit.kind = Edit::Kind::RemoveFeature;
    edit.id = id;
    edit.feature = m_features[index];
    if (makesBody(edit.feature)) {
        edit.hasBody = true;
        edit.index = indexOf(edit.feature.body);
        edit.body = m_bodies[edit.index];
    }
    edit.place = placeOf(id);
    markersForRemoving(edit, edit.place);
    const std::string label = "Delete " + edit.feature.name;
    record(std::move(edit), label);
}

void Document::renameFeature(ObjectId id, std::string name) {
    const Feature* feature = findFeature(id);
    if (feature == nullptr) {
        throw std::invalid_argument("renameFeature: there is no feature with id " +
                                    std::to_string(id.value));
    }
    requireName(name, "renameFeature");
    if (feature->name == name) {
        return;
    }
    Edit edit{};
    edit.kind = Edit::Kind::ChangeFeature;
    edit.id = id;
    edit.feature = *feature;
    edit.newFeature = *feature;
    edit.newFeature.name = std::move(name);
    record(std::move(edit), "Rename Feature");
}

void Document::setFeatureSuppressed(ObjectId id, bool suppressed) {
    const Feature* feature = findFeature(id);
    if (feature == nullptr) {
        throw std::invalid_argument("setFeatureSuppressed: there is no feature with id " +
                                    std::to_string(id.value));
    }
    if (feature->suppressed == suppressed) {
        return;
    }
    Edit edit{};
    edit.kind = Edit::Kind::ChangeFeature;
    edit.id = id;
    edit.feature = *feature;
    edit.newFeature = *feature;
    edit.newFeature.suppressed = suppressed;
    const std::string label = (suppressed ? "Suppress " : "Restore ") + feature->name;
    record(std::move(edit), label);
}

std::string Document::featureError(ObjectId id) const {
    const auto found = m_built.find(id);
    return found == m_built.end() ? std::string() : found->second.error;
}

ItemKind Document::kindOf(ObjectId item) const {
    if (featureIndexOf(item) != notFound) {
        return ItemKind::Feature;
    }
    if (sketchIndexOf(item) != notFound) {
        return ItemKind::Sketch;
    }
    if (placeOf(item) != notFound) {
        return ItemKind::Body;
    }
    throw std::invalid_argument("kindOf: " + std::to_string(item.value) +
                                " is not in the timeline");
}

std::size_t Document::historyMarker() const {
    return std::min(m_marker, m_timeline.size());
}

void Document::setHistoryMarker(std::size_t applied) {
    const std::size_t wanted = applied >= m_timeline.size() ? atTheEnd : applied;
    if (wanted == m_marker || (wanted == atTheEnd && m_marker >= m_timeline.size())) {
        return;
    }
    Edit edit{};
    edit.kind = Edit::Kind::SetMarker;
    edit.oldMarker = m_marker;
    edit.newMarker = wanted;
    record(std::move(edit), "Move History Marker");
}

void Document::moveInTimeline(ObjectId item, std::size_t index) {
    const std::size_t place = placeOf(item);
    if (place == notFound) {
        throw std::invalid_argument("moveInTimeline: " + std::to_string(item.value) +
                                    " is not in the timeline");
    }
    index = std::min(index, m_timeline.size() - 1);
    if (index == place) {
        return;
    }
    std::vector<ObjectId> order = m_timeline;
    order.erase(order.begin() + static_cast<std::ptrdiff_t>(place));
    order.insert(order.begin() + static_cast<std::ptrdiff_t>(index), item);
    if (const std::string problem = orderProblem(order); !problem.empty()) {
        throw std::invalid_argument(problem);
    }
    Edit edit{};
    edit.kind = Edit::Kind::MoveItem;
    edit.id = item;
    edit.place = place;
    edit.newPlace = index;
    record(std::move(edit), "Reorder Timeline");
}

std::optional<AxisState> Document::axisState(ObjectId axis) const {
    const auto found = m_axes.find(axis);
    if (found == m_axes.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<PlaneState> Document::planeState(ObjectId plane) const {
    const auto found = m_planes.find(plane);
    if (found == m_planes.end()) {
        return std::nullopt;
    }
    return found->second;
}

kernel::NamedShape Document::namedBody(ObjectId body) const {
    const Body* found = findBody(body);
    return found == nullptr ? kernel::NamedShape{}
                            : kernel::NamedShape{found->shape, found->faceNames};
}

// ---------------------------------------------------------------------------------------------
// Parameters

const Parameter* Document::findParameter(ObjectId id) const {
    const auto found = std::find_if(m_parameters.begin(), m_parameters.end(),
                                    [id](const Parameter& p) { return p.id == id; });
    return found == m_parameters.end() ? nullptr : &*found;
}

const Parameter* Document::findParameter(std::string_view name) const {
    const auto found = std::find_if(m_parameters.begin(), m_parameters.end(),
                                    [name](const Parameter& p) { return p.name == name; });
    return found == m_parameters.end() ? nullptr : &*found;
}

std::string Document::uniqueParameterName(std::string_view base) const {
    for (int number = 1;; ++number) {
        const std::string candidate = std::string(base) + std::to_string(number);
        if (findParameter(candidate) == nullptr) {
            return candidate;
        }
    }
}

namespace {

void requireParameterName(const std::string& name, const char* function) {
    if (!core::isValidName(name)) {
        throw std::invalid_argument(std::string(function) + ": \"" + name +
                                    "\" cannot be a parameter's name. A name starts with a "
                                    "letter, and has only letters, digits, and _ in it.");
    }
}

} // namespace

ObjectId Document::addParameter(std::string name, std::string expression, std::string comment) {
    requireParameterName(name, "addParameter");
    if (findParameter(name) != nullptr) {
        throw std::invalid_argument("addParameter: there is a parameter called " + name +
                                    " already");
    }
    Edit edit{};
    edit.kind = Edit::Kind::AddParameter;
    edit.id = ObjectId{m_nextId++};
    edit.index = m_parameters.size();
    edit.parameter = Parameter{edit.id, std::move(name), std::move(expression), std::move(comment)};
    const ObjectId id = edit.id;
    record(std::move(edit), "Add Parameter");
    return id;
}

void Document::changeParameter(ObjectId id, Parameter definition) {
    const Parameter* old = findParameter(id);
    if (old == nullptr) {
        throw std::invalid_argument("changeParameter: there is no parameter with id " +
                                    std::to_string(id.value));
    }
    definition.id = id;
    requireParameterName(definition.name, "changeParameter");
    if (const Parameter* same = findParameter(definition.name); same != nullptr && same->id != id) {
        throw std::invalid_argument("changeParameter: there is a parameter called " +
                                    definition.name + " already");
    }
    if (definition == *old) {
        return;
    }
    Edit edit{};
    edit.kind = Edit::Kind::ChangeParameter;
    edit.id = id;
    edit.parameter = *old;
    edit.newParameter = definition;
    if (definition.name != old->name) {
        // Everything that uses the old name uses the new one instead.
        const auto uses = [&old](const std::string& expression) {
            try {
                const std::vector<std::string> names = core::namesIn(expression);
                return std::find(names.begin(), names.end(), old->name) != names.end();
            } catch (const core::ExpressionError&) {
                return false;
            }
        };
        const auto rename = [&old, &definition](const std::string& expression) {
            return core::renamed(expression, old->name, definition.name);
        };
        for (const Parameter& other : m_parameters) {
            if (other.id != id && uses(other.expression)) {
                Parameter changed = other;
                changed.expression = rename(other.expression);
                edit.renamedParameters.emplace_back(other, changed);
            }
        }
        if (uses(edit.newParameter.expression)) {
            edit.newParameter.expression = rename(edit.newParameter.expression);
        }
        for (const Feature& feature : m_features) {
            Feature changed = feature;
            bool any = false;
            for (auto& [field, expression] : changed.expressions) {
                if (uses(expression)) {
                    expression = rename(expression);
                    any = true;
                }
            }
            if (any) {
                edit.renamedFeatures.emplace_back(feature, changed);
            }
        }
        for (const SketchItem& sketch : m_sketches) {
            Edit::SketchExpressions change{sketch.id, sketch.expressions, sketch.expressions};
            bool any = false;
            for (auto& [dimension, expression] : change.after) {
                if (uses(expression)) {
                    expression = rename(expression);
                    any = true;
                }
            }
            if (any) {
                edit.renamedSketches.push_back(std::move(change));
            }
        }
    }
    record(std::move(edit), "Change Parameter");
}

void Document::removeParameter(ObjectId id) {
    const auto found = std::find_if(m_parameters.begin(), m_parameters.end(),
                                    [id](const Parameter& p) { return p.id == id; });
    if (found == m_parameters.end()) {
        throw std::invalid_argument("removeParameter: there is no parameter with id " +
                                    std::to_string(id.value));
    }
    Edit edit{};
    edit.kind = Edit::Kind::RemoveParameter;
    edit.id = id;
    edit.index = static_cast<std::size_t>(found - m_parameters.begin());
    edit.parameter = *found;
    record(std::move(edit), "Delete Parameter");
}

std::optional<double> Document::parameterValue(ObjectId id) const {
    const auto found = m_parameterValues.find(id);
    if (found == m_parameterValues.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::string Document::parameterError(ObjectId id) const {
    const auto found = m_parameterErrors.find(id);
    return found == m_parameterErrors.end() ? std::string() : found->second;
}

double Document::evaluateForModel(const std::string& expression) const {
    try {
        return core::evaluate(expression, [this](const std::string& name) -> std::optional<double> {
            const Parameter* parameter = findParameter(name);
            if (parameter == nullptr) {
                return std::nullopt;
            }
            if (const auto value = parameterValue(parameter->id)) {
                return value;
            }
            throw core::ExpressionError("The parameter " + name + " cannot be worked out itself.");
        });
    } catch (const core::ExpressionError& error) {
        throw std::invalid_argument(error.what());
    }
}

double Document::evaluate(std::string_view expression) const {
    return evaluateForModel(std::string(expression));
}

void Document::setDimensionExpression(ObjectId sketch, sketch::ConstraintId dimension,
                                      std::string expression) {
    const std::size_t index = sketchIndexOf(sketch);
    if (index == notFound) {
        throw std::invalid_argument("setDimensionExpression: there is no sketch with id " +
                                    std::to_string(sketch.value));
    }
    const sketch::Constraint* constraint = m_sketches[index].geometry.findConstraint(dimension);
    if (constraint == nullptr || !sketch::isDimension(constraint->kind)) {
        throw std::invalid_argument("setDimensionExpression: there is no such dimension");
    }
    if (!expression.empty()) {
        (void)evaluateForModel(expression);
    }
    Edit::SketchExpressions change{sketch, m_sketches[index].expressions,
                                   m_sketches[index].expressions};
    if (expression.empty()) {
        change.after.erase(dimension);
    } else {
        change.after[dimension] = std::move(expression);
    }
    if (change.after == change.before) {
        return;
    }
    Edit edit{};
    edit.kind = Edit::Kind::SetSketchExpressions;
    edit.id = sketch;
    edit.renamedSketches.push_back(std::move(change));
    record(std::move(edit), "Dimension from Parameters");
}

void Document::computeParameters() {
    m_parameterValues.clear();
    m_parameterErrors.clear();
    std::unordered_set<ObjectId> working;
    std::function<void(const Parameter&)> compute = [&](const Parameter& parameter) {
        if (m_parameterValues.contains(parameter.id) || m_parameterErrors.contains(parameter.id)) {
            return;
        }
        working.insert(parameter.id);
        try {
            m_parameterValues[parameter.id] = core::evaluate(
                parameter.expression, [&](const std::string& name) -> std::optional<double> {
                    const Parameter* other = findParameter(name);
                    if (other == nullptr) {
                        return std::nullopt;
                    }
                    if (working.contains(other->id)) {
                        throw core::ExpressionError(
                            other->id == parameter.id ? std::string("It is worked out from itself.")
                                                      : "It is worked out from " + name +
                                                            ", which is worked out from it.");
                    }
                    compute(*other);
                    if (const auto error = m_parameterErrors.find(other->id);
                        error != m_parameterErrors.end()) {
                        throw core::ExpressionError("It uses " + name +
                                                    ", which cannot be worked out.");
                    }
                    return m_parameterValues.at(other->id);
                });
        } catch (const core::ExpressionError& error) {
            m_parameterErrors[parameter.id] = error.what();
        }
        working.erase(parameter.id);
    };
    for (const Parameter& parameter : m_parameters) {
        compute(parameter);
    }
}

Feature Document::resolved(const Feature& feature) const {
    if (feature.expressions.empty()) {
        return feature;
    }
    static const std::map<std::string, std::string> labels{
        {"distance", "distance"},
        {"distanceBack", "distance the other way"},
        {"taper", "taper"},
        {"angle", "angle"},
        {"size", "size"},
        {"count", "count"},
        {"count2", "second count"},
        {"spacing", "spacing"},
        {"spacing2", "second spacing"},
        {"tipAngle", "tip angle"},
        {"headDiameter", "head diameter"},
        {"headDepth", "head depth"},
        {"countersinkAngle", "countersink angle"},
    };
    Feature result = feature;
    for (const auto& [field, expression] : feature.expressions) {
        const auto label = labels.find(field);
        if (label == labels.end()) {
            throw detail::FeatureFailure("It has an expression for \"" + field +
                                         "\", which it has no value of.");
        }
        double value = 0.0;
        try {
            value = evaluateForModel(expression);
        } catch (const std::invalid_argument& error) {
            throw detail::FeatureFailure("Its " + label->second + ", \"" + expression +
                                         "\", cannot be worked out. " + error.what());
        }
        const auto whole = [&]() {
            if (std::abs(value - std::round(value)) > 1e-9 || std::abs(value) > 1e6) {
                throw detail::FeatureFailure("Its " + label->second + ", \"" + expression +
                                             "\", is not a whole number.");
            }
            return static_cast<int>(std::lround(value));
        };
        if (field == "distance") {
            result.distance = value;
        } else if (field == "distanceBack") {
            result.distanceBack = value;
        } else if (field == "taper") {
            result.taper = value;
        } else if (field == "angle") {
            result.angle = value;
        } else if (field == "size") {
            result.size = value;
        } else if (field == "count") {
            result.count = whole();
        } else if (field == "count2") {
            result.count2 = whole();
        } else if (field == "spacing") {
            result.spacing = value;
        } else if (field == "spacing2") {
            result.spacing2 = value;
        } else if (field == "tipAngle") {
            result.tipAngle = value;
        } else if (field == "headDiameter") {
            result.headDiameter = value;
        } else if (field == "headDepth") {
            result.headDepth = value;
        } else if (field == "countersinkAngle") {
            result.countersinkAngle = value;
        }
    }
    return result;
}

FeatureTrial Document::tryFeature(Feature feature) const {
    for (std::vector<sketch::EntityId>& outline : feature.regions) {
        std::sort(outline.begin(), outline.end());
    }
    detail::BodyStates bodies;
    for (const Body& body : m_bodies) {
        if (!body.shape.isNull()) {
            bodies[body.id] = kernel::NamedShape{body.shape, body.faceNames};
        }
    }
    FeatureTrial trial;
    try {
        feature = resolved(feature);
    } catch (const detail::FeatureFailure& failure) {
        trial.error = failure.what();
        return trial;
    }
    if (feature.kind == FeatureKind::Axis) {
        try {
            trial.axis = detail::placeAxis(feature, *this, bodies);
        } catch (const detail::FeatureFailure& failure) {
            trial.error = failure.what();
        }
        return trial;
    }
    if (feature.kind == FeatureKind::Plane) {
        try {
            trial.plane = detail::placePlane(feature, *this, bodies);
        } catch (const detail::FeatureFailure& failure) {
            trial.error = failure.what();
        }
        return trial;
    }
    try {
        trial.result = detail::evaluate(feature, *this, bodies, &trial.tool);
    } catch (const detail::FeatureFailure& failure) {
        trial.result = {};
        trial.error = failure.what();
    }
    return trial;
}

void Document::setEditedFeature(ObjectId feature) {
    if (feature.isValid() && featureIndexOf(feature) == notFound) {
        throw std::invalid_argument("setEditedFeature: there is no feature with id " +
                                    std::to_string(feature.value));
    }
    if (feature == m_edited) {
        return;
    }
    m_edited = feature;
    m_rebuildAll = true;
    rebuild();
    notify(Change::Kind::HistoryChanged);
}

void Document::suspendRebuild() {
    ++m_suspended;
}

void Document::resumeRebuild() {
    if (m_suspended == 0) {
        throw std::logic_error("resumeRebuild: rebuilding is not suspended");
    }
    if (--m_suspended == 0) {
        rebuild();
        notify(Change::Kind::HistoryChanged);
    }
}

bool Document::canUndo() const {
    return !m_open && !m_undo.empty();
}

bool Document::canRedo() const {
    return !m_open && !m_redo.empty();
}

std::string Document::undoLabel() const {
    return canUndo() ? m_undo.back().label : std::string();
}

std::string Document::redoLabel() const {
    return canRedo() ? m_redo.back().label : std::string();
}

void Document::undo() {
    if (m_open) {
        throw std::logic_error("undo: a transaction is open");
    }
    if (m_undo.empty()) {
        throw std::logic_error("undo: there is nothing to undo");
    }
    Step step = std::move(m_undo.back());
    m_undo.pop_back();
    for (auto edit = step.edits.rbegin(); edit != step.edits.rend(); ++edit) {
        revert(*edit);
    }
    m_redo.push_back(std::move(step));
    rebuild();
    notify(Change::Kind::HistoryChanged);
}

void Document::redo() {
    if (m_open) {
        throw std::logic_error("redo: a transaction is open");
    }
    if (m_redo.empty()) {
        throw std::logic_error("redo: there is nothing to redo");
    }
    Step step = std::move(m_redo.back());
    m_redo.pop_back();
    for (const Edit& edit : step.edits) {
        apply(edit);
    }
    m_undo.push_back(std::move(step));
    rebuild();
    notify(Change::Kind::HistoryChanged);
}

bool Document::isModified() const {
    return currentSerial() != m_savedSerial;
}

void Document::markSaved() {
    if (m_savedSerial != currentSerial()) {
        m_savedSerial = currentSerial();
        notify(Change::Kind::HistoryChanged);
    }
}

Document::ListenerId Document::addListener(Listener listener) {
    const ListenerId id = m_nextListenerId++;
    m_listeners.emplace_back(id, std::move(listener));
    return id;
}

void Document::removeListener(ListenerId id) {
    std::erase_if(m_listeners, [id](const auto& entry) { return entry.first == id; });
}

void Document::record(Edit edit, const std::string& label) {
    apply(edit);
    rebuild();
    if (m_open) {
        // Dragging sketch geometry replaces the sketch once per pointer movement. Keep only
        // where it started and where it has got to, not every position in between.
        if (edit.kind == Edit::Kind::SetSketchGeometry && !m_open->edits.empty() &&
            m_open->edits.back().kind == Edit::Kind::SetSketchGeometry &&
            m_open->edits.back().id == edit.id) {
            m_open->edits.back().newGeometry = std::move(edit.newGeometry);
            return;
        }
        m_open->edits.push_back(std::move(edit));
        return;
    }
    // An edit made on its own is its own undo step.
    Step step{label, {}, m_nextSerial++};
    step.edits.push_back(std::move(edit));
    m_undo.push_back(std::move(step));
    m_redo.clear();
    notify(Change::Kind::HistoryChanged);
}

void Document::apply(const Edit& edit) {
    switch (edit.kind) {
    case Edit::Kind::AddBody:
        m_bodies.insert(m_bodies.begin() + static_cast<std::ptrdiff_t>(edit.index), edit.body);
        insertItem(edit.id, edit.place);
        m_marker = edit.newMarker;
        touch(edit.id);
        notify(Change::Kind::BodyAdded, edit.id);
        break;
    case Edit::Kind::RemoveBody:
        m_bodies.erase(m_bodies.begin() + static_cast<std::ptrdiff_t>(edit.index));
        eraseItem(edit.id);
        m_marker = edit.newMarker;
        touch(edit.id);
        notify(Change::Kind::BodyRemoved, edit.id);
        break;
    case Edit::Kind::RenameBody:
        m_bodies[indexOf(edit.id)].name = edit.newName;
        notify(Change::Kind::BodyRenamed, edit.id);
        break;
    case Edit::Kind::SetBodyVisible:
        m_bodies[indexOf(edit.id)].visible = edit.newVisible;
        notify(Change::Kind::BodyVisibilityChanged, edit.id);
        break;
    case Edit::Kind::AddSketch:
        m_sketches.insert(m_sketches.begin() + static_cast<std::ptrdiff_t>(edit.index),
                          edit.sketch);
        insertItem(edit.id, edit.place);
        m_marker = edit.newMarker;
        touch(edit.id);
        notify(Change::Kind::SketchAdded, edit.id);
        break;
    case Edit::Kind::RemoveSketch:
        m_sketches.erase(m_sketches.begin() + static_cast<std::ptrdiff_t>(edit.index));
        eraseItem(edit.id);
        m_marker = edit.newMarker;
        touch(edit.id);
        notify(Change::Kind::SketchRemoved, edit.id);
        break;
    case Edit::Kind::RenameSketch:
        m_sketches[sketchIndexOf(edit.id)].name = edit.newName;
        notify(Change::Kind::SketchRenamed, edit.id);
        break;
    case Edit::Kind::SetSketchVisible:
        m_sketches[sketchIndexOf(edit.id)].visible = edit.newVisible;
        notify(Change::Kind::SketchVisibilityChanged, edit.id);
        break;
    case Edit::Kind::SetSketchGeometry:
        m_sketches[sketchIndexOf(edit.id)].geometry = edit.newGeometry;
        touch(edit.id);
        notify(Change::Kind::SketchGeometryChanged, edit.id);
        break;
    case Edit::Kind::AddFeature:
        m_features.push_back(edit.feature);
        insertItem(edit.id, edit.place);
        m_marker = edit.newMarker;
        touch(edit.id);
        touch(edit.feature.body);
        if (edit.hasBody) {
            m_bodies.insert(m_bodies.begin() + static_cast<std::ptrdiff_t>(edit.index), edit.body);
            notify(Change::Kind::BodyAdded, edit.body.id);
        }
        notify(Change::Kind::TimelineChanged);
        break;
    case Edit::Kind::RemoveFeature:
        m_features.erase(m_features.begin() + static_cast<std::ptrdiff_t>(featureIndexOf(edit.id)));
        eraseItem(edit.id);
        m_marker = edit.newMarker;
        m_built.erase(edit.id);
        touch(edit.feature.body);
        if (edit.hasBody) {
            m_bodies.erase(m_bodies.begin() + static_cast<std::ptrdiff_t>(edit.index));
            notify(Change::Kind::BodyRemoved, edit.body.id);
        }
        notify(Change::Kind::TimelineChanged);
        break;
    case Edit::Kind::ChangeFeature: {
        // A new name alone changes nothing about the model.
        Feature renamed = edit.feature;
        renamed.name = edit.newFeature.name;
        m_features[featureIndexOf(edit.id)] = edit.newFeature;
        if (!(renamed == edit.newFeature)) {
            touch(edit.id);
            touch(edit.feature.body);
            touch(edit.newFeature.body);
        }
        if (edit.bodyChange > 0) {
            m_bodies.insert(m_bodies.begin() + static_cast<std::ptrdiff_t>(edit.index), edit.body);
            notify(Change::Kind::BodyAdded, edit.body.id);
        } else if (edit.bodyChange < 0) {
            m_bodies.erase(m_bodies.begin() + static_cast<std::ptrdiff_t>(edit.index));
            notify(Change::Kind::BodyRemoved, edit.body.id);
        }
        notify(Change::Kind::TimelineChanged);
        break;
    }
    case Edit::Kind::MoveItem:
        m_timeline.erase(m_timeline.begin() + static_cast<std::ptrdiff_t>(edit.place));
        m_timeline.insert(m_timeline.begin() + static_cast<std::ptrdiff_t>(edit.newPlace), edit.id);
        m_rebuildAll = true;
        notify(Change::Kind::TimelineChanged);
        break;
    case Edit::Kind::SetMarker:
        m_marker = edit.newMarker;
        m_rebuildAll = true;
        notify(Change::Kind::TimelineChanged);
        break;
    case Edit::Kind::AddParameter:
        m_parameters.insert(m_parameters.begin() + static_cast<std::ptrdiff_t>(edit.index),
                            edit.parameter);
        m_rebuildAll = true;
        notify(Change::Kind::ParametersChanged);
        break;
    case Edit::Kind::RemoveParameter:
        m_parameters.erase(m_parameters.begin() + static_cast<std::ptrdiff_t>(edit.index));
        m_rebuildAll = true;
        notify(Change::Kind::ParametersChanged);
        break;
    case Edit::Kind::ChangeParameter:
    case Edit::Kind::SetSketchExpressions:
        applyExpressions(edit, true);
        break;
    }
}

void Document::applyExpressions(const Edit& edit, bool forward) {
    // A parameter changed, and with a new name, what used the old one; or a sketch's
    // expressions.
    const auto setParameter = [this](const Parameter& to) {
        for (Parameter& parameter : m_parameters) {
            if (parameter.id == to.id) {
                parameter = to;
            }
        }
    };
    if (edit.kind == Edit::Kind::ChangeParameter) {
        setParameter(forward ? edit.newParameter : edit.parameter);
    }
    for (const auto& [before, after] : edit.renamedParameters) {
        setParameter(forward ? after : before);
    }
    for (const auto& [before, after] : edit.renamedFeatures) {
        const std::size_t index = featureIndexOf(before.id);
        if (index != notFound) {
            m_features[index] = forward ? after : before;
            touch(before.id);
        }
    }
    for (const Edit::SketchExpressions& change : edit.renamedSketches) {
        const std::size_t index = sketchIndexOf(change.sketch);
        if (index != notFound) {
            m_sketches[index].expressions = forward ? change.after : change.before;
            touch(change.sketch);
        }
    }
    m_rebuildAll = true;
    if (edit.kind == Edit::Kind::ChangeParameter) {
        notify(Change::Kind::ParametersChanged);
    } else {
        notify(Change::Kind::SketchGeometryChanged, edit.id);
    }
}

void Document::revert(const Edit& edit) {
    switch (edit.kind) {
    case Edit::Kind::AddBody:
        m_bodies.erase(m_bodies.begin() + static_cast<std::ptrdiff_t>(indexOf(edit.id)));
        eraseItem(edit.id);
        m_marker = edit.oldMarker;
        touch(edit.id);
        notify(Change::Kind::BodyRemoved, edit.id);
        break;
    case Edit::Kind::RemoveBody: {
        // Back where it was, with the name and visibility it had when it was removed.
        const std::size_t index = std::min(edit.index, m_bodies.size());
        m_bodies.insert(m_bodies.begin() + static_cast<std::ptrdiff_t>(index), edit.body);
        insertItem(edit.id, edit.place);
        m_marker = edit.oldMarker;
        touch(edit.id);
        notify(Change::Kind::BodyAdded, edit.id);
        break;
    }
    case Edit::Kind::RenameBody:
        m_bodies[indexOf(edit.id)].name = edit.oldName;
        notify(Change::Kind::BodyRenamed, edit.id);
        break;
    case Edit::Kind::SetBodyVisible:
        m_bodies[indexOf(edit.id)].visible = edit.oldVisible;
        notify(Change::Kind::BodyVisibilityChanged, edit.id);
        break;
    case Edit::Kind::AddSketch:
        m_sketches.erase(m_sketches.begin() + static_cast<std::ptrdiff_t>(sketchIndexOf(edit.id)));
        eraseItem(edit.id);
        m_marker = edit.oldMarker;
        touch(edit.id);
        notify(Change::Kind::SketchRemoved, edit.id);
        break;
    case Edit::Kind::RemoveSketch: {
        const std::size_t index = std::min(edit.index, m_sketches.size());
        m_sketches.insert(m_sketches.begin() + static_cast<std::ptrdiff_t>(index), edit.sketch);
        insertItem(edit.id, edit.place);
        m_marker = edit.oldMarker;
        touch(edit.id);
        notify(Change::Kind::SketchAdded, edit.id);
        break;
    }
    case Edit::Kind::RenameSketch:
        m_sketches[sketchIndexOf(edit.id)].name = edit.oldName;
        notify(Change::Kind::SketchRenamed, edit.id);
        break;
    case Edit::Kind::SetSketchVisible:
        m_sketches[sketchIndexOf(edit.id)].visible = edit.oldVisible;
        notify(Change::Kind::SketchVisibilityChanged, edit.id);
        break;
    case Edit::Kind::SetSketchGeometry:
        m_sketches[sketchIndexOf(edit.id)].geometry = edit.sketch.geometry;
        touch(edit.id);
        notify(Change::Kind::SketchGeometryChanged, edit.id);
        break;
    case Edit::Kind::AddFeature:
        m_features.erase(m_features.begin() + static_cast<std::ptrdiff_t>(featureIndexOf(edit.id)));
        eraseItem(edit.id);
        m_marker = edit.oldMarker;
        m_built.erase(edit.id);
        touch(edit.feature.body);
        if (edit.hasBody) {
            m_bodies.erase(m_bodies.begin() + static_cast<std::ptrdiff_t>(indexOf(edit.body.id)));
            notify(Change::Kind::BodyRemoved, edit.body.id);
        }
        notify(Change::Kind::TimelineChanged);
        break;
    case Edit::Kind::RemoveFeature:
        m_features.push_back(edit.feature);
        insertItem(edit.id, edit.place);
        m_marker = edit.oldMarker;
        touch(edit.id);
        touch(edit.feature.body);
        if (edit.hasBody) {
            const std::size_t index = std::min(edit.index, m_bodies.size());
            m_bodies.insert(m_bodies.begin() + static_cast<std::ptrdiff_t>(index), edit.body);
            notify(Change::Kind::BodyAdded, edit.body.id);
        }
        notify(Change::Kind::TimelineChanged);
        break;
    case Edit::Kind::ChangeFeature: {
        Feature renamed = edit.newFeature;
        renamed.name = edit.feature.name;
        m_features[featureIndexOf(edit.id)] = edit.feature;
        if (!(renamed == edit.feature)) {
            touch(edit.id);
            touch(edit.feature.body);
            touch(edit.newFeature.body);
        }
        if (edit.bodyChange > 0) {
            m_bodies.erase(m_bodies.begin() + static_cast<std::ptrdiff_t>(indexOf(edit.body.id)));
            notify(Change::Kind::BodyRemoved, edit.body.id);
        } else if (edit.bodyChange < 0) {
            const std::size_t index = std::min(edit.index, m_bodies.size());
            m_bodies.insert(m_bodies.begin() + static_cast<std::ptrdiff_t>(index), edit.body);
            notify(Change::Kind::BodyAdded, edit.body.id);
        }
        notify(Change::Kind::TimelineChanged);
        break;
    }
    case Edit::Kind::MoveItem:
        m_timeline.erase(m_timeline.begin() + static_cast<std::ptrdiff_t>(edit.newPlace));
        m_timeline.insert(m_timeline.begin() + static_cast<std::ptrdiff_t>(edit.place), edit.id);
        m_rebuildAll = true;
        notify(Change::Kind::TimelineChanged);
        break;
    case Edit::Kind::SetMarker:
        m_marker = edit.oldMarker;
        m_rebuildAll = true;
        notify(Change::Kind::TimelineChanged);
        break;
    case Edit::Kind::AddParameter:
        m_parameters.erase(m_parameters.begin() + static_cast<std::ptrdiff_t>(edit.index));
        m_rebuildAll = true;
        notify(Change::Kind::ParametersChanged);
        break;
    case Edit::Kind::RemoveParameter:
        m_parameters.insert(m_parameters.begin() + static_cast<std::ptrdiff_t>(
                                                       std::min(edit.index, m_parameters.size())),
                            edit.parameter);
        m_rebuildAll = true;
        notify(Change::Kind::ParametersChanged);
        break;
    case Edit::Kind::ChangeParameter:
    case Edit::Kind::SetSketchExpressions:
        applyExpressions(edit, false);
        break;
    }
}

void Document::notify(Change::Kind kind, ObjectId body) {
    // A copy: a listener may add or remove listeners while it runs.
    const auto listeners = m_listeners;
    for (const auto& [id, listener] : listeners) {
        const bool stillRegistered =
            std::any_of(m_listeners.begin(), m_listeners.end(),
                        [id = id](const auto& entry) { return entry.first == id; });
        if (stillRegistered) {
            listener(Change{kind, body});
        }
    }
}

std::size_t Document::indexOf(ObjectId id) const {
    for (std::size_t i = 0; i < m_bodies.size(); ++i) {
        if (m_bodies[i].id == id) {
            return i;
        }
    }
    return notFound;
}

std::size_t Document::sketchIndexOf(ObjectId id) const {
    for (std::size_t i = 0; i < m_sketches.size(); ++i) {
        if (m_sketches[i].id == id) {
            return i;
        }
    }
    return notFound;
}

std::size_t Document::featureIndexOf(ObjectId id) const {
    for (std::size_t i = 0; i < m_features.size(); ++i) {
        if (m_features[i].id == id) {
            return i;
        }
    }
    return notFound;
}

std::size_t Document::placeOf(ObjectId item) const {
    const auto found = std::find(m_timeline.begin(), m_timeline.end(), item);
    return found == m_timeline.end() ? notFound
                                     : static_cast<std::size_t>(found - m_timeline.begin());
}

void Document::insertItem(ObjectId item, std::size_t place) {
    place = std::min(place, m_timeline.size());
    m_timeline.insert(m_timeline.begin() + static_cast<std::ptrdiff_t>(place), item);
}

void Document::eraseItem(ObjectId item) {
    std::erase(m_timeline, item);
}

std::size_t Document::placeForNew() const {
    return historyMarker();
}

void Document::markersForAdding(Edit& edit) const {
    edit.oldMarker = m_marker;
    // A marker in the middle moves on past what is put there. One at the end stays there.
    edit.newMarker = m_marker < m_timeline.size() ? m_marker + 1 : m_marker;
}

void Document::markersForRemoving(Edit& edit, std::size_t place) const {
    edit.oldMarker = m_marker;
    edit.newMarker = m_marker < m_timeline.size() && place < m_marker ? m_marker - 1 : m_marker;
}

std::string Document::orderProblem(const std::vector<ObjectId>& order) const {
    const auto at = [&order](ObjectId item) {
        return static_cast<std::size_t>(std::find(order.begin(), order.end(), item) -
                                        order.begin());
    };
    // A sketch comes after what it is drawn on.
    for (const SketchItem& sketch : m_sketches) {
        if (!sketch.attachedTo.isValid()) {
            continue;
        }
        const std::size_t place = at(sketch.id);
        if (const Feature* plane = findFeature(sketch.attachedTo)) {
            if (at(plane->id) > place) {
                return sketch.name + " is drawn on " + plane->name +
                       ", so it cannot come before it.";
            }
        } else if (const Body* body = findBody(sketch.attachedTo)) {
            const Feature* maker = findFeature(body->madeBy);
            const ObjectId first = maker != nullptr ? maker->id : body->id;
            if (at(first) > place) {
                return sketch.name + " is drawn on a face of " + body->name +
                       (maker != nullptr ? ", which " + maker->name + " makes" : std::string()) +
                       ", so it cannot come before it.";
            }
        }
    }
    for (const Feature& feature : m_features) {
        const std::size_t place = at(feature.id);
        if (usesSketch(feature)) {
            const SketchItem* sketch = findSketch(feature.sketch);
            if (sketch != nullptr && at(sketch->id) > place) {
                return feature.name + " is made from " + sketch->name +
                       ", so it cannot come before it.";
            }
        }
        for (const std::string& reference : {feature.reference, feature.reference2}) {
            const Feature* construction = findFeature(constructionIn(reference));
            if (construction != nullptr && at(construction->id) > place) {
                return feature.name + " uses " + construction->name +
                       ", so it cannot come before it.";
            }
        }
        for (const ObjectId source : feature.sources) {
            const Feature* copied = findFeature(source);
            if (copied != nullptr && at(copied->id) > place) {
                return feature.name + " copies " + copied->name + ", so it cannot come before it.";
            }
        }
        // A pull up to a face comes after what makes the face.
        if (const Body* limit = findBody(feature.extentBody);
            limit != nullptr && !feature.extentFace.empty()) {
            const Feature* maker = findFeature(limit->madeBy);
            const ObjectId first = maker != nullptr ? maker->id : limit->id;
            if (first != feature.id && at(first) > place) {
                return feature.name + " goes up to a face of " + limit->name +
                       (maker != nullptr ? ", which " + maker->name + " makes" : std::string()) +
                       ", so it cannot come before it.";
            }
        }
        if (makesBody(feature)) {
            continue;
        }
        const Body* body = findBody(feature.body);
        if (body == nullptr) {
            continue;
        }
        if (body->madeBy.isValid()) {
            const Feature* maker = findFeature(body->madeBy);
            if (maker != nullptr && at(maker->id) > place) {
                return feature.name + " works on " + body->name + ", which " + maker->name +
                       " makes, so it cannot come before it.";
            }
        } else if (at(body->id) > place) {
            return feature.name + " works on " + body->name + ", so it cannot come before it.";
        }
    }
    return {};
}

void Document::touch(ObjectId item) {
    if (item.isValid()) {
        m_touched.insert(item);
    }
}

void Document::rebuild() {
    if (m_suspended > 0) {
        return;
    }
    if (!m_rebuildAll && m_touched.empty()) {
        return;
    }
    // How far the timeline is applied: to the marker, and no further than the feature that is
    // being edited.
    std::size_t applied = historyMarker();
    if (m_edited.isValid() && placeOf(m_edited) != notFound) {
        applied = std::min(applied, placeOf(m_edited));
    }

    // What the parameters come to, first: features and sketches are worked out from them.
    const auto valuesBefore = m_parameterValues;
    const auto errorsBefore = m_parameterErrors;
    computeParameters();
    const bool parametersChanged =
        valuesBefore != m_parameterValues || errorsBefore != m_parameterErrors;

    detail::BodyStates state;
    // Bodies whose state, at this point in the timeline, is not what it was last time.
    std::unordered_set<ObjectId> changed;
    // Sketches that have moved with what they are attached to.
    std::unordered_set<ObjectId> moved;
    // Construction planes and axes that have moved, or come or gone.
    std::unordered_set<ObjectId> movedConstruction;
    std::unordered_map<ObjectId, PlaneState> planes;
    bool statusChanged = false;
    // Notes why an item that is not a solid feature fails, or that it works again.
    const auto setError = [this, &statusChanged](ObjectId item, const std::string& error) {
        Built& built = m_built[item];
        if (built.error != error) {
            statusChanged = true;
        }
        built.error = error;
        built.done = true;
        built.applied = error.empty();
    };
    for (std::size_t place = 0; place < m_timeline.size(); ++place) {
        const ObjectId item = m_timeline[place];
        const bool reached = place < applied;
        const std::size_t featureIndex = featureIndexOf(item);
        if (const std::size_t sketchIndex = sketchIndexOf(item); sketchIndex != notFound) {
            SketchItem& sketch = m_sketches[sketchIndex];
            if (!reached) {
                setError(item, {});
                continue;
            }
            std::string error;
            // Dimensions worked out from parameters take what they come to.
            if (!sketch.expressions.empty()) {
                sketch::Sketch geometry = sketch.geometry;
                bool differs = false;
                for (const auto& [dimension, expression] : sketch.expressions) {
                    const sketch::Constraint* constraint = geometry.findConstraint(dimension);
                    if (constraint == nullptr || !sketch::isDimension(constraint->kind)) {
                        // The dimension was deleted; its expression goes with it on the next
                        // edit of the sketch.
                        continue;
                    }
                    double value = 0.0;
                    try {
                        value = evaluateForModel(expression);
                    } catch (const std::invalid_argument& failure) {
                        error = "Its dimension \"" + expression + "\" cannot be worked out. " +
                                failure.what();
                        break;
                    }
                    if (constraint->kind == sketch::ConstraintKind::Angle) {
                        value *= 3.14159265358979323846 / 180.0;
                    }
                    if (std::abs(constraint->value - value) <= 1e-12 * std::max(1.0, value)) {
                        continue;
                    }
                    if (!sketch::tryDimensionValue(geometry, dimension, value).ok()) {
                        error = "Its dimension \"" + expression +
                                "\" comes to a value the sketch cannot take.";
                        break;
                    }
                    differs = true;
                }
                if (error.empty() && differs) {
                    sketch.geometry = std::move(geometry);
                    moved.insert(item);
                }
            }
            // An attached sketch goes where what it is attached to now is.
            if (error.empty() && sketch.attachedTo.isValid()) {
                try {
                    const kernel::Plane now = detail::placeSketch(sketch, *this, state, planes);
                    if (!samePlace(now, sketch.plane)) {
                        sketch.plane = now;
                        moved.insert(item);
                    }
                } catch (const detail::FeatureFailure& failure) {
                    // It stays where it was, and says why.
                    error = failure.what();
                }
            }
            setError(item, error);
            continue;
        }
        if (featureIndex != notFound && m_features[featureIndex].kind == FeatureKind::Axis) {
            const Feature& feature = m_features[featureIndex];
            if (!reached || feature.suppressed) {
                if (m_axes.erase(item) > 0) {
                    statusChanged = true;
                }
                setError(item, {});
                continue;
            }
            try {
                const AxisState placed = detail::placeAxis(resolved(feature), *this, state);
                const auto before = m_axes.find(item);
                const auto same = [](const AxisState& a, const AxisState& b) {
                    const auto near = [](double x, double y) { return std::abs(x - y) < 1e-12; };
                    return near(a.axis.origin.x, b.axis.origin.x) &&
                           near(a.axis.origin.y, b.axis.origin.y) &&
                           near(a.axis.origin.z, b.axis.origin.z) &&
                           near(a.axis.direction.x, b.axis.direction.x) &&
                           near(a.axis.direction.y, b.axis.direction.y) &&
                           near(a.axis.direction.z, b.axis.direction.z) &&
                           near(a.length, b.length) && near(a.center.x, b.center.x) &&
                           near(a.center.y, b.center.y) && near(a.center.z, b.center.z);
                };
                if (before == m_axes.end() || !same(before->second, placed)) {
                    statusChanged = true;
                    // What turns about it, or goes around it, is built again.
                    movedConstruction.insert(item);
                }
                m_axes[item] = placed;
                setError(item, {});
            } catch (const detail::FeatureFailure& failure) {
                if (m_axes.erase(item) > 0) {
                    statusChanged = true;
                    movedConstruction.insert(item);
                }
                setError(item, failure.what());
            }
            continue;
        }
        if (featureIndex != notFound && m_features[featureIndex].kind == FeatureKind::Plane) {
            const Feature& feature = m_features[featureIndex];
            if (!reached || feature.suppressed) {
                if (m_planes.erase(item) > 0) {
                    statusChanged = true;
                }
                setError(item, {});
                continue;
            }
            try {
                const PlaneState placed = detail::placePlane(resolved(feature), *this, state);
                const auto before = m_planes.find(item);
                if (before == m_planes.end() || !samePlace(before->second.plane, placed.plane) ||
                    before->second.size != placed.size) {
                    statusChanged = true;
                    movedConstruction.insert(item);
                }
                planes[item] = placed;
                m_planes[item] = placed;
                setError(item, {});
            } catch (const detail::FeatureFailure& failure) {
                if (m_planes.erase(item) > 0) {
                    statusChanged = true;
                }
                setError(item, failure.what());
            }
            continue;
        }
        if (featureIndex == notFound) {
            if (const Body* body = findBody(item)) {
                // A body no feature makes starts from the shape it was given.
                if (reached) {
                    state[item] =
                        kernel::nameFaces(body->source, std::to_string(item.value) + "/f");
                }
                if (m_rebuildAll || m_touched.contains(item)) {
                    changed.insert(item);
                }
            }
            continue;
        }
        const Feature& feature = m_features[featureIndex];
        Built& built = m_built[item];
        if (!reached || feature.suppressed) {
            if (built.applied || !built.error.empty() || m_touched.contains(item)) {
                changed.insert(feature.body);
                statusChanged = true;
            }
            built = Built{};
            built.done = true;
            continue;
        }
        const bool stale =
            m_rebuildAll || !built.done || m_touched.contains(item) ||
            m_touched.contains(feature.body) ||
            (feature.sketch.isValid() &&
             (m_touched.contains(feature.sketch) || moved.contains(feature.sketch))) ||
            changed.contains(feature.body) ||
            (feature.extentBody.isValid() &&
             (changed.contains(feature.extentBody) || m_touched.contains(feature.extentBody))) ||
            movedConstruction.contains(constructionIn(feature.reference)) ||
            movedConstruction.contains(constructionIn(feature.reference2));
        if (!stale) {
            if (built.applied) {
                state[feature.body] = built.result;
            }
            continue;
        }
        const std::string before = built.error;
        try {
            built.result = detail::evaluate(resolved(feature), *this, state);
            built.applied = true;
            built.error.clear();
            state[feature.body] = built.result;
        } catch (const detail::FeatureFailure& failure) {
            // Left out. The model carries on from what it was before this feature.
            built.result = {};
            built.applied = false;
            built.error = failure.what();
        }
        built.done = true;
        statusChanged = statusChanged || before != built.error;
        changed.insert(feature.body);
    }
    m_touched.clear();
    m_rebuildAll = false;

    std::vector<ObjectId> reshaped;
    for (Body& body : m_bodies) {
        const auto found = state.find(body.id);
        const kernel::NamedShape now = found == state.end() ? kernel::NamedShape{} : found->second;
        if (now.shape.impl() != body.shape.impl()) {
            body.shape = now.shape;
            reshaped.push_back(body.id);
        }
        body.faceNames = now.faceNames;
    }
    for (const ObjectId body : reshaped) {
        if (findBody(body) != nullptr) {
            notify(Change::Kind::BodyGeometryChanged, body);
        }
    }
    for (const ObjectId sketch : moved) {
        notify(Change::Kind::SketchGeometryChanged, sketch);
    }
    if (parametersChanged) {
        notify(Change::Kind::ParametersChanged);
    }
    if (statusChanged || !reshaped.empty()) {
        notify(Change::Kind::TimelineChanged);
    }
}

std::uint64_t Document::currentSerial() const {
    return m_undo.empty() ? 0 : m_undo.back().serial;
}

void Document::beginTransaction(std::string label) {
    if (m_open) {
        throw std::logic_error("Transaction: \"" + m_open->label + "\" is still open");
    }
    m_open = Step{std::move(label), {}, 0};
    notify(Change::Kind::HistoryChanged);
}

void Document::commitTransaction() {
    Step step = std::move(*m_open);
    m_open.reset();
    if (!step.edits.empty()) {
        step.serial = m_nextSerial++;
        m_undo.push_back(std::move(step));
        m_redo.clear();
    }
    notify(Change::Kind::HistoryChanged);
}

void Document::rollbackTransaction() {
    Step step = std::move(*m_open);
    m_open.reset();
    for (auto edit = step.edits.rbegin(); edit != step.edits.rend(); ++edit) {
        revert(*edit);
    }
    rebuild();
    notify(Change::Kind::HistoryChanged);
}

Transaction::Transaction(Document& document, std::string label) : m_document(document) {
    m_document.beginTransaction(std::move(label));
}

Transaction::~Transaction() {
    if (!m_finished) {
        m_document.rollbackTransaction();
    }
}

void Transaction::commit() {
    if (m_finished) {
        throw std::logic_error("Transaction: already committed");
    }
    m_finished = true;
    m_document.commitTransaction();
}

} // namespace lcad::document
