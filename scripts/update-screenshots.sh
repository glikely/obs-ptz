#!/bin/bash
# Retakes the screenshots the documentation uses (docs/*.png) from this
# checkout, in the Parallels "Windows 11" VM, which has a real desktop to
# take them on (this Mac can't capture its own screen from a script).
#
# It builds this checkout for the VM and overlays it into
# C:\OBS-Test\<arch> (see scripts/vm-windows-dev.sh and
# tests/ui-harness/README.md for that setup), then has
# scripts/update-screenshots.py take the pictures in the VM, against a
# scripts/ptzsim it runs there. They land in docs/ here, over the old ones:
# look at them before committing.
#
# Usage: scripts/update-screenshots.sh [arm64|x64]     (default arm64)
#
# Needs python3 in the VM. Nothing on this Mac, or the network, is involved
# beyond the shared folder.

set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

"$ROOT/scripts/vm-windows-dev.sh" screenshots "${1:-arm64}"

echo "update-screenshots: done. Look at them, then commit:"
git -C "$ROOT" status --short docs
