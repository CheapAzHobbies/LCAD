# Testing

How LCAD is tested, how to run the tests, and the rules new tests follow.

Two rules come straight from the project brief and shape everything else:

- **A picture is not proof.** A viewport that looks right says nothing about whether the geometry
  is right. Geometry is checked by measuring the exact shape.
- **A feature works when a test has exercised it**, not when its code has been written.

## Running the tests

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Useful variations:

```sh
ctest --preset dev -L unit            # no display needed, runs in well under a second
ctest --preset dev -L gui             # interface tests, on a virtual display
ctest --preset dev -R makeBox         # tests whose name matches
LCAD_TEST_ARTIFACTS=/tmp/frames ctest --preset dev -L gui    # keep the rendered frames as PNG

cmake --preset asan && cmake --build --preset asan && ctest --preset asan    # sanitizers
```

## Kinds of test

| Kind | Framework | Label | Needs a display | Covers today |
|---|---|---|---|---|
| Unit | Catch2 | `unit` | No | `core`: the log. `kernel`: primitives, measurement, moving, sub-shapes, serialization, errors, message routing, planes and projection onto them. `sketch`: geometry, every constraint, the solver's states, dragging, closed outlines, and the sketch tools. `document`: the model, undo, transactions, the file format, sketches as shapes. `kernel` and `document` also: STEP and IGES import, the STEP, STL, OBJ, and 3MF files written (read back by the tests' own parsers: closed meshes, volumes), direct edits, geometry checks, repair |
| Save and reopen | Catch2 | `unit` | No | A reopened document measures the same as the one saved; damaged, foreign, and newer files are refused |
| Reference models | Catch2 | `unit` | No | The documents and STEP files in `tests/models/` open and measure correctly, whichever OCCT wrote them |
| Interface | Qt Test | `gui` | A virtual one | Eight programs, each run at the screen's scale and, on Linux, again at twice that (the screens of CI's Windows and macOS machines are too small to double). `ViewportTest`: the rendering path, camera, selection, display styles, guides. `SketchViewTest`: the sketch editor in the 3D view, driven with the pointer and the keyboard. `ApplicationTest`: the main window, modeling commands, files, and Phase 1's acceptance test through the interface. `InterfaceTest`: the command registry and search, themes and icons, tabs, the remembered layout, the properties panel, context menus, shortcuts. `SketchInterfaceTest`: sketching through the whole window, from New Sketch to save and reopen. `ModelingInterfaceTest`: features, the timeline, and measuring through the whole window. `InterchangeInterfaceTest`: import, the direct edits, Check Geometry and Repair, export, and the export dialog. `AssemblyInterfaceTest`: components and working on one, joints set up by clicks and animated, moving a component, interference, mass and materials, linked parts and packaging, recovery after a real crash (the test runs itself again as a process that keeps unsaved work and is killed), and the arrow a pull is dragged by |
| Sanitizers | All of the above, built with ASan and UBSan | all | A virtual one | Memory errors, leaks, undefined behavior |
| Package | A script, in CI | | A virtual one | The AppImage starts on another distribution and draws a document; the Flatpak is built, installed, and does the same; the Windows installer installs and the macOS disk image is opened, and each opens a document through the system, away from what it was built with |

Planned, each when the code it tests arrives:

| Kind | Phase | What it checks |
|---|---|---|
| Recompute | 3 | Edit an early feature or parameter, then measure the result |
| Migration | With the first format change | Every older format version still opens |
| Benchmarks | 3 onward | Time and memory for simple parts, long histories, large imports, dense meshes, large assemblies |

## The acceptance test of a phase

Each phase ends with the acceptance test the roadmap gives it, automated. Phase 1's is "create
a document, add a solid, save, close, reopen, and verify the geometry is identical". It exists
twice: in `tests/document/FileTest.cpp` against the document module alone, and in
`tests/gui/ApplicationTest.cpp` through the interface, by triggering the Create action, typing
sizes into the panel, pressing OK, saving, destroying the window, and opening the file in a new
one. Both compare measurements of the exact geometry with closed-form values.

Phase 1b's acceptance test is about the interface itself, and is in
`tests/gui/InterfaceTest.cpp`: every command has one definition, is in a menu, and is found by
the search under its own name; no two commands share a shortcut; the window's layout, theme, and
view settings come back after a restart; and at three window sizes, in both themes and at both
display scales, no control overlaps another or is cut off.

