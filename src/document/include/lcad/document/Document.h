// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <lcad/document/Assembly.h>
#include <lcad/document/Drawing.h>
#include <lcad/document/Feature.h>
#include <lcad/document/Machining.h>
#include <lcad/document/ObjectId.h>
#include <lcad/kernel/Modeling.h>
#include <lcad/kernel/Plane.h>
#include <lcad/kernel/Shape.h>
#include <lcad/sketch/Sketch.h>

#include <cstdint>
#include <functional>
#include <map>
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
    /// Solids that do not touch are bodies of their own. Where a feature leaves several, the body
    /// it works on holds the first, and this says which of the others this body holds, from 1.
    /// `madeBy` is then that feature. Zero for every other body.
    int piece = 0;
    /// Deleted, though the feature behind it stays for the sake of the other solids it leaves.
    /// The body has no shape, and it is not made again.
    bool discarded = false;
    /// A name for each face of `shape`; see kernel::NamedShape.
    std::vector<std::string> faceNames;
    /// The component it is part of. Not valid for the document's root.
    ObjectId component;
    /// The name of what it is made of (see Document::findMaterial()), or empty when not said.
    std::string material;
    /// For a body that no feature makes: what the names of its faces start with. Empty means
    /// its id. A copy keeps its original's, as a copied feature does.
    std::string tag;
    /// For a body of a linked component: which body of the linked document it is (see
    /// LinkedBody::key). Such a body is not in the timeline, and no feature works on it.
    std::string linkKey;
    /// For a body of a linked component: the names its faces have in the linked document.
    std::vector<std::string> sourceFaceNames;
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
    /// Dimensions whose values are worked out from parameters, and the expressions they are
    /// worked out from. The values in `geometry` are what they last came to.
    std::map<sketch::ConstraintId, std::string> expressions;
    /// The component it is part of; its plane is in that component's coordinates. Not valid for
    /// the document's root.
    ObjectId component;
};

/// What a construction axis is now: the line, and where on it to draw it.
struct AxisState {
    kernel::Axis axis;
    /// The middle of what it was placed along, dropped onto the line: where it is drawn.
    kernel::Point center;
    /// How long to draw it, in millimeters.
    double length = 100.0;
};

/// A value the user names, which the values of features and the dimensions of sketches can be
/// worked out from: "width" is "60 mm", "hole" is "width / 10".
struct Parameter {
    ObjectId id;
    std::string name;
    /// As typed: a number, with a unit if wanted, or an expression of other parameters.
    std::string expression;
    std::string comment;

    friend bool operator==(const Parameter&, const Parameter&) = default;
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
        /// A parameter was added, removed, or changed, or what one comes to changed.
        ParametersChanged,
        /// What undo and redo would do, or whether there are unsaved changes, may have changed.
        HistoryChanged,
        /// A component, an occurrence, or a joint was added, removed, renamed, shown or hidden,
        /// grounded, suppressed, or changed, or a joint now fails or holds again.
        AssemblyChanged,
        /// Where occurrences are, in the root's coordinates, changed.
        PlacementsChanged,
        /// The document's materials, or what a body (`body`) is made of, changed.
        MaterialsChanged,
        /// A drawing (`body` is its id) was added, removed, or changed.
        DrawingsChanged,
        /// A setup or a tool (`body` is its id) was added, removed, or changed.
        MachiningChanged,
    };
    Kind kind = Kind::HistoryChanged;
    /// The body or sketch concerned. Not valid for HistoryChanged, TimelineChanged, and the
    /// changes to the assembly.
    ObjectId body;
};

