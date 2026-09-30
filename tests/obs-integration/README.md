# OBS + ptzsim integration tests

Boots a real, plugin-loaded OBS Studio against a real `ptzsim` instance
and drives PTZ commands through the actual plugin code, not just the
emulator. Where `scripts/ptzsim`'s own manual testing confirms the wire
protocols are decoded correctly, this suite confirms obs-ptz's real
`PTZDevice` implementations produce those wire commands in the first
place, and that OBS's device-config loading and `ptz_action_source`
actually work end to end.

## How it works

1. **`ptzsim`** is started as a subprocess on fixed local ports, one
   camera exposing every backend under test (VISCA TCP/UDP/serial,
   Pelco), plus `--debug-http-port` so the tests can read back its state
   as JSON without decoding a camera protocol themselves.
2. **obs-ptz's own device config** -- the JSON file
   `PTZControls::LoadConfig()` reads from `obs_module_config_path
   ("config.json")` (see `src/ptz-controls.cpp`) -- is pre-written with
   one device per backend, each given an explicit `"id"` so the test
   knows each device's `device_id` up front (`PTZDevice::PTZDevice()`
   reads `id` straight from config; there's no auto-assignment to rely
   on). This is the *only* obs-ptz/OBS file this suite hand-writes --
   everything else about OBS's state (scenes, sources) is driven live
   over obs-websocket once OBS is running, specifically to avoid
   depending on OBS's own scene-collection JSON schema, which is
   internal and changes between versions.
3. **OBS Studio** is launched with an isolated `$HOME` (so it doesn't
   touch your real profile) and obs-websocket enabled via its documented
   environment variables (`OBS_WEBSOCKET_SERVER_ENABLE`,
   `_PORT`, `_PASSWORD`) -- no manual "enable websocket server" click
   needed.
4. Each test uses `obsws.py` (a from-scratch, dependency-light
   obs-websocket v5 client -- just the Hello/Identify handshake and
   Request/RequestResponse, see its docstring) to create a scene, add a
   `ptz_action_source` (`src/ptz-action-source.c`) configured for one
   device + one action, and switch to that scene. A `ptz_action_source`
   fires its configured action the moment it's added to what's already,
   or becomes, the current program scene (`ptz_action_source_activate()`
   with the default `PTZ_ACTION_TRIGGER_PROGRAM_ACTIVE` trigger) --
   which calls straight into `proc_handler_call(..., "ptz_move_continuous"
   / "ptz_preset_recall" / "ptz_preset_save", ...)`, the same entry
   point a joystick move or a hotkey uses.
5. The test polls `ptzsim`'s `/state` endpoint until it sees the
   expected pan/tilt speed or position, or times out.
6. `test_preset_import_export.py` covers the preset export/import feature.
   It triggers the import/export context menu action. However, it passes
   a filename at call time as the tests cannot drive the file dialog.
   Needs the plugin built with `-DENABLE_UI_TESTS=ON` (`tests/ui-harness/`
   is entirely inert otherwise) and OBS launched with
   `PTZ_UI_TEST_HARNESS=1`, which `conftest.py`'s `obs_world` fixture
   always sets -- a no-op if the binary wasn't built with that option, so
   it doesn't affect the rest of the suite.
7. `test_device_status.py` covers camera connect/disconnect detection via
   `tests/ui-harness/device-status-test.cpp`'s `get_device_status` test
   (same `-DENABLE_UI_TESTS=ON` requirement as above), using its own
   disposable `flaky_ptzsim` fixture -- a second, VISCA-TCP-only `ptzsim`
   instance a test can kill and restart -- rather than the session-scoped
   `ptzsim` every other test shares, since stopping *that* one would break
   the rest of the suite.
8. `test_absolute_relative_moves.py` covers absolute/relative pan-tilt
   moves and absolute zoom (`PTZDevice::move_abs()`/`move_rel()`), via
   `tests/ui-harness/move-device-test.cpp`'s `move_device` test (same
   `-DENABLE_UI_TESTS=ON` requirement as above) -- there's no
   `ptz_action_source` action type for either, so obs-websocket alone
   can't reach them.
