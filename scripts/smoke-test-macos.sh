#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Takes LCAD.app from its disk image as a user would, opens a document with it through the
# system, and checks that it stays up, opens the document, and logs no warnings or errors. CI
# runs this with the libraries LCAD was built with out of reach.
#
# Usage: scripts/smoke-test-macos.sh <LCAD.dmg> <document.lcad> <screenshot.png>
set -eu

if [ $# -ne 3 ]; then
    sed -n '8p' "$0" | sed 's/^# \{0,1\}//' >&2
    exit 2
fi
dmg=$1
document=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
screenshot=$3

work=$(mktemp -d)
mounted=""
log="$HOME/Library/Application Support/LCAD/lcad.log"
cleanup() {
    pkill -x LCAD 2>/dev/null || true
    if [ -n "$mounted" ]; then
        hdiutil detach "$mounted" -quiet || true
    fi
    rm -rf "$work"
}
trap cleanup EXIT

fail() {
    echo "smoke-test-macos.sh: $1" >&2
    if [ -f "$log" ]; then
        echo "--- its log:" >&2
        cat "$log" >&2
    fi
    exit 1
}

hdiutil attach "$dmg" -nobrowse -readonly -mountpoint "$work/image" -quiet
mounted="$work/image"
cp -R "$work/image/LCAD.app" "$work/"
hdiutil detach "$mounted" -quiet
mounted=""

rm -f "$log"
open -a "$work/LCAD.app" "$document"
sleep 20
pgrep -x LCAD >/dev/null || fail "LCAD exited within 20 seconds of starting."
screencapture -x "$screenshot" || fail "The screen could not be captured."

[ -f "$log" ] || fail "LCAD did not write its log."
grep -q "Opened .*$(basename "$document")" "$log" || fail "LCAD did not open the document."
if grep -E '^[^ ]+ (warning|error) ' "$log"; then
    fail "LCAD logged warnings or errors."
fi
echo "LCAD started from $(basename "$dmg"), opened $(basename "$document"), and is running."
