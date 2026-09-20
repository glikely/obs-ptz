"""VISCA protocol driver: TCP, UDP ("VISCA-over-IP"), and emulated serial.

All three transports share one ViscaCameraLogic instance's worth of wire
protocol per link; only framing differs:

- TCP and serial carry raw VISCA datagrams terminated by 0xff, exactly as
  on the wire for ptz-visca-tcp.cpp / ptz-visca-uart.cpp.
- UDP wraps each datagram in Sony's 8-byte VISCA-over-IP header (type,
  length, sequence number), matching ptz-visca-udp.cpp.

Pan, tilt, zoom and focus are read/written on a shared PTZState instead
of being owned here. Fields VISCA alone cares about (white balance,
exposure, gain, ...) stay local to each logic instance since they're
purely cosmetic for inquiry responses and aren't part of the shared PTZ
model.
"""

import asyncio

from .base import Backend
from ..serial_port import DatagramFramer, EmulatedSerialPort

# Position/speed ranges taken from the original VISCA emulator.
PT_POS_RANGE = 0xe500      # pan/tilt position, signed
ZF_POS_RANGE = 0xe500      # zoom/focus position, unsigned
PT_SPEED_RANGE = 0x18      # typical max pan/tilt speed
ZF_SPEED_RANGE = 0x08      # typical max zoom/focus variable speed


def to_shared_signed(value, rng):
    return max(min(value / rng, 1.0), -1.0)


def to_shared_unsigned(value, rng):
    return max(min(value / rng, 1.0), 0.0)


def from_shared_signed(value, rng):
    return int(round(value * rng))


def from_shared_unsigned(value, rng):
    return int(round(value * rng))


def sign_extend(val, bits):
    sign_bit = 1 << (bits - 1)
    return (val & (sign_bit - 1)) - (val & sign_bit)