/// Everything needed to rebuild a document, as read from a file.
struct DocumentState {
    /// A body that a feature makes has `madeBy` set and needs no shape. Any other has its
    /// shape in `shape`.
    std::vector<Body> bodies;
    std::vector<SketchItem> sketches;
    std::vector<Feature> features;
    std::vector<Parameter> parameters;
    /// The ids of bodies no feature makes, of sketches, and of features, in the order they are
    /// applied. Empty means: those bodies, then the sketches, then the features, each in the
    /// order given.
    std::vector<ObjectId> timeline;
    /// How many items of the timeline are applied. Larger than the timeline means all.
    std::size_t marker = static_cast<std::size_t>(-1);
    /// The id the next new object gets. Larger than every id in use.
    std::uint64_t nextId = 1;
    std::vector<Component> components;
    std::vector<Occurrence> occurrences;
    std::vector<Joint> joints;
    /// The materials the document defines for itself, besides LCAD's own.
    std::vector<Material> materials;
    std::vector<Drawing> drawings;
    std::vector<Tool> tools;
    std::vector<Setup> setups;
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
    /// The user stopped it before it finished. There is no result, and it has not failed: the
    /// feature may well work, given the time.
    bool stopped = false;
    /// For a construction plane: where it would be.
    std::optional<PlaneState> plane;
    /// For a construction axis: where it would be.
    std::optional<AxisState> axis;
};

/// What a joint would give if it were added or changed now. See Document::tryJoint().
struct JointTrial {
    /// Where everything would be.
    std::vector<Instance> instances;
    /// Why it cannot hold, in a sentence fit to show, or empty.
    std::string error;
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
    /// How the interface wants long work done, so that it can stay alive and offer to stop it. It
    /// is given what the work is called, and returns when the work has been done. Anything the
    /// work throws comes out of it the same way. Not set, work is simply done where it is asked
    /// for, which is what tests of the document do. Set it once, at the start.
    using WorkRunner =
        std::function<void(const std::string& name, const std::function<void()>& work)>;
    static void setWorkRunner(WorkRunner runner);

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

    /// Adds a body to a component (the root for an invalid id) and returns its id. The name
    /// must not be empty, and the shape must have faces. It need not be valid geometry: a body
    /// read from another program may not be, and a Repair feature can be applied to it.
    ObjectId addBody(std::string name, kernel::Shape shape, ObjectId component = {});
    /// Removes a body. For a body that features make or change, this removes those features.
    void removeBody(ObjectId id);
    void renameBody(ObjectId id, std::string name);
    void setBodyVisible(ObjectId id, bool visible);

    /// Adds an empty sketch on a plane and returns its id. A sketch can be attached to a
    /// construction plane (`attachedTo` a Plane feature, no face), or to a flat face of a body
    /// (`attachedTo` the body, `attachedFace` the face's name): it then follows it, and is part
    /// of the component that is. Otherwise it is part of `component`, in whose coordinates its
    /// plane is. Throws std::invalid_argument if what it is attached to does not exist.
    ObjectId addSketch(std::string name, const kernel::Plane& plane, ObjectId attachedTo = {},
                       std::string attachedFace = {}, ObjectId component = {});
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
    /// Builds a feature again that stopped, or failed for a reason that may have gone.
    void buildAgain(ObjectId id);
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
    /// Where a construction axis is now. Nothing if it does not exist at this point in the
    /// timeline, or cannot be placed.
    [[nodiscard]] std::optional<AxisState> axisState(ObjectId axis) const;

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

    // Parameters

    [[nodiscard]] const std::vector<Parameter>& parameters() const { return m_parameters; }
    [[nodiscard]] const Parameter* findParameter(ObjectId id) const;
    [[nodiscard]] const Parameter* findParameter(std::string_view name) const;
    /// `base` followed by the lowest number no parameter has yet: "width1", "width2".
    [[nodiscard]] std::string uniqueParameterName(std::string_view base) const;
    /// Adds a parameter. Throws std::invalid_argument if the name cannot be a parameter's (a
    /// letter or "_", then letters, digits, and "_"; not a function or a unit) or is taken.
    ObjectId addParameter(std::string name, std::string expression, std::string comment = {});
    /// Changes a parameter. A new name takes the old one's place in every expression that uses
    /// it, in the same undo step. Throws std::invalid_argument as addParameter() does.
    void changeParameter(ObjectId id, Parameter definition);
    void removeParameter(ObjectId id);
    /// What a parameter comes to, in millimeters or degrees, or nothing if it cannot be worked
    /// out, in which case parameterError() says why.
    [[nodiscard]] std::optional<double> parameterValue(ObjectId id) const;
    [[nodiscard]] std::string parameterError(ObjectId id) const;
    /// Works out an expression from the parameters as they are. Throws std::invalid_argument
    /// with a sentence that says why, if it cannot be.
    [[nodiscard]] double evaluate(std::string_view expression) const;
    /// Makes a dimension of a sketch follow an expression, or, given an empty one, a plain
    /// value again. The dimension takes the expression's value now. Throws
    /// std::invalid_argument if there is no such dimension, or the expression cannot be worked
    /// out.
    void setDimensionExpression(ObjectId sketch, sketch::ConstraintId dimension,
                                std::string expression);

