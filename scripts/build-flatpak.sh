#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds LCAD as a Flatpak bundle: one file that installs LCAD with flatpak, on KDE's runtime
# from Flathub. Open CASCADE and the smaller libraries LCAD needs are built from source on the
# way, which takes a while the first time; flatpak-builder keeps them for the next build.
#
# Usage: scripts/build-flatpak.sh [output-directory]
#   The default output directory is build/flatpak. The result is LCAD-<version>.flatpak.
#   Install it with: flatpak install --user LCAD-<version>.flatpak
#
# Needs flatpak and flatpak-builder, and the Flathub remote for the runtime and the SDK.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-"$root/build/flatpak"}
mkdir -p "$out"
out=$(cd "$out" && pwd)
app=io.github.cheapazhobbies.LCAD
version=$(sed -n 's/^ *VERSION \([0-9.]*\)$/\1/p' "$root/CMakeLists.txt" | head -n 1)

for program in flatpak flatpak-builder; do
    if ! command -v "$program" >/dev/null 2>&1; then
        echo "build-flatpak.sh: $program is needed and was not found." >&2
        exit 1
    fi
done

flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak-builder --user --force-clean --install-deps-from=flathub \
    --state-dir="$out/state" --repo="$out/repo" \
    "$out/build" "$root/packaging/flatpak/$app.yml"
flatpak build-bundle --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
    "$out/repo" "$out/LCAD-$version.flatpak" "$app"
echo "Built $out/LCAD-$version.flatpak"
