#!/usr/bin/env python3
"""
Minimal ONVIF camera emulator for testing obs-ptz.

Implements just enough of the spec to be discoverable by WS-Discovery,
return a few capabilities, hand out a real RTSP URI, and respond to PTZ
commands by mutating an internal pan/tilt/zoom state. The state is written
to a text file (default /tmp/onvif-sim-state.txt) that an ffmpeg subprocess
can overlay onto a generated test pattern via the drawtext filter, so the
RTSP stream visibly responds to PTZ moves sent from OBS.

Run modes
---------
    python3 scripts/onvifemu.py                 # SOAP only, no RTSP
    python3 scripts/onvifemu.py --with-stream   # also stream a test pattern (needs ffmpeg + mediamtx in PATH)
    python3 scripts/onvifemu.py --host 0.0.0.0 --http-port 8899 --rtsp-port 8554
    python3 scripts/onvifemu.py --with-stream --mediamtx /path/to/mediamtx

End-to-end test with OBS
------------------------
1. Run this script (with --with-stream for video).
2. Launch OBS with obs-ptz installed.
3. Open the PTZ dock -> + -> ONVIF (experimental). The emulator should appear
   in the discovery list (obs-ptz-sim / SIM-PTZ-1). Credentials are not
   enforced; admin/admin is fine.
4. Click "Use Selected Camera". With --with-stream, the auto-created Media
   Source plays the test pattern.
5. Drag the pan/tilt joystick. The emulator's stdout logs every PTZ call
   and the RTSP overlay updates pan/tilt/zoom values in real time. Stop,
   Home, presets, and right-click "Save Home" all work.

Notes
-----
- ffmpeg can't act as an RTSP server, so --with-stream needs MediaMTX
  (https://github.com/bluenviron/mediamtx). Download a release binary
  and put it in $PATH, or pass --mediamtx /path/to/it.
- Auth is intentionally not enforced — the goal is exercising obs-ptz's
  discovery and command paths, not the WS-Security implementation.
- State is written to /tmp/onvif-sim-state.txt (or --state-file).
  ffmpeg's drawtext filter reloads it every few frames.

Single file, no third-party Python dependencies (and ffmpeg/mediamtx
only if --with-stream is passed).
"""

import argparse
import os
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from xml.etree import ElementTree as ET

# ---------------------------------------------------------------------------
# Namespaces (the only ones we actually emit)
# ---------------------------------------------------------------------------
NS = {
    "s": "http://www.w3.org/2003/05/soap-envelope",
    "a_disco": "http://schemas.xmlsoap.org/ws/2004/08/addressing",
    "a_norm": "http://www.w3.org/2005/08/addressing",
    "d": "http://schemas.xmlsoap.org/ws/2005/04/discovery",
    "dn": "http://www.onvif.org/ver10/network/wsdl",
    "tds": "http://www.onvif.org/ver10/device/wsdl",
    "trt": "http://www.onvif.org/ver10/media/wsdl",
    "tptz": "http://www.onvif.org/ver20/ptz/wsdl",
    "tt": "http://www.onvif.org/ver10/schema",
    "wsse": "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd",
}

WSD_GROUP = "239.255.255.250"
WSD_PORT = 3702


# ---------------------------------------------------------------------------
# Utilities
# ---------------------------------------------------------------------------
def clamp(v, lo, hi):
    return max(lo, min(hi, v))


def now_iso():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


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


