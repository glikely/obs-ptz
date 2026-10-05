#!/usr/bin/env python3
"""Takes the screenshots the documentation uses (docs/*.png) of the plugin
running in a real OBS Studio on Windows.

Run it on Windows, in the logged in user's session (it needs the desktop),
with an OBS Studio extracted to --obs-dir that has this checkout's
obs-ptz built with -DENABLE_UI_TESTS=ON. It runs scripts/ptzsim itself, as
the camera, with a camera view for OBS's Browser Source (needs python on
Windows, and OBS's browser source). scripts/update-screenshots.sh, run on the
Mac host of the Parallels Windows VM, builds and runs it.
See tests/ui-harness/README.md for the OBS Studio setup.

OBS runs in portable mode, on its own, empty configuration in
<obs dir>\\config, so it never touches the one you use, and the screenshots
always start from the same place: three cameras, four
presets each with a thumbnail of what the camera saw, and nothing else.

Takes: docs/ptz-controls-screenshot.png, docs/ptz-presets-screenshot.png and
docs/ptz-settings-screenshot.png.

Usage: python scripts\\update-screenshots.py
"""
import argparse
import base64
import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import obs_ws_client  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
CAPTURE = REPO / "scripts" / "windows-capture.ps1"
WS_PORT = 4455
FILTER_KIND = "ca.secretlab.obs-ptz.visca"

# What the sim shows the camera at, for each preset: pan, tilt, zoom (absolute,
# in the range the harness's move_device takes), and the preset's name
PRESETS = [
    ("Wide", 0.0, 0.0, 0.0),
    ("Podium", -0.4, 0.1, 0.4),
    ("Choir", 0.5, -0.1, 0.3),
    ("Close up", 0.1, 0.2, 0.8),
]


