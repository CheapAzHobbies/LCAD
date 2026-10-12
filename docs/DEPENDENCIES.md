# Dependencies

What LCAD depends on, under which license, how well maintained it is, and which version each
supported distribution ships. Versions and dates were checked on 2026-10-12 against each
project's repository and the distributions' package indexes.

This is an engineering summary, not legal advice.

## In use now

| Dependency | Used for | License | Latest upstream | Minimum LCAD supports |
|---|---|---|---|---|
| [Open CASCADE Technology](https://github.com/Open-Cascade-SAS/OCCT) (OCCT) | Geometry kernel and 3D viewer | LGPL-2.1 with the Open CASCADE exception 1.0 | 8.0.1 (2026-07-30) | 7.6 |
| [Qt 6](https://www.qt.io) (Core, Gui, Widgets, OpenGL, OpenGLWidgets) | Desktop interface | LGPL-3.0, GPL-2.0, GPL-3.0, or commercial | 6.12.0 | 6.4 |
| [Catch2](https://github.com/catchorg/Catch2) | Unit tests | BSL-1.0 | 3.16.1 (2026-10-08) | 3.4 (oldest tested) |
| Qt Test | GUI tests | Same as Qt | Same as Qt | 6.4 |
| CMake, Ninja, GCC or Clang | Build | Build tools only | | CMake 3.28, C++20 |

Catch2 and Qt Test are used only to build the tests. They are not part of what LCAD ships.

### Versions in each distribution

| | Ubuntu 24.04 | Ubuntu 26.04 | Fedora 44 | Arch |
|---|---|---|---|---|
| OCCT | 7.6.3 | 7.9.2 | 7.9.3 | 7.9.3 |
| Qt | 6.4.2 | 6.10.2 | 6.11.2 | 6.12.0 |
| Catch2 | 3.4.0 | 3.7.1 | 3.13.0 | 3.16.1 |

Linux Mint 22 is built on Ubuntu 24.04 and has the same versions. Ubuntu 24.04 is the floor: it
sets the minimum Qt and OCCT that LCAD's source has to compile against.

### Open CASCADE Technology

OCCT is the only open source B-rep kernel that is mature across everything LCAD needs: exact
solids, booleans, fillets, STEP and IGES exchange, and a viewer. FreeCAD and Mayo are built on
it. It is developed in the open by Open Cascade SAS, with commits on the day this was written.

- **License.** LGPL-2.1 plus an exception that lets a program include material from OCCT's
  headers without that material falling under the LGPL. LCAD links OCCT dynamically and does not
  modify it. The exception asks for a prominent notice that the program uses OCCT; the README and
  the About dialog carry it.
- **Version range.** Distributions ship 7.6 to 7.9. Version 8.0 (May 2026) is, in its own
  release notes, the largest single-version step in years. It removes long-standing interfaces
  such as `Standard_Failure::Raise` and `Standard_Mutex`, so code written for 7.x does not
  necessarily compile. No supported distribution ships 8.x yet. LCAD targets 7.6 to 7.9 and
  warns at configure time on 8.x. The port to 8.x gets scheduled when the first supported
  distribution moves.
- **Graphics.** Distributions build OCCT's viewer against GLX, which needs an X11 connection.
  Checked on Ubuntu 24.04 (the viewer library links `libGL` and `libX11`, and the package is
  configured without OpenGL ES) and on Arch (only the desktop OpenGL viewer library is packaged,
  and the package depends on `libx11`). The consequence is in
  [ARCHITECTURE.md](ARCHITECTURE.md#viewport).
- **CMake package.** LCAD does not use the CMake package that OCCT installs;
  `cmake/LcadOpenCascade.cmake` finds the headers and libraries itself. As distributions ship
  it, the package fails on a clean install: on Ubuntu 24.04 its targets link `libtbb.so` by
  absolute path with nothing depending on the package that provides that file, and on Ubuntu
  26.04 it will not load unless OCCT's separately packaged VTK bridge is installed. Its version
  file also accepts only exact matches, so asking for "7.6 or newer" fails on 7.6.3.

### Qt 6

Qt Widgets gives native menus, docking, dialogs, and high-DPI support on Linux, Windows, and
macOS, which is what the port in the roadmap needs. All modules LCAD uses are in `qtbase` and are
offered under LGPL-3.0, GPL-2.0, GPL-3.0, or a commercial license.

Under the LGPL, the practical way to ship a binary release (AppImage, Flatpak) is to link Qt
dynamically, include the license texts, and leave users able to replace the Qt libraries. That is
how Qt applications are normally packaged, so it costs nothing extra.

## Sketch constraint solver (decision due at the start of Phase 2)

| Candidate | License | Status | Packaged | Notes |
|---|---|---|---|---|
| [SolveSpace](https://github.com/solvespace/solvespace) solver (`libslvs`) | GPL-3.0-or-later | v3.2 (2026-03-27), active | Debian and Ubuntu only (`libslvs1-dev`, 3.1) | C interface. Handles 2D and 3D constraints, so it could also serve assemblies. Dune3D, a GPL parametric modeler, bundles it. Needs Eigen. Would be vendored for Fedora and Arch. |
| FreeCAD's [planegcs](https://github.com/FreeCAD/FreeCAD/tree/main/src/Mod/Sketcher/App/planegcs) | LGPL-2.1-or-later | Maintained inside FreeCAD (1.1.4, 2026-09-28) | No, it is not a standalone library | 2D only. Reports redundant and conflicting constraints. Needs Eigen and Boost.Graph, and includes FreeCAD's own base headers, so it has to be extracted and patched, then kept in sync by hand. |
| [ezpz](https://github.com/KittyCAD/ezpz) | MIT | Young (last push 2026-08-05) | No | Written in Rust for Zoo Design Studio. Would add a Rust toolchain to the build. |
| A solver written for LCAD | n/a | n/a | n/a | Months of work before the first sketch solves. Not justified while two proven solvers exist. |

LCAD is licensed under GPL-3.0-or-later, so every candidate can be used, including `libslvs`,
which requires it. `libslvs` is the first candidate because it is a real library with a stable
interface and is proven in a Fusion-style sketcher. The choice should be confirmed with a spike
at the start of Phase 2 that checks conflict reporting and dragging of underconstrained geometry.

Eigen differs by a major version between distributions (3.4.0 on Ubuntu, 5.0.1 on Arch and
Fedora 44), which matters to whichever solver is vendored.

## Native file format (Phase 1)

See [FILE_FORMAT.md](FILE_FORMAT.md) for the decision. It needs:

| Dependency | Used for | License | Latest upstream | Ubuntu 24.04 | Ubuntu 26.04 | Fedora 44 | Arch |
|---|---|---|---|---|---|---|---|
| [libzip](https://github.com/nih-at/libzip) | The `.lcad` container | BSD-3-Clause | 1.12 (2026-10-07) | 1.7.3 | 1.11.4 | 1.11.4 | 1.11.4 |
| [nlohmann/json](https://github.com/nlohmann/json) | Document and manifest | MIT | 3.12.0 | 3.11.3 | 3.11.3 | 3.12.0 | 3.12.0 |
| OCCT `BinTools` | B-rep geometry | Part of OCCT | | | | | |

Neither library is linked yet. They are added to the build when Phase 1 implements saving.

## Later phases

Noted now because they affect planning; each gets a full assessment when its phase starts.

- **3MF export (Phase 4).** [lib3mf](https://github.com/3MFConsortium/lib3mf) is BSD-2-Clause
  and current upstream (2.5.0), but distributions are far apart: Ubuntu ships 1.8.1, Fedora
  2.2.0, Arch 2.5.0. LCAD will have to bundle it or write the small core of 3MF itself.
- **STEP, IGES, STL, OBJ, glTF (Phase 4).** OCCT's own data exchange toolkits. Their library
  names changed between 7.6 and 7.9 (`TKSTEP` became `TKDESTEP`, and so on), so the build has to
  handle both.
- **Assembly solver (Phase 5).** Candidates are the SolveSpace solver in 3D and
  [OndselSolver](https://github.com/FreeCAD/OndselSolver) (LGPL-2.1), which FreeCAD maintains
  after the original repository was archived in 2025.
- **DXF and PDF (Phase 6).** Qt writes PDF. A DXF library is to be chosen.
- **CAM, mesh repair, Python scripting.** To be assessed.

## LCAD's own license

LCAD is licensed under GPL-3.0-or-later. The owner decided this on 2026-10-12. It keeps every
solver option open, matches the closest comparable projects (SolveSpace and Dune3D), and means
that anyone who distributes a modified LCAD has to publish their source.

For the record, what each option would have allowed:

| If LCAD were licensed under | OCCT | Qt | SolveSpace solver | planegcs | Effect on others |
|---|---|---|---|---|---|
| GPL-3.0-or-later | Yes | Yes | Yes | Yes | Anyone distributing a modified LCAD must publish their source. |
| LGPL-2.1-or-later or LGPL-3.0 | Yes | Yes | No | Yes | Others may link LCAD's libraries from closed programs. |
| MIT or Apache-2.0 | Yes | Yes | No | Yes | Others may build closed products from LCAD's code. Binaries still carry the LGPL terms of Qt and OCCT. |

As sole copyright holder the owner can still relicense later. That gets harder with each outside
contribution unless contributors agree to it up front.
