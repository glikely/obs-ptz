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
PT_POS_RANGE = 0x2800      # pan/tilt position, signed (and in 16 bits, as a camera's is)
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


# The camera's settings, by the state key obs-ptz has for each, and what the
# camera starts with. Kept in PTZState.visca, one set for every link to the
# camera, as a real camera has. The values are VISCA's own, from the Sony
# SRG-120DH manual: a mode by its number, an on/off setting as a bool.
VISCA_SETTINGS = {
    'focus_af_enabled': True,
    'focus_af_mode': 0,              # 0 normal, 1 interval, 2 zoom trigger
    'focus_af_sensitivity': True,    # True normal, False low
    'focus_af_move_time': 5,         # seconds
    'focus_af_interval_time': 7,     # seconds
    'focus_near_limit': 0x1000,      # 0x1000 (far) to 0xe000
    'ir_correction': 0,              # 0 standard, 1 IR light
    'dzoom_on': False,
    'dzoom_pos': 0,
    'low_contrast': False,
    'wb_mode': 0,
    'r_gain': 0x80,
    'b_gain': 0x80,
    'ae_mode': 0,                    # 0 full auto, 3, 0xa, 0xb, 0xd
    'slow_shutter': False,           # True auto
    'shutter_pos': 0x06,
    'iris_pos': 0x0c,
    'gain_pos': 0x01,
    'gain_limit': 0x0f,
    'bright_pos': 0x10,
    'exposure_comp': False,
    'exposure_comp_pos': 0x07,       # 0x07 is 0
    'back_light': False,
    'wd_mode': 0,                    # 0 off to 3 high
    'defog_mode': False,
    'high_sensitivity': False,
    'aperture_gain': 0x05,
    'high_resolution': False,
    'nr_level': 2,                   # 0 off, 1 to 5
    'gamma': 0,                      # 0 standard, 1 off
    'chroma_suppress': 0,            # 0 off, 1 to 3
    'color_gain': 4,                 # 0 (60%) to 0xe (200%), master
    'color_hue': 7,                  # 0 (-14 degrees) to 0xe, master
    'picture_effect': 0,             # 0 off, 2 negative, 4 black and white
    'camera_id': 0xfedc,
    'video_format': 0x08,            # 1080p/50
    'color_system': 0,               # 0 HDMI YUV to 3 DVI YUV
    'low_latency': False,
    'info_display': False,
    'menu_on': False,
    'ir_receive': True,
    'ir_condition': 0,               # 0 stable, 1 unstable, 2 not checked
    'flicker_mode': 0,               # PTZOptics': 0 off, 1 50 Hz, 2 60 Hz
}

# Commands that set one setting from one byte of the command, "8x 01 .. 0p":
# the handler name's bytes, the setting, and the byte its value is in. A
# setting in VISCA_SETTINGS that is a bool is on for 02 and off for 03.
VISCA_BYTE_SETTINGS = {
    '010406': ('dzoom_on', 4),
    '010458': ('focus_af_sensitivity', 4),
    '010457': ('focus_af_mode', 4),
    '010411': ('ir_correction', 4),
    '010439': ('ae_mode', 4),
    '01045a': ('slow_shutter', 4),
    '01042c': ('gain_limit', 4),
    '01043e': ('exposure_comp', 4),
    '010433': ('back_light', 4),
    '010437': ('defog_mode', 4),
    '010452': ('high_resolution', 4),
    '010453': ('nr_level', 4),
    '01045b': ('gamma', 4),
    '01045e': ('high_sensitivity', 4),
    '010463': ('picture_effect', 4),
    '01045f': ('chroma_suppress', 4),
    '017e0400': ('wd_mode', 5),
    '017e015a': ('low_latency', 5),
    '017e0118': ('info_display', 5),
    '017e0103': ('color_system', 6),
    '010423': ('flicker_mode', 4),
}

