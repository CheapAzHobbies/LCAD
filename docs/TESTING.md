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
| Unit | Catch2 | `unit` | No | `kernel`: primitives, measurement, validity, the `Shape` value |
| Interface | Qt Test | `gui` | A virtual one | The rendering path, pointer input, the main window |
| Sanitizers | Both, built with ASan and UBSan | all | A virtual one | Memory errors, leaks, undefined behavior |

Planned, each when the code it tests arrives:

| Kind | Phase | What it checks |
|---|---|---|
| Solver | 2 | Constraint systems with known solutions; under-, fully, and overconstrained states |
| Recompute | 3 | Edit an early feature or parameter, then measure the result |
| Save and reopen | 1 | A reopened document measures the same as the one saved; every older format version still opens |
| Round trip | 4 | Export to STEP, import again, compare measurements |
| Reference models | 1 onward | Files in `tests/models/` with their expected measurements beside them |
| Benchmarks | 3 onward | Time and memory for simple parts, long histories, large imports, dense meshes, large assemblies |

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

Rules:

- Never compare measured values with `==`. The exception is a value that is exact by definition,
  such as the volume of a null shape being 0.
- Curved shapes are integrated numerically and need a looser tolerance. Add a named constant for
  it, with the reason, when the first curved primitive is tested. Do not loosen an existing one.
- Check validity and topology counts as well as size. A box with the right volume and seven faces
  is wrong.
- When a tolerance has to be loosened to make a test pass, find out why first. The reason belongs
  in the comment beside the constant.

## Interface tests

Interface tests run under `xvfb-run`: each test program gets a private X server and Mesa's
software renderer. Results are the same on a desktop and in CI, and no window appears on a real
screen. CTest sets this up; run them through `ctest`.

What they establish is narrow: the rendering path works, input reaches the viewer, and the
window is assembled correctly. They compare frames only in coarse ways (how much of the view is
drawn, whether a frame changed), never pixel-for-pixel against stored images, which would break
with every driver update.

Three lessons from Phase 0 are built into them:

- **Look at the screen, not only at the widget.** The first viewport rendered correctly into its
  own framebuffer while the window on screen was blank. `ApplicationTest` therefore reads the
  window back from the X server and compares it with what the widgets drew.
- **Qt reports graphics trouble as a warning and carries on.** Interface tests fail on any Qt
  warning that mentions OpenGL, a context, or a surface.
- **A clean developer machine hides missing dependencies.** Both Ubuntu builds failed in CI on
  packages that happened to be installed locally. CI installs into a bare container using the
  same script the build instructions give.

Under XWayland the X server cannot be read back, so the screen comparison skips itself when the
test program is run by hand on a Wayland desktop. It always runs under `ctest`.

## Checking that a test can fail

A test that cannot fail proves nothing. When a test is added for a behavior, break the
behavior once and watch the test fail, then restore it. For a bug fix, write the test first and
see it fail on the bug.

This was done for the Phase 0 tests. Not drawing the shape fails the rendering tests; giving the
viewer the wrong native window fails the screen comparison and the warning check; discarding
pointer positions or wheel input fails all four input tests; a deliberate leak fails the
sanitizer run.

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

A run takes about four minutes and uses about twelve minutes of GitHub Actions time across the
five jobs. The frames the interface tests rendered are attached to each run as artifacts.
