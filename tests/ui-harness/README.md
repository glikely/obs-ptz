# PTZ UI test harness

Drives UI-level integration tests against a live, running OBS process
from inside the plugin itself - things like opening a real dialog,
clicking through it, and checking what a widget actually rendered.
Not part of the normal plugin build, not CI-integrated (needs a real
windowed `OBS.app`, and a test that drives a dialog pops it open and
closed for real - don't drive the mouse/keyboard while one runs) - link
it in explicitly when you want to run it.

Ships with one test, `appearance_row_sizing` (see
`appearance-row-sizing-test.cpp` and its own driver,
`scripts/test_preset_row_sizing.py`) - see "Adding a new test" below
for how to add another.

## Why in-process, not a standalone test binary

`tests/uart-hil` tests `PTZUARTWrapper` in isolation, without OBS
running at all. UI tests need the opposite: real widgets, real dialogs,
real OBS-internal state that only exists inside a running OBS process -
much of it not reachable through `obs-frontend-api` from an external
tool at all (there's no call to trigger a theme reload, for instance,
which `appearance-row-sizing-test.cpp` needed - see its own comment).
So the harness has to be linked into the plugin binary itself and
driven from inside.

## Why obs-websocket, not a polled file

Commands reach the harness through obs-websocket's vendor request
mechanism (`obs-websocket-api.h`, vendored here - header-only, talks to
obs-websocket purely via libobs's own `proc_handler`/`calldata`, no
link dependency, degrades to a harmless no-op if obs-websocket isn't
loaded). `PTZUITestHarness` registers as vendor `"obs-ptz"` with one
request type, `"ui_test_run"`, from `ptz_load_ui_tests()` (called from
`obs_module_post_load()` in `ptz.c` - vendor registration requires that
specific hook, not `obs_module_load()`, since it needs obs-websocket's
own module to have already finished loading first).

An earlier version polled a temp file every 100ms instead. obs-websocket
is strictly better here: it already ships with OBS on every platform,
already has a mature client ecosystem in virtually every language (this
repo's own client, `scripts/obs_ws_client.py`, is a small
dependency-free Python one, precisely so nothing needs `pip install`
first), and gives real request/response instead of a fixed poll
interval plus log-grepping to notice a request landed.

**Threading**: obs-websocket dispatches every request, including vendor
ones, on its own `QThreadPool`
(`websocketserver/WebSocketServer.cpp`) - never the Qt GUI thread. A
test that drives real widgets is only safe doing so on the GUI thread,
so `PTZUITestHarness::vendorRequestCallback()` never touches UI itself -
it only parses the request and marshals the actual work back over via
`QMetaObject::invokeMethod(qApp, ..., Qt::QueuedConnection)`, then
acknowledges receipt immediately; `PTZUITestHarness::dispatch()` (which
that queued call lands on) is what actually runs on the GUI thread, and
is what calls into a registered test function. A *test's own result*
therefore still only reaches a caller via whatever it itself logs (e.g.
`blog(LOG_INFO, "[ptz-ui-test] ...")`, the convention the harness's own
code uses) - the obs-websocket response only confirms the request was
accepted and dispatched, not that the test has finished or what it
found. Keep that in mind when writing a test that needs its result
observed synchronously; that needs its own further plumbing, not
something already provided here.

## Building

```
cmake --preset macos -DENABLE_UI_TESTS=ON   # or your platform's preset
cmake --build build_macos --target obs-ptz
```

`ENABLE_UI_TESTS` is never on by default - always pass it explicitly.
This only adds the harness to the binary; it stays completely inert
(no vendor registration, nothing) unless `PTZ_UI_TEST_HARNESS=1` is
also set in OBS's environment at runtime.

## Running

With OBS running under `PTZ_UI_TEST_HARNESS=1` and obs-websocket
enabled:

```
python3 scripts/obs_ws_client.py --vendor obs-ptz --request-type ui_test_run \
    --password <server_password from obs-websocket's config.json> \
    --data '{"cmd": "appearance_row_sizing", "density": "-4", "fontscale": "10"}'
```

`scripts/obs_ws_client.py` prints obs-websocket's own response (whether
the request was *accepted*), not a test's actual result - watch OBS's
own log (`~/Library/Application Support/obs-studio/logs/` on macOS) for
whatever a given test logs.

`scripts/test_preset_row_sizing.py` is a full driver built on top of
this: it launches OBS, sweeps `appearance_row_sizing` across every
Density/FontScale combination, and compares the result against the
real Sources dock. Use it as the template for a new test's own driver.

By default the driver launches whatever OBS install is normal for the
platform (`/Applications/OBS.app` on macOS, `%ProgramFiles%\obs-studio`
on Windows, `obs` on `$PATH` elsewhere). Set `PTZ_TEST_OBS_BIN` to an
absolute path to point it at a different install instead - e.g. to
choose between multiple architectures' worth of OBS on the same
machine (see "Testing on Windows" below).

## Testing on Windows

Windows has no single "the installed OBS" the way macOS/Linux do here,
and this repo builds for both `windows-arm64` and `windows-x64`
(CMakePresets.json) - Recommended practice is to keep two
separate OBS installs side by side instead of overwriting
one with the other for each test run:

