#!/bin/bash
# Set up, build and run obs-ptz on macOS from any checkout, including a
# git worktree. Every step is idempotent: re-running does only what is
# missing or stale.
#
# Usage (run from anywhere; it locates the repo root from its own path):
#   scripts/macos-dev.sh setup     # clone .deps, configure, build, sign
#   scripts/macos-dev.sh install   # setup, then point OBS's plugin dir at this build
#   scripts/macos-dev.sh run       # install, then launch OBS
#   scripts/macos-dev.sh restore   # put back the plugin install() moved aside
#   scripts/macos-dev.sh test [pytest args]   # setup with the test options, then run tests/obs-integration
#
# Why .deps is cloned: a fresh worktree has no .deps, so the first configure
# would re-download and rebuild every dependency. A worktree gets an APFS
# copy-on-write clone of the main checkout's .deps instead, which takes
# no extra disk space and is not shared afterwards. It can't be a symlink:
# the OBS and SDL build directories in .deps cache absolute paths, so
# worktrees sharing one .deps fail each other's configure ("The current
# CMakeCache.txt directory ... is different"). The clone's own copies of
# those caches are deleted so they are made again with this checkout's paths.
# The main checkout needs no clone.
#
# Why configure pins SDL2_DIR and obs-frontend-api_DIR: a .deps that the
# Windows builds also install into has Windows-layout packages at
# .deps/sdl/cmake and .deps/cmake, which a fresh configure finds before
# the macOS ones and turns into Windows link flags (-lkernel32 ...).
#
# test hands off to scripts/run-macos-integration-tests.sh, which runs OBS on
# an empty profile in a temporary home directory, so your real one is not
# touched. Quit OBS first. test-build is the setup it needs, without the run.
#
# Env overrides:
#   OBS_APP   the OBS app bundle to launch (default: newest "/Applications/OBS <version>.app")
#   CONFIG    build configuration (default RelWithDebInfo)

set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build_macos"
CONFIG="${CONFIG:-RelWithDebInfo}"
PLUGIN="$BUILD/rundir/$CONFIG/obs-ptz.plugin"
OBS_DATA="$HOME/Library/Application Support/obs-studio"
INSTALLED="$OBS_DATA/plugins/obs-ptz.plugin"
# Outside plugins/ so OBS never scans it, and outside the bundle so codesign
# doesn't see it as a stray subcomponent.
BACKUP_DIR="$OBS_DATA/obs-ptz-plugin-backup"

log() { echo "==> $*"; }
die() { echo "error: $*" >&2; exit 1; }

main_checkout() {
    # The common git dir lives in the main checkout, which is also where
    # worktrees were created from.
    local common
    common="$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)"
    dirname "$common"
}

setup_deps() {
    local main
    main="$(main_checkout)"
    if [ "$main" = "$ROOT" ]; then
        return
    fi
    if [ -L "$ROOT/.deps" ]; then
        log "replacing the .deps symlink with a clone"
        rm "$ROOT/.deps"
    fi
    [ ! -e "$ROOT/.deps" ] || return 0
    [ -d "$main/.deps" ] || die "$main/.deps does not exist; build the main checkout once first"
    log "cloning $main/.deps"
    # Clone to a temporary name so an interrupted copy is not mistaken for a finished one.
    rm -rf "$ROOT/.deps.partial"
    cp -c -R "$main/.deps" "$ROOT/.deps.partial"
    # CMake build directories in there cache absolute paths from whichever
    # checkout made them.
    find "$ROOT/.deps.partial" -maxdepth 3 -name CMakeCache.txt -print0 |
        while IFS= read -r -d '' cache; do rm -rf "$(dirname "$cache")"; done
    mv "$ROOT/.deps.partial" "$ROOT/.deps"
}

cache_value() { sed -n "s/^$1:[A-Z]*=//p" "$BUILD/CMakeCache.txt" 2>/dev/null; }

