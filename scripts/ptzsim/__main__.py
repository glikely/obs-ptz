#!/usr/bin/env python3
"""
Unified PTZ camera simulator for testing obs-ptz.

One shared pan/tilt/zoom/focus model is exposed through pluggable
protocol backends -- VISCA (TCP, UDP/"VISCA-over-IP", and an emulated
serial port), ONVIF, and Pelco-D/P (emulated serial) -- and can
serve a WebGL view of the camera (--web-port, on by default) for an OBS
Browser Source. Moving the camera through any one protocol is reflected
in all the others and, if enabled, in the view.

Run modes
---------
    python3 scripts/ptzsim                       # everything, the WebGL camera view on port 8080
    python3 scripts/ptzsim --no-web              # without the camera view
    python3 scripts/ptzsim --no-onvif --no-pelco  # VISCA only (all 3 transports)
    python3 scripts/ptzsim --no-visca-tcp --no-visca-serial   # VISCA/UDP only
    python3 scripts/ptzsim --host 0.0.0.0 --onvif-http-port 8899   # reachable from other machines

The emulated serial ports (VISCA and Pelco) are pty pairs; point obs-ptz's
serial device field at the printed path, or at the fixed symlink
(--visca-serial-path / --pelco-serial-path) which stays stable across
restarts.

End-to-end test with OBS
------------------------
1. Run this script.
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
4. A Browser Source of http://127.0.0.1:8080/ shows
   what the camera sees; it can be dragged in OBS's Interact window.
5. Drag the pan/tilt joystick. The simulator's stdout logs every PTZ
   call and the view follows in real time. Stop, Home, and presets all work over ONVIF and
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
import ipaddress
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
from . import backdrop, rooms, scene as scene_module
from .webview import WebViewServer


DEFAULT_WEB_PORT = 8080


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
    ap.add_argument("--host", default="127.0.0.1",
                     help="the one address the simulator listens on and says it is at, for VISCA, "
                          "ONVIF, the web view and --debug-http-port alike (default 127.0.0.1: "
                          "this machine only; give this machine's address, or 0.0.0.0 for all of "
                          "them, to be reached from another, such as a VM; ONVIF and Sony "
                          "discovery only work then)")

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
    ap.add_argument("--web-port", type=int, default=None,
                     help="serve a WebGL camera view on this port, for an OBS Browser Source "
                          f"(default {DEFAULT_WEB_PORT}, and carry on without it if that is taken; "
                          "0 turns it off)")
    ap.add_argument("--no-web", action="store_true", help="don't serve the web view")
    ap.add_argument("--backdrop", default=None, metavar="FILE|URL|KEY",
                     help="show this equirectangular panorama as the room in the web view, "
                          "rather than the drawn grid: a file (.hdr, .jpg, .png), a URL, or "
                          "the key of a Poly Haven HDRI, such as chapel_day (CC0, "
                          "https://polyhaven.com/hdris), downloaded once to --backdrop-cache")
    ap.add_argument("--backdrop-res", default="4k", metavar="RES",
                     help="which size of a Poly Haven HDRI to use: 1k, 2k, 4k, 8k... (default 4k)")
    ap.add_argument("--backdrop-cache", default=None, metavar="DIR",
                     help="where backdrops are downloaded to (default ~/.cache/ptzsim/backdrops)")
    ap.add_argument("--scene", default=None, metavar="FILE|URL|NAME",
                     help="put the web view's camera in this 3D scene, drawn with three.js, "
                          "which is downloaded on first use: a glTF file (.glb/.gltf), a URL, or "
                          "\"sponza\" or \"khronos:<Name>\" for a Khronos sample model "
                          "(check its licence)")
    ap.add_argument("--camera-pos", default=None, metavar="X,Y,Z",
                     help="where the camera stands in the --scene, in its units (default: "
                          "the middle of it, a third of the way up)")
    ap.add_argument("--heading", type=float, default=0.0, metavar="DEGREES",
                     help="turn the web view's world this many degrees to the right, so that "
                          "pan 0 looks that way (default 0)")
    ap.add_argument("--room", default=None, metavar="NAME",
                     help="a ready-made room for the web view: a backdrop or 3D scene with "
                          "camera positions in it; --list-rooms says which")
    ap.add_argument("--camera", default=None, metavar="NAME",
                     help="which of the --room's cameras to be (default: its first)")
    ap.add_argument("--list-rooms", action="store_true",
                     help="list the --room names and their cameras, then exit")
    ap.add_argument("--scene-cache", default=None, metavar="DIR",
                     help="where scenes and three.js are downloaded to (default ~/.cache/ptzsim)")
    ap.add_argument("--debug-http-port", type=int, default=0,
                     help="serve GET /state as JSON on this port for test "
                          "harnesses (0 disables it, the default)")
    args = ap.parse_args()
    # Asked for by number, the web view is an error if it can't start; by default it is not
    args.web_asked = args.web_port is not None and not args.no_web
    if args.no_web:
        args.web_port = 0
    elif args.web_port is None:
        args.web_port = DEFAULT_WEB_PORT
    if args.list_rooms:
        print(rooms.describe())
        sys.exit(0)
    args.initial_view = None
    if args.room:
        if args.backdrop or args.scene:
            ap.error("--room sets the backdrop or scene itself: don't also give --backdrop or --scene")
        if not args.web_port:
            ap.error("--room is for the web view: drop --no-web or --web-port 0")
        try:
            room = rooms.pick(args.room, args.camera)
        except rooms.RoomError as e:
            ap.error(str(e))
        args.backdrop, args.scene = room.get("backdrop"), room.get("scene")
        if args.camera_pos is None and room.get("position"):
            args.camera_pos = ",".join(str(v) for v in room["position"])
        if not args.heading:
            args.heading = room.get("heading", 0.0)
        args.initial_view = room.get("view")
        print(f"[room] {room['title']}, camera '{room['camera']}'")
    elif args.camera:
        ap.error("--camera is for a --room")
    # Where the simulator says it is, for what it advertises: a wildcard
    # means this machine's address on the network
    args.advertise = primary_ipv4() if args.host in ("0.0.0.0", "") else args.host
    args.local_only = ipaddress.ip_address(args.advertise).is_loopback
    if args.sony_discovery_name and args.local_only:
        ap.error("--sony-discovery-name is answered on the network: give --host a LAN address, or 0.0.0.0")
    if (args.backdrop or args.scene) and not args.web_port:
        ap.error("--backdrop and --scene are for the web view: drop --no-web or --web-port 0")
    if args.backdrop and args.scene:
        ap.error("--backdrop and --scene both set what the camera sees: use one")
    if args.camera_pos:
        try:
            args.camera_pos = [float(v) for v in args.camera_pos.split(",")]
            assert len(args.camera_pos) == 3
        except (ValueError, AssertionError):
            ap.error("--camera-pos is X,Y,Z, three numbers")
    return args


def main():
    args = parse_args()

    state = PTZState()
    if args.move_time > 0:
        state.move_rate = 2.0 / args.move_time
    if args.start_in_standby:
        state.power = False
    if args.initial_view:
        # Where the camera starts, and goes back to for Home
        state.set_position(**args.initial_view)
        state.set_home()
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
        sony_setup = SonySetupBackend(args.advertise, args.sony_discovery_name)
        sony_setup.start()
        backends.append(sony_setup)

    if not args.no_onvif:
        onvif = OnvifBackend(state, args.advertise, args.onvif_http_port, args.rtsp_port,
                             bind=args.host, discovery=not args.local_only)
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
        scene = None
        if args.scene:
            try:
                directory, entry = scene_module.resolve(args.scene, args.scene_cache)
                scene = {"dir": directory, "entry": entry, "camera_pos": args.camera_pos,
                         "vendor": scene_module.ensure_three(args.scene_cache)}
            except backdrop.BackdropError as e:
                sys.exit(f"[scene] {e}")
        web = WebViewServer(state, args.host, args.web_port, picture, scene, args.heading,
                            advertise=args.advertise)
        try:
            web.start()
        except OSError as e:
            if args.web_asked or picture or scene:
                sys.exit(f"[web] can't listen on {args.host}:{args.web_port}: {e}")
            print(f"[web] not serving the camera view: {args.host}:{args.web_port} is taken ({e}); "
                  "give --web-port another, or --no-web")
            web = None

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
