// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/Feature.h>
#include <lcad/document/ObjectId.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/Plane.h>
#include <lcad/kernel/Shape.h>
#include <lcad/sketch/Sketch.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lcad::document {

/// A solid (or other shape) in the document, as the browser lists it.
struct Body {
    Body() = default;
    Body(ObjectId bodyId, std::string bodyName, bool isVisible, kernel::Shape bodyShape)
        : id(bodyId), name(std::move(bodyName)), visible(isVisible), shape(std::move(bodyShape)) {}

    ObjectId id;
    std::string name;
    bool visible = true;
    /// The body as it is now, after every feature that is applied to it. Null while the body
    /// does not exist: its feature is suppressed, failed, or beyond the history marker.
    kernel::Shape shape;
    /// The feature that makes this body. Not valid for a body that was put into the document
    /// as it is, such as a primitive: that one starts out as `source`.
    ObjectId madeBy;
    /// For a body that no feature makes: the shape it was given. Features that act on the
    /// body start from this.
    kernel::Shape source;
    /// A name for each face of `shape`; see kernel::NamedShape.
    std::vector<std::string> faceNames;
};

/// A sketch in the document: 2D geometry and constraints, and the plane they lie in.
struct SketchItem {
    ObjectId id;
    std::string name;
    bool visible = true;
    /// Where the sketch sits in space. For a sketch that is attached (see below), where it was
    /// when the model was last built.
    kernel::Plane plane;
    sketch::Sketch geometry;
    /// What the sketch's plane follows, if anything: a construction plane (a feature of kind
    /// Plane), or, with `attachedFace` the name of one of its flat faces, a body. When that
    /// moves, the sketch moves with it, and so does what is made from the sketch.
    ObjectId attachedTo;
    std::string attachedFace;
};

/// What a construction plane is now: where it lies, and where on it to draw it.
struct PlaneState {
    kernel::Plane plane;
    /// The middle of what it was placed against, dropped onto it: where it is drawn.
    kernel::Point center;
    /// How large to draw it, in millimeters.
    double size = 100.0;
};

/// What a listener is told when the document changes.
struct Change {
    enum class Kind {
        BodyAdded,
        BodyRemoved,
        BodyRenamed,
        BodyVisibilityChanged,
        SketchAdded,
        SketchRemoved,
        SketchRenamed,
        SketchVisibilityChanged,
        /// A sketch's geometry or constraints changed.
        SketchGeometryChanged,
        /// A body's shape is another one, or the body has come to exist or stopped existing,
        /// because the features were applied again.
        BodyGeometryChanged,
        /// The timeline changed: a feature was added, removed, changed, renamed, suppressed,
        /// or moved, the history marker moved, or a feature now fails or works again.
        TimelineChanged,
        /// What undo and redo would do, or whether there are unsaved changes, may have changed.
        HistoryChanged,
    };
    Kind kind = Kind::HistoryChanged;
    /// The body or sketch concerned. Not valid for HistoryChanged and TimelineChanged.
    ObjectId body;
};

/// Everything needed to rebuild a document, as read from a file.
struct DocumentState {
    /// A body that a feature makes has `madeBy` set and needs no shape. Any other has its
    /// shape in `shape`.
    std::vector<Body> bodies;
    std::vector<SketchItem> sketches;
    std::vector<Feature> features;
    /// The ids of bodies no feature makes, of sketches, and of features, in the order they are
    /// applied. Empty means: those bodies, then the sketches, then the features, each in the
    /// order given.
    std::vector<ObjectId> timeline;
    /// How many items of the timeline are applied. Larger than the timeline means all.
    std::size_t marker = static_cast<std::size_t>(-1);
    /// The id the next new object gets. Larger than every id in use.
    std::uint64_t nextId = 1;
};

/// What a feature would give if it were applied now. See Document::tryFeature().
struct FeatureTrial {
    /// The body the feature makes or changes, as it would be. Null if the feature fails.
    kernel::NamedShape result;
    /// For a pull or a revolve: the solid it makes, before that is combined with a body. Null
    /// for other features, and if the solid cannot be made.
    kernel::NamedShape tool;
    /// Why the feature fails, in a sentence fit to show, or empty.
    std::string error;
    /// For a construction plane: where it would be.
    std::optional<PlaneState> plane;
};

/// What an entry of the timeline is.
enum class ItemKind { Body, Sketch, Feature };

