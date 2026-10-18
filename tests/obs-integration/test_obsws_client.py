"""Covers what the tests' own obs-websocket client (obsws.py) needs of the
websockets it is run with: that it connects with a release that has no `legacy`
argument, as well as one that has.

Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
SPDX-License-Identifier: GPL-2.0-or-later

That a request which finds the connection closed, as one made after OBS's
obs-websocket server was turned off does, says so as the connection closing
and with the request, as one that is waiting when it closes already does.

That the fixture's wait for OBS to be ready tries again when the connection is
closed on it, as OBS does while it is still starting: it accepts the
connection, and closes it before it says Hello.

websockets 17 added `legacy`, to keep `connect()` returning the connection when
it is not used as a context manager. Before it, `connect()` passes an argument
it does not know on to socket.create_connection, which refuses it, so a client
that always passed it could not connect at all: every test that needs OBS
timed out waiting for a handshake that was never tried.
"""

import json
import threading

import obsws
import pytest
from websockets.sync.server import serve


@pytest.fixture(autouse=True)
def _cleanup_scenes_between_tests():
    """Stands in for the suite's, which starts OBS to clean up after: nothing
    here uses one"""
    yield


def test_client_connects_with_a_websockets_that_has_no_legacy_argument(monkeypatch):
    real_connect = obsws.connect

    def connect_before_17(url, **kwargs):
        if "legacy" in kwargs:
            raise TypeError("create_connection() got an unexpected keyword argument 'legacy'")
        return real_connect(url, **kwargs)

    monkeypatch.setattr(obsws, "connect", connect_before_17)

    def handler(connection):
        connection.send(json.dumps({"op": 0, "d": {"rpcVersion": 1}}))
        connection.recv()  # Identify
        connection.send(json.dumps({"op": 2, "d": {"negotiatedRpcVersion": 1}}))
        try:
            connection.recv()  # until the client closes
        except Exception:
            pass

    with serve(handler, "127.0.0.1", 0) as server:
        port = server.socket.getsockname()[1]
        threading.Thread(target=server.serve_forever, daemon=True).start()
        client = obsws.Client(f"ws://127.0.0.1:{port}", timeout=5)
        client.close()
        server.shutdown()


def test_a_request_made_after_the_server_has_gone_says_the_connection_closed():
    def handler(connection):
        connection.send(json.dumps({"op": 0, "d": {"rpcVersion": 1}}))
        connection.recv()  # Identify
        connection.send(json.dumps({"op": 2, "d": {"negotiatedRpcVersion": 1}}))
        connection.close(1001, "Server stopping")

    with serve(handler, "127.0.0.1", 0) as server:
        port = server.socket.getsockname()[1]
        threading.Thread(target=server.serve_forever, daemon=True).start()
        client = obsws.Client(f"ws://127.0.0.1:{port}", timeout=5)
        client._reader.join(5)  # the connection has closed, and the client has noticed
        with pytest.raises(ConnectionError, match="GetVersion: the connection closed"):
            client.call("GetVersion", timeout=3)
        server.shutdown()


def test_the_wait_for_obs_tries_again_when_the_connection_is_closed_on_it():
    from conftest import connect_when_ready

    attempts = []

    def handler(connection):
        attempts.append(1)
        if len(attempts) <= 2:
            return  # closes the connection before it says Hello
        connection.send(json.dumps({"op": 0, "d": {"rpcVersion": 1}}))
        connection.recv()  # Identify
        connection.send(json.dumps({"op": 2, "d": {"negotiatedRpcVersion": 1}}))
        request = json.loads(connection.recv())  # GetSceneList, to see that OBS is ready
        connection.send(json.dumps({"op": 7, "d": {"requestType": request["d"]["requestType"],
                                                   "requestId": request["d"]["requestId"],
                                                   "requestStatus": {"result": True, "code": 100},
                                                   "responseData": {}}}))
        try:
            connection.recv()  # until the client closes
        except Exception:
            pass

    with serve(handler, "127.0.0.1", 0) as server:
        port = server.socket.getsockname()[1]
        threading.Thread(target=server.serve_forever, daemon=True).start()
        client = connect_when_ready(port, None)
        client.close()
        server.shutdown()

    assert len(attempts) == 3
