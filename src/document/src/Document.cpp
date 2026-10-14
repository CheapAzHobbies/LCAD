// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/document/Document.h>

#include "Rebuild.h"

#include <lcad/kernel/Measure.h>

#include <algorithm>
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
    return feature.kind == FeatureKind::Pull || feature.kind == FeatureKind::Revolve;
}

} // namespace

Document::Document() = default;

Document::Document(DocumentState state)
    : m_bodies(std::move(state.bodies)), m_sketches(std::move(state.sketches)),
      m_features(std::move(state.features)), m_timeline(std::move(state.timeline)),
      m_marker(state.marker), m_nextId(state.nextId) {
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
    for (const Feature& feature : m_features) {
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

ObjectId Document::addSketch(std::string name, const kernel::Plane& plane) {
    requireName(name, "addSketch");
    Edit edit{};
    edit.kind = Edit::Kind::AddSketch;
    edit.id = ObjectId{m_nextId};
    edit.index = m_sketches.size();
    edit.sketch = SketchItem{edit.id, std::move(name), true, plane, sketch::Sketch()};
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
    if (!makes && findBody(feature.body) == nullptr) {
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
    } else if (findBody(definition.body) == nullptr) {
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

kernel::NamedShape Document::namedBody(ObjectId body) const {
    const Body* found = findBody(body);
    return found == nullptr ? kernel::NamedShape{}
                            : kernel::NamedShape{found->shape, found->faceNames};
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
    for (const Feature& feature : m_features) {
        const std::size_t place = at(feature.id);
        if (usesSketch(feature)) {
            const SketchItem* sketch = findSketch(feature.sketch);
            if (sketch != nullptr && at(sketch->id) > place) {
                return feature.name + " is made from " + sketch->name +
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

    detail::BodyStates state;
    // Bodies whose state, at this point in the timeline, is not what it was last time.
    std::unordered_set<ObjectId> changed;
    bool statusChanged = false;
    for (std::size_t place = 0; place < m_timeline.size(); ++place) {
        const ObjectId item = m_timeline[place];
        const bool reached = place < applied;
        const std::size_t featureIndex = featureIndexOf(item);
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
        const bool stale = m_rebuildAll || !built.done || m_touched.contains(item) ||
                           m_touched.contains(feature.body) ||
                           (feature.sketch.isValid() && m_touched.contains(feature.sketch)) ||
                           changed.contains(feature.body);
        if (!stale) {
            if (built.applied) {
                state[feature.body] = built.result;
            }
            continue;
        }
        const std::string before = built.error;
        try {
            built.result = detail::evaluate(feature, *this, state);
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
