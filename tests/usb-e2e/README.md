# USB camera end-to-end test (macOS)

Drives a **real USB UVC PTZ camera** through real OBS and the obs-ptz plugin built
from this checkout, and checks the position the camera reports afterwards.

**What "reports" means.** UVC lets a camera answer "where are you?" however it
likes. Some cameras answer with where the motor really is; others, such as the
BirdDog P1xx this was written against, simply return the last position they were
*told* to go to (the answer arrives within milliseconds of a command that takes
seconds to carry out). On such a camera the test shows the command reached the
camera by way of OBS, the plugin and USB, and was stored, but **not** that the
camera physically moved. Watch the camera the first time you run it.

It needs hardware, so nothing runs it automatically. The unit tests for the USB
code, which need no camera, are in [`tests/usb-backend/`](../usb-backend).

## Running it

Configure once with the test targets on, then run the script:

```sh
cmake --preset macos -DENABLE_USB_TESTS=ON -DENABLE_UI_TESTS=ON
tests/usb-e2e/run-macos.sh                  # the only USB camera, if there is one
tests/usb-e2e/run-macos.sh --camera 0x210000036050064   # or say which
```

The camera is named by its AVFoundation unique ID: `0x` and hex digits, from
`system_profiler SPCameraDataType` (the built-in and virtual cameras have UUIDs
instead). `--skip-build` skips the build, and `USB_E2E_CAMERA` can stand in for
`--camera`.

What you need:

- a UVC camera with **pan, tilt and zoom**. The camera **moves** (a few tens of
  degrees) and is sent back to its home position, pan/tilt/zoom 0, at the end,
  or if the test fails part way through;
- **OBS quit**. The script refuses to run otherwise;
- the Python environment `scripts/run-macos-integration-tests.sh` uses. The
  script creates it, which downloads the packages in
  `tests/obs-integration/requirements.txt`, if it isn't there.

## What it does to your OBS

On macOS OBS ignores every documented way of giving it a config of its own (see
`scripts/macos-integration-test-obs-wrapper.py`), so the test has to use the real
one, for the length of the run. The script:

1. **backs up** `~/Library/Application Support/obs-studio` and checks the backup
   against the original. Two things are left out because a run can't change them:
   OBS's browser cache (`plugin_config/obs-browser`, which is most of the size)
   and the other plugins' binaries. Only `plugins/obs-ptz.plugin`, which it swaps,
   is kept from `plugins/`. The backup is a few MB;
2. moves **every** profile and scene collection aside, whatever they are called,
   so OBS starts with a new empty one and can't touch your real cameras. (Don't
   rely on OBS's own profile switching: it ignores that too);
3. puts the plugin from `build_macos` in place of the installed one;
4. runs the test, which launches OBS a few times;
5. **restores** everything from the backup, whatever happened, and checks that
   the config matches it again. If it doesn't, the backup is kept, and the
   script says where.

The wrapper overwrites the plugin's config on every launch, so presets saved in
one launch aren't there in the next.

## What it checks

Each scenario launches OBS, drives the plugin over obs-websocket, quits OBS, and
then asks the camera where it is with `usb-uvc-position`, which uses the plugin's
own backend. It can't ask while OBS runs: the plugin keeps the camera's USB device
open, and only one process can.

1. A `usb-cam` device bound to an OBS source on the camera reports **connected**,
   and an absolute move (`ptz_move_abs`) puts the camera where the backend would
   send it: the position is worked out from the camera's own ranges.
2. After a **relaunch** it reconnects. A preset saved at once, before any move,
   holds the position the plugin read from the camera when it opened it; after
   going home, recalling it takes the camera back there.
3. A **continuous move**, the way a hotkey or joystick drives it (a
   `ptz_action_source`), keeps panning from where the camera was, without
   tilting, and stops; then home.

Unplugging isn't tested: that needs a person to pull the cable.

## Notes

- OBS started from a shell has no Camera permission, so it gets no video from the
  camera. Controlling the camera doesn't need it, so this doesn't matter here.
- The logs from the run are left in the temporary directory the script prints.
