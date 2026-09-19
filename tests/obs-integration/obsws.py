"""Minimal obs-websocket v5 client.

Just enough of the protocol (Hello/Identify handshake with the SHA256
challenge, and Request/RequestResponse) to drive OBS from a test: create
a scene, add a ptz_action_source to it, switch to it (which fires the
source's configured PTZ action), and clean up. Not a general-purpose
client -- see https://github.com/obsproject/obs-websocket/blob/master/docs/generated/protocol.md
for the full protocol this implements a slice of.
"""

import base64
import hashlib
import itertools
import json

from websockets.sync.client import connect

OP_HELLO = 0
OP_IDENTIFY = 1
OP_IDENTIFIED = 2
OP_REQUEST = 6
OP_REQUEST_RESPONSE = 7


class ObsWebSocketError(RuntimeError):
    def __init__(self, message, status=None):
        super().__init__(message)
        self.status = status or {}


class Client:
    def __init__(self, url, password=None, timeout=10):
        self._ws = connect(url, open_timeout=timeout, legacy=True)
        self._ids = itertools.count(1)

        hello = json.loads(self._ws.recv(timeout=timeout))
        if hello.get("op") != OP_HELLO:
            raise ObsWebSocketError(f"expected Hello, got {hello}")

        identify = {"rpcVersion": hello["d"]["rpcVersion"], "eventSubscriptions": 0}
        auth = hello["d"].get("authentication")
        if auth and password is not None:
            secret = base64.b64encode(hashlib.sha256((password + auth["salt"]).encode()).digest())
            identify["authentication"] = base64.b64encode(
                hashlib.sha256(secret + auth["challenge"].encode()).digest()).decode()
        self._ws.send(json.dumps({"op": OP_IDENTIFY, "d": identify}))

        ack = json.loads(self._ws.recv(timeout=timeout))
        if ack.get("op") != OP_IDENTIFIED:
            raise ObsWebSocketError(f"Identify failed: {ack}")

    def call(self, request_type, request_data=None, timeout=10):
        request_id = str(next(self._ids))
        payload = {"op": OP_REQUEST, "d": {"requestType": request_type, "requestId": request_id}}
        if request_data is not None:
            payload["d"]["requestData"] = request_data
        self._ws.send(json.dumps(payload))

        while True:
            msg = json.loads(self._ws.recv(timeout=timeout))
            if msg.get("op") != OP_REQUEST_RESPONSE:
                continue  # ignore Event frames etc. while waiting for our response
            d = msg["d"]
            if d.get("requestId") != request_id:
                continue
            status = d["requestStatus"]
            if not status["result"]:
                raise ObsWebSocketError(f"{request_type} failed: {status}", status=status)
            return d.get("responseData") or {}

    def close(self):
        self._ws.close()
