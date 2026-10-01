"""Sony's VISCA-over-IP "Setting Protocol", the part a controller uses to
find cameras: it broadcasts an "ENQ:network" inquiry to UDP port 52380, and
each camera broadcasts back to the same port its MAC and IP address, model
and name (see "IP Related Setting Command" in Sony's VISCA over IP
documentation, the SRG-120DH's command list, say).

Only the inquiry is answered; the network setting command, which changes a
camera's address, is not something a simulator can do.
"""

import hashlib
import socket
import sys
import threading

from .base import Backend

SETUP_PORT = 52380
STX, ETX, FIELD_END = b"\x02", b"\x03", b"\xff"


class SonySetupBackend(Backend):
    def __init__(self, host, name, model="SIM-PTZ-1"):
        self.host = host
        self.name = name[:8]  # the protocol's limit
        self.model = model
        digest = hashlib.sha1(name.encode()).digest()
        self.mac = "-".join(f"{b:02x}" for b in (b"\x02" + digest[:5]))
        self._running = threading.Event()

    def start(self, loop=None):
        self._running.set()
        threading.Thread(target=self._loop, daemon=True).start()

    def stop(self):
        self._running.clear()

    def reply(self):
        fields = [
            f"MAC:{self.mac}", f"MODEL:{self.model}", "SOFTVERSION:01.00.00",
            f"IPADR:{self.host}", "MASK:255.255.255.0", "GATEWAY:0.0.0.0",
            f"NAME:{self.name}", "WRITE:on",
        ]
        return STX + b"".join(f.encode("ascii") + FIELD_END for f in fields) + ETX

    def _loop(self):
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            # BSD and macOS only share a wildcard UDP port when every socket
            # on it sets SO_REUSEPORT, as Qt's ShareAddress does for obs-ptz
            try:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
            except (AttributeError, OSError):
                pass
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            sock.bind(("0.0.0.0", SETUP_PORT))
            sock.settimeout(0.5)
        except OSError as e:
            print(f"[sony] setup bind failed ({e}); will not answer inquiries", file=sys.stderr)
            return

        print(f"[sony] setup protocol listening on udp/{SETUP_PORT} as {self.name} ({self.mac})")
        while self._running.is_set():
            try:
                data, addr = sock.recvfrom(1024)
            except socket.timeout:
                continue
            except OSError:
                break
            if data != STX + b"ENQ:network" + FIELD_END + ETX:
                continue
            try:
                sock.sendto(self.reply(), ("255.255.255.255", SETUP_PORT))
                print(f"[sony] answered inquiry from {addr}")
            except OSError as e:
                print(f"[sony] setup send failed: {e}", file=sys.stderr)