Phase 2's is "create a fully constrained rectangle, change its dimensions, and verify the
geometry updates". It exists three times, each closer to the user than the one before:

- `tests/sketch/SolverTest.cpp` builds the rectangle from lines and constraints, and checks the
  solver's count of freedoms after each one, down to zero.
- `tests/gui/SketchViewTest.cpp` draws it in the 3D view with the rectangle tool, dimensions it
  with the dimension tool by clicking and typing into the field that opens in the view, then
  double-clicks a value and types another.
- `tests/gui/SketchInterfaceTest.cpp` does the same through the window's commands, starting
  from New Sketch, and goes on to finish the sketch, save, close the window, open the file in a
  new one, open the sketch from the browser, and change a dimension there.

All three end by measuring the sketch: where its corners are, and the area its outline
encloses. None of them judges a picture.

Phase 3's is "build a dimensioned bracket, edit the first sketch, and verify downstream
features update or report a specific failure". The bracket is a plate 60 by 40 and 8 thick,
pulled from a fully dimensioned rectangle, with a hole of radius 6 cut through it and its four
upright edges rounded to 5. Its volume is known in closed form, `(w·h − (4 − π)·25 − π·36)·8`,
so every check is a measurement against a formula:

- `tests/document/FeatureTest.cpp` builds it from the document's API, widens the plate to 85,
  and checks the volume; then breaks features on purpose (a hole moved off the plate, an edge
  that no longer exists, a radius too large) and checks each says what is wrong and the rest of
  the model carries on.
- `tests/gui/ModelingInterfaceTest.cpp` builds it through the window: Pull from inside the
  sketch, a cut that LCAD guesses is a cut, edges picked for the fillet, the sketch opened from
  the browser and its dimension changed in the 3D view, the model following when the sketch is
  finished, a feature selected and edited from the timeline. It also drives the timeline with
  the pointer (dragging the marker, dragging an entry, refused moves), patterns, mirror,
  measure, construction planes and axes, holes, taper and up-to-face pulls, parameters, and
  saving and reopening.

Phase 4's is "import a STEP solid, offset a face, save, export STEP, and validate the result":

- `tests/document/DirectEditTest.cpp` does it from the document's API, and
  `tests/gui/InterchangeInterfaceTest.cpp` through the window; the exported STEP is read back,
  checked, and its volume compared with the closed-form value.

Phase 5's is "assemble two independently editable parts, constrain them, reopen the project,
and verify the placement survives":

- `tests/document/AssemblyTest.cpp` makes two parts in components, holds one on the other with a
  revolute joint turned 30° with limits, saves, opens the file again, and checks the joint, its
  values and limits, and where the part is; then makes the other part thicker and checks that
  the joint follows. The same file tests occurrences, copies, the other kinds of joint, joints
  that cannot hold, linked parts, and materials; `tests/document/LinksTest.cpp` tests linked
  files, their stamps, updating, and packaging.
- `tests/gui/AssemblyInterfaceTest.cpp` sets the joint up through the window, picking the faces
  as clicks in the 3D view would, and animates it.

## Sketch assertions

A sketch test asserts on three things, in this order of preference:

- **Measurements of the geometry**: positions, lengths, radii, the area of a closed outline.
  Positions that came from a click are compared to within a pixel's worth of the view; positions
  that came from a typed value or a constraint are compared to a millionth of a millimeter.
- **What the solver reports**: the number of degrees of freedom, and which curves are free. A
  constraint that is "met" but takes away the wrong number of freedoms is a bug the geometry
  alone does not show. The tangent constraint had exactly that fault.
- **What was drawn**, only for what a picture is the specification of: that geometry which can
  still move is in one color and fully constrained geometry in another. The sketch is drawn on
  a widget of its own, which the tests read back and probe at the places where the geometry is.

Refusals are tested as much as successes: a redundant or conflicting constraint, an impossible
dimension value, and text that is not a number each have to leave the sketch exactly as it was,
add no undo step, and say why.

## Geometry assertions

A geometry test builds or loads a shape and measures it with `kernel`: volume, surface area,
center of mass, bounding box, counts of faces, edges, and vertices, and validity. All of these
work on the exact B-rep and never on a display mesh.

Every comparison of a measured number states its tolerance, using a named constant from
`tests/support/Tolerances.h`:

| Constant | Value | Use for |
|---|---|---|
| `lengthTolerance` | 1e-6 mm, absolute | Coordinates and lengths |
| `exactPropertyTolerance` | 1e-9, relative | Areas and volumes of shapes bounded only by planes, where the kernel's integration is exact up to rounding |
| `curvedPropertyTolerance` | 1e-9, relative | Areas and volumes of solids bounded by cylinders, spheres, cones, and tori |

Rules:

- Never compare measured values with `==`. The exception is a value that is exact by definition,
  such as the volume of a null shape being 0.
- Cylinders, spheres, cones, and tori turned out to need no looser tolerance: the kernel
  integrates these analytic surfaces to rounding too (errors near 1e-16 were measured). They
  have a constant of their own all the same. Spline surfaces will need a looser one, with its
  own name and reason; do not loosen an existing constant.
- Check validity and topology counts as well as size. A box with the right volume and seven faces
  is wrong.
- When a tolerance has to be loosened to make a test pass, find out why first. The reason belongs
  in the comment beside the constant.

## Reference models

`tests/models/format-1/` holds one document, written by LCAD built against different versions of
Open CASCADE. Every build reads every file there and checks it against closed-form values, so a
file saved on one distribution is proven to open on another. `tests/models/README.md` says how a
new file is added. These files are also the samples for format version 1: they stay when the
format changes, and from then on they test the migration.

## Interface tests

Interface tests run under `xvfb-run`: each test program gets a private X server and Mesa's
software renderer. Results are the same on a desktop and in CI, and no window appears on a real
screen. CTest sets this up; run them through `ctest`. Run by hand on a desktop they open real
windows, and the real pointer and window manager interfere with some of them.

They compare frames only in coarse ways (how much of the view is drawn, whether a frame
changed), never pixel-for-pixel against stored images, which would break with every driver
update. Where a number can be had instead of a picture, they use the number: the camera's
direction, where a model point lands on screen, and above all what a click selected, which is
then measured with the kernel. A test that clicks the middle of a box's top face asserts that
the selected face has an area of 6000 mm², not that some pixels turned blue.

Nine lessons are built into them:

- **Look at the screen, not only at the widget.** The first viewport rendered correctly into its
  own framebuffer while the window on screen was blank. `ApplicationTest` therefore reads the
  window back from the X server and compares it with what the widgets drew.
- **Qt reports graphics trouble as a warning and carries on.** Interface tests fail on any Qt
  warning that mentions OpenGL, a context, or a surface.
- **So does OCCT, on a channel of its own.** Interface tests fail on any warning, error, or
  OpenGL error OCCT reports (`tests/support/GraphicsMessages.h`). This caught OCCT making the
  OpenGL context current on a window that had already been destroyed, which no picture showed.
- **A frame can be one step behind.** OCCT reuses the last frame until told the scene changed,
  and a layout change reaches a widget only when the event loop runs. Tests that look at a
  frame after such a change wait for it.
- **A high-DPI display is a different display.** Everything OCCT draws at a fixed size on
  screen is measured in physical pixels. At twice the scale the view cube was half its size and
  sat where no click meant for it could land, and every test at the ordinary scale passed. So
  on Linux, CTest runs each interface test program twice, the second time with
  `QT_SCALE_FACTOR=2`.
- **Fonts are part of the environment.** OCCT warns when the font it wants for the labels in
  the 3D view is not installed and it has to substitute. Bare containers have almost no fonts,
  so the CI jobs for Arch and Fedora install DejaVu, as a desktop would have.
- **A projection rounded to whole pixels is not good enough to draw on.** OCCT's own function
  for "where is this point on screen" answers in whole pixels. A sketch drawn from that sat up
  to half a pixel off the view it was drawn over, and a click mapped back to the sketch landed a
  third of a millimeter from where it was aimed. Positions now go through the camera's own
  matrices in both directions, and a test maps points to the screen and back and expects them
  to within a millionth of a millimeter, on a slanted plane and after the view is turned.
- **A window that does not have the keyboard gets no shortcuts.** Under a virtual display with
  no window manager, a test window is shown but not active, and a key sent to it reaches the
  widget but not the shortcut that key is bound to. Tests that press a shortcut activate the
  window first and wait until it is.
- **A clean developer machine hides missing dependencies.** Both Ubuntu builds failed in CI on
  packages that happened to be installed locally. CI installs into a bare container using the
  same script the build instructions give.

