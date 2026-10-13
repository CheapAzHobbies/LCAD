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
| Unit | Catch2 | `unit` | No | `core`: the log. `kernel`: primitives, measurement, moving, sub-shapes, serialization, errors, message routing. `document`: the model, undo, transactions, the file format |
| Save and reopen | Catch2 | `unit` | No | A reopened document measures the same as the one saved; damaged, foreign, and newer files are refused |
| Reference models | Catch2 | `unit` | No | The documents in `tests/models/` open and measure correctly, whichever OCCT wrote them |
| Interface | Qt Test | `gui` | A virtual one | The rendering path, camera, selection, the main window, commands, and the acceptance test through the interface |
| Sanitizers | All of the above, built with ASan and UBSan | all | A virtual one | Memory errors, leaks, undefined behavior |
| Package | A shell script, in CI | | A virtual one | The AppImage starts on another distribution and draws a document |

Planned, each when the code it tests arrives:

| Kind | Phase | What it checks |
|---|---|---|
| Solver | 2 | Constraint systems with known solutions; under-, fully, and overconstrained states |
| Recompute | 3 | Edit an early feature or parameter, then measure the result |
| Migration | With the first format change | Every older format version still opens |
| Round trip | 4 | Export to STEP, import again, compare measurements |
| Benchmarks | 3 onward | Time and memory for simple parts, long histories, large imports, dense meshes, large assemblies |

## The acceptance test of a phase

Each phase ends with the acceptance test the roadmap gives it, automated. Phase 1's is "create
a document, add a solid, save, close, reopen, and verify the geometry is identical". It exists
twice: in `tests/document/FileTest.cpp` against the document module alone, and in
`tests/gui/ApplicationTest.cpp` through the interface, by triggering the Create action, typing
sizes into the panel, pressing OK, saving, destroying the window, and opening the file in a new
one. Both compare measurements of the exact geometry with closed-form values.

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

Six lessons are built into them:

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
- **Fonts are part of the environment.** OCCT warns when the font it wants for the labels in
  the 3D view is not installed and it has to substitute. Bare containers have almost no fonts,
  so the CI jobs for Arch and Fedora install DejaVu, as a desktop would have.
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
radius; giving OCCT the top-level window again.

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

Versions are those the jobs installed on 2026-10-12. Arch and Fedora move forward on their own,
so these jobs also give early warning when a new Qt, OCCT, or compiler breaks the build.

Two more jobs cover the package. **AppImage** builds it on Ubuntu 24.04. **AppImage runs on
Fedora 44** starts it in a container with neither Qt nor Open CASCADE installed, gives it a
document to open, and checks that it stays up, draws the model, and logs no warnings
(`scripts/smoke-test-appimage.sh`).

Each run attaches the frames the interface tests rendered, the reference document each job
wrote with its own OCCT, the AppImage, and a screenshot of it running.