# ---------------------------------------------------------------------------
# Simulator state
# ---------------------------------------------------------------------------
class State:
    def __init__(self, host, http_port, rtsp_port, state_file):
        self.host = host
        self.http_port = http_port
        self.rtsp_port = rtsp_port
        self.state_file = state_file
        self.uuid = "urn:uuid:" + str(uuid.uuid4())
        # PTZ in normalized [-1,1] for pan/tilt, [0,1] for zoom
        self.pan = 0.0
        self.tilt = 0.0
        self.zoom = 0.0
        # Active speeds for ContinuousMove (in units of (range) per second)
        self.pan_speed = 0.0
        self.tilt_speed = 0.0
        self.zoom_speed = 0.0
        # preset_token -> (name, pan, tilt, zoom)
        self.presets = {}
        self.next_preset_id = 1
        # Home position. Set via SetHomePosition; GotoHomePosition moves here.
        self.home_pan = 0.0
        self.home_tilt = 0.0
        self.home_zoom = 0.0
        self.lock = threading.Lock()
        self.running = True

    # --- mutations -------------------------------------------------------

    def continuous_move(self, x, y, z):
        with self.lock:
            self.pan_speed = float(x)
            self.tilt_speed = float(y)
            self.zoom_speed = float(z)

    def stop_motion(self, pantilt=True, zoom=True):
        with self.lock:
            if pantilt:
                self.pan_speed = 0.0
                self.tilt_speed = 0.0
            if zoom:
                self.zoom_speed = 0.0

    def absolute_move(self, x, y, z):
        with self.lock:
            self.pan = clamp(float(x), -1.0, 1.0)
            self.tilt = clamp(float(y), -1.0, 1.0)
            self.zoom = clamp(float(z), 0.0, 1.0)

    def relative_move(self, x, y, z):
        with self.lock:
            self.pan = clamp(self.pan + float(x), -1.0, 1.0)
            self.tilt = clamp(self.tilt + float(y), -1.0, 1.0)
            self.zoom = clamp(self.zoom + float(z), 0.0, 1.0)

    def home(self):
        with self.lock:
            self.pan = self.home_pan
            self.tilt = self.home_tilt
            self.zoom = self.home_zoom
        self.stop_motion()

    def set_home(self):
        with self.lock:
            self.home_pan = self.pan
            self.home_tilt = self.tilt
            self.home_zoom = self.zoom

    def set_preset(self, token, name):
        with self.lock:
            if not token:
                token = f"P{self.next_preset_id}"
                self.next_preset_id += 1
            self.presets[token] = (name or token, self.pan, self.tilt, self.zoom)
            return token

    def goto_preset(self, token):
        with self.lock:
            if token in self.presets:
                _, p, t, z = self.presets[token]
                self.pan, self.tilt, self.zoom = p, t, z
                return True
        return False

    def remove_preset(self, token):
        with self.lock:
            self.presets.pop(token, None)

    # --- tick / overlay --------------------------------------------------

    def step(self, dt):
        with self.lock:
            # Scale speed so a max speed of 1.0 takes 5s to traverse full
            # range. Tweak to taste.
            k = 0.2
            self.pan = clamp(self.pan + self.pan_speed * dt * k, -1.0, 1.0)
            self.tilt = clamp(self.tilt + self.tilt_speed * dt * k, -1.0, 1.0)
            self.zoom = clamp(self.zoom + self.zoom_speed * dt * k, 0.0, 1.0)

    def status_text(self):
        with self.lock:
            moving = abs(self.pan_speed) + abs(self.tilt_speed) + abs(self.zoom_speed) > 0.001
            move_tag = "MOVING" if moving else "idle"
            lines = [
                "ONVIF Simulator",
                f"Pan  {self.pan:+.2f}   Tilt {self.tilt:+.2f}   Zoom {self.zoom:+.2f}",
                f"Speed p={self.pan_speed:+.2f} t={self.tilt_speed:+.2f} z={self.zoom_speed:+.2f}  [{move_tag}]",
                f"Presets: {len(self.presets)}",
            ]
            return "\n".join(lines)

    def write_state_file(self):
        try:
            tmp = self.state_file + ".tmp"
            with open(tmp, "w") as f:
                f.write(self.status_text())
            os.replace(tmp, self.state_file)
        except OSError as e:
            print(f"[state] write failed: {e}", file=sys.stderr)

    def status_loop(self):
        last = time.time()
        while self.running:
            time.sleep(0.1)
            now = time.time()
            dt = now - last
            last = now
            self.step(dt)
            self.write_state_file()


