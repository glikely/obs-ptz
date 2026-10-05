# PTZ device API

This is the API used to interact with PTZ Devices.
It covers how to find and manage PTZ devices, to move them, to recall
their presets, and to be notified of state changes.
It is the interface this plugin uses to communicate between the front
end UI and the back end driver.
It can also be called by other plugins and scripts.
It is built on the OBS `proc_handler` abi for making calls to the
camera, and `signal_handler` for receiving notifications.
See [AGENTS.md](../AGENTS.md) for the design it is part of.

Other plugins can implement this API, either to discover and control PTZ
devices, or to implement PTZ device instances and have them exposed in
the PTZ User Interface.

This file is the specification.
`tests/obs-integration/test_api_doc.py` reads it and holds the plugin to it.
The signatures in the headings and the names in the tables below are checked
against the source, and the keys against a running plugin.
A change to the API that isn't made here fails that test.

## API version

This is version **0.1** of the PTZ API.
The API is in a pre-release state, published as an RFC.
It is subject to change at any time.
Please provide feedback on the [API discussion page](https://github.com/glikely/obs-ptz/discussions/API)

A caller asks the plugin which version it implements with `ptz_get_api_version`
on OBS's own proc_handler, before relying on anything else here:

```c
calldata_t cd = {0};
if (proc_handler_call(obs_get_proc_handler(), "ptz_get_api_version", &cd)) {
	long long major = calldata_int(&cd, "major");
	long long minor = calldata_int(&cd, "minor");
}
calldata_free(&cd);
```

The version covers the procs and signals below, and the calldata fields, state
keys and trigger names they take.
The minor version goes up when something is added, which an existing caller
can't notice.
The major version goes up, and the minor goes back to 0, when something
is removed, renamed or changes meaning.
So a caller written for version M.m works with any version M.n where n >= m,
and isn't promised to with any other.
For example, a caller written for 1.0 works with any 1.x.

Each device also has `ptz_get_api_version` on its own proc_handler.
Other plugins that implement this API may be implementing a different
version than this plugin.
A caller must check the API level of the device before making any other
calls to make sure the API is implemented as expected.

## Quick start

PTZ devices are exposed as filters to the rest of OBS.
The filter is attached to the camera video source and this API is
attached to the filter's proc_handler and signal_handler.
A caller finds them by walking the filters of the sources it cares about:

```c
#define PTZ_FILTER_PREFIX "ca.secretlab.obs-ptz."

static void each_filter(obs_source_t *parent, obs_source_t *filter, void *data)
{
	if (strncmp(obs_source_get_id(filter), PTZ_FILTER_PREFIX,
		    strlen(PTZ_FILTER_PREFIX)))
		return;
	proc_handler_t *ph = obs_source_get_proc_handler(filter);
	signal_handler_t *sh = obs_source_get_signal_handler(filter);

	/* check the version of this device before relying on it */
	calldata_t v = {0};
	bool ok = proc_handler_call(ph, "ptz_get_api_version", &v) &&
		  calldata_int(&v, "major") == 1;
	calldata_free(&v);
	if (!ok)
		return;

	signal_handler_connect(sh, "state_changed", device_state_changed, data);
	/* keep ph and sh, for as long as the filter exists */
}

/* for each source: obs_source_enum_filters(source, each_filter, data); */
```

Then call procs on a device's handler by name, with the fields each one reads
(they are listed under each proc below):

```c
calldata_t cd = {0};
calldata_set_float(&cd, "pan", 0.5);
calldata_set_float(&cd, "tilt", 0.0);
proc_handler_call(ph, "ptz_move", &cd);   /* pan right at half speed */
calldata_free(&cd);
```

And read what changed in a `state_changed` handler:

```c
static void device_state_changed(void *data, calldata_t *cd)
{
	/* don't modify or release it */
	obs_data_t *changed = calldata_ptr(cd, "changed");
	if (obs_data_has_user_value(changed, "features")) {
		/* what the device can do changed: ask again with ptz_get_state */
	}
	if (obs_data_has_user_value(changed, "pan")) {
		double pan = obs_data_get_double(changed, "pan");
		(void)pan;
	}
}
```

Any device, with or without a filter, can also be reached by its `device_id`
through the [global proc_handler](#global-ptz-proc_handler), for the few
things it offers.

### Rules for calling

- **Threads.** A proc that moves the camera or recalls a preset can be called
  from any thread: it queues the work for the device's own thread, which is the
  one OBS made the device on (the Qt main thread).
  A proc that reads or changes the device's data, or returns something, must be
  called from that thread.
  From another it logs an error and does nothing.
  Each proc below says which it is.
- **Ownership.** A `ptr` the caller passes in (the `state`, `config` and
  `statistics` objects) is the caller's: the device fills it in.
  A `ptr` a proc returns as `return` is a new reference or object that the
  caller releases, and each proc below says with what.
  A `ptr` in a signal belongs to the plugin.
- **Fields.** A proc reads the calldata fields named under it.
  The declared signature doesn't always list them all, so the fields here are
  the contract.
  A field a caller leaves out is taken as not given, not as zero, unless a proc
  says otherwise.
- **Units.** A speed is -1.0 to 1.0 and a position is -1.0 to 1.0 for pan and
  tilt and 0.0 to 1.0 for zoom and focus, whatever the camera's own units.

## Global PTZ proc_handler

Obtained with `ptz_get_proc_handler()` on OBS's proc_handler.
It reaches any device by `device_id`, for a caller that does not or
cannot lookup the filter source.

### `void ptz_preset_save(int device_id, int preset_id)`

Saves the device's current position as preset `preset_id`.
Any thread.

### `void ptz_preset_recall(int device_id, int preset_id)`

Moves the device to preset `preset_id`.
Any thread.

### `void ptz_move_continuous(int device_id, float pan, float tilt, float zoom, float focus)`

Moves the device at the given speeds.
`pan`, `tilt`, `zoom` and `focus` are all optional.
The device will only move the axis provided.
Any thread.

## OBS main proc_handler

The plugin's entry points on `obs_get_proc_handler()`.

### `ptr ptz_get_proc_handler()`

Returns, as `return`, the global PTZ proc_handler above.
It isn't a new reference: the plugin owns it.

### `void ptz_get_api_version(out int major, out int minor)`

The version of the PTZ API this plugin implements, for a caller to check before
it relies on anything else here.
Any thread.

### `void ptz_pantilt(int device_id, float pan, float tilt, float zoom, float focus)`

Deprecated: the same as `ptz_move_continuous`, kept for plugins that call it.

## Global PTZ signal_handler

Announces devices coming and going inside the plugin.
It isn't reachable from another plugin: `ptz_get_signal_handler()` isn't
exported from the module, and no proc on OBS's proc_handler returns it.
A plugin finds devices through their filters instead, as in the
[quick start](#quick-start).

### `void ptz_device_create(int device_id, ptr proc_handler, ptr signal_handler)`

A device has been created.
`proc_handler` and `signal_handler` are the device's own, which are its
filter's, and good until `ptz_device_destroy`.
The signal also carries `filter`, the device's `obs_weak_source_t *` for its
PTZ Control filter.

### `void ptz_device_destroy(int device_id)`

The device is gone: stop using its handlers.

## Per-device proc_handler

The device's own proc_handler: its filter's, from
`obs_source_get_proc_handler()`.
All its procs start with `ptz_`, so they can be added to an existing
proc_handler with low risk of conflicts.

### `void ptz_get_api_version(out int major, out int minor)`

The version of the PTZ API this device implements.
A device may come from another plugin, at another version than the
`ptz_get_api_version` on OBS's proc_handler reports.
Callers must checks each device's own before relying on anything else it has.
Any thread.

### `void ptz_stop()`

Stops all movement: pan, tilt, zoom and focus.
Any thread.

### `void ptz_home_recall()`

Moves to the home position.
Needs the `home` feature.
Any thread.

### `void ptz_home_save()`

Saves the current position as the home position.
Needs the `home_set` feature.
Any thread.

### `void ptz_move()`

Moves at a speed, until told otherwise or stopped.
Needs the `pantilt`, `zoom` and `focus` features, for each axis it moves.
Any thread.
Reads:

- `pan`, `tilt` (float, -1.0 to 1.0): the speed.
  If either is given the other is taken as 0, so a caller moving only one axis
  gives both.
  The sign is the direction, after the `pan_invert` and `tilt_invert` settings,
  and the speed is capped by `pantilt_speed_max`.
- `zoom` (float, -1.0 to 1.0): the zoom speed, capped by `zoom_speed_max`.
- `focus` (float, -1.0 to 1.0): the focus speed, capped by `focus_speed_max`.

A zero stops that axis; an axis left out is left as it is.

### `void ptz_move_abs()`

Moves to a position and stops there.
Needs the `pantilt_abs`, `zoom_abs` and `focus_abs` features, for each axis it
moves.
Any thread.
Reads:

- `pan`, `tilt` (float, -1.0 to 1.0): the position.
  As for `ptz_move`, if either is given the other is taken as 0.
- `zoom`, `focus` (float, 0.0 to 1.0): the position.

### `void ptz_move_rel()`

Moves by a distance.
Needs the `pantilt_rel` feature.
Any thread.
Reads:

- `pan`, `tilt` (float, -1.0 to 1.0): how far, as a fraction of the axis's
  whole travel.
  If either is given the other is taken as 0.

### `void ptz_preset_save()`

Saves the current position as a preset, and a thumbnail of the device's source.
Needs the `presets` feature.
Any thread.
Reads:

- `preset_id` (int): the preset to save.
  The calldata has no declared fields, so a caller that doesn't give it saves
  nothing.

### `void ptz_preset_recall()`

Moves to a preset.
Needs the `presets` feature.
Any thread.
Reads `preset_id` (int), as `ptz_preset_save` does.

### `void ptz_preset_clear()`

Clears a preset's saved position on the camera, and its thumbnail.
Needs the `presets` feature.
Any thread.
Reads `preset_id` (int), as `ptz_preset_save` does.

### `ptr ptz_get_state(ptr state)`

Fills in the `state` object the caller owns with the device's whole transient
state: see [State keys](#state-keys).
Device thread.

### `ptr ptz_get_statistics(ptr statistics)`

Fills in the `statistics` object the caller owns with what the device has
counted of its own working, such as how much it has sent to the camera and how
fast it answers, as numbers under names that the driver gives them.
It isn't state: it is read when wanted and never announced as changing, and a
rate in it is over the time since it was last read.
Device thread.

### `ptr ptz_get_parent_source()`

Returns, as `return`, the OBS source whose video the camera shows.
It is a new reference, which the caller releases with `obs_source_release()`,
and it is null while the device has no source, or the source has been removed.
Any thread.

### `void ptz_set_locked(bool locked)`

Locks or unlocks the device, which refuses to be moved while it is locked, and
reports it as the `locked` state key.
Reads `locked` (bool).
A device is also locked by the plugin whenever it is live.
Device thread.

### `void ptz_get_config(ptr config)`

Fills in the `config` object the caller owns with the device's settings: see
[Config keys](#config-keys).
Device thread.

### `void ptz_set_config(ptr config)`

Applies the settings in the `config` object, and announces them with
`settings_changed`.
For a device that is a filter it goes through `obs_source_update()`, which
merges them into the filter's settings: keys left out keep their value, and the
Filters dialog shows the same values.
A device with no filter has nothing to merge into, so give it the whole config,
as `ptz_get_config` returns it.
Device thread.

### `ptr ptz_get_properties()`

Returns, as `return`, the `obs_properties_t *` that edit the settings.
The caller destroys it with `obs_properties_destroy()`.
Device thread.

### `void ptz_request_state(ptr state)`

Asks the device to change some of its state, which is never saved: the keys in
the `state` object the caller owns that the device acts on.
Only `focus_af_enabled` is one so far, and the rest are ignored.
The state then changes when the camera reports it has, and not before.
Device thread.

### `void ptz_trigger(string name)`

Runs a one-shot action on the camera, which isn't state: see
[Triggers](#triggers).
Reads `name` (string).
An unknown name is logged and otherwise ignored.
Any thread.

### `void ptz_get_camera_report(out string report)`

Returns, as `report`, the last report of what the camera has, which its
`camera_report` trigger makes, as JSON, or "" if there isn't one.
It is for the user to look at and send in themselves; nothing in it identifies
the camera, the user, or where either is.
Device thread.

### `ptr ptz_preset_get_list()`

Returns, as `return`, an `obs_data_array_t *` of the presets in display order,
which the caller releases with `obs_data_array_release()`.
Each is an object with `id` (int), `name` (string), `token` (string, the
camera's own name for it, "" if it hasn't one) and `thumbnail` (string, the
path of its thumbnail image, or "").
The calldata also has `max_presets` (int), the most the device keeps: the
`preset_max` setting.
Device thread.

### `int ptz_preset_new(int row)`

Adds a preset, at display row `row` or at the end if that isn't a row in the
list, and returns its `id` as `return`, or -1 if the device already has
`max_presets`.
Announces `preset_inserted`.
Device thread.

### `void ptz_preset_remove(int row)`

Removes the preset at display row `row`, which has to be a row in the list.
Announces `preset_removed`.
Device thread.

### `void ptz_preset_move(int src_row, int dest_row)`

Moves the preset at display row `src_row` to before the one at `dest_row`,
where `dest_row` is its index before `src_row` is taken out, as
`QAbstractItemModel::moveRows()` has it.
Announces `preset_moved`.
Device thread.

### `void ptz_preset_set_name(int id, string name)`

Names the preset `id`, which is its `id` and not its row.
Announces `preset_renamed`.
Device thread.

### `void ptz_scene_changed()`

Tells the device that the program or preview scene changed, so it checks again
whether it is live or in the preview, and so locked.
A caller that changes scenes without the OBS frontend knowing calls it.
Device thread.

## Per-device signal_handler

The device's own signal_handler: its filter's, from
`obs_source_get_signal_handler()`.
A signal fires on the device's own thread, and the `device_id` it carries is
the device's.

### `void state_changed(int device_id, ptr changed)`

The device's state changed.
`changed` is an `obs_data_t *` holding only the values that changed, with the
keys of [State keys](#state-keys).
A listener may keep a reference to it, but must not change it: every listener
gets the same one, and the device never touches it again.

### `void settings_changed(int device_id)`

The device's settings were applied, from anywhere: `ptz_set_config`, the
Filters dialog, or obs-websocket.
A listener reads the settings again with `ptz_get_config`.

### `void preset_inserted(int device_id, int row)`

A preset was added at display row `row`.

### `void preset_removed(int device_id, int row)`

The preset that was at display row `row` was removed.

### `void preset_moved(int device_id, int src_row, int dest_row)`

A preset moved from display row `src_row` to before `dest_row`, which is its
index before `src_row` was taken out.

### `void preset_renamed(int device_id, int id)`

The name of preset `id` changed.

### `void preset_thumbnail_changed(int device_id, int id)`

The thumbnail of preset `id` changed.

## State keys

Both `ptz_get_state` and `ptz_get_config` fill in an `obs_data_t` that the
caller owns.
The keys here are the ones every device has, whatever its protocol.
A driver adds its own (a VISCA device has `pan_pos` and `vendor_id`, say), so a
caller ignores keys it doesn't know and checks that one is there, with
`obs_data_has_user_value()`, before it reads it.

State is transient: it describes the device right now, and is never saved.
Changes to it are announced by `state_changed`, which carries only the keys
that changed.

| Key | Type | Present | Meaning |
| --- | --- | --- | --- |
| `connected` | bool | always | The device has a working link to its camera |
| `live` | bool | always | The device's source is in the program scene |
| `preview` | bool | always | The device's source is in the preview scene (studio mode only) |
| `locked` | bool | always | Movement is refused: set with `ptz_set_locked`, and also set whenever the device is `live` |
| `features` | object | always | What the device can do, with each [feature](#features) it has `true` and none for one it hasn't. It can change while the device runs, as it finds out what its camera has |
| `pan`, `tilt` | number | when known | Position, -1.0 to 1.0, as the camera last reported it. Absent until it has |
| `zoom`, `focus` | number | when known | Position, 0.0 to 1.0, as above |
| `focus_af_enabled` | bool | when known | Autofocus is on. Can be changed with `ptz_request_state` |
| `camera_report` | object | when known | Progress of a `camera_report` trigger while one is running |

`live`, `preview` and `locked` follow the program and preview scenes, and
`ptz_scene_changed` makes the device check them again.
A position isn't in the camera's own units: a driver scales it to these ranges,
and clamps what a camera reports outside them.

Keys that start with `user_` are a user's own, for a camera given commands the
plugin doesn't have, and are never ones the plugin has.

## Features

`features` is how a caller finds out what to offer, rather than trying things
and seeing what fails.
A device without a `features` key predates them, and a caller then assumes
nothing.
What a device has depends on its driver and, for most, on the camera behind it:
a VISCA device gets them from the commands its camera profile has, an ONVIF
device from the services the camera offers, a USB device from the controls the
camera reports.
So a caller reads `features` again after a `state_changed` that carries it, and
doesn't keep it.

| Feature | The device can |
| --- | --- |
| `pantilt`, `zoom`, `focus` | Move on that axis at a speed: `ptz_move` |
| `pantilt_abs`, `zoom_abs`, `focus_abs` | Go to a position: `ptz_move_abs` |
| `pantilt_rel` | Move pan and tilt by a distance: `ptz_move_rel` |
| `home` | Go to its home position: `ptz_home_recall` |
| `home_set` | Save its current position as home: `ptz_home_save` |
| `autofocus` | Turn autofocus on and off, with the `focus_af_enabled` state key |
| `focus_onetouch` | Focus once, with the `focus_onetouch` trigger |
| `presets` | Save, recall and clear presets: the `ptz_preset_*` procs |
| `power` | Be powered on and off, which `power_on` in the state reports |
| `wb_onepush` | Set white balance once, with the `wb_onepush` trigger |
| `diagnostics` | Make a camera report, with the `camera_report` trigger |

Asking for something a device doesn't have the feature for isn't an error a
caller can detect, and what happens isn't promised: check `features` first.

## Config keys

Config is persistent: it is what the device saves, in the PTZ Control filter's
own settings.
It is read with `ptz_get_config`, changed with `ptz_set_config`, and every
change is announced by `settings_changed`.

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `name` | string | - | The OBS source the device is on, "" if it has none. Only a device with no filter (`is-self-managed`) takes it from `ptz_set_config`: a filter's is its parent's |
| `id` | int | - | The device's id, the `device_id` in the procs and signals. Read-only |
| `type` | string | - | The kind of device, such as `visca-over-ip`, `visca-over-tcp`, `pelco`, `onvif` or `usb-cam`. Read-only |
| `is-self-managed` | bool | - | The device has no filter, and the plugin owns its settings. Read-only |
| `pantilt_speed_max`, `zoom_speed_max`, `focus_speed_max` | number | 1.0 | A cap on the speed a move asks for: a `ptz_move` speed above it is clamped to it, whichever way it points. 0.1 to 1.0 |
| `pan_invert`, `tilt_invert`, `zoom_invert`, `focus_invert` | bool | false | Reverse the direction of the axis |
| `preset_max` | int | 16 | The most presets the device keeps, 1 to 128. It is also the `max_presets` that `ptz_preset_get_list` returns |
| `presets` | array | empty | The presets, in display order, each an object with an `id` and `name` and whatever the driver keeps to recall it. Edit it with the `ptz_preset_*` procs rather than by writing it |

`name`, `id` and `is-self-managed` identify the device rather than set it: for
a filter, `ptz_set_config` drops them.
A driver's connection settings (`host`, the ports, `serial_port`, `address`,
...) are config keys too, and what they are is the driver's to say.
`ptz_get_properties` lists the ones a user can edit, with their types and
ranges.

## Triggers

`ptz_trigger` names a one-shot action rather than a value, so a trigger isn't
state.
A device has the ones its [features](#features) name.
A name that starts with `user_` is a user's own.

| Trigger | Devices | Meaning |
| --- | --- | --- |
| `focus_onetouch` | with the `focus_onetouch` feature | Focus once, then hold |
| `wb_onepush` | with the `wb_onepush` feature | Set the white balance once, from what the camera sees |
| `camera_report` | VISCA | Ask the camera what it has, and make the report that `ptz_get_camera_report` returns, with its progress in the `camera_report` state key |
| `discover_limits` | VISCA | Find the camera's pan, tilt, zoom and focus ranges by moving it to their ends |
