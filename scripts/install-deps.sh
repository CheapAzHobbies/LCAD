#!/bin/sh
# Installs what is needed to build and test LCAD, using the distribution's package manager.
#
# Supported: Ubuntu, Debian, Linux Mint and other Debian derivatives; Fedora; Arch Linux and its
# derivatives. docs/BUILDING.md lists the versions that are tested.
#
# Usage: scripts/install-deps.sh [--yes]
#   --yes   install without asking for confirmation (for CI and containers)
set -eu

assume_yes=false
for arg in "$@"; do
    case "$arg" in
        -y | --yes) assume_yes=true ;;
        -h | --help)
            sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "install-deps.sh: unknown option: $arg" >&2
            exit 2
            ;;
    esac
done

if [ "$(id -u)" -eq 0 ]; then
    sudo=""
elif command -v sudo >/dev/null 2>&1; then
    sudo="sudo"
else
    echo "install-deps.sh: run this as root, or install sudo." >&2
    exit 1
fi

if [ ! -r /etc/os-release ]; then
    echo "install-deps.sh: cannot identify this distribution (no /etc/os-release)." >&2
    exit 1
fi
# shellcheck source=/dev/null
. /etc/os-release
family=" ${ID:-} ${ID_LIKE:-} "

case "$family" in
    *" debian "* | *" ubuntu "*)
        # Toolchain; Qt 6; Open CASCADE (kernel and viewer); test framework; and, for the GUI
        # tests, a virtual X server with Mesa's software renderer.
        packages="
            build-essential cmake ninja-build
            qt6-base-dev
            libocct-foundation-dev libocct-modeling-data-dev libocct-modeling-algorithms-dev
            libocct-visualization-dev
            catch2 libxcb1-dev
            xvfb xauth libgl1-mesa-dri"
        $sudo apt-get update
        # --no-upgrade: only add what is missing. Without it, apt would upgrade any listed
        # package that is already installed, and with it the rest of the graphics stack.
        if $assume_yes; then
            # shellcheck disable=SC2086
            DEBIAN_FRONTEND=noninteractive $sudo apt-get install -y --no-upgrade $packages
        else
            # shellcheck disable=SC2086
            $sudo apt-get install --no-upgrade $packages
        fi
        ;;
    *" fedora "*)
        packages="
            gcc-c++ cmake ninja-build
            qt6-qtbase-devel
            opencascade-devel
            catch-devel libxcb-devel
            xorg-x11-server-Xvfb xorg-x11-xauth mesa-dri-drivers mesa-libGL"
        if $assume_yes; then
            # shellcheck disable=SC2086
            $sudo dnf install -y $packages
        else
            # shellcheck disable=SC2086
            $sudo dnf install $packages
        fi
        ;;
    *" arch "*)
        packages="
            base-devel cmake ninja
            qt6-base
            opencascade
            catch2 libxcb
            xorg-server-xvfb xorg-xauth mesa"
        # No -y here: refreshing the package database without a full upgrade leaves an Arch
        # system partially upgraded. Run 'pacman -Syu' first if packages cannot be found.
        if $assume_yes; then
            # shellcheck disable=SC2086
            $sudo pacman -S --needed --noconfirm $packages
        else
            # shellcheck disable=SC2086
            $sudo pacman -S --needed $packages
        fi
        ;;
    *)
        echo "install-deps.sh: unsupported distribution: ${PRETTY_NAME:-$family}" >&2
        echo "See docs/BUILDING.md for the list of dependencies to install by hand." >&2
        exit 1
        ;;
esac

echo "LCAD build dependencies are installed."
