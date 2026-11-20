"""Panasonic HTTP CGI protocol driver.

Speaks the AW-HE / AW-HN / AW-UE HTTP CGI subset against a shared PTZState:
- GET /cgi-bin/aw_ptz?cmd=%23<CMD>&res=1
- GET /cgi-bin/aw_cam?cmd=OSE:69:1&res=1
- GET /live/camdata.html
"""

import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from .base import Backend

ZOOM_FOCUS_MIN = 0x555
ZOOM_FOCUS_MAX = 0xFFF
ZOOM_FOCUS_SPAN = ZOOM_FOCUS_MAX - ZOOM_FOCUS_MIN


def _clamp(val, lo, hi):
    return max(lo, min(hi, val))


def _norm_to_pan_tilt_hex(val):
    raw = 0x8000 + round(_clamp(val, -1.0, 1.0) * 0x7FFF)
    return _clamp(raw, 0x0000, 0xFFFF)


def _pan_tilt_hex_to_norm(raw):
    return _clamp((raw - 0x8000) / 0x7FFF, -1.0, 1.0)


def _norm_to_zoom_focus_hex(val):
    raw = ZOOM_FOCUS_MIN + round(_clamp(val, 0.0, 1.0) * ZOOM_FOCUS_SPAN)
    return _clamp(raw, ZOOM_FOCUS_MIN, ZOOM_FOCUS_MAX)


def _zoom_focus_hex_to_norm(raw):
    return _clamp((raw - ZOOM_FOCUS_MIN) / ZOOM_FOCUS_SPAN, 0.0, 1.0)


class PanasonicCameraLogic:
    """Protocol logic for Panasonic AW HTTP CGI, independent of HTTP transport."""

    def __init__(self, state):
        self.state = state
        self._lock = threading.Lock()
        self.autofocus = True
        self.onepush_af_count = 0
        self.command_log = []

    def handle_ptz_cmd(self, cmd):
        """Handles a decoded #... command from /cgi-bin/aw_ptz?cmd=..."""
        if not cmd or not cmd.startswith("#"):
            return 400, "ERR"
        body = cmd[1:]
        with self._lock:
            self.command_log.append(cmd)

        if body.startswith("PTS") and len(body) == 7:
            try:
                pp = int(body[3:5])
                tt = int(body[5:7])
            except ValueError:
                return 400, "ERR"
            if not (1 <= pp <= 99 and 1 <= tt <= 99):
                return 400, "ERR"
            self.state.set_pt_speed((pp - 50) / 49.0, (tt - 50) / 49.0)
            return 200, f"pTS{pp:02d}{tt:02d}"

        if body.startswith("Z") and len(body) == 3:
            try:
                zz = int(body[1:3])
            except ValueError:
                return 400, "ERR"
            if not (1 <= zz <= 99):
                return 400, "ERR"
            self.state.set_zoom_speed((zz - 50) / 49.0)
            return 200, f"zS{zz:02d}"

        if body.startswith("F") and len(body) == 3:
            try:
                ff = int(body[1:3])
            except ValueError:
                return 400, "ERR"
            if not (1 <= ff <= 99):
                return 400, "ERR"
            self.state.set_focus_speed((ff - 50) / 49.0)
            return 200, f"fS{ff:02d}"

        if body.startswith("APC") and len(body) == 11:
            try:
                pan_raw = int(body[3:7], 16)
                tilt_raw = int(body[7:11], 16)
            except ValueError:
                return 400, "ERR"
            self.state.set_position(
                pan=_pan_tilt_hex_to_norm(pan_raw),
                tilt=_pan_tilt_hex_to_norm(tilt_raw),
            )
            return 200, f"aPC{pan_raw:04X}{tilt_raw:04X}"

        if body.startswith("AXZ") and len(body) == 6:
            try:
                zoom_raw = int(body[3:6], 16)
            except ValueError:
                return 400, "ERR"
            self.state.set_position(zoom=_zoom_focus_hex_to_norm(zoom_raw))
            return 200, f"aXZ{zoom_raw:03X}"

        if body.startswith("AXF") and len(body) == 6:
            try:
                focus_raw = int(body[3:6], 16)
            except ValueError:
                return 400, "ERR"
            self.state.set_position(focus=_zoom_focus_hex_to_norm(focus_raw))
            return 200, f"aXF{focus_raw:03X}"

        if body.startswith("M") and len(body) == 3 and body[1:3].isdigit():
            slot = body[1:3]
            self.state.set_preset(slot, f"Preset {slot}")
            return 200, f"s{slot}"

        if body.startswith("R") and len(body) == 3 and body[1:3].isdigit():
            slot = body[1:3]
            self.state.goto_preset(slot)
            return 200, f"s{slot}"

        if body.startswith("C") and len(body) == 3 and body[1:3].isdigit():
            slot = body[1:3]
            self.state.remove_preset(slot)
            return 200, f"s{slot}"

        if body in ("D10", "D11"):
            with self._lock:
                self.autofocus = (body == "D11")
            return 200, body.lower()

        if body in ("O0", "O1"):
            self.state.set_power(body == "O1")
            return 200, body.lower()

        return 400, "ERR"

    def handle_cam_cmd(self, cmd):
        with self._lock:
            self.command_log.append(f"cam:{cmd}")
            if cmd == "OSE:69:1":
                self.onepush_af_count += 1
                return 200, cmd
        return 400, "ERR"

    def render_camdata(self):
        snap = self.state.snapshot()
        with self._lock:
            af = self.autofocus
        pan_hex = _norm_to_pan_tilt_hex(snap.pan)
        tilt_hex = _norm_to_pan_tilt_hex(snap.tilt)
        zoom_hex = _norm_to_zoom_focus_hex(snap.zoom)
        focus_hex = _norm_to_zoom_focus_hex(snap.focus)
        lines = [
            "p1" if snap.power else "p0",
            f"aPC{pan_hex:04X}{tilt_hex:04X}",
            f"gz{zoom_hex:03X}",
            f"gf{focus_hex:03X}",
            "d11" if af else "d10",
        ]
        return "\r\n".join(lines) + "\r\n"


