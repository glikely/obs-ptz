# VISCA Implementation details

This document describes How the VISCA protocol is implemented in the plugin.
There are several different variants of the VISCA protocol and supported by many different camera vendors.
Not all cameras behave according to the original SONY spec.

## VISCA Basic Protocol

All variants of the VISCA protocol use the same basic protocol of the
controller sends a command or inquiry datagram, and the device responds with an
acknowledgements, a reply, or an error.

ccc := Controller Address; typically 0
ddd := Device Address; range 1-7
sss := slot id

### Framing

All datagrams in the VISCA protocol start with an address byte, are 3-12 bytes
long, and finish with the value 0xff.
A raw value of 0xFF cannot appear anywhere in the datagram because it is used as the datagram delimiter.
Data values are encoded such that 0xFF never appears as a data byte in the datagram.

The first byte of a datagram is the address field that encodes the sender and receiver of the datagram.

### Commands

Commands tell the device to do something or to set a property.
The controller sends the command datagram to the device,
and the device replies with an Ack datagram followed by a Complete datagram.
If the command could not be executed then the device responds with an error datagram.

Command: 0b1ccc0ddd 0b00000001 [command; 1-3 bytes] [data; 0-12 bytes] 0xff
Ack:     0b1ddd0ccc 0b01000sss 0xff
Complete:0b1ddd0ccc 0b01010sss 0xff
Error:   0b1ddd0ccc 0b01100000 0xff

### Inquiries

Inquiries ask the device to return data about the current state of the device.
The controller sends an inquiry datagram to the device,
and the device replies with a Complete datagram.
If the inquiry was invalid, or the device otherwise cannot complete the request,
then an error datagram is returned instead.

Command: 0b1ccc0ddd 0b00001001 [property; 1-3 bytes] 0xff
Complete:0b1ddd0ccc 0b01010000 [data; 0-12 bytes] 0xff
Error:   0b1ddd0ccc 0b01100000 0xff

### VISCA over Serial

This is the original version of the VISCA protocol.
VISCA over serial will work over an RS232 point-to-point connection,
An RS232 daisy chain with up to 7 devices,
or an RS422 serial bus that also supports up to 7 devices.
Serial connections can be wired up as either bidirectional (the device sends response packets)
or single ended (controller can send commands, but replys will not be received).
When in a single ended configuration the controller is able to control the camera,
but it will not be able to get any status messages from the device or enquire about its state.

### VISCA over UDP (Sony VISCA-over-IP Protocol)

The Sony implementation of VISCA over IP encapusates VISCA datagrams in UDP datagrams with some additional
encoding to track order of datagram delivery.

### VISCA over TCP (PTZOptics and others)

The VISCA over TCP protocol encapsulates the serial protocol in a TCP socket.
Since TCP is a reliable transport that delivers data in-order,
this version of the protocol doesn't need to track sequence numbers.
Instead, it treats the TCP socket as a UART port and uses exactly the same framing to decode datagrams.

The controller establishes a TCP connection with the device in the normal way.
Once the TCP socket is established is uses the UART protocol init sequence to initialize the device.

### Datavideo DVIP

Datavideo's cameras have VISCA over TCP on port 5002, with each packet, both ways,
after its length in 2 bytes, big endian, the length's own 2 bytes included:
`00 07 81 09 04 47 ff` is the zoom position inquiry.
That is how Bitfocus' Datavideo VISCA Companion module sends them; it hasn't been tried with a camera here.

## Command Sets

Cameras don't all have the same commands and inquiries, and not every camera
that has one means the same by it.
What the plugin sends a camera comes from its command set (`ViscaProfile`, in `src/ptz-visca-commands.cpp`),
which has:

- **Controls**: a state value (`wb_mode`, `focus_af_enabled`, ...),
  the command that sets it, or a command for each value it can be set to,
  and the inquiries that read it, best first.
  When a camera answers an inquiry with a syntax error,
  the next one is used for everything it read.
  A block inquiry reads many values at once, so it comes first where it has one.
- **Actions**: what the driver moves the camera and uses its presets with
  (`pantilt_drive`, `pantilt_abs`, `pantilt_rel`, `pantilt_home`, `zoom_drive`, `zoom_abs`,
  `focus_drive`, `focus_onetouch`, `zoom_focus_abs`, `memory_reset`, `memory_set`, `memory_recall`).