class ViscaCameraLogic:
    """Transport-agnostic VISCA command decoder/encoder for one link.

    Call handle_datagram() with one received VISCA datagram (without its
    trailing 0xff); it returns a list of reply datagrams to send back
    (each already including its trailing 0xff).
    """

    def __init__(self, state):
        self.state = state
        self._out = []
        # VISCA-only cosmetic inquiry fields; not part of the shared model.
        self.zoomnearlimit = 0
        self.rgain = 0
        self.bgain = 0
        self.wbmode = 0
        self.aperturegain = 0
        self.exposuremode = 0
        self.shutterpos = 0
        self.irispos = 0
        self.gainpos = 0
        self.brightpos = 0
        self.exposurecomppos = 0
        self.pictureeffectmode = 0
        self.camera_id = 0xfedc
        self.palsystem = True
        self.gamma = 0
        self.high_sensitivity = False
        self.nr_level = 0
        self.chroma_suppress = 0
        self.gain_limit = 0
        self.digitalzoompos = 0
        self.af_activation_time = 5
        self.af_interval_time = 7
        self.defog_mode = False
        self.color_hue = 9

    def print_state(self, data1, data2):
        snap = self.state.snapshot()
        print(f'[{snap.pan:+.2f}{snap.pan_speed:+.2f}, {snap.tilt:+.2f}{snap.tilt_speed:+.2f}, '
              f'{snap.zoom:.2f}{snap.zoom_speed:+.2f}, {snap.focus:.2f}{snap.focus_speed:+.2f}]',
              data1, data2)

    def hello(self):
        """Greeting some transports send when a new link comes up."""
        self._out = []
        self.send_broadcast(b'\x38')
        return self._out

    def handle_datagram(self, dg):
        self._out = []
        self.receive_datagram(dg)
        return self._out

    def send_datagram(self, dg):
        self._send_datagram(b'\x90%b\xff' % dg)

    def send_broadcast(self, dg):
        self._send_datagram(b'\x88%b\xff' % dg)

    def _send_datagram(self, dg):
        self._out.append(dg)
        self.print_state('<--', dg.hex())

    # VISCA protocol encode/decode helpers
    def decode_s4(self, val):
        '''VISCA 4 bit signed value [SM | 0S]
        M: magnitude, 3 bit, range 0x0-0x7
        S: sign, 2 bits, 2 = negative, 3 = positive, 0 = stop
        if M is omitied, then by default use the fastest magnitude
        Returned magnitude is incremented by 1 so it can be differentiated from 0
        '''
        if val == 0:
            return 0
        m = (val & 0x7) + 1
        s = val & 0x30
        if not s:
            s = val & 0x3 << 4
            m = 0x8
        if s == 0x20:
            return m
        if s == 0x30:
            return -m
        raise ValueError

    def decode_s9(self, field):
        '''VISCA 9 bit signed value [MM ** 0D]
        MM: magnitude, 8 bit
        S: sign, 1 bit, 1 = negative, 2 = positive, 3 = 0
        '''
        if len(field) != 3 or field[2] < 1 or field[2] > 3:
            raise ValueError
        s = field[2]
        return field[0] * (((s >> 1) & 0x1) - (s & 0x1))

    def decode_s16(self, field):
        '''VISCA 16 bit signed value [0Y 0Y 0Y 0Y]
        YYYY: 0x7fff..0x8000
        '''
        if (len(field) != 4):
            raise ValueError
        val = 0
        for i in range(0, 4):
            if field[i] & 0xf0 != 0:
                raise ValueError
            val = (val << 4) | (field[i] & 0x0f)
        return sign_extend(val, 16)

    def encode_bool(self, value):
        if value:
            return b'\x02'
        else:
            return b'\x03'

    def encode_s8(self, value):
        value = int(value)
        return bytes([(value >> 4) & 0xf, value & 0xf])

    def encode_s16(self, value):
        value = int(value)
        return bytes([(value >> 12) & 0xf, (value >> 8) & 0xf,
                      (value >> 4) & 0xf, value & 0xf])

    def cmd_ack(self):
        self.send_datagram(b'\x41')
        self.send_datagram(b'\x51')

    def cmd_error(self):
        self.send_datagram(b'\x60\x01')

    # VISCA Protocol command handlers
    # Handlers are named 'cmdXXXXXX', where XXXXXX is one or more hex values
    # matching the protocol command starting at the second byte. For example,
    # the CAM_Power command format is "8x 01 04 00 0y FF", and it's handler is
    # named "cmd010400".
    #
    # Multiple handlers may match the same received command. When this happens,
    # the handler specifying the most number of bytes will be called. i.e., if
    # handlers "cmd010400" and "cmd01" are both defined, and "81 01 04 00 02 FF"
    # is received, then both functions match the command, but only cmd010400()
    # will get called because it is the most specific handler.

    def cmd(self, dg):
        print("[visca] no command handler for", dg.hex())
        self.send_datagram(b'\x60\x02')

    def cmd010001(self, dg):
        '''IF_Clear'''
        self.send_datagram(b'\x50')  # slot 0 response only

    def cmd010407(self, dg):
        '''CAM_Zoom-Move'''
        speed = self.decode_s4(dg[4])
        self.state.set_zoom_speed(to_shared_signed(speed, ZF_SPEED_RANGE))
        self.cmd_ack()

    def cmd010408(self, dg):
        '''CAM_Focus-Move'''
        speed = -self.decode_s4(dg[4])
        self.state.set_focus_speed(to_shared_signed(speed, ZF_SPEED_RANGE))
        self.cmd_ack()

    def cmd010601(self, dg):
        '''Pan-tiltDrive-Move'''
        panspeed = self.decode_s9(dg[4:7])
        tiltspeed = -self.decode_s9(dg[5:8])
        self.state.set_pt_speed(to_shared_signed(panspeed, PT_SPEED_RANGE),
                                 to_shared_signed(tiltspeed, PT_SPEED_RANGE))
        self.cmd_ack()

    def cmd010602(self, dg):
        '''Pan-tiltDrive-AbsolutePosition'''
        pan = self.decode_s16(dg[6:10])
        tilt = self.decode_s16(dg[10:14])
        self.state.set_position(pan=to_shared_signed(pan, PT_POS_RANGE),
                                 tilt=to_shared_signed(tilt, PT_POS_RANGE))
        self.cmd_ack()

    def cmd010603(self, dg):
        '''Pan-tiltDrive-RelativePosition'''
        dpan = self.decode_s16(dg[6:10])
        dtilt = self.decode_s16(dg[10:14])
        self.state.move_relative(dpan=to_shared_signed(dpan, PT_POS_RANGE),
                                  dtilt=to_shared_signed(dtilt, PT_POS_RANGE))
        self.cmd_ack()

    def cmd010604(self, dg):
        '''Pan-tiltDrive-Home'''
        self.state.set_position(pan=0.0, tilt=0.0)
        self.state.stop(pan_tilt=True, zoom=False)

    def cmd010605(self, dg):
        '''Pan-tiltDrive-Reset'''
        self.state.set_position(pan=0.0, tilt=0.0)
        self.state.stop(pan_tilt=True, zoom=False)

    def cmd01043f00(self, dg):
        '''CAM_Memory Reset'''
        self.state.remove_preset(str(dg[5] & 0x7f))
        self.cmd_ack()

    def cmd01043f01(self, dg):
        '''CAM_Memory Set'''
        self.state.set_preset(str(dg[5] & 0x7f), None)
        self.cmd_ack()

    def cmd01043f02(self, dg):
        '''CAM_Memory Recall'''
        self.state.goto_preset(str(dg[5] & 0x7f))
        self.cmd_ack()

    def cmd090002(self, dg):
        '''CAM_VersionInq'''
        self.send_datagram(b'\x50\x00\x01\x05\x11\x00\x00\x02')

    def cmd090400(self, dg):
        '''CAM_PowerInq'''
        self.send_datagram(b'\x50' + self.encode_bool(self.state.snapshot().power))

    def cmd090612(self, dg):
        '''Pan-tiltPosInq'''
        snap = self.state.snapshot()
        self.send_datagram(
            b'\x50' + self.encode_s16(from_shared_signed(snap.pan, PT_POS_RANGE)) +
            self.encode_s16(from_shared_signed(snap.tilt, PT_POS_RANGE)))

    def cmd097e7e00(self, dg):
        '''Lens Control System Inquiry'''
        snap = self.state.snapshot()
        self.send_datagram(
            b'\x50' + self.encode_s16(from_shared_unsigned(snap.zoom, ZF_POS_RANGE)) +
            self.encode_s8(self.zoomnearlimit) +
            self.encode_s16(from_shared_unsigned(snap.focus, ZF_POS_RANGE)) +
            b'\x00\x00\x00')

    def cmd097e7e01(self, dg):
        '''Camera Control System Inquiry'''
        self.send_datagram(b'\x50' + self.encode_s8(self.rgain) +
                            self.encode_s8(self.bgain) +
                            bytes([self.wbmode & 0x0f,
                                   self.aperturegain & 0x0f,
                                   self.exposuremode & 0x1f,
                                   0,
                                   self.shutterpos & 0x1f,
                                   self.irispos & 0x1f,
                                   self.gainpos & 0x0f,
                                   self.brightpos & 0x1f,
                                   self.exposurecomppos & 0x0f]))

    def cmd097e7e02(self, dg):
        '''Other Inquiry'''
        snap = self.state.snapshot()
        self.send_datagram(bytes([0x50, snap.power & 0x1, 0,
                                   self.pictureeffectmode, 0, 0]) +
                            self.encode_s16(self.camera_id) +
                            bytes([0x16 | self.palsystem, 0, 0]))

    def cmd097e7e03(self, dg):
        '''Enlargement Function1 Inquiry'''
        gamma_hs = (((self.gamma & 0x7) << 4) |
                    self.high_sensitivity << 3 |
                    (self.nr_level & 0x7))
        chroma_gl = (((self.chroma_suppress & 0x7) << 4) |
                     (self.gain_limit & 0xf))
        self.send_datagram(b'\x50' + self.encode_s8(self.digitalzoompos) +
                            self.encode_s8(self.af_activation_time) +
                            self.encode_s8(self.af_interval_time) +
                            bytes([0x08, 0x08, 0, gamma_hs, 1, 1, chroma_gl]))

    def cmd097e7e04(self, dg):
        '''Enlargement Function2 Inquiry'''
        self.send_datagram(bytes([0x50, self.color_hue & 0xf,
                                   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]))

    def cmd097e7e05(self, dg):
        '''Enlargement Function3 Inquiry'''
        self.send_datagram(b'\x50\x00\x00\x00\x00\x00' + bytes([self.defog_mode]) +
                            b'\x00\x00\x00\x00\x00\x00\x00')

    def receive_datagram(self, dg):
        if len(dg) < 2:  # Ignore messages that are too short
            return
        self.print_state("-->", dg.hex())
        if dg[0] == 0x88:  # Broadcast message
            if (dg[1] == 0x30) and len(dg) == 3:
                self.send_broadcast(b'\x30\x02')
                return

        if dg[0] != 0x81:  # Ignore messages not addressed properly
            print("[visca] malformed", dg.hex(), dg[0])
            return

        # Find the command handler for the message
        for count in range(5, 0, -1):
            name = "cmd" + dg[1:count].hex()
            if hasattr(self, name):
                try:
                    getattr(self, name)(dg)
                except Exception:
                    print("[visca] decode error")
                    self.cmd_error()
                    return
                break


