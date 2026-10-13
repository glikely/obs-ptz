"""Minimal obs-websocket v5 client.

Just enough of the protocol (the Hello/Identify handshake with its SHA256
challenge, Request/RequestResponse, and Events) to drive OBS from a test:
create a scene, add a ptz_action_source to it, call a vendor's request, and
wait for an event. Not a general-purpose client -- see
https://github.com/obsproject/obs-websocket/blob/master/docs/generated/protocol.md
for the protocol it implements a slice of.
"""

import base64
import collections
import hashlib
import itertools
import json
import time

from websockets.sync.client import connect

OP_HELLO = 0
OP_IDENTIFY = 1
OP_IDENTIFIED = 2
OP_EVENT = 5
OP_REQUEST = 6
OP_REQUEST_RESPONSE = 7

# obs-websocket's EventSubscription bits. A vendor's events have their own,
# which a client that asks for no more than General is not sent.
EVENT_GENERAL = 1 << 0
EVENT_VENDORS = 1 << 9


class ObsWebSocketError(RuntimeError):
    def __init__(self, message, status=None):
        super().__init__(message)
        self.status = status or {}


class Client:
    def __init__(self, url, password=None, timeout=10, event_subscriptions=0):
        self._ws = connect(url, open_timeout=timeout, legacy=True)
        self._ids = itertools.count(1)
        # The events that came while a request was waiting for its answer, in
        # the order they came, for wait_event()
        self._events = collections.deque()

        hello = json.loads(self._ws.recv(timeout=timeout))
        if hello.get("op") != OP_HELLO:
            raise ObsWebSocketError(f"expected Hello, got {hello}")

        identify = {"rpcVersion": hello["d"]["rpcVersion"], "eventSubscriptions": event_subscriptions}
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
            if msg.get("op") == OP_EVENT:
                # What a request causes can be said before the request is answered
                self._events.append(msg["d"])
                continue
            if msg.get("op") != OP_REQUEST_RESPONSE:
                continue
            d = msg["d"]
            if d.get("requestId") != request_id:
                continue
            status = d["requestStatus"]
            if not status["result"]:
                raise ObsWebSocketError(f"{request_type} failed: {status}", status=status)
            return d.get("responseData") or {}

    def vendor(self, vendor, request, data=None, timeout=10):
        """Calls a vendor's request, and returns what the vendor answered. An
        "error" in the answer, which is how a vendor says it could not, is raised,
        with the answer as the error's status."""
        response = self.call("CallVendorRequest",
                             {"vendorName": vendor, "requestType": request, "requestData": data or {}},
                             timeout=timeout)
        answer = response.get("responseData") or {}
        if "error" in answer:
            raise ObsWebSocketError(f"{vendor} {request} failed: {answer['error']}: {answer.get('message', '')}",
                                    status=answer)
        return answer

    def _next_event(self, timeout):
        """The next event, of those kept by call() first; None if there is none in time"""
        if self._events:
            return self._events.popleft()
        deadline = time.time() + timeout
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                return None
            try:
                msg = json.loads(self._ws.recv(timeout=remaining))
            except TimeoutError:
                return None
            if msg.get("op") == OP_EVENT:
                return msg["d"]

    def wait_event(self, event_type, predicate=None, timeout=10):
        """The data of the next `event_type` event for which predicate(data) is
        true, of those that have come since the client connected and have not
        been returned. An event that does not match is dropped."""
        deadline = time.time() + timeout
        while True:
            event = self._next_event(deadline - time.time())
            if event is None:
                raise TimeoutError(f"no {event_type} event within {timeout}s")
            if event.get("eventType") != event_type:
                continue
            data = event.get("eventData") or {}
            if predicate is None or predicate(data):
                return data

    def events(self, event_type, settle=0.2):
        """The data of every `event_type` event that has come and not been
        returned, with those that come within `settle` seconds. Takes them all."""
        found = []
        while True:
            event = self._next_event(settle)
            if event is None:
                return found
            if event.get("eventType") == event_type:
                found.append(event.get("eventData") or {})

    def close(self):
        self._ws.close()


def connect_when_ready(url, password=None, timeout=30, event_subscriptions=0, alive=lambda: True):
    """A Client, once the server will complete the handshake. OBS accepts the
    connection while it is still starting, and may close it before saying
    Hello, so any failure to connect is tried again until the deadline, or
    until `alive()` says the process behind the server is gone."""
    deadline = time.time() + timeout
    last_error = None
    while time.time() < deadline:
        if not alive():
            raise ObsWebSocketError(f"the server exited while it was being waited for: {last_error}")
        try:
            return Client(url, password=password, event_subscriptions=event_subscriptions)
        except Exception as e:  # websockets raises its own, OSError, EOFError...
            last_error = e
            time.sleep(0.2)
    raise TimeoutError(f"no obs-websocket handshake within {timeout}s: {last_error}")
