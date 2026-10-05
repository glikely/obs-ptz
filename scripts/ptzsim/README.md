# ptzsim -- unified PTZ camera simulator

A single simulated PTZ camera exposed through every wire protocol
obs-ptz speaks: VISCA (TCP, UDP/"VISCA-over-IP", and an emulated serial
port), Pelco-D/P (emulated serial), and ONVIF. Moving the camera through
any one protocol updates the same shared pan/tilt/zoom/focus state, so
you can point two different obs-ptz device entries at it (say, VISCA-TCP
and Pelco) and watch them agree. It also serves a
WebGL view of what the camera sees, for OBS to show in a Browser Source.

There is no Windows support: the emulated serial ports use Python's
`pty` module, which is POSIX-only. Everything else (VISCA TCP/UDP,
ONVIF, the camera view) has no OS-specific code, but hasn't been
exercised on Windows.

## Prerequisites

- Python 3.8+. No third-party packages are required for VISCA/ONVIF/
  Pelco control -- only the standard library is used.

### macOS

macOS may also prompt to allow incoming network connections the first
time `ptzsim` binds a listening socket (TCP/UDP for VISCA, HTTP for
ONVIF/the debug endpoint). Allow it -- these are the ports obs-ptz needs
to reach the simulator.

## WebGL camera view

ptzsim serves a WebGL page at `http://127.0.0.1:8080/` (`--web-port` changes
the port, `--no-web` turns it off; if 8080 is taken it carries on without
the page) showing a
labelled panorama (degree grid, lettered landmarks) seen through a virtual
camera that follows the simulated pan, tilt, zoom and focus, with the tally
lamp as a coloured border and a standby screen when powered off. Add it in
OBS as a **Browser Source** (e.g. 1280x720); `?hud=0` hides the text readout.
You can also move the camera from the view: right-click the Browser Source
in OBS and choose **Interact**, then drag to pan and tilt (the view follows
the pointer, so the camera moves the other way), scroll to zoom, and
double-click to send it home. Every protocol sees the new position.
It needs OBS's browser source (CEF), and it isn't a video stream, so it
doesn't exercise OBS's Media Source path.

### Real room backdrops

`--backdrop` shows a real room instead of the drawn grid. It takes a file
(an equirectangular `.hdr`, `.jpg` or `.png`), a URL, or the key of a
[Poly Haven](https://polyhaven.com/hdris) HDRI (CC0, so free to use and
share), which ptzsim downloads once to `~/.cache/ptzsim/backdrops` and
credits in its output. A downloaded one works offline from then on, and is
checked against its recorded checksum (delete it to fetch it again):

```
python3 scripts/ptzsim --backdrop chapel_day
```

Good indoor ones: `chapel_day`, `afrikaans_church_interior`, `ballroom`,
`cinema_hall`, `cyclorama_hard_light`, `climbing_gym`. `--backdrop-res`
picks the download's size (`1k` to `8k`, default `4k`, about 26 MB for a
`.hdr`), and `--backdrop-cache DIR` where it goes. The page tonemaps an HDR
itself; pan 0 is the middle of the picture and tilt 0 its horizon. The
degree lines are drawn over it, as on the grid; add `?grid=0` to the page's
URL to leave the room bare.

### 3D scenes

