# Keep Device State in PTZListModel Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `PTZListModel` holds each device's whole state, and every widget reads it from there, with `PTZStateView` following the model through `setDevice()`.

**Architecture:** The model keeps the object `refreshDeviceState()` already fetches and offers it through `deviceState()`. The header and the other `saveState()` callers read that. The view connects to `dataChanged` and shows the whole state each time, keeping only what it needs to roll back a user's edit.

**Tech Stack:** C++17, Qt 6, libobs, pytest integration tests in `tests/obs-integration` driven through the UI driver.

**Spec:** `docs/superpowers/specs/2026-10-31-list-model-device-state-design.md`

## Global Constraints

- No PTZ API change, so no API version bump.
- `deviceStateUpdated(uuid, changed)` stays: `PTZCameraReportDialog` and `PTZSettings` depend on the diff.
- Readers must not write to the state, or keep a nested object from it: `axes` and `camera_report` are shared with the device's own state.
- `shownValues` and `updateCount` stay as Qt properties for tests, and no production code may read them.
- Build and test on macOS only with `scripts/macos-dev.sh`, never bare `cmake`.
- Run each commit through clang-format and gersemi before pushing.
- Commit trailer: `Assisted-by: Claude:claude-opus-5-5`. Never `Signed-off-by:`, never `Co-Authored-By:`.
- New user-visible strings go in `data/locale/en-GB.ini`. This plan adds none.

## Review Focus

1. Switching from a device that reports a key to one that doesn't: its rows must hide, not freeze at the last device's values. Test in Task 4.
2. A report of an unrelated key while the user's edit is pending: the edit must stay until the camera does it or the field settles back. Test in Task 4.
3. The device shown is removed while the dialog is open: the dialog keeps working and shows the next device. Test in Task 4.
4. A camera that is switched off: the header says so and hides its position, whatever it last reported. Test in Task 2.
5. A device that doesn't report `power_on`: the header must not claim either power state, since a missing key now reads as `false` from `obs_data_get_bool()`. Test in Task 2.

---

### Task 1: Audit that state changes notify

The cache is only as fresh as `ptz_state_changed` and the `update` signal. This was checked while planning, and these are known good: `ptz-usb-cam.cpp:153`, `ptz-soft-cam.cpp:216`, `ptz-onvif.cpp:419`, the VISCA reply path (`ptz-visca.cpp:948-961`, including its `update_position()`), `apply_assumed()`, `setConnected()`, `featuresChanged()`, and the parent source name (`ptz-device.cpp:439`).

**Files:**
- Modify, only if the audit finds a gap: `src/drivers/visca/ptz-visca.cpp`, `src/drivers/pelco/*`

**Interfaces:**
- Consumes: nothing
- Produces: a guarantee later tasks rely on, that every change to what `saveState()` returns is followed by `notifyStateChanged()`, or by an `update` signal

- [ ] **Step 1: Find the writers still to check**

Run: `grep -rnE "update_position\(|choose_profile\(|set_profile\(|obs_data_(set_[a-z]+|apply)\(state\b" src/drivers src/core`

Expected: a list to go through. Check `update_position()` and `set_profile()` when they run outside the VISCA reply path, since `setAxisStep()` and `setAxisScale()` (`ptz-visca.cpp:476,485`) write `axes` and don't notify by themselves. Also check that `features()` can't change, for any driver, without `featuresChanged()`.

- [ ] **Step 2: Add `notifyStateChanged()` where a path has none**

For each gap, call it after the last write of that path. Nothing goes out if nothing changed, so an extra call is safe. If there are no gaps, skip this and the next two steps, and say so in the task's report.

- [ ] **Step 3: Run the device state tests**

Run: `scripts/macos-dev.sh test -k "device_state or visca_profiles"`
Expected: PASS

- [ ] **Step 4: Commit any fixes**

```bash
git add src/drivers
git commit -m "visca: Say what changed when <the path found>"
```

### Task 2: Cache the state, and read the header from it

Covers spec order steps 2 and 3 together: the cache has no reader to test it through until the header uses it.

