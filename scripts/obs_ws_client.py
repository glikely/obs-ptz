#!/usr/bin/env python3
"""Minimal obs-websocket v5 client, standard library only.

Speaks just enough of the protocol (RFC6455 handshake/framing +
obs-websocket's Hello/Identify/Identified/Request/RequestResponse) to
send one CallVendorRequest and print the response as JSON - no
`websockets`/`simpleobsws`/etc. dependency, so it runs anywhere a
plain `python3` does without `pip install` first.

Usage:
    obs_ws_client.py --vendor obs-ptz --request-type ui_test_run \\
        --data '{"cmd": "appearance_row_sizing", "density": "-2", "fontscale": "8"}' \\
        [--host 127.0.0.1] [--port 4455] [--password ...]

Prints the vendor response's responseData as JSON on stdout and exits
0 on success. Exits non-zero (with a message on stderr) on a transport,
auth, or protocol-level failure - NOT on the vendor request itself
reporting an application-level failure, which is left to the caller to
interpret from the printed JSON.
"""
import argparse
import base64
import hashlib
import json
import os
import socket
import struct
import sys
import uuid


class ObsWebSocketError(Exception):
    pass


def _recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ObsWebSocketError("connection closed while reading")
        buf += chunk
    return buf


def _ws_handshake(sock, host, port):
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    request = (
        f"GET / HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        f"Upgrade: websocket\r\n"
        f"Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n"
        f"\r\n"
    )
    sock.sendall(request.encode("ascii"))

    # Read headers up to the blank line; obs-websocket's own response is
    # small enough this never needs more than a couple of recv() calls.
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise ObsWebSocketError("connection closed during handshake")
        buf += chunk
    header_end = buf.index(b"\r\n\r\n")
    status_line = buf[:header_end].split(b"\r\n", 1)[0]
    if b"101" not in status_line:
        raise ObsWebSocketError(f"handshake failed: {status_line!r}")
    # Any bytes after the header blank line are the start of the first
    # websocket frame; obs-websocket sends Hello immediately, so stash
    # them for the first _ws_recv_text() call rather than dropping them.
    return buf[header_end + 4:]


def _ws_send_text(sock, text):
    payload = text.encode("utf-8")
    length = len(payload)
    if length <= 125:
        header = struct.pack("!BB", 0x81, 0x80 | length)
    elif length <= 0xFFFF:
        header = struct.pack("!BBH", 0x81, 0x80 | 126, length)
    else:
        header = struct.pack("!BBQ", 0x81, 0x80 | 127, length)
    mask = os.urandom(4)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    sock.sendall(header + mask + masked)


def _ws_recv_text(sock, leftover):
    """Reads exactly one text frame, using `leftover` bytes already
    buffered (from the handshake, or a previous over-read) before
    reading more from the socket. Returns (text, new_leftover)."""
    buf = bytearray(leftover)

    def need(n):
        nonlocal buf
        while len(buf) < n:
            chunk = sock.recv(4096)
            if not chunk:
                raise ObsWebSocketError("connection closed while reading a frame")
            buf.extend(chunk)

    need(2)
    b0, b1 = buf[0], buf[1]
    opcode = b0 & 0x0F
    masked = bool(b1 & 0x80)
    length = b1 & 0x7F
    offset = 2
    if length == 126:
        need(4)
        length = struct.unpack("!H", bytes(buf[2:4]))[0]
        offset = 4
    elif length == 127:
        need(10)
        length = struct.unpack("!Q", bytes(buf[2:10]))[0]
        offset = 10
    mask_key = b""
    if masked:
        need(offset + 4)
        mask_key = bytes(buf[offset:offset + 4])
        offset += 4
    need(offset + length)
    payload = bytes(buf[offset:offset + length])
    if masked:
        payload = bytes(b ^ mask_key[i % 4] for i, b in enumerate(payload))
    new_leftover = bytes(buf[offset + length:])

    if opcode == 0x8:  # close
        raise ObsWebSocketError("server closed the connection")
    if opcode != 0x1:  # not text - obs-websocket only ever sends text/close
        return _ws_recv_text(sock, new_leftover)
    return payload.decode("utf-8"), new_leftover


def _auth_string(password, salt, challenge):
    # obs-websocket v5 auth: base64(sha256(base64(sha256(password + salt)) + challenge))
    secret = hashlib.sha256((password + salt).encode("utf-8")).digest()
    secret_b64 = base64.b64encode(secret).decode("ascii")
    auth = hashlib.sha256((secret_b64 + challenge).encode("utf-8")).digest()
    return base64.b64encode(auth).decode("ascii")


def call_vendor_request(host, port, password, vendor_name, request_type, request_data, timeout):
    sock = socket.create_connection((host, port), timeout=timeout)
    try:
        leftover = _ws_handshake(sock, host, port)

        hello_text, leftover = _ws_recv_text(sock, leftover)
        hello = json.loads(hello_text)
        if hello.get("op") != 0:
            raise ObsWebSocketError(f"expected Hello (op 0), got {hello}")
        hello_d = hello["d"]

        identify = {"rpcVersion": hello_d.get("rpcVersion", 1), "eventSubscriptions": 0}
        auth_info = hello_d.get("authentication")
        if auth_info:
            if not password:
                raise ObsWebSocketError("server requires authentication but no --password given")
            identify["authentication"] = _auth_string(password, auth_info["salt"], auth_info["challenge"])
        _ws_send_text(sock, json.dumps({"op": 1, "d": identify}))

        identified_text, leftover = _ws_recv_text(sock, leftover)
        identified = json.loads(identified_text)
        if identified.get("op") != 2:
            raise ObsWebSocketError(f"authentication/identify failed: {identified}")

        request_id = str(uuid.uuid4())
        request_msg = {
            "op": 6,
            "d": {
                "requestType": "CallVendorRequest",
                "requestId": request_id,
                "requestData": {
                    "vendorName": vendor_name,
                    "requestType": request_type,
                    "requestData": request_data,
                },
            },
        }
        _ws_send_text(sock, json.dumps(request_msg))

        while True:
            resp_text, leftover = _ws_recv_text(sock, leftover)
            resp = json.loads(resp_text)
            if resp.get("op") == 7 and resp.get("d", {}).get("requestId") == request_id:
                return resp["d"]
            # Ignore anything else (e.g. an Event message) and keep waiting
            # for our own RequestResponse.
    finally:
        sock.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=4455)
    parser.add_argument("--password", default=None)
    parser.add_argument("--vendor", required=True)
    parser.add_argument("--request-type", required=True)
    parser.add_argument("--data", default="{}", help="JSON object string")
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()

    try:
        request_data = json.loads(args.data)
    except json.JSONDecodeError as e:
        print(f"error: --data is not valid JSON: {e}", file=sys.stderr)
        return 2

    try:
        result = call_vendor_request(
            args.host, args.port, args.password, args.vendor, args.request_type, request_data, args.timeout
        )
    except (ObsWebSocketError, OSError, socket.timeout) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    status = result.get("requestStatus", {})
    print(json.dumps(result, indent=2))
    return 0 if status.get("result") else 1


if __name__ == "__main__":
    sys.exit(main())
