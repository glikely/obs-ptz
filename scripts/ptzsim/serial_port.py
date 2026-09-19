"""Emulated serial port for backends that only speak UART on real hardware.

We can't open a physical COM port, so instead we open one end of a Unix
pty pair and hand the other end's path to obs-ptz's serial port picker.
Bytes written to that device arrive here exactly as they would over a
real RS-232/RS-422 link; termios settings the client applies (baud rate,
parity, ...) are irrelevant since nothing is actually being clocked onto
a wire.
"""

import os
import pty
import sys
import tty


class EmulatedSerialPort:
    def __init__(self, symlink_path=None):
        self.master_fd, self.slave_fd = pty.openpty()
        # A freshly opened pty defaults to cooked mode with echo on, which
        # would loop our own writes (arriving as "input" on the slave)
        # straight back out through the master. Raw mode turns that off.
        tty.setraw(self.slave_fd)
        self.slave_name = os.ttyname(self.slave_fd)
        self.symlink_path = symlink_path
        if symlink_path:
            try:
                os.remove(symlink_path)
            except OSError:
                pass
            try:
                os.symlink(self.slave_name, symlink_path)
            except OSError as e:
                print(f"[serial] couldn't create symlink {symlink_path}: {e}", file=sys.stderr)
                self.symlink_path = None

    @property
    def path(self):
        return self.symlink_path or self.slave_name

    def register(self, loop, on_data):
        """Deliver incoming bytes to on_data(bytes) via the given asyncio loop."""
        loop.add_reader(self.master_fd, self._read_ready, loop, on_data)

    def _read_ready(self, loop, on_data):
        try:
            data = os.read(self.master_fd, 4096)
        except OSError:
            return
        if data:
            on_data(data)
        else:
            loop.remove_reader(self.master_fd)

    def write(self, data):
        try:
            os.write(self.master_fd, data)
        except OSError:
            pass

    def close(self, loop=None):
        if loop:
            try:
                loop.remove_reader(self.master_fd)
            except (ValueError, OSError):
                pass
        for fd in (self.master_fd, self.slave_fd):
            try:
                os.close(fd)
            except OSError:
                pass
        if self.symlink_path:
            try:
                os.remove(self.symlink_path)
            except OSError:
                pass


class DatagramFramer:
    """Accumulates bytes and calls on_frame() once per terminator byte,
    matching VISCA's 0xff-terminated datagram framing."""

    def __init__(self, on_frame, terminator=0xff):
        self.on_frame = on_frame
        self.terminator = terminator
        self._buf = bytearray()

    def feed(self, data):
        for b in data:
            self._buf.append(b)
            if b == self.terminator:
                dg = bytes(self._buf[:-1])
                self._buf.clear()
                if dg:
                    self.on_frame(dg)