**Files:**
- Create: `src/controller/ptz-state-format.hpp`, `src/controller/ptz-state-format.cpp`
- Modify: `CMakeLists.txt` (next to `ptz-state-view` at lines 318 and 334), `src/controller/ptz-list-model.hpp`, `src/controller/ptz-list-model.cpp:263-297`, `src/controller/ptz-state-view.hpp`, `src/controller/ptz-state-view.cpp:133-142,818-872`, `src/controller/settings.cpp:237,904-960`
- Test: `tests/obs-integration/test_settings_dialog.py`, `tests/obs-integration/test_visca_camera_state.py`

**Interfaces:**
- Consumes: nothing
- Produces:
  - `OBSData PTZListModel::deviceState(const QModelIndex &index) const`: never null, an empty object if there is no such device
  - `QString ptzAxisText(obs_data_t *state, const char *axis)`: empty if the state has no such axis
  - `m_power` has the object name `"power_on"`
  - `CameraSim.freeze()` and `CameraSim.thaw()` in `test_visca_camera_state.py`

- [ ] **Step 1: Write the failing test**

In `test_visca_camera_state.py`, import `signal`, and `SETTINGS_DIALOG` and `header_pan` from `conftest`. Give `CameraSim` `freeze()` and `thaw()`, sending `signal.SIGSTOP` and `signal.SIGCONT` to `self.proc`, with `stop()` thawing first. In `ptz-state-view.cpp`, `m_power->setObjectName("power_on")`, so a test can click it.

```python
@pytest.mark.skipif(sys.platform == "win32", reason="needs SIGSTOP")
def test_header_waits_for_the_camera_to_say_it_changed(request, obs_world, tmp_path):
    camera = make_camera(request.addfinalizer, obs_world, tmp_path)
    out, _ = open_dialog(obs_world, camera, tmp_path)
    obs_world.wait_for_settings_dialog(out, lambda r: "Powered on" in r["header"])

    camera.sim.freeze()
    request.addfinalizer(camera.sim.thaw)
    obs_world.edit_dialog_state(power_on=False)
    # Anything that redraws the header will do, and this leaves it as it was
    locked = obs_world.ui.get1(SETTINGS_DIALOG, "livemoveCheckBox", name="checked")
    obs_world.set_live_move_lock(not locked)
    obs_world.set_live_move_lock(locked)

    dialog = obs_world.wait_for_settings_dialog(out, lambda r: r["shown"]["power_on"] is False)
    assert "Powered on" in dialog["header"]
```

- [ ] **Step 2: Run it to see it fail**

Run: `scripts/macos-dev.sh test -k test_header_waits_for_the_camera`
Expected: FAIL, with `Powered off` in the header.

- [ ] **Step 3: Write the header tests for Review Focus 4 and 5**

```python
def test_header_hides_where_a_camera_that_is_off_is(request, obs_world, tmp_path):
    camera = make_camera(request.addfinalizer, obs_world, tmp_path)
    out, _ = open_dialog(obs_world, camera, tmp_path)
    obs_world.wait_for_settings_dialog(out, header_pan)

    obs_world.set_device_state(camera.device_name, power_on=False)
    dialog = obs_world.wait_for_settings_dialog(out, lambda r: "Powered off" in r["header"])
    assert header_pan(dialog) is None


def test_header_says_nothing_of_power_a_device_doesnt_report(obs_world, tmp_path):
    device_name = obs_world.device_names["pelco-d"]
    state = obs_world.wait_for_device_state(device_name, tmp_path / "state.json",
                                            lambda r: r["state"].get("connected"))["state"]
    obs_world.open_settings_dialog(device_name)
    dialog = obs_world.wait_for_settings_dialog(tmp_path / "dialog.json", lambda r: r["header"])
    assert ("power_on" in state) == ("Powered" in dialog["header"])
```

`header_pan(dialog)` is a module-level function in `conftest.py`, imported by both test files: the text after `P&nbsp;` in `dialog["header"]`, by `re.search(r"\bP&nbsp;([^&]+)", ...)`, or `None`.

Also rewrite `test_settings_dialog.py:48-61` to follow the header. It waits for `header_pan(r)` where it waited for the four axes in `state_keys`, waits with `wait_for_device_state` for `abs(state["pan"]) < 0.01` after the absolute move, takes `before = header_pan(...)`, and ends by waiting for `header_pan(r) != before`.

- [ ] **Step 4: Add `state` to the model**

