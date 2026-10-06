#!/bin/bash
# Build and run obs-ptz on the Parallels "Windows 11" VM, from whatever
# checkout (branch, worktree, even uncommitted changes) this script is run
# from on the Mac host. Companion to scripts/vm-linux-dev.sh -- same idea,
# different toolchain, and a genuinely different Windows testing model (see
# below), so the subcommands aren't a 1:1 match.
#
# Per tests/ui-harness/README.md's "Testing on Windows" section, the
# established rig is two persistent, dedicated OBS installs,
# C:\OBS-Test\arm64 and C:\OBS-Test\x64.
# scripts\windows-build-and-test.bat install obs-ptz into and runs
# tests\obs-integration with pytest.
# This script is a thin Mac-side wrapper around that
#
# One-time setup this depends on (not automated here -- see
# tests/ui-harness/README.md for why each matters):
#   1. C:\OBS-Test\arm64 and C:\OBS-Test\x64 each holding a plain
#      OBS-Studio-<version>-Windows-<arch>.zip extraction.
#   2. Any system-wide obs-ptz install (e.g.
#      C:\ProgramData\obs-studio\plugins\obs-ptz) renamed aside, so it can't
#      race the dev build for obs-websocket's vendor registration.
# `setup` below checks both and tells you what's missing; it won't fix
# either for you, since #1 means downloading and extracting a real OBS
# release zip.
#
# Usage (run from anywhere; it locates the repo root from its own path):
#   scripts/vm-windows-dev.sh setup            # check the VM is reachable and one-time setup is done
#   scripts/vm-windows-dev.sh test <arm64|x64> # build, overlay into C:\OBS-Test\<arch>, run tests\obs-integration (more args go to pytest)
#   scripts/vm-windows-dev.sh run <arm64|x64>  # launch that arch's C:\OBS-Test obs64.exe for interactive poking
#   scripts/vm-windows-dev.sh clean            # kill a stray obs64.exe left over from a previous run
#
# Every subcommand reflects this checkout's *current on-disk* files --
# including uncommitted changes -- via the VM's existing "hacking" shared
# folder (mapped from this Mac's ~/hacking), not a git push. That folder
# has to already exist on the VM (Parallels > this VM > Configure >
# Options > Sharing) and this checkout has to live under ~/hacking for it
# to be reachable; `setup` checks both.
#
# Every prlctl exec below uses --current-user: the default SYSTEM context
# can't see custom shared folders (they're only mapped into the
# interactive user's own logon session) and can't put a window on the
# interactive desktop either (session 0 isolation) -- see this repo's
# CLAUDE.md and tests/ui-harness/README.md for both gotchas. Each Windows
# step is also a single, standalone .bat file invocation rather than an
# inline `cmd /c "a && b"` string: chaining inside one prlctl exec argument
# is documented (CLAUDE.md) to silently drop steps partway through.
#
# Env overrides: VM (Parallels VM name, default below).
#
# Requires bash 3.2-compatible syntax throughout (this is macOS's default
# /bin/bash) -- no arrays, no ${var,,}, etc.

set -eu

VM="${VM:-Windows 11}"
SRC_MAC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HACKING_MAC="$(cd "$HOME/hacking" 2>/dev/null && pwd || true)"

require_arch() {
	case "${1:-}" in
	arm64 | x64) ;;
	*)
		echo "usage: $0 $2 <arm64|x64>" >&2
		exit 2
		;;
	esac
}

