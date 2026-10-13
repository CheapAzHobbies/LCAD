// SPDX-License-Identifier: GPL-3.0-or-later

#include <lcad/document/Document.h>

#include <lcad/kernel/Measure.h>

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace lcad::document {

namespace {

constexpr std::size_t notFound = static_cast<std::size_t>(-1);

void requireName(const std::string& name, const char* function) {
    if (name.find_first_not_of(" \t\r\n") == std::string::npos) {
        throw std::invalid_argument(std::string(function) + ": a body needs a name");
    }
}

} // namespace

Document::Document() = default;

Document::Document(DocumentState state)
    : m_bodies(std::move(state.bodies)), m_nextId(state.nextId) {
    std::unordered_set<ObjectId> seen;
    for (const Body& body : m_bodies) {
        if (!body.id.isValid() || body.id.value >= m_nextId) {
            throw std::invalid_argument("Document: body id " + std::to_string(body.id.value) +
                                        " is outside the ids the document has handed out");
        }
        if (!seen.insert(body.id).second) {
            throw std::invalid_argument("Document: two bodies share the id " +
                                        std::to_string(body.id.value));
        }
        requireName(body.name, "Document");
        if (body.shape.isNull()) {
            throw std::invalid_argument("Document: body \"" + body.name + "\" has no geometry");
        }
    }
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
    Edit edit{Edit::Kind::AddBody, ObjectId{m_nextId}, {}, m_bodies.size(), {}, {}, true, true};
    edit.body = Body{edit.id, std::move(name), true, std::move(shape)};
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
    record(Edit{Edit::Kind::RemoveBody, id, m_bodies[index], index, {}, {}, true, true},
           "Delete Body");
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
    record(Edit{Edit::Kind::RenameBody,
                id,
                {},
                index,
                m_bodies[index].name,
                std::move(name),
                true,
                true},
           "Rename Body");
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
    record(
        Edit{Edit::Kind::SetBodyVisible, id, {}, index, {}, {}, m_bodies[index].visible, visible},
        visible ? "Show Body" : "Hide Body");
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

void Document::record(Edit edit, const char* label) {
    apply(edit);
    if (m_open) {
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
        notify(Change::Kind::BodyAdded, edit.id);
        break;
    case Edit::Kind::RemoveBody:
        m_bodies.erase(m_bodies.begin() + static_cast<std::ptrdiff_t>(edit.index));
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
    }
}

void Document::revert(const Edit& edit) {
    switch (edit.kind) {
    case Edit::Kind::AddBody:
        m_bodies.erase(m_bodies.begin() + static_cast<std::ptrdiff_t>(indexOf(edit.id)));
        notify(Change::Kind::BodyRemoved, edit.id);
        break;
    case Edit::Kind::RemoveBody: {
        // Back where it was, with the name and visibility it had when it was removed.
        const std::size_t index = std::min(edit.index, m_bodies.size());
        m_bodies.insert(m_bodies.begin() + static_cast<std::ptrdiff_t>(index), edit.body);
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