- **Triggers**: commands sent as they are by the `ptz_trigger` proc (`wb_onepush`).

The "Command Set" setting, in the advanced settings, chooses it.
On "Automatic", the default, the plugin asks the camera what it is with the version inquiry,
before anything else, and uses the command set for that vendor and model ID,
or the generic one, which has everything and finds out what a camera doesn't have
from the syntax errors it answers with.
The generic command set is built into the plugin's code.
The others shipped with it are JSON files in `src/visca-profiles/`, in the same format as a user's (below),
linked into the plugin as Qt resources when it is built: a camera is added by adding a file there.

### A user's command sets

A command set can be added in a JSON file in the `visca-profiles` directory of the plugin's config
(`plugin_config/obs-ptz/visca-profiles/` in OBS's config directory).
They are read when OBS starts, and the log says what was read from each file, or why it couldn't be.
A key the reader doesn't know, a `remove` of something the command set it extends doesn't have,
or a `remove_inquiries` of an inquiry it doesn't read with, is a reason it can't be:
misspelt, any of them would otherwise do nothing, and nothing would say so.
A command set extends another, the generic one unless it says, and changes some of what that one has.
For example (the auto tracking commands are made up):

```json
{
  "id": "my-camera",
  "name": "My Camera",
  "extends": "generic",
  "models": ["0001:0513"],
  "remove": ["low_latency"],
  "controls": [
    {
      "key": "user_auto_tracking",
      "set": {"cmd": "81010a1100ff", "args": [{"type": "flag", "offset": 4}]},
      "reads": [{"cmd": "81090a11ff", "results": [{"type": "flag", "offset": 2}]}]
    },
    {
      "key": "wb_mode",
      "reads": [{"cmd": "81090435ff", "results": [{"type": "u4", "offset": 2}]}]
    }
  ],
  "actions": {
    "pantilt_home": "81010604ff"
  },
  "triggers": {
    "user_ir_reset": "8101060505ff"
  }
}
```

- `id`: lower case letters, numbers and dashes. What the setting has for it,
  and what another command set's `extends` names.
  A user's command set with the `id` of one shipped with the plugin, but the generic one, is used instead of it.
- `name`: what the setting shows. The `id` if there isn't one.
- `models`: the cameras "Automatic" chooses it for, by vendor and model ID in hex.
  A user's command set is chosen over a built-in one for the same model.
- `remove`: controls, actions and triggers it doesn't have.
- `remove_inquiries`: inquiries the camera doesn't have, which no control reads with then.
  A camera without the block inquiries has `["81097e7e00ff", "81097e7e01ff", ...]`.
- `controls`: each replaces what the command set it extends does for its `key`,
  with whichever of `set`, `set_to` and `reads` it has.
  `set_to` is a command for each value: `{"1": "8101043802ff", "0": "8101043803ff"}`.
  `"set": null` is for a value the camera can only be asked for, not set.
  A key the command set it extends doesn't have must start with `user_`,
  so it is never one the plugin has; it is in the device's state like any other.
- `actions`: commands for the driver, which must take as many arguments as the generic one's.
- `triggers`: a name the command set it extends doesn't have must start with `user_`.

A command is its bytes in hex, to camera 1 (they're readdressed for a serial bus),
or an object with them as `cmd` and:

- `args` (or `results`, for an inquiry): the fields the value is in, each a `type`,
  the `offset` of its first byte, the address byte being 0, in the command (or in the inquiry's reply),
  and a `key` if it isn't the control's. The types are:
  - `u4` and `u7`: a value in the low 4 or 7 bits of one byte;
  - `u8`, `u16` and `s16`: one in the low 4 bits of each of 2 or 4 bytes, the VISCA way;
  - `u15`: one in the low 7 bits of each of 2 bytes;
  - `s4` and `s7`: a speed and direction, as the zoom and pan/tilt drive commands have them;
  - `flag`: 2 for on, 3 for off;
  - `int` and `bool`: the bits a `mask` says, across as many bytes as it has
    (and `"signed": true` for a signed `int`).
- `affects`: the keys to read again once the camera has done it, the control's if it doesn't say.
- `assumes`: the state the camera is in once it has taken the command, for one that can't be read back:
  `{"user_lamp": true}`.

## Camera Reports

There are far more cameras than anyone working on the plugin has,
and the way to know what one has is to ask it.
**Create Camera Report…**, under Diagnostics in a camera's state in **Tools → PTZ Devices**,
makes a report of what the camera has, for its user to look over and send in.
The plugin never sends it anywhere: the user saves it, copies it,
or presses **Open Issue Page**, which copies it and opens the plugin's issue form for camera reports
(`.github/ISSUE_TEMPLATE/camera-report.yml`) in their browser, for them to paste it into.
GitHub is the only place reports are sent in.

For anyone who can't run the plugin, or whose camera it can't talk to,
`scripts/ptz-probe/ptz-probe.py` makes the same report without OBS: one Python file, which needs nothing else,
and talks to the camera and nothing else (see its README).

### What making one does

The device's `camera_report` trigger asks the camera, one at a time, for everything
the generic command set and the camera's own can ask for.
Then it sends each value the camera said it has, and can be set, back to it as the camera said it was,
which changes nothing. It sends no moves, and doesn't send back the video format, the video output
or low latency, which some cameras restart their video for even when they are set to what they were,
nor the camera's ID.
A camera in standby isn't asked anything, since some won't wake after being asked for what they can't be then.
The state's `camera_report` says how far it has got (`running`, `done` and `total`),
or why there is no report (`"error": "standby"`),
and the device's `ptz_get_camera_report` proc hands back the last report made, as JSON.

### What is in one

- `report`, `format`: `"obs-ptz camera report"`, and 1, this format's version.
- `made_by`: `ptz-probe`, in a report it made; not in one the plugin made.
- `plugin_version`, `os` (`Linux`, `macOS` or `Windows`), and `type`, the device's (`visca-over-tcp`, say).
- `protocol`: `visca`.
- `camera`: the `vendor_id`, `model_id` and `rom_version` the camera says it has, in hex,
  and the `vendor_name` and `model_name` the plugin knows for them, if it does.
- `command_set`: the id of the one the camera was using.
- `inquiries`: each one asked, and its `reply`, in hex, or the `error`
  (`syntax error`, `not executable`, `no reply`, ...).
  A reply with the camera's ID in it has it as zeros, and says so in `masked`.
- `commands`: each value sent back, by its `key`, the `command` sent, and the `result`:
  `completed`, `ack` for one the camera never said it had done, or the error.
- `buffer_full`: how often the camera said it was too busy to take a request.
- `draft_command_set`: a command set for the camera, which its user can try by saving it
  in their command sets (above) and restarting OBS: the generic one, for the camera's model,
  without the inquiries it didn't answer, the commands it said it doesn't have,
  and the values it can neither read nor set.
  What wasn't tried, such as the moves, is as the generic one has it.
  It has an `error` if the plugin couldn't read it as a command set.

Nothing else is ever in one: not the camera's address, port or ID,
nor the names of its device, sources or scenes, nor anything from the log.
A report is made field by field, never by copying the state or the log,
so a value added to the state later can't end up in it, and `tests/obs-integration/test_camera_report.py`
checks that a report has none of the simulator's address, port, camera ID or device name.

### From a report to a command set

A report's `draft_command_set` is where a command set for the camera starts:
in `src/visca-profiles/`, with an `id` and `name` for the camera,
its `source` saying which issue its report came from and which firmware it was tried with,
and anything the issue says about what works and what doesn't.
A command set there for the camera's model is chosen for it automatically.

The report itself goes in `tests/camera-reports/`, as it was sent in.
ptzsim replays it (`--visca-report`) as the camera it was made of,
answering what it answered as it did and refusing what it refused,
and `tests/obs-integration/test_camera_report_replay.py` checks that the command set
the plugin chooses for each camera there asks it for nothing it doesn't have.
So every camera that has been reported stays tested, by people who don't have it.

A command set shipped with the plugin that it can't read is left out, with only a warning in the log,
so CI checks each one in `src/visca-profiles/` before it can get that far.
`tests/visca-profile-check` (`ENABLE_VISCA_PROFILE_CHECK`, on in the Ubuntu CI builds) reads them
with the plugin's own reader, as it reads the ones shipped with it, on every build,
and fails it if one can't be read, isn't in a file named for its id, doesn't have a `source`,
or is for a camera another one is for.
Its `bad/` command sets are wrong on purpose: each must be found to be, so the checks are checked too.
`scripts/check-visca-profiles.py`, with the formatting checks, fails one that isn't laid out
as the generator lays them out (`--fix` lays it out).
