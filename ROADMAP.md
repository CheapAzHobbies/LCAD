# LCAD roadmap

Phases follow section 30 of the [project brief](docs/PROJECT_BRIEF.md). Each phase ends with working
software and an acceptance test, not a mockup. A phase is done when its acceptance test passes in an
automated or scripted run.

The dates are estimates for part-time development, set on 2026-10-12. They get revised at the end of every
phase based on how long that phase actually took. Each phase is also a GitHub milestone with the same due
date.

Phase 0 was finished on 2026-10-12, four weeks ahead of its target, apart from the license decision. The
later dates stay as they are until Phase 1 shows whether that pace holds for real modeling work.

## Timeline

| Phase | What ships | Start | Target | Release |
|---|---|---|---|---|
| 0. Groundwork | Architecture, dependency and license decisions, build system, CI | 2026-10-12 | 2026-11-08 | |
| 1. Desktop foundation | Window, viewport, selection, documents, save/open, undo | 2026-11-09 | 2027-01-31 | |
| 2. Sketcher | Sketch planes, drawing tools, constraints, solver | 2027-02-01 | 2027-05-31 | |
| 3. Parametric solids | Extrude, cut, revolve, fillet, patterns, timeline, parameters | 2027-06-01 | 2027-11-30 | v0.1 alpha |
| 4. Direct modeling and interchange | STEP/IGES import, STL/3MF export, face edits, healing | 2027-12-01 | 2028-03-31 | v0.2 |
| 5. Components and assemblies | Components, linked parts, joints, interference checks | 2028-04-01 | 2028-09-30 | v0.5 beta |
| Port | Windows and macOS builds | 2028-10-01 | 2028-12-31 | v0.6 |
| 6. Drawings and manufacturing | Associative drawings, PDF/DXF, 2.5D and 3-axis CAM | 2029-01-01 | 2029-09-30 | v1.0 |
| 7. Advanced capabilities | Surfaces, sheet metal, mesh repair, rendering, plugins | 2029-10-01 | ongoing | |

v0.5 is the first release that meets the acceptance criteria in section 32 of the brief. The repository
stays private until the owner decides to open it; v0.1 is the earliest point where there is something
worth showing.

## Phase 0: Groundwork (2026-10-12 to 2026-11-08)

Decide before writing application code.

