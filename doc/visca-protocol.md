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

| Command set | Chosen for | From |
|---|---|---|
| Sony SRG-120DH, SRG-300H, SRG-300SE, SRG-360SHE, BRC-X400, SRG-X400, BRC-X1000, SRG-X40UH, SRG-A40, ILME-FR7, BRC-AM7 | their model IDs | Sony's protocol tables, as Bitfocus' Sony VISCA Companion module has them |
| PTZOptics Gen-2 NDI (PT12X/PT20X/PT30X-NDI) | by hand: the version reply doesn't say | grafton-visca's consolidated VISCA reference |
| Axis | any Axis camera | grafton-visca's consolidated VISCA reference |
| BirdDog | any BirdDog camera | BirdDog's own block inquiries, as Bitfocus' BirdDog PTZ Companion module reads them |
| BirdDog P100 | its model ID | measured on one |
| Datavideo PTC series | by hand: its IDs aren't known | Datavideo's tally command, as Bitfocus' Datavideo VISCA Companion module sends it |

Most are generated from published command tables by `scripts/visca-profile-gen`,
and none but the BirdDog P100's has been tried on a camera: each says where it came from in its `source`.
A camera report from one that has been tried is what makes it more than that.

### A user's command sets

A command set can be added in a JSON file in the `visca-profiles` directory of the plugin's config
(`plugin_config/obs-ptz/visca-profiles/` in OBS's config directory).
They are read when OBS starts, and the log says what was read from each file, or why it couldn't be.
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
- `source`: where what it says came from, for whoever looks at it. The plugin doesn't read it.
- `models`: the cameras "Automatic" chooses it for, by vendor and model ID in hex,
  or `VVVV:*` for any of a vendor's, for one whose model IDs vary. A command set for the model
  is chosen over one for any of the vendor's.
  A user's command set is chosen over a built-in one for the same model.
- `ranges`: how far the camera goes, by `pan`, `tilt`, `zoom` and `focus`, each the camera's position at
  one end (left, down, wide, far) and the other, as numbers or hex in strings: `"pan": ["-0x2200", "0x2200"]`.
  A camera whose pan positions run right to left has them the other way round.
  They are what the movement controls' ends stand for, unless the device's settings say otherwise.
- `standby_reads`: the keys the camera can be asked for while it is in standby, for one that can't be
  asked for everything then: a BirdDog asked for anything but whether it is on won't wake, so it has
  `["power_on"]`. The rest is read once it is on.
- `remove`: controls, actions and triggers it doesn't have.
- `remove_inquiries`: inquiries the camera doesn't have, which no control reads with then.
  A camera without the block inquiries has `["81097e7e00ff", "81097e7e01ff", ...]`.
- `controls`: each replaces what the command set it extends does for its `key`,
  with whichever of `set`, `set_to` and `reads` it has.
  `set_to` is a command for each value: `{"1": "8101043802ff", "0": "8101043803ff"}`.
  `"set": null` is for a value the camera can only be asked for, not set.
  In a user's command set, a key the command set it extends doesn't have must start with `user_`,
  so it is never one the plugin has; it is in the device's state like any other.
  One shipped with the plugin can have new keys by any name, for what the state view shows for
  only some cameras, like the PTZOptics `flicker_mode`.
- `actions`: commands for the driver, which must take as many arguments as the generic one's.
- `triggers`: in a user's command set, a name the command set it extends doesn't have must start with `user_`.

A command is its bytes in hex, to camera 1 (they're readdressed for a serial bus),
or an object with them as `cmd` and:

- `args` (or `results`, for an inquiry): the fields the value is in, each a `type`,
  the `offset` of its first byte, the address byte being 0, in the command (or in the inquiry's reply),
  and a `key` if it isn't the control's. The types are:
  - `u4` and `u7`: a value in the low 4 or 7 bits of one byte;
  - `u8`, `u16` and `s16`: one in the low 4 bits of each of 2 or 4 bytes, the VISCA way;
  - `u15`: one in the low 7 bits of each of 2 bytes;
  - `s20`: a signed one in the low 4 bits of each of 5 bytes, a position some cameras have;
  - `none`: in no byte at all, for an argument the driver gives an action that the camera's command doesn't have;
  - `s4` and `s7`: a speed and direction, as the zoom and pan/tilt drive commands have them;
  - `flag`: 2 for on, 3 for off;
  - `int` and `bool`: the bits a `mask` says, across as many bytes as it has
    (and `"signed": true` for a signed `int`).
- `affects`: the keys to read again once the camera has done it, the control's if it doesn't say.
- `assumes`: the state the camera is in once it has taken the command, for one that can't be read back:
  `{"user_lamp": true}`.
