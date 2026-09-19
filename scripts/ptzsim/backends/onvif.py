"""ONVIF (WS-Discovery + SOAP) protocol driver.

Speaks the same minimal ONVIF subset as the original onvifemu.py, but
drives a shared PTZState instead of owning pan/tilt/zoom/preset state
itself. Auth is intentionally not enforced -- the goal is exercising
obs-ptz's discovery and command paths, not the WS-Security implementation.
"""

import socket
import struct
import sys
import threading
import time
import uuid
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from xml.etree import ElementTree as ET

from .base import Backend

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


def now_iso():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def soap_envelope(body_xml):
    return (
        '<?xml version="1.0" encoding="UTF-8"?>'
        f'<s:Envelope xmlns:s="{NS["s"]}" xmlns:tds="{NS["tds"]}" '
        f'xmlns:trt="{NS["trt"]}" xmlns:tptz="{NS["tptz"]}" '
        f'xmlns:tt="{NS["tt"]}">'
        f"<s:Body>{body_xml}</s:Body>"
        "</s:Envelope>"
    )


def soap_fault(reason):
    return soap_envelope(
        f'<s:Fault xmlns:s="{NS["s"]}">'
        f"<s:Code><s:Value>s:Receiver</s:Value></s:Code>"
        f"<s:Reason><s:Text>{reason}</s:Text></s:Reason>"
        f"</s:Fault>"
    )


def parse_soap_action(body):
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
    for el in root.iter():
        if el.tag.endswith("}Body"):
            for child in el:
                op = child.tag.split("}")[-1]
                break
            break
    if not op and action:
        op = action.rsplit("/", 1)[-1]
    return action, op


def first_text(body, local_name):
    try:
        root = ET.fromstring(body)
    except ET.ParseError:
        return ""
    for el in root.iter():
        if el.tag.endswith("}" + local_name) or el.tag == local_name:
            return (el.text or "").strip()
    return ""


def read_xyz(body, container):
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


def extract_message_id(data):
    try:
        root = ET.fromstring(data)
    except ET.ParseError:
        return ""
    for el in root.iter():
        if el.tag.endswith("}MessageID"):
            return (el.text or "").strip()
    return ""


