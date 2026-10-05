"""Shared PTZ camera model used by every protocol backend.

Positions are normalized so backends don't need to agree on wire units:
pan/tilt in [-1, 1], zoom/focus in [0, 1]. Speeds use the same ranges,
where sign indicates direction. A single background ticker (see
run_ticker()) integrates speed into position at a fixed rate, so the
simulated camera keeps moving smoothly regardless of which backends are
attached or how often they poll.

Absolute moves (set_position, goto_home, goto_preset, move_relative) go
to their destination at move_rate per second, as a real camera's motors
do, instead of jumping there; move_rate 0 makes them instant. Drive
commands (set_*_speed) and stop() cancel a move in progress.
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
        # Tally lamps a protocol can light, by colour
        self.tally = {'red': False, 'green': False}
        # The camera's picture, exposure and system settings, which only
        # VISCA has a way to read or change: see VISCA_SETTINGS in
        # backends/visca.py, which fills it in, for what each is
        self.visca = {}
        # How many VISCA requests were answered with a syntax error: ones
        # the camera doesn't have
        self.visca_syntax_errors = 0
        # The VISCA inquiries received, in order, to see what is asked for
        # first. Only the first INQUIRY_LOG_MAX, as a camera is asked
        # forever.
        self.visca_inquiries = []
        # Where each axis is heading after an absolute move, and how fast
        # it gets there in normalized units per second (0: it jumps)
        self.move_rate = 0.0
        self._targets = {}
        self.home = Position()
        self.presets = {}
        self._next_preset_id = 1

    def snapshot(self):
        with self._lock:
            return PTZSnapshot(self.pan, self.tilt, self.zoom, self.focus,
                                self.pan_speed, self.tilt_speed,
                                self.zoom_speed, self.focus_speed, self.power)

    def _go_to(self, pan=None, tilt=None, zoom=None, focus=None):
        """Send each given axis to its position: now if move_rate is 0,
        else at move_rate per second. Called with the lock held."""
        for axis, value, lo in (("pan", pan, -1.0), ("tilt", tilt, -1.0),
                                ("zoom", zoom, 0.0), ("focus", focus, 0.0)):
            if value is None:
                continue
            value = clamp(value, lo, 1.0)
            if self.move_rate > 0:
                self._targets[axis] = value
            else:
                setattr(self, axis, value)
                self._targets.pop(axis, None)

    def _cancel(self, *axes):
        for axis in axes:
            self._targets.pop(axis, None)

    @property
    def moving_to_target(self):
        with self._lock:
            return bool(self._targets)

    def target_axes(self):
        """The axes still heading for a position, by name"""
        with self._lock:
            return set(self._targets)

    def set_pt_speed(self, pan_speed, tilt_speed):
        with self._lock:
            self._cancel("pan", "tilt")
            self.pan_speed = clamp(pan_speed, -1.0, 1.0)
            self.tilt_speed = clamp(tilt_speed, -1.0, 1.0)

    def set_zoom_speed(self, speed):
        with self._lock:
            self._cancel("zoom")
            self.zoom_speed = clamp(speed, -1.0, 1.0)

    def set_focus_speed(self, speed):
        with self._lock:
            self._cancel("focus")
            self.focus_speed = clamp(speed, -1.0, 1.0)

    def set_power(self, power):
        with self._lock:
            self.power = power

    def stop(self, pan_tilt=True, zoom=True, focus=False):
        with self._lock:
            if pan_tilt:
                self._cancel("pan", "tilt")
                self.pan_speed = 0.0
                self.tilt_speed = 0.0
            if zoom:
                self._cancel("zoom")
                self.zoom_speed = 0.0
            if focus:
                self._cancel("focus")
                self.focus_speed = 0.0

    def set_position(self, pan=None, tilt=None, zoom=None, focus=None):
        with self._lock:
            self._go_to(pan, tilt, zoom, focus)

    def jump_to(self, pan=None, tilt=None, zoom=None):
        """Put the camera at a position now, whatever move_rate is, and
        cancel any move or drive of those axes: for something that holds
        the camera, such as the web view being dragged"""
        with self._lock:
            for axis, value, lo in (("pan", pan, -1.0), ("tilt", tilt, -1.0), ("zoom", zoom, 0.0)):
                if value is not None:
                    self._cancel(axis)
                    setattr(self, axis, clamp(value, lo, 1.0))
                    setattr(self, axis + "_speed", 0.0)

    def move_relative(self, dpan=0.0, dtilt=0.0, dzoom=0.0, dfocus=0.0):
        with self._lock:
            # From where the camera is going, so two in a row add up
            def base(axis):
                return self._targets.get(axis, getattr(self, axis))
            self._go_to(base("pan") + dpan, base("tilt") + dtilt,
                        base("zoom") + dzoom, base("focus") + dfocus)

    def goto_home(self):
        with self._lock:
            self.stop(pan_tilt=True, zoom=True)
            self._go_to(self.home.pan, self.home.tilt, self.home.zoom)

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
            self._go_to(preset.position.pan, preset.position.tilt,
                        preset.position.zoom, preset.position.focus)
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
            # An axis heading for a position ignores its drive speed
            heading = set(self._targets)
            reach = self.move_rate * dt
            for axis in heading:
                target = self._targets[axis]
                here = getattr(self, axis)
                if abs(target - here) <= reach:
                    setattr(self, axis, target)
                    del self._targets[axis]
                else:
                    setattr(self, axis, here + (reach if target > here else -reach))
            for axis, speed, lo in (("pan", self.pan_speed, -1.0), ("tilt", self.tilt_speed, -1.0),
                                    ("zoom", self.zoom_speed, 0.0), ("focus", self.focus_speed, 0.0)):
                if axis not in heading:
                    setattr(self, axis, clamp(getattr(self, axis) + speed * dt * speed_scale, lo, 1.0))


def run_ticker(state, stop_event, hz=50):
    """Continuously integrate state.step() until stop_event is set."""
    interval = 1.0 / hz
    last = time.time()
    while not stop_event.is_set():
        time.sleep(interval)
        now = time.time()
        state.step(now - last)
        last = now