9. `test_autofocus_white_balance.py` covers autofocus on/off
   (`PTZDevice::set_autofocus()`) and VISCA white balance mode
   (`PTZVisca::requestState()`'s `"wb_mode"` handling), via
   `tests/ui-harness/device-state-test.cpp`'s `set_device_state` test (same
   `-DENABLE_UI_TESTS=ON` requirement as above), read back through
   `get_device_status`.
10. `test_power.py` covers camera power on/off
    (`PTZVisca::requestState()`'s `"power_on"` handling), the same way as
    autofocus/white balance -- `set_device_state` to request it, `get_device_status` to read it back.
    `PTZ_ACTION_POWER_OFF`/`PTZ_ACTION_POWER_ON` exist in
    `ptz_action_source`'s own action enum but are never actually wired
    up or exposed in its properties, so obs-websocket alone can't reach
    this either.
11. `test_change_interface.py` covers switching an existing VISCA
    device's transport live -- the "Protocol" list in the properties
    dialog (`PTZVisca::get_obs_properties()`/`PTZVisca::update()`'s
    `"type"` -> `ViscaTransport` switch, see `src/ptz-visca.cpp`) -- and
    that it persists through the original `"type"`/`"host"`/`"port"`
    field names rather than a separate field per transport. Walks one
    device through all three transports (TCP -> UDP -> serial -> back to
    TCP), confirming each one both connects and actually delivers a move
    the simulated camera decodes; the serial leg uses its own dedicated
    ptzsim instance rather than the shared "visca-serial" device's, since
    ptzsim's simulated VISCA-serial camera only answers at bus address 1
    and sharing it would mean two devices filtering the same address off
    the same wire. Uses `tests/ui-harness/update-device-test.cpp`'s
    `update_device` test (same `-DENABLE_UI_TESTS=ON` requirement as
    above), which applies a
    settings change to a device the same way the properties dialog's
    Apply/OK does.
12. `test_preset_view.py` covers what the PTZ Controls dock's preset list
    shows when there is no camera selected to show the presets of: nothing,
    not the list of cameras -- including after the model resets, as it does
    when a device is added or removed. Uses
    `tests/ui-harness/preset-view-test.cpp`'s `get_preset_view` test (same
    `-DENABLE_UI_TESTS=ON` requirement as above), which can select a camera
    in the dock, or add or remove a device, and reports the rows the preset
    list is showing.
13. `test_device_source_binding.py` covers how a `PTZDevice` binds to the
    OBS source it controls (`PTZDevice::source()`, see
    `src/ptz-device.cpp`): that it finds a source created after the
    device config was loaded (the config is loaded before OBS loads its
    scene collection, so a device can never bind at load time), follows
    the source being renamed, and copes with the source being removed and
    a new one of the same name created. Uses
    `tests/ui-harness/device-source-test.cpp`'s `get_device_source` test
    (same `-DENABLE_UI_TESTS=ON` requirement as above) to read back which
    source a device holds and what it is called, and creates, renames and
    removes the sources themselves over obs-websocket. Each test has its
    own device (see `DEVICE_IDS` in `conftest.py`), configured with the
    name of a source that doesn't exist yet and with no camera behind it.
    They remove a source's scene item before the source, since otherwise
    OBS keeps the source, and its name, until its scene is next drawn
    (see `test_obs_removed_source.py`, below), which would leave the tests
    that recreate a source under a removed one's name waiting, or, on
    macOS, unable to run at all.
14. `test_filter_devices.py` covers PTZ devices that belong to an OBS
    filter (`ptz_visca_filter_info`, see `src/ptz-device.cpp`): adding a
    "VISCA PTZ Control" filter to a source creates a device for it,
    named after and bound to the source, that connects to the camera and
    can be driven like any other; it follows the source's rename and
    shows as live with it; removing the filter removes the device; and it
    is saved with the filter, not in the plugin's own device list. Each
    test points its filters at a dedicated `ptzsim` instance, and finds
    the device by name through the `get_device_source` test (a filter's
    device is given its id by the plugin, not by the config file).
