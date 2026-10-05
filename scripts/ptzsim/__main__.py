#!/usr/bin/env python3
"""
Unified PTZ camera simulator for testing obs-ptz.

One shared pan/tilt/zoom/focus model is exposed through pluggable
protocol backends -- VISCA (TCP, UDP/"VISCA-over-IP", and an emulated
serial port), ONVIF, and Pelco-D/P (emulated serial) -- and can
optionally serve a WebGL view of the camera (--web-port) for an OBS
Browser Source. Moving the camera through any one protocol is reflected
in all the others and, if enabled, in the view.

Run modes
---------
    python3 scripts/ptzsim                       # everything, no camera view
    python3 scripts/ptzsim --web-port 8080        # also serve the WebGL camera view
    python3 scripts/ptzsim --no-onvif --no-pelco  # VISCA only (all 3 transports)
    python3 scripts/ptzsim --no-visca-tcp --no-visca-serial   # VISCA/UDP only
    python3 scripts/ptzsim --host 0.0.0.0 --onvif-http-port 8899 --rtsp-port 8554

The emulated serial ports (VISCA and Pelco) are pty pairs; point obs-ptz's
serial device field at the printed path, or at the fixed symlink
(--visca-serial-path / --pelco-serial-path) which stays stable across
restarts.

End-to-end test with OBS
------------------------
1. Run this script (with --web-port for a picture).
2. Launch OBS with obs-ptz installed.
3. Open the PTZ dock -> + -> pick the camera's source, then a protocol
   under "New device", and set it up:
   - VISCA (TCP): host, port 5678
   - VISCA (UDP): host, port 52381
   - VISCA (Serial): the printed /tmp/ptzsim-visca-serial path
   - Pelco: the printed /tmp/ptzsim-pelco-serial path, device ID 1
   or pick the simulator under "Detected devices": ONVIF finds it as
   obs-ptz-sim SIM-PTZ-1 (credentials aren't enforced, admin/admin is
   fine), and with --sony-discovery-name NAME, Sony's VISCA-over-IP
   discovery finds it as NAME SIM-PTZ-1.
4. With --web-port, a Browser Source of http://127.0.0.1:<port>/ shows
   what the camera sees; it can be dragged in OBS's Interact window.
5. Drag the pan/tilt joystick. The simulator's stdout logs every PTZ
   call and, with --web-port, the view follows in real time. Stop, Home, and presets all work over ONVIF and
   Pelco; VISCA exercises the same shared position/speed state without
   presets.

Notes
-----
- Auth is intentionally not enforced for ONVIF -- the goal is exercising
  obs-ptz's discovery and command paths, not the WS-Security implementation.
- --rtsp-port only sets the port in the RTSP stream URI ONVIF advertises;
  nothing serves a stream there.

Single file tree, no third-party Python dependencies.
"""

import argparse
import asyncio
import signal
import socket
import sys
import threading

