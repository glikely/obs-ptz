#!/bin/bash
# Runs tests/obs-integration on macOS.
#
# The suite gives OBS an empty profile in a temporary home directory, so a
# run never reads or changes your real OBS profile or its camera config.
# OBS on macOS finds that directory through a system call that ignores
# $HOME, so conftest.py sets CFFIXED_USER_HOME, which it does honor. The
# plugin is not in the temporary home, so it is symlinked in from
# PTZSIM_PLUGIN_BUNDLE (set here to this checkout's build).
#
# Usage: scripts/run-macos-integration-tests.sh [pytest args...]
#
# See tests/obs-integration/README.md for what the suite actually does,
# and Linux (e.g. the Ubuntu VM) for a run that needs none of this.
#
# Env overrides:
#   PTZSIM_OBS_APP   the OBS app bundle to run (default: the newest "/Applications/OBS <version>.app")

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV="${XDG_CACHE_HOME:-$HOME/Library/Caches}/obs-ptz-macos-integration-venv"

if [ "$#" -eq 0 ]; then
    set -- -v
fi

OBS_APP="${PTZSIM_OBS_APP:-$(ls -d /Applications/OBS\ [0-9]*.app 2>/dev/null | sort -V | tail -1)}"
OBS_BINARY="$OBS_APP/Contents/MacOS/OBS"
if [ ! -x "$OBS_BINARY" ]; then
    echo "error: OBS not found at $OBS_BINARY (set PTZSIM_OBS_APP to override)" >&2
    exit 1
fi

# Your own OBS can stay open, but a second one makes this one's start-up
# ask whether to launch anyway, and nothing here can answer it.
if pgrep -x OBS >/dev/null 2>&1; then
    echo "error: OBS is already running -- quit it first" >&2
    exit 1
fi

echo "==> Setting up the Python test environment"
if ! "$VENV/bin/python" -c "import pytest, websockets" >/dev/null 2>&1; then
    # --clear: start over if there's a half-broken venv already there
    python3 -m venv --clear "$VENV"
    "$VENV/bin/pip" install -q -r "$REPO_ROOT/tests/obs-integration/requirements.txt"
fi

echo "==> Running the integration tests"
cd "$REPO_ROOT"
PTZSIM_PLUGIN_BUNDLE="$REPO_ROOT/build_macos/rundir/RelWithDebInfo/obs-ptz.plugin" \
    PTZSIM_OBS_BINARY="$OBS_BINARY" \
    "$VENV/bin/pytest" tests/obs-integration "$@"