setup_configure() {
    local deps sdl api
    deps="$(cd "$ROOT/.deps" && pwd -P)"
    sdl="$deps/sdl/lib/cmake/SDL2"
    api="$deps/lib/cmake/obs-frontend-api"
    # Extra options (from `test`) always reconfigure, which is quick.
    if [ "$#" -eq 0 ] && [ "$(cache_value CMAKE_HOME_DIRECTORY)" = "$ROOT" ] &&
        [ "$(cache_value SDL2_DIR)" = "$sdl" ] &&
        [ "$(cache_value obs-frontend-api_DIR)" = "$api" ]; then
        return
    fi
    log "configuring $BUILD"
    (cd "$ROOT" && cmake --preset macos -DSDL2_DIR="$sdl" -Dobs-frontend-api_DIR="$api" "$@")
}

setup_build() {
    log "building ($CONFIG)"
    if command -v xcbeautify >/dev/null; then
        # pipefail keeps a failed build from being masked by xcbeautify's exit status.
        set -o pipefail
        cmake --build "$BUILD" --config "$CONFIG" --parallel 2>&1 | xcbeautify
        set +o pipefail
    else
        cmake --build "$BUILD" --config "$CONFIG" --parallel
    fi
    # OBS refuses to dlopen an unsigned plugin ("missing code signature").
    # Signing is quick and the build re-links the binary, so always redo it.
    log "ad-hoc signing"
    codesign --force --sign - --timestamp=none -o runtime --generate-entitlement-der "$PLUGIN"
}

# Extra arguments are passed to cmake when configuring.
do_setup() {
    setup_deps
    setup_configure "$@"
    setup_build
}

do_install() {
    do_setup
    mkdir -p "$OBS_DATA/plugins"
    if [ -L "$INSTALLED" ]; then
        [ "$(readlink "$INSTALLED")" = "$PLUGIN" ] && { log "already installed"; return; }
        log "re-pointing the installed plugin symlink"
        rm "$INSTALLED"
    elif [ -e "$INSTALLED" ]; then
        [ ! -e "$BACKUP_DIR/obs-ptz.plugin" ] || die "$BACKUP_DIR/obs-ptz.plugin already exists; run 'restore' or move it"
        log "moving the existing plugin to $BACKUP_DIR"
        mkdir -p "$BACKUP_DIR"
        mv "$INSTALLED" "$BACKUP_DIR/obs-ptz.plugin"
    fi
    ln -s "$PLUGIN" "$INSTALLED"
    log "installed (symlink to this build)"
}

do_restore() {
    [ -e "$BACKUP_DIR/obs-ptz.plugin" ] || die "no backup at $BACKUP_DIR/obs-ptz.plugin"
    [ ! -e "$INSTALLED" ] || [ -L "$INSTALLED" ] || die "$INSTALLED is a real directory; not overwriting"
    rm -f "$INSTALLED"
    mv "$BACKUP_DIR/obs-ptz.plugin" "$INSTALLED"
    log "restored the original plugin"
}

find_obs() {
    if [ -n "${OBS_APP:-}" ]; then
        echo "$OBS_APP"
        return
    fi
    # Not `open -a OBS`: Launch Services can resolve that to a stray
    # locally built OBS with different bundled Qt.
    local app
    app="$(ls -d /Applications/OBS\ [0-9]*.app 2>/dev/null | sort -V | tail -1)"
    [ -n "$app" ] || die "no /Applications/OBS <version>.app found; set OBS_APP"
    echo "$app"
}

do_run() {
    do_install
    local app
    app="$(find_obs)"
    # A second launch races the first and can leave a stale .sentinel file,
    # which blocks the next start on a Safe Mode dialog.
    ! pgrep -x OBS >/dev/null || die "OBS is already running; quit it first"
    log "launching $app"
    open "$app"
}

# Setup with the options the integration tests need.
do_test_build() {
    do_setup -DENABLE_SERIALPORT=ON -DENABLE_ONVIF=ON -DENABLE_UI_TESTS=ON
}

do_test() {
    do_test_build
    PTZSIM_OBS_APP="$(find_obs)" "$ROOT/scripts/run-macos-integration-tests.sh" "$@"
}

cmd="${1:-}"
[ "$#" -eq 0 ] || shift
case "$cmd" in
    setup) do_setup ;;
    install) do_install ;;
    run) do_run ;;
    restore) do_restore ;;
    test) do_test "$@" ;;
    test-build) do_test_build ;;
    *) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//' && exit 2 ;;
esac
