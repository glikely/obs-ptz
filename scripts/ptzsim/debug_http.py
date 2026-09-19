"""Optional JSON introspection endpoint, mainly for automated testing.

Not a camera protocol -- just a way for a test harness to ask "what does
the simulated camera think its state is right now" without decoding one
of the real wire protocols. Off unless --debug-http-port is passed.
"""

import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class DebugHandler(BaseHTTPRequestHandler):
    state = None  # injected

    def log_message(self, fmt, *args):
        pass

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
