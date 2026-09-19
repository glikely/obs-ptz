"""Pelco-D / Pelco-P protocol driver over an emulated serial port.

obs-ptz's real Pelco driver (src/ptz-pelco.cpp) only ever runs over
UART, so this backend does too. Framing auto-detects the variant from
the leading sync byte, so one link speaks both:

- Pelco-D: FF, ADDR, D1, D2, D3, D4, CHECKSUM (7 bytes).
  Checksum is sum(ADDR..D4) % 100, matching PTZPelco::checkSum() exactly
  (not the usual mod-256 Pelco-D checksum -- this driver's own quirk).
- Pelco-P: A0, ADDR-1, D1, D2, D3, D4, AF, CHECKSUM (8 bytes).
  Checksum is XOR of every byte before it.

The command payload (D1..D4) is decoded the same way for both variants,
matching PTZPelco::do_update()/pantilt_home()/memory_*() bit-for-bit:
D1 bit0 = focus near, D2 bits 1/2 = pan right/left, D2 bits 3/4 = tilt
up/down, D2 bits 5/6 = zoom tele/wide, D2 bit7 = focus far. Zoom/focus
speed magnitude arrives in a separate "Set Zoom/Focus Speed" command
just before the movement command that sets its direction bit.

There's no reply: this driver only ever sends commands and never waits
for one, so the backend just updates the shared PTZState.
"""

from .base import Backend
from ..serial_port import EmulatedSerialPort

PELCO_D_LEN = 7
PELCO_P_LEN = 8

CMD_SET_ZOOM_SPEED = (0x00, 0x25)
CMD_SET_FOCUS_SPEED = (0x00, 0x27)
CMD_SET_PRESET = (0x00, 0x03)
CMD_CLEAR_PRESET = (0x00, 0x05)
CMD_GOTO_PRESET = (0x00, 0x07)
HOME_PRESET_BYTE = 0x2b  # matches HOME = QByteArray::fromHex("0007002B")


class PelcoFramer:
    """Resyncs on 0xff (Pelco-D) or 0xa0 (Pelco-P) and emits fixed-length
    frames; any other leading byte is dropped and resync retried."""

    def __init__(self, on_frame):
        self.on_frame = on_frame
        self._buf = bytearray()

    def feed(self, data):
        self._buf.extend(data)
        while self._buf:
            lead = self._buf[0]
            if lead == 0xff:
                need = PELCO_D_LEN
            elif lead == 0xa0:
                need = PELCO_P_LEN
            else:
                del self._buf[0]
                continue
            if len(self._buf) < need:
                break
            frame = bytes(self._buf[:need])
            del self._buf[:need]
            self.on_frame(frame)


class PelcoCameraLogic:
    def __init__(self, state, address=1):
        self.state = state
        self.address = address
        self._zoom_speed_mag = 0.0
        self._focus_speed_mag = 0.0

    def handle_frame(self, frame):
        if frame[0] == 0xff and len(frame) == PELCO_D_LEN:
            self._handle_d(frame)
        elif frame[0] == 0xa0 and len(frame) == PELCO_P_LEN and frame[6] == 0xaf:
            self._handle_p(frame)
        else:
            print(f"[pelco] unrecognized frame {frame.hex()}")

    def _handle_d(self, frame):
        addr = frame[1]
        body = frame[1:6]
        checksum = sum(body) % 100 & 0xff
        if frame[6] != checksum:
            print(f"[pelco-d] checksum mismatch {frame.hex()}")
        self._dispatch(addr, frame[2:6])

    def _handle_p(self, frame):
        addr = frame[1] + 1
        body = frame[0:7]
        checksum = 0
        for b in body:
            checksum ^= b
        if frame[7] != checksum:
            print(f"[pelco-p] checksum mismatch {frame.hex()}")
        self._dispatch(addr, frame[2:6])

    def _dispatch(self, addr, data):
        if addr != self.address:
            return
        d0, d1, d2, d3 = data
        if (d0, d1) == CMD_SET_ZOOM_SPEED:
            self._zoom_speed_mag = d3 / 0x33
        elif (d0, d1) == CMD_SET_FOCUS_SPEED:
            self._focus_speed_mag = d3 / 0x33
        elif (d0, d1) == CMD_CLEAR_PRESET:
            self.state.remove_preset(str(d3 - 1))
        elif (d0, d1) == CMD_SET_PRESET:
            self.state.set_preset(str(d3 - 1), None)
        elif (d0, d1) == CMD_GOTO_PRESET:
            if d3 == HOME_PRESET_BYTE:
                self.state.goto_home()
            else:
                self.state.goto_preset(str(d3 - 1))
        else:
            pan = d2 / 0x3f if d1 & 0x02 else (-(d2 / 0x3f) if d1 & 0x04 else 0.0)
            tilt = d3 / 0x3f if d1 & 0x08 else (-(d3 / 0x3f) if d1 & 0x10 else 0.0)
            zoom = self._zoom_speed_mag if d1 & 0x20 else (-self._zoom_speed_mag if d1 & 0x40 else 0.0)
            focus = self._focus_speed_mag if d0 & 0x01 else (-self._focus_speed_mag if d1 & 0x80 else 0.0)
            self.state.set_pt_speed(pan, tilt)
            self.state.set_zoom_speed(zoom)
            self.state.set_focus_speed(focus)


class PelcoBackend(Backend):
    def __init__(self, state, serial_path, address=1):
        self.state = state
        self.serial_path = serial_path
        self.address = address
        self.logic = PelcoCameraLogic(state, address)
        self.framer = PelcoFramer(self.logic.handle_frame)
        self.port = None
        self._loop = None

    def start(self, loop=None):
        self._loop = loop
        self.port = EmulatedSerialPort(self.serial_path)
        self.port.register(loop, self.framer.feed)
        print(f'[pelco] emulated serial port at {self.port.path} '
              f'(address {self.address}, auto-detects Pelco-D/P)')

    def stop(self):
        if self.port:
            self.port.close(self._loop)