from .backends.onvif import OnvifBackend
from .backends.sony_setup import SonySetupBackend
from .backends.pelco import PelcoBackend
from .backends.visca import ViscaBackend, ViscaCameraLogic, SonyUdpQuirks
from .backends.visca_report import ViscaReportReplay
from .debug_http import DebugHttpServer
from .state import PTZState, run_ticker
from . import backdrop
from .webview import WebViewServer


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
        description="Unified PTZ camera simulator exposing VISCA (TCP/UDP/"
                    "serial), ONVIF and Pelco-D/P protocol backends, with an "
                    "optional WebGL camera view for OBS.")
    ap.add_argument("--host", default=primary_ipv4(),
                     help="IP advertised for ONVIF discovery/stream URIs (default: auto)")

    ap.add_argument("--no-visca", action="store_true",
                     help="disable the VISCA backend entirely (all transports)")
    ap.add_argument("--visca-tcp-port", type=int, default=5678,
                     help="VISCA-over-TCP listen port")
    ap.add_argument("--no-visca-tcp", action="store_true", help="disable VISCA-over-TCP")
    ap.add_argument("--visca-udp-port", type=int, default=52381,
                     help="VISCA-over-IP (UDP) listen port")
    ap.add_argument("--no-visca-udp", action="store_true", help="disable VISCA-over-IP")
    ap.add_argument("--visca-no-block-inquiries", action="store_true",
                     help="answer VISCA's \"7e 7e xx\" block inquiries with a syntax error, as "
                          "a BirdDog does: only the single-value inquiries work")
    ap.add_argument("--visca-no-version-inquiry", action="store_true",
                     help="answer VISCA's version inquiry with a syntax error, so the camera's "
                          "model can't be known")
    ap.add_argument("--visca-model", default="0001:0000",
                     help="the vendor and model ID the VISCA version inquiry answers with, as "
                          "hex VVVV:MMMM (default: 0001:0000, a Sony of no model the plugin has "
                          "a command set for, so it uses the generic one, which has everything "
                          "this does; a Sony SRG-120DH is 0001:0511, a BirdDog P100 0109:2020)")
    ap.add_argument("--visca-report", default=None, metavar="FILE",
                     help="be the camera an obs-ptz camera report (docs/visca-protocol.md) was made "
                          "of, as far as the report goes: answer what it answered as it did, and "
                          "refuse what it refused; its vendor and model ID are the ones the version "
                          "inquiry answers with, whatever --visca-model says")
    ap.add_argument("--start-in-standby", action="store_true",
                     help="start with the camera powered off")
    ap.add_argument("--visca-no-completions", action="store_true",
                     help="ACK VISCA commands but never send the completion, as a BirdDog "
                          "backpack does")
    ap.add_argument("--visca-no-green-tally", action="store_true",
                     help="answer VISCA's green tally lamp command with a syntax error, as a "
                          "BirdDog P100 does; only the red lamp works")
    ap.add_argument("--visca-udp-sony-quirks", action="store_true",
                     help="make VISCA-over-IP misbehave like a real Sony camera: drop requests "
                          "that come too soon after a reply, enforce strictly increasing "
                          "sequence numbers, answer slowly, and have only two command sockets "
                          "(see SonyUdpQuirks in backends/visca.py). Its counters are added to "
                          "the --debug-http-port /state output")
    ap.add_argument("--sony-discovery-name", default=None, metavar="NAME",
                     help="answer Sony's VISCA-over-IP camera discovery (an \"ENQ:network\" "
                          "broadcast to UDP port 52380) as a camera called NAME, at --host "
                          "(off by default, so that several simulators don't all answer)")
    ap.add_argument("--visca-serial-path", default="/tmp/ptzsim-visca-serial",
                     help="symlink path for the emulated VISCA serial port")
    ap.add_argument("--no-visca-serial", action="store_true", help="disable emulated VISCA serial")

    ap.add_argument("--no-onvif", action="store_true", help="disable the ONVIF backend")
    ap.add_argument("--onvif-http-port", type=int, default=8899,
                     help="ONVIF SOAP/HTTP listen port")

    ap.add_argument("--no-pelco", action="store_true", help="disable the Pelco-D/P backend")
    ap.add_argument("--pelco-serial-path", default="/tmp/ptzsim-pelco-serial",
                     help="symlink path for the emulated Pelco serial port")
    ap.add_argument("--pelco-address", type=int, default=1,
                     help="Pelco device address to respond to")

    ap.add_argument("--rtsp-port", type=int, default=8554,
                     help="RTSP port in the stream URI ONVIF advertises (nothing serves one)")
    ap.add_argument("--move-time", type=float, default=0.0, metavar="SECONDS",
                     help="make absolute moves, presets and home take time, as a camera's "
                          "motors do: SECONDS to cross the full pan range, the other axes at "
                          "the same rate (0, the default, makes them instant)")
    ap.add_argument("--web-port", type=int, default=0,
                     help="serve a WebGL camera view on this port, for an OBS Browser "
                          "Source (0 disables it, the default; try 8080)")
    ap.add_argument("--backdrop", default=None, metavar="FILE|URL|KEY",
                     help="show this equirectangular panorama as the room in the web view, "
                          "rather than the drawn grid: a file (.hdr, .jpg, .png), a URL, or "
                          "the key of a Poly Haven HDRI, such as chapel_day (CC0, "
                          "https://polyhaven.com/hdris), downloaded once to --backdrop-cache")
    ap.add_argument("--backdrop-res", default="4k", metavar="RES",
                     help="which size of a Poly Haven HDRI to use: 1k, 2k, 4k, 8k... (default 4k)")
    ap.add_argument("--backdrop-cache", default=None, metavar="DIR",
                     help="where backdrops are downloaded to (default ~/.cache/ptzsim/backdrops)")
    ap.add_argument("--debug-http-port", type=int, default=0,
                     help="serve GET /state as JSON on this port for test "
                          "harnesses (0 disables it, the default)")
    args = ap.parse_args()
    if args.backdrop and not args.web_port:
        ap.error("--backdrop is for the web view: add --web-port")
    return args