/// The model, and the single source of truth for it. The viewport and the interface observe a
/// document and ask it to change; they keep no model state of their own.
///
/// A document holds sketches, features, and bodies. The sketches and the features are what the
/// user made, in the order of the timeline. The bodies are what applying the features in that
/// order gives: the document works them out again whenever something they depend on changes,
/// and only as much as depends on the change. A feature that cannot be applied is left out,
/// keeps the reason, and the rest carries on from the model as it was before it.
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

    /// The sketches, in the order they were created.
    [[nodiscard]] const std::vector<SketchItem>& sketches() const { return m_sketches; }
    /// The sketch with this id, or nullptr. The pointer is valid until the next edit.
    [[nodiscard]] const SketchItem* findSketch(ObjectId id) const;
    /// `base` followed by the lowest number no sketch has yet: "Sketch 1", "Sketch 2".
    [[nodiscard]] std::string uniqueSketchName(std::string_view base) const;

    // Editing. Each throws std::invalid_argument for input that cannot be applied and leaves
    // the document unchanged.

    /// Adds a body and returns its id. The name must not be empty, and the shape must pass the
    /// kernel's validity check.
    ObjectId addBody(std::string name, kernel::Shape shape);
    /// Removes a body. For a body that features make or change, this removes those features.
    void removeBody(ObjectId id);
    void renameBody(ObjectId id, std::string name);
    void setBodyVisible(ObjectId id, bool visible);

    /// Adds an empty sketch on a plane and returns its id. A sketch can be attached to a
    /// construction plane (`attachedTo` a Plane feature, no face), or to a flat face of a body
    /// (`attachedTo` the body, `attachedFace` the face's name): it then follows it. Throws
    /// std::invalid_argument if what it is attached to does not exist.
    ObjectId addSketch(std::string name, const kernel::Plane& plane, ObjectId attachedTo = {},
                       std::string attachedFace = {});
    void removeSketch(ObjectId id);
    void renameSketch(ObjectId id, std::string name);
    void setSketchVisible(ObjectId id, bool visible);
    /// Replaces a sketch's geometry and constraints. `label` names the undo step, for example
    /// "Line" or "Horizontal".
    void setSketchGeometry(ObjectId id, sketch::Sketch geometry, std::string label);

    // Features and the timeline

    /// The features, in the order they were added. The order they are applied in is the
    /// timeline's.
    [[nodiscard]] const std::vector<Feature>& features() const { return m_features; }
    /// The feature with this id, or nullptr. The pointer is valid until the next edit.
    [[nodiscard]] const Feature* findFeature(ObjectId id) const;
    /// `base` followed by the lowest number no feature has yet: "Pull 1", "Pull 2".
    [[nodiscard]] std::string uniqueFeatureName(std::string_view base) const;

    /// Adds a feature where the history marker is, applies it, and returns its id. The id, an
    /// empty name, and for Operation::NewBody the body are filled in. Whether it could be
    /// applied is told by featureError(): a feature that fails is still added, so that it can
    /// be put right. Throws std::invalid_argument for a feature that names a sketch or a body
    /// the document does not have.
    ObjectId addFeature(Feature feature);
    /// Replaces what a feature does, keeping its id and its place.
    void changeFeature(ObjectId id, Feature definition);
    void removeFeature(ObjectId id);
    void renameFeature(ObjectId id, std::string name);
    void setFeatureSuppressed(ObjectId id, bool suppressed);
    /// Why a feature could not be applied, or an attached sketch could not follow what it is
    /// attached to, in a sentence fit to show. Empty if all is well, or the item is suppressed
    /// or lies beyond the history marker.
    [[nodiscard]] std::string featureError(ObjectId id) const;

    /// Bodies no feature makes, sketches, and features, in the order they are applied.
    [[nodiscard]] const std::vector<ObjectId>& timeline() const { return m_timeline; }
    /// What a timeline entry is. Throws std::invalid_argument for an id that is not in it.
    [[nodiscard]] ItemKind kindOf(ObjectId item) const;
    /// How many items of the timeline are applied: all of them, unless the marker was moved
    /// back to look at, or work on, an earlier state of the model.
    [[nodiscard]] std::size_t historyMarker() const;
    void setHistoryMarker(std::size_t applied);
    /// Moves an item to another place in the timeline. Throws std::invalid_argument, saying
    /// what stands in the way, if that would put a feature before something it depends on.
    void moveInTimeline(ObjectId item, std::size_t index);

    /// A body as it is now, with the names of its faces. A null shape if it does not exist.
    [[nodiscard]] kernel::NamedShape namedBody(ObjectId body) const;
    /// Where a construction plane is now. Nothing if it does not exist at this point in the
    /// timeline, or cannot be placed.
    [[nodiscard]] std::optional<PlaneState> planeState(ObjectId plane) const;

    /// What a feature would make of the model as it is now: at the history marker, which is
    /// where addFeature() puts it, or, while a feature is being edited, just before that one.
    /// Changes nothing. For previewing a feature while its values are being chosen.
    [[nodiscard]] FeatureTrial tryFeature(Feature feature) const;

    /// Shows the model as it is just before a feature, for as long as that feature is being
    /// edited: what the feature refers to can be seen and picked there. Not an edit: it is not
    /// saved and not undone. An invalid id shows the whole model again.
    void setEditedFeature(ObjectId feature);
    [[nodiscard]] ObjectId editedFeature() const { return m_edited; }

    /// While rebuilding is suspended, edits are taken but the bodies are left as they are.
    /// Resuming rebuilds once. For a run of edits whose in-between states nobody looks at,
    /// such as a sketch being drawn that features depend on. Calls nest.
    void suspendRebuild();
    void resumeRebuild();

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
        enum class Kind {
            AddBody,
            RemoveBody,
            RenameBody,
            SetBodyVisible,
            AddSketch,
            RemoveSketch,
            RenameSketch,
            SetSketchVisible,
            SetSketchGeometry,
            AddFeature,
            RemoveFeature,
            ChangeFeature,
            MoveItem,
            SetMarker,
        };
        Kind kind;
        ObjectId id;
        // AddBody and RemoveBody: the body, and where in the list it sits.
        Body body;
        std::size_t index = 0;
        // RenameBody
        std::string oldName;
        std::string newName;
        // SetBodyVisible, SetSketchVisible
        bool oldVisible = true;
        bool newVisible = true;
        // AddSketch and RemoveSketch: the sketch. SetSketchGeometry: its geometry before.
        SketchItem sketch;
        // SetSketchGeometry: its geometry after.
        sketch::Sketch newGeometry;
        // AddFeature and RemoveFeature: the feature; `hasBody` if `body` is the body it makes.
        // ChangeFeature: the feature before, and after.
        Feature feature;
        Feature newFeature;
        bool hasBody = false;
        // ChangeFeature: +1 if the change makes the feature make `body`, -1 if it stops it.
        int bodyChange = 0;
        // Where in the timeline: for whatever is added or removed, and for MoveItem from
        // where to where.
        std::size_t place = 0;
        std::size_t newPlace = 0;
        // The history marker before and after, for the edits that move it.
        std::size_t oldMarker = 0;
        std::size_t newMarker = 0;
    };

    struct Step {
        std::string label;
        std::vector<Edit> edits;
        // Identifies the state the document is in after this step. Never reused.
        std::uint64_t serial = 0;
    };

    void record(Edit edit, const std::string& label);
    void apply(const Edit& edit);
    void revert(const Edit& edit);
    void notify(Change::Kind kind, ObjectId body = {});
    [[nodiscard]] std::size_t indexOf(ObjectId id) const;
    [[nodiscard]] std::size_t sketchIndexOf(ObjectId id) const;
    [[nodiscard]] std::size_t featureIndexOf(ObjectId id) const;
    [[nodiscard]] std::size_t placeOf(ObjectId item) const;

    // The timeline
    void insertItem(ObjectId item, std::size_t place);
    void eraseItem(ObjectId item);
    /// Where a new item goes: where the marker is.
    [[nodiscard]] std::size_t placeForNew() const;
    /// Fills in the marker before and after for an edit that adds an item at `place`.
    void markersForAdding(Edit& edit) const;
    void markersForRemoving(Edit& edit, std::size_t place) const;
    /// Why the timeline in this order would not work, or an empty string.
    [[nodiscard]] std::string orderProblem(const std::vector<ObjectId>& order) const;

    // Rebuilding
    /// Notes that an item changed, so that what depends on it is built again.
    void touch(ObjectId item);
    /// Applies the features again, as far as anything changed, and tells the listeners.
    void rebuild();
    [[nodiscard]] std::uint64_t currentSerial() const;

    void beginTransaction(std::string label);
    void commitTransaction();
    void rollbackTransaction();

    std::vector<Body> m_bodies;
    std::vector<SketchItem> m_sketches;
    std::vector<Feature> m_features;
    std::vector<ObjectId> m_timeline;
    /// How many timeline items are applied; larger than the timeline means all.
    std::size_t m_marker = static_cast<std::size_t>(-1);
    std::uint64_t m_nextId = 1;

    /// What applying a feature gave, kept so that it need not be worked out again while
    /// nothing it depends on has changed.
    struct Built {
        bool done = false;    ///< whether what follows is up to date
        bool applied = false; ///< whether the feature took effect
        kernel::NamedShape result;
        std::string error;
    };
    std::unordered_map<ObjectId, Built> m_built;
    std::unordered_map<ObjectId, PlaneState> m_planes;
    std::unordered_set<ObjectId> m_touched;
    bool m_rebuildAll = false;
    int m_suspended = 0;
    ObjectId m_edited;

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
