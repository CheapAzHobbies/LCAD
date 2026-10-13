#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds LCAD as an AppImage: one file that holds LCAD together with Qt, Open CASCADE, and the
# other libraries it needs, and runs without installing anything.
#
# An AppImage runs on distributions at least as new as the one it was built on. CI builds the
# released one on Ubuntu 24.04, the oldest distribution LCAD supports.
#
# Usage: scripts/build-appimage.sh [output-directory]
#   The default output directory is build/appimage. The result is LCAD-<version>-x86_64.AppImage.
#
# Needs the build dependencies (scripts/install-deps.sh), plus curl, file, and qmake6. Downloads
# linuxdeploy and its Qt plugin from their GitHub releases, at the versions pinned below, and
# checks them against the checksums recorded here.
set -eu

linuxdeploy_url="https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage"
linuxdeploy_sha256="c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d"
plugin_url="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-x86_64.AppImage"
plugin_sha256="15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724"

root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-"$root/build/appimage"}
mkdir -p "$out"
out=$(cd "$out" && pwd)
tools="$out/tools"
work="$out/work"

for program in cmake ninja curl file sha256sum; do
    if ! command -v "$program" >/dev/null 2>&1; then
        echo "build-appimage.sh: $program is needed and was not found." >&2
        exit 1
    fi
done
qmake=$(command -v qmake6 || true)
if [ -z "$qmake" ]; then
    echo "build-appimage.sh: qmake6 is needed and was not found (Ubuntu: package qmake6)." >&2
    exit 1
fi

# fetch <url> <sha256> <file>: downloads unless a file with the right checksum is already there.
fetch() {
    if [ ! -f "$3" ] || ! echo "$2  $3" | sha256sum --check --status; then
        echo "Downloading $(basename "$3")"
        curl --fail --silent --show-error --location --output "$3" "$1"
    fi
    if ! echo "$2  $3" | sha256sum --check --status; then
        echo "build-appimage.sh: $(basename "$3") does not match its recorded checksum." >&2
        exit 1
    fi
    chmod +x "$3"
}

mkdir -p "$tools"
fetch "$linuxdeploy_url" "$linuxdeploy_sha256" "$tools/linuxdeploy-x86_64.AppImage"
fetch "$plugin_url" "$plugin_sha256" "$tools/linuxdeploy-plugin-qt-x86_64.AppImage"

# Everything below is generated, so start from nothing.
rm -rf "$work"
cmake -S "$root" -B "$work/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$work/build"
DESTDIR="$work/AppDir" cmake --install "$work/build"

version=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$work/build/CMakeCache.txt")
appimage="LCAD-$version-x86_64.AppImage"

# The tools are AppImages themselves. Unpacking them instead of mounting them works where FUSE
# is not available, as in a container.
export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE="$qmake"
export LINUXDEPLOY_OUTPUT_VERSION="$version"
export LDAI_OUTPUT="$appimage"
export OUTPUT="$appimage"
# linuxdeploy finds its plugins beside itself.
cd "$out"
rm -f "$appimage"
"$tools/linuxdeploy-x86_64.AppImage" \
    --appdir "$work/AppDir" \
    --desktop-file "$work/AppDir/usr/share/applications/io.github.cheapazhobbies.LCAD.desktop" \
    --plugin qt \
    --output appimage

echo "Built $out/$appimage"