    // Components. See Assembly.h for what they are.

    [[nodiscard]] const std::vector<Component>& components() const { return m_components; }
    /// The component with this id, or nullptr. The pointer is valid until the next edit.
    [[nodiscard]] const Component* findComponent(ObjectId id) const;
    [[nodiscard]] const std::vector<Occurrence>& occurrences() const { return m_occurrences; }
    [[nodiscard]] const Occurrence* findOccurrence(ObjectId id) const;
    /// The occurrences of a component, in the order they were made.
    [[nodiscard]] std::vector<ObjectId> occurrencesOf(ObjectId component) const;
    /// The occurrences directly inside a component, or inside the root for an invalid id.
    [[nodiscard]] std::vector<ObjectId> occurrencesIn(ObjectId component) const;
    /// What an occurrence is called: its component's name and which of its occurrences it is,
    /// "Bracket:2".
    [[nodiscard]] std::string occurrenceName(ObjectId occurrence) const;
    /// The component a body, a sketch, or a feature is part of: not valid for the root.
    [[nodiscard]] ObjectId componentOf(ObjectId item) const;
    /// `base` followed by the lowest number no component has yet: "Component 1".
    [[nodiscard]] std::string uniqueComponentName(std::string_view base) const;

    /// Makes an empty component, with one occurrence of it in `parent` (the root for an invalid
    /// id) at `placement`, and returns the component's id.
    ObjectId addComponent(std::string name, ObjectId parent = {},
                          const kernel::Placement& placement = {});
    /// Another occurrence of a component in `parent`, and returns its id: the same component,
    /// appearing again. Throws std::invalid_argument if that would put a component inside
    /// itself.
    ObjectId addOccurrence(ObjectId component, ObjectId parent, const kernel::Placement& placement);
    /// Removes an occurrence, and the joints that hold it. The last occurrence of a component
    /// takes the component with it, and everything in it, in the same step.
    void removeOccurrence(ObjectId occurrence);
    void renameComponent(ObjectId component, std::string name);
    /// Puts an occurrence somewhere in its parent by hand. Where joints hold it, they still do;
    /// this is where it goes back to when they are deleted.
    void setOccurrencePlacement(ObjectId occurrence, const kernel::Placement& placement);
    void setOccurrenceGrounded(ObjectId occurrence, bool grounded);
    void setOccurrenceVisible(ObjectId occurrence, bool visible);
    void setOccurrenceSuppressed(ObjectId occurrence, bool suppressed);
    /// Moves an occurrence into another component (the root for an invalid id), keeping where it
    /// is in the root's coordinates, as seen through the first occurrence of each. Throws
    /// std::invalid_argument if that would put a component inside itself, or a joint holds it.
    void moveOccurrence(ObjectId occurrence, ObjectId parent);
    /// Makes a component of bodies of one component: they move into a new one, together with
    /// the features that make and change them and the sketches and construction geometry those
    /// use, and an occurrence of it takes their place, where they were. Returns the new
    /// component. Throws std::invalid_argument, saying what stands in the way, if something
    /// that has to move is used by something that stays.
    ObjectId makeComponent(std::string name, const std::vector<ObjectId>& bodies);
    /// Makes an independent copy of an occurrence's component: a new component with a copy of
    /// each of its bodies, sketches, features, and occurrences, which is changed on its own from
    /// now on. The copy's occurrence goes where the original's is, and its id is returned.
    ObjectId copyComponent(ObjectId occurrence);

    // Linked components

