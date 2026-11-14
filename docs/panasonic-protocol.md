# Panasonic HTTP CGI Protocol

This document describes the Panasonic AW-HE/HN/UE HTTP CGI protocol subset implemented by `obs-ptz` (`src/drivers/panasonic/`) and simulated by `ptzsim` (`scripts/ptzsim/backends/panasonic.py`).

## Transport & Endpoints

Commands and status queries are plain HTTP `GET` requests to `http://<host>:<port>` (default port `80`):

| Endpoint | Purpose |
| --- | --- |
| `/cgi-bin/aw_ptz?cmd=%23<CMD>&res=1` | PTZ motor, preset, focus mode, and power commands |
| `/cgi-bin/aw_cam?cmd=<CMD>&res=1` | Camera-side commands (e.g. one-push autofocus) |
| `/live/camdata.html` | Periodic status readout (`\r\n`-delimited lines) |

**URL encoding note:** Commands on `/cgi-bin/aw_ptz` begin with `#`, which **must** be URL-encoded as `%23` (e.g. `cmd=%23PTS5050`). Commands on `/cgi-bin/aw_cam` do not use `#`.

## PTZ Commands (`/cgi-bin/aw_ptz`)

### Continuous Velocity Control

Speeds are 2-digit zero-padded decimal integers in `01`–`99`, where `50` is **stop**:

- `obs-ptz` maps normalized speed `v` in `[-1.0, 1.0]` as:
  `50 + clamp(round(v * 49.0), -49, 49)`

| Command | Meaning | Range |
| --- | --- | --- |
| `#PTS[pp][tt]` | Pan & Tilt speed | `01` = fastest left/down, `49` = slowest left/down, `50` = stop, `51` = slowest right/up, `99` = fastest right/up |
| `#Z[zz]` | Zoom speed | `01` = fastest wide (out), `49` = slowest wide, `50` = stop, `51` = slowest tele (in), `99` = fastest tele |
| `#F[ff]` | Focus speed | `01` = fastest near, `49` = slowest near, `50` = stop, `51` = slowest far, `99` = fastest far |

To avoid flooding the camera's embedded HTTP server during rapid joystick updates, `PTZPanasonic` keeps at most one in-flight velocity request per axis group (`pt`, `z`, `f`), coalesces intermediate updates, and bypasses the in-flight gate for stop commands (`#PTS5050`, `#Z50`, `#F50`) so stops are always sent immediately.

### Absolute Position & Home

| Command | Meaning | Encoding |
| --- | --- | --- |
| `#APC[pppp][tttt]` | Absolute Pan/Tilt | 4-digit uppercase hex `0000`–`FFFF`; `8000` is center (`norm = (raw - 0x8000) / 0x7FFF`) |
| `#APC80008000` | Pan/Tilt Home | Center (`0x8000`, `0x8000`) |
| `#AXZ[zzz]` | Absolute Zoom | 3-digit uppercase hex `555` (wide, `0.0`) to `FFF` (tele, `1.0`) |
| `#AXF[fff]` | Absolute Focus | 3-digit uppercase hex `555` (near, `0.0`) to `FFF` (far, `1.0`) |

### Presets

Presets are 2-digit zero-padded indices `00`–`99` (100 slots):

| Command | Meaning |
| --- | --- |
| `#M[nn]` | Save current position to preset `nn` |
| `#R[nn]` | Recall preset `nn` |
| `#C[nn]` | Clear/delete preset `nn` |

### Focus Mode & Power

| Command | Meaning |
| --- | --- |
| `#D11` | Auto Focus ON |
| `#D10` | Manual Focus |
| `#O1` | Power ON |
| `#O0` | Power Standby |

## Camera Commands (`/cgi-bin/aw_cam`)

| Command | Meaning |
| --- | --- |
| `OSE:69:1` | One-Push Auto Focus |

## Status Polling (`/live/camdata.html`)

`PTZPanasonic` polls `/live/camdata.html` every `1500 ms` at rest and `500 ms` while moving (plus follow-up polls at `400 ms` and `1500 ms` after discrete commands). The response body consists of `\r\n`-delimited lines; `obs-ptz` parses:

| Line | State Field | Format |
| --- | --- | --- |
| `aPC[pppp][tttt]` | `pan`, `tilt` | 4-digit hex each (`8000` = `0.0`, mapped to `[-1.0, 1.0]`) |
| `gz[zzz]` | `zoom` | 3-digit hex (`555` = `0.0`, `FFF` = `1.0`) |
| `gf[fff]` | `focus` | 3-digit hex (`555` = `0.0`, `FFF` = `1.0`) |
| `d11` / `d10` | `focus_af_enabled` | `d11` = `true`, `d10` = `false` |
| `p1` / `p0` | `power_on` | `p1` = `true`, `p0` = `false` |

## Connection Tracking

- Any successful HTTP reply marks the device connected.
- 3 consecutive HTTP errors mark the device disconnected.