Add `OBSData state;` to `PTZDeviceEntry`, with the comment from the spec. `refreshDeviceState()` assigns the object it fetched to `entry->state`, whether or not the proc call succeeded, so it is never null. `deviceState()` returns that, or a new empty object for an invalid index.

- [ ] **Step 5: Move the axis text**

Move `formatAxisValue()` and the body of `axisText()` to `ptz-state-format.cpp`, reading the position from `state` and the unit and points from its `axes`. Remove `PTZStateView::axisText()`, and the `_text` keys from `shownValues()`, which no test reads. `m_axis` and `m_axes` stay until Task 4.

- [ ] **Step 6: Switch `updateHeader()` to the model**

Replace `shown` with `OBSData state = ptzDeviceList->deviceState(current)`, taken after the invalid-index return. `connected` comes from `IsConnectedRole`. `powerKnown` and the autofocus check use `obs_data_has_user_value()`. Positions use `ptzAxisText()`, where the axis key has a user value. Remove the `shownChanged` connection at `settings.cpp:237`, since the `dataChanged` handler already redraws the header. Add the new file to `CMakeLists.txt`.

- [ ] **Step 7: Run the tests**

Run: `scripts/macos-dev.sh test -k "header or test_settings_dialog or test_visca_camera_state"`
Expected: PASS

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt src/controller tests/obs-integration
git commit -m "controller: Have the settings header say what the device reported"
```

### Task 3: Remove saveState()

**Files:**
- Modify: `src/controller/ptz-list-model.hpp:177`, `src/controller/ptz-list-model.cpp:838-856`, `src/controller/ptz-movement-controls.cpp:238-240`, `src/controller/ptz-controls.cpp:1211-1221`, `src/controller/ptz-camera-report.cpp:49-58`, `src/controller/settings.cpp:1007-1009`

**Interfaces:**
- Consumes: `PTZListModel::deviceState()` from Task 2
- Produces: `saveState()` no longer exists, for Task 4 to rely on

No new test: this is covered by the suites run below, as behaviour doesn't change.

- [ ] **Step 1: Convert the callers**

Movement controls and the context menu read `deviceState()`, by the table in the spec. The report dialog reads `index.data(PTZListModel::IsConnectedRole).toBool()`, which is false for an invalid index, as the empty state was. `PTZSettings::currentChanged()` calls `ui->stateView->setState(ptzDeviceList->deviceState(current))` until Task 4 replaces it.

- [ ] **Step 2: Remove `saveState()`**

Delete it from the header and the source. `setState()` stays, as the write half. The block comment above them moves to `setState()`, reworded for a request alone.

- [ ] **Step 3: Run the tests**

Run: `scripts/macos-dev.sh test -k "camera_report or autofocus or test_power or test_settings_dialog"`
Expected: PASS, including `test_the_dialog_says_a_camera_that_isnt_connected_cant_be_asked`.

- [ ] **Step 4: Commit**

```bash
git add src/controller
git commit -m "controller: Read a device's state from the list model"
```

### Task 4: Have PTZStateView follow the model

**Files:**
- Modify: `src/controller/ptz-state-view.hpp`, `src/controller/ptz-state-view.cpp:619-792,874-919`, `src/controller/settings.cpp:1009,1042-1054`, `src/controller/settings.hpp:50`
- Test: `tests/obs-integration/conftest.py:1142-1143`, `tests/obs-integration/test_settings_dialog.py:138-157`, `tests/obs-integration/test_visca_camera_state.py`

**Interfaces:**
- Consumes: `deviceState()` from Task 2, and `CameraSim.freeze()` and `thaw()`
- Produces: `void PTZStateView::setDevice(const QModelIndex &index)`, and no public `setState()` or `applyChanges()`

- [ ] **Step 1: Write the tests for Review Focus 1 to 3**

```python
def test_a_device_without_a_key_hides_its_row(obs_world, tmp_path):
    out = tmp_path / "dialog.json"
    obs_world.open_settings_dialog(obs_world.device_names["visca-tcp"])
    obs_world.wait_for_settings_dialog(out, lambda r: "wb_mode" in r["state_keys"])

    obs_world.open_settings_dialog(obs_world.device_names["pelco-d"])
    obs_world.wait_for_settings_dialog(out, lambda r: "wb_mode" not in r["state_keys"])


