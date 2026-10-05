"""Runs Blender as ptzsim's renderer: --blender starts it on a .blend (or the
built-in test room), with blender/client.py, which follows the simulated
camera and posts rendered frames to the web view's /frame (see webview.py),
which streams them to the page as MJPEG.
"""

import os
import secrets
import shutil
import subprocess
import sys
import threading

CLIENT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "blender", "client.py")
MAC_APPS = ("/Applications/Blender.app/Contents/MacOS/Blender",)


class BlenderError(Exception):
    pass


def find_blender(explicit=None):
    candidates = [explicit, os.environ.get("BLENDER")] + list(MAC_APPS)
    for candidate in candidates:
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    found = shutil.which("blender")
    if found:
        return found
    raise BlenderError("Blender not found: install it, or give --blender-exe or $BLENDER its path")


class FrameStore:
    """The latest frame the renderer posted, and a way to wait for the next"""

    def __init__(self):
        self.cond = threading.Condition()
        self.jpeg = None
        self.serial = 0
        self.token = secrets.token_urlsafe(16)

    def put(self, jpeg):
        with self.cond:
            self.jpeg = jpeg
            self.serial += 1
            self.cond.notify_all()

    def wait_after(self, serial, timeout=5.0):
        with self.cond:
            self.cond.wait_for(lambda: self.serial != serial, timeout)
            return self.serial, self.jpeg


class BlenderRenderer:
    def __init__(self, scene, url, token, exe=None, camera=None, heading=0.0, engine="eevee",
                 samples=4, size="1280x720", fps=15.0, fstop=2.8, exposure=0.0, raytracing=False,
                 log_path=None):
        self.scene = scene          # a .blend, or None for the built-in room
        self.url, self.token = url, token
        self.exe = find_blender(exe)
        self.camera, self.heading, self.engine = camera, heading, engine
        self.samples, self.size, self.fps, self.fstop = samples, size, fps, fstop
        self.exposure, self.raytracing = exposure, raytracing
        self.log_path = log_path or os.path.join(os.path.expanduser("~"), ".cache", "ptzsim", "blender.log")
        self._proc = None

    def start(self):
        cmd = [self.exe, "--background", "--factory-startup", "-noaudio"]
        if self.scene:
            cmd = [self.exe, self.scene, "--background", "-noaudio"]
        cmd += ["--python", CLIENT, "--", "--url", self.url, "--token", self.token,
                "--heading", str(self.heading), "--engine", self.engine, "--samples", str(self.samples),
                "--size", self.size, "--fps", str(self.fps), "--fstop", str(self.fstop)]
        cmd += ["--exposure", str(self.exposure)]
        if self.raytracing:
            cmd += ["--raytracing"]
        if self.camera:
            cmd += ["--camera", self.camera]
        if not self.scene:
            cmd += ["--builtin"]
        os.makedirs(os.path.dirname(self.log_path), exist_ok=True)
        self._log = open(self.log_path, "w")
        self._proc = subprocess.Popen(cmd, stdout=self._log, stderr=subprocess.STDOUT)
        print(f"[blender] rendering with {self.exe} (log: {self.log_path})")

    def poll(self):
        """The exit code if Blender has stopped, else None"""
        return self._proc.poll() if self._proc else None

    def stop(self):
        if self._proc and self._proc.poll() is None:
            self._proc.terminate()
            try:
                self._proc.wait(5)
            except subprocess.TimeoutExpired:
                self._proc.kill()
