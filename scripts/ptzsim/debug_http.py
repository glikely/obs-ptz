"""Optional JSON introspection endpoint, mainly for automated testing.

Not a camera protocol -- just a way for a test harness to ask "what does
the simulated camera think its state is right now" without decoding one
of the real wire protocols. Off unless --debug-http-port is passed. A POST
to /move_time changes how long the camera's moves take, for a test of what
happens while one is under way, and one to /serial/release lets the emulated
serial ports be opened again by a program after one that never closed them
(see EmulatedSerialPort.release()).
"""

import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class DebugHandler(BaseHTTPRequestHandler):
    state = None  # injected

    def log_message(self, fmt, *args):
        pass

    def do_POST(self):
        """/move_time {"seconds": s}: how long a move across the range takes
        from now on, as --move-time says (0 for at once)"""
        if self.path == "/serial/release":
            from .serial_port import release_all

            release_all()
            self.send_response(204)
            self.end_headers()
            return
        if self.path != "/move_time":
            self.send_response(404)
            self.end_headers()
            return
        length = int(self.headers.get("Content-Length", 0))
        try:
            seconds = float(json.loads(self.rfile.read(length))["seconds"])
        except (ValueError, KeyError, TypeError):
            self.send_response(400)
            self.end_headers()
            return
        self.state.set_move_time(seconds)
        self.send_response(204)
        self.end_headers()

    def do_GET(self):
        if self.path != "/state":
            self.send_response(404)
            self.end_headers()
            return
        snap = self.state.snapshot()
        body = {
            "pan": snap.pan,
            "tilt": snap.tilt,
            "zoom": snap.zoom,
            "focus": snap.focus,
            "pan_speed": snap.pan_speed,
            "tilt_speed": snap.tilt_speed,
            "zoom_speed": snap.zoom_speed,
            "focus_speed": snap.focus_speed,
            "power": snap.power,
            "tally": dict(self.state.tally),
            # The camera settings VISCA reads and changes, by the state key
            # obs-ptz has for each
            "visca": dict(self.state.visca),
            "visca_syntax_errors": self.state.visca_syntax_errors,
            "visca_inquiries": list(self.state.visca_inquiries),
            # Counters of the VISCA-over-IP server's Sony quirks, if it has any
            "visca_udp_quirks": getattr(self.state, "visca_udp_stats", None),
            "presets": {
                token: {
                    "name": preset.name,
                    "pan": preset.position.pan,
                    "tilt": preset.position.tilt,
                    "zoom": preset.position.zoom,
                    "focus": preset.position.focus,
                }
                for token, preset in self.state.list_presets().items()
            },
        }
        payload = json.dumps(body).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


class DebugHttpServer:
    def __init__(self, state, host="127.0.0.1", port=0):
        self.state = state
        self.host = host
        self.port = port
        self._httpd = None

    def start(self):
        handler_cls = type("BoundDebugHandler", (DebugHandler,), {"state": self.state})
        self._httpd = ThreadingHTTPServer((self.host, self.port), handler_cls)
        self.port = self._httpd.server_address[1]
        threading.Thread(target=self._httpd.serve_forever, daemon=True).start()
        print(f"[debug] state introspection at http://{self.host}:{self.port}/state")

    def stop(self):
        if self._httpd:
            self._httpd.shutdown()
