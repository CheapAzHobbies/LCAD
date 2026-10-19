# Building LCAD

## Quick start

```sh
git clone https://github.com/CheapAzHobbies/LCAD.git
cd LCAD
scripts/install-deps.sh          # asks before installing; uses sudo
cmake --preset release
cmake --build --preset release
build/release/bin/lcad
```

To run the tests as well:

```sh
ctest --preset release
```

## Supported distributions

CI builds and tests LCAD on these, from a bare install, on every change:

| | Ubuntu 24.04 | Ubuntu 26.04 | Fedora 44 | Arch |
|---|---|---|---|---|
| Compiler | GCC 13.3 | GCC 15.2 | GCC 16.2 | GCC 16.2 |
| CMake | 3.28 | 4.2 | 4.3 | 4.4 |
| Qt | 6.4.2 | 6.10.2 | 6.11.2 | 6.12.0 |
| Open CASCADE | 7.6.3 | 7.9.2 | 7.9.3 | 7.9.3 |

Linux Mint 22 uses Ubuntu 24.04's packages. Other derivatives of Debian, Fedora, and Arch are
recognized by the install script and are expected to work if their packages are at least as new
as Ubuntu 24.04's. Clang 18 also builds LCAD without warnings; CI does not cover it yet.

## Dependencies

`scripts/install-deps.sh` installs everything from the distribution's own packages. It only adds
packages that are missing and does not upgrade ones you already have. On Arch, run `pacman -Syu`
first if it reports packages it cannot find.

To install by hand, or on a distribution the script does not know, you need:

| Need | Ubuntu, Debian, Mint | Fedora | Arch |
|---|---|---|---|
| C++20 compiler, CMake 3.28+, Ninja | `build-essential cmake ninja-build` | `gcc-c++ cmake ninja-build` | `base-devel cmake ninja` |
| Qt 6.4+ | `qt6-base-dev` | `qt6-qtbase-devel` | `qt6-base` |
| Open CASCADE 7.6 to 7.9 | `libocct-foundation-dev libocct-modeling-data-dev libocct-modeling-algorithms-dev libocct-visualization-dev libocct-data-exchange-dev` | `opencascade-devel` | `opencascade` |
| File format: libzip, nlohmann/json | `libzip-dev nlohmann-json3-dev` | `libzip-devel json-devel` | `libzip nlohmann-json` |
| Sketch solver: Eigen 3.4 or 5 | `libeigen3-dev` | `eigen3-devel` | `eigen` |
| Tests: Catch2 3, xcb headers | `catch2 libxcb1-dev` | `catch-devel libxcb-devel` | `catch2 libxcb` |
| Tests: virtual display, software OpenGL | `xvfb xauth libgl1-mesa-dri` | `xorg-x11-server-Xvfb xorg-x11-xauth mesa-dri-drivers mesa-libGL` | `xorg-server-xvfb xorg-xauth mesa` |

The last two rows are only needed to build and run the tests. Configure with
`-DBUILD_TESTING=OFF` to build without them.

### Using your own Open CASCADE build

LCAD finds Open CASCADE by its headers and libraries. To use a build outside the system
directories, pass its install prefix:

```sh
cmake --preset release -DCMAKE_PREFIX_PATH=/opt/occt-7.9.3
```

Open CASCADE 8 is not supported yet. Configuring against it prints a warning, and the build is
not expected to succeed.

## Presets

| Preset | Use |
|---|---|
| `dev` | Debug build for everyday work |
| `release` | Optimized build |
| `ci` | What CI builds: optimized, with compiler warnings as errors |
| `asan` | Debug build with AddressSanitizer and UndefinedBehaviorSanitizer |

Each preset builds into `build/<preset>/`, with programs in `build/<preset>/bin/`. All four work
with `cmake --preset`, `cmake --build --preset`, and `ctest --preset`.

## Installing, and the AppImage

`cmake --install build/release --prefix ~/.local` installs the program together with its
launcher, icon, and the file type for `.lcad` documents.

To build LCAD as an AppImage, one file that carries Qt, Open CASCADE, and the other libraries
with it:

```sh
sudo apt install curl file qmake6      # besides the build dependencies; names are Ubuntu's
scripts/build-appimage.sh              # writes build/appimage/LCAD-<version>-x86_64.AppImage
```

The script downloads linuxdeploy and its Qt plugin at pinned versions and checks them against
recorded checksums. An AppImage runs on distributions at least as new as the one it was built
on, so CI builds it on Ubuntu 24.04; that one needs glibc 2.39 or newer, which rules out Ubuntu
22.04 and Debian 12. It uses the graphics driver, X11 libraries, and fonts of the system it
runs on.

## Wayland

LCAD runs on Wayland desktops through XWayland, and selects this itself. The Open CASCADE
packages that distributions ship can only draw through X11; see
[ARCHITECTURE.md](ARCHITECTURE.md#viewport).

If the 3D view shows a message saying it could not use the OpenGL context, `QT_QPA_PLATFORM` is
probably set to `wayland` in your environment. Start LCAD with `QT_QPA_PLATFORM=xcb lcad`.

## Code style

Format C++ with `clang-format` before committing; the rules are in `.clang-format`.