# ---------------------------------------------------------------------------
# WS-Discovery responder
# ---------------------------------------------------------------------------
def ws_discovery_loop(state):
    """Listens on the WS-Discovery multicast group and replies to Probes."""
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        except (AttributeError, OSError):
            pass
        sock.bind(("0.0.0.0", WSD_PORT))
        mreq = struct.pack("4sl", socket.inet_aton(WSD_GROUP), socket.INADDR_ANY)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
        sock.settimeout(0.5)
    except OSError as e:
        print(f"[wsd] bind failed ({e}); will not respond to discovery probes", file=sys.stderr)
        return

    print(f"[wsd] listening on udp/{WSD_PORT}")
    while state.running:
        try:
            data, addr = sock.recvfrom(65536)
        except socket.timeout:
            continue
        except OSError:
            break

        text = data.decode("utf-8", errors="replace")
        if "Probe" not in text:
            continue
        msg_id = extract_message_id(data) or "uuid:?"
        reply = build_probe_match(state, msg_id)
        try:
            sock.sendto(reply.encode("utf-8"), addr)
            print(f"[wsd] replied to {addr}")
        except OSError as e:
            print(f"[wsd] send failed: {e}", file=sys.stderr)


def extract_message_id(data: bytes) -> str:
    try:
        root = ET.fromstring(data)
    except ET.ParseError:
        return ""
    for el in root.iter():
        if el.tag.endswith("}MessageID"):
            return (el.text or "").strip()
    return ""


def build_probe_match(state, relates_to):
    xaddr = f"http://{state.host}:{state.http_port}/onvif/device_service"
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<s:Envelope xmlns:s="{NS['s']}" xmlns:a="{NS['a_disco']}" xmlns:d="{NS['d']}" xmlns:dn="{NS['dn']}">
 <s:Header>
  <a:Action s:mustUnderstand="1">http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</a:Action>
  <a:MessageID>uuid:{uuid.uuid4()}</a:MessageID>
  <a:RelatesTo>{relates_to}</a:RelatesTo>
  <a:To s:mustUnderstand="1">http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</a:To>
 </s:Header>
 <s:Body>
  <d:ProbeMatches>
   <d:ProbeMatch>
    <a:EndpointReference><a:Address>{state.uuid}</a:Address></a:EndpointReference>
    <d:Types>dn:NetworkVideoTransmitter tds:Device</d:Types>
    <d:Scopes>
      onvif://www.onvif.org/type/Network_Video_Transmitter
      onvif://www.onvif.org/Profile/Streaming
      onvif://www.onvif.org/Profile/T
      onvif://www.onvif.org/hardware/SIM-PTZ-1
      onvif://www.onvif.org/name/obs-ptz-sim
      onvif://www.onvif.org/location/local-lab
    </d:Scopes>
    <d:XAddrs>{xaddr}</d:XAddrs>
    <d:MetadataVersion>1</d:MetadataVersion>
   </d:ProbeMatch>
  </d:ProbeMatches>
 </s:Body>