def main():
    args = parse_args()

    state = PTZState()
    if args.move_time > 0:
        state.move_rate = 2.0 / args.move_time
    if args.start_in_standby:
        state.power = False
    stop_event = threading.Event()
    threading.Thread(target=run_ticker, args=(state, stop_event), daemon=True).start()

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)

    ViscaCameraLogic.block_inquiries = not args.visca_no_block_inquiries
    ViscaCameraLogic.version_inquiry = not args.visca_no_version_inquiry
    vendor_id, model_id = args.visca_model.split(":")
    ViscaCameraLogic.vendor_id = int(vendor_id, 16)
    ViscaCameraLogic.model_id = int(model_id, 16)
    ViscaCameraLogic.green_tally = not args.visca_no_green_tally
    ViscaCameraLogic.completions = not args.visca_no_completions
    if args.visca_report:
        report = ViscaReportReplay.load(args.visca_report)
        ViscaCameraLogic.report = report
        if report.vendor_id is not None:
            ViscaCameraLogic.vendor_id = report.vendor_id
            ViscaCameraLogic.model_id = report.model_id

    backends = []
    if not args.no_visca:
        tcp_port = 0 if args.no_visca_tcp else args.visca_tcp_port
        udp_port = 0 if args.no_visca_udp else args.visca_udp_port
        serial_path = None if args.no_visca_serial else args.visca_serial_path
        if tcp_port or udp_port or serial_path:
            quirks = SonyUdpQuirks() if args.visca_udp_sony_quirks else None
            state.visca_udp_stats = quirks.stats if quirks else None
            visca = ViscaBackend(state, args.host, tcp_port, udp_port, serial_path, quirks)
            loop.run_until_complete(visca.start(loop))
            backends.append(visca)
        else:
            print("[sim] VISCA enabled but all its transports are disabled; skipping")

    if args.sony_discovery_name:
        sony_setup = SonySetupBackend(args.host, args.sony_discovery_name)
        sony_setup.start()
        backends.append(sony_setup)

    if not args.no_onvif:
        onvif = OnvifBackend(state, args.host, args.onvif_http_port, args.rtsp_port)
        onvif.start()
        backends.append(onvif)
        print(f"[sim] onvif uuid = {onvif.uuid}")

    if not args.no_pelco:
        pelco = PelcoBackend(state, args.pelco_serial_path, args.pelco_address)
        pelco.start(loop)
        backends.append(pelco)

    if not backends:
        print("[sim] warning: no backends are enabled, the camera can't be controlled")

    debug_http = None
    if args.debug_http_port:
        debug_http = DebugHttpServer(state, args.host, args.debug_http_port)
        debug_http.start()

    web = None
    if args.web_port:
        picture = None
        if args.backdrop:
            try:
                picture = backdrop.resolve(args.backdrop, args.backdrop_res, args.backdrop_cache)
            except backdrop.BackdropError as e:
                sys.exit(f"[backdrop] {e}")
        web = WebViewServer(state, args.host if args.host.startswith("127.") else "0.0.0.0", args.web_port)
        web.start()

    def shutdown(*_):
        print("[sim] shutting down")
        stop_event.set()
        if debug_http:
            debug_http.stop()
        if web:
            web.stop()
        for backend in backends:
            backend.stop()
        loop.call_soon_threadsafe(loop.stop)

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    try:
        loop.run_forever()
    finally:
        loop.close()


if __name__ == "__main__":
    main()
