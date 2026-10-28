# PTZ Device API

This is the API used to interact with PTZ Devices.
It covers how to find and manage PTZ devices, to move them, to recall
their presets, and to be notified of state changes.
It is the API that PTZ Controls uses to communicate between the front
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

This is version **0.8** of the PTZ API.
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

	/* Then read the state as it is: the signal says only what changes, and
	 * a device that is not changing says nothing. Any thread may. */
	obs_data_t *state = obs_data_create();
	calldata_t cd = {0};
	calldata_set_ptr(&cd, "state", state);
	proc_handler_call(obs_source_get_proc_handler(source), "ptz_get_state", &cd);
	calldata_free(&cd);
	/* ...use it, and release it with obs_data_release(state) */

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
  So can `ptz_get_state`, which gives a copy of the state that the device
  makes as it announces it.
  A proc that reads or changes the device's other data, or returns something,
  must be called from that thread.
  From another it logs an error and does nothing.
  Each proc below says which it is.
- **Reading the state.** `ptz_state_changed` says only what changed, and a
  device that is not changing says nothing, so a listener that has just found
  a device does not know what it is like.
  It reads the whole state once with `ptz_get_state`, and keeps it up to date
  from the signals.
  Connect to the signal first and read after: what is read is the state as the
  device last announced it, or newer, so it is at least as new as every signal
  the listener has heard, and a change that came between the two is in it and
  is announced as well, which does no harm, as a change is a value and not a
  difference.
  Read first and connect after, and a change in between is lost.
- **Ownership.** A `ptr` the caller passes in (the `state`, `config` and
  `statistics` objects) is the caller's: the device fills it in.
  A `ptr` a proc returns as `return` is a new reference or object that the
  caller releases, and each proc below says with what.
  A `ptr` in a signal belongs to the plugin.
- **Fields.** A declaration lists every field a proc can be given, and not
  which ones it has to be: libobs has no way to declare a parameter optional.
  So each proc below marks each field it reads *required* or *optional*.
  An optional field a caller leaves out is not given, and not zero: the proc
  says what it does without it, as the move procs leave an axis that isn't
  given as it is.
  A proc that is not given a required field does nothing, unless it says
  otherwise.
  An `out` field is the proc's to set, and not the caller's to give.
- **Units.** A speed is -1.0 to 1.0 and a position is -1.0 to 1.0 for pan and
  tilt and 0.0 to 1.0 for zoom and focus, whatever the camera's own units.

## WebSocket binding

