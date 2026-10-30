# Building LCAD

## Quick start

On Linux; see [Windows](#windows) and [macOS](#macos) for those.

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

## Supported systems

CI builds and tests LCAD on Windows (Windows Server 2025, Visual Studio 2022) and macOS (macOS 15
on Apple silicon), with conda-forge's Qt and Open CASCADE 7.9, and on these Linux
distributions, from a bare install, on every change:

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

### The Flatpak

`packaging/flatpak/io.github.cheapazhobbies.LCAD.yml` builds LCAD on KDE's runtime from
Flathub, which brings Qt; Open CASCADE 7.9.3, libzip, nlohmann/json, and Eigen are built from
source on the way.

```sh
sudo apt install flatpak flatpak-builder   # names are Ubuntu's
scripts/build-flatpak.sh                   # writes build/flatpak/LCAD-<version>.flatpak
flatpak install --user build/flatpak/LCAD-<version>.flatpak
flatpak run io.github.cheapazhobbies.LCAD
```

The first build takes most of an hour, nearly all of it Open CASCADE; flatpak-builder keeps what
it built for the next time. The Flatpak runs on X11 or XWayland (`--socket=x11`; see Wayland
below) and may read and write the home folder, where documents and the parts they link to are
opened by their paths. Its log and its copies of unsaved work are under
`~/.var/app/io.github.cheapazhobbies.LCAD/.local/state/lcad/`.

## Windows

LCAD builds with Visual Studio 2022 and takes Qt, Open CASCADE, and the smaller libraries from
[conda-forge](https://conda-forge.org/), where they come built for Windows. You need:

- Visual Studio 2022 (the free Community edition is enough) with the **Desktop development with
  C++** workload, which brings CMake and Ninja as well
- [micromamba](https://mamba.readthedocs.io/en/latest/installation/micromamba-installation.html)
  (or conda or mamba) for the libraries
- Git

In a **Developer PowerShell for VS 2022**:

```powershell
git clone https://github.com/CheapAzHobbies/LCAD.git
cd LCAD
micromamba create -f packaging/conda/environment.yml
micromamba activate lcad
cmake --preset release "-DCMAKE_PREFIX_PATH=$env:CONDA_PREFIX/Library"
cmake --build --preset release
build/release/bin/lcad.exe
```

The program finds Qt and the other libraries through the environment's `Library\bin` folder,
which `micromamba activate` puts on the `PATH`. `ctest --preset release` runs the tests the same
way. The 3D view needs OpenGL 2.1 or newer: a graphics driver gives that; a virtual machine
without one does not, and CI puts Mesa's software OpenGL (`opengl32.dll` and
`libgallium_wgl.dll` from [mesa-dist-win](https://github.com/pal1000/mesa-dist-win)) beside the
tests instead.

### The installer

With [Inno Setup 6](https://jrsoftware.org/isinfo.php) installed
(`winget install JRSoftware.InnoSetup`), in the same shell:

```powershell
scripts/build-windows-installer.ps1      # writes build\windows\LCAD-<version>-windows-x64-setup.exe
```

It builds LCAD afresh, gathers the program, the libraries it uses, Visual C++'s runtime, and
Qt's plugins, and makes an installer of them. The installer installs for the current user, or
for everyone when run as an administrator; adds LCAD to the Start menu; and, unless told not to,
makes LCAD what opens `.lcad` documents. It is not signed, so Windows SmartScreen asks before
running it the first time.

## macOS

LCAD builds on macOS with conda-forge's Clang and libraries, so that it uses the same C++
library as Qt and Open CASCADE do. You need Xcode's command line tools
(`xcode-select --install`) and micromamba (`brew install micromamba`, or see its site).

```sh
git clone https://github.com/CheapAzHobbies/LCAD.git
cd LCAD
micromamba create -f packaging/conda/environment.yml cxx-compiler
micromamba activate lcad
cmake --preset release -DCMAKE_PREFIX_PATH="$CONDA_PREFIX"
cmake --build --preset release
open build/release/bin/LCAD.app
```

### The disk image

```sh
scripts/build-macos-dmg.sh               # writes build/macos/LCAD-<version>-macos-<arch>.dmg
```

It builds LCAD.app afresh, copies Qt, Open CASCADE, and the other libraries into it, and puts
it on a disk image beside a link to Applications. The app is signed ad hoc, not with an Apple
developer identity, and not notarized: the first time, open it from its context menu (Open),
and macOS asks once whether to trust it. CI builds it for Apple silicon (arm64).

## Wayland

LCAD runs on Wayland desktops through XWayland, and selects this itself. The Open CASCADE
packages that distributions ship can only draw through X11; see
[ARCHITECTURE.md](ARCHITECTURE.md#viewport).

If the 3D view shows a message saying it could not use the OpenGL context, `QT_QPA_PLATFORM` is
probably set to `wayland` in your environment. Start LCAD with `QT_QPA_PLATFORM=xcb lcad`.

## Code style

Format C++ with `clang-format` before committing; the rules are in `.clang-format`.
