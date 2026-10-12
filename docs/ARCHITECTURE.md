# Architecture

How LCAD is divided into modules, the rules between them, and the decisions made in Phase 0 with
the reasons for each. The product requirements are in [PROJECT_BRIEF.md](PROJECT_BRIEF.md).

## What exists today

Phase 0 built a skeleton that passes through every major dependency, so the decisions below rest
on running code and not only on reading documentation:

- `kernel` can make a box and measure any shape (volume, area, center of mass, bounding box,
  topology counts, validity).
- `viewport` shows one shape in OCCT's viewer inside a Qt widget, with orbit, pan, zoom, and
  hover highlighting.
- `app` is a main window with the viewport, a File menu, and an About dialog.

Everything else in this document is the plan the later phases build to.

## Modules

```mermaid
flowchart TD
    app["app<br/>windows, panels, commands, tools"]
    viewport["viewport<br/>3D display and picking"]
    document["document<br/>model, history, undo, files"]
    sketch["sketch<br/>2D geometry and constraints"]
    kernel["kernel<br/>exact geometry"]
    qt(["Qt 6"])
    occt(["Open CASCADE"])

    app --> viewport
    app --> document
    viewport --> document
    viewport --> kernel
    document --> sketch
    document --> kernel
    app -.-> qt
    viewport -.-> qt
    viewport -.-> occt
    kernel -.-> occt
```

| Module | Responsibility | Status |
|---|---|---|
| `kernel` | Exact B-rep geometry: shapes, modeling operations, measurement, validity, tessellation, import and export. A thin layer over OCCT. | Started |
| `sketch` | Sketch entities, constraints, and the adapter to the constraint solver. | Phase 2 |
| `document` | The model: components, bodies, sketches, parameters, and the feature history. Recompute, transactions and undo, saving and loading. | Phase 1 (document, undo, files), Phase 3 (features) |
| `viewport` | Displays the document's geometry and turns pointer input into picks and camera motion. Wraps OCCT's viewer in a Qt widget. | Started |
| `app` | Main window, browser, timeline, toolbars, the command system, and interactive tools. | Started |

Drawings, CAM, and scripting become modules of their own when their phases start. Each sits beside
`app` and uses `document` and `kernel` the same way.

### Rules

1. **Dependencies point one way.** A module may use the ones below it in the diagram and never
   the ones above.
2. **OCCT stays inside `kernel` and `viewport`.** No other module includes an OCCT header or
   names an OCCT type. The build enforces this: only those two modules link the OCCT targets, and
   OCCT's headers are not on anyone else's include path. `kernel` hands out its own `Shape`
   value; the few places that must reach the OCCT shape behind it include
   `lcad/kernel/occt/ShapeAccess.h`, which only compiles for modules that link OCCT.
3. **`kernel`, `sketch`, and `document` do not use Qt.** They build and test without a display,
   and can later serve a command-line tool or a scripting interface. The build enforces this too:
   they do not link Qt.
4. **The document is the single source of truth.** The viewport and the interface observe it and
   ask it to change. They hold no model state of their own.
5. **Geometry is exact.** Measurement, validation, and export work on the B-rep. Triangle meshes
   exist only for display and for mesh export.

The cost of rule 2 is wrapper code: every OCCT capability LCAD uses needs a kernel function. The
return is that OCCT's API changes (the move to 8.x is a large one) are absorbed in one module, and
that the rest of the code reads in LCAD's own vocabulary.

## Decisions

Each was made on 2026-10-12 unless noted.