# ---------------------------------------------------------------------------
# TCP transport: raw VISCA datagrams terminated by 0xff.
# ---------------------------------------------------------------------------
class ViscaTcpConnection(asyncio.Protocol):
    def __init__(self, state):
        self.logic = ViscaCameraLogic(state)
        self.framer = DatagramFramer(self._on_datagram)

    def connection_made(self, transport):
        self.transport = transport
        print('[visca-tcp] connection from', transport.get_extra_info('peername'))
        for reply in self.logic.hello():
            self.transport.write(reply)

    def data_received(self, data):
        self.framer.feed(data)

    def _on_datagram(self, dg):
        for reply in self.logic.handle_datagram(dg):
            self.transport.write(reply)


class ViscaTcpServer:
    def __init__(self, state, host='', port=5678):
        self.state = state
        self.host = host
        self.port = port
        self._server = None

    async def start(self, loop):
        state = self.state
        self._server = await loop.create_server(lambda: ViscaTcpConnection(state),
                                                  self.host, self.port)
        print(f'[visca-tcp] serving on {self._server.sockets[0].getsockname()}')

    def stop(self):
        if self._server:
            self._server.close()


# ---------------------------------------------------------------------------
# UDP transport: Sony "VISCA-over-IP", an 8-byte header (type, length, a
# 32-bit sequence number) in front of the same raw VISCA datagram used by
# TCP/serial. See src/ptz-visca-udp.cpp for the client side this mirrors.
# ---------------------------------------------------------------------------
VISCA_IP_COMMAND = 0x0100
VISCA_IP_INQUIRY = 0x0110
VISCA_IP_REPLY = 0x0111
VISCA_IP_CONTROL_CMD = 0x0200
VISCA_IP_CONTROL_REPLY = 0x0201
VISCA_IP_RESET = 0x01