Under XWayland the X server cannot be read back, so the screen comparison skips itself when the
test program is run by hand on a Wayland desktop. It always runs under `ctest`.

One test needs an ordinary user: a save into a folder that cannot be written to. Root can write
anywhere, so run as root that check does nothing and says so. CI's containers run as root, so
CI runs the tests as a user it creates for the purpose.

## Checking that a test can fail

A test that cannot fail proves nothing. When a test is added for a behavior, break the
behavior once and watch the test fail, then restore it. For a bug fix, write the test first and
see it fail on the bug.

This was done for the tests of Phases 0 and 1, by changing the code under test one line at a
time and running the tests. A few of the changes made, each of which fails at least one test:
not drawing the shape; discarding pointer positions; not clearing what could be redone after a
new edit; skipping the rollback of an abandoned transaction; restoring an undone deletion at
the end of the list; accepting a file in a newer format; ignoring a zip checksum; not giving
the saved file the permissions of the one it replaces; treating a cylinder's diameter as its
radius; giving OCCT the top-level window again; starting a command that is not enabled;
ranking an alias above a name in the search; accepting a shortcut that belongs to another
command; not saving the theme, the layout, or a tab's camera; opening a file that is already
open a second time; drawing the view cube at one size whatever the display's scale.

One change survived and the code was kept as it is, with a comment: the check after reading
geometry (an empty result, or a stream in a failed state) can be removed without a test
failing, because OCCT 7.6 throws on every damaged input tried, before that check is reached.

## Regression tests

Every fixed bug in modeling, recompute, or file handling gets a test that fails without the
fix. Name the test after what went wrong and mention the issue number in a comment.

## Sanitizers

The `asan` preset builds with AddressSanitizer and UndefinedBehaviorSanitizer and runs every
test, including the interface tests. `tests/lsan.supp` suppresses leak reports from inside two
third-party libraries (libdbus and the GTK accessibility bridge) that leak when no D-Bus session
is available, as in CI. Nothing in LCAD's own code is suppressed, and nothing should be: fix the
leak.

## Continuous integration

`.github/workflows/ci.yml` runs on every pull request and every push to `main`. Each job starts
from a bare container, runs `scripts/install-deps.sh`, builds with warnings treated as errors,
and runs all tests.

| Job | Compiler | Qt | OCCT |
|---|---|---|---|
| Ubuntu 24.04 | GCC 13.3 | 6.4.2 | 7.6.3 |
| Ubuntu 26.04 | GCC 15.2 | 6.10.2 | 7.9.2 |
| Fedora 44 | GCC 16.2 | 6.11.2 | 7.9.3 |
| Arch Linux | GCC 16.2 | 6.12.0 | 7.9.3 |
| Sanitizers (Ubuntu 24.04) | GCC 13.3 | 6.4.2 | 7.6.3 |
| Windows (Server 2025) | Visual Studio 2026, MSVC 19.51 | 6.10.2 | 7.9.3 |
| macOS (15, Apple silicon) | Clang 21.1 | 6.10.2 | 7.9.3 |

Versions are those the jobs installed on 2026-10-12, and on 2026-10-30 for Windows and macOS. Arch
and Fedora move forward on their own, so these jobs also give early warning when a new Qt, OCCT,
or compiler breaks the build. Windows and macOS start from GitHub's images and take their
libraries from conda-forge. Their screens are about 1024 by 768 and cannot be made larger, so the
interface tests run at the sizes that fit, and only once, at the screen's own scale. Windows has
no graphics card, and runs on Mesa's software OpenGL; macOS has Apple's software renderer.

Two more jobs cover the package. **AppImage** builds it on Ubuntu 24.04. **AppImage runs on
Fedora 44** starts it in a container with neither Qt nor Open CASCADE installed, gives it a
document to open, and checks that it stays up, draws the model, and logs no warnings
(`scripts/smoke-test-appimage.sh`).

The Windows and macOS jobs do the same for their packages, which have no separate machine to go
to. `scripts/smoke-test-windows.ps1` runs the installer, and opens a document by its file type
from outside the conda environment. `scripts/smoke-test-macos.sh` takes LCAD.app from its disk
image and opens a document through the system, with the conda environment moved out of the way.
Each checks that LCAD stays up, says in its log that it opened the document, and logs no warnings.

Each run attaches the frames the interface tests rendered, the reference document each job
wrote with its own OCCT, the packages, and a screenshot of each running.
