# Keep device state in PTZListModel

## Goal

`PTZListModel` holds each device's whole state, and every widget reads it from
there. `PTZStateView` follows the model, as `PTZMovementControls` does, instead
of `PTZSettings` pushing state into it and reading state back out of it.

Success looks like this:

- No production code calls `PTZStateView::shownValues()`.
- `PTZSettings` no longer passes state to the view.
- `PTZListModel::saveState()` is gone.
- A widget that wants to know a device's state has one place to ask.

## Why

- `refreshDeviceState()` already fetches the whole state on every
  `ptz_state_changed`, keeps a few fields, and drops the rest. Keeping it adds
  no proc calls.
- `updateHeader()` is the one production caller of `shownValues()`, which reads
  values back out of widgets. The header can therefore show what the user just
  picked in a checkbox or list as though the camera had reported it.
- `saveState()` makes a proc call for each reader. `PTZMovementControls::
  updateControls()` runs on every `dataChanged`, so it makes one for each state
  change, to read one key.
- `axisText()` turns a position into degrees, a multiplier or a distance. It
  needs nothing from a widget, but is on one, so nothing else can use it.

## Design

### PTZListModel

`PTZDeviceEntry` gains the state, kept by `refreshDeviceState()`:

```cpp
/* The device's whole state, as ptz_get_state last gave it. Replaced, never
 * changed here. */
OBSData state;
```

```cpp
/* The device's state as last reported, or an empty object if there is no such
 * device. For reading: the model and the device's own state are behind it. */
OBSData deviceState(const QModelIndex &index) const;
```

- `refreshDeviceState()` already makes a new object for each fetch; it stores
  that object and goes on deriving `connected`, `poweredOff` and `features`
  from it. The roles stay, because delegates paint from them.
- It has to be stored on every path. `deviceCreated()` seeds it and
  `deviceSettingsChanged()` refreshes it, so `state` is never null for a device
  that has a row.
- Not a role: `OBSData` is not a registered metatype, so it can't go in a
  `QVariant` without registering one for a single reader.
- `deviceStateUpdated(uuid, changed)` stays. `camera_report` is in the whole
  state, where its last value stays, so `PTZCameraReportDialog` needs the diff
  to know that progress just happened. `PTZSettings` needs it to know that
  `features` changed.
- `saveState()` is removed. Its `live`, `preview` and `locked` keys have no
  reader; those stay roles.

Nested objects are the exception to "replaced". The device runs `ptz_get_state`
on the main thread here, and that path uses `obs_data_apply()`, which shares
nested objects with the device's own state. `axes` and `camera_report` in the
cache can therefore change in place, between refreshes, as the device changes
them. That is harmless, since everything concerned is on the main thread and
the value is only ever newer, but it is why readers must not write to the state
and must not keep a nested object to compare against later.

### Axis text

`axisText()` and `formatAxisValue()` leave the view for a new
`src/controller/ptz-state-format.{hpp,cpp}`:

```cpp
/* An axis's position as text, in its unit where the state says what it is, and
 * as the position, to 3 places, where not. Empty if the state has no such axis. */
QString ptzAxisText(obs_data_t *state, const char *axis);
```

### PTZStateView

```cpp
/* Follow this device, as the list model has it */
void setDevice(const QModelIndex &index);
```

- The view keeps a `QPersistentModelIndex m_device`, and connects to
  `dataChanged` the way `PTZMovementControls` does, showing
  `ptzDeviceList->deviceState(m_device)` for a range that contains it.
  `dataChanged` is used rather than `deviceStateUpdated` because it also covers
  a settings change, which refreshes the state too, and it fires after the
  cache is current.
- `setState()`, `applyChanges()` and the `all` argument go. What is left of
  `applyData()` is a private `showState()` that takes a whole state, with the
  `all` branches as its only behaviour.
- `m_axis`, `m_axes` and `m_axisKeys` go. The view never displayed them.
- `field->reported`, `m_reportedWhiteBalance` and the settle timers stay. They
  are not a cache of the device: they say what to go back to if the camera
  doesn't do what the user asked. Comparing against `reported` is also what
  leaves a pending edit alone when an unrelated key changes.
- `shownValues` and `updateCount` stay as Qt properties for tests, and
  `shownChanged` stays as their notify signal. `shownValues` loses the four
  axes and their `_text` keys.