- [ ] Choose the license. The options and a recommendation are in
      [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md#what-the-license-choice-for-lcad-allows). This is the
      owner's decision and the only item still open.
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

## Phase 1: Desktop foundation (2026-11-09 to 2027-01-31)

- [ ] Main window with the five regions from the brief (top bar, toolbar, browser, viewport, timeline),
      empty where features do not exist yet.
- [ ] 3D viewport: perspective/orthographic, view cube, standard views, fit, and Fusion-style mouse
      mapping. Basic orbit, pan, and zoom exist from Phase 0.
- [ ] Face, edge, vertex, and body selection. Hover highlighting of whole shapes exists from Phase 0.
- [ ] Document model, object tree in the browser.
- [ ] Primitive solids (cylinder, sphere, and the rest) as the first real geometry. The box exists from
      Phase 0.
- [ ] Native project format, save and open, atomic writes, built to
      [docs/FILE_FORMAT.md](docs/FILE_FORMAT.md). Includes the test that a file saved with one OCCT
      version opens with another.
- [ ] Undo and redo through a transaction system.
- [ ] The kernel's own error type, and OCCT's diagnostic messages routed into LCAD's log.
- [ ] AppImage built by CI.

Acceptance: create a document, add a solid, save, close, reopen, and verify the geometry is identical.

## Phase 2: Sketcher (2027-02-01 to 2027-05-31)

- [ ] Sketch on origin planes and planar faces, with automatic camera orientation.
- [ ] Line, rectangle, circle, arc, point, construction geometry first; ellipse, spline, polygon, slot after.
- [ ] Geometric constraints: coincident, horizontal, vertical, parallel, perpendicular, tangent, equal,
      concentric, midpoint, symmetry, collinear, fix.
- [ ] Dimensional constraints: distance, length, radius, diameter, angle, editable in the canvas.
- [ ] Solver integration with under-, fully, and overconstrained states and conflict reporting.
- [ ] Dragging underconstrained geometry.
- [ ] Inference while drawing (endpoints, alignment, tangency), with an off switch.
- [ ] Trim, extend, offset, mirror, project.
- [ ] Closed profile detection for later features.

Acceptance: create a fully constrained rectangle, change its dimensions, and verify the geometry updates.

## Phase 3: Parametric solids (2027-06-01 to 2027-11-30)

- [ ] Feature graph with dependency tracking and incremental recompute.
- [ ] Stable references to faces and edges that survive upstream edits (topological naming).
- [ ] Extrude (join, cut, intersect, new body; symmetric, two-sided, to-object, taper), revolve.
- [ ] Fillet, chamfer, shell, hole, mirror, rectangular and circular pattern.
- [ ] Timeline: edit, suppress, reorder, roll back, failure markers.
- [ ] Failure handling that keeps the last valid state and names the broken feature and reference.
- [ ] Global parameter table with units and expressions.
- [ ] Measure tool, construction planes and axes.
- [ ] Command search.

Acceptance: build a dimensioned bracket, edit the first sketch, and verify downstream features update or
report a specific failure.

Stable references are the hardest problem in this phase and the usual reason parametric CAD projects stall.
Budget time for it first, not last.

## Phase 4: Direct modeling and interchange (2027-12-01 to 2028-03-31)

- [ ] STEP import and export, IGES import.
- [ ] STL, 3MF, OBJ export with resolution control.
- [ ] Direct edits on imported solids: move face, offset face, delete face with healing.
- [ ] Geometry validation and repair.
- [ ] Direct edits recorded as timeline features.

Acceptance: import a STEP solid, offset a face, save, export STEP, and validate the result.

## Phase 5: Components and assemblies (2028-04-01 to 2028-09-30)

- [ ] Components, bodies inside components, subassemblies.
- [ ] Linked external parts with dependency tracking and project packaging.
- [ ] Joints: rigid and revolute first, then slider, cylindrical, planar, ball, pin-slot.
- [ ] Joint limits and motion preview.
- [ ] Interference checking, mass properties, materials.
- [ ] Autosave and crash recovery hardened for release.
- [ ] Flatpak package.

Acceptance: assemble two independently editable parts, constrain them, reopen the project, and verify the
placement survives.

## Port: Windows and macOS (2028-10-01 to 2028-12-31)

Qt 6 and OCCT both run on Windows and macOS, so the port is mostly build, packaging, and platform testing,
provided the code stays portable from the start: no Linux-only APIs outside a small platform layer, CMake
throughout, and no hard-coded paths.

- [ ] Windows build and installer.
- [ ] macOS build and app bundle.
- [ ] CI on all three platforms.

## Phase 6: Drawings and manufacturing (2029-01-01 to 2029-09-30)

- [ ] Associative drawings: orthographic, isometric, section, and detail views; dimensions; title blocks.
- [ ] PDF and DXF export.
- [ ] CAM: stock, work coordinates, tool library, facing, pocket, contour, drill.
- [ ] Toolpath simulation and time estimate.
- [ ] Postprocessors, starting with GRBL and LinuxCNC.

Acceptance: produce a dimensioned drawing from a model, create a machining operation, simulate it, and
export G-code through a validated postprocessor.

## Phase 7: Advanced capabilities (2029-10-01 onward)

Surface modeling, sheet metal, advanced mesh repair, advanced assembly motion, rendering, simulation
integrations, advanced CAM, Python scripting and plugins. Ordered by user demand once v1.0 is out.

## Decisions

Decided in Phase 0, with the reasoning in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md):

| Decision | Chosen |
|---|---|
| Viewport | OCCT's viewer inside a Qt widget |
| Native file format | Zip container of JSON plus B-rep |
| Document model | LCAD's own, not OCCT's application framework |
| Supported OCCT versions | 7.6 to 7.9 |

Still open:

| Decision | Options | Needed by |
|---|---|---|
| License | GPL-3.0-or-later (recommended), an LGPL, or a permissive license. It decides which sketch solver can be used: SolveSpace's is GPL-3.0-or-later, FreeCAD's planegcs is LGPL-2.1-or-later. | Now: last open item of Phase 0 |
| Sketch solver | SolveSpace (libslvs), planegcs, or ezpz; chosen by a spike | Start of Phase 2 |
| Stable references to faces and edges | To be designed | Start of Phase 3 |
| When to make the repository public | v0.1, v0.5, or later | Owner's call |

## Risks found in Phase 0

- **OCCT 8.** Upstream released 8.0 in May 2026 with many interface changes. No supported distribution
  ships it yet. When the first one does, LCAD needs a port, and for a while has to build against both.
- **Wayland.** Distributions build OCCT's viewer for X11 only, so LCAD runs through XWayland on Wayland
  desktops. That works today; native Wayland depends on how distributions build OCCT.
- **Distribution packaging.** Two of the four distributions shipped OCCT build files that did not work
  as installed. LCAD now avoids them, and CI installs from scratch on every run to catch the next case.