# This checkout's path as the VM sees it, via the existing "hacking"
# shared folder -- e.g. ~/hacking/obs-ptz/.claude/worktrees/foo becomes
# \\psf\hacking\obs-ptz\.claude\worktrees\foo. Errors out (with the reason)
# rather than guessing if that share, or this checkout's place under it,
# isn't there.
win_src() {
	if [ -z "$HACKING_MAC" ]; then
		echo "error: ~/hacking doesn't exist on this Mac -- the VM's \"hacking\" shared folder maps from there, so this checkout needs to live under it" >&2
		exit 1
	fi
	case "$SRC_MAC" in
	"$HACKING_MAC"/*) ;;
	*)
		echo "error: $SRC_MAC is not under $HACKING_MAC, so it isn't reachable via the VM's existing \"hacking\" shared folder" >&2
		exit 1
		;;
	esac
	rel="${SRC_MAC#"$HACKING_MAC"/}"
	echo "\\\\psf\\hacking\\$(echo "$rel" | sed 's#/#\\#g')"
}

# Runs a command on the VM as the interactive user (see this script's own
# header for why --current-user, not the prlctl exec default).
vm_exec() { prlctl exec "$VM" --current-user "$@"; }

# `prlctl exec` has been observed to fail transiently and spuriously (e.g.
# "PrlJob_GetResult: Invalid argument" on a call that succeeds moments
# later, no VM-side change involved) -- a plain existence check is cheap
# enough to retry a couple of times before believing a "missing" result.
vm_path_exists() {
	i=0
	while [ "$i" -lt 3 ]; do
		vm_exec cmd /c "dir \"$1\"" >/dev/null 2>&1 && return 0
		i=$((i + 1))
		sleep 1
	done
	return 1
}

# Writes a script (its stdin) into the shared checkout (visible to both
# sides) and runs it on the VM -- see this script's header on why every
# Windows step is a real standalone file, never an inline `cmd /c "..."`.
run_vm_bat() {
	name="$1"; shift
	cat > "$SRC_MAC/.vm-windows-dev-$name.bat"
	vm_exec "$(win_src)\\.vm-windows-dev-$name.bat" "$@"
}

cmd_setup() {
	src_win="$(win_src)"
	echo "vm-windows-dev: checkout reachable at $src_win"
	if ! vm_path_exists "$src_win\\scripts\\windows-build-and-test.bat"; then
		echo "error: $src_win doesn't look like this repo (no scripts\\windows-build-and-test.bat there)" >&2
		exit 1
	fi
	missing=0
	for arch in arm64 x64; do
		if ! vm_path_exists "C:\\OBS-Test\\$arch\\bin\\64bit\\obs64.exe"; then
			echo "missing: C:\\OBS-Test\\$arch\\bin\\64bit\\obs64.exe -- extract an OBS-Studio-<version>-Windows-$arch.zip release (not the installer) there"
			missing=1
		fi
	done
	if vm_path_exists "C:\\ProgramData\\obs-studio\\plugins\\obs-ptz"; then
		echo "warning: C:\\ProgramData\\obs-studio\\plugins\\obs-ptz exists -- a system-wide obs-ptz install will race the dev build for obs-websocket's vendor registration. Rename it aside (e.g. append .disabled)."
		missing=1
	fi
	[ "$missing" = 0 ] && echo "vm-windows-dev: one-time setup looks complete"
	return 0
}

cmd_test() {
	require_arch "${1:-}" test
	# What follows the architecture goes to pytest
	src_win="$(win_src)"
	run_vm_bat test <<EOF
@echo off
rem cmd's cd /d can't target a UNC path (\\psf\hacking\...); pushd maps
rem one to a temporary drive letter and cds into that instead.
pushd "$src_win"
call scripts\windows-build-and-test.bat $*
exit /b %ERRORLEVEL%
EOF
}

cmd_run() {
	require_arch "${1:-}" run
	# OBS resolves its data path against the *launching process's* cwd on
	# Windows, not its own exe's folder (GetDataFilePath(), see
	# tests/ui-harness/README.md's "cwd-relative" gotcha) -- launching the
	# exe directly by path fails InitLocale() and exits immediately with
	# "Failed to find locale"/"Failed to load locale". `start /D <dir> ""`
	# sets that cwd; the empty "" is cmd's required title placeholder
	# before a quoted command.
	vm_exec cmd /c start /D "C:\\OBS-Test\\$1\\bin\\64bit" "" obs64.exe
}

cmd_clean() {
	vm_exec taskkill /F /IM obs64.exe /T || true
}

main() {
	cmd="${1:-}"
	[ -n "$cmd" ] && shift || true
	case "$cmd" in
	setup) cmd_setup ;;
	test) cmd_test "$@" ;;
	run) cmd_run "$@" ;;
	clean) cmd_clean ;;
	*)
		echo "usage: $0 {setup|test <arm64|x64>|run <arm64|x64>|clean}" >&2
		exit 2
		;;
	esac
}

main "$@"
