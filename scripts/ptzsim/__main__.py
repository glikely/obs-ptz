#!/usr/bin/env python3
"""
Unified PTZ camera simulator for testing obs-ptz.

One shared pan/tilt/zoom/focus model is exposed through pluggable
protocol backends (VISCA-over-TCP, ONVIF), and can optionally be paired
with a live RTSP video feed so OBS has something to attach as a source.
Moving the camera through either protocol is reflected in the other and,
if enabled, in the video overlay.

Run modes
---------
    python3 scripts/ptzsim                       # VISCA + ONVIF, no video
    python3 scripts/ptzsim --with-video           # also stream a test pattern (needs ffmpeg + mediamtx in PATH)
    python3 scripts/ptzsim --no-onvif             # VISCA only
    python3 scripts/ptzsim --no-visca             # ONVIF only
    python3 scripts/ptzsim --host 0.0.0.0 --onvif-http-port 8899 --rtsp-port 8554

End-to-end test with OBS
------------------------
1. Run this script (with --with-video for video).
2. Launch OBS with obs-ptz installed.
3. Open the PTZ dock -> + -> pick VISCA (host, port 5678) or ONVIF
   (experimental; the emulator appears in the discovery list as
   obs-ptz-sim / SIM-PTZ-1, credentials aren't enforced -- admin/admin
   is fine).
4. Click "Use Selected Camera". With --with-video, the auto-created
   Media Source (ONVIF) plays the test pattern.
5. Drag the pan/tilt joystick. The simulator's stdout logs every PTZ
   call and, with --with-video, the RTSP overlay updates pan/tilt/zoom
   values in real time. Stop, Home, presets, and right-click "Save Home"
   all work over ONVIF; VISCA exercises the same shared position/speed
   state without presets.

Notes
-----
- ffmpeg can't act as an RTSP server, so --with-video needs MediaMTX
  (https://github.com/bluenviron/mediamtx). Download a release binary
  and put it in $PATH, or pass --mediamtx /path/to/it.
- Auth is intentionally not enforced for ONVIF -- the goal is exercising
  obs-ptz's discovery and command paths, not the WS-Security implementation.
- The overlay is written to /tmp/ptzsim-state.txt (or --state-file).
  ffmpeg's drawtext filter reloads it every few frames.

Single file tree, no third-party Python dependencies (and ffmpeg/mediamtx
only if --with-video is passed).
"""

import argparse
import asyncio
import signal
import socket
import threading
import time

from .backends.onvif import OnvifBackend
from .backends.visca import ViscaBackend
from .state import PTZState, run_ticker
from .video import VideoFeed


def primary_ipv4():
    """Best-effort guess of an IPv4 we'd reply on. Falls back to 127.0.0.1."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        addr = s.getsockname()[0]
        s.close()
        return addr
    except OSError:
        return "127.0.0.1"


def parse_args():
    ap = argparse.ArgumentParser(
        description="Unified PTZ camera simulator exposing VISCA and ONVIF "
                    "protocol backends, with an optional live RTSP video "
                    "feed for OBS.")
    ap.add_argument("--host", default=primary_ipv4(),
                     help="IP advertised for ONVIF discovery/stream URIs (default: auto)")
    ap.add_argument("--visca-port", type=int, default=5678,
                     help="VISCA-over-TCP listen port")
    ap.add_argument("--no-visca", action="store_true", help="disable the VISCA backend")
    ap.add_argument("--onvif-http-port", type=int, default=8899,
                     help="ONVIF SOAP/HTTP listen port")
    ap.add_argument("--no-onvif", action="store_true", help="disable the ONVIF backend")
    ap.add_argument("--rtsp-port", type=int, default=8554,
                     help="RTSP port advertised/used for the video feed")
    ap.add_argument("--with-video", action="store_true",
                     help="spawn ffmpeg + mediamtx and serve a live RTSP "
                          "test pattern that overlays PTZ state")
    ap.add_argument("--mediamtx", default=None,
                     help="explicit path to the mediamtx binary (default: search $PATH)")
    ap.add_argument("--state-file", default="/tmp/ptzsim-state.txt",
                     help="path the video overlay text is written to")
    return ap.parse_args()


def main():
    args = parse_args()
    if args.no_visca and args.no_onvif:
        raise SystemExit("at least one of VISCA or ONVIF must be enabled")

    state = PTZState()
    stop_event = threading.Event()
    threading.Thread(target=run_ticker, args=(state, stop_event), daemon=True).start()

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)

    backends = []
    if not args.no_visca:
        visca = ViscaBackend(state, port=args.visca_port)
        loop.run_until_complete(visca.start(loop))
        backends.append(visca)

    if not args.no_onvif:
        onvif = OnvifBackend(state, args.host, args.onvif_http_port, args.rtsp_port)
        onvif.start()
        backends.append(onvif)
        print(f"[sim] onvif uuid = {onvif.uuid}")

    video = None
    if args.with_video:
        video = VideoFeed(state, args.host, args.rtsp_port, args.state_file, args.mediamtx)
        video.start()

    print(f"[sim] state file = {args.state_file}")

    def shutdown(*_):
        print("[sim] shutting down")
        stop_event.set()
        if video:
            video.stop()
        for backend in backends:
            backend.stop()
        loop.stop()

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    if video:
        async def watch_video():
            while True:
                await asyncio.sleep(0.5)
                for _proc, code in video.poll():
                    print(f"[video] subprocess exited with code {code}")
        loop.create_task(watch_video())

    try:
        loop.run_forever()
    finally:
        loop.close()


if __name__ == "__main__":
    main()
