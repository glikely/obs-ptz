"""Optional WebGL "camera view" served over HTTP, for an OBS Browser Source.

Serves web/index.html, which renders a panorama through a virtual camera
whose yaw/pitch/FOV/blur follow the shared PTZState, and GET /events, a
Server-Sent Events stream of that state at ~50 Hz. Unlike the RTSP feed
this needs no ffmpeg or MediaMTX, but it is not a video stream: it
doesn't exercise OBS's Media Source decode path.
"""

import json
import os
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

WEB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")
EVENT_HZ = 50


def state_event(state):
    snap = state.snapshot()
    with state._lock:
        tally = dict(state.tally)
    return {
        "pan": snap.pan, "tilt": snap.tilt, "zoom": snap.zoom, "focus": snap.focus,
        "pan_speed": snap.pan_speed, "tilt_speed": snap.tilt_speed,
        "zoom_speed": snap.zoom_speed, "focus_speed": snap.focus_speed,
        "power": snap.power, "tally": tally,
    }


class WebHandler(BaseHTTPRequestHandler):
    state = None  # injected

    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path in ("/", "/index.html"):
            self._send_file("index.html", "text/html; charset=utf-8")
        elif path == "/events":
            self._stream_events()
        else:
            self.send_response(404)
            self.end_headers()

    def _send_file(self, name, content_type):
        with open(os.path.join(WEB_DIR, name), "rb") as f:
            body = f.read()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(body)

    def _stream_events(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        try:
            while True:
                data = json.dumps(state_event(self.state))
                self.wfile.write(f"data: {data}\n\n".encode("utf-8"))
                self.wfile.flush()
                time.sleep(1.0 / EVENT_HZ)
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass


class WebViewServer:
    def __init__(self, state, host="0.0.0.0", port=8080):
        self.state = state
        self.host = host
        self.port = port
        self._httpd = None

    def start(self):
        handler_cls = type("BoundWebHandler", (WebHandler,), {"state": self.state})
        self._httpd = ThreadingHTTPServer((self.host, self.port), handler_cls)
        self._httpd.daemon_threads = True
        self.port = self._httpd.server_address[1]
        threading.Thread(target=self._httpd.serve_forever, daemon=True).start()
        print(f"[web] camera view at http://127.0.0.1:{self.port}/ "
              "(add as an OBS Browser Source; ?hud=0 hides the readout)")

    def stop(self):
        if self._httpd:
            self._httpd.shutdown()
