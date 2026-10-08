#!/usr/bin/env python3
"""Run OBS with obs-ptz and several emulated cameras, in a profile of its own.

For trying the plugin by hand, as tests/obs-integration does for the tests:

- an empty OBS profile in a temporary home directory, so your own is never
  read or changed (macOS finds it through CFFIXED_USER_HOME, Linux through $HOME);
- one ptzsim (scripts/ptzsim) per camera, each on ports of its own and with
  its own view of the room;
- obs-ptz's config.json with a device for each, talking the protocol you chose for it, which
  the plugin turns into a filter on the camera's source, and presets for it: some kept in
  the camera and some local;
- the PTZ dock docked at the bottom of the window, where OBS would otherwise leave it hidden
  and floating in a new profile;
- a scene collection with a Browser Source for each camera, named after it
  and showing its ptzsim's web view, a scene of each alone and one of them all;
- studio mode, with the first camera's scene in the program and the second's
  in the preview;
- obs-websocket, with no password, on the port it prints.

Then OBS runs here, with its verbose log in this terminal, until you quit it or press
Ctrl-C, when the cameras are stopped and the temporary home is removed.

    scripts/ptz-demo.py                       # three cameras: VISCA/TCP, VISCA/UDP, ONVIF
    scripts/ptz-demo.py -n 4 --move-time 3    # four, whose moves take time
    scripts/ptz-demo.py -p visca-tcp,pelco-d  # a camera for each protocol, in turn
    scripts/ptz-demo.py --room sponza         # each in a place in a 3D room (downloads it)
    scripts/ptz-demo.py --home ~/ptz-demo     # keep the profile between runs

Protocols: visca-tcp, visca-udp, onvif, visca-serial, pelco-d, pelco-p. The
serial ones need a plugin built with ENABLE_SERIALPORT=ON.

On Windows it uses the portable OBS at PTZSIM_OBS_BINARY (such as
C:\\OBS-Test\\arm64\\bin\\64bit\\obs64.exe, with the plugin built into it by
scripts\\windows-build-and-test.bat), whose own profile is put aside for the run
and put back at the end; only the TCP, UDP and ONVIF cameras are there.

Env: PTZSIM_OBS_BINARY (the OBS executable) and PTZSIM_PLUGIN_BUNDLE (the
obs-ptz.plugin on macOS, the obs-ptz.so on Linux) as for the integration tests.
"""

import argparse
import json
import math
import os
import platform
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = REPO_ROOT / "scripts"
DARWIN = platform.system() == "Darwin"
WINDOWS = platform.system() == "Windows"

# Presets every camera starts with: (slot, name, pan, tilt, zoom) kept in the camera, which
# its protocol calls a slot or a token, and local ones the plugin keeps (name, pan, tilt, zoom)
CAMERA_PRESETS = (("0", "Wide", 0.0, 0.0, 0.0), ("1", "Left", -0.5, 0.0, 0.2), ("2", "Close-up", 0.2, 0.1, 0.7))
LOCAL_PRESETS = (("Stage right", 0.6, 0.0, 0.3), ("Overhead", 0.0, 0.5, 0.0))

PROTOCOLS = ("visca-tcp", "visca-udp", "onvif", "visca-serial", "pelco-d", "pelco-p")
ALL_SCENE = "All cameras"
COLLECTION = "ptz-demo"
SOURCE_SIZE = (1280, 720)
CANVAS = (1920, 1080)