- `C:\OBS-Test\arm64\` - official OBS Studio Windows-arm64 build
- `C:\OBS-Test\x64\` - official OBS Studio Windows-x64 build

Both are plain extractions of the `OBS-Studio-<version>-Windows-<arch>.zip`
asset from an [obs-studio release](https://github.com/obsproject/obs-studio/releases)
(obs-websocket is bundled, no separate install needed) - **not** the
`-Installer.exe`, which installs to `Program Files` and fights with
whatever's already there. Both share the same `%APPDATA%\obs-studio`
profile (scene collection, obs-websocket password, etc.) - only the
binary differs, and nothing about OBS's config is architecture-specific.

`scripts/windows-build-and-test.bat <arm64|x64>` does the full cycle
for one architecture: finds Visual Studio via `vswhere.exe`, configures
and builds `obs-ptz` with `-DENABLE_UI_TESTS=ON` for that arch's preset,
overlays the fresh `obs-ptz.dll`/data into that arch's `C:\OBS-Test\`
tree, and runs `test_preset_row_sizing.py` against it (via
`PTZ_TEST_OBS_BIN`). Run it from the repo root:

```
scripts\windows-build-and-test.bat arm64
scripts\windows-build-and-test.bat x64
```

One-time setup this depends on:

1. Create `C:\OBS-Test\arm64` and `C:\OBS-Test\x64` as described above.
2. **Disable any system-wide obs-ptz install.** An installer-based
   obs-ptz (e.g. under `C:\ProgramData\obs-studio\plugins\obs-ptz`) is
   scanned by *every* OBS install on the machine regardless of where it
   lives, architecture-permitting. If its architecture happens to match
   the dev build under test, both get loaded, they race for
   obs-websocket's `obs-ptz` vendor registration
   (`WebSocketApi::vendor_register_cb`), and if the installed one wins,
   the harness silently ends up running that *older* copy's test code
   instead of the dev build's - confusing, since the symptom is
   individual test commands the current code definitely has (e.g.
   `measure_now`) logging `unknown cmd '...', ignoring`. Rename that
   directory (e.g. append `.disabled`) rather than uninstalling, so
   it's easy to tell it was intentional and to put back.
3. A GUI process launched via `prlctl exec`'s default `nt authority\system`
   context can't put a window on the interactive desktop (session 0
   isolation) - OBS starts, sits there consuming no CPU, and never logs
   past whatever needs a real window station. Use
   `prlctl exec "<VM>" --current-user <cmd>` instead, which runs as the
   already-logged-in interactive user with a real session - confirmed
   with `prlctl capture "<VM>" --file out.png` (a real VM screenshot,
   independent of any of this) while chasing this down.

Gotchas specific to this setup:

- **OBS's data-path resolution is cwd-relative on Windows**
  (`GetDataFilePath()` in `frontend/utility/platform-windows.cpp` checks
  `"data/obs-studio/..."` against the *launching process's* current
  directory, not the exe's own folder. A one-off manual launch needs
  `start /D "<install>\bin\64bit" "" obs64.exe` or it'll fail
  `InitLocale()` and exit immediately with "Failed to load locale"
- **Unclean-shutdown sentinel files can silently block a launch.**
  Same mechanism as the macOS one in this repo's `CLAUDE.md` -
  `%APPDATA%\obs-studio\.sentinel\run_<uuid>` marker files, one per
  launch, deleted on clean shutdown. A force-killed OBS (`taskkill /F`,
  or a crash) leaves one behind, and the *next* launch blocks forever
  on the "Crash or unclean shutdown detected" modal dialog.  If a run
  times out waiting for a fresh log or "harness active", check for stale
  files there first and move (don't delete) any `run_*` aside before
  retrying.
- **A vendored obs-studio sub-build can come up missing a project.**
  Every configure re-runs `cmake/common/buildspec_common.cmake`'s
  `_setup_obs_studio()`, which builds `obs-frontend-api` out of a
  vendored obs-studio checkout under `.deps/obs-studio-<version>/build_<arch>`
  regardless of whether it's already built. This was observed to fail
  reproducibly for `x64` specifically (arm64 unaffected) with
  `MSBUILD : error MSB1009: Project file does not exist. Switch:
  obs-frontend-api.vcxproj` - the generated `.sln` was simply missing
  that project's entry, confirmed by grepping it directly. Root cause
  not fully nailed down (didn't reproduce with the generator, VS
  toolset, or CMake version, and a genuinely fresh regenerate always
  came up correct - looked like corruption accumulating in that one
  build tree over repeated incremental reconfigures rather than
  anything x64-specific per se). If it recurs: delete
  `.deps/obs-studio-<version>/build_<arch>` entirely (not just the
  top-level `build_<arch>`) and reconfigure - confirmed to clear it and
  stay clear across repeated runs afterward.

## Adding a new test

1. Add `tests/ui-harness/<name>-test.hpp`/`.cpp`: a
   `register<Name>Test(PTZUITestHarness *harness)` function that calls
   `harness->registerTest("<cmd>", &yourFunction)`, where `yourFunction`
   takes a `const QMap<QString, QString> &params` (the request's
   key=value pairs, `cmd` included) and does whatever driving/checking
   the test needs - log its result (`blog(LOG_INFO, "[ptz-ui-test] ...")`
   is the convention so far) so an external driver script has something
   to grep for. This function always runs already on the Qt GUI thread
   (`PTZUITestHarness::dispatch()` guarantees that), so it's safe to
   touch widgets directly.
2. `#include` the new header and call your `register<Name>Test(this)`
   from `PTZUITestHarness`'s constructor (`ui-test-harness.cpp`).
3. Add both new files to `CMakeLists.txt`'s `ENABLE_UI_TESTS` block.
4. If a driver script needs to exist for it: launch OBS with
   `PTZ_UI_TEST_HARNESS=1`, then shell out to
   `python3 scripts/obs_ws_client.py --vendor obs-ptz --request-type
   ui_test_run --data '{"cmd": "...", ...}'` per invocation (reading
   obs-websocket's port/password from its own
   `plugin_config/obs-websocket/config.json`, since that's simpler than
   prompting for them) and grep OBS's log for the test's own result
   lines.