| # | Decision | Chosen | Revisit when |
|---|---|---|---|
| 1 | Geometry kernel | OCCT 7.6 to 7.9, behind `kernel` | A supported distribution ships OCCT 8 |
| 2 | Interface toolkit | Qt 6 Widgets | Not expected |
| 3 | Viewport | OCCT's own viewer inside a `QOpenGLWidget` | Large-assembly benchmarks in Phase 5; native Wayland |
| 4 | Document model | LCAD's own, not OCCT's application framework | Not expected |
| 5 | Native file format | A zip container of JSON and B-rep; see [FILE_FORMAT.md](FILE_FORMAT.md) | Not expected |
| 6 | Units | Millimeters and radians internally | Not expected |
| 7 | Errors | No OCCT exception leaves `kernel` | Not expected |
| 8 | Finding OCCT at build time | From headers and libraries, not OCCT's CMake package | Distributions fix their packages |
| 9 | Tests | Catch2 for units, Qt Test for the interface; see [TESTING.md](TESTING.md) | Not expected |
| 10 | License | Open: the owner's decision, and the last open item of Phase 0; see [DEPENDENCIES.md](DEPENDENCIES.md) | |
| 11 | Stable references to faces and edges | Open, designed at the start of Phase 3 | |
| 12 | Sketch solver | Open, chosen by a spike at the start of Phase 2 | |

### 1. Geometry kernel

OCCT, used through `kernel`. The assessment is in [DEPENDENCIES.md](DEPENDENCIES.md).

### 2. Interface toolkit

