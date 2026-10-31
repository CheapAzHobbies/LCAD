#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds LCAD.app, with Qt, Open CASCADE, and the other libraries it needs inside it, and a disk
# image to hand it out on.
#
# Usage: scripts/build-macos-dmg.sh [output-directory] [build-directory]
#   The default output directory is build/macos. The result is LCAD-<version>-macos-<arch>.dmg.
#   Without a build directory LCAD is built afresh, in the output directory; with one that is
#   already configured and built, as CI has, that one is used.
#
# Needs what LCAD builds with on macOS (docs/BUILDING.md), with its conda environment active,
# and Xcode's command line tools for codesign and hdiutil.
#
# The app is signed ad hoc, which is not an identity: macOS asks before opening it the first
# time (open it from the context menu). LCAD has no Apple developer identity yet.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-"$root/build/macos"}
mkdir -p "$out"
out=$(cd "$out" && pwd)
work="$out/work"
build=${2:-}

# Everything below is generated, so start from nothing.
rm -rf "$work"
if [ -z "$build" ]; then
    build="$work/build"
    cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
        ${CONDA_PREFIX:+"-DCMAKE_PREFIX_PATH=$CONDA_PREFIX"}
    cmake --build "$build"
fi

# Installing runs macdeployqt, which copies the libraries into the bundle.
cmake --install "$build" --prefix "$work/stage"
codesign --force --deep --sign - "$work/stage/LCAD.app"

version=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$build/CMakeCache.txt")
dmg="LCAD-$version-macos-$(uname -m).dmg"
mkdir -p "$work/image"
cp -R "$work/stage/LCAD.app" "$work/image/"
ln -s /Applications "$work/image/Applications"
rm -f "$out/$dmg"
hdiutil create -volname LCAD -srcfolder "$work/image" -format UDZO "$out/$dmg"

echo "Built $out/$dmg"
