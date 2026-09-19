"""Shared PTZ camera model used by every protocol backend.

Positions are normalized so backends don't need to agree on wire units:
pan/tilt in [-1, 1], zoom/focus in [0, 1]. Speeds use the same ranges,
where sign indicates direction. A single background ticker (see
run_ticker()) integrates speed into position at a fixed rate, so the
simulated camera keeps moving smoothly regardless of which backends are
attached or how often they poll.
"""

import threading
import time
from dataclasses import dataclass, replace


def clamp(value, lo, hi):
    return max(lo, min(hi, value))


@dataclass
class Position:
    pan: float = 0.0
    tilt: float = 0.0
    zoom: float = 0.0
    focus: float = 0.5


@dataclass
class Preset:
    name: str
    position: Position


@dataclass
class PTZSnapshot:
    pan: float
    tilt: float
    zoom: float
    focus: float
    pan_speed: float
    tilt_speed: float
    zoom_speed: float
    focus_speed: float
    power: bool

    @property
    def pan_tilt_moving(self):
        return abs(self.pan_speed) > 1e-3 or abs(self.tilt_speed) > 1e-3

    @property
    def zoom_moving(self):
        return abs(self.zoom_speed) > 1e-3

    @property
    def focus_moving(self):
        return abs(self.focus_speed) > 1e-3


class PTZState:
    """Thread-safe pan/tilt/zoom/focus model shared by all backends."""

    def __init__(self):
        self._lock = threading.RLock()
        self.pan = 0.0
        self.tilt = 0.0
        self.zoom = 0.0
        self.focus = 0.5
        self.pan_speed = 0.0
        self.tilt_speed = 0.0
        self.zoom_speed = 0.0
        self.focus_speed = 0.0
        self.power = True
        self.home = Position()
        self.presets = {}
        self._next_preset_id = 1

    def snapshot(self):
        with self._lock:
            return PTZSnapshot(self.pan, self.tilt, self.zoom, self.focus,
                                self.pan_speed, self.tilt_speed,
                                self.zoom_speed, self.focus_speed, self.power)

    def set_pt_speed(self, pan_speed, tilt_speed):
        with self._lock:
            self.pan_speed = clamp(pan_speed, -1.0, 1.0)
            self.tilt_speed = clamp(tilt_speed, -1.0, 1.0)

    def set_zoom_speed(self, speed):
        with self._lock:
            self.zoom_speed = clamp(speed, -1.0, 1.0)

    def set_focus_speed(self, speed):
        with self._lock:
            self.focus_speed = clamp(speed, -1.0, 1.0)

    def stop(self, pan_tilt=True, zoom=True, focus=False):
        with self._lock:
            if pan_tilt:
                self.pan_speed = 0.0
                self.tilt_speed = 0.0
            if zoom:
                self.zoom_speed = 0.0
            if focus:
                self.focus_speed = 0.0

    def set_position(self, pan=None, tilt=None, zoom=None, focus=None):
        with self._lock:
            if pan is not None:
                self.pan = clamp(pan, -1.0, 1.0)
            if tilt is not None:
                self.tilt = clamp(tilt, -1.0, 1.0)
            if zoom is not None:
                self.zoom = clamp(zoom, 0.0, 1.0)
            if focus is not None:
                self.focus = clamp(focus, 0.0, 1.0)

    def move_relative(self, dpan=0.0, dtilt=0.0, dzoom=0.0, dfocus=0.0):
        with self._lock:
            self.pan = clamp(self.pan + dpan, -1.0, 1.0)
            self.tilt = clamp(self.tilt + dtilt, -1.0, 1.0)
            self.zoom = clamp(self.zoom + dzoom, 0.0, 1.0)
            self.focus = clamp(self.focus + dfocus, 0.0, 1.0)

    def goto_home(self):
        with self._lock:
            self.pan = self.home.pan
            self.tilt = self.home.tilt
            self.zoom = self.home.zoom
            self.stop(pan_tilt=True, zoom=True)

    def set_home(self):
        with self._lock:
            self.home = Position(self.pan, self.tilt, self.zoom, self.focus)

    def set_preset(self, token, name):
        with self._lock:
            if not token:
                token = f"P{self._next_preset_id}"
                self._next_preset_id += 1
            position = Position(self.pan, self.tilt, self.zoom, self.focus)
            self.presets[token] = Preset(name or token, position)
            return token

    def goto_preset(self, token):
        with self._lock:
            preset = self.presets.get(token)
            if preset is None:
                return False
            self.pan = preset.position.pan
            self.tilt = preset.position.tilt
            self.zoom = preset.position.zoom
            self.focus = preset.position.focus
            return True

    def remove_preset(self, token):
        with self._lock:
            self.presets.pop(token, None)

    def list_presets(self):
        with self._lock:
            return {token: replace(preset) for token, preset in self.presets.items()}

    def step(self, dt, speed_scale=0.2):
        """Integrate speed into position. speed_scale sets how much of the
        full range a speed of 1.0 covers per second."""
        with self._lock:
            self.pan = clamp(self.pan + self.pan_speed * dt * speed_scale, -1.0, 1.0)
            self.tilt = clamp(self.tilt + self.tilt_speed * dt * speed_scale, -1.0, 1.0)
            self.zoom = clamp(self.zoom + self.zoom_speed * dt * speed_scale, 0.0, 1.0)
            self.focus = clamp(self.focus + self.focus_speed * dt * speed_scale, 0.0, 1.0)

    def status_lines(self):
        snap = self.snapshot()
        moving = snap.pan_tilt_moving or snap.zoom_moving or snap.focus_moving
        with self._lock:
            preset_count = len(self.presets)
        return [
            "PTZ Simulator",
            f"Pan  {snap.pan:+.2f}   Tilt {snap.tilt:+.2f}   "
            f"Zoom {snap.zoom:.2f}   Focus {snap.focus:.2f}",
            f"Speed p={snap.pan_speed:+.2f} t={snap.tilt_speed:+.2f} "
            f"z={snap.zoom_speed:+.2f} f={snap.focus_speed:+.2f}  "
            f"[{'MOVING' if moving else 'idle'}]",
            f"Presets: {preset_count}",
        ]


def run_ticker(state, stop_event, hz=20):
    """Continuously integrate state.step() until stop_event is set."""
    interval = 1.0 / hz
    last = time.time()
    while not stop_event.is_set():
        time.sleep(interval)
        now = time.time()
        state.step(now - last)
        last = now
