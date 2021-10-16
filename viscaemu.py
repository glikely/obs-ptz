#!/usr/bin/env python3

import asyncio
import os
import binascii

class ViscaDevice(asyncio.Protocol):
    def connection_made(self, transport):
        peername = transport.get_extra_info('peername')
        print('connection from', peername)
        self.transport = transport

    def send_datagram(self, dg):
        dg.insert(0, 0x91)
        dg.append(0xff)
        self.transport.write(bytes(dg))

    def receive_datagram(self, dg):
        if len(dg) < 2: # Ignore messages that are too shoret.
            return
        if dg[0] != 0x81: # Ignore messages not addressed properly
            return
        if dg[1] == 0x01: # Command
            print('Commands received')
            self.send_datagram([0x41])
            self.send_datagram([0x51])
        elif dg[1] == 0x09: # Inquiry
            print('Inquiry not supported')
            self.send_datagram([0x60, 0x02])
        else: # Unrecognized
            self.send_datagram([0x60, 0x02])

    def data_received(self, data):
        print('data received:', binascii.hexlify(data))
        datagrams = data.split(b'\xff')
        for dg in datagrams:
            self.receive_datagram(list(dg))

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
