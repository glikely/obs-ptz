# PTZ Controls for Stream Deck

A sample Stream Deck plugin that moves the cameras of OBS's PTZ Controls and
recalls their presets, over obs-websocket: a sample of the PTZ API's WebSocket
binding (see `docs/ptz-device-api.md`), not a finished product.

It has six actions:

- **Move**: moves a camera for as long as the key is held, in one of eight
  directions, zooming in or out, or focusing near or far, at a speed set on the
  key. The key's picture shows the direction.
- **Autofocus**: turns the camera's autofocus on and off, and lights up green
  while it is on.
- **Preset**: recalls one of a camera's presets. Its title is the preset's name,
  and it lights up green while the camera is at the preset. A preset that has a
  thumbnail shows it as the key's picture, with a green frame while the camera is
  there, fetched from OBS with `ptz_preset_get_thumbnail` (API 0.5), so it works
  with OBS on another machine. Held for a second, the key saves the
  camera's position into the preset instead.
- **Command**: stops the camera, sends it home, saves its home position (held
  for a second), focuses or sets its white balance once, or turns its power on
  and off, lit while it is on. It sends `ptz_stop`, `ptz_home_recall`,
  `ptz_home_save` and the `focus_onetouch` and `wb_onepush` triggers; power goes
  through `ptz_request_state` with `power_on`, which needs the `power` feature
  (VISCA has it) though the API's description of that proc names only
  `focus_af_enabled`.
- **Camera status**: shows the camera's pan, tilt, zoom and focus as its title, in
  degrees, a zoom multiplier and a focus distance where the camera says what its
  positions are in (the `axes` of its state), and as the position, from -1.0 or 0.0
  to 1.0, where it doesn't, and goes
  red when the camera's link is down or OBS can't be reached.
- **Axis**: a dial for a Stream Deck +, for pan, tilt, zoom or focus. Turning it moves the
  camera, by a step you set for each tick (a tenth of it for pan and tilt, and more the faster the dial is turned), and the
  LCD shows where the axis is. A tilt dial turned clockwise tilts down. A pan is the camera from above, with the range it
  can pan round it and a wedge of where it looks; a tilt is the same from the
  side. A zoom or focus is a marker on a track, with a focus near on the left and
  infinity on the right, and a clockwise turn goes toward infinity. The value shown is in the same units as the status key.
  The wedge narrows as the camera zooms in. The degrees of the pictures are
  nominal (pan 120 to either side, tilt 45, a view of 70 degrees wide to 7 at
  tele), only there to draw the range. Pushing a focus dial turns the camera's autofocus on or off. While
  autofocus is on the focus dial is dimmed, and turning it shows a red forbidden
  sign for a third of a second and moves nothing. A camera
  with the `pantilt_abs`, `zoom_abs` or `focus_abs` feature is sent positions; one without is
  moved at a speed while the dial turns.

Each is for the camera selected in the PTZ dock, unless it is set to one
camera. A Preset key on the selected camera is set to a position in the dock's
list, as the dock's "Recall preset N" hotkeys are, since which presets there are
changes with the camera: its title and light follow whichever camera is
selected.

## Building and installing

It needs Node.js 20 or later to build, and Stream Deck 7.1 or later to run.

```bash
npm install
npm run build
npx streamdeck dev                                   # once: turns on Stream Deck's developer mode
npx streamdeck link ca.secretlab.obs-ptz.sdPlugin    # once: installs it, as a link to this folder
npx streamdeck restart ca.secretlab.obs-ptz          # after each build
```

`npm run watch` rebuilds and restarts it whenever a source file changes.

In OBS, turn on obs-websocket's server (Tools > WebSocket Server Settings), and
leave PTZ Controls' `websocket_access` at `control`. Then put a key on the Stream
Deck, and give it OBS's host, port and password, which every key shares, and
choose its camera, and its direction or its preset.

## How it uses the API

`src/obs-ptz.ts` holds one connection to obs-websocket for as long as the plugin
runs, subscribed to vendors' events, and makes it again whenever it drops: the
Stream Deck software usually starts before OBS does. Everything goes through
obs-websocket's `CallVendorRequest`, to the `obs-ptz` vendor.

- The property inspectors' camera menus have "Selected camera", and each camera
  from `ptz_list_devices`, by the source it is on. A key keeps the camera's
  `device` UUID, which is the same from one run of OBS to the next, or
  `selected`. The preset menu comes from `ptz_preset_get_list`, and keeps the
  preset's `id`, which is too, or for the selected camera its position.
- A key on the selected camera sends its requests with `device` set to
  `selected`, and the binding sends them on to the camera selected as they come.
  The answer says which camera that was, and what has to go to the same camera
  goes there by its UUID: the release of a Move key, which the selection may
  have moved on from while it was held, and the recall after a Preset key has
  read the selected camera's list.
- What is selected, for the keys' titles and lights, is read with
  `ptz_get_selected` as the plugin connects, and followed with the
  `ptz_selected_changed` event.
- A Move key calls `ptz_move` with its speeds as it goes down, and `ptz_move`
  with the same axes at 0 as it comes up. It doesn't call `ptz_stop`, which
  would also stop a zoom that another key, held at the same time, had started.
- A Preset key calls `ptz_preset_recall`. It lights up on the
  `ptz_preset_recalled` event, which comes when the camera has arrived. It goes
  dark when `ptz_state_changed` reports the camera more than a little way from
  where it arrived, whatever moved it: one of these keys, the PTZ dock, a
  joystick or the camera's own remote.
- Preset keys' titles follow `ptz_preset_changed`, `ptz_preset_added`,
  `ptz_preset_removed`, `ptz_preset_order_changed` and `ptz_preset_list_reset`.

## What it doesn't do

- If the connection to OBS drops while a Move key is held, there is no
  connection to send the stop on: the camera goes on moving until it is
  stopped some other way.
- A camera without the `move_completion` feature is taken to have arrived a few
  seconds after a recall, and if it was still moving then, its key may light up
  and go dark again.
- No dials on a Stream Deck +, and no focus, home or other actions.
- Stream Deck keeps the obs-websocket password in its own settings for the
  plugin, which are not encrypted.
