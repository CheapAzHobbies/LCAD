// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/ObjectId.h>
#include <lcad/kernel/Shape.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lcad::document {

/// A solid (or other shape) in the document, as the browser lists it.
struct Body {
    ObjectId id;
    std::string name;
    bool visible = true;
    kernel::Shape shape;
};

/// What a listener is told when the document changes.
struct Change {
    enum class Kind {
        BodyAdded,
        BodyRemoved,
        BodyRenamed,
        BodyVisibilityChanged,
        /// What undo and redo would do, or whether there are unsaved changes, may have changed.
        HistoryChanged,
    };
    Kind kind = Kind::HistoryChanged;
    /// The body concerned. Not valid for HistoryChanged.
    ObjectId body;
};

/// Everything needed to rebuild a document, as read from a file.
struct DocumentState {
    std::vector<Body> bodies;
    /// The id the next new object gets. Larger than every id in use.
    std::uint64_t nextId = 1;
};

/// The model, and the single source of truth for it. The viewport and the interface observe a
/// document and ask it to change; they keep no model state of their own.
///
/// Every edit is undoable. An edit made on its own is one undo step. To make several edits one
/// step, or to be able to abandon them together, make them inside a Transaction.
///
/// Not thread-safe: use a document from one thread.
class Document {
public:
    Document();
    /// Rebuilds a document from saved state, with an empty undo history and no unsaved changes.
    /// Throws std::invalid_argument when the state is inconsistent.
    explicit Document(DocumentState state);
    ~Document();

    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    // Reading

    /// The bodies, in the order they were created.
    [[nodiscard]] const std::vector<Body>& bodies() const { return m_bodies; }
    /// The body with this id, or nullptr. The pointer is valid until the next edit.
    [[nodiscard]] const Body* findBody(ObjectId id) const;
    /// `base` if no body has that name, otherwise `base` with the lowest free number appended:
    /// "Box", "Box 2", "Box 3".
    [[nodiscard]] std::string uniqueBodyName(std::string_view base) const;
    [[nodiscard]] std::uint64_t nextObjectId() const { return m_nextId; }

    // Editing. Each throws std::invalid_argument for input that cannot be applied and leaves
    // the document unchanged.

    /// Adds a body and returns its id. The name must not be empty, and the shape must pass the
    /// kernel's validity check.
    ObjectId addBody(std::string name, kernel::Shape shape);
    void removeBody(ObjectId id);
    void renameBody(ObjectId id, std::string name);
    void setBodyVisible(ObjectId id, bool visible);

    // Undo

    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;
    /// What undo() would undo, for example "Create Box". Empty when there is nothing.
    [[nodiscard]] std::string undoLabel() const;
    [[nodiscard]] std::string redoLabel() const;
    /// Throw std::logic_error when there is nothing to undo or redo, or a transaction is open.
    void undo();
    void redo();

    // Unsaved changes

    /// Whether the document differs from what was last saved (or, if never saved, from new).
    [[nodiscard]] bool isModified() const;
    /// Records that the current state is what is on disk.
    void markSaved();

    // Observation

    using Listener = std::function<void(const Change&)>;
    using ListenerId = std::uint64_t;
    /// Calls `listener` after each change, until it is removed.
    ListenerId addListener(Listener listener);
    void removeListener(ListenerId id);

private:
    friend class Transaction;

    struct Edit {
        enum class Kind { AddBody, RemoveBody, RenameBody, SetBodyVisible };
        Kind kind;
        ObjectId id;
        // AddBody and RemoveBody: the body, and where in the list it sits.
        Body body;
        std::size_t index = 0;
        // RenameBody
        std::string oldName;
        std::string newName;
        // SetBodyVisible
        bool oldVisible = true;
        bool newVisible = true;
    };

    struct Step {
        std::string label;
        std::vector<Edit> edits;
        // Identifies the state the document is in after this step. Never reused.
        std::uint64_t serial = 0;
    };

    void record(Edit edit, const char* label);
    void apply(const Edit& edit);
    void revert(const Edit& edit);
    void notify(Change::Kind kind, ObjectId body = {});
    [[nodiscard]] std::size_t indexOf(ObjectId id) const;
    [[nodiscard]] std::uint64_t currentSerial() const;

    void beginTransaction(std::string label);
    void commitTransaction();
    void rollbackTransaction();

    std::vector<Body> m_bodies;
    std::uint64_t m_nextId = 1;

    std::vector<Step> m_undo;
    std::vector<Step> m_redo;
    std::optional<Step> m_open;
    std::uint64_t m_nextSerial = 1;
    std::uint64_t m_savedSerial = 0;

    std::vector<std::pair<ListenerId, Listener>> m_listeners;
    ListenerId m_nextListenerId = 1;
};

/// Groups edits into one undo step, and undoes them if the work is abandoned.
///
///     Transaction transaction(document, "Create Box");
///     document.addBody(...);
///     transaction.commit();
///
/// If the Transaction is destroyed without commit(), for example because an exception passes
/// through, every edit made since it began is reverted and no undo step is recorded. Only one
/// transaction can be open on a document at a time.
class Transaction {
public:
    /// Throws std::logic_error if the document already has an open transaction.
    Transaction(Document& document, std::string label);
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    /// Makes the edits one undo step. A transaction with no edits records nothing.
    void commit();

private:
    Document& m_document;
    bool m_finished = false;
};

} // namespace lcad::document