Qt Widgets, not Qt Quick. LCAD's interface is a dense desktop layout of docked panels, tree
views, toolbars, and menus, which is what Widgets is built for, and embedding OCCT's viewer in a
`QOpenGLWidget` is a path other applications already use (Mayo, and OCCT's own Qt samples).

### 3. Viewport

<a id="viewport"></a>

**Chosen:** OCCT's interactive viewer (its `AIS` and `V3d` layers) drawing into a
`QOpenGLWidget`, hidden behind `viewport` so the rest of LCAD never sees it.

**Alternative:** a renderer written for LCAD.

**Why.** OCCT's viewer already does most of what Phase 1 asks of the viewport, and does it
directly from the B-rep: display meshes that follow the exact shape, picking of faces, edges, and
vertices with hover highlighting, a view cube, section planes, manipulators, and dimensions. A
custom renderer would spend months reaching the point where a face can be picked reliably. The
spike in this repository had a box on screen with orbit, pan, zoom, and hover highlighting in a
day, and it runs unchanged on all four tested distributions.

**What it costs.**

- *X11 only, for now.* Distributions build OCCT's viewer for GLX, so it needs an X11 connection.
  Under a Wayland session LCAD therefore runs through XWayland: `prepareApplication()` selects
  Qt's `xcb` platform plugin unless `QT_QPA_PLATFORM` is already set. This was tested on a GNOME
  Wayland session: through XWayland the viewer runs on the machine's GPU (OpenGL 4.6); with Qt's
  native Wayland plugin OCCT cannot adopt Qt's OpenGL context, and the viewport reports that
  instead of drawing. Native Wayland needs an OCCT built for EGL, which none of the
  distribution packages checked provide.
- *OCCT's look.* Rendering quality and style are bounded by what OCCT's renderer offers.
- *Unknown at scale.* Performance with large assemblies has not been measured. Phase 5 has to
  benchmark it; because of rule 2, replacing the renderer later touches `viewport` only.

**How the embedding works, and what goes wrong.** Qt owns the OpenGL context, the framebuffer,
and the buffer swap; OCCT adopts all three and draws when Qt asks the widget to paint. Three
details matter:

- The viewer must be given the native window the widget is drawn *in* (`effectiveWinId()`),
  never a native window of the widget's own (`winId()`). The first version of the spike used
  `winId()`. Its own framebuffer looked right, while the real window showed nothing, because Qt
  could no longer compose the widget with the menus around it. Two tests now fail if this
  regresses: one compares what the X server displays with what the widgets drew, and the
  interface tests fail on any OpenGL warning from Qt.
- The surface format (depth and stencil buffers) is made Qt's default before `QApplication`
  exists, so the top-level window's surface matches the viewport's. Without it the viewer still
  drew correctly in testing, but OCCT warned on every start that the window had no depth or
  stencil buffer.
- Qt and OCCT share one context and each changes OpenGL state the other relies on, so the widget
  resets that state before and after OCCT draws. This follows the upstream sample. Nothing LCAD
  draws yet depends on it: removing the reset breaks no current test.

The technique follows the QOpenGLWidget sample maintained for OCCT; see
[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

### 4. Document model

**Chosen:** plain C++ classes owned by `document`, with LCAD's own transaction system for undo
and its own file format.

**Alternative:** OCAF, the application framework that ships with OCCT. It offers a document tree,
undo, persistence, and a naming service for sub-shapes.

**Why.** A history-based modeler is defined by its feature graph, its recompute rules, and how
references survive edits. Those need to be designed for LCAD's workflow and kept under LCAD's
control, including the file format and its migrations. Building on OCAF would also tie every
module that touches the document to OCCT, which rule 2 exists to prevent. OCAF's exchange
structures will still be used inside `kernel` where STEP import and export need them.

### 5. Native file format

One `.lcad` file per document: a zip container holding JSON for the model and binary B-rep for
geometry. The layout, versioning, and saving rules are in [FILE_FORMAT.md](FILE_FORMAT.md).

### 6. Units

Lengths are stored in millimeters and angles in radians, as `double`. A document's display unit
(millimeters or inches) is applied where values are shown or typed, and nowhere else. Parameters
carry their own units from Phase 3. OCCT is unitless and its tolerances assume a scale near
millimeters, which this matches.

### 7. Errors

- A wrong argument is the caller's mistake: `kernel` throws `std::invalid_argument`, as
  `makeBox` does for a size that is not a positive length.
- A modeling operation that cannot produce a result (a fillet too large for its edge, a boolean
  that fails) is an expected outcome. From the first operation that can fail, `kernel` reports
  it as an LCAD error type that says what failed and why. The recompute engine catches it per
  feature, marks that feature failed, and keeps the last valid model, as the brief requires.
- No OCCT exception crosses the `kernel` or `viewport` boundary. The viewport already works this
  way: if it cannot start, it shows an explanation and the rest of the window keeps working.

### 8. Finding OCCT at build time

`cmake/LcadOpenCascade.cmake` locates OCCT's headers and libraries itself. The CMake package
OCCT installs cannot be used as distributions ship it: on Ubuntu 24.04 its targets link
`libtbb.so` by absolute path with nothing depending on the package that provides the file, and
on Ubuntu 26.04 it refuses to load unless OCCT's separately packaged VTK bridge is installed.
Both failures showed up in CI on clean installs and not on a developer machine that happened to
have the extra packages.

### 11. Stable references (open)

When an early feature changes, later features must still find "that edge" and "that face". This
is the hardest problem in a parametric modeler and the usual reason such projects stall. OCCT
reports which faces each operation created or modified, which is the raw material; what to
build from it is designed at the start of Phase 3, before any feature that takes a face or edge
as input.

## Threads

The interface thread never runs a long geometry operation. Recompute runs on a worker thread and
hands finished shapes back, and long operations can be canceled and report progress.

One constraint from OCCT shapes this: meshing a shape for display stores the mesh inside the
shape's shared data, so a shape must not be meshed on one thread while another thread reads it.
The design for recompute in Phase 3 has to respect that.

## Source layout

```
cmake/              build helpers (OCCT lookup, warning flags)
scripts/            install-deps.sh
src/<module>/
    include/lcad/<module>/   the module's public headers
    src/                     its implementation
tests/
    kernel/         unit tests (Catch2), no display needed
    gui/            interface tests (Qt Test), run on a virtual display
    support/        shared tolerances and frame checks
docs/
```

Code is C++20. Formatting is whatever `clang-format` produces from `.clang-format`. Types are
`PascalCase`, functions and variables `camelCase`, private members `m_name`, and a file is named
after the class it holds.
