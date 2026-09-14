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