15. `test_device_settings.py` covers the split between a device's
    settings (persisted, edited through `PTZDevice::get_obs_properties()`,
    which the PTZ settings dialog and OBS's Filters dialog both show) and
    its transient state: every value the settings properties edit is one
    `save()` also writes, a PTZ filter persists no runtime identity (device
    id, name), settings sent through the plugin's own dialog path
    reach the filter's settings, and a filter updated with just one setting
    keeps every other at its default. Uses `tests/ui-harness/device-settings-test.cpp`'s
    `get_device_settings` test.
16. `test_device_state.py` covers the transient-state half of that split:
    the `ptz_get_state` and `ptz_request_state` proc handlers
    (`PTZDevice::saveState()`, `requestState()`). Uses
    `tests/ui-harness/device-state-test.cpp`'s `get_device_state` and
    `set_device_state` tests, checking that a request asks the camera for
    just the values it holds, even one it already reports. Also that nothing
    the camera reports is among a filter's settings properties, and that
    VISCA advertises its diagnostics, and that the `ptz_trigger` actions
    send their commands (through the harness's `trigger_device`) and an
    unknown one sends nothing.
17. `test_device_signals.py` covers what `PTZListModel` tells a listener
    (the settings dialog) about a device changing: the values a state change
    reported, and settings changes, including ones that don't go through
    the model, such as OBS's Filters dialog and obs-websocket editing a
    filter. Uses `tests/ui-harness/device-signals-test.cpp`.
18. `test_onvif_position.py` covers ONVIF reporting where the camera is
    (`PTZOnvif::handleGetStatusResponse()` in `src/ptz-onvif.cpp`) as the
    generic position state, against the ONVIF backend of the shared
    `ptzsim`: which axes it reports, a position in range, and that it
    follows a move as it happens instead of at the slow poll's pace.
19. The USB (UVC) driver reporting where the camera is
    (`PTZUSBCam::report_state()`, `PTZUsbWorker::captureState()` in
    `src/ptz-usb-cam.cpp`/`src/ptz-usb-worker.cpp`) has no test here: there is
    no camera to drive through a live OBS, so it is covered at the worker
    level instead, in `tests/usb-backend/test_worker.cpp` ("the worker reports
    its state, for the axes the camera has", and a camera with no focus
    range), the same way the rest of the USB driver is tested.
20. `test_settings_dialog.py` covers the PTZ settings dialog showing a
    device's settings and its state apart, as a properties view and a
    `PTZStateView` stacked on one scrolling page rather than as tabs: what
    each holds, that a state change changes only the status view and a
    settings change only the settings view, that the status view is updated
    in place (the same widgets before and after, none replaced) rather than
    redrawn, that picking a white balance in the status view changes the
    camera, that a camera with diagnostics, and only one, has buttons for
    them that work, and that the dialog copes with its device being removed
    or changing interface while it is open. Opens the real dialog through
    `tests/ui-harness/settings-dialog-test.cpp`.
21. `test_api_version.py` covers the PTZ API's version: that
    `ptz_get_api_version`, on OBS's proc_handler and on each device's,
    reports the version `src/ptz.h` declares, called the way another plugin
    would through
    `tests/ui-harness/api-version-test.cpp`, and that
    `docs/ptz-device-api.md` states it.
22. `test_visca_udp_sony.py` covers how VISCA-over-IP copes with a camera
    that behaves like a real Sony one, against a session-scoped `sony_ptzsim`
    fixture: a `ptzsim` started with `--visca-udp-sony-quirks`, which drops
    requests that come too soon after a reply, answers slowly, has only two
    command sockets and says "command buffer full" when they are busy. It
    counts what it saw, and the tests read those counters from its
    `--debug-http-port` `/state` output.
23. `test_visca_power_at_obs_events.py` covers VISCA's "power on at startup"
    and "power off at shutdown" settings, with OBS's own start and exit: a
    camera that starts off (a `ptzsim` started with `--start-in-standby`) is on
    once OBS has loaded, so is one that only starts once OBS has, and one
    with neither setting is left alone. Closing OBS ends the session, so that
    it turns the cameras off when it does, those of a standalone device and of
    a filter, is asserted when the `obs_world` fixture is torn down.
24. `test_visca_camera_state.py` covers the rest of what a VISCA camera
    reports of itself (focus, exposure, white balance gains, picture,
    system and pan/tilt status, everything the Sony manual has an inquiry
    for): that it is all read, with the block inquiries or, for a camera
    without them (`--visca-no-block-inquiries`), the single-value ones; that
    each setting with a command can be asked for with `set_device_state`;
    and that the settings dialog's state view shows every one and changes
    them through `edit_dialog_state`. Each test has its own source with a
    VISCA filter, pointed at a `ptzsim` that reports the camera's settings
    under `"visca"` in its `--debug-http-port` `/state` output.

## Running locally

Needs a real OBS Studio with this plugin built and installed into it --
see the top-level `CMakeLists.txt` options; you'll want
`-DENABLE_SERIALPORT=ON` so the VISCA-serial and Pelco device types
exist at all (off by default), and `-DENABLE_UI_TESTS=ON` if you want
`test_preset_import_export.py` to actually find anything to drive
(`test_ptz_backends.py` works fine without it).

**macOS**: real OBS on macOS ignores every isolation mechanism this
suite relies on and always loads your live profile, so don't run pytest
against it directly -- use `scripts/run-macos-integration-tests.sh`
instead, which builds the plugin, backs up and restores your whole OBS
profile around the run, and redirects obs-ptz's/obs-websocket's config to
where real OBS actually reads it (see that script's and
`scripts/macos-integration-test-obs-wrapper.py`'s docstrings for why).
Last verified end to end this way (20/20 passing, against OBS 32.2.1)
before the preset export/import suite dropped its vendor-request tests
in favor of testing only through the real UI action, and
on_actionPresetImport_triggered() was fixed to restore device selection
after its model reset (see `src/ptz-controls.cpp`'s own comment there) --
not yet re-run against that change; expect 14/14 (the vendor-request
tests are gone, see `test_preset_import_export.py`'s own docstring) if
it holds.

**Linux**: `$HOME` isolation actually works, so a plain

```
apt install obs-studio libobs-dev  # or your distro's equivalents
cmake --preset ubuntu-<arch> -DENABLE_SERIALPORT=ON -DENABLE_ONVIF=ON -DENABLE_UI_TESTS=ON
cmake --build --preset ubuntu-<arch>
cp build_<arch>/obs-ptz.so /usr/lib/<arch>-linux-gnu/obs-plugins/  # real file, not a symlink -- OBS's plugin scan skips symlinks
pip install -r tests/obs-integration/requirements.txt
PTZSIM_OBS_BINARY=obs pytest tests/obs-integration -v
```

is enough -- no wrapper script needed. Verified end to end this way
(14/14 passing) against Ubuntu 24.04's packaged OBS 30.0.2, run under a
throwaway `Xvfb` (confirmed a viable fallback for this suite
specifically, since every test here drives OBS purely over
obs-websocket and never needs synthetic X11 input) in 22s; `Xvfb`'s
software (`llvmpipe`) rendering is a known way to starve OBS's main
thread on a low-core-count VM (see the top-level `CLAUDE.md`), so prefer
a real accelerated GNOME/Xwayland session where one's available.

With no `DISPLAY` set, the suite starts its own `Xvfb` (the `xvfb`
package) on a free display and launches OBS on it directly. At the end
it quits OBS with SIGINT, which OBS handles as a normal quit (SIGTERM only
is since OBS 32.1; before that it kills OBS outright, so nothing gets
turned off), waits for it to exit, then stops the `Xvfb`.

`PTZSIM_OBS_BINARY` defaults to `obs` on Linux and OBS.app's real binary
path on macOS; override it if yours lives elsewhere.

What OBS and each `ptzsim` print goes to its own file in the temp directory
(`obs-*.log`, `ptzsim-*.log`), not to a pipe: nothing reads the pipe, and once
it filled up OBS's main thread or a whole `ptzsim` would block on its next
write, and every later test would time out.

## Known limitations

- **Timing constants** (`wait_for_state` timeouts, the `time.sleep(0.5)`
  move durations in `test_preset_save_and_recall`) are reasonable
  guesses tuned against `ptzsim`, not real camera hardware timing.