    /// Adds a component that is another document: `content` is what that document holds now,
    /// `link` where it is. One occurrence of it goes into `parent`. Returns the component.
    ObjectId addLinkedComponent(std::string name, std::string link, LinkedContent content,
                                ObjectId parent = {}, const kernel::Placement& placement = {});
    /// Takes the content of a linked document as it is now. Bodies keep their ids, by key, so
    /// that joints on them still hold.
    void updateLinkedComponent(ObjectId component, LinkedContent content);
    /// Where a linked component's document is to be found from now on.
    void setComponentLink(ObjectId component, std::string link);

    // Joints

    [[nodiscard]] const std::vector<Joint>& joints() const { return m_joints; }
    [[nodiscard]] const Joint* findJoint(ObjectId id) const;
    /// `base` followed by the lowest number no joint has yet: "Revolute 1".
    [[nodiscard]] std::string uniqueJointName(std::string_view base) const;
    /// Adds a joint and returns its id; an empty name is filled in. Whether it holds is told by
    /// jointError(): a joint that cannot hold is still added, to be put right. Throws
    /// std::invalid_argument for a joint whose sides do not lie in its component.
    ObjectId addJoint(Joint joint);
    void changeJoint(ObjectId id, Joint definition);
    void removeJoint(ObjectId id);
    /// Why a joint does not hold, in a sentence fit to show, or empty.
    [[nodiscard]] std::string jointError(ObjectId id) const;

    // Where things are

    /// Every occurrence that is not suppressed, put together from the root down, the root
    /// first. See Instance.
    [[nodiscard]] const std::vector<Instance>& instances() const { return m_instances; }
    /// Where an occurrence is in its parent now: where joints hold it, or where it was put.
    [[nodiscard]] kernel::Placement placementOf(ObjectId occurrence) const;
    /// Where the end of a path of occurrences (see Instance::path) is in the root's
    /// coordinates.
    [[nodiscard]] kernel::Placement placementOf(const std::vector<ObjectId>& path) const;
    /// Where everything would be if these joints had moved to these values, the rest as they
    /// are. Changes nothing: for showing joints in motion.
    [[nodiscard]] std::vector<Instance>
    instancesWith(const std::map<ObjectId, std::array<double, 3>>& values) const;
    /// What adding a joint, or changing one (one with the id of a joint the document has),
    /// would give. Changes nothing: for previewing a joint while it is being set up.
    [[nodiscard]] JointTrial tryJoint(const Joint& joint) const;

    // Materials

    /// The materials this document defines, besides those of materialLibrary().
    [[nodiscard]] const std::vector<Material>& materials() const { return m_materials; }
    /// The material of that name: the document's own first, then LCAD's.
    [[nodiscard]] std::optional<Material> findMaterial(std::string_view name) const;
    /// What a body is made of; an empty name for not said. Throws std::invalid_argument for a
    /// material findMaterial() does not know.
    void setBodyMaterial(ObjectId body, std::string material);
    /// Adds a material of the document's own. Throws std::invalid_argument for an empty or
    /// taken name, or a density that is not more than zero.
    void addMaterial(Material material);
    /// Changes one of the document's own materials. A new name is taken over by the bodies
    /// made of it.
    void changeMaterial(const std::string& name, Material material);
    /// Removes one of the document's own materials. Bodies made of it are then not said to be
    /// made of anything.
    void removeMaterial(const std::string& name);

    // What is made from the model. Each change is one undo step. What these hold refers to the
    // model by names that outlast changes to it; what they lead to is worked out by the drawing
    // and cam modules.

    /// An id no other object of the document has, for something inside a drawing or a setup.
    ObjectId newId();

    [[nodiscard]] const std::vector<Drawing>& drawings() const { return m_drawings; }
    /// The drawing with this id, or nullptr. The pointer is valid until the next edit.
    [[nodiscard]] const Drawing* findDrawing(ObjectId id) const;
    /// `base` followed by the lowest number no drawing has yet: "Drawing 1".
    [[nodiscard]] std::string uniqueDrawingName(std::string_view base) const;
    /// Adds a drawing, and gives it its id. Throws std::invalid_argument without a name, or for a
    /// view without a scale above zero.
    ObjectId addDrawing(Drawing drawing);
    /// Replaces the drawing with the same id.
    void changeDrawing(const Drawing& drawing, const std::string& label = "Change Drawing");
    void removeDrawing(ObjectId id);