# Commands that set one setting from two nibbles, "8x 01 04 xx 00 00 0p 0q"
VISCA_DIRECT_SETTINGS = {
    '010443': 'r_gain',
    '010444': 'b_gain',
    '01044a': 'shutter_pos',
    '01044b': 'iris_pos',
    '01044c': 'gain_pos',
    '01044d': 'bright_pos',
    '01044e': 'exposure_comp_pos',
    '010442': 'aperture_gain',
}

# Inquiries answered with one setting as "y0 50 0p": the handler name's
# bytes, and the setting. A bool is 02 on, 03 off.
VISCA_BYTE_INQUIRIES = {
    '090406': 'dzoom_on',
    '090458': 'focus_af_sensitivity',
    '090457': 'focus_af_mode',
    '090411': 'ir_correction',
    '090439': 'ae_mode',
    '09045a': 'slow_shutter',
    '09042c': 'gain_limit',
    '09043e': 'exposure_comp',
    '090433': 'back_light',
    '090452': 'high_resolution',
    '090453': 'nr_level',
    '09045b': 'gamma',
    '09045e': 'high_sensitivity',
    '090463': 'picture_effect',
    '09045f': 'chroma_suppress',
    '097e0400': 'wd_mode',
    '097e015a': 'low_latency',
    '097e0118': 'info_display',
    '097e0103': 'color_system',
    '090623': 'video_format',
    '090606': 'menu_on',
    '090608': 'ir_receive',
    '090634': 'ir_condition',
    '090455': 'flicker_mode',
}

# Inquiries answered with one setting as "y0 50 00 00 0p 0q"
VISCA_DIRECT_INQUIRIES = {
    '090443': 'r_gain',
    '090444': 'b_gain',
    '09044a': 'shutter_pos',
    '09044b': 'iris_pos',
    '09044c': 'gain_pos',
    '09044d': 'bright_pos',
    '09044e': 'exposure_comp_pos',
    '090442': 'aperture_gain',
}


