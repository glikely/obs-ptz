# visca-probe

Throwaway tools used to investigate how real cameras behave over
VISCA-over-IP (UDP), which led to the fixes in `src/ptz-visca.cpp` and
`src/ptz-visca-udp.cpp`. Python 3 standard library only. Run them from this
directory, with a camera's IP address as the argument, e.g.
`python3 gap.py 192.168.16.11`.

To find out what a camera has, for a command set for it, use
`scripts/ptz-probe` instead, which makes the plugin's camera report.

They bind local UDP port 52381, the same as the plugin (a Sony sends its
replies to that port whatever port a request came from), so **quit OBS
first**.

## Probing a camera

`vp.py` is the small VISCA-over-IP client the others share.

| Script | Shows |
| --- | --- |
| `id.py IP...` | reset, then version / power / pan-tilt inquiries |
| `inqscan.py IP` | which inquiries the camera answers and which it rejects with a syntax error (`60 02`) |
| `tallyscan.py IP` | which tally lamp commands and inquiries the camera answers. **Lights lamps** (and turns them off again) |
| `lat.py IP...` | inquiry latency, min / median / p90 / max |
| `gap.py IP [N]` | how many requests are silently dropped, by the gap between a reply and the next request |
| `seq.py IP...` | how sequence numbers are treated: duplicates, backwards, gaps, wraparound, first after a reset |
| `rst.py IP` | whether a request sent right after a reset datagram is answered |
| `cmd.py IP...` | ACK / completion timing of commands, and what overlapping ones do |

`cmd.py` **moves the camera** a little (pan/tilt, zoom, home). It starts
from and returns to pan 0, tilt 0 but assumes the camera is there to begin
with.

## Modelling the driver

`model.py` is a line-for-line model of `PTZVisca` and `ViscaUDPTransport`
as they were before the fixes (50 ms retry timer, a new sequence number per
retry, the per-slot sequence table), driven against a real camera.
`fixed.py` subclasses it with the fixes (stop the timer on any reply, 15 ms
gap after a reply, 250 ms timeout, accept replies to recent sequence
numbers).

- `run_model1.py IP [seconds] [-v]`: connect and run the startup inquiries
- `run_model2.py IP`: an idle queue, then a command (zoom, home)
- `run_model3.py IP`: a burst of commands, then joystick-style traffic
- `cmp.py IP`: the current model against the fixed one, side by side

The model has not been kept in step with the driver since the fixes; the
regression tests are in `tests/obs-integration/test_visca_udp_sony.py`,
against `ptzsim --visca-udp-sony-quirks`.