- `setStatistics()` stays, pushed by `PTZSettings`' timer. Statistics are polled
  rather than state.
- `stateRequested`, `actionRequested` and `cameraReportRequested` stay as they
  are. Writes still go through `PTZSettings` to `ptzDeviceList->setState()`.

The model resets when a device is added or removed, and a reset throws away the
list's selection without `currentChanged()`. `PTZSettings` therefore remembers
the device it shows, and after a reset goes back to it, or on to the first device
if it has gone.

### PTZSettings

- `currentChanged()` calls `ui->stateView->setDevice(current)` in place of
  `saveState()` and `setState()`.
- `updateHeader()` reads `IsConnectedRole` and `deviceState(current)`, and
  `ptzAxisText()` for the position. The `shownValues()` call goes.
- The `shownChanged` to `updateHeader` connection goes: the `dataChanged`
  handler already updates the header, and it no longer has to wait for the view.
- `deviceStateUpdated()` keeps only its `features` handling.

### Other readers of saveState()

| Caller | Reads | Becomes |
| --- | --- | --- |
| `PTZMovementControls::updateControls()` | `focus_af_enabled` | `deviceState()` |
| `PTZControls` context menu | `power_on`, `wb_mode` | `deviceState()` |
| `PTZCameraReportDialog` constructor | `connected` | `IsConnectedRole` |

## Behaviour changes

- The header's connected, power, autofocus and position values are what the
  camera reported, not what a widget holds. A checkbox clicked in the state view
  no longer shows in the header until the camera confirms it.
- The view takes the whole state each time, so a new value for a key that never
  reached it as a diff now shows. It loops over all its fields per update
  instead of those that changed, but touches only widgets whose value differs.
- A reader can be one event-loop pass behind a direct `ptz_get_state`, as the
  cache is updated by a queued call. No current reader needs better than that.
- `updateCount` no longer counts position changes, as the view doesn't show
  position.

## Risks

- **State that changes without a notification.** The cache is only as fresh as
  `ptz_state_changed` and the `update` signal make it, where a proc call sees
  the state as it is now. `saveState()` also computes `connected`, `source` and
  `features` when called. From reading the code, `connected`, `source`, the axes,
  the VISCA paths and `camera_report` all reach `stateChanged`, and a device's
  `features()` is already cached this way by the model. Not audited: that every
  `setPosition()` caller in the drivers then calls `notifyStateChanged()`. That
  check is the first step of the plan.
- **Device switch.** `showState()` must still hide keys the new device doesn't
  report, which is the old `all` behaviour.

## Testing

Integration tests are the coverage here: `tests/unit` links neither OBS nor Qt,
so `ptzAxisText()` can't be unit tested there.

Existing tests that change:

- `conftest.py` builds `dialog[axis]` from `shownValues`, and
  `test_settings_dialog.py:48,54,61` waits on those keys. That test becomes one
  of the header: it waits for the header's position text to change.
- `test_status_view_updates_in_place` waits for `state_updates >= 3`, part of
  which came from a pan. It changes `wb_mode` enough times instead, and keeps
  the check that no widget was replaced. The pan stays, to show the view isn't
  rebuilt by position reports.

`test_settings_changes_redraw_only_the_settings_view` asserts
`state_updates == 0`, which holds only if an unchanged state shows nothing new.
Keep it: the view now sees that state on every `dataChanged`.

New tests:

- The header doesn't show a state view edit until the camera confirms it.
- Switching between devices with different keys hides the first's rows.
- A pending edit survives a report of a different key.

Run the `tests/obs-integration` suite with `scripts/macos-dev.sh test` for each
step, and do a manual pass on a real camera before any PR.

## Order of work

Each step leaves a working plugin and can land alone:

1. Audit that each driver state change notifies, and fix any that doesn't.
2. Cache the state in the model and add `deviceState()`.
3. Move axis text out, and switch `updateHeader()` to the model, with its tests.
4. Convert the `saveState()` callers and remove it.
5. Give the view `setDevice()`, and drop `applyChanges()`, the axes and the
   `shownChanged` connection, with their tests.

## Out of scope

- Moving `refreshDeviceState()` to apply diffs rather than refetch.
- Narrowing `dataChanged` by role, or coalescing it, at a camera's report rate.
- Moving statistics into the model.
- Changing the PTZ API, so no API version bump.
