# PTZ device API

This is the API used to interact with PTZ Devices.
It covers how to find and manage PTZ devices, to move them, to recall
their presets, and to be notified of state changes.
It is the API that PTZ Controls uses internally to communicate between the front
end UI and the back end protocol drivers.

Any plugin can also implement this API, either to control PTZ
devices, or to implement PTZ device instances and have them exposed to
PTZ controllers.

This file is the specification.
`tests/obs-integration/test_api_doc.py` reads it and holds the plugin to it.
The signatures in the headings and the names in the tables below are checked
against the source, and the keys against a running plugin.
A change to the API that isn't made here fails that test.

## Design

A PTZ device is a source in the OBS source tree, an input or a filter, that
implements the PTZ Device API.
A PTZ controller is a plugin or script that consumes the PTZ Device to
read camera status or send camera commands.
The PTZ Controls Dock is an example of a controller.

note: PTZ Controls implements PTZ Devices as source filters, but a regular
source can implement the API as well.

The API consists of endpoints registered with the PTZ device's
`proc_handler` and `signal_handler`.
`proc_handler` endpoints are used by a controller to send commands
and query the device.
`signal_handler` endpoints are used to notify the controller of state
changes in the camera.

See [AGENTS.md](../AGENTS.md) for the design it is part of.

## API version

