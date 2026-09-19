#!/bin/bash
# Runs tests/obs-integration on macOS.
#
# Real OBS on macOS ignores every isolation mechanism the test suite
# relies on: setting $HOME (it resolves its config dir through a
# Qt/Cocoa API that doesn't consult it), --collection/--profile CLI
# flags, and even editing global.ini's [Basic] section directly (a
# separate, newer user.ini file holds the actual "last used" pointer
# and silently overrides it). Every launch loads your real, live OBS
# profile regardless -- including plugin_config/obs-ptz/config.json,
# which likely has real camera devices in it on a machine you actually
# use OBS on. Running pytest against that directly would overwrite your
# real device config and could send live PTZ commands to real hardware
# if a device ID collided.
#
# This script works around all of that:
#   1. builds and installs the plugin,
#   2. backs up the whole ~/Library/Application Support/obs-studio,
#   3. renames the real "Untitled" profile/scene collection aside so
#      OBS boots into a genuinely fresh, empty one instead (its normal
#      first-run behavior -- confirmed no blocking dialog),
#   4. runs pytest through macos-integration-test-obs-wrapper.py, which
#      redirects obs-ptz's and obs-websocket's config to the real paths
#      the wrapper's env-var overrides get ignored at,
#   5. restores everything from the backup afterward, unconditionally
#      -- even if the run crashes, is interrupted, or OBS hangs.
#
# Usage: scripts/run-macos-integration-tests.sh [--skip-build] [pytest args...]
#
# See tests/obs-integration/README.md for what the suite actually does,
# and Linux (e.g. the Ubuntu VM) for a simpler run if you don't
# specifically need to test macOS behavior -- $HOME isolation actually
# works there, matching CI, with none of the above.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OBS_APP="${PTZSIM_OBS_APP:-/Applications/OBS.app}"
OBS_BINARY="$OBS_APP/Contents/MacOS/OBS"
CFG="$HOME/Library/Application Support/obs-studio"
VENV="${TMPDIR:-/tmp}/obs-ptz-macos-integration-venv"
WRAPPER="$REPO_ROOT/scripts/macos-integration-test-obs-wrapper.py"

SKIP_BUILD=0
if [ "${1:-}" = "--skip-build" ]; then
    SKIP_BUILD=1
    shift
fi
if [ "$#" -eq 0 ]; then
    set -- -v
fi

if [ ! -x "$OBS_BINARY" ]; then
    echo "error: OBS not found at $OBS_BINARY (set PTZSIM_OBS_APP to override)" >&2
    exit 1
fi

if pgrep -f "$OBS_BINARY" >/dev/null 2>&1; then
    echo "error: OBS is already running -- quit it first (this script needs to control its config/profile)" >&2
    exit 1
fi

if [ "$SKIP_BUILD" -eq 0 ]; then
    echo "==> Building the plugin (cmake --preset macos)"
    cmake --preset macos -DENABLE_SERIALPORT=ON -DENABLE_ONVIF=ON
    cmake --build --preset macos
else
    echo "==> Skipping build (--skip-build)"
fi

echo "==> Installing the plugin bundle"
PLUGIN_DIR="$HOME/Library/Application Support/obs-studio/plugins"
mkdir -p "$PLUGIN_DIR"
rm -rf "$PLUGIN_DIR/obs-ptz.plugin"
cp -R "$REPO_ROOT/build_macos/RelWithDebInfo/obs-ptz.plugin" "$PLUGIN_DIR/obs-ptz.plugin"
xattr -cr "$PLUGIN_DIR/obs-ptz.plugin"

echo "==> Setting up the Python test environment"
if [ ! -x "$VENV/bin/pytest" ]; then
    python3 -m venv "$VENV"
    "$VENV/bin/pip" install -q -r "$REPO_ROOT/tests/obs-integration/requirements.txt"
fi

BACKUP="$(mktemp -d "${TMPDIR:-/tmp}/obs-ptz-obs-studio-backup.XXXXXX")"
echo "==> Backing up $CFG to $BACKUP"
mkdir -p "$CFG"
rsync -a "$CFG/" "$BACKUP/"

restore() {
    echo "==> Restoring your real OBS profile from $BACKUP"
    pkill -TERM -f "$OBS_BINARY" 2>/dev/null || true
    for _ in 1 2 3 4 5; do
        pgrep -f "$OBS_BINARY" >/dev/null 2>&1 || break
        sleep 1
    done
    pkill -9 -f "$OBS_BINARY" 2>/dev/null || true
    sleep 1
    rsync -a --delete "$BACKUP/" "$CFG/"
    rm -rf "$BACKUP"
    echo "==> Restore complete"
}
trap restore EXIT

# Clear any stale unclean-shutdown sentinel left by a previous crashed
# run, so OBS doesn't block on a "Safe Mode" dialog this script can't
# dismiss (no Accessibility/Screen Recording permission in this kind of
# environment -- see the top-level CLAUDE.md's macOS GUI notes).
rm -f "$CFG/.sentinel/"* 2>/dev/null || true

echo "==> Moving your real profile/scene collection aside so OBS boots fresh"
[ -e "$CFG/basic/profiles/Untitled" ] && mv "$CFG/basic/profiles/Untitled" "$CFG/basic/profiles/Untitled.realbackup"
[ -e "$CFG/basic/scenes/Untitled.json" ] && mv "$CFG/basic/scenes/Untitled.json" "$CFG/basic/scenes/Untitled.json.realbackup"
[ -e "$CFG/basic/scenes/Untitled.json.bak" ] && mv "$CFG/basic/scenes/Untitled.json.bak" "$CFG/basic/scenes/Untitled.json.bak.realbackup"
[ -e "$CFG/basic/scenes/Untitled.json.v1" ] && mv "$CFG/basic/scenes/Untitled.json.v1" "$CFG/basic/scenes/Untitled.json.v1.realbackup"

echo "==> Running the integration tests"
cd "$REPO_ROOT"
PTZSIM_OBS_BINARY="$WRAPPER" PTZSIM_REAL_OBS_BINARY="$OBS_BINARY" "$VENV/bin/pytest" tests/obs-integration "$@"