    [[nodiscard]] const std::vector<Tool>& tools() const { return m_tools; }
    /// The tool with this id, or nullptr. The pointer is valid until the next edit.
    [[nodiscard]] const Tool* findTool(ObjectId id) const;
    /// Adds a tool, and gives it its id. Throws std::invalid_argument without a name, or
    /// without a diameter above zero.
    ObjectId addTool(Tool tool);
    /// Replaces the tool with the same id.
    void changeTool(const Tool& tool);
    /// Throws std::invalid_argument if an operation uses it.
    void removeTool(ObjectId id);

    [[nodiscard]] const std::vector<Setup>& setups() const { return m_setups; }
    /// The setup with this id, or nullptr. The pointer is valid until the next edit.
    [[nodiscard]] const Setup* findSetup(ObjectId id) const;
    /// `base` followed by the lowest number no setup has yet: "Setup 1".
    [[nodiscard]] std::string uniqueSetupName(std::string_view base) const;
    /// Adds a setup, and gives it its id. Throws std::invalid_argument without a name.
    ObjectId addSetup(Setup setup);
    /// Replaces the setup with the same id.
    void changeSetup(const Setup& setup, const std::string& label = "Change Setup");
    void removeSetup(ObjectId id);

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
    /// Records that the current state is not what is on disk: for a document restored from a
    /// copy of unsaved work.
    void markModified();

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
            AddParameter,
            RemoveParameter,
            ChangeParameter,
            SetSketchExpressions,
            // Components, occurrences, joints, materials
            AddComponent,
            RemoveComponent,
            ChangeComponent,
            AddOccurrence,
            RemoveOccurrence,
            ChangeOccurrence,
            AddJoint,
            RemoveJoint,
            ChangeJoint,
            /// A body's persistent fields: its component, material, linked shape, name.
            ChangeBody,
            /// A sketch's component.
            ChangeSketchComponent,
            AddMaterial,
            RemoveMaterial,
            ChangeMaterial,
            // Drawings, tools, setups
            AddDrawing,
            RemoveDrawing,
            ChangeDrawing,
            AddTool,
            RemoveTool,
            ChangeTool,
            AddSetup,
            RemoveSetup,
            ChangeSetup,
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
        // AddParameter and RemoveParameter: the parameter. ChangeParameter: before and after,
        // and what a new name changed in features and sketches.
        Parameter parameter;
        Parameter newParameter;
        std::vector<std::pair<Feature, Feature>> renamedFeatures;
        std::vector<std::pair<Parameter, Parameter>> renamedParameters;
        // SetSketchExpressions, and the sketches a new name changed: the sketch, and its
        // expressions before and after.
        struct SketchExpressions {
            ObjectId sketch;
            std::map<sketch::ConstraintId, std::string> before;
            std::map<sketch::ConstraintId, std::string> after;
        };
        std::vector<SketchExpressions> renamedSketches;
        // The components, occurrences, joints, and materials added, removed, or changed: before
        // and after. ChangeBody: `body` before and `newBody` after. ChangeSketchComponent: the
        // component before and after.
        Component component;
        Component newComponent;
        Occurrence occurrence;
        Occurrence newOccurrence;
        Joint joint;
        Joint newJoint;
        Body newBody;
        ObjectId oldComponent;
        ObjectId newComponentId;
        Material material;
        Material newMaterial;
        // The drawings, tools, and setups added, removed, or changed: before and after.
        Drawing drawing;
        Drawing newDrawing;
        Tool tool;
        Tool newTool;
        Setup setup;
        Setup newSetup;
    };

    struct Step {
        std::string label;
        std::vector<Edit> edits;
        // Identifies the state the document is in after this step. Never reused.
        std::uint64_t serial = 0;
    };

    void record(Edit edit, const std::string& label);
    /// Runs a step of long work the way the interface wants it run.
    static void runWork(const std::string& name, const std::function<void()>& step);
    /// Makes a body for each solid that has none, as part of the step that split them.
    void settlePieces();
    /// The bodies that hold the solids a feature leaves past the first, in order.
    [[nodiscard]] std::vector<ObjectId> piecesOf(ObjectId feature) const;
    void apply(const Edit& edit);
    void revert(const Edit& edit);
    void applyExpressions(const Edit& edit, bool forward);
    void notify(Change::Kind kind, ObjectId body = {});
    [[nodiscard]] std::size_t indexOf(ObjectId id) const;
    [[nodiscard]] std::size_t sketchIndexOf(ObjectId id) const;
    [[nodiscard]] std::size_t featureIndexOf(ObjectId id) const;
    [[nodiscard]] std::size_t placeOf(ObjectId item) const;
    [[nodiscard]] std::size_t componentIndexOf(ObjectId id) const;
    [[nodiscard]] std::size_t occurrenceIndexOf(ObjectId id) const;
    [[nodiscard]] std::size_t jointIndexOf(ObjectId id) const;
    [[nodiscard]] std::size_t materialIndexOf(const std::string& name) const;
    /// Throws std::invalid_argument, naming `function`, unless `component` is the root or a
    /// component of the document that is not linked.
    void requireOwnComponent(ObjectId component, const char* function) const;
    /// The component a feature is part of: that of the body it works on, or of its sketch, or
    /// of the construction geometry it is placed on, or else the one it names.
    [[nodiscard]] ObjectId componentForFeature(const Feature& feature) const;
    /// Why a feature cannot use what it uses, all of which has to be part of its component and
    /// made in this document, or empty.
    [[nodiscard]] std::string componentProblem(const Feature& feature) const;
    /// Whether `inner` is `outer` or appears somewhere inside it.
    [[nodiscard]] bool contains(ObjectId outer, ObjectId inner) const;
    /// Why a joint's sides do not lie in its component, or empty.
    [[nodiscard]] std::string jointProblem(const Joint& joint) const;
    /// Makes the edits one undo step named `label`, or, inside an open transaction, part of it.
    void inOneStep(const std::string& label, const std::function<void()>& edits);
    /// Removes a component that has no occurrences left, with everything in it.
    void removeComponentContents(ObjectId component);
    void applyAssembly(const Edit& edit, bool forward);
    void applyOutput(const Edit& edit, bool forward);

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
    /// Works out every parameter, in the order they depend on each other.
    void computeParameters();
    /// A feature with the values its expressions come to. Throws detail::FeatureFailure.
    [[nodiscard]] Feature resolved(const Feature& feature) const;
    [[nodiscard]] double evaluateForModel(const std::string& expression) const;
    /// Applies the features again, as far as anything changed, and tells the listeners.
    void rebuild();
    /// Places the occurrences by their joints, and tells the listeners what moved.
    void solveAssembly();
    [[nodiscard]] std::uint64_t currentSerial() const;

    void beginTransaction(std::string label);
    void commitTransaction();
    void rollbackTransaction();

    std::vector<Body> m_bodies;
    std::vector<SketchItem> m_sketches;
    std::vector<Feature> m_features;
    std::vector<Parameter> m_parameters;
    std::vector<Component> m_components;
    std::vector<Occurrence> m_occurrences;
    std::vector<Joint> m_joints;
    std::vector<Material> m_materials;
    std::vector<Drawing> m_drawings;
    std::vector<Tool> m_tools;
    std::vector<Setup> m_setups;
    /// Where each occurrence is in its parent, as the joints last placed it, and why a joint
    /// does not hold.
    std::unordered_map<ObjectId, kernel::Placement> m_solved;
    std::unordered_map<ObjectId, std::string> m_jointErrors;
    std::vector<Instance> m_instances{Instance{}};
    /// Whether the occurrences have to be placed again, and whether the listeners are to hear
    /// that the assembly changed once they are.
    bool m_resolve = true;
    bool m_assemblyChanged = false;
    /// What each parameter comes to, or why it does not.
    std::unordered_map<ObjectId, double> m_parameterValues;
    std::unordered_map<ObjectId, std::string> m_parameterErrors;
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
        /// `result` a solid at a time, the first for the body the feature works on.
        std::vector<kernel::NamedShape> solids;
        std::string error;
    };
    std::unordered_map<ObjectId, Built> m_built;
    std::unordered_map<ObjectId, PlaneState> m_planes;
    std::unordered_map<ObjectId, AxisState> m_axes;
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
