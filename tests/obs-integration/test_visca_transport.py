"""Covers what ViscaTCPTransport and ViscaUART do with their connection, seen
from the other end of it: a plain listening socket or a pty instead of ptzsim,
as these are about what the camera's end sees and sends, not about VISCA.
"""

import os
import socket
import subprocess
import threading
import time
import tty

import pytest

from conftest import IS_WINDOWS

VISCA_FILTER = "ca.secretlab.obs-ptz.visca"
SOURCE = "visca-transport-cam"

# Well over the memory a transport should need for a packet
FLOOD = 256 * 1024 * 1024
MAX_GROWTH = 64 * 1024 * 1024

# As much as a transport keeps of a packet, and an ack to come after it
RX_LIMIT = 256
ACK = bytes([0x90, 0x41, 0xff])


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


def recv_count(world):
    return world.device_state(SOURCE)["state"]["statistics"].get("visca_recv_count", 0)


def rss(pid):
    return int(subprocess.check_output(["ps", "-o", "rss=", "-p", str(pid)])) * 1024


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


def test_tcp_starts_again_at_the_limit(obs_world, camera_source, fake_cameras):
    camera = fake_cameras()
    add_filter(obs_world, tcp_settings(camera.port))
    obs_world.wait_for(lambda: camera.connections, timeout=10)

    camera.connections[0].sendall(bytes(RX_LIMIT) + ACK)

    obs_world.wait_for(lambda: recv_count(obs_world) == 1, timeout=5)


@pytest.mark.skipif(IS_WINDOWS, reason="no pty to stand in for a serial port")
def test_uart_starts_again_at_the_limit(obs_world, camera_source):
    camera, device = os.openpty()
    tty.setraw(camera)
    tty.setraw(device)
    add_filter(obs_world, {"type": "visca", "serial_port": os.ttyname(device), "baud_rate": 9600})
    time.sleep(1)

    os.write(camera, bytes(RX_LIMIT) + ACK)

    obs_world.wait_for(lambda: recv_count(obs_world) == 1, timeout=5)
    os.close(camera)
    os.close(device)


@pytest.mark.skipif(IS_WINDOWS, reason="no ps to read the size of a process with")
def test_tcp_stream_without_a_terminator_is_not_kept(obs_world, camera_source, fake_cameras):
    camera = fake_cameras()
    add_filter(obs_world, tcp_settings(camera.port))
    obs_world.wait_for(lambda: camera.connections, timeout=10)
    pid = obs_world.obs_pid()
    before = rss(pid)

    block = bytes(1024 * 1024)
    camera.connections[0].settimeout(60)
    for _ in range(FLOOD // len(block)):
        camera.connections[0].sendall(block)

    # Everything sent has been read once a request has been answered
    obs_world.wait_for(lambda: obs_world.ws.call("GetVersion"), timeout=30)
    assert rss(pid) - before < MAX_GROWTH


@pytest.mark.skipif(IS_WINDOWS, reason="no pty to stand in for a serial port")
def test_uart_stream_without_a_terminator_is_not_kept(obs_world, camera_source):
    camera, device = os.openpty()
    tty.setraw(camera)
    tty.setraw(device)
    add_filter(obs_world, {"type": "visca", "serial_port": os.ttyname(device), "baud_rate": 9600})
    time.sleep(1)
    pid = obs_world.obs_pid()
    before = rss(pid)

    block = bytes(4096)
    os.set_blocking(camera, True)
    for _ in range(FLOOD // len(block)):
        os.write(camera, block)

    obs_world.wait_for(lambda: obs_world.ws.call("GetVersion"), timeout=30)
    assert rss(pid) - before < MAX_GROWTH
    os.close(camera)
    os.close(device)
