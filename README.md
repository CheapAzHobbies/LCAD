# LCAD

LCAD (LinuxCAD) is an open source parametric 3D CAD application for Linux, with a workflow modeled on
Fusion 360: sketch, constrain, extrude, edit through a feature timeline, assemble. It runs offline, keeps
project files on your own disk, and needs no account.

Linux comes first. Windows and macOS ports are planned once the core modeling workflow is complete.

## Status

Planning. There is no application code yet. Phase 0 (architecture, dependency and license decisions, build
system) runs until 2026-11-08.

## Documents

- [Roadmap](ROADMAP.md): phases, target dates, and open decisions.
- [Project brief](docs/PROJECT_BRIEF.md): the full product requirements.

## Planned stack

Candidates under evaluation in Phase 0, not final:

- C++ and CMake
- Open CASCADE Technology (OCCT) for the geometry kernel
- Qt 6 for the desktop interface
- A 2D constraint solver for sketches

## License

Not chosen yet. It will be an open source license, decided during Phase 0 because it affects which
libraries LCAD can use.