class ViscaUdpProtocol(asyncio.DatagramProtocol):
    def __init__(self, state):
        self.logic = ViscaCameraLogic(state)

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        if len(data) < 9:
            return
        ptype = (data[0] << 8) | data[1]
        seq = int.from_bytes(data[4:8], 'big')
        payload = data[8:]
        if ptype in (VISCA_IP_COMMAND, VISCA_IP_INQUIRY):
            dg = payload[:-1] if payload.endswith(b'\xff') else payload
            for reply in self.logic.handle_datagram(dg):
                self._send(addr, VISCA_IP_REPLY, seq, reply)
        elif ptype == VISCA_IP_CONTROL_CMD:
            if payload[:1] == bytes([VISCA_IP_RESET]):
                self._send(addr, VISCA_IP_CONTROL_REPLY, seq, bytes([VISCA_IP_RESET]))
        # Unrecognized control opcodes are silently ignored.

    def _send(self, addr, ptype, seq, payload):
        header = bytes([(ptype >> 8) & 0xff, ptype & 0xff,
                         (len(payload) >> 8) & 0xff, len(payload) & 0xff]) + \
            seq.to_bytes(4, 'big')
        self.transport.sendto(header + payload, addr)


class ViscaUdpServer:
    def __init__(self, state, host='', port=52381):
        self.state = state
        self.host = host
        self.port = port
        self.transport = None

    async def start(self, loop):
        self.transport, _protocol = await loop.create_datagram_endpoint(
            lambda: ViscaUdpProtocol(self.state), local_addr=(self.host or '0.0.0.0', self.port))
        print(f'[visca-udp] serving on {self.transport.get_extra_info("sockname")}')

    def stop(self):
        if self.transport:
            self.transport.close()


# ---------------------------------------------------------------------------
# Emulated serial transport: same raw framing as TCP, carried over a pty.
# ---------------------------------------------------------------------------
class ViscaSerialLink:
    def __init__(self, state, symlink_path):
        self.logic = ViscaCameraLogic(state)
        self.port = EmulatedSerialPort(symlink_path)
        self.framer = DatagramFramer(self._on_datagram)
        self._loop = None

    def start(self, loop):
        self._loop = loop
        self.port.register(loop, self.framer.feed)
        print(f'[visca-serial] emulated serial port at {self.port.path}')
        for reply in self.logic.hello():
            self.port.write(reply)

    def _on_datagram(self, dg):
        for reply in self.logic.handle_datagram(dg):
            self.port.write(reply)

    def stop(self):
        self.port.close(self._loop)


class ViscaBackend(Backend):
    """Umbrella backend: starts whichever VISCA transports are configured.
    Pass a falsy port/path to skip that transport."""

    def __init__(self, state, host='', tcp_port=5678, udp_port=52381, serial_path=None):
        self.state = state
        self.host = host
        self.tcp_port = tcp_port
        self.udp_port = udp_port
        self.serial_path = serial_path
        self._tcp = None
        self._udp = None
        self._serial = None

    async def start(self, loop):
        if self.tcp_port:
            self._tcp = ViscaTcpServer(self.state, self.host, self.tcp_port)
            await self._tcp.start(loop)
        if self.udp_port:
            self._udp = ViscaUdpServer(self.state, self.host, self.udp_port)
            await self._udp.start(loop)
        if self.serial_path:
            self._serial = ViscaSerialLink(self.state, self.serial_path)
            self._serial.start(loop)

    def stop(self):
        if self._tcp:
            self._tcp.stop()
        if self._udp:
            self._udp.stop()
        if self._serial:
            self._serial.stop()
