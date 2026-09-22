"""Covers switching an existing VISCA device's transport live, through
the "Protocol" list in the properties dialog (PTZVisca::get_obs_properties()'s
"type" list + PTZVisca::update()'s "type" -> ViscaTransport switch, see
src/ptz-visca.cpp) -- and specifically that it persists using the original
"type"/"host"/"*_port" field names (see the plugin's on-disk config format),
not a separate namespaced field per transport.

Driven through tests/ui-harness/update-device-test.cpp's "update_device"
test, which applies a settings change to a device the same way
PTZSettings::on_applyButton_clicked()/updateProperties() do when a user
edits the properties dialog and clicks Apply/OK (src/settings.cpp): the
device's current full config, overlaid with the fields under test, through
PTZListModel::update().

test_switch_visca_interface_across_all_transports walks the same device
through all three VISCA transports (TCP -> UDP -> serial -> back to TCP),
confirming for each one that the device both reports itself connected and
actually delivers a VISCA command the simulated camera decodes -- not just
that "connected" flips, which a stub or a stale link could fake.

The "visca-tcp" device (id 1, see conftest.py's DEVICE_IDS) is shared with
every other test module in this suite, so this module always restores its
original type/host/port afterward -- and the serial leg talks to its own,
throwaway ptzsim instance on its own pty (rather than reusing the
"visca-serial" device's own, session-scoped one) since ptzsim's simulated
VISCA-serial camera only ever answers at bus address 1: sharing it with the
already-running "visca-serial" device (also address 1, see conftest.py's
write_ptz_plugin_config()) would mean both PTZVisca objects filtering the
same address off the same wire, each liable to pick up replies meant for
the other's commands.
"""

import json
import subprocess
import sys
import time
import urllib.request

import pytest

from conftest import REPO_ROOT, free_port, wait_for_port

ACTION_PAN_TILT = 3
ACTION_STOP = 4

VISCA_TCP = "visca-tcp"


def _state(debug_url):
    with urllib.request.urlopen(debug_url, timeout=5) as resp:
        return json.loads(resp.read())


def _wait_for_state(debug_url, predicate, timeout=5, interval=0.1):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = _state(debug_url)
        if predicate(last):
            return last
        time.sleep(interval)
    raise AssertionError(f"state at {debug_url} never matched predicate; last seen: {last}")


def _connect_and_move(obs_world, device_id, status_file, debug_url, pan_speed):
    """Confirms `device_id` is connected over its currently configured
    transport, then that a pan/tilt move actually reaches and moves the
    simulated camera at `debug_url` (proving the transport doesn't just
    report "connected" but genuinely round-trips VISCA commands), and
    leaves the camera stopped again afterward."""
    obs_world.wait_for_device_status(device_id, status_file, lambda s: s["connected"] is True, timeout=10)

    obs_world.trigger_action(device_id, ACTION_PAN_TILT, pan_speed=pan_speed, tilt_speed=0.0)
    _wait_for_state(debug_url, lambda s: abs(s["pan_speed"] - pan_speed) < 0.05)
    obs_world.trigger_action(device_id, ACTION_STOP)
    _wait_for_state(debug_url, lambda s: s["pan_speed"] == 0.0 and s["tilt_speed"] == 0.0)


class _SerialOnlyPtzsim:
    """A dedicated, VISCA-serial-only ptzsim instance on its own pty,
    for the serial leg of the interface-switch test (see module
    docstring for why this can't just reuse the "visca-serial" device's
    own ptzsim backend)."""

    def __init__(self, work_dir):
        self.serial_path = work_dir / "change-interface-visca-serial"
        self.debug_port = free_port()
        self.debug_url = f"http://127.0.0.1:{self.debug_port}/state"
        self.proc = None

    def start(self):
        cmd = [
            sys.executable, "-m", "ptzsim",
            "--host", "127.0.0.1",
            "--visca-serial-path", str(self.serial_path),
            "--no-visca-tcp", "--no-visca-udp", "--no-onvif", "--no-pelco",
            "--debug-http-port", str(self.debug_port),
        ]
        self.proc = subprocess.Popen(cmd, cwd=REPO_ROOT / "scripts", stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT, text=True)
        wait_for_port("127.0.0.1", self.debug_port, timeout=15)

    def stop(self):
        if self.proc is None:
            return
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.proc = None


@pytest.fixture
def serial_ptzsim(tmp_path_factory):
    sim = _SerialOnlyPtzsim(tmp_path_factory.mktemp("change-iface-serial"))
    sim.start()
    yield sim
    sim.stop()


@pytest.fixture
def restore_visca_tcp_device(obs_world, ptz_ports, tmp_path):
    yield
    device_id = obs_world.device_ids[VISCA_TCP]
    obs_world.run_ui_test(
        "update_device", device_id=device_id,
        type="visca-over-tcp", host="127.0.0.1", tcp_port=ptz_ports["visca_tcp"],
    )
    obs_world.wait_for_device_status(
        device_id, tmp_path / "restore-status.json",
        lambda s: s["connected"] is True, timeout=10)


def test_switch_visca_interface_across_all_transports(obs_world, ptz_ports, serial_ptzsim, tmp_path,
                                                        restore_visca_tcp_device):
    device_id = obs_world.device_ids[VISCA_TCP]
    status_file = tmp_path / "status.json"
    shared_debug_url = obs_world.debug_url

    # Sanity: starts out connected and working over its original
    # VISCA-over-TCP transport (set up by conftest.py's
    # write_ptz_plugin_config()).
    _connect_and_move(obs_world, device_id, status_file, shared_debug_url, pan_speed=0.4)

    # Switch its "type"/protocol to VISCA-over-IP, but point it at a port
    # nothing is listening on. If PTZVisca::update() didn't actually tear
    # down and replace the live transport -- e.g. silently kept using the
    # old, already-connected TCP socket -- the device would stay
    # connected and this negative check would fail.
    dead_port = free_port()
    obs_world.run_ui_test(
        "update_device", device_id=device_id,
        type="visca-over-ip", host="127.0.0.1", udp_port=dead_port,
    )
    obs_world.trigger_action(device_id, ACTION_PAN_TILT, pan_speed=0.3)
    obs_world.wait_for_device_status(device_id, status_file, lambda s: s["connected"] is False, timeout=10)
    obs_world.trigger_action(device_id, ACTION_STOP)

    # VISCA-over-UDP, pointed at ptzsim's real UDP listener: connects and
    # actually moves the shared simulated camera, proving it's genuinely
    # speaking VISCA-over-UDP end to end (encoded by ViscaUDPTransport,
    # decoded by ptzsim's UDP backend) rather than having quietly fallen
    # back to anything else.
    obs_world.run_ui_test(
        "update_device", device_id=device_id,
        type="visca-over-ip", host="127.0.0.1", udp_port=ptz_ports["visca_udp"],
    )
    _connect_and_move(obs_world, device_id, status_file, shared_debug_url, pan_speed=0.5)

    # VISCA serial, pointed at its own dedicated ptzsim instance.
    obs_world.run_ui_test(
        "update_device", device_id=device_id,
        type="visca", serial_port=str(serial_ptzsim.serial_path), address=1,
    )
    _connect_and_move(obs_world, device_id, status_file, serial_ptzsim.debug_url, pan_speed=0.6)

    # Back to VISCA-over-TCP, on the shared simulated camera again.
    obs_world.run_ui_test(
        "update_device", device_id=device_id,
        type="visca-over-tcp", host="127.0.0.1", tcp_port=ptz_ports["visca_tcp"],
    )
    _connect_and_move(obs_world, device_id, status_file, shared_debug_url, pan_speed=0.7)