This is version **0.1** of the PTZ API.
The API is in a pre-release state, published as an RFC.
It is subject to change at any time.
Please provide feedback on the [API discussion
page](https://github.com/glikely/obs-ptz/discussions/391)

A caller asks a device which version it implements with `ptz_get_api_version`
on the device's own `proc_handler`, before relying on anything else here:

```c
proc_handler_t *ph = obs_source_get_proc_handler(source);
calldata_t cd = {0};
if (proc_handler_call(ph, "ptz_get_api_version", &cd)) {
	long long major = calldata_int(&cd, "major");
	long long minor = calldata_int(&cd, "minor");
}
calldata_free(&cd);
```

The version covers the procs and signals below, and the calldata fields, state
keys and trigger names they take.
The minor version goes up when something is added that doesn't change the
existing API.
The major version goes up, and the minor goes back to 0, when something
is removed, renamed or changes meaning.
So a caller written for version M.m works with any version M.n where n >= m.
For example, a caller written for 1.0 works with any 1.x.

Controllers must check the version number on each PTZ device.
PTZ device instances can come from different plugins, which may not all
implement the same API version.
The API level check must be made before making any other calls to the
device.

## Quick start

PTZ devices are exposed as OBS sources.
This API is attached to the source's proc_handler and signal_handler.
A controller finds them by walking the source tree, and asking each source and
each of its filters for its version.

```c
/* the version this caller was written for */
#define WRITTEN_FOR_MAJOR 0
#define WRITTEN_FOR_MINOR 1

static bool is_ptz_device(obs_source_t *source)
{
	proc_handler_t *ph = obs_source_get_proc_handler(source);
	calldata_t v = {0};
	bool ok = proc_handler_call(ph, "ptz_get_api_version", &v) &&
		  calldata_int(&v, "major") == WRITTEN_FOR_MAJOR &&
		  calldata_int(&v, "minor") >= WRITTEN_FOR_MINOR;
	calldata_free(&v);
	return ok;
}

static void use_device(obs_source_t *source, void *data)
{
	signal_handler_t *sh = obs_source_get_signal_handler(source);
	signal_handler_connect(sh, "ptz_state_changed", device_state_changed, data);
	/* keep a weak reference to the source, for as long as it exists */
}

static void each_filter(obs_source_t *parent, obs_source_t *filter, void *data)
{
	if (is_ptz_device(filter))
		use_device(filter, data);
}

static bool each_source(void *data, obs_source_t *source)
{
	if (is_ptz_device(source))
		use_device(source, data);
	obs_source_enum_filters(source, each_filter, data);
	return true;
}

/* for every source: obs_enum_sources(each_source, data);
 * obs_enum_all_sources() has the private ones as well */
```

To hear of devices added later, connect two of the global signals, on
`obs_get_signal_handler()`: `source_create`, whose calldata has the `source`,
for a device that is a source, and `source_filter_add`, whose calldata has the
`source` and the `filter`, for one that is a filter.
Ask each for its version, as above.
A private source never says it was created, so a device that is one is not
heard of that way.
The plugin does both.

Then call procs on a device's handler by name, with the fields each one reads
(they are listed under each proc below):

```c
calldata_t cd = {0};
calldata_set_float(&cd, "pan", 0.5);
calldata_set_float(&cd, "tilt", 0.0);
proc_handler_call(ph, "ptz_move", &cd);   /* pan right at half speed */
calldata_free(&cd);
```

And read what changed in a `ptz_state_changed` handler:

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

To store a long term reference to a PTZ Device that can be saved to a
configuration file, record its UUID from `obs_source_get_uuid()`:
OBS saves it in the scene collection, so it is the same on the next run, and
`obs_get_source_by_uuid()` finds the device again.
A caller that has only the UUID looks the device up, and calls the proc before
it releases the reference, which is what keeps the proc_handler good:

```c
obs_source_t *source = obs_get_source_by_uuid(uuid);
if (source) {
	calldata_t cd = {0};
	calldata_set_int(&cd, "preset_id", 3);
	proc_handler_call(obs_source_get_proc_handler(source), "ptz_preset_recall", &cd);
	calldata_free(&cd);
	obs_source_release(source);
}
```

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

## Providing a device

Any plugin can provide devices, and the plugin's own camera list and dock find
them the same way another caller would, with nothing private between them.

A device is a source, either a regular source (`OBS_SOURCE_TYPE_INPUT`) or a
filter (`OBS_SOURCE_TYPE_FILTER`), whose proc_handler has the
[procs below](#per-device-proc_handler) and whose signal_handler has the
[signals](#per-device-signal_handler).
It is found when OBS says a source was created, with the global `source_create`
signal, or a filter was added to a source, with `source_filter_add`, and its
proc_handler answers `ptz_get_api_version` with the same major version as the
finder's and a minor version at least as new.
A source made private is not announced by OBS, and so is not found.
Nothing else of the plugin's is needed.
It needs the state keys that are "always" there, `source` among them, and the
procs and signals for what it can do, which its `features` say.
A proc a device doesn't have is read as something it can't do.
To be edited from a controller's settings page, a device has a `get_properties`
in its `obs_source_info`, as any source does for its settings.

A device is listed until its source is destroyed, which its own `destroy`
signal, the one every OBS source has, says.
A filter that is added to a source again is the same device.

## Per-device proc_handler

The device's own proc_handler: its source's, from
`obs_source_get_proc_handler()`.
All its procs start with `ptz_`, so they can be added to an existing
proc_handler with low risk of conflicts.

### `void ptz_get_api_version(out int major, out int minor)`

The version of the PTZ API this device implements.
Each device may implement a different version of the API.
Controllers must not assume that all devices implement the same API version.
A caller must check each device's own before relying on anything else in the API.
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

Returns, as `return`, the OBS source whose video the camera shows: for a device
that is a filter the source it is on, and for one that is a source, itself.
It is a new reference, which the caller releases with `obs_source_release()`,
and it is null while the device has no source, or the source has been removed.
Any thread.

### `void ptz_set_locked(bool locked)`

Locks or unlocks the device, which refuses to be moved while it is locked, and
reports it as the `locked` state key.
Reads `locked` (bool).
A device is also locked by the plugin whenever it is live.
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
Announces `ptz_preset_inserted`.
Device thread.

### `void ptz_preset_remove(int row)`

Removes the preset at display row `row`, which has to be a row in the list.
Announces `ptz_preset_removed`.
Device thread.

### `void ptz_preset_move(int src_row, int dest_row)`

Moves the preset at display row `src_row` to before the one at `dest_row`,
where `dest_row` is its index before `src_row` is taken out, as
`QAbstractItemModel::moveRows()` has it.
Announces `ptz_preset_moved`.
Device thread.

### `void ptz_preset_set_name(int id, string name)`

Names the preset `id`, which is its `id` and not its row.
Announces `ptz_preset_renamed`.
Device thread.

### `void ptz_scene_changed()`

Tells the device that the program or preview scene changed, so it checks again
whether it is live or in the preview, and so locked.
A caller that changes scenes without the OBS frontend knowing calls it.
Device thread.

## Per-device signal_handler

The device's own signal_handler: its source's, from
`obs_source_get_signal_handler()`.
All its signals start with `ptz_`, so they do not conflict with the signals OBS
gives every source, such as `destroy` and `update`.
A signal fires on the device's own thread.
Each carries `source`, the device's source as an `obs_source_t *`, lent for the
call, for a listener that connects the same function to more than one device
and needs to tell them apart.

### `void ptz_state_changed(ptr source, ptr changed)`

The device's state changed.
`changed` is an `obs_data_t *` holding only the values that changed, with the
keys of [State keys](#state-keys).
A listener may keep a reference to it, but must not change it: every listener
gets the same one, and the device never touches it again.

### `void ptz_preset_inserted(ptr source, int row)`

A preset was added at display row `row`.

### `void ptz_preset_removed(ptr source, int row)`

The preset that was at display row `row` was removed.

### `void ptz_preset_moved(ptr source, int src_row, int dest_row)`

A preset moved from display row `src_row` to before `dest_row`, which is its
index before `src_row` was taken out.

### `void ptz_preset_renamed(ptr source, int id)`

The name of preset `id` changed.

### `void ptz_preset_thumbnail_changed(ptr source, int id)`

The thumbnail of preset `id` changed.

## State keys

`ptz_get_state` fills in an `obs_data_t` that the caller owns.
The keys here are the ones every device has, whatever its protocol.
A driver adds its own (a VISCA device has `pan_pos` and `vendor_id`, say), so a
caller ignores keys it doesn't know and checks that one is there, with
`obs_data_has_user_value()`, before it reads it.

State is transient: it describes the device right now, and is never saved.
Changes to it are announced by `ptz_state_changed`, which carries only the keys
that changed.

| Key | Type | Present | Meaning |
| --- | --- | --- | --- |
| `connected` | bool | always | The device has a working link to its camera |
| `live` | bool | always | The device's source is in the program scene |
| `preview` | bool | always | The device's source is in the preview scene (studio mode only) |
| `source` | string | always | The name of the OBS source the device is on: for a filter the source it is on, or the last it was on while it is on none, "" if it never has been; for a source its own name. Follows the source being renamed |
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
So a caller reads `features` again after a `ptz_state_changed` that carries it, and
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

Config is persistent: it is what the device saves, in its source's own
settings.
What a device changes itself, such as its presets, it writes to the source's
settings as it changes it, so they are the one copy.
A device can also say what a blank setting would be as the default of
`<key>:placeholder` in the source's settings, such as `host:placeholder` for the
host its source receives from. A default is not saved, and a controller can show
it in the empty field.
It is read with `obs_source_get_settings()`, changed with
`obs_source_update()`, and every change is announced by the source's own
`update` signal.
A device that changes its settings itself, as when it finds its movement limits,
writes them and calls `obs_source_update()` to say so.

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `type` | string | - | The kind of device, such as `visca-over-ip`, `visca-over-tcp`, `pelco`, `onvif` or `usb-cam`. Read-only |
| `pantilt_speed_max`, `zoom_speed_max`, `focus_speed_max` | number | 1.0 | A cap on the speed a move asks for: a `ptz_move` speed above it is clamped to it, whichever way it points. 0.1 to 1.0 |
| `pan_invert`, `tilt_invert`, `zoom_invert`, `focus_invert` | bool | false | Reverse the direction of the axis |
| `preset_max` | int | 16 | The most presets the device keeps, 1 to 128. It is also the `max_presets` that `ptz_preset_get_list` returns |
| `presets` | array | empty | The presets, in display order, each an object with an `id` and `name` and whatever the driver keeps to recall it. Edit it with the `ptz_preset_*` procs rather than by writing it |

A driver's connection settings (`host`, the ports, `serial_port`, `address`,
...) are config keys too, and what they are is the driver's to say.
The source's properties, which `obs_source_properties()` gives, list the ones a
user can edit, with their types and ranges.

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
