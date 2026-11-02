"""Covers what ViscaTCPTransport does with its connection, seen from the other
end of it: a plain listening socket instead of ptzsim, as this is about what
the camera's end sees, not about VISCA.
"""

import socket
import threading
import time

import pytest

VISCA_FILTER = "ca.secretlab.obs-ptz.visca"
SOURCE = "visca-transport-cam"


class FakeCamera:
    """A VISCA over TCP port that takes connections and says nothing"""

    def __init__(self):
        self.listener = socket.create_server(("127.0.0.1", 0))
        self.port = self.listener.getsockname()[1]
        self.listener.settimeout(0.1)
        self.connections = []
        self.stopping = False
        self.thread = threading.Thread(target=self._accept, daemon=True)
        self.thread.start()

    def _accept(self):
        while not self.stopping:
            try:
                self.connections.append(self.listener.accept()[0])
            except TimeoutError:
                pass

    def closed_by_peer(self, connection):
        connection.settimeout(0.1)
        try:
            # What the device sent before it went comes first
            while connection.recv(4096):
                pass
            return True
        except ConnectionError:
            return True
        except TimeoutError:
            return False

    def stop(self):
        self.stopping = True
        self.thread.join()
        self.listener.close()
        for connection in self.connections:
            connection.close()


@pytest.fixture
def fake_cameras():
    started = []

    def start():
        started.append(FakeCamera())
        return started[-1]

    yield start
    for camera in started:
        camera.stop()


@pytest.fixture
def camera_source(obs_world):
    scene = obs_world.create_scene()
    obs_world.ws.call("CreateInput", {
        "sceneName": scene,
        "inputName": SOURCE,
        "inputKind": "ffmpeg_source",
        "inputSettings": {"is_local_file": True, "local_file": ""},
    })
    yield SOURCE
    obs_world.ws.call("RemoveInput", {"inputName": SOURCE})


def add_filter(world, settings):
    world.ws.call("CreateSourceFilter", {
        "sourceName": SOURCE,
        "filterName": "PTZ",
        "filterKind": VISCA_FILTER,
        "filterSettings": settings,
    })


def tcp_settings(port):
    return {"type": "visca-over-tcp", "host": "127.0.0.1", "tcp_port": port}


def set_settings(world, settings):
    world.ws.call("SetSourceFilterSettings", {
        "sourceName": SOURCE,
        "filterName": "PTZ",
        "filterSettings": settings,
        "overlay": True,
    })


def test_changing_the_port_moves_the_connection(obs_world, camera_source, fake_cameras):
    first, second = fake_cameras(), fake_cameras()
    add_filter(obs_world, tcp_settings(first.port))
    obs_world.wait_for(lambda: first.connections, timeout=10)

    set_settings(obs_world, tcp_settings(second.port))

    obs_world.wait_for(lambda: second.connections, timeout=5)
    assert first.closed_by_peer(first.connections[0])


def test_the_reconnect_timer_leaves_a_new_connection_alone(obs_world, camera_source, fake_cameras):
    first, second = fake_cameras(), fake_cameras()
    add_filter(obs_world, tcp_settings(first.port))
    obs_world.wait_for(lambda: first.connections, timeout=10)
    log_size = obs_world.obs_log().stat().st_size

    set_settings(obs_world, tcp_settings(second.port))
    obs_world.wait_for(lambda: second.connections, timeout=5)
    # The timer that dropping the first connection started is 1.9s long
    time.sleep(2.5)

    assert len(second.connections) == 1
    assert not second.closed_by_peer(second.connections[0])
    with open(obs_world.obs_log(), errors="replace") as log:
        log.seek(log_size)
        assert "already looking up or connecting/connected" not in log.read()


def test_other_settings_leave_the_connection_alone(obs_world, camera_source, fake_cameras):
    camera = fake_cameras()
    add_filter(obs_world, tcp_settings(camera.port))
    obs_world.wait_for(lambda: camera.connections, timeout=10)

    set_settings(obs_world, {"tally_auto": False})

    time.sleep(0.5)
    assert len(camera.connections) == 1
    assert not camera.closed_by_peer(camera.connections[0])