class ViscaCameraLogic:
    """Transport-agnostic VISCA command decoder/encoder for one link.

    Call handle_datagram() with one received VISCA datagram (without its
    trailing 0xff); it returns a list of reply datagrams to send back
    (each already including its trailing 0xff).
    """

    # Whether the "7e 7e xx" block inquiries are understood. A BirdDog
    # answers them all with a syntax error and has only the single-value
    # inquiries; __main__ clears this to imitate one.
    block_inquiries = True
    # Whether the version inquiry is understood, and the vendor and model ID
    # it answers with: a Sony of no model a command set is for, which has
    # everything this does, unless __main__ says otherwise
    version_inquiry = True
    vendor_id = 0x0001
    model_id = 0x0000
    # Whether there is a green tally lamp to command. Sony's own manual has
    # none, and a BirdDog P100 has only the red one, answering the green
    # command with a syntax error; BirdDog's X4 series has both.
    green_tally = True
    # Whether a command that is ACKed is then completed. A BirdDog backpack
    # ACKs and never completes.
    completions = True
    # How many nibbles the pan and tilt positions are, and the position at
    # either end. Some Sony cameras have 5 for pan, or for both.
    position_nibbles = (4, 4)
    pt_pos_range = PT_POS_RANGE

    def __init__(self, state):
        self.state = state
        self._out = []
        # The camera's settings, shared with every other link to it
        self.cam = state.visca
        for key, value in VISCA_SETTINGS.items():
            self.cam.setdefault(key, value)

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
        if dg == b'\x60\x02':
            self.state.visca_syntax_errors += 1
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

    def decode_u8(self, field):
        '''VISCA 8 bit value in two nibbles [0p 0q]'''
        if len(field) != 2 or field[0] & 0xf0 or field[1] & 0xf0:
            raise ValueError
        return (field[0] << 4) | field[1]

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

    def decode_signed(self, field):
        '''VISCA signed value, a nibble a byte'''
        val = 0
        for byte in field:
            if byte & 0xf0:
                raise ValueError
            val = (val << 4) | byte
        return sign_extend(val, 4 * len(field))

    def encode_signed(self, value, nibbles):
        value = int(value)
        return bytes((value >> (4 * i)) & 0xf for i in reversed(range(nibbles)))

    def decode_pantilt(self, dg):
        '''The pan and tilt positions from 6 in a move'''
        p, t = self.position_nibbles
        if len(dg) != 6 + p + t:
            raise ValueError
        return self.decode_signed(dg[6:6 + p]), self.decode_signed(dg[6 + p:6 + p + t])

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
        if self.completions:
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

    def cmd010400(self, dg):
        '''CAM_Power on/off'''
        if dg[4] == 0x02 and not self.state.visca_standby_inquiries:
            self.state.set_power(True)
        elif dg[4] == 0x03:
            self.state.set_power(False)
        self.cmd_ack()

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

    def cmd010447(self, dg):
        '''CAM_Zoom-Direct (absolute)'''
        zoom = self.decode_s16(dg[4:8])
        self.state.set_position(zoom=to_shared_unsigned(zoom, ZF_POS_RANGE))
        self.cmd_ack()

    def cmd010438(self, dg):
        '''CAM_Focus Auto/Manual/AutoManual'''
        mode = dg[4]
        if mode == 0x02:
            self.cam['focus_af_enabled'] = True
        elif mode == 0x03:
            self.cam['focus_af_enabled'] = False
        elif mode == 0x10:
            self.cam['focus_af_enabled'] = not self.cam['focus_af_enabled']
        self.cmd_ack()

    def cmd010435(self, dg):
        '''CAM_WB_Mode (also matches the fixed Auto/Indoor/Outdoor/
        OnePush/AutoTracing/Manual variants, which just spell the mode
        out in the command itself rather than as an argument)'''
        self.cam['wb_mode'] = dg[4] & 0x0f
        self.cmd_ack()

    def cmd010427(self, dg):
        '''CAM_AFMode Active/Interval Time: 8x 01 04 27 0p 0q 0r 0s'''
        self.cam['focus_af_move_time'] = self.decode_u8(dg[4:6])
        self.cam['focus_af_interval_time'] = self.decode_u8(dg[6:8])
        self.cmd_ack()

    def cmd010428(self, dg):
        '''CAM_Focus Near Limit: 8x 01 04 28 0p 0q 0r 0s'''
        self.cam['focus_near_limit'] = self.decode_s16(dg[4:8]) & 0xffff
        self.cmd_ack()

    def cmd010422(self, dg):
        '''CAM_IDWrite: 8x 01 04 22 0p 0q 0r 0s'''
        self.cam['camera_id'] = self.decode_s16(dg[4:8]) & 0xffff
        self.cmd_ack()

    def cmd010449(self, dg):
        '''CAM_ColorGain Direct: 8x 01 04 49 00 00 0p 0q, p the colour (0 for
        all of them, the only one there is an inquiry for), q the gain'''
        if dg[6] == 0:
            self.cam['color_gain'] = dg[7] & 0x0f
        self.cmd_ack()

    def cmd01044f(self, dg):
        '''CAM_ColorHue Direct: 8x 01 04 4f 00 00 0p 0q, as CAM_ColorGain'''
        if dg[6] == 0:
            self.cam['color_hue'] = dg[7] & 0x0f
        self.cmd_ack()

    def cmd010606(self, dg):
        '''SYS_Menu Off: 8x 01 06 06 03. The menu can't be opened over VISCA.'''
        if dg[4] != 0x03:
            self.send_datagram(b'\x60\x02')
            return
        self.cam['menu_on'] = False
        self.cmd_ack()

    def cmd010608(self, dg):
        '''IR_Receive On/Off/Toggle: 8x 01 06 08 0p, p = 2 on, 3 off, 10 toggle'''
        if dg[4] == 0x10:
            self.cam['ir_receive'] = not self.cam['ir_receive']
        else:
            self.cam['ir_receive'] = dg[4] == 0x02
        self.cmd_ack()

    def cmd017e011e(self, dg):
        '''Video Format Change: 8x 01 7e 01 1e 0p 0q'''
        self.cam['video_format'] = self.decode_u8(dg[5:7])
        self.cmd_ack()

    def set_byte(self, dg, key, at):
        '''One of VISCA_BYTE_SETTINGS'''
        if isinstance(VISCA_SETTINGS[key], bool):
            if dg[at] not in (0x02, 0x03):
                raise ValueError
            self.cam[key] = dg[at] == 0x02
        else:
            self.cam[key] = dg[at]
        self.cmd_ack()

    def set_direct(self, dg, key):
        '''One of VISCA_DIRECT_SETTINGS'''
        self.cam[key] = self.decode_u8(dg[6:8])
        self.cmd_ack()

    def reply_byte(self, key):
        '''One of VISCA_BYTE_INQUIRIES'''
        value = self.cam[key]
        self.send_datagram(b'\x50' + (self.encode_bool(value) if isinstance(value, bool) else bytes([value])))

    def reply_direct(self, key):
        '''One of VISCA_DIRECT_INQUIRIES'''
        self.send_datagram(b'\x50\x00\x00' + self.encode_s8(self.cam[key]))

    def cmd010601(self, dg):
        '''Pan-tiltDrive-Move'''
        panspeed = self.decode_s9(dg[4:7])
        tiltspeed = -self.decode_s9(dg[5:8])
        self.state.set_pt_speed(to_shared_signed(panspeed, PT_SPEED_RANGE),
                                 to_shared_signed(tiltspeed, PT_SPEED_RANGE))
        self.cmd_ack()

    def cmd010602(self, dg):
        '''Pan-tiltDrive-AbsolutePosition'''
        pan, tilt = self.decode_pantilt(dg)
        self.state.set_position(pan=to_shared_signed(pan, self.pt_pos_range),
                                 tilt=to_shared_signed(tilt, self.pt_pos_range))
        self.cmd_ack()

    def cmd010603(self, dg):
        '''Pan-tiltDrive-RelativePosition'''
        dpan, dtilt = self.decode_pantilt(dg)
        self.state.move_relative(dpan=to_shared_signed(dpan, self.pt_pos_range),
                                  dtilt=to_shared_signed(dtilt, self.pt_pos_range))
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
        self.send_datagram(b'\x50' + self.vendor_id.to_bytes(2, 'big') + self.model_id.to_bytes(2, 'big') +
                           b'\x00\x00\x02')

    def cmd090400(self, dg):
        '''CAM_PowerInq'''
        self.send_datagram(b'\x50' + self.encode_bool(self.state.snapshot().power))

    def cmd017e010a(self, dg):
        '''Tally lamp, red (program): 8x 01 7e 01 0a 00 0p, p = 2 on, 3 off; or
        a Datavideo's both lamps, red then green: 8x 01 7e 01 0a 00 0p 0q'''
        self.state.tally['red'] = dg[6] == 0x02
        if len(dg) > 7:
            self.state.tally['green'] = dg[7] == 0x02
        self.cmd_ack()

    def cmd017e041a(self, dg):
        '''Tally lamp, green (preview): 8x 01 7e 04 1a 00 0p, p = 2 on, 3 off'''
        if not self.green_tally:
            self.send_datagram(b'\x60\x02')
            return
        self.state.tally['green'] = dg[6] == 0x02
        self.cmd_ack()

    def cmd090447(self, dg):
        '''CAM_ZoomPosInq'''
        self.send_datagram(b'\x50' + self.encode_s16(from_shared_unsigned(self.state.snapshot().zoom, ZF_POS_RANGE)))

    def cmd090448(self, dg):
        '''CAM_FocusPosInq'''
        self.send_datagram(b'\x50' + self.encode_s16(from_shared_unsigned(self.state.snapshot().focus, ZF_POS_RANGE)))

    def cmd090438(self, dg):
        '''CAM_FocusModeInq'''
        self.send_datagram(b'\x50' + self.encode_bool(self.cam['focus_af_enabled']))

    def cmd090435(self, dg):
        '''CAM_WBModeInq'''
        self.send_datagram(b'\x50' + bytes([self.cam['wb_mode']]))

    def cmd090427(self, dg):
        '''CAM_AFTimeSettingInq: y0 50 0p 0q 0r 0s, pq active time, rs interval'''
        self.send_datagram(b'\x50' + self.encode_s8(self.cam['focus_af_move_time']) +
                           self.encode_s8(self.cam['focus_af_interval_time']))

    def cmd090428(self, dg):
        '''CAM_FocusNearLimitInq'''
        self.send_datagram(b'\x50' + self.encode_s16(self.cam['focus_near_limit']))

    def cmd090422(self, dg):
        '''CAM_IDInq'''
        self.send_datagram(b'\x50' + self.encode_s16(self.cam['camera_id']))

    def cmd090449(self, dg):
        '''CAM_ColorGainInq: y0 50 00 00 00 0p'''
        self.send_datagram(b'\x50\x00\x00\x00' + bytes([self.cam['color_gain']]))

    def cmd09044f(self, dg):
        '''CAM_ColorHueInq: y0 50 00 00 00 0p'''
        self.send_datagram(b'\x50\x00\x00\x00' + bytes([self.cam['color_hue']]))

    def cmd090437(self, dg):
        '''CAM_DefogInq: y0 50 0p 00'''
        self.send_datagram(b'\x50' + self.encode_bool(self.cam['defog_mode']) + b'\x00')

    def cmd090611(self, dg):
        '''Pan-tiltMaxSpeedInq: y0 50 ww zz'''
        self.send_datagram(bytes([0x50, PT_SPEED_RANGE, 0x14]))

    def cmd090610(self, dg):
        '''Pan-tiltModeInq: y0 50 pq rs, the manual's "Pan/Tilt Status Code
        List": initialized, whether it is moving, and which ends it is at'''
        snap = self.state.snapshot()
        moving = 1 if snap.pan_tilt_moving else 0
        ends = ((snap.pan <= -1.0) | (snap.pan >= 1.0) << 1 |
                (snap.tilt >= 1.0) << 2 | (snap.tilt <= -1.0) << 3)
        self.send_datagram(bytes([0x50, 0x20 | moving << 2, ends]))

    def cmd090612(self, dg):
        '''Pan-tiltPosInq'''
        snap = self.state.snapshot()
        p, t = self.position_nibbles
        self.send_datagram(
            b'\x50' + self.encode_signed(from_shared_signed(snap.pan, self.pt_pos_range), p) +
            self.encode_signed(from_shared_signed(snap.tilt, self.pt_pos_range), t))

    # BirdDog's own block inquiries, which only a BirdDog has, as Bitfocus'
    # BirdDog PTZ Companion module reads them: the inquiry's own number back,
    # then the values
    def cmd097e7e15(self, dg):
        '''BirdDog camera details: autofocus at 12, power at 13, freeze at 14'''
        if self.vendor_id != 0x0109:
            self.send_datagram(b'\x60\x02')
            return
        self.send_datagram(b'\x50\x15' + bytes(9) + self.encode_bool(self.cam['focus_af_enabled']) +
                           self.encode_bool(self.state.snapshot().power) + b'\x03')

    def cmd097e7e17(self, dg):
        '''BirdDog pan, tilt and zoom, a nibble a byte from 3'''
        if self.vendor_id != 0x0109:
            self.send_datagram(b'\x60\x02')
            return
        snap = self.state.snapshot()
        self.send_datagram(
            b'\x50\x17' + self.encode_s16(from_shared_signed(snap.pan, PT_POS_RANGE)) +
            self.encode_s16(from_shared_signed(snap.tilt, PT_POS_RANGE)) +
            self.encode_s16(from_shared_unsigned(snap.zoom, ZF_POS_RANGE)) + bytes(4))

    # The block inquiries. Each reply is 16 bytes, "y0 50", 13 bytes of
    # settings, and ff, laid out as the manual's "Block Inquiry Command List"
    # has it; the comments number the bytes from y0 as the manual does.

    def cmd097e7e00(self, dg):
        '''Lens Control System Inquiry'''
        snap = self.state.snapshot()
        cam = self.cam
        self.send_datagram(
            b'\x50' +
            # 2-5 zoom position, 6-7 the top byte of the focus near limit,
            # 8-11 focus position
            self.encode_s16(from_shared_unsigned(snap.zoom, ZF_POS_RANGE)) +
            self.encode_s8(cam['focus_near_limit'] >> 8) +
            self.encode_s16(from_shared_unsigned(snap.focus, ZF_POS_RANGE)) +
            bytes([0,
                   # 13: AF mode, AF sensitivity, digital zoom, focus mode
                   cam['focus_af_mode'] << 3 | cam['focus_af_sensitivity'] << 2 |
                   cam['dzoom_on'] << 1 | cam['focus_af_enabled'],
                   # 14: low contrast, and memory recall, focus and zoom
                   # commands running
                   cam['low_contrast'] << 3 | snap.zoom_moving | snap.focus_moving << 1]))

    def cmd097e7e01(self, dg):
        '''Camera Control System Inquiry'''
        cam = self.cam
        self.send_datagram(
            b'\x50' +
            # 2-3 R gain, 4-5 B gain
            self.encode_s8(cam['r_gain']) + self.encode_s8(cam['b_gain']) +
            bytes([cam['wb_mode'],                   # 6
                   cam['aperture_gain'],             # 7
                   cam['ae_mode'],                   # 8
                   # 9: high resolution, wide dynamic range (on in any
                   # mode but off), backlight, exposure comp, slow shutter
                   cam['high_resolution'] << 5 | (cam['wd_mode'] != 0) << 4 |
                   cam['back_light'] << 2 | cam['exposure_comp'] << 1 | cam['slow_shutter'],
                   cam['shutter_pos'],               # 10
                   cam['iris_pos'],                  # 11
                   cam['gain_pos'],                  # 12
                   cam['bright_pos'],                # 13
                   cam['exposure_comp_pos']]))       # 14

    def cmd097e7e02(self, dg):
        '''Other Inquiry'''
        snap = self.state.snapshot()
        cam = self.cam
        self.send_datagram(
            bytes([0x50, int(snap.power),            # 2: power
                   0, 0,
                   cam['picture_effect'],            # 5
                   0, 0]) +
            self.encode_s16(cam['camera_id']) +      # 8-11
            bytes([int(cam['video_format'] >= 0x08),  # 12: 50 Hz
                   0, 0]))

    def cmd097e7e03(self, dg):
        '''Enlargement Function1 Inquiry'''
        cam = self.cam
        self.send_datagram(
            b'\x50' +
            # 2-3 digital zoom position, 4-5 AF active time, 6-7 AF interval
            self.encode_s8(cam['dzoom_pos']) +
            self.encode_s8(cam['focus_af_move_time']) +
            self.encode_s8(cam['focus_af_interval_time']) +
            bytes([0, 0, 0,
                   cam['color_gain'] << 3,           # 11
                   0,
                   # 13: gamma, high sensitivity, noise reduction
                   cam['gamma'] << 4 | cam['high_sensitivity'] << 3 | cam['nr_level'],
                   # 14: chroma suppress, gain limit
                   cam['chroma_suppress'] << 4 | cam['gain_limit']]))

    def cmd097e7e04(self, dg):
        '''Enlargement Function2 Inquiry'''
        self.send_datagram(bytes([0x50, 0, 0, 0, 0, 0,
                                   int(self.cam['defog_mode']),  # 7
                                   0, 0, 0, 0, 0, 0, 0]))

    def cmd097e7e05(self, dg):
        '''Enlargement Function3 Inquiry'''
        self.send_datagram(bytes([0x50, self.cam['color_hue'],  # 2
                                   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]))

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

        birddog_block = dg[1:4] == b'\x09\x7e\x7e' and dg[4] in (0x15, 0x17)
        if ((not self.block_inquiries and dg[1:4] == b'\x09\x7e\x7e' and not birddog_block) or
                (not self.version_inquiry and dg[1:4] == b'\x09\x00\x02')):
            self.send_datagram(b'\x60\x02')
            return

        # A BirdDog in standby that is asked for anything but its camera
        # details (or what it is) won't wake, Bitfocus' BirdDog PTZ Companion
        # module says
        if (self.vendor_id == 0x0109 and dg[1] == 0x09 and not self.state.snapshot().power and
                dg[2:5] != b'\x7e\x7e\x15' and dg[2:4] != b'\x00\x02'):
            self.state.visca_standby_inquiries += 1

        # Find the command handler for the message: a method, or a setting
        # the tables above say how to set or read
        for count in range(5, 0, -1):
            key = dg[1:count].hex()
            if hasattr(self, "cmd" + key):
                handler = getattr(self, "cmd" + key)
            elif key in VISCA_BYTE_SETTINGS:
                handler = lambda dg, key=key: self.set_byte(dg, *VISCA_BYTE_SETTINGS[key])  # noqa: E731
            elif key in VISCA_DIRECT_SETTINGS:
                handler = lambda dg, key=key: self.set_direct(dg, VISCA_DIRECT_SETTINGS[key])  # noqa: E731
            elif key in VISCA_BYTE_INQUIRIES and len(dg) == count:
                handler = lambda dg, key=key: self.reply_byte(VISCA_BYTE_INQUIRIES[key])  # noqa: E731
            elif key in VISCA_DIRECT_INQUIRIES and len(dg) == count:
                handler = lambda dg, key=key: self.reply_direct(VISCA_DIRECT_INQUIRIES[key])  # noqa: E731
            else:
                continue
            try:
                handler(dg)
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


