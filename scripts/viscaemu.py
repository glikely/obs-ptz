#!/usr/bin/env python3

import asyncio
import os
import binascii
import time

def clamp(val, min_val, max_val):
    return max(min(val, max_val), min_val)

def sign_extend(val, bits):
    sign_bit = 1 << bits
    return (val & (sign_bit - 1)) - (val & sign_bit)

class ViscaDevice(asyncio.Protocol):
    def __init__(self):
        self.last_update = time.time()
        self.panpos = 0
        self.panpos_min = -0xe500
        self.panpos_max = 0xe500
        self.panspeed = 0
        self.panspeed_min = -0xe500
        self.panspeed_max = 0xe500
        self.tiltpos = 0
        self.tiltpos_min = -0xe500
        self.tiltpos_max = 0xe500
        self.tiltspeed = 0
        self.tiltspeed_min = -0xe500
        self.tiltspeed_max = 0xe500
        self.zoompos = 0
        self.zoompos_min = 0
        self.zoompos_max = 0xe500
        self.zoomnearlimit = 0
        self.zoomspeed = 0
        self.zoomspeed_min = 0
        self.zoomspeed_max = 0xe500
        self.focuspos = 0
        self.focuspos_min = 0
        self.focuspos_max = 0xe500
        self.focusspeed = 0
        self.focusspeed_min = 0
        self.focusspeed_max = 0xe500
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
        self.power = True
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
        print(f'[{self.panpos:#6.1f}{self.panspeed:+03}, {self.tiltpos:#6.1f}{self.tiltspeed:+03}, {self.zoompos:#6.1f}{self.zoomspeed:+03}, {self.focuspos:#6.1f}{self.focusspeed:+03}]', data1, data2)

    def connection_made(self, transport):
        peername = transport.get_extra_info('peername')
        print('connection from', peername)
        self.transport = transport
        self.send_broadcast(b'\x38')

    def send_datagram(self, dg):
        self._send_datagram(b'\x90%b\xff' % dg)

    def send_broadcast(self, dg):
        self._send_datagram(b'\x88%b\xff' % dg)

    def _send_datagram(self, dg):
        self.transport.write(dg)
        self.print_state('<--', dg.hex())

    # VISCA protocol encode/decode helpers
    def decode_s4(self, val):
        '''VISCA 4 bit signed value [SM | 0S]
        M: magnitude, 3 bit, range 0x0-0x7
        S: sign, 2 bits, 2 = negative, 3 = positive, 0 = stop
        if M is omitied, then by default use the fastest magnitude
        Returned magnitude is incremended by 1 so it can be differentiated from 0
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
        # In the sign field, bit 1 is +ve, bit 2 is -ve, both set == 0
        return field[0] * ((s & 0x1) - ((s >> 1 & 0x1)))

    def decode_s16(self, field):
        '''VISCA 16 bit signed value [0Y 0Y 0Y 0Y]
        YYYY: 0x7fff..0x8000
        '''
        if (len(field) != 4):
            raise ValueError
        val = 0
        for i in range(0,4):
            if field[i] & 0xf0 != 0:
                raise ValueError
            val = (val << 4) | (field[i] & 0x0f)
        return val

    def encode_bool(self, value):
        if (value):
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

    def update_all(self):
        '''Update all state variables'''
        ts = time.time()
        dt = ts - self.last_update
        factor = 1.0
        self.panpos = clamp(self.panpos + self.panspeed * (dt * factor),
                            self.panpos_min, self.panpos_max)
        self.tiltpos = clamp(self.tiltpos + self.tiltspeed * (dt * factor),
                             self.tiltpos_min, self.tiltpos_max)
        self.zoompos = clamp(self.zoompos + self.zoomspeed * (dt * factor),
                             self.zoompos_min, self.zoompos_max)
        self.focuspos = clamp(self.focuspos + self.focusspeed * (dt * factor),
                              self.focuspos_min, self.focuspos_max)
        self.last_update = ts

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
        print ("No command handler for", dg.hex())
        self.send_datagram(b'\x60\x02')

    def cmd010001(self, dg):
        '''IF_Clear'''
        self.send_datagram(b'\x50') # slot 0 response only

    def cmd010407(self, dg):
        '''CAM_Zoom-Move'''
        self.zoomspeed = self.decode_s4(dg[4])
        self.cmd_ack()

    def cmd010408(self, dg):
        '''CAM_Focus-Move'''
        self.focusspeed = -self.decode_s4(dg[4])
        self.cmd_ack()

    def cmd010601(self, dg):
        '''Pan-tiltDrive-Move'''
        self.panspeed = self.decode_s9(dg[4:7])
        self.tiltspeed = self.decode_s9(dg[5:8])
        self.cmd_ack()

    def cmd010602(self, dg):
        '''Pan-tiltDrive-AbsolutePosition'''
        panspeed = dg[4] & 0x7f
        tiltspeed = dg[5] & 0x7f
        self.panpos = clamp(self.decode_s16(dg[6:10]),
                            self.panpos_min, self.panpos_max)
        self.tiltpos = clamp(self.decode_s16(dg[10:14]),
                             self.tiltpos_min, self.tiltpos_max)
        self.cmd_ack()

    def cmd010603(self, dg):
        '''Pan-tiltDrive-AbsolutePosition'''
        panspeed = dg[4] & 0x7f
        tiltspeed = dg[5] & 0x7f
        self.panpos = clamp(self.panpos + self.decode_s16(dg[6:10]),
                            self.panpos_min, self.panpos_max)
        self.tiltpos = clamp(self.tiltpos + self.decode_s16(dg[10:14]),
                             self.tiltpos_min, self.tiltpos_max)
        self.cmd_ack()

    def cmd010604(self, dg):
        '''Pan-tiltDrive-Home'''
        self.panpos = 0
        self.tiltpos = 0
        self.panspeed = 0
        self.tiltspeed = 0

    def cmd010605(self, dg):
        '''Pan-tiltDrive-Reset'''
        self.panpos = 0
        self.tiltpos = 0
        self.panspeed = 0
        self.tiltspeed = 0

    def cmd090002(self, dg):
        '''CAM_VersionInq'''
        self.send_datagram(b'\x50\x00\x01\x05\x11\x00\x00\x02')

    def cmd090400(self, dg):
        '''CAM_PowerInq'''
        self.send_datagram(b'\x50' + self.encode_bool(self.power))

    def cmd090612(self, dg):
        '''Pan-tiltPosInq'''
        self.send_datagram(b'\x50' + self.encode_s16(self.panpos) +
                           self.encode_s16(self.tiltpos))

    def cmd097e7e00(self, dg):
        '''Lens Control System Inquiry'''
        self.send_datagram(b'\x50' + self.encode_s16(self.zoompos) +
                           self.encode_s8(self.zoomnearlimit) +
                           self.encode_s16(self.focuspos) +
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
        self.send_datagram(bytes([0x50, self.power & 0x1, 0,
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
        if len(dg) < 2: # Ignore messages that are too short
            return
        self.print_state("-->", dg.hex())
        if dg[0] == 0x88: # Broadcast message
            if (dg[1] == 0x30) and len(dg) == 3:
                self.send_broadcast(b'\x30\x02')
                return

        if dg[0] != 0x81: # Ignore messages not addressed properly
            print("malformed", dg.hex(), dg[0])
            return

        # Find the command handler for the message
        for count in range(5, 0, -1):
            name = "cmd" + dg[1:count].hex()
            if hasattr(self, name):
                try:
                    getattr(self, name)(dg)
                except:
                    print("decode error")
                    self.cmd_error()
                    return
                break

    def data_received(self, data):
        self.update_all()
        datagrams = data.split(b'\xff')
        for dg in datagrams:
            self.receive_datagram(dg)

loop = asyncio.get_event_loop()
coro = loop.create_server(ViscaDevice, '', 5678)
server = loop.run_until_complete(coro)
print('serving on', server.sockets[0].getsockname())

try:
    loop.run_forever()
except KeyboardInterrupt:
    print('exit')
finally:
    server.close()
    loop.close()
