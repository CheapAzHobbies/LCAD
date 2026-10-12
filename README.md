# LCAD

LCAD (LinuxCAD) is an open source parametric 3D CAD application for Linux, with a workflow modeled on
Fusion 360: sketch, constrain, extrude, edit through a feature timeline, assemble. It runs offline, keeps
project files on your own disk, and needs no account.

Linux comes first. Windows and macOS ports are planned once the core modeling workflow is complete.

## Status

Early groundwork. LCAD cannot model anything yet.

What exists is the foundation from Phase 0: a window with a 3D viewport (orbit, pan, zoom, hover
highlighting), a small geometry kernel layer over Open CASCADE with exact measurement, a test suite, and
CI that builds and tests on Ubuntu 24.04, Ubuntu 26.04, Fedora 44, and Arch. Phase 1 adds documents,
saving, selection, and undo. See the [roadmap](ROADMAP.md).

## Building

```sh
scripts/install-deps.sh
cmake --preset release
cmake --build --preset release
build/release/bin/lcad
```

Details, supported distributions, and how to run the tests are in [docs/BUILDING.md](docs/BUILDING.md).

## Documents

- [Roadmap](ROADMAP.md): phases, target dates, and open decisions.
- [Project brief](docs/PROJECT_BRIEF.md): the full product requirements.
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

Not chosen yet. It will be an open source license. The options and what each allows are laid out in
[docs/DEPENDENCIES.md](docs/DEPENDENCIES.md#what-the-license-choice-for-lcad-allows).