class SonyUdpQuirks:
    """Makes the UDP server behave the way a real Sony SRG-120DH was
    measured to over VISCA-over-IP (see src/ptz-visca.cpp), so a client's
    handling of those quirks can be tested:

    - a request that arrives within min_gap of the last reply sent is
      silently dropped (the camera measured dropping about half of the
      requests sent within 3ms of a reply);
    - sequence numbers must strictly increase after a reset; anything else
      gets a 0x0200 "0f 01" error reply;
    - inquiries are answered after inquiry_latency, longer than a client's
      naive reply timeout;
    - there are only `sockets` command sockets, each busy from the ACK until
      the command completes. A pan/tilt move completes after move_time;
      every other command completes right after its ACK. When every socket
      is busy, any request, inquiries included, gets "command buffer full"
      (90 60 03 ff).

    `stats` counts what happened, for a test to read back through the
    --debug-http-port /state endpoint.
    """

    def __init__(self, min_gap=0.008, inquiry_latency=0.07, ack_latency=0.02, move_time=0.15, sockets=2):
        self.min_gap = min_gap
        self.inquiry_latency = inquiry_latency
        self.ack_latency = ack_latency
        self.move_time = move_time
        self.sockets = sockets
        self.stats = {
            'requests': 0,
            'dropped_too_soon': 0,
            'seq_errors': 0,
            'buffer_full': 0,
            # An inquiry sent again while the same one still awaited its reply
            'retried_requests': 0,
            'commands_executed': 0,
            'pan_tilt_abs_executed': 0,
            'zoom_direct_executed': 0,
        }


