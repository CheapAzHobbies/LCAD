# LCAD roadmap

Phases follow section 30 of the [project brief](docs/PROJECT_BRIEF.md). The interface is built to the
[GUI specification](docs/GUI_SPEC.md). Each phase ends with working
software and an acceptance test, not a mockup. A phase is done when its acceptance test passes in an
automated or scripted run.

Each phase is also a GitHub milestone with the same due date. The dates are estimates, revised at
the end of every phase based on how long that phase actually took.

**Revised on 2026-10-12.** The first plan gave Phase 0 four weeks and Phase 1 twelve. Both were
finished on the day they were started, so the dates below were pulled in. They are not pulled in
by the same factor: the phases ahead contain the two problems that stall CAD projects (the sketch
solver, and references to faces and edges that survive edits), and from here on each phase needs
its owner to use what was built and say what is wrong with it before the next one starts. That,
more than writing the code, now sets the pace.

## Timeline

| Phase | What ships | Start | Target | Finished | Release |
|---|---|---|---|---|---|
| 0. Groundwork | Architecture, dependency and license decisions, build system, CI | 2026-10-12 | 2026-11-08 | 2026-10-12 | |
| 1. Desktop foundation | Window, viewport, selection, documents, save/open, undo | 2026-10-12 | 2027-01-31 | 2026-10-12 | |
| 1b. Interface shell | Design system, themes, command registry and search, tabs, layout, viewport controls | 2026-10-12 | 2026-10-25 | 2026-10-12 | |
| 2. Sketcher | Sketch planes, drawing tools, constraints, solver | 2026-10-12 | 2026-11-30 | 2026-10-13 | |
| 3. Parametric solids | Extrude, cut, revolve, fillet, patterns, timeline, parameters | 2026-10-13 | 2027-02-15 | 2026-10-14 | v0.1 alpha |
| 4. Direct modeling and interchange | STEP/IGES import, STL/3MF export, face edits, healing | 2026-10-14 | 2027-03-31 | 2026-10-19 | v0.2 |
| 5. Components and assemblies | Components, linked parts, joints, interference checks | 2026-10-19 | 2027-07-15 | 2026-10-21 | v0.5 beta |
| Port | Windows and macOS builds | 2026-10-21 | 2027-09-15 | 2026-10-30 | v0.6 |
| 6. Drawings and manufacturing | Associative drawings, PDF/DXF, 2.5D CAM | 2026-10-31 | 2028-01-15 | in progress | v1.0 |
| 7. Advanced capabilities | Surfaces, sheet metal, mesh repair, rendering, plugins | 2028-01-16 | ongoing | | |

The targets of Phases 0 and 1 are the original ones, kept for the record.

**Revised on 2026-10-13.** Phase 2 was finished in two days, with ellipse and spline carried
over. The targets of the later phases are left as they are: Phase 3 holds the problem this plan
is most worried about (references to faces and edges that survive edits), and a sketcher that
has been tested by its author is not yet a sketcher that has been used. Phase 3 starts when the
owner has drawn with this one and says so.

v0.5 is the first release that meets the acceptance criteria in section 32 of the brief. The repository
stays private until the owner decides to open it; v0.1 is the earliest point where there is something
worth showing.

## Phase 0: Groundwork (finished 2026-10-12)

Decide before writing application code.

