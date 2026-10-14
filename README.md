# LCAD

LCAD (LinuxCAD) is an open source parametric 3D CAD application for Linux, with a workflow modeled on
Fusion 360: sketch, constrain, extrude, edit through a feature timeline, assemble. It runs offline, keeps
project files on your own disk, and needs no account.

Linux comes first. Windows and macOS ports are planned once the core modeling workflow is complete.

## Status

Early. LCAD can sketch, turn sketches into solids, and change them through a feature timeline.
Phases 0 to 3 of the [roadmap](ROADMAP.md) are done; assemblies, import and export, and drawings
are still to come.

What works today:

- **Sketching.** A sketch on a plane or on a flat face of a body, with lines, rectangles,
  circles, arcs, ellipses, splines, points, polygons, slots, and construction geometry; twelve geometric
  constraints; dimensions typed in the 3D view; a solver that shows what can still move and
  refuses what contradicts; dragging; trim, extend, offset, mirror, and projecting edges of a
  body into the sketch.
- **Solids from sketches.** Pull (extrude) closed areas of a sketch into a new body, or join,
  cut, or intersect with one, to one side, both sides, symmetrically, or up to a face, with a
  taper if wanted; revolve them about a
  line or an axis; holes (simple, counterbored, countersunk); fillet, chamfer, and shell; mirror and
  rectangular and circular patterns of features.
  Each with a live preview, and editable later.
- **A feature timeline.** Edit, rename, suppress, reorder, and roll back. Change a sketch and the
  solid follows; a feature that no longer fits says why, and the rest of the model carries on.
- **Solids from primitives.** Box, cylinder, sphere, cone, and torus by their sizes, with a
  live preview.
- **Construction planes** to sketch on and **construction axes** to revolve about or pattern
  around; they, and sketches on a face or a plane, follow what they are placed on when the
  model changes.
- **Parameters**: named values, with units and expressions, that feature values and sketch
  dimensions can be worked out from.
- **Measuring** distances and angles between faces, edges, and bodies.
- **The window.** A 3D view with a view cube, standard views, display styles, and a grid;
  selecting bodies, faces, edges, vertices, and sketches, with their measurements; a browser
  tree and a properties panel to rename, hide, isolate, and delete; several documents in tabs;
  undo and redo; a command search; a light and a dark theme.
- **Files.** Saving and opening `.lcad` documents.

[docs/USING.md](docs/USING.md) shows how.

## Building

```sh
scripts/install-deps.sh
cmake --preset release
cmake --build --preset release
build/release/bin/lcad
```

Details, supported distributions, how to run the tests, and how to build the AppImage are in
[docs/BUILDING.md](docs/BUILDING.md).

## Documents

- [Using LCAD](docs/USING.md): what it can do today, and the mouse and keyboard controls.
- [Roadmap](ROADMAP.md): phases, target dates, and open decisions.
- [Project brief](docs/PROJECT_BRIEF.md): the full product requirements.
- [GUI specification](docs/GUI_SPEC.md): the requirements for the interface, and
  [where the interface stands](docs/INTERFACE_STATUS.md) against them.
- [Architecture](docs/ARCHITECTURE.md): modules, the rules between them, and the decisions made so far.
- [Dependencies](docs/DEPENDENCIES.md): what LCAD is built on, with licenses and versions.
- [File format](docs/FILE_FORMAT.md): the design of the native `.lcad` format.
- [Testing](docs/TESTING.md): how LCAD is tested and the rules for new tests.
- [Building](docs/BUILDING.md): build instructions.

## Built with

LCAD uses [Open CASCADE Technology](https://dev.opencascade.org) for exact geometry and 3D display, and
[Qt](https://www.qt.io) for its interface. Both are used under the LGPL. See
[docs/DEPENDENCIES.md](docs/DEPENDENCIES.md) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

Copyright (C) 2026 the LCAD authors.

LCAD is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
License as published by the Free Software Foundation, either version 3 of the License, or (at your option)
any later version. It is distributed in the hope that it will be useful, but without any warranty. The full
text is in [LICENSE](LICENSE).

The libraries LCAD links against keep their own licenses; see
[docs/DEPENDENCIES.md](docs/DEPENDENCIES.md).