A program outside OBS uses the API over
[obs-websocket](https://github.com/obsproject/obs-websocket), which OBS has
built in.
PTZ Controls registers a vendor there, `obs-ptz`, with a request for each proc
of the API and an event for each signal, so that what is written of a proc or a
signal in this document is true of its request or its event.
Nothing is added for a client to learn but what is in this section.

This is version **1.0** of the binding, which `ptz_bridge_version` answers
with.
It is the binding's own, and goes up as the API's version does, when the
binding changes: the API's version is still each device's own, from
`ptz_get_api_version`.

### Calling a proc

A client calls obs-websocket's `CallVendorRequest`, with the vendor `obs-ptz`,
the name of the proc as the request, and the proc's fields as the request's
data, with one more:

- `device` (string): the UUID of the device, which `ptz_list_devices` gives, and
  which is the same from one run of OBS to the next; or `selected`, for the
  camera selected in the PTZ dock.

```json
{
  "requestType": "CallVendorRequest",
  "requestData": {
    "vendorName": "obs-ptz",
    "requestType": "ptz_move",
    "requestData": {"device": "c7d8...", "pan": 0.5, "tilt": 0.0}
  }
}
```

The answer's data has what the proc gave back: each field its signature
declares as `out`, under its name, what it returns as `return`, and each `ptr`
the client passed in, as the proc left it.
A proc that gives nothing back answers with nothing, `{}`.

A client need not think about threads.
A proc that is "Device thread" is run on OBS's main thread and waited for, for
up to five seconds, and one that is "Any thread" is run at once.
Requests may be made at the same time, on one connection or on several, and
each is answered when it is done: two that are sent one after the other are
not always answered in that order.

A device from another plugin has requests and events if it has
`ptz_get_api_info`, which is where the binding learns what a device has.
One that has not is in `ptz_list_devices`, and that is all.

### The selected camera

A request whose `device` is `selected` is for whichever camera is selected in
the PTZ dock as the request comes, as the dock's own hotkeys are: a client
that only sends requests can control the camera the operator is working on
without following the selection itself.
Its answer has one field more, `device`, the UUID of the camera that was
selected, and that is the camera to send what follows to, when it has to be the
same one: the stop at the end of a move, which a client that sent it to
`selected` would send to another camera if the selection had changed in
between.
With no camera selected it answers `no_selection`.

The selection is never in an event of a device, which always names the device
by its UUID: a client that shows the selected camera's state, its presets or
where it is, follows the selection with `ptz_get_selected` and the
`ptz_selected_changed` event.

### Fields

A field is the JSON that its declared type takes:

| Type | JSON | |
| --- | --- | --- |
| `string` | string | |
| `int` | number | A whole number |
| `float` | number | A whole number is one too |
| `bool` | `true` or `false` | |
| `ptr` | object | An `obs_data_t`: the settings, state or changes the proc documents. One the proc fills in for the caller, as `ptz_get_state`'s `state` is, can be left out, and comes back in the answer all the same |

A field that is left out is not given, as for any caller.
A field that the proc doesn't have is an error, `unknown_field`, and the proc
is not called: a misspelt field doesn't move a camera somewhere else.

### Errors

A request that could not be done answers with `error`, one of the codes below,
and `message`, which says more, for a person to read.
obs-websocket itself reports the request as having succeeded: it is the
answer's data that has the error.

| Error | |
| --- | --- |
| `unknown_device` | `device` is missing, or is not the UUID of a PTZ device that exists |
| `no_selection` | `device` is `selected`, and no camera is selected in the PTZ dock |
| `no_such_proc` | The device hasn't this proc, as a discovery instance hasn't `ptz_move` |
| `unknown_field` | The request has a field that the proc doesn't have |
| `bad_type` | A field is not of the type the proc declares for it |
| `forbidden` | The proc is not allowed by the access setting, which was changed as the request was made |
| `timeout` | A "Device thread" proc was not run within five seconds, as OBS's main thread was busy. It may still run |
| `unavailable` | OBS is closing |

A request for a proc that the access setting doesn't allow, or for one that no
device there has ever been has had, is not a request at all: obs-websocket
answers that there is no request by that name.

### Events

Each signal of a device is an event of the vendor, by the signal's name, with
the signal's fields as its data, but for `source`: in its place is `device`, the
UUID of the device the signal is from.
A `ptr` field is an object, as it is in a request.
obs-websocket sends a vendor's event as a `VendorEvent`, which has the vendor,
the event's name, and its data:

```json
{
  "eventType": "VendorEvent",
  "eventData": {
    "vendorName": "obs-ptz",
    "eventType": "ptz_state_changed",
    "eventData": {"device": "c7d8...", "changed": {"pan": 0.25}}
  }
}
```

A client is sent them only if it asks for obs-websocket's `Vendors` events when
it identifies itself, `1 << 9` in its `eventSubscriptions`: they are not
among the events a client gets if it says nothing.

A device says a great deal as its camera moves.
A client that subscribes has to keep reading what it is sent.

### Access

One setting, in the plugin's settings and as `websocket_access` in its
`config.json`, says how much of this a client is let do:

| Setting | A client can |
| --- | --- |
| `off` | Do nothing: the vendor has no request, and no event is sent |
| `read-only` | Call the procs that are "Access: read", and hear every event |
| `control` | Call every proc but those that are "Access: none", and hear every event. The default |

It is changed while OBS runs, with no client disconnected.
A `websocket_access` that is none of the three is taken as `off`.

obs-websocket has one password, and no permissions: whatever has the password
is let do everything the setting allows, to every camera.
The plugin is not told which client a request is from, so the setting is the
same for all of them, and is no substitute for the password.

### Requests of the binding's own

These are not procs of a device, and take no `device`.
They are there unless the setting is `off`, but for `ptz_list_discovery`, which
makes sources and is there only when the setting is `control`.

| Request | Answers |
| --- | --- |
| `ptz_bridge_version` | `major` and `minor`: the version of the binding |
| `ptz_get_selected` | `device`: the UUID of the camera selected in the PTZ dock, "" if none is |
| `ptz_list_devices` | `devices`: an array with, for each PTZ device there is, its `device` (the UUID), its `name`, the `source` it is on (the name of the source whose video the camera shows), and its `api`, the `major` and `minor` of the version of the API it implements |
| `ptz_list_discovery` | `instances`: an array with, for each [discovery kind](#discovery-kinds) there is, the `kind` and the `device` (the UUID) of a discovery instance of it, to call `ptz_discovery_start` on. The binding makes them the first time it is asked, and keeps them: a client can't make one. What an instance finds can be added as a device with obs-websocket's own `CreateSourceFilter`, of the `kind` and with the `settings` its `ptz_device_found` gives |
| `ptz_list_api` | The requests and events there are for the API, as four arrays, `device-proc`, `device-signal`, `discovery-proc` and `discovery-signal`, of an object for each with its `name`. What a device or an instance has is not there until there has been one |

### Events of the binding's own

| Event | |
| --- | --- |
| `ptz_device_removed` | `device`: a device has gone. No signal of the API says so, as there is no device left to say it |
| `ptz_selected_changed` | `device`: another camera is selected in the PTZ dock, by the user, a hotkey, or the dock following the scene; "" if none is |

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

## Driver kinds

For controllers that create new ptz devices, the controller needs to
know which filters will create new devices.
A plugin that provides devices as filters advertises itself by putting the ptz
api version into the data returned by its `obs_source_info->get_defaults()` callback.
The following two default values must be set for a filter that implementes the PTZ api.
The values must be the same as the values returned by `ptz_get_api_version`.

| Key                     | Type | Value                       |
|-------------------------|------|-----------------------------|
| `ptz_api_version_major` | int  | The major version           |
| `ptz_api_version_minor` | int  | The minor version           |

They are defaults, not settings: they are never saved in a scene collection.
A controller finds the kinds by walking `obs_enum_filter_types()` and keeping
those whose `obs_get_source_defaults()` has the marker.

A device is made from a kind by creating a filter of that kind on a source
with `obs_source_create()` and `obs_source_filter_add()`.

## Per-device proc_handler

The following proc_handler calls are defined for PTZ Devices.
All the procs start with `ptz_`, so they can be added to an existing
proc_handler with low risk of conflicts.

Each proc says which thread it may be called from, "Any thread." or "Device
thread.", as [Rules for calling](#rules-for-calling) has it.
A proc with an "Access:" line is one that a caller outside OBS is treated
differently for: `read` if it only reads the device, and so is let through
where control of the camera is not, and `none` if it can't be called from
outside OBS at all.
A proc with no such line controls the device.
`ptz_get_api_info` gives both, for each proc.

Each field a proc reads is marked *required* or *optional*, which its
declaration can't say: see Fields in [Rules for calling](#rules-for-calling).

### `void ptz_get_api_version(out int major, out int minor)`

Returns the version of the PTZ API this device implements.
Each device may implement a different version of the API.
Controllers must not assume that all devices implement the same API version.
A caller must check each device's own before relying on anything else in the API.
Any thread.
Access: read.

### `ptr ptz_get_api_info()`

Returns, as `return`, an `obs_data_t *` that the caller releases with
`obs_data_release()`, describing the procs and signals the device has, which a
caller has no other way to ask for: libobs calls a proc and connects a signal
only by a name the caller already knows.

- `procs` (array): each an object with the proc's `name`, the `decl` it is
  declared with (`"void ptz_preset_recall(string id)"`), the `thread` it may be
  called from, `"any"` or `"device"`, and the `access` a caller outside OBS
  needs to call it, `"read"`, `"control"` or `"none"`.
- `signals` (array): each an object with the signal's `name` and `decl`.

A caller takes a proc that has no `thread` as `"device"`, and one that has no
`access` as `"control"`.
Any thread.
Access: read.

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

### `void ptz_move(float pan, float tilt, float zoom, float focus)`

Moves at a speed, until told otherwise or stopped.
Needs the `pantilt`, `zoom` and `focus` features, for each axis it moves.
Any thread.
Reads, each optional, so that a caller gives only the axes it changes:

- `pan`, `tilt` (float, optional, -1.0 to 1.0): the speed.
  Each is an axis of its own: one given alone leaves the other at the speed it
  has.
  The sign is the direction, after the `pan_invert` and `tilt_invert` settings,
  and the speed is capped by `pan_speed_max` and `tilt_speed_max`.
- `zoom` (float, optional, -1.0 to 1.0): the zoom speed, capped by
  `zoom_speed_max`.
- `focus` (float, optional, -1.0 to 1.0): the focus speed, capped by
  `focus_speed_max`.

A zero stops that axis; an axis left out is left as it is.

The sign of a speed is the way its axis's position goes, as the state reports it:
a positive speed increases the position, a negative one decreases it.

### `void ptz_move_abs(float pan, float tilt, float zoom, float focus)`

Moves to a position and stops there.
Needs the `pantilt_abs`, `zoom_abs` and `focus_abs` features, for each axis it
moves.
Any thread.
Reads, each optional, so that a caller gives only the axes it moves:

- `pan`, `tilt` (float, optional, -1.0 to 1.0): the position.
  One given alone leaves the other where it is, which the device takes from
  its state's `pan` or `tilt`.
  If the state hasn't that, the device doesn't know where the other is, and
  doesn't move either of them.
- `zoom`, `focus` (float, optional, 0.0 to 1.0): the position.

An axis left out is left where it is.

### `void ptz_move_rel(float pan, float tilt)`

Moves by a distance.
Needs the `pantilt_rel` feature.
Any thread.
Reads, each optional, so that a caller gives only the axes it moves:

- `pan`, `tilt` (float, optional, -1.0 to 1.0): how far, as a fraction of the
  axis's whole travel.
  One given alone leaves the other where it is.

### `ptr ptz_get_state(ptr state)`

Fills in the `state` object the caller owns with the device's whole transient
state: see [State keys](#state-keys).
Reads `state` (ptr, required).
Any thread.
On the device's own thread that is the state as it is.
On another it is the copy made when the state was last announced with
`ptz_state_changed`, which the device makes before it says so, so it is never
older than a signal the caller has heard, and can be a moment behind a change
that has not been announced yet.
The caller's object shares nothing with the device or with another caller.
Access: read.

### `ptr ptz_get_statistics(ptr statistics)`

Fills in the `statistics` object the caller owns with what the device has
counted of its own working, such as how much it has sent to the camera and how
fast it answers, as numbers under names that the driver gives them.
Reads `statistics` (ptr, required).
It isn't state: it is read when wanted and never announced as changing, and a
rate in it is over the time since it was last read.
Device thread.
Access: read.

### `ptr ptz_get_parent_source()`

Returns, as `return`, the OBS source whose video the camera shows: for a device
that is a filter the source it is on, and for one that is a source, itself.
It is a new reference, which the caller releases with `obs_source_release()`,
and it is null while the device has no source, or the source has been removed.
Any thread.
Access: none.

### `void ptz_request_state(ptr state)`

Asks the device to change some of its state, which is never saved: the keys in
the `state` object the caller owns that the device acts on.
Reads `state` (ptr, required).
The state then changes when the camera reports it has, and not before.
Device thread.

### `void ptz_trigger(string name)`

Runs a one-shot action on the camera, which isn't state: see
[Triggers](#triggers).
Reads `name` (string, required).
An unknown name is logged and otherwise ignored.
Any thread.

### `void ptz_get_camera_report(out string report)`

Returns, as `report`, the last report of what the camera has, which its
`camera_report` trigger makes, as JSON, or "" if there isn't one.
It is for the user to look at and send in themselves; nothing in it identifies
the camera, the user, or where either is.
Device thread.
Access: read.

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
- `enumerable` (bool): the camera can say what presets it has.
  If not, its presets are the ones this device has saved.
- `value_keys` (object): what a local preset captures and a recall applies,
  each `true`.

Device thread.
Access: read.

### `void ptz_preset_save(string id)`

Saves the current position into the preset `id`, replacing what it held.
Reads `id` (string, required).
A preset of the camera store is written to the camera, and a thumbnail of the
device's source is taken.
A preset of the local store captures every value the device reports, see
[Presets](#presets).
Needs the `presets` feature.
Any thread.

### `void ptz_preset_recall(string id)`

Goes to the preset `id`.
Reads `id` (string, required).
A preset of the camera store is the camera's to recall.
One of the local store's goes to the pan, tilt, zoom and focus it has.
Announces `ptz_preset_recalled` when the camera has arrived, unless a newer
recall or a move comes first.
Needs the `presets` feature.
Any thread.

### `void ptz_preset_get_thumbnail(string id, out string mime, out string data)`

Returns the image of the thumbnail of the preset `id`, for a caller that can't
read the file whose path `ptz_preset_get_list` gives, as one outside the
machine OBS runs on can't: its type as `mime`, `image/jpeg` or `image/png`, and
the file's bytes, base64 encoded, as `data`.
Reads `id` (string, required).
Both are "" if the preset has no thumbnail, the device hasn't the preset, or
`id` is left out.
A thumbnail is small, a few kilobytes, and a caller asks for it again when
`ptz_preset_changed` says `thumbnail` changed.
Device thread.
Access: read.

### `void ptz_preset_capture_thumbnail(string id, bool if_arrived)`

Takes a thumbnail of the device's source for the preset `id`, replacing the one
it had, and announces it with `ptz_preset_changed`.
Reads `id` (string, required) and `if_arrived` (bool, optional).
Saving a preset takes one already; this is for a caller that wants one once a
recall has arrived, after `ptz_preset_recalled`.
With `if_arrived` true it does so only if `id` is the preset the last
`ptz_preset_recalled` announced and the camera has not been sent anywhere since,
by a recall or by the moves that signal names; so a caller that waits a moment
after the signal, for the video to catch up, gets no thumbnail of the camera on
its way somewhere else.
False or absent takes one regardless.
Does nothing for a preset the device has not.
Any thread.

### `string ptz_preset_create(string name, string store)`

Asks for a preset to be made in `store` from the current position, and returns the
request as `return`, or "" if the device refuses at once: the store is not one the
device has, or it knows it has no room for another.
It does not wait for the camera, which may take a while to say it made the
preset: how the request went is the `ptz_preset_create_done` signal's to say,
which comes after this has returned, whatever the store.
Until then there is no `id`; the preset is announced by `ptz_preset_added`
as it is made.
A camera may also be full without saying so, which a caller finds out from
the signal: the list has no count of how many a camera keeps.
Reads `name` (string, optional) and `store` (string, required).
The `name` is the user's, and "" or left out for none.
Left out, `store` is no store the device has, and the request is refused.
Device thread.

### `void ptz_preset_delete(string id)`

Removes the preset `id`: clears its slot on the camera, or drops its record,
and its thumbnail.
Reads `id` (string, required).
Announces `ptz_preset_removed`.
Device thread.

### `void ptz_preset_update(string id, ptr changes)`

Changes the preset `id` by the keys that are in the `changes` object the caller
owns, and leaves the others as they are.
Reads `id` (string, required) and `changes` (ptr, required), whose keys are
each optional:

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
Reads `id` (string, required) and `index` (int, required).
Left out, `index` is taken as 0.
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

### `void ptz_preset_create_done(ptr source, string request, string id)`

The request to make a preset, which `ptz_preset_create` returned `request` for, is
over.
`id` is the id of the preset that was made, which `ptz_preset_added` has already
announced, or "" if the camera made none.
Every request that `ptz_preset_create` returned gets one of these.
A call that it refused at once, with "", is not a request and gets none.

### `void ptz_preset_removed(ptr source, string id)`

The preset `id` was removed.

### `void ptz_preset_order_changed(ptr source)`

The display order changed, other than by a preset being added or removed.
A listener reads `order` again with `ptz_preset_get_list`.

### `void ptz_preset_changed(ptr source, string id, ptr changed)`

The preset `id` changed.
`changed` holds just the keys that did, as `ptz_preset_update` takes them, and
`camera_name`.

### `void ptz_preset_recalled(ptr source, string id)`

The camera has arrived at the preset `id`, which `ptz_preset_recall` sent it to,
as best the device can tell.
With the `move_completion` feature the camera said so (a VISCA camera by
completing the move, an ONVIF camera by its status saying it is no longer
moving), or, if the device never hears that (a completion lost, or a recall that
moved nothing), 15 seconds after the recall; without it, the device guesses, a
fixed 3 seconds after the recall.
Only the last recall is announced: a newer recall, or a `ptz_stop`,
`ptz_move_abs`, `ptz_move_rel`, `ptz_home_recall`, or a `ptz_move` with a speed
that is not 0, before it arrives, means it never does.

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
A listener reads the whole of it once, as [Rules for
calling](#rules-for-calling) says.

| Key | Type | Present | Meaning |
| --- | --- | --- | --- |
| `connected` | bool | always | The device has a working link to its camera |
| `source` | string | always | The name of the OBS source the device is on: for a filter the source it is on, or the last it was on while it is on none, "" if it never has been; for a source its own name. Follows the source being renamed |
| `features` | object | always | What the device can do, with each [feature](#features) it has `true` and none for one it hasn't. It can change while the device runs, as it finds out what its camera has |
| `pan`, `tilt` | number | when known | Position, -1.0 to 1.0, as the camera last reported it. Absent until it has |
| `zoom`, `focus` | number | when known | Position, 0.0 to 1.0, as above |
| `axes` | object | when known | What each of `pan`, `tilt`, `zoom` and `focus` means and how finely it is set, as an object for each axis that a driver knows something of: see [Axes](#axes) |
| `focus_af_enabled` | bool | when known | Autofocus is on. Can be changed with `ptz_request_state` |
| `power_on` | bool | when known | The camera is powered on, and not in standby. A camera in standby is still `connected`. Can be changed with `ptz_request_state` by a device with the `power` feature |
| `camera_report` | object | when known | Progress of a `camera_report` trigger while one is running |

A position isn't in the camera's own units: a driver scales it to these ranges,
and clamps what a camera reports outside them.

### Axes

A position is normalized: -1.0 to 1.0 for pan and tilt and 0.0 to 1.0 for zoom and
focus, whatever the camera's own units. `axes` says what it means in the units a
person knows, and how finely the camera sets it, for a client that shows or steps
a position. Each axis it names has these keys, all optional, and a client falls
back to the normalized position, or to a step of its own, for what is missing:

- `step` (number): the smallest change of the position that the camera makes, in
  the units of the position. A position moved by less than this may not move at
  all, so a client that steps a position, as a dial does, steps it by at least this.
- `unit` (string): what `points` are in: `deg` for a pan or a tilt, `x` for a zoom
  multiplier (`x` of 4.2 is a zoom of 4.2 times) and `m` for a focus distance in
  metres.
- `points` (array): at least two objects, in order of `position`, each with the
  `position` and the `value` the camera has there, in `unit`. A client finds the
  value of a position between two points by interpolating linearly, and does not
  extrapolate outside them. A point with no `value` is infinity, as the far end of a
  focus is. A lens is not linear, so a camera's points can be as many as it
  takes: two are enough for a pan or a tilt.

```json
"axes": {
	"pan": { "step": 0.00058, "unit": "deg", "points": [{ "position": -1, "value": -170 }, { "position": 1, "value": 170 }] },
	"focus": { "unit": "m", "points": [{ "position": 0, "value": 0.01 }, { "position": 0.5, "value": 0.6 }, { "position": 1 }] }
}
```

They are state, as they can change (`discover_limits` changes where a VISCA camera's
ends are), and `ptz_state_changed` carries only what changed of them.
, for a camera given commands the
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
| `power` | Be powered on and off, with the `power_on` state key |
| `wb_onepush` | Set white balance once, with the `wb_onepush` trigger |
| `diagnostics` | Make a camera report, with the `camera_report` trigger |
| `tally_light` | Light its camera's tally lamps, which the `tally_auto` setting has it do while its source is in the program or preview scene |
| `move_completion` | Say when its camera has arrived at a recalled preset because the camera said so, with `ptz_preset_recalled`, and after a longer backstop time if it never hears; without it, that signal comes after a fixed time. A VISCA device has it once its camera has completed a move (one that only ACKs, as a BirdDog does, never has it); an ONVIF device once its camera's status has said whether it is moving (`MoveStatus` `IDLE` or `MOVING`; many cameras don't say, and their devices never have it); a Soft PTZ device always |

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
| `type` | string | - | The kind of device, such as `visca-over-ip`, `visca-over-tcp`, `pelco`, `onvif`, `usb-cam` or `soft-ptz`. Read-only |
| `pan_speed_max`, `tilt_speed_max`, `zoom_speed_max`, `focus_speed_max` | number | 1.0 | A cap on the speed a move asks for: a `ptz_move` speed above it is clamped to it, whichever way it points. 0.1 to 1.0. The `pantilt_speed_max` of an older version, one cap for both, becomes `pan_speed_max` and `tilt_speed_max` |
| `pan_invert`, `tilt_invert`, `zoom_invert`, `focus_invert` | bool | false | Reverse the direction of the axis |
| `tally_auto` | bool | true | Light the camera's tally lamps by itself, for a device with the `tally_light` feature: red while its source is in the program scene, green while it is in the preview scene (studio mode only) and not in the program scene. Turn it off for a camera whose tally something else drives. A device without the feature has the key and ignores it |
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

## Discovery kinds

A plugin whose devices can be found, on the network or elsewhere, says how
with a second source kind next to its filter kind: an input kind whose id is
the filter kind's with `.discovery` after it
(`ca.secretlab.obs-ptz.visca.discovery`, say), with
`OBS_SOURCE_CAP_DISABLED` in its `output_flags`, so OBS never offers it as a
source to add, and the same marker in its defaults as a
[driver kind](#driver-kinds).
A controller finds them by walking `obs_enum_input_types()` and keeping those
whose id ends in `.discovery` and whose defaults have the marker, with the
major version of its own and a minor version at least as new; the id only
tells them from other input kinds.

To look for devices, a controller makes an instance of each with
`obs_source_create_private()`, connects to the
[signals](#per-discovery-signal_handler) on its own `signal_handler`, and
calls `ptz_discovery_start` on its own `proc_handler`.
Each instance says `ptz_discovery_finished` once a search is over, so a
controller that counts one per instance it made knows when they all have,
without knowing how many drivers there are.
Releasing the instance ends its search and frees what the driver made for
it: nothing is said once OBS has destroyed it.
A controller disconnects first, and may call `ptz_discovery_stop` first, to
stop it at once rather than when OBS gets round to destroying it.

## Per-discovery proc_handler

The procs on a discovery instance's own `proc_handler`, from
`obs_source_get_proc_handler()`.
Any thread: the instance does the work on the main thread.

### `void ptz_get_api_version(out int major, out int minor)`

The version of the PTZ API the instance implements, as a device's
`ptz_get_api_version` says it: one version covers both.
Any thread.
Access: read.

### `ptr ptz_get_api_info()`

The procs and signals the instance has, as a device's `ptz_get_api_info` gives
its own.
Any thread.
Access: read.

### `void ptz_discovery_start()`

Looks for devices for a while (a few seconds), saying `ptz_device_found` for
each one it finds, then `ptz_discovery_finished`.
While a search is running, it does nothing; after one, it looks again, and
says again each device it finds.
Any thread.

### `void ptz_discovery_stop()`

Gives up the search, and says `ptz_discovery_finished` if one was running.
Any thread.

## Per-discovery signal_handler

The signals on a discovery instance's own `signal_handler`, from
`obs_source_get_signal_handler()`.
A signal fires on the main thread, where the instance's driver does its
work, whichever thread the instance was made on.
A listener may call the instance's procs from inside a signal: a
`ptz_discovery_stop` from `ptz_device_found`, say, or a `ptz_discovery_start`
from `ptz_discovery_finished` to look again.
Each carries `source`, the instance's source, lent for the call.

### `void ptz_device_found(ptr source, string kind, string id, string name, string address, ptr settings)`

A device was found, once per search.
`kind` is the [filter kind](#driver-kinds) to make a device for it with.
`id` tells it from every other device found, and from itself found again or
found by another way of looking.
`name` and `address` are for a user to recognise it by.
`settings` is an `obs_data_t *`, lent for the call, holding `type` and only
what it takes to reach the device (`host`, and `port` or `udp_port`, say):
made a filter of `kind` with these settings, the kind's defaults fill in the
rest.

### `void ptz_discovery_finished(ptr source)`

The search is over: it ran its course, or `ptz_discovery_stop` was called.
