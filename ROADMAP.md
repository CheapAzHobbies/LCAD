# LCAD roadmap

Phases follow section 30 of the [project brief](docs/PROJECT_BRIEF.md). Each phase ends with working
software and an acceptance test, not a mockup. A phase is done when its acceptance test passes in an
automated or scripted run.

The dates are estimates for part-time development, set on 2026-10-12. They get revised at the end of every
phase based on how long that phase actually took. Each phase is also a GitHub milestone with the same due
date.

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

- [ ] Choose the license (see "Open decisions").
- [ ] Dependency assessment: OCCT, Qt 6, sketch solver candidates, file format libraries. Record license,
      maintenance status, and distro package availability for each in `docs/DEPENDENCIES.md`.
- [ ] Architecture document: module boundaries for kernel wrapper, document model, feature graph,
      rendering, and UI (`docs/ARCHITECTURE.md`).
- [ ] Decide the viewport approach: OCCT's own viewer (AIS/V3d) inside Qt, or a custom renderer.
- [ ] Decide the native file format and how it will be versioned.
- [ ] CMake project skeleton that builds an empty Qt window linked against OCCT on Ubuntu, Fedora, Arch.
- [ ] CI: build and test on every push.
- [ ] Test strategy: unit test framework, geometry assertions with tolerances, reference models.

Done when: a clean checkout builds and runs an empty window on the three distro families, CI is green, and
the decisions above are written down.

## Phase 1: Desktop foundation (2026-11-09 to 2027-01-31)

- [ ] Main window with the five regions from the brief (top bar, toolbar, browser, viewport, timeline),
      empty where features do not exist yet.
- [ ] 3D viewport: orbit, pan, zoom, perspective/orthographic, view cube, standard views, fit.
- [ ] Face, edge, vertex, and body selection with hover highlighting.
- [ ] Document model, object tree in the browser.
- [ ] Primitive solids (box, cylinder, sphere) as the first real geometry.
- [ ] Native project format, save and open, atomic writes.
- [ ] Undo and redo through a transaction system.
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

## Open decisions

| Decision | Options | Needed by |
|---|---|---|
| License | GPL-3.0-or-later, LGPL-2.1-or-later, or a permissive license. It constrains which sketch solver can be used: SolveSpace's solver is GPL-3.0, FreeCAD's planegcs is LGPL-2.1+. OCCT is LGPL-2.1 with an exception; Qt 6 is LGPL-3.0/GPL-3.0. | End of Phase 0 |
| Sketch solver | SolveSpace (libslvs), planegcs, or a new solver | Start of Phase 2 |
| Viewport | OCCT viewer inside Qt, or a custom renderer | End of Phase 0 |
| Native file format | Zip container of JSON plus B-rep, or SQLite | End of Phase 0 |
| When to make the repository public | v0.1, v0.5, or later | Owner's call |
