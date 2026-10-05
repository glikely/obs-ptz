"""Optional WebGL "camera view" served over HTTP, for an OBS Browser Source.

Serves web/index.html, which renders a panorama through a virtual camera
whose yaw/pitch/FOV/blur follow the shared PTZState, and GET /events, a
Server-Sent Events stream of that state at ~50 Hz. Dragging the view
(in an OBS Browser Source's Interact window, say) moves the camera:
the page POSTs the position it wants to /move. It is not a video stream, so
it doesn't exercise OBS's Media Source decode path.
"""

import json
import os
import threading
import time
import urllib.parse
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
    backdrop = None  # path of the room picture, if any, injected
    scene = None     # {"dir", "entry", "camera_pos", "vendor"} of a 3D scene, if any, injected

    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path in ("/", "/index.html"):
            self._send_file("index.html", "text/html; charset=utf-8")
        elif path == "/scene.js":
            self._send_file("scene.js", "text/javascript; charset=utf-8")
        elif path == "/config":
            self._send_json({"scene": f"scene/{urllib.parse.quote(self.scene['entry'])}" if self.scene else None,
                             "cameraPos": self.scene["camera_pos"] if self.scene else None})
        elif path.startswith("/scene/") and self.scene:
            self._send_under(self.scene["dir"], path[len("/scene/"):])
        elif path.startswith("/vendor/") and self.scene:
            self._send_under(self.scene["vendor"], path[len("/vendor/"):])
        elif path == "/backdrop" and self.backdrop:
            self._send_backdrop()
        elif path == "/events":
            self._stream_events()
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        """The page holding the camera: POST /move {"pan", "tilt", "zoom"}
        (any of them, normalized as in PTZState) puts it there now, and
        POST /home sends it home"""
        path = self.path.split("?", 1)[0]
        length = int(self.headers.get("Content-Length") or 0)
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
            if path == "/move":
                self.state.jump_to(*(float(body[k]) if k in body else None
                                     for k in ("pan", "tilt", "zoom")))
            elif path == "/home":
                self.state.goto_home()
            else:
                self.send_response(404)
                self.end_headers()
                return
        except (ValueError, TypeError, AttributeError):
            self.send_response(400)
            self.end_headers()
            return
        self.send_response(204)
        self.end_headers()

    def _send_json(self, body):
        payload = json.dumps(body).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(payload)

    def _send_under(self, base, relative):
        """A file of the scene or of three.js: only from under `base`"""
        relative = urllib.parse.unquote(relative)
        path = os.path.realpath(os.path.join(base, relative))
        if not path.startswith(os.path.realpath(base) + os.sep) or not os.path.isfile(path):
            self.send_response(404)
            self.end_headers()
            return
        ext = os.path.splitext(path)[1].lower()
        self.send_response(200)
        self.send_header("Content-Type", {
            ".js": "text/javascript", ".gltf": "model/gltf+json", ".glb": "model/gltf-binary",
            ".jpg": "image/jpeg", ".jpeg": "image/jpeg", ".png": "image/png", ".webp": "image/webp",
        }.get(ext, "application/octet-stream"))
        self.send_header("Content-Length", str(os.path.getsize(path)))
        self.send_header("Cache-Control", "max-age=3600")
        self.end_headers()
        with open(path, "rb") as f:
            while chunk := f.read(1 << 20):
                self.wfile.write(chunk)

    def _send_backdrop(self):
        ext = os.path.splitext(self.backdrop)[1].lower()
        self.send_response(200)
        self.send_header("Content-Type", {".jpg": "image/jpeg", ".jpeg": "image/jpeg",
                                          ".png": "image/png"}.get(ext, "application/octet-stream"))
        self.send_header("Content-Length", str(os.path.getsize(self.backdrop)))
        self.send_header("Cache-Control", "max-age=3600")
        self.end_headers()
        with open(self.backdrop, "rb") as f:
            while chunk := f.read(1 << 20):
                self.wfile.write(chunk)

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
    def __init__(self, state, host="0.0.0.0", port=8080, backdrop=None, scene=None):
        self.state = state
        self.backdrop = backdrop
        self.scene = scene
        self.host = host
        self.port = port
        self._httpd = None

    def start(self):
        handler_cls = type("BoundWebHandler", (WebHandler,), {"state": self.state, "backdrop": self.backdrop, "scene": self.scene})
        self._httpd = ThreadingHTTPServer((self.host, self.port), handler_cls)
        self._httpd.daemon_threads = True
        self.port = self._httpd.server_address[1]
        threading.Thread(target=self._httpd.serve_forever, daemon=True).start()
        print(f"[web] camera view at http://127.0.0.1:{self.port}/ "
              "(add as an OBS Browser Source; ?hud=0 hides the readout)")

    def stop(self):
        if self._httpd:
            self._httpd.shutdown()