class ViscaUdpProtocol(asyncio.DatagramProtocol):
    def __init__(self, state, quirks=None):
        self.logic = ViscaCameraLogic(state)
        self.quirks = quirks
        self._last_seq = 0
        self._last_reply = float('-inf')
        self._busy_sockets = set()
        self._awaiting = {}

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
            if self.quirks:
                self._sony_request(addr, seq, dg)
                return
            for reply in self.logic.handle_datagram(dg):
                self._send(addr, VISCA_IP_REPLY, seq, reply)
        elif ptype == VISCA_IP_CONTROL_CMD:
            if payload[:1] == bytes([VISCA_IP_RESET]):
                self._last_seq = 0
                self._send(addr, VISCA_IP_CONTROL_REPLY, seq, bytes([VISCA_IP_RESET]))
        # Unrecognized control opcodes are silently ignored.

    def _send(self, addr, ptype, seq, payload):
        header = bytes([(ptype >> 8) & 0xff, ptype & 0xff,
                         (len(payload) >> 8) & 0xff, len(payload) & 0xff]) + \
            seq.to_bytes(4, 'big')
        self.transport.sendto(header + payload, addr)

    def _sony_reply(self, addr, seq, reply, awaited=None):
        if awaited is not None:
            self._awaiting[awaited] -= 1
        self._last_reply = asyncio.get_running_loop().time()
        self._send(addr, VISCA_IP_REPLY, seq, reply)

    def _sony_request(self, addr, seq, dg):
        q = self.quirks
        loop = asyncio.get_running_loop()
        q.stats['requests'] += 1

        if loop.time() - self._last_reply < q.min_gap:
            q.stats['dropped_too_soon'] += 1
            return
        if seq <= self._last_seq:
            q.stats['seq_errors'] += 1
            self._send(addr, VISCA_IP_CONTROL_CMD, seq, b'\x0f\x01')
            return
        self._last_seq = seq

        is_inquiry = len(dg) > 1 and dg[1] == 0x09
        if is_inquiry and self._awaiting.get(dg):
            q.stats['retried_requests'] += 1
        if len(self._busy_sockets) >= q.sockets:
            q.stats['buffer_full'] += 1
            self._sony_reply(addr, seq, b'\x90\x60\x03\xff')
            return

        replies = self.logic.handle_datagram(dg)
        if is_inquiry:
            self._awaiting[dg] = self._awaiting.get(dg, 0) + 1
            for reply in replies:
                loop.call_later(q.inquiry_latency, self._sony_reply, addr, seq, reply, dg)
            return
        if not (len(replies) == 2 and replies[0][1] == 0x41 and replies[1][1] == 0x51):
            for reply in replies:
                self._sony_reply(addr, seq, reply)
            return

        q.stats['commands_executed'] += 1
        if dg[1:4] == b'\x01\x06\x02':
            q.stats['pan_tilt_abs_executed'] += 1
        if dg[1:4] == b'\x01\x04\x47':
            q.stats['zoom_direct_executed'] += 1
        moves = dg[1:4] in (b'\x01\x06\x02', b'\x01\x06\x03')
        sock = min(set(range(1, q.sockets + 1)) - self._busy_sockets)
        self._busy_sockets.add(sock)
        done = q.ack_latency + (q.move_time if moves else 0)
        loop.call_later(q.ack_latency, self._sony_reply, addr, seq, bytes([0x90, 0x40 | sock, 0xff]))
        loop.call_later(done, self._sony_complete, addr, seq, sock)

    def _sony_complete(self, addr, seq, sock):
        self._busy_sockets.discard(sock)
        self._sony_reply(addr, seq, bytes([0x90, 0x50 | sock, 0xff]))


class ViscaUdpServer:
    def __init__(self, state, host='', port=52381, quirks=None):
        self.state = state
        self.quirks = quirks
        self.host = host
        self.port = port
        self.transport = None

    async def start(self, loop):
        self.transport, _protocol = await loop.create_datagram_endpoint(
            lambda: ViscaUdpProtocol(self.state, self.quirks), local_addr=(self.host or '0.0.0.0', self.port))
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

    def __init__(self, state, host='', tcp_port=5678, udp_port=52381, serial_path=None, udp_quirks=None):
        self.state = state
        self.udp_quirks = udp_quirks
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
            self._udp = ViscaUdpServer(self.state, self.host, self.udp_port, self.udp_quirks)
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