`--scene` puts the camera in a 3D scene instead, drawn with
[three.js](https://threejs.org/) (MIT), which ptzsim downloads on first use
to `~/.cache/ptzsim` along with the scene. It takes a glTF file (`.glb` or
`.gltf`), a URL of one, or the name of a model in Khronos's
[glTF sample assets](https://github.com/KhronosGroup/glTF-Sample-Assets):
`sponza`, or `khronos:<Name>`. Nothing is bundled, so check a scene's
licence before sharing it (Sponza's is the Cryengine Limited License).

```
python3 scripts/ptzsim --scene sponza
```

Pan, tilt and zoom turn and zoom the camera as in the panorama, and drags
work the same. The camera stands in the middle of the scene a third of the
way up unless `--camera-pos X,Y,Z` (in the scene's units) says otherwise:
point several simulators at one scene from different spots to make a
multi-camera set. The degree lines are drawn over it (`?grid=0` hides them),
and focus is real depth of field: focus 0 is sharp at 0.5 m and 1 at 100 m
(0.5 is 7 m), and what is nearer or farther blurs more the more you zoom
(`?dof=0` turns the blur off). It can't be combined with `--backdrop`. `--scene-cache DIR` moves the cache.

### Blender as the renderer (experimental)

`--blender` has Blender render the web view, so the camera has real optics:
pan and tilt turn a Blender camera, zoom sets its field of view, and focus
sets its focus distance for real depth of field. ptzsim starts Blender in
the background with `blender/client.py`, which follows the simulated camera
and posts rendered frames back; ptzsim streams them to the page as MJPEG, so
the Browser Source, dragging, the HUD, tally and standby all work as before.

```
python3 scripts/ptzsim --blender                      # a built-in test room: lettered pillars and a ruler
python3 scripts/ptzsim --blender room.blend --blender-camera Camera
```

A `.blend` can be any scene with a camera, so its camera objects are the
camera locations (run one simulator per camera, as with `--room`). Pan is a
turn about the vertical axis from the camera's own direction, so point it
along the way you want pan 0 to look. Blender is found as `$BLENDER`,
`--blender-exe`, the macOS app or `blender` on `$PATH`, and its output is in
`~/.cache/ptzsim/blender.log`.

On a 2026 Apple-silicon Mac the built-in room gives about 5 frames a second
in EEVEE at 1280x720 with 4 samples; `--blender-engine workbench` (flat, no
lighting) is about ten times faster, and `--blender-size`, `--blender-samples`
and `--blender-fps` trade quality for speed. A real room is slower: Blender's CC0 [Classroom demo](https://download.blender.org/demo/test/classroom.zip)
(300 meshes, 10 lights, made for Cycles) takes about 20 seconds to
compile its shaders on the first frame, then gives 2 to 3 frames a second
at 1280x720. EEVEE has no bounced light, so such a scene comes out dark:
`--blender-exposure` (stops) brightens it and `--blender-raytracing` turns
on EEVEE's ray tracing for bounced light, at little extra cost here.
For example, `--blender classroom/classroom.blend --blender-camera renderCam
--blender-exposure 2.5 --blender-raytracing`.

Workbench is the practical choice for a live picture: it draws a scene's textures
with studio lighting, so the Classroom comes out bright and clear at about 6
frames a second, with depth of field and no bounced-light tuning:
`--blender classroom/classroom.blend --blender-camera renderCam --blender-engine workbench`. The
degree lines aren't drawn over Blender's picture, and `--blender` can't be
combined with `--backdrop`, `--scene` or `--room`.

### Ready-made rooms

`--room NAME` is a backdrop or 3D scene with cameras standing in it, so you
don't have to find a good spot yourself; `--camera NAME` picks one of its
cameras (the first by default) and `--list-rooms` lists them:

```
python3 scripts/ptzsim --room sponza --camera east-end
```

| Room | Cameras |
|---|---|
| `sponza` (3D) | `west-end`, `east-end`, `north-gallery`, `south-gallery` |
| `hallway` (3D) | `mid`, `near-end`, `far-end` |
| `dungeon` (3D) | `hall`, `arches` |
| `chapel` | `pews`, `platform`, `windows` |
| `church` | `platform`, `aisle` |
| `gallery` | `hall`, `doorway` |

Run one simulator per camera, each on its own `--web-port` and protocol
ports, to get a multi-camera set in the same room. A panorama is one spot,
so its cameras are different ways of looking from it. `--heading DEGREES`
turns any picture or scene so that pan 0 looks that way; a camera's heading
is what a room sets. Licences are those of the sources: Poly Haven's are CC0,
Sponza's is the Cryengine Limited License, the hallway's (by yeeyeeman) is Creative
Commons Attribution, and the dungeon's (by Warkarma) isn't confirmed: both of those
come from the three.js examples, pinned to release r170, and are open-air or unlit, so
`exposure` brightens the dungeon. Check a licence before sharing a room's picture.

## Moving like a camera

By default an absolute move, preset recall or home jumps straight to its
destination. `--move-time SECONDS` makes them take time, as a camera's
motors do: SECONDS to cross the full pan range, with tilt, zoom and focus
at the same rate. Every protocol reads the in-transit position back while
it moves, and a drive command or stop cancels the move. VISCA ACKs such a
command at once and sends its completion when the camera gets there, on the
command socket (1 or 2) it ACKed on; a move that replaces one still under
way cancels it with a "command canceled" error. For the WebGL view,
try `--move-time 4`.

## Running it

From the repository root:

```
python3 scripts/ptzsim                       # everything, the camera view on port 8080
python3 scripts/ptzsim --no-web              # without the WebGL camera view
python3 scripts/ptzsim --no-onvif --no-pelco # VISCA only (all 3 transports)
python3 scripts/ptzsim --no-visca-tcp --no-visca-serial   # VISCA/UDP only
```

Every backend is on by default; disable what you don't need with
`--no-visca` (all VISCA transports), `--no-visca-tcp`/`--no-visca-udp`/
`--no-visca-serial` (one transport), `--no-onvif`, or `--no-pelco`.

On startup it prints what it's listening on, e.g.:

```
[visca-tcp] serving on ('127.0.0.1', 5678)
[visca-udp] serving on ('127.0.0.1', 52381)
[visca-serial] emulated serial port at /tmp/ptzsim-visca-serial
[onvif] HTTP listening on 0.0.0.0:8899
[onvif] advertise = http://127.0.0.1:8899/onvif/device_service
[pelco] emulated serial port at /tmp/ptzsim-pelco-serial (address 1, auto-detects Pelco-D/P)
```

### Pointing obs-ptz at it

Open OBS's PTZ dock -> Settings (gear icon) -> `+`, pick the source for
the camera, and a protocol under "New device" (then fill in the settings
below), or the simulator itself under "Detected devices":

| Protocol           | Host/Port field                              |
|--------------------|-----------------------------------------------|
| VISCA (over TCP)   | host = the machine running `ptzsim`, port 5678 |
| VISCA (over IP/UDP)| host = the machine running `ptzsim`, port 52381 |
| VISCA (serial)     | the printed `/tmp/ptzsim-visca-serial` path (or `--visca-serial-path`) |
| Pelco              | the printed `/tmp/ptzsim-pelco-serial` path (or `--pelco-serial-path`), device ID matching `--pelco-address` (default 1) |
| ONVIF (experimental) | with a network `--host`, detected as `obs-ptz-sim SIM-PTZ-1`, with its host and port; credentials aren't enforced, `admin`/`admin` works |
| VISCA (over IP/UDP), detected | with `--sony-discovery-name NAME`, detected as `NAME SIM-PTZ-1`, at host port 52381 (so leave `--visca-udp-port` at its default) |

The VISCA-serial and Pelco entries require `ENABLE_SERIALPORT=ON` at
build time (off by default -- see the top-level `CMakeLists.txt`); the
TCP/UDP/ONVIF entries work with a stock build.

Both emulated serial ports are just symlinks to a pty pair, so they're
stable across simulator restarts as long as you don't change
`--visca-serial-path`/`--pelco-serial-path`. If you restart `ptzsim`
while OBS still has the device open, close and reopen the device in OBS
afterwards -- the symlink target (the underlying pty) changes on every
run.

### Watching it work

Drag the pan/tilt joystick or recall a preset in OBS; `ptzsim`'s stdout
logs every command it receives, e.g.:

```
[+0.30+0.00, -0.10+0.00, 0.00+0.00, 0.50+0.00] --> 8101060118140201
```

A Browser Source of the web view shows the camera's view moving with
the commands (see above). `--debug-http-port PORT` serves the
same state as JSON on `GET /state` -- mainly useful for scripts/tests
rather than manual use.

## Full option reference

Run `python3 scripts/ptzsim --help` for the authoritative list; the
highlights:

- `--host HOST`: the one address everything listens on and says it is at:
  VISCA (TCP and UDP), ONVIF, the web view and `--debug-http-port`. The
  default, `127.0.0.1`, is this machine only. To be reached from another
  machine, such as a VM, give this machine's address, or `0.0.0.0` for all of
  them (ptzsim then advertises its address on the network). ONVIF's
  WS-Discovery and `--sony-discovery-name`, which are answered over the
  network, only work with such an address.
- `--visca-tcp-port`, `--visca-udp-port` (defaults 5678, 52381).
- `--visca-serial-path`, `--pelco-serial-path` (symlink paths, defaults
  under `/tmp`).
- `--pelco-address` (default 1; must match the device ID configured in
  OBS).
- `--onvif-http-port` (default 8899).
- `--visca-report FILE`: be the camera an obs-ptz camera report was made
  of, as far as the report goes (`backends/visca_report.py`): answer the
  inquiries it answered with its replies, or ptzsim's own where they are
  as long, so what is set shows when it is read back; refuse with the
  camera's error what it refused, and leave unanswered what it never
  answered. Reports sent in are kept in `tests/camera-reports/`.
- `--sony-discovery-name NAME`: answer Sony's VISCA-over-IP camera
  discovery (an `ENQ:network` broadcast to UDP port 52380) as a camera
  called NAME, at `--host`. Off by default, so that several simulators on
  one machine don't all answer.
- `--web-port PORT` (default 8080; `0` or `--no-web` turns it off): the
  WebGL camera view's port, and
  `--move-time SECONDS`: make moves take time.
- `--rtsp-port` (default 8554): only the port in the RTSP stream URI ONVIF
  advertises; nothing serves a stream there.
- `--debug-http-port PORT`: serves `GET /state` as JSON; used by the CI
  test framework in `tests/obs-integration/`, off by default.