- [x] License: GPL-3.0-or-later. What it allows, and what the alternatives would have, is in
      [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md#lcads-own-license).
- [x] Dependency assessment: OCCT, Qt 6, sketch solver candidates, file format libraries, with license,
      maintenance status, and distro versions for each. See [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md).
- [x] Architecture document: module boundaries and the rules between them. See
      [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
- [x] Viewport approach: OCCT's own viewer inside a Qt widget, proven by a working viewport with orbit,
      pan, zoom, and hover highlighting.
- [x] Native file format and its versioning: a zip container of JSON and B-rep. See
      [docs/FILE_FORMAT.md](docs/FILE_FORMAT.md).
- [x] CMake project that builds an LCAD window linked against OCCT on Ubuntu 24.04, Ubuntu 26.04,
      Fedora 44, and Arch. See [docs/BUILDING.md](docs/BUILDING.md).
- [x] CI: builds and runs the tests on those four distributions, plus a sanitizer build, on every pull
      request and push to `main`.
- [x] Test strategy: Catch2 and Qt Test, geometry assertions with named tolerances, the plan for
      reference models. See [docs/TESTING.md](docs/TESTING.md).

Done when: a clean checkout builds and runs an empty window on the three distro families, CI is green, and
the decisions above are written down.

## Phase 1: Desktop foundation (finished 2026-10-12)

- [x] Main window with the five regions from the brief: top bar, toolbar, browser, viewport, and
      timeline. The timeline is there and empty until features exist.
- [x] 3D viewport: perspective and orthographic projection, view cube, standard views, fit, and
      Fusion's mouse mapping.
- [x] Face, edge, vertex, and body selection, with a filter, rectangle selection, and a status
      bar that measures what is picked.
- [x] Document model, with the bodies as a tree in the browser: rename, show, hide, delete.
- [x] Primitive solids: box, cylinder, sphere, cone, and torus, each created from a panel with a
      live preview. Fusion's coil and pipe are swept shapes and come with sweeps.
- [x] Native project format, save and open, atomic writes, built to
      [docs/FILE_FORMAT.md](docs/FILE_FORMAT.md). A file saved with OCCT 7.6 opens with 7.9 and
      the other way round, tested on every CI run.
- [x] Undo and redo through a transaction system.
- [x] The kernel's own error type, and OCCT's diagnostic messages routed into LCAD's log.
- [x] AppImage built by CI, and started there on a distribution it was not built on.

Acceptance: create a document, add a solid, save, close, reopen, and verify the geometry is
identical. Automated twice, against the document module and through the interface; see
[docs/TESTING.md](docs/TESTING.md). How to use what exists is in [docs/USING.md](docs/USING.md).

Left out on purpose, with where each belongs: autosave and crash recovery (Phase 5, where the
brief's release criteria need them), inches as a display unit, and per-body colors. Tabs and
command search are in Phase 1b.

## Phase 1b: Interface shell (finished 2026-10-12)

Added on 2026-10-12, when the owner's [GUI specification](docs/GUI_SPEC.md) arrived. It asks for
the application shell and design system to be built first, and for commands to be connected to
them as they become real. [docs/INTERFACE_STATUS.md](docs/INTERFACE_STATUS.md) compares the
interface with the specification section by section.

- [x] Design system: colors and sizes defined once as tokens; light and dark themes built from
      them, following the system by default, with the choice remembered.
- [x] LCAD's own icons for its tools, in one consistent style, and a document icon.
- [x] Command registry: each command defined once, with its name, aliases, shortcuts, icon, and
      when it applies, and used by the toolbar, menus, context menus, shortcuts, and search alike.
- [x] Command search.
- [x] Properties panel: what is selected, when no command is running.
- [x] Context menu in the 3D view, driven by the selection.
- [x] Several documents in tabs, and a list of recent files.
- [x] Layout: remembered between launches, a command to reset it, a timeline that can be hidden,
      and a mode that gives the whole window to the 3D view.
- [x] 3D view: reference grid, display styles (shaded, with edges, wireframe, hidden line,
      transparent), zoom to selection, choosing among overlapping objects, a second navigation
      preset. The view cube can be dragged to orbit.
- [x] Browser: isolate, show all, filter by name, origin planes and axes.
- [x] Status bar: brief messages such as "Saved", and the active command.
- [x] Shortcuts that can be changed.
- [x] Interface tests at three window sizes and two display scales, in both themes.

Acceptance: every command can be started from a menu and from the search through one definition,
which the toolbar buttons and shortcuts use too; the window comes back as it was left, in the
theme that was chosen; and the interface tests pass in both themes, at 1280 by 800 and at twice
the display scale. All three are automated in `tests/gui/InterfaceTest.cpp`.

Moved to later phases, with the reason beside each in
[docs/INTERFACE_STATUS.md](docs/INTERFACE_STATUS.md): controls mounted on the edges of the 3D
view, density modes, a progress display, and a scale setting of LCAD's own.

## Phase 2: Sketcher (finished 2026-10-13, except ellipse and spline)

- [x] Sketch on origin planes and planar faces, with automatic camera orientation.
- [x] Line, rectangle, circle, arc, point, construction geometry; polygon and slot.
- [ ] Ellipse and spline. Not done. Each needs a new kind of curve in the sketch, the solver, the
      file format, and the outline finder, which is more than a drawing tool. Carried into Phase 3.
- [x] Geometric constraints: coincident, horizontal, vertical, parallel, perpendicular, tangent, equal,
      concentric, midpoint, symmetry, collinear, fix.
- [x] Dimensional constraints: distance (straight across, horizontal, or vertical), length, radius,
      diameter, angle, typed and changed in the 3D view.
- [x] A solver with under-, fully, and overconstrained states and conflict reporting.
- [x] Dragging underconstrained geometry.
- [x] Inference while drawing (ends on existing points, curves, and midpoints; level and upright
      lines), with an off switch. Tangency is not inferred: it is a constraint to add.
- [x] Trim, extend, offset, mirror, project.
- [x] Closed profile detection for later features.

Acceptance: create a fully constrained rectangle, change its dimensions, and verify the geometry
updates. Met, and tested at three levels: in the sketch module, in the 3D view with the pointer
and the keyboard, and through the whole window including save, close, and reopen.

What Phase 2 leaves for later, besides ellipse and spline:

- **Curves that cross are not split.** An outline is found where curves meet at their ends. Two
  lines that cross in the middle do not make four regions; trim them first.
- **A projected edge does not follow its body.** It is a copy made when it was projected. Following
  needs the feature history of Phase 3.
- **A sketch on a face does not follow the face** either, for the same reason. (Fixed in
  Phase 3: a sketch started on a face, or on a construction plane, follows it.)
- **Offset keeps one dimension.** The copy's lines stay parallel to their originals and its corners
  stay joined, but only the first line carries the offset distance. There is no constraint yet
  that says "the same distance all round".
- **Dimension values place themselves.** They are drawn next to what they measure and cannot be
  dragged somewhere else. That needs a place for them in the file.
- **Values are numbers.** `40`, `40 mm`, `40,5`. (Phase 3 added expressions and named
  parameters.)
- **A sketch being edited is drawn over the model**, not hidden by bodies in front of it.
- **Tangent arcs in a chain of lines, fillets, patterns, and text** are not there yet.

## Phase 3: Parametric solids (finished 2026-10-14)

Started early, on the owner's word. The extrude command is called **Pull**: it pulls, and with
Cut it pushes in, so one short word covers both (owner's choice, 2026-10-13).

- [x] Carried over from Phase 2: ellipse and spline in the sketcher. An ellipse is its center,
      the end of its major axis, and a minor radius the solver can move; a spline is a clamped
      cubic B-spline through its first and last control points, which are ordinary points, so
      every point constraint works on them. Trim, extend, offset, and crossings leave both
      alone; mirror, outlines with holes, Pull, and files take them.
- [x] Feature graph with dependency tracking and incremental recompute. A feature is built again
      only when something it depends on changed; while a sketch is open, rebuilding waits.
- [x] Stable references to faces and edges that survive upstream edits. Faces are named by the
      feature and the sketch curve they come from, edges by the faces they lie between. See
      [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#11-stable-references). Tested by the bracket:
      widen the plate, and the fillets stay on the same four edges.
- [x] Pull (join, cut, intersect, new body; one side, symmetric, two-sided; up to a flat face,
      which it follows; taper on a pull to one side) and revolve. Up to a curved face is not
      there yet.
- [x] Fillet, chamfer, shell, hole (simple, counterbore, countersink; to a depth with a drill
      point, or through all; at the points of a sketch), mirror, rectangular and circular
      pattern.
- [x] Timeline: select, edit (the model rolls back to just before the feature), rename,
      suppress, delete, reorder by dragging (refused, with the reason, where it would put a
      feature before what it depends on), the history marker dragged to roll back, failure
      markers with the reason, a compact size.
- [x] Failure handling that keeps the last valid state and names the broken feature and reference:
      a feature that cannot be applied is left out with a sentence saying why ("It does not reach
      Body, so there is nothing to cut away.", "An edge it works on no longer exists: the edge
      between a side of Pull 1 and the end of Pull 1."), and the rest carries on without it.
- [x] Global parameter table with units and expressions: named parameters (Modify >
      Parameters), expressions with units (mm, cm, m, in, ft, deg, rad), arithmetic, and
      functions, typed into any value box of a feature or any sketch dimension. A change to a
      parameter rebuilds what uses it; a new name is put everywhere the old one was. Units are
      converted, not checked: adding a length to an angle is not refused.
- [x] Measure tool, construction planes (offset from an origin plane or a flat face, and they
      follow it), and construction axes (an origin axis, a straight edge, or the axis of a round
      edge or face, and they follow it). Revolves turn about an origin axis or a construction
      axis as well as a sketch line; patterns go along or around construction axes, and mirrors
      are in construction planes.

Acceptance: build a dimensioned bracket, edit the first sketch, and verify downstream features update or
report a specific failure. Met, at two levels: in the document (`FeatureTest`) and through the
window with the pointer and the keyboard (`ModelingInterfaceTest`), including saving and reopening.

Built on the branch `phase-3-parametric-solids` (pull request #5), merged 2026-10-14.

What Phase 3 leaves for later: pulling up to a curved face (only flat faces so far), a taper on
a pull to both sides, trimming and offsetting ellipses and splines, checking that the units of
an expression go together (they are converted, not checked), and picking a construction axis by
clicking it in the 3D view (it is chosen from a list).

Stable references are the hardest problem in this phase and the usual reason parametric CAD projects stall.
Budget time for it first, not last.

## Phase 4: Direct modeling and interchange (finished 2026-10-19)

Started early, on the owner's word, once Phase 3 was merged. Built on the branch
`phase-4-direct-modeling-interchange` (pull request #6), merged 2026-10-19.

- [x] STEP import and export, IGES import. STEP (AP203, AP214, AP242) in any length unit, parts
      named as in the file; loose faces sewn and closed into solids where they close up, open
      surfaces kept and reported. STEP export writes AP214 in millimeters, a named part per body.
      Tried on files other programs wrote: a Euclid screw, five Onshape parts, an IGES canopy.
- [x] STL (binary or text), 3MF, OBJ export with resolution control: Coarse, Medium, Fine, or a
      deviation and an angle of one's own. The meshes are closed and face outward.
- [x] Direct edits on imported solids: Offset Face (also widens or narrows a hole), Move Face
      (flat faces along their normals; a hole or a boss across the face it is in), Delete Face
      with healing (a fillet, a hole, a boss goes).
- [x] Geometry validation and repair: Check Geometry lists invalid parts, open edges, inside-out
      solids, crossing faces, and short edges by body, and shows where; Repair sews faces (gaps
      up to a width it is given), turns faces outward, and fixes edges and tolerances.
- [x] Direct edits recorded as timeline features: edited, suppressed, reordered, saved, like any
      other. The imported geometry itself is kept: suppress the edits and it is as it was read.

Acceptance: import a STEP solid, offset a face, save, export STEP, and validate the result. Met
in the document (`tests/document/DirectEditTest.cpp`) and through the window
(`tests/gui/InterchangeInterfaceTest.cpp`): the STEP export is read back and checked, and its
volume compared with the closed-form value. STEP files written with each OCCT version are read
by every build (`tests/models/step/`).

What Phase 4 leaves for later: importing meshes (STL, OBJ, 3MF) and exporting IGES; moving a
flat face within its own plane, or faces that are not all flat in a direction that takes them
off their neighbors' surfaces in a way defeaturing cannot undo; AP242 export (AP214 is
written); and remembering which file a body was imported from.

## Phase 5: Components and assemblies (finished 2026-10-21)

Started on the owner's word once Phase 4 was merged, on the branch
`phase-5-components-assemblies` (pull request #7).

- [x] Components, bodies inside components, subassemblies. A component is defined once and
      appears as often as it has occurrences, nested as deep as wanted. New Component,
      Component from Bodies (with the features and sketches that made them), working on a
      component, Duplicate (another occurrence of the same component), Make Independent Copy,
      Move Component, Ground, Suppress, hide, isolate, rename, and moving a component into
      another by dragging it in the browser.
- [x] Linked external parts with dependency tracking and project packaging. Insert Part links
      another LCAD document; a copy of its bodies is kept in the document for when the file is
      missing. Linked Files lists every link, and the links of linked files, with their state;
      Update Linked Parts takes changes and keeps what is joined to the parts; Package Project
      gathers the document and its linked files into one folder that can be moved as it is.
- [x] Joints: rigid, revolute, slider, cylindrical, pin-slot, planar, and ball, placed on faces
      and edges, with a suggested kind, an offset, an angle, and a flip. They follow the faces
      when the parts change.
- [x] Joint limits and motion preview: values and limits for each way a joint moves, and
      Animate, which shows a joint going through its range without changing the document.
- [x] Interference checking (the overlapping volume, listed and shown), mass properties (mass,
      center of mass, inertia, principal moments) of a selection or of everything, and materials
      (a library for printing, metals, and woods, and the document's own).
- [x] Autosave and crash recovery: a copy of each document with unsaved changes every minute,
      apart from the user's files, and an offer to restore after a crash. Tested with a process
      that is killed on purpose.
- [x] Flatpak package, on KDE's runtime, built and started in CI.

Acceptance: assemble two independently editable parts, constrain them, reopen the project, and
verify the placement survives. Met in the document (`tests/document/AssemblyTest.cpp`: two
parts in components, a revolute joint at 30° with limits, saved, opened again, in the same
place, and each part still changed on its own with the joint following), and through the window
(`tests/gui/AssemblyInterfaceTest.cpp`: the joint set up in its panel, by clicks in the 3D view).

Asked for by the owner along the way, and done: an arrow in the 3D view that is dragged to set a
pull's distance, with a box beside it for typing one; slightly see-through sketch planes; and
colored X, Y, Z arrows on the view cube.

What Phase 5 leaves for later: joints in loops that have to be solved together (each part is
placed by one joint after another; a loop that already closes is accepted), dragging a part
along its joint in the 3D view (values are typed), contact between parts in motion, exploded
views, a bill of materials, joint origins on construction geometry or a component's origin
picked in the 3D view, materials that change how a body looks, the materials of a linked
document's own list (only LCAD's are known in the document it is linked into), and the
large-assembly benchmarks decision 3 asks for.

## Port: Windows and macOS (finished 2026-10-30)

Qt 6 and OCCT both run on Windows and macOS, so the port is mostly build, packaging, and platform testing,
provided the code stays portable from the start: no Linux-only APIs outside a small platform layer, CMake
throughout, and no hard-coded paths.

- [x] Windows build and installer: Visual Studio, with Qt and OCCT from conda-forge; an Inno Setup
      installer that adds LCAD to the Start menu and opens `.lcad` documents with it.
- [x] macOS build and app bundle: LCAD.app on a disk image, for Apple silicon, carrying its
      libraries, and opening `.lcad` documents from the Finder.
- [x] CI on all three platforms, on every pull request. Each package is installed and opens a
      document through the system, away from what it was built with.

So far both have only run on CI's machines, which have small screens and no graphics card. Using
them on a real Windows PC and a real Mac, with a real graphics driver and a high-DPI screen, is
still to do, and neither package is signed. What differs between the systems, and what that costs,
is in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#17-windows-and-macos).

## Phase 6: Drawings and manufacturing (started 2026-10-31, in progress, target 2028-01-15)

- [x] Associative drawings: orthographic, isometric, section, and detail views; dimensions; title blocks.
      The views are worked out from the model each time; dimensions hang on named edges and follow it.
- [x] PDF and DXF export.
- [x] CAM: stock, work coordinates, tool library, facing, pocket, contour, drill.
- [x] Toolpath simulation and time estimate: a height field of the stock, cut by the tools, that catches
      gouges, rapid moves into material, and tools buried past their flutes.
- [x] Postprocessors, starting with GRBL and LinuxCNC. What they write is read back by a strict reader
      of the dialect, and has to come to the same moves.
- [ ] Run LCAD's G-code on a real machine. Until then nothing says that it cuts what the simulation
      says it cuts.

Left for later, by the brief's own lights: 3-axis surfacing (these are 2.5D paths), arcs in the
G-code (a curve is many short lines), adaptive clearing, tabs, ramped entries, auxiliary views, GD&T,
balloons and bills of materials, a tool library shared between documents.

Acceptance: produce a dimensioned drawing from a model, create a machining operation, simulate it, and
export G-code through a validated postprocessor.

## Phase 7: Advanced capabilities (2028-01-16 onward)

Surface modeling, sheet metal, advanced mesh repair, advanced assembly motion, rendering, simulation
integrations, advanced CAM, Python scripting and plugins. Ordered by user demand once v1.0 is out.

## Decisions

Decided in Phases 0 and 1, with the reasoning in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md):

| Decision | Chosen |
|---|---|
| License | GPL-3.0-or-later |
| Viewport | OCCT's viewer inside a Qt widget |
| Native file format | Zip container of JSON plus B-rep |
| Document model | LCAD's own, not OCCT's application framework |
| Supported OCCT versions | 7.6 to 7.9 |
| Undo | Reversible edits recorded by the document, grouped by transactions |
| Commands | Defined once in a registry; settings in a panel beside the viewport with a live preview, not a modal dialog |
| Appearance | Design tokens; a light and a dark theme; Qt's Fusion widget style on every desktop |
| Up direction | Z. The front view looks along +Y |
| Mouse | Fusion's mapping. Wheel away from the user zooms in, with a setting to reverse it |
| Sketch solver | LCAD's own, built on Eigen. See [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md) for what was weighed |
| Sketch display | Drawn by LCAD over the 3D view while a sketch is open; as OCCT wires among the bodies otherwise |
| Stable references | Faces named by what made them, carried through booleans by OCCT's history; edges by the faces they lie between |
| Extrude command | Called Pull (owner's choice, 2026-10-13) |

Still open:

| Decision | Options | Needed by |
|---|---|---|
| When to make the repository public | v0.1, v0.5, or later | Owner's call |

## Risks found so far

- **OCCT 8.** Upstream released 8.0 in May 2026 with many interface changes. No supported distribution
  ships it yet. When the first one does, LCAD needs a port, and for a while has to build against both.
- **Wayland.** Distributions build OCCT's viewer for X11 only, so LCAD runs through XWayland on Wayland
  desktops. That works today; native Wayland depends on how distributions build OCCT.
- **Distribution packaging.** Two of the four distributions shipped OCCT build files that did not work
  as installed, and Ubuntu's libzip build files have the same kind of fault. LCAD avoids both, and CI
  installs from scratch on every run to catch the next case.
- **OCCT's viewer fails quietly.** Twice now a fault in how the viewer is embedded left every picture
  looking right: a blank window in Phase 0, and in Phase 1 an OpenGL context being made current on a
  window that no longer existed. Both are covered by tests now, and the tests fail on anything OCCT
  complains about. The next such fault will most likely show up the same way, as a message and not as
  a wrong picture.
- **The sketch solver is LCAD's own, and new.** The plan was to adopt a proven solver. The spike
  at the start of Phase 2 found that the packaged one says how many freedoms a sketch has but not
  which curves have them, which is what coloring a sketch by its state needs, and that it is
  packaged for only one of the four distributions. LCAD's solver has the sketch module's 46 test
  cases behind it and none of the years of use the others have. It sits behind the `sketch` module's interface, so
  it can be replaced without touching the rest. Expect sketches that it solves badly; each one
  that turns up becomes a test.
- **Mouse wheel direction.** Fusion's pan, orbit, and zoom buttons were checked against Autodesk's
  documentation. Which way its wheel zooms by default was not confirmed, so LCAD zooms in when the
  wheel rolls away from the user, as most Linux applications do, and has a setting to reverse it.