class PanasonicBackend(Backend):
    """HTTP CGI server implementing the Panasonic AW PTZ endpoints."""

    def __init__(self, state, host="127.0.0.1", http_port=8081, delay=0.0):
        self.state = state
        self.host = host
        self.http_port = http_port
        self.delay = delay
        self.logic = PanasonicCameraLogic(state)
        self._httpd = None
        self._thread = None

    def start(self, loop=None):
        logic = self.logic
        delay = self.delay

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, format, *args):
                pass

            def do_GET(self):
                if delay > 0:
                    time.sleep(delay)
                parsed = urlparse(self.path)
                params = parse_qs(parsed.query, keep_blank_values=True)

                if parsed.path == "/cgi-bin/aw_ptz":
                    cmd = params.get("cmd", [""])[0]
                    code, text = logic.handle_ptz_cmd(cmd)
                elif parsed.path == "/cgi-bin/aw_cam":
                    cmd = params.get("cmd", [""])[0]
                    code, text = logic.handle_cam_cmd(cmd)
                elif parsed.path == "/live/camdata.html":
                    code, text = 200, logic.render_camdata()
                else:
                    code, text = 404, "Not Found"

                payload = text.encode("utf-8")
                self.send_response(code)
                self.send_header("Content-Type", "text/plain; charset=utf-8")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

        self._httpd = ThreadingHTTPServer((self.host, self.http_port), Handler)
        self.http_port = self._httpd.server_address[1]
        self._thread = threading.Thread(target=self._httpd.serve_forever, daemon=True)
        self._thread.start()
        print(f"[panasonic] http listening on {self.host}:{self.http_port}")

    def stop(self):
        if self._httpd is not None:
            self._httpd.shutdown()
            self._httpd.server_close()
            self._httpd = None