</s:Envelope>"""


# ---------------------------------------------------------------------------
# SOAP HTTP handler
# ---------------------------------------------------------------------------
def soap_envelope(body_xml: str) -> str:
    return (
        '<?xml version="1.0" encoding="UTF-8"?>'
        f'<s:Envelope xmlns:s="{NS["s"]}" xmlns:tds="{NS["tds"]}" '
        f'xmlns:trt="{NS["trt"]}" xmlns:tptz="{NS["tptz"]}" '
        f'xmlns:tt="{NS["tt"]}">'
        f"<s:Body>{body_xml}</s:Body>"
        "</s:Envelope>"
    )


def soap_fault(reason: str) -> str:
    return soap_envelope(
        f'<s:Fault xmlns:s="{NS["s"]}">'
        f"<s:Code><s:Value>s:Receiver</s:Value></s:Code>"
        f"<s:Reason><s:Text>{reason}</s:Text></s:Reason>"
        f"</s:Fault>"
    )


class OnvifHandler(BaseHTTPRequestHandler):
    state: State = None  # injected

    def log_message(self, fmt, *args):
        # quieter default access log
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        action, op = parse_soap_action(body)
        print(f"[soap] {self.path} -> {op}")
        response = self.dispatch(op, body)
        if response is None:
            response = soap_fault(f"Unsupported operation: {op}")
            code = 500
        else:
            code = 200
        self.send_response(code)
        self.send_header("Content-Type", "application/soap+xml; charset=utf-8")
        self.send_header("Content-Length", str(len(response.encode("utf-8"))))
        self.end_headers()
        self.wfile.write(response.encode("utf-8"))

    def dispatch(self, op, body):
        s = self.state
        # ---- Device service ----
        if op == "GetCapabilities":
            base = f"http://{s.host}:{s.http_port}"
            return soap_envelope(
                "<tds:GetCapabilitiesResponse><tds:Capabilities>"
                f"<tt:Device><tt:XAddr>{base}/onvif/device_service</tt:XAddr></tt:Device>"
                f"<tt:Media><tt:XAddr>{base}/onvif/media_service</tt:XAddr></tt:Media>"
                f"<tt:PTZ><tt:XAddr>{base}/onvif/ptz_service</tt:XAddr></tt:PTZ>"
                f"<tt:Imaging><tt:XAddr>{base}/onvif/imaging_service</tt:XAddr></tt:Imaging>"
                "</tds:Capabilities></tds:GetCapabilitiesResponse>"
            )
        if op == "GetDeviceInformation":
            return soap_envelope(
                "<tds:GetDeviceInformationResponse>"
                "<tds:Manufacturer>obs-ptz-sim</tds:Manufacturer>"
                "<tds:Model>SIM-PTZ-1</tds:Model>"
                "<tds:FirmwareVersion>0.0.1</tds:FirmwareVersion>"
                f"<tds:SerialNumber>{s.uuid[-12:]}</tds:SerialNumber>"
                "<tds:HardwareId>SIM</tds:HardwareId>"
                "</tds:GetDeviceInformationResponse>"
            )
        if op == "GetSystemDateAndTime":
            now = datetime.now(timezone.utc)
            return soap_envelope(
                "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime>"
                "<tt:DateTimeType>NTP</tt:DateTimeType>"
                "<tt:DaylightSavings>false</tt:DaylightSavings>"
                "<tt:TimeZone><tt:TZ>UTC0</tt:TZ></tt:TimeZone>"
                f"<tt:UTCDateTime><tt:Time><tt:Hour>{now.hour}</tt:Hour>"
                f"<tt:Minute>{now.minute}</tt:Minute><tt:Second>{now.second}</tt:Second></tt:Time>"
                f"<tt:Date><tt:Year>{now.year}</tt:Year><tt:Month>{now.month}</tt:Month>"
                f"<tt:Day>{now.day}</tt:Day></tt:Date></tt:UTCDateTime>"
                "</tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>"
            )
        # ---- Media service ----
        if op == "GetProfiles":
            return soap_envelope(
                '<trt:GetProfilesResponse>'
                '<trt:Profiles fixed="true" token="MainProfile">'
                "<tt:Name>MainProfile</tt:Name>"
                '<tt:VideoSourceConfiguration token="VSC0">'
                "<tt:SourceToken>VS0</tt:SourceToken>"
                "</tt:VideoSourceConfiguration>"
                "</trt:Profiles>"
                '<trt:Profiles fixed="true" token="SubProfile">'
                "<tt:Name>SubProfile</tt:Name>"
                '<tt:VideoSourceConfiguration token="VSC0">'
                "<tt:SourceToken>VS0</tt:SourceToken>"
                "</tt:VideoSourceConfiguration>"
                "</trt:Profiles>"
                "</trt:GetProfilesResponse>"
            )
        if op == "GetStreamUri":
            # Both profiles share the single MediaMTX path that ffmpeg
            # pushes to. Differentiating profiles would require running
            # a second ffmpeg, which isn't worth the complexity for a sim.
            uri = f"rtsp://{s.host}:{s.rtsp_port}/stream"
            return soap_envelope(
                "<trt:GetStreamUriResponse><trt:MediaUri>"
                f"<tt:Uri>{uri}</tt:Uri>"
                "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
                "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
                "<tt:Timeout>PT60S</tt:Timeout>"
                "</trt:MediaUri></trt:GetStreamUriResponse>"
            )
        if op == "GetSnapshotUri":
            uri = f"http://{s.host}:{s.http_port}/snapshot.jpg"
            return soap_envelope(
                "<trt:GetSnapshotUriResponse><trt:MediaUri>"
                f"<tt:Uri>{uri}</tt:Uri>"
                "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
                "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
                "<tt:Timeout>PT60S</tt:Timeout>"
                "</trt:MediaUri></trt:GetSnapshotUriResponse>"
            )
        # ---- PTZ service ----
        if op == "ContinuousMove":
            x, y, z = read_xyz(body, "Velocity")
            s.continuous_move(x, y, z)
            return soap_envelope("<tptz:ContinuousMoveResponse/>")
        if op == "AbsoluteMove":
            x, y, z = read_xyz(body, "Position")
            s.absolute_move(x, y, z)
            return soap_envelope("<tptz:AbsoluteMoveResponse/>")
        if op == "RelativeMove":
            x, y, z = read_xyz(body, "Translation")
            s.relative_move(x, y, z)
            return soap_envelope("<tptz:RelativeMoveResponse/>")
        if op == "Stop":
            # PTZ Stop has PanTilt/Zoom flags; Imaging Stop has VideoSourceToken.
            if first_text(body, "VideoSourceToken"):
                print("[focus] stop")
                return soap_envelope(
                    "<timg:StopResponse xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\"/>"
                )
            pantilt = (first_text(body, "PanTilt") or "true").lower() == "true"
            zoom = (first_text(body, "Zoom") or "true").lower() == "true"
            s.stop_motion(pantilt=pantilt, zoom=zoom)
            return soap_envelope("<tptz:StopResponse/>")
        if op == "GotoHomePosition":
            s.home()
            return soap_envelope("<tptz:GotoHomePositionResponse/>")
        if op == "SetHomePosition":
            s.set_home()
            print(f"[home] set to pan={s.home_pan:+.2f} tilt={s.home_tilt:+.2f} zoom={s.home_zoom:+.2f}")
            return soap_envelope("<tptz:SetHomePositionResponse/>")
        if op == "GetStatus":
            return soap_envelope(
                "<tptz:GetStatusResponse><tptz:PTZStatus>"
                "<tt:Position>"
                f'<tt:PanTilt x="{s.pan:.4f}" y="{s.tilt:.4f}"/>'
                f'<tt:Zoom x="{s.zoom:.4f}"/>'
                "</tt:Position>"
                "<tt:MoveStatus>"
                f"<tt:PanTilt>{'MOVING' if (s.pan_speed or s.tilt_speed) else 'IDLE'}</tt:PanTilt>"
                f"<tt:Zoom>{'MOVING' if s.zoom_speed else 'IDLE'}</tt:Zoom>"
                "</tt:MoveStatus>"
                f"<tt:UtcTime>{now_iso()}</tt:UtcTime>"
                "</tptz:PTZStatus></tptz:GetStatusResponse>"
            )
        if op == "GetPresets":
            entries = []
            for token, (name, p, t, z) in s.presets.items():
                entries.append(
                    f'<tptz:Preset token="{token}"><tt:Name>{name}</tt:Name>'
                    f'<tt:PTZPosition><tt:PanTilt x="{p:.4f}" y="{t:.4f}"/>'
                    f'<tt:Zoom x="{z:.4f}"/></tt:PTZPosition></tptz:Preset>'
                )
            return soap_envelope(
                "<tptz:GetPresetsResponse>" + "".join(entries) + "</tptz:GetPresetsResponse>"
            )
        if op == "SetPreset":
            token = first_text(body, "PresetToken") or ""
            name = first_text(body, "PresetName") or ""
            new_token = s.set_preset(token, name)
            return soap_envelope(
                f"<tptz:SetPresetResponse><tptz:PresetToken>{new_token}</tptz:PresetToken>"
                "</tptz:SetPresetResponse>"
            )
        if op == "GotoPreset":
            token = first_text(body, "PresetToken") or ""
            s.goto_preset(token)
            return soap_envelope("<tptz:GotoPresetResponse/>")
        if op == "RemovePreset":
            token = first_text(body, "PresetToken") or ""
            s.remove_preset(token)
            return soap_envelope("<tptz:RemovePresetResponse/>")
        # ---- Imaging service ----
        if op == "Move":
            speed_el = None
            try:
                root = ET.fromstring(body)
                for el in root.iter():
                    if el.tag.endswith("}Continuous"):
                        speed_el = el
                        break
            except ET.ParseError:
                pass
            focus_speed = 0.0
            if speed_el is not None:
                for child in speed_el.iter():
                    if child.tag.endswith("}Speed"):
                        try:
                            focus_speed = float((child.text or "0").strip())
                        except ValueError:
                            pass
                        break
            print(f"[focus] continuous speed={focus_speed:+.3f}")
            return soap_envelope("<timg:MoveResponse xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\"/>")
        if op == "SetImagingSettings":
            wb = ""
            af = ""
            try:
                root = ET.fromstring(body)
                for el in root.iter():
                    if el.tag.endswith("}WhiteBalance"):
                        for c in el.iter():
                            if c.tag.endswith("}Mode"):
                                wb = (c.text or "").strip()
                    if el.tag.endswith("}AutoFocusMode"):
                        af = (el.text or "").strip()
            except ET.ParseError:
                pass
            if wb:
                print(f"[imaging] WhiteBalance.Mode = {wb}")
            if af:
                print(f"[imaging] Focus.AutoFocusMode = {af}")
            return soap_envelope(
                "<timg:SetImagingSettingsResponse xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\"/>"
            )
        return None


def parse_soap_action(body: bytes):
    """Returns (action_uri, operation_local_name) from a SOAP request."""
    action = ""
    op = ""
    try:
        root = ET.fromstring(body)
    except ET.ParseError:
        return action, op
    for el in root.iter():
        if el.tag.endswith("}Action"):
            action = (el.text or "").strip()
            break
    # fall back to the first element inside <Body>
    for el in root.iter():
        if el.tag.endswith("}Body"):
            for child in el:
                op = child.tag.split("}")[-1]
                break
            break
    if not op and action:
        op = action.rsplit("/", 1)[-1]
    return action, op


def first_text(body: bytes, local_name: str) -> str:
    try:
        root = ET.fromstring(body)
    except ET.ParseError:
        return ""
    for el in root.iter():
        if el.tag.endswith("}" + local_name) or el.tag == local_name:
            return (el.text or "").strip()
    return ""


def read_xyz(body: bytes, container: str):
    """Extract PanTilt x/y and Zoom x from a <container> element."""
    px = py = pz = 0.0
    try:
        root = ET.fromstring(body)
    except ET.ParseError:
        return px, py, pz
    for el in root.iter():
        if el.tag.endswith("}" + container):
            for child in el.iter():
                if child.tag.endswith("}PanTilt"):
                    px = float(child.attrib.get("x", "0") or 0)
                    py = float(child.attrib.get("y", "0") or 0)
                elif child.tag.endswith("}Zoom"):
                    pz = float(child.attrib.get("x", "0") or 0)
            break
    return px, py, pz


# ---------------------------------------------------------------------------
# Optional MediaMTX + ffmpeg RTSP stream
#
# ffmpeg can't act as an RTSP server, so we use MediaMTX (single Go binary)
# to accept ffmpeg's RTSP push and re-serve it to clients. If MediaMTX
# isn't on PATH or alongside this script, we print install instructions
# and skip the video. The SOAP/discovery half of the simulator still works.
# ---------------------------------------------------------------------------
MEDIAMTX_HELP = (
    "MediaMTX not found. Download a release binary from\n"
    "  https://github.com/bluenviron/mediamtx/releases/latest\n"
    "extract it, and either put it on your $PATH or pass --mediamtx /path/to/mediamtx."
)


def find_mediamtx(explicit_path=None):
    if explicit_path:
        if os.path.exists(explicit_path) and os.access(explicit_path, os.X_OK):
            return explicit_path
        return None
    return shutil.which("mediamtx")


def write_mediamtx_config(state) -> str:
    """Write a minimal config that only enables RTSP on our chosen port."""
    cfg_path = os.path.join(tempfile.gettempdir(), "onvifemu-mediamtx.yml")
    with open(cfg_path, "w") as f:
        f.write(
            f"rtspAddress: :{state.rtsp_port}\n"
            "hls: no\n"
            "webrtc: no\n"
            "rtmp: no\n"
            "srt: no\n"
            "api: no\n"
            "metrics: no\n"
            "logLevel: warn\n"
            "paths:\n"
            "  all_others:\n"
        )
    return cfg_path


def spawn_stream(state, mediamtx_path=None):
    if shutil.which("ffmpeg") is None:
        print("[stream] ffmpeg not on PATH; skipping video. (apt install ffmpeg)")
        return []

    mediamtx = find_mediamtx(mediamtx_path)
    if mediamtx is None:
        print("[stream] " + MEDIAMTX_HELP)
        return []

    state.write_state_file()
    cfg = write_mediamtx_config(state)
    print(f"[stream] starting {mediamtx} (config: {cfg})")
    mtx_proc = subprocess.Popen([mediamtx, cfg])

    # Give MediaMTX a moment to open its listening socket.
    time.sleep(0.6)

    push_url = f"rtsp://127.0.0.1:{state.rtsp_port}/stream"
    ff_cmd = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel", "warning",
        "-re",
        "-f", "lavfi",
        "-i", "testsrc2=size=1280x720:rate=15",
        "-vf",
        f"drawtext=textfile={state.state_file}:reload=5:"
        "x=20:y=20:fontsize=28:fontcolor=white:box=1:boxcolor=black@0.6",
        "-c:v", "libx264",
        "-preset", "ultrafast",
        "-tune", "zerolatency",
        "-g", "30",
        "-f", "rtsp",
        "-rtsp_transport", "tcp",
        push_url,
    ]
    print(f"[stream] $ {' '.join(ff_cmd)}")
    ff_proc = subprocess.Popen(ff_cmd)
    print(f"[stream] OBS can now pull rtsp://{state.host}:{state.rtsp_port}/stream")
    return [mtx_proc, ff_proc]


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=primary_ipv4(),
                    help="IP advertised in discovery/stream URIs (default: auto)")
    ap.add_argument("--http-port", type=int, default=8899)
    ap.add_argument("--rtsp-port", type=int, default=8554)
    ap.add_argument("--state-file", default="/tmp/onvif-sim-state.txt")
    ap.add_argument("--with-stream", action="store_true",
                    help="also spawn ffmpeg + mediamtx so OBS gets a real RTSP test pattern")
    ap.add_argument("--mediamtx", default=None,
                    help="explicit path to the mediamtx binary (default: search $PATH)")
    args = ap.parse_args()

    state = State(args.host, args.http_port, args.rtsp_port, args.state_file)
    print(f"[sim] uuid       = {state.uuid}")
    print(f"[sim] advertise  = http://{state.host}:{state.http_port}/onvif/device_service")
    print(f"[sim] rtsp uri   = rtsp://{state.host}:{state.rtsp_port}/...")
    print(f"[sim] state file = {state.state_file}")

    OnvifHandler.state = state
    httpd = ThreadingHTTPServer(("0.0.0.0", state.http_port), OnvifHandler)
    http_thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    http_thread.start()
    print(f"[sim] HTTP listening on 0.0.0.0:{state.http_port}")

    threading.Thread(target=ws_discovery_loop, args=(state,), daemon=True).start()
    threading.Thread(target=state.status_loop, daemon=True).start()

    stream_procs = spawn_stream(state, args.mediamtx) if args.with_stream else []

    def shutdown(*_):
        state.running = False
        for p in stream_procs:
            if p.poll() is None:
                p.terminate()
        httpd.shutdown()

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    try:
        while state.running:
            time.sleep(0.5)
            for p in list(stream_procs):
                if p.poll() is not None:
                    print(f"[stream] subprocess exited with code {p.returncode}")
                    stream_procs.remove(p)
    except KeyboardInterrupt:
        pass
    finally:
        shutdown()


if __name__ == "__main__":
    main()