class OnvifHandler(BaseHTTPRequestHandler):
    backend = None  # injected: the owning OnvifBackend

    def log_message(self, fmt, *args):
        pass  # quieter default access log

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        action, op = parse_soap_action(body)
        print(f"[onvif] {self.path} -> {op}")
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
        b = self.backend
        state = b.state
        # ---- Device service ----
        if op == "GetCapabilities":
            base = f"http://{b.host}:{b.http_port}"
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
                f"<tds:SerialNumber>{b.uuid[-12:]}</tds:SerialNumber>"
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
            uri = f"rtsp://{b.host}:{b.rtsp_port}/stream"
            return soap_envelope(
                "<trt:GetStreamUriResponse><trt:MediaUri>"
                f"<tt:Uri>{uri}</tt:Uri>"
                "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
                "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
                "<tt:Timeout>PT60S</tt:Timeout>"
                "</trt:MediaUri></trt:GetStreamUriResponse>"
            )
        if op == "GetSnapshotUri":
            uri = f"http://{b.host}:{b.http_port}/snapshot.jpg"
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
            state.set_pt_speed(x, y)
            state.set_zoom_speed(z)
            return soap_envelope("<tptz:ContinuousMoveResponse/>")
        if op == "AbsoluteMove":
            x, y, z = read_xyz(body, "Position")
            state.set_position(pan=x, tilt=y, zoom=z)
            return soap_envelope("<tptz:AbsoluteMoveResponse/>")
        if op == "RelativeMove":
            x, y, z = read_xyz(body, "Translation")
            state.move_relative(dpan=x, dtilt=y, dzoom=z)
            return soap_envelope("<tptz:RelativeMoveResponse/>")
        if op == "Stop":
            # PTZ Stop has PanTilt/Zoom flags; Imaging Stop has VideoSourceToken.
            if first_text(body, "VideoSourceToken"):
                print("[onvif] focus stop")
                return soap_envelope(
                    "<timg:StopResponse xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\"/>"
                )
            pantilt = (first_text(body, "PanTilt") or "true").lower() == "true"
            zoom = (first_text(body, "Zoom") or "true").lower() == "true"
            state.stop(pan_tilt=pantilt, zoom=zoom)
            return soap_envelope("<tptz:StopResponse/>")
        if op == "GotoHomePosition":
            state.goto_home()
            return soap_envelope("<tptz:GotoHomePositionResponse/>")
        if op == "SetHomePosition":
            state.set_home()
            home = state.home
            print(f"[onvif] home set to pan={home.pan:+.2f} tilt={home.tilt:+.2f} zoom={home.zoom:.2f}")
            return soap_envelope("<tptz:SetHomePositionResponse/>")
        if op == "GetStatus":
            snap = state.snapshot()
            return soap_envelope(
                "<tptz:GetStatusResponse><tptz:PTZStatus>"
                "<tt:Position>"
                f'<tt:PanTilt x="{snap.pan:.4f}" y="{snap.tilt:.4f}"/>'
                f'<tt:Zoom x="{snap.zoom:.4f}"/>'
                "</tt:Position>"
                "<tt:MoveStatus>"
                f"<tt:PanTilt>{'MOVING' if snap.pan_tilt_moving else 'IDLE'}</tt:PanTilt>"
                f"<tt:Zoom>{'MOVING' if snap.zoom_moving else 'IDLE'}</tt:Zoom>"
                "</tt:MoveStatus>"
                f"<tt:UtcTime>{now_iso()}</tt:UtcTime>"
                "</tptz:PTZStatus></tptz:GetStatusResponse>"
            )
        if op == "GetPresets":
            entries = []
            for token, preset in state.list_presets().items():
                p, t, z = preset.position.pan, preset.position.tilt, preset.position.zoom
                entries.append(
                    f'<tptz:Preset token="{token}"><tt:Name>{preset.name}</tt:Name>'
                    f'<tt:PTZPosition><tt:PanTilt x="{p:.4f}" y="{t:.4f}"/>'
                    f'<tt:Zoom x="{z:.4f}"/></tt:PTZPosition></tptz:Preset>'
                )
            return soap_envelope(
                "<tptz:GetPresetsResponse>" + "".join(entries) + "</tptz:GetPresetsResponse>"
            )
        if op == "SetPreset":
            token = first_text(body, "PresetToken") or ""
            name = first_text(body, "PresetName") or ""
            new_token = state.set_preset(token, name)
            return soap_envelope(
                f"<tptz:SetPresetResponse><tptz:PresetToken>{new_token}</tptz:PresetToken>"
                "</tptz:SetPresetResponse>"
            )
        if op == "GotoPreset":
            token = first_text(body, "PresetToken") or ""
            state.goto_preset(token)
            return soap_envelope("<tptz:GotoPresetResponse/>")
        if op == "RemovePreset":
            token = first_text(body, "PresetToken") or ""
            state.remove_preset(token)
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
            state.set_focus_speed(focus_speed)
            print(f"[onvif] focus continuous speed={focus_speed:+.3f}")
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
                print(f"[onvif] WhiteBalance.Mode = {wb}")
            if af:
                print(f"[onvif] Focus.AutoFocusMode = {af}")
            return soap_envelope(
                "<timg:SetImagingSettingsResponse xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\"/>"
            )
        return None


class OnvifBackend(Backend):
    def __init__(self, state, host, http_port=8899, rtsp_port=8554):
        self.state = state
        self.host = host
        self.http_port = http_port
        self.rtsp_port = rtsp_port
        self.uuid = "urn:uuid:" + str(uuid.uuid4())
        self._httpd = None
        self._running = threading.Event()

    def start(self, loop=None):
        self._running.set()

        handler_cls = type("BoundOnvifHandler", (OnvifHandler,), {"backend": self})
        self._httpd = ThreadingHTTPServer(("0.0.0.0", self.http_port), handler_cls)
        threading.Thread(target=self._httpd.serve_forever, daemon=True).start()
        print(f"[onvif] HTTP listening on 0.0.0.0:{self.http_port}")
        print(f"[onvif] advertise = http://{self.host}:{self.http_port}/onvif/device_service")

        threading.Thread(target=self._ws_discovery_loop, daemon=True).start()

    def stop(self):
        self._running.clear()
        if self._httpd:
            self._httpd.shutdown()

    def _ws_discovery_loop(self):
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
            print(f"[onvif] wsd bind failed ({e}); will not respond to discovery probes", file=sys.stderr)
            return

        print(f"[onvif] wsd listening on udp/{WSD_PORT}")
        while self._running.is_set():
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
            reply = self._build_probe_match(msg_id)
            try:
                sock.sendto(reply.encode("utf-8"), addr)
                print(f"[onvif] wsd replied to {addr}")
            except OSError as e:
                print(f"[onvif] wsd send failed: {e}", file=sys.stderr)

    def _build_probe_match(self, relates_to):
        xaddr = f"http://{self.host}:{self.http_port}/onvif/device_service"
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
    <a:EndpointReference><a:Address>{self.uuid}</a:Address></a:EndpointReference>
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