@pytest.mark.skipif(sys.platform == "win32", reason="needs SIGSTOP")
def test_a_pending_edit_outlasts_a_report_of_something_else(request, obs_world, tmp_path):
    camera = make_camera(request.addfinalizer, obs_world, tmp_path)
    out, opened = open_dialog(obs_world, camera, tmp_path)
    was = opened["shown"]["ae_mode"]

    camera.sim.freeze()
    request.addfinalizer(camera.sim.thaw)
    obs_world.edit_dialog_state(ae_mode=0xb if was != 0xb else 0x3)
    # A new "source" is a report that needs nothing of the camera
    camera.source += "-renamed"
    obs_world.ws.call("SetInputName", {"inputName": camera.source[:-len("-renamed")],
                                       "newInputName": camera.source})

    assert obs_world.wait_for_settings_dialog(out, lambda r: True)["shown"]["ae_mode"] != was
    # ...and it settles back to what the camera last said
    obs_world.wait_for_settings_dialog(out, lambda r: r["shown"]["ae_mode"] == was, timeout=5)


def test_removing_the_device_shown_leaves_the_dialog_working(request, obs_world, tmp_path):
    camera = make_camera(request.addfinalizer, obs_world, tmp_path)
    out, _ = open_dialog(obs_world, camera, tmp_path)

    camera.cleanup()

    obs_world.open_settings_dialog(obs_world.device_names["visca-tcp"])
    obs_world.wait_for_settings_dialog(out, lambda r: r["connected"] and "wb_mode" in r["state_keys"])
```

All three go in `test_visca_camera_state.py`. They pass today: they guard behaviour that this task could break.

- [ ] **Step 2: Run them to see them pass**

Run: `scripts/macos-dev.sh test -k "without_a_key or pending_edit or removing_the_device"`
Expected: PASS

- [ ] **Step 3: Update the tests that lose position reports**

Delete the axis loop at `conftest.py:1142-1143`. In `test_status_view_updates_in_place`, change `for mode in (1, 2, 1)` to `(1, 2, 1, 2)`: at least three of those change `wb_mode` whatever it started at. Keep the pan, and update the comment to say that it changes nothing the view shows.

- [ ] **Step 4: Add `setDevice()`**

Declare it with `QPersistentModelIndex m_device`. In the constructor, connect `ptzDeviceList`'s `dataChanged`, with the `QItemSelectionRange(topLeft, bottomRight).contains(m_device)` check `PTZMovementControls` uses, to show `deviceState(m_device)`. `setDevice()` stores the index and shows `deviceState(index)` at once.

- [ ] **Step 5: Reduce `applyData()` to `showState(obs_data_t *data)`**

Rename it, make it private, and remove `setState()`, `applyChanges()` and the `all` argument: every `all` condition is now true, and `if (!all && !has(key))` goes. Remove the axis loop, the `axes` block, `m_axis`, `m_axes`, `m_axisKeys` and `AxisCount`, and the axes in `shownValues()`. Update the class comment, which still says it shows live, preview and locked, and the comment at `settings.hpp:50` if it describes pushing.

Don't touch the check `value != field->reported`: it is what leaves a pending edit alone.

- [ ] **Step 6: Reduce `PTZSettings`**

`currentChanged()` calls `ui->stateView->setDevice(current)`. `deviceStateUpdated()` keeps its `features` check, its comment reworded, and loses `applyChanges()`.

- [ ] **Step 7: Run the whole suite**

Run: `scripts/macos-dev.sh test`
Expected: PASS, including `test_settings_changes_redraw_only_the_settings_view`, which asserts `state_updates == 0`.

- [ ] **Step 8: Check nothing reads the view from production code**

Run: `grep -rnE "shownValues|updateCount|applyChanges|saveState" src/controller | grep -v ptz-state-view`
Expected: no output.

- [ ] **Step 9: Commit**

```bash
git add src/controller tests/obs-integration
git commit -m "controller: Have the state view follow the list model"
```

### Task 5: Try it on real hardware

- [ ] **Step 1: Install, and ask Grant to test**

Run: `scripts/macos-dev.sh install`, then `scripts/macos-dev.sh run`.

Native UI can't be checked from this environment, and CLAUDE.md forbids opening a PR for untested code. Ask Grant to try the settings dialog with a real camera: the header follows a pan, a state view edit, switching between cameras, and the camera report. No PR until they have.
