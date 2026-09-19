# ptzsim -- unified PTZ camera simulator

A single simulated PTZ camera exposed through every wire protocol
obs-ptz speaks: VISCA (TCP, UDP/"VISCA-over-IP", and an emulated serial
port), Pelco-D/P (emulated serial), and ONVIF. Moving the camera through
any one protocol updates the same shared pan/tilt/zoom/focus state, so
you can point two different obs-ptz device entries at it (say, VISCA-TCP
and Pelco) and watch them agree. Add `--with-video` and it also serves a
live RTSP test pattern, overlaid with the current PTZ state, for OBS to
pull in as a source.

There is no Windows support: the emulated serial ports use Python's
`pty` module, which is POSIX-only. Everything else (VISCA TCP/UDP,
ONVIF, the video feed) has no OS-specific code, but hasn't been
exercised on Windows.

## Prerequisites

- Python 3.8+. No third-party packages are required for VISCA/ONVIF/
  Pelco control -- only the standard library is used.
- Optional, only for `--with-video`:
  - `ffmpeg` (generates the test pattern and pushes it out over RTSP).
  - [MediaMTX](https://github.com/bluenviron/mediamtx/releases/latest)
    (ffmpeg can't act as an RTSP server by itself; MediaMTX is a single
    static binary that accepts ffmpeg's push and re-serves it to OBS).

### Linux

```
sudo apt install ffmpeg          # only needed for --with-video
```

Download a MediaMTX release tarball for your architecture, extract it,
and either put the `mediamtx` binary on your `$PATH` or pass
`--mediamtx /path/to/mediamtx`.

### macOS

```
brew install ffmpeg              # only needed for --with-video
```

MediaMTX isn't in Homebrew; download the `darwin` release tarball from
the link above, extract it, and either put it on your `$PATH` or pass
`--mediamtx /path/to/mediamtx`. The first time you run it, Gatekeeper
will refuse to launch an unsigned binary downloaded from the internet --
either right-click it in Finder and choose "Open" once to approve it, or
run `xattr -d com.apple.quarantine /path/to/mediamtx`.

macOS may also prompt to allow incoming network connections the first
time `ptzsim` binds a listening socket (TCP/UDP for VISCA, HTTP for
ONVIF/the debug endpoint). Allow it -- these are the ports obs-ptz needs
to reach the simulator.

## Running it

From the repository root:

```
python3 scripts/ptzsim                       # everything, no video
python3 scripts/ptzsim --with-video          # also stream a test pattern
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

Open OBS's PTZ dock -> Settings (gear icon) -> `+` and pick a protocol:

| Protocol           | Host/Port field                              |
|--------------------|-----------------------------------------------|
| VISCA (over TCP)   | host = the machine running `ptzsim`, port 5678 |
| VISCA (over IP/UDP)| host = the machine running `ptzsim`, port 52381 |
| VISCA (serial)     | the printed `/tmp/ptzsim-visca-serial` path (or `--visca-serial-path`) |
| Pelco              | the printed `/tmp/ptzsim-pelco-serial` path (or `--pelco-serial-path`), device ID matching `--pelco-address` (default 1) |
| ONVIF (experimental) | should appear in OBS's discovery list as `obs-ptz-sim` / `SIM-PTZ-1`; credentials aren't enforced, `admin`/`admin` works |

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

With `--with-video`, OBS's Media Source (once you "Use Selected Camera"
on the ONVIF entry) shows a test pattern with a live-updating overlay of
the current pan/tilt/zoom/speed. `--debug-http-port PORT` serves the
same state as JSON on `GET /state` -- mainly useful for scripts/tests
rather than manual use.

## Full option reference

Run `python3 scripts/ptzsim --help` for the authoritative list; the
highlights:

- `--host HOST`: IP advertised for ONVIF discovery/stream URIs (default:
  auto-detected).
- `--visca-tcp-port`, `--visca-udp-port` (defaults 5678, 52381).
- `--visca-serial-path`, `--pelco-serial-path` (symlink paths, defaults
  under `/tmp`).
- `--pelco-address` (default 1; must match the device ID configured in
  OBS).
- `--onvif-http-port` (default 8899).
- `--rtsp-port` (default 8554), `--with-video`, `--mediamtx PATH`,
  `--state-file PATH` (the video overlay's backing text file).
- `--debug-http-port PORT`: serves `GET /state` as JSON; used by the CI
  test framework in `tests/obs-integration/`, off by default.