def die(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(1)


class Obs:
    """An OBS Studio run in portable mode, so on a configuration of its own in
    <obs dir>\\config. On Windows OBS finds %APPDATA% from the system, not
    from the environment, so that is the only way to keep it from using (and
    leaving crash markers in) the configuration you use."""

    MARKER = ".ptz-screenshots"

    def __init__(self, obs_dir):
        self.obs_dir = Path(obs_dir)
        self.bin_dir = self.obs_dir / "bin" / "64bit"
        self.config_dir = self.obs_dir / "config"
        self.process = None

    def request(self, request_type, data=None, timeout=15):
        reply = obs_ws_client.call_request("127.0.0.1", WS_PORT, None, request_type, data or {}, timeout)
        status = reply.get("requestStatus", {})
        if not status.get("result"):
            raise RuntimeError(f"{request_type} failed: {status}")
        return reply.get("responseData", {})

    def ui(self, cmd, **params):
        """Runs a UI test harness command (tests/ui-harness). The request only
        says it was accepted, not that it is done."""
        reply = obs_ws_client.call_vendor_request(
            "127.0.0.1", WS_PORT, None, "obs-ptz", "ui_test_run", {"cmd": cmd, **{k: str(v) for k, v in params.items()}}, 15
        )
        if not reply.get("requestStatus", {}).get("result"):
            raise RuntimeError(f"{cmd} failed: {reply}")

    def config(self):
        """The empty configuration OBS starts on, with no first run wizard"""
        if self.config_dir.exists() and not (self.config_dir / self.MARKER).exists():
            die(f"{self.config_dir} is a portable configuration of yours, not one this made: move it away first")
        shutil.rmtree(self.config_dir, ignore_errors=True)
        root = self.config_dir / "obs-studio"
        (root / "plugin_config" / "obs-websocket").mkdir(parents=True)
        (self.config_dir / self.MARKER).write_text("made by scripts/update-screenshots.py; safe to delete\n")
        (root / "user.ini").write_text(
            "[General]\nFirstRun=true\nCurrentTheme3=Yami\nConfirmOnExit=false\n\n"
            "[Basic]\nProfile=Untitled\nProfileDir=Untitled\n"
            "SceneCollection=Untitled\nSceneCollectionFile=Untitled.json\n\n"
            # obs-websocket keeps its settings in one of these two places,
            # depending on the OBS version
            f"[OBSWebSocket]\nFirstLoad=false\nServerEnabled=true\nServerPort={WS_PORT}\nAuthRequired=false\n"
            "AlertsEnabled=false\n"
        )
        (root / "plugin_config" / "obs-websocket" / "config.json").write_text(
            json.dumps({"server_enabled": True, "server_port": WS_PORT, "auth_required": False, "first_load": False,
                        "alerts_enabled": False})
        )

    def running(self):
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq obs64.exe"], capture_output=True, text=True).stdout
        return "obs64.exe" in out

    def start(self):
        if self.running():
            die("OBS is running: quit it first, so it isn't taken for this one")
        self.config()
        # OBS finds its data relative to the working directory, not its exe
        self.process = subprocess.Popen(
            [str(self.bin_dir / "obs64.exe"), "--portable", "--disable-updater", "--disable-missing-files-check"],
            cwd=self.bin_dir,
            env={**os.environ, "PTZ_UI_TEST_HARNESS": "1"},
        )
        deadline = time.time() + 60
        while time.time() < deadline:
            try:
                self.request("GetVersion", timeout=3)
                return
            except (OSError, RuntimeError, obs_ws_client.ObsWebSocketError):
                time.sleep(1)
        die("OBS didn't start answering on obs-websocket")

    def stop(self):
        if not self.process:
            return
        # Asked to close, so it shuts down cleanly and leaves no crash marker
        subprocess.run(["taskkill", "/PID", str(self.process.pid)], capture_output=True)
        try:
            self.process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            subprocess.run(["taskkill", "/F", "/PID", str(self.process.pid), "/T"], capture_output=True)


def capture(title, out, prefix=False, geometry=None):
    command = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(CAPTURE), "-Title", title,
               "-Out", str(out)]
    if prefix:
        command.append("-Prefix")
    if geometry:
        for flag, value in zip(("-X", "-Y", "-W", "-H"), geometry):
            command += [flag, str(value)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        die(f"capturing '{title}': {result.stderr.strip() or result.stdout.strip()}")
    print(result.stdout.strip())


def device_id(obs, name, scratch):
    out = scratch / "device.json"
    out.unlink(missing_ok=True)
    obs.ui("get_device_source", name=name, filename=str(out))
    for _ in range(50):
        if out.exists():
            found = json.loads(out.read_text())
            if found.get("found"):
                return found["device_id"]
        time.sleep(0.2)
    die(f"no device for '{name}'")


def start_sim(args, scratch):
    """Runs the simulated camera, with its camera view, here: the camera is
    what the screenshots are taken of, so it can't depend on a network"""
    sim = subprocess.Popen(
        [sys.executable, "-u", "-m", "ptzsim", "--host", args.camera_host, "--web-port", str(args.web_port),
         "--visca-tcp-port", str(args.tcp_port), "--visca-udp-port", str(args.udp_port),
         # the emulated serial ports need a POSIX pty
         "--no-visca-serial", "--no-pelco"],
        cwd=REPO / "scripts", stdout=(scratch / "ptzsim.log").open("w"), stderr=subprocess.STDOUT,
    )
    deadline = time.time() + 20
    while time.time() < deadline:
        if sim.poll() is not None:
            die(f"ptzsim exited: {(scratch / 'ptzsim.log').read_text()}")
        try:
            urllib.request.urlopen(f"http://{args.camera_host}:{args.web_port}/", timeout=2).close()
            return sim
        except OSError:
            time.sleep(0.5)
    sim.terminate()
    die("ptzsim's camera view didn't come up")


def snapshot(obs, source, path):
    """Saves what OBS renders of a source as a PNG"""
    reply = obs.request("GetSourceScreenshot", {
        "sourceName": source, "imageFormat": "png", "imageWidth": 640, "imageHeight": 360,
    })
    path.write_bytes(base64.b64decode(reply["imageData"].split(",", 1)[1]))


def build_world(obs, args, scratch):
    cameras = [
        ("Stage Left", {"type": "visca-over-tcp", "host": args.camera_host, "tcp_port": args.tcp_port}),
        ("Stage Right", {"type": "visca-over-tcp", "host": args.camera_host, "tcp_port": args.tcp_port}),
        ("Balcony", {"type": "visca-over-tcp", "host": args.camera_host, "tcp_port": args.tcp_port}),
    ]
    scene = obs.request("GetCurrentProgramScene")["sceneName"]
    # What the camera sees is ptzsim's WebGL view, in a Browser Source
    obs.request("CreateInput", {
        "sceneName": scene, "inputName": "Camera view", "inputKind": "browser_source",
        "inputSettings": {"url": f"http://{args.camera_host}:{args.web_port}/", "width": 1280, "height": 720},
    })
    time.sleep(8)  # let the page load
    snapshot(obs, "Camera view", scratch / "view.png")

    # Only the first camera is on air, so the list shows a live one and others
    obs.request("CreateScene", {"sceneName": "Other cameras"})
    for n, (name, settings) in enumerate(cameras):
        # A filter with no video of its own is only added to an async source
        # such as a Media Source: OBS silently refuses it anywhere else, a
        # Browser Source included. So the camera is a Media Source showing a
        # still of the view (see make_presets()), which is also what the
        # preset thumbnails are taken of.
        obs.request("CreateInput", {
            "sceneName": scene if n == 0 else "Other cameras", "inputName": name, "inputKind": "ffmpeg_source",
            "inputSettings": {"is_local_file": True, "local_file": str(scratch / "view.png"), "looping": True,
                              "close_when_inactive": False},
        })
        obs.request("CreateSourceFilter", {
            "sourceName": name, "filterName": "PTZ Control", "filterKind": FILTER_KIND, "filterSettings": settings,
        })
    time.sleep(4)  # let the cameras connect
    return {name: device_id(obs, name, scratch) for name, _ in cameras}


def make_presets(obs, camera, source, scratch):
    for n, (name, pan, tilt, zoom) in enumerate(PRESETS):
        obs.ui("move_device", device_id=camera, mode="abs", pan=pan, tilt=tilt, zoom=zoom)
        time.sleep(2)  # let the view follow the camera
        shot = scratch / f"view-{n}.png"
        snapshot(obs, "Camera view", shot)
        obs.request("SetInputSettings", {"inputName": source, "inputSettings": {"local_file": str(shot)}})
        time.sleep(3)  # let the source show it, for the thumbnail
        obs.ui("add_preset", device_id=camera, name=name)
        time.sleep(1.5)
    obs.ui("move_device", device_id=camera, mode="abs", pan=0.0, tilt=0.0, zoom=0.0)
    time.sleep(2)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--obs-dir", default=r"C:\OBS-Test\arm64", help="the OBS Studio to run")
    parser.add_argument("--camera-host", default="127.0.0.1", help="where the simulated camera listens")
    parser.add_argument("--tcp-port", type=int, default=5678)
    parser.add_argument("--udp-port", type=int, default=52381)
    parser.add_argument("--web-port", type=int, default=8080, help="ptzsim's --web-port")
    parser.add_argument("--out", default=str(REPO / "docs"), help="where to save the screenshots")
    args = parser.parse_args()

    if os.name != "nt":
        die("this takes screenshots of OBS on Windows; run it there (see scripts/update-screenshots.sh)")
    out = Path(args.out)
    scratch = Path(tempfile.mkdtemp(prefix="ptz-screenshots-"))
    obs = Obs(args.obs_dir)
    sim = start_sim(args, scratch)
    try:
        obs.start()
        cameras = build_world(obs, args, scratch)
        camera = cameras["Stage Left"]
        make_presets(obs, camera, "Stage Left", scratch)

        # The dock beside the presets as a list, then as thumbnails
        obs.ui("show_dock", device_id=camera, grid="false", x=100, y=100, w=560, h=450)
        time.sleep(2)
        capture("PTZ Controls", out / "ptz-controls-screenshot.png")
        obs.ui("show_dock", device_id=camera, grid="true", x=100, y=100, w=640, h=450)
        time.sleep(2)
        capture("PTZ Controls", out / "ptz-presets-screenshot.png")

        obs.ui("show_settings", device_id=camera, x=150, y=100, w=1000, h=640)
        time.sleep(3)
        capture("PTZ Settings", out / "ptz-settings-screenshot.png")
    finally:
        obs.stop()
        sim.terminate()
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    main()
