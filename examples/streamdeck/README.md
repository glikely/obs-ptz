# PTZ Controls for Stream Deck

A sample Stream Deck plugin that moves the cameras of OBS's PTZ Controls and
recalls their presets, over obs-websocket: a sample of the PTZ API's WebSocket
binding (see `docs/ptz-device-api.md`), not a finished product.

It has two actions:

- **Move**: moves a camera for as long as the key is held, in one of eight
  directions or zooming in or out, at a speed set on the key.
- **Preset**: recalls one of a camera's presets. Its title is the preset's name,
  and it lights up green while the camera is at the preset.

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

- The property inspectors' camera menus come from `ptz_list_devices`, showing
  each camera by the source it is on, and keep its `device` UUID, which is the
  same from one run of OBS to the next. The preset menu comes from
  `ptz_preset_get_list`, and keeps the preset's `id`, which is too.
- A Move key calls `ptz_move` with its speeds as it goes down, and `ptz_move`
  with the same axes at 0 as it comes up. It doesn't call `ptz_stop`, which
  would also stop a zoom that another key, held at the same time, had started.
- A Preset key calls `ptz_preset_recall`. It lights up on the
  `ptz_preset_recalled` event, which comes when the camera has arrived. It goes
  dark when `ptz_state_changed` reports the camera more than a little way from
  where it arrived, whatever moved it: one of these keys, the PTZ dock, a
  joystick or the camera's own remote.
- Preset keys' titles follow `ptz_preset_changed`, `ptz_preset_added`,
  `ptz_preset_removed` and `ptz_preset_list_reset`.

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
