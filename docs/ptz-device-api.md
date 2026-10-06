# PTZ Device API

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

## API version

This is version **0.2** of the PTZ API.
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
	calldata_set_string(&cd, "id", "camera:3");
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

Any plugin can provide devices.
A device is a source, either a regular source (`OBS_SOURCE_TYPE_INPUT`) or a
filter (`OBS_SOURCE_TYPE_FILTER`), whose proc_handler has the
[procs below](#per-device-proc_handler) and whose signal_handler has the
[signals](#per-device-signal_handler).
It is found when OBS says a source was created, with the global `source_create`
signal, or a filter was added to a source, with `source_filter_add`, and its
proc_handler answers `ptz_get_api_version` with the same major version as the
finder's and a minor version at least as new.
A source made private is not announced by OBS, and so is not found.
It needs the state keys that are "always" there, `source` among them, and the
procs and signals for what it can do, which its `features` say.
A proc a device doesn't have is read as something it can't do.
Device configuration is managed the same way as any other source, via
`save`, `update`, and `get_properties` in its `obs_source_info`.

## Per-device proc_handler

The following proc_handler calls are defined for PTZ Devices.
All the procs start with `ptz_`, so they can be added to an existing
proc_handler with low risk of conflicts.

### `void ptz_get_api_version(out int major, out int minor)`

Returns the version of the PTZ API this device implements.
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

Returns, as `return`, an `obs_data_t *` that the caller releases with
`obs_data_release()`, describing the presets:

- `presets` (object): the presets, each an object with the fields in
  [Presets](#presets), under its `id`. It is in no order.
- `order` (array): the display order, each an object with an `id`. It has every
  preset in `presets` once, and nothing else.
- `stores` (object): the stores the device can make a preset in, `camera` and
  `local`, each `true`. A set of names is an object of bools, as it is in
  `features`, since an `obs_data_t` has no array of strings.
- `names_on_camera` (bool): the camera keeps the preset's name, and a rename is
  sent to it.
- `camera_slots` (int): how many presets the camera keeps, or 0 if it doesn't
  keep a fixed number: the `preset_max` setting.
- `enumerable` (bool): the camera can say what presets it has.
  If not, its presets are the ones this device has saved.
- `value_keys` (object): what a local preset captures and a recall applies,
  each `true`.

Device thread.

### `void ptz_preset_save(string id)`

Saves the current position into the preset `id`, replacing what it held.
A preset of the camera store is written to the camera, and a thumbnail of the
device's source is taken.
A preset of the local store captures every value the device reports, see
[Presets](#presets).
Needs the `presets` feature.
Any thread.

### `void ptz_preset_recall(string id)`

Goes to the preset `id`.
A preset of the camera store is the camera's to recall.
One of the local store's goes to the pan, tilt, zoom and focus it has.
Needs the `presets` feature.
Any thread.

### `string ptz_preset_create(string name, string store)`

Makes a preset in `store` from the current position, and returns its `id` as
`return`, or "" if it can't: the store is not one the device has, or the camera
has no free slot.
The `name` is the user's, and "" for none.
Announces `ptz_preset_added`.
Device thread.

### `void ptz_preset_delete(string id)`

Removes the preset `id`: clears its slot on the camera, or drops its record,
and its thumbnail.
Announces `ptz_preset_removed`.
Device thread.

### `void ptz_preset_update(string id, ptr changes)`

Changes the preset `id` by the keys that are in the `changes` object the caller
owns, and leaves the others as they are:

- `name` (string): the user's name for it. On a camera that keeps names it is
  sent to the camera.
- `thumbnail` (string): the path of its thumbnail image, or "" for none.
- `values` (object): the values a recall applies, which replace those it has.
  A local preset only.

Announces `ptz_preset_changed`.
Device thread.

### `void ptz_preset_move(string id, int index)`

Moves the preset `id` to display `index` in `order`, counting from 0 in the list
as it is once it has been taken out of its old place.
Announces `ptz_preset_order_changed`.
Device thread.

### `void ptz_preset_refresh()`

Asks the camera for its presets again.
Announces `ptz_preset_list_reset` once it has them.
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

### `void ptz_preset_added(ptr source, string id)`

A preset was added, last in `order`.

### `void ptz_preset_removed(ptr source, string id)`

The preset `id` was removed.

### `void ptz_preset_order_changed(ptr source)`

The display order changed, other than by a preset being added or removed.
A listener reads `order` again with `ptz_preset_get_list`.

### `void ptz_preset_changed(ptr source, string id, ptr changed)`

The preset `id` changed.
`changed` holds just the keys that did, as `ptz_preset_update` takes them, and
`camera_name`.

### `void ptz_preset_list_reset(ptr source)`

The list changed more than the signals above say, as when the camera's presets
were read again.
A listener reads it again with `ptz_preset_get_list`.

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
| `source` | string | always | The name of the OBS source the device is on: for a filter the source it is on, or the last it was on while it is on none, "" if it never has been; for a source its own name. Follows the source being renamed |
| `features` | object | always | What the device can do, with each [feature](#features) it has `true` and none for one it hasn't. It can change while the device runs, as it finds out what its camera has |
| `pan`, `tilt` | number | when known | Position, -1.0 to 1.0, as the camera last reported it. Absent until it has |
| `zoom`, `focus` | number | when known | Position, 0.0 to 1.0, as above |
| `focus_af_enabled` | bool | when known | Autofocus is on. Can be changed with `ptz_request_state` |
| `camera_report` | object | when known | Progress of a `camera_report` trigger while one is running |

A position isn't in the camera's own units: a driver scales it to these ranges,
and clamps what a camera reports outside them.

Keys that start with `user_` are a user's own, for a camera given commands the
plugin doesn't have, and are never ones the plugin has.

## Presets

A preset is where the camera goes, and a few things it does when it gets there.
It is in one of two stores, the camera's or the local, and a device can
have both: the camera's presets are in the camera, and the local's are
in the device's settings, where it takes the camera to the position they hold
with an absolute move.
So any device that can move to a position can have local presets, with no
help from its camera.

An `id` is a string that belongs to the device, and that a caller keeps and gives
back as it is.
It has the store in it, `camera:3` or `local:ab12cd34`, so ids from the two
never meet: the part after the colon is the driver's own key for a camera preset
(VISCA's slot, ONVIF's token), and one the device made for a local preset.
It is the same from one run to the next.
A preset stays in the store it was made in, so putting one in the other is
making another, with another `id`.

A preset in the list is an object with:

| Key | Type | Meaning |
| --- | --- | --- |
| `id` | string | See above |
| `store` | string | `camera` or `local` |
| `name` | string | What to show: the name the user gave it, or if there is none the camera's own |
| `camera_name` | string | What the camera calls it, "" if it doesn't |
| `thumbnail` | string | The path of its thumbnail image, or "" |
| `values` | object | A local preset's saved values, below |

The display order is the controller's, over both stores, and is `order` in
`ptz_preset_get_list`, which is kept apart from the presets so that they can be
in whatever order suits the camera.
A camera can't change it, and a preset it adds goes last.
It is kept in the device's settings.
What the camera keeps of a preset, whether it has one and where it goes, is
read from the camera and not saved.
What the user adds to it, a name the camera can't keep and a thumbnail, is kept
by the device, by `id`.

A local preset saves every value the device can, whenever it is saved: the
`value_keys` of `ptz_preset_get_list`, which are these, and of which a camera
has the ones it does.
A recall applies all of them:

| Key | Holds | A recall |
| --- | --- | --- |
| `pan`, `tilt` | a position, -1.0 to 1.0 | goes to it, with `ptz_move_abs` |
| `zoom` | a position, 0.0 to 1.0 | goes to it, with `ptz_move_abs` |
| `focus` | `af_enabled`, a bool, with the `autofocus` feature, and `position`, 0.0 to 1.0, with `focus_abs` | turns autofocus on or off first, and goes to the position if it is off |

Nothing else is saved or restored: not the white balance, or any other state of
the camera.
A preset that has `pan` without `tilt`, or `tilt` without `pan`, as one made by
`ptz_preset_update` can, takes the other from where the camera is: from the
latest state, and if it hasn't one yet the recall logs that and does nothing for
them.

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
| `presets` | Save, recall and delete presets: the `ptz_preset_*` procs |
| `power` | Be powered on and off, which `power_on` in the state reports |
| `wb_onepush` | Set white balance once, with the `wb_onepush` trigger |
| `diagnostics` | Make a camera report, with the `camera_report` trigger |
| `tally_light` | Light its camera's tally lamps, which the `tally_auto` setting has it do while its source is in the program or preview scene |

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
| `tally_auto` | bool | true | Light the camera's tally lamps by itself, for a device with the `tally_light` feature: red while its source is in the program scene, green while it is in the preview scene (studio mode only) and not in the program scene. Turn it off for a camera whose tally something else drives. A device without the feature has the key and ignores it |
| `preset_max` | int | 16 | The most presets in the camera store, 1 to 128. It is also the `camera_slots` that `ptz_preset_get_list` returns |
| `presets` | array | empty | The presets the device keeps: each local preset in full, and for a camera's what the user added to it, an `id` with a `name` and `thumbnail`. Edit it with the `ptz_preset_*` procs rather than by writing it |
| `preset_order` | array | empty | The display order, as an `id` object for each preset. One that is left out goes last, and one that is not there is ignored |

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