def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for_port(port, timeout, proc=None):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc is not None and proc.poll() is not None:
            raise RuntimeError(f"ptzsim exited with {proc.returncode}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                return
        except OSError:
            time.sleep(0.2)
    raise TimeoutError(f"nothing listening on port {port} after {timeout}s")


def config_root(home):
    if WINDOWS:
        # A portable OBS, whose home is the folder it is installed in, keeps its profile there
        return home / "config" / "obs-studio"
    if DARWIN:
        return home / "Library" / "Application Support" / "obs-studio"
    return home / ".config" / "obs-studio"


def find_obs():
    explicit = os.environ.get("PTZSIM_OBS_BINARY")
    if explicit:
        return Path(explicit)
    if DARWIN:
        apps = sorted(Path("/Applications").glob("OBS [0-9]*.app"), key=lambda p: [
            int(x) if x.isdigit() else 0 for x in p.name[4:-4].replace("-", ".").split(".")])
        for app in reversed(apps) or [Path("/Applications/OBS.app")]:
            if (app / "Contents/MacOS/OBS").exists():
                return app / "Contents/MacOS/OBS"
        return Path("/Applications/OBS.app/Contents/MacOS/OBS")
    if WINDOWS:
        sys.exit("set PTZSIM_OBS_BINARY to the obs64.exe of an OBS to run, as C:\\OBS-Test\\arm64\\bin\\64bit\\obs64.exe")
    return Path(shutil.which("obs") or "obs")


def find_plugin():
    if WINDOWS:
        return None  # the one in the OBS install is the one to run: see scripts/windows-build-and-test.bat
    explicit = os.environ.get("PTZSIM_PLUGIN_BUNDLE")
    if explicit:
        return Path(explicit)
    name = "obs-ptz.plugin" if DARWIN else "obs-ptz.so"
    for build in sorted(REPO_ROOT.glob("build_*")):
        found = build / "rundir" / "RelWithDebInfo" / name
        if found.exists():
            return found
    sys.exit(f"no {name} built in this checkout: build it (scripts/macos-dev.sh setup on macOS) "
             "or set PTZSIM_PLUGIN_BUNDLE")


def install_plugin(home, plugin):
    """Make the plugin the only one OBS loads from this home"""
    if WINDOWS:
        return
    plugins = config_root(home) / "plugins"
    plugins.mkdir(parents=True, exist_ok=True)
    if DARWIN:
        (plugins / "obs-ptz.plugin").symlink_to(plugin.resolve())
        return
    # OBS on Linux skips a symlinked .so, so it is a copy; and its locale
    # strings are in data/ next to it
    dest = plugins / "obs-ptz"
    (dest / "bin" / "64bit").mkdir(parents=True, exist_ok=True)
    shutil.copy2(plugin, dest / "bin" / "64bit" / "obs-ptz.so")
    data = REPO_ROOT / "data"
    if data.is_dir():
        shutil.copytree(data, dest / "data", dirs_exist_ok=True)


class Camera:
    def __init__(self, index, protocol, work, args):
        self.index = index
        self.name = f"Camera {index + 1}"
        self.protocol = protocol
        self.ports = {
            "web": free_port(),
            "visca_tcp": free_port(),
            "visca_udp": free_port(socket.SOCK_DGRAM),
            "onvif": free_port(),
            "rtsp": free_port(),
        }
        self.visca_serial = work / f"camera{index + 1}-visca-serial"
        self.pelco_serial = work / f"camera{index + 1}-pelco-serial"
        self.args = args
        self.proc = None
        self.uuid = None

    def command(self):
        a = self.args
        cmd = [sys.executable, "-m", "ptzsim", "--host", "127.0.0.1",
               "--web-port", str(self.ports["web"]),
               "--visca-tcp-port", str(self.ports["visca_tcp"]),
               "--visca-udp-port", str(self.ports["visca_udp"]),
               "--onvif-http-port", str(self.ports["onvif"]),
               "--rtsp-port", str(self.ports["rtsp"]),
               "--visca-serial-path", str(self.visca_serial),
               "--pelco-serial-path", str(self.pelco_serial),
               "--pelco-address", "1"]
        for slot, name, pan, tilt, zoom in CAMERA_PRESETS:
            cmd += ["--preset", f"{slot}={name}@{pan},{tilt},{zoom}"]
        if WINDOWS:  # there is no pty there to be a serial port
            cmd += ["--no-visca-serial", "--no-pelco"]
        if a.move_time:
            cmd += ["--move-time", str(a.move_time)]
        if a.room:
            cmd += ["--room", a.room, "--camera", a.room_cameras[self.index % len(a.room_cameras)]]
        else:
            # The same panorama from a different way round for each camera
            cmd += ["--heading", str((self.index * 360 / max(a.cameras, 1)) % 360)]
        return cmd

    def start(self, log_dir):
        log = open(log_dir / f"ptzsim-camera{self.index + 1}.log", "w")
        self.proc = subprocess.Popen(self.command(), cwd=SCRIPTS, stdout=log, stderr=subprocess.STDOUT,
                                     # No console window for a camera, which Windows Terminal would
                                     # open one of each of
                                     **({"creationflags": subprocess.CREATE_NO_WINDOW} if WINDOWS else
                                        {"start_new_session": True}))
        log.close()
        wait_for_port(self.ports["web"], 30, self.proc)

    def stop(self):
        if self.proc is None or self.proc.poll() is not None:
            return
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    def device(self):
        """The camera as the plugin's old config.json has it, which the plugin
        turns into a filter on the source of this name"""
        device = {"id": self.index + 1, "name": self.name}
        p = self.protocol
        if p == "visca-tcp":
            device.update(type="visca-over-tcp", host="127.0.0.1", tcp_port=self.ports["visca_tcp"])
        elif p == "visca-udp":
            device.update(type="visca-over-ip", host="127.0.0.1", udp_port=self.ports["visca_udp"])
        elif p == "onvif":
            device.update(type="onvif", host="127.0.0.1", port=self.ports["onvif"], username="admin", password="")
        elif p == "visca-serial":
            device.update(type="visca", serial_port=str(self.visca_serial), address=1)
        else:
            device.update(type="pelco", serial_port=str(self.pelco_serial), address=1, use_pelco_d=(p == "pelco-d"))
        # The presets the dock lists: ONVIF's camera says which it has, and the others cannot, so
        # have them as the plugin saved them; a local preset is the plugin's own
        presets = [] if p == "onvif" else [{"id": f"camera:{slot}", "name": name}
                                           for slot, name, *_ in CAMERA_PRESETS]
        presets += [{"id": f"local:demo{n}", "name": name, "values": {"pan": pan, "tilt": tilt, "zoom": zoom}}
                    for n, (name, pan, tilt, zoom) in enumerate(LOCAL_PRESETS, 1)]
        device["presets"] = presets
        return device


def scene_source(name, uuid, items):
    return {"id": "scene", "versioned_id": "scene", "name": name, "uuid": uuid, "enabled": True,
            "settings": {"id_counter": len(items), "items": items}}


def scene_item(camera, item_id, x, y, scale):
    return {"name": camera.name, "source_uuid": camera.uuid, "id": item_id, "visible": True, "align": 5,
            "pos": {"x": x, "y": y}, "scale": {"x": scale, "y": scale}}


def scene_names(cameras):
    """(preview, program, all): a scene of each camera alone, and one with them all. The
    program is the first camera's and the preview the second's, to have a different
    one in each (with one camera, the same)"""
    return (f"{cameras[1 % len(cameras)].name} scene", f"{cameras[0].name} scene", ALL_SCENE)


def scene_collection(cameras):
    """A Browser Source for each camera, a scene of each alone, and one of them all in a grid"""
    cols = math.ceil(math.sqrt(len(cameras)))
    rows = math.ceil(len(cameras) / cols)
    cell_w, cell_h = CANVAS[0] / cols, CANVAS[1] / rows
    grid_scale = min(cell_w / SOURCE_SIZE[0], cell_h / SOURCE_SIZE[1])
    full_scale = min(CANVAS[0] / SOURCE_SIZE[0], CANVAS[1] / SOURCE_SIZE[1])
    sources, scenes, grid = [], [], []
    for i, camera in enumerate(cameras):
        camera.uuid = f"00000000-0000-4000-8000-{i + 1:012d}"
        sources.append({
            "id": "browser_source",
            "versioned_id": "browser_source",
            "name": camera.name,
            "uuid": camera.uuid,
            "enabled": True,
            "settings": {
                "url": f"http://127.0.0.1:{camera.ports['web']}/",
                "width": SOURCE_SIZE[0],
                "height": SOURCE_SIZE[1],
                "fps_custom": False,
                "reroute_audio": False,
                "shutdown": False,
                "restart_when_active": False,
            },
        })
        col, row = i % cols, i // cols
        grid.append(scene_item(camera, i + 1, col * cell_w + (cell_w - SOURCE_SIZE[0] * grid_scale) / 2,
                               row * cell_h + (cell_h - SOURCE_SIZE[1] * grid_scale) / 2, grid_scale))
        scenes.append(scene_source(f"{camera.name} scene", f"00000000-0000-4000-9000-{i + 1:012d}", [
            scene_item(camera, 1, (CANVAS[0] - SOURCE_SIZE[0] * full_scale) / 2,
                       (CANVAS[1] - SOURCE_SIZE[1] * full_scale) / 2, full_scale)]))
    scenes.append(scene_source(ALL_SCENE, "00000000-0000-4000-9000-ffffffffffff", grid))
    preview, program, _ = scene_names(cameras)
    return {
        "name": COLLECTION,
        # In studio mode OBS's current scene is the preview's
        "current_scene": preview,
        "current_program_scene": program,
        "scene_order": [{"name": scene["name"]} for scene in scenes],
        "sources": sources + scenes,
    }


def write_profile(home, cameras, plugin, ws_port):
    root = config_root(home)
    root.mkdir(parents=True, exist_ok=True)
    install_plugin(home, plugin)
    # The plugin's strings are en-GB's, so OBS must not be in a language with
    # a file of its own. FirstRun stops the wizard, a dialog nothing here can answer
    # Studio mode, with a camera's scene in the preview and another in the program
    window = "[BasicWindow]\nPreviewProgramMode=true\n\n"
    websocket = ("[OBSWebSocket]\nFirstLoad=false\nServerEnabled=true\nAlertsEnabled=false\n"
                 f"AuthRequired=false\nServerPort={ws_port}\n\n")
    basic = f"[Basic]\nSceneCollection={COLLECTION}\nSceneCollectionFile={COLLECTION}\n"
    # user.ini is OBS 31 and later's user config, global.ini an older one's
    (root / "user.ini").write_text("[General]\nFirstRun=true\nLanguage=en-GB\n\n" + window + basic)
    (root / "global.ini").write_text(("[General]\nMacOSPermissionsDialogLastShown=1000\n\n" if DARWIN else "") + window + websocket + basic)
    scenes = root / "basic" / "scenes"
    scenes.mkdir(parents=True, exist_ok=True)
    (scenes / f"{COLLECTION}.json").write_text(json.dumps(scene_collection(cameras), indent=2))
    config = root / "plugin_config" / "obs-ptz"
    config.mkdir(parents=True, exist_ok=True)
    (config / "config.json").write_text(json.dumps({"devices": [c.device() for c in cameras]}, indent=2))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("-n", "--cameras", type=int, default=3, help="how many cameras (default 3)")
    ap.add_argument("-p", "--protocols", default="visca-tcp,visca-udp,onvif",
                    help="the protocols to give the cameras in turn, comma separated (default: "
                         "visca-tcp,visca-udp,onvif); one of " + ", ".join(PROTOCOLS))
    ap.add_argument("--move-time", type=float, default=0.0, metavar="SECONDS",
                    help="make absolute moves take time, as a camera's motors do (see ptzsim)")
    ap.add_argument("--room", default=None, metavar="NAME",
                    help="put the cameras in this ptzsim room, one at each of its cameras in turn "
                         "(see python3 scripts/ptzsim --list-rooms)")
    ap.add_argument("--home", default=None, metavar="DIR",
                    help="keep OBS's profile here, and start from it again next time, instead of "
                         "a temporary one that is removed at the end")
    args = ap.parse_args()

    protocols = [p.strip() for p in args.protocols.split(",") if p.strip()]
    for p in protocols:
        if p not in PROTOCOLS:
            ap.error(f"unknown protocol '{p}': choose from {', '.join(PROTOCOLS)}")
    if not protocols or args.cameras < 1:
        ap.error("need at least one camera and one protocol")
    args.room_cameras = []
    if args.room:
        sys.path.insert(0, str(SCRIPTS))
        from ptzsim import rooms  # noqa: E402
        if args.room not in rooms.ROOMS:
            ap.error(f"no room '{args.room}': {', '.join(rooms.ROOMS)}")
        args.room_cameras = list(rooms.ROOMS[args.room]["cameras"])

    obs = find_obs()
    if not obs.exists() and not shutil.which(str(obs)):
        sys.exit(f"OBS not found at {obs}: set PTZSIM_OBS_BINARY")
    plugin = find_plugin()
    if WINDOWS:
        running = "obs64.exe" in subprocess.run(["tasklist", "/FI", "IMAGENAME eq obs64.exe"], capture_output=True,
                                                text=True).stdout
    else:
        running = subprocess.run(["pgrep", "-x", "OBS" if DARWIN else "obs"], capture_output=True).returncode == 0
    if running:
        sys.exit("OBS is already running: quit it first, or its start-up asks whether to launch another")

    for p in protocols:
        if WINDOWS and p.endswith(("serial", "pelco-d", "pelco-p")):
            ap.error(f"'{p}' needs a serial port, which Windows has no emulation of")
    temporary = args.home is None
    aside = None
    if WINDOWS:
        # OBS keeps its profile in the folder it is in, with portable_mode.txt there. Put
        # the one that is there aside for the run
        home = obs.resolve().parents[2]
        (home / "portable_mode.txt").touch()
        if (home / "config").exists():
            aside = home / f"config.before-ptz-demo-{os.getpid()}"
            (home / "config").rename(aside)
        temporary = False
    else:
        home = Path(tempfile.mkdtemp(prefix="ptz-demo-")) if temporary else Path(args.home).expanduser().resolve()
    work = Path(tempfile.mkdtemp(prefix="ptz-demo-sim-"))
    if not temporary:
        home.mkdir(parents=True, exist_ok=True)
        # Start the profile again: the cameras' ports are new each time
        shutil.rmtree(config_root(home), ignore_errors=True)

    cameras = [Camera(i, protocols[i % len(protocols)], work, args) for i in range(args.cameras)]
    obs_proc = None
    # A kill from another terminal quits and cleans up as Ctrl-C does, instead of leaving OBS,
    # the cameras and the temporary home behind
    signal.signal(signal.SIGTERM, signal.default_int_handler)
    try:
        for camera in cameras:
            camera.start(work)
            print(f"==> {camera.name}: {camera.protocol}, view http://127.0.0.1:{camera.ports['web']}/", flush=True)
        ws_port = free_port()
        write_profile(home, cameras, plugin, ws_port)

        env = dict(os.environ)
        env["HOME"] = str(home)
        if DARWIN:
            env["CFFIXED_USER_HOME"] = str(home)
        env["OBS_WEBSOCKET_SERVER_ENABLE"] = "true"
        env["OBS_WEBSOCKET_SERVER_PORT"] = str(ws_port)
        print(f"==> obs-websocket on ws://127.0.0.1:{ws_port}, with no password", flush=True)
        print(f"==> Running {obs} with its profile in {home}", flush=True)
        print("    Quit OBS or press Ctrl-C to stop; the cameras' logs are in " + str(work))
        # Its own process group, so that a Ctrl-C reaches it only through us,
        # as a SIGINT, which OBS takes as a normal quit
        # OBS on Windows finds its data folder from where it is run
        obs_proc = subprocess.Popen([str(obs), "--disable-updater", "--verbose"], env=env,
                                    cwd=obs.resolve().parent if WINDOWS else home,
                                    **({} if WINDOWS else {"start_new_session": True}))
        try:
            obs_proc.wait()
        except KeyboardInterrupt:
            print("\n==> Asking OBS to quit")
            if WINDOWS:  # closes its window, which is a normal quit
                subprocess.run(["taskkill", "/PID", str(obs_proc.pid)], capture_output=True)
            else:
                obs_proc.send_signal(signal.SIGINT)
            try:
                obs_proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                print("==> OBS did not quit: killing it")
                obs_proc.kill()
    finally:
        if obs_proc is not None and obs_proc.poll() is None:
            obs_proc.kill()
        for camera in cameras:
            camera.stop()
        shutil.rmtree(work, ignore_errors=True)
        if temporary:
            shutil.rmtree(home, ignore_errors=True)
        if aside is not None:
            # The demo's profile goes with its log: keep that where it can be read
            kept = Path(tempfile.gettempdir()) / "ptz-demo-obs-logs"
            for name in ("logs", "crashes"):
                if (config_root(home) / name).is_dir():
                    shutil.copytree(config_root(home) / name, kept / name, dirs_exist_ok=True)
            print(f"==> OBS's logs are in {kept}", flush=True)
            shutil.rmtree(home / "config", ignore_errors=True)
            aside.rename(home / "config")


if __name__ == "__main__":
    main()
