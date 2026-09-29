"""VISCA-over-IP against a camera that behaves like a real Sony one.

A Sony SRG-120DH, measured over UDP, drops requests that come too soon
after its last reply, answers some inquiries slower than a naive reply
timeout, has only two command sockets, and answers "command buffer full"
to anything sent while both are busy. ptzsim's --visca-udp-sony-quirks
mode (SonyUdpQuirks in scripts/ptzsim/backends/visca.py) does the same, and
counts what it saw, so these tests check how PTZVisca and its UDP transport
(src/ptz-visca.cpp, src/ptz-visca-udp.cpp) cope:

- the startup inquiries are spaced out enough to not be dropped, and the
  reply timeout is long enough that none is sent twice;
- a command that is ACKed and completed on a socket leaves the device
  connected, instead of its reply timer going off afterwards;
- a command that meets "buffer full" is sent again, not lost.

Each test looks at how the sim's counters changed, except the first, which
looks at everything since startup and so has to run before the others (it
is the first in this file, and nothing else talks to this device).
"""

import time

ABS_PAN_RANGE = 0x1400


def settled(sim, quiet=1.5, timeout=15):
    """The sim's counters, once no request has arrived for `quiet` seconds"""
    deadline = time.time() + timeout
    last = sim.stats()
    since = time.time()
    while time.time() < deadline:
        time.sleep(0.2)
        now = sim.stats()
        if now != last:
            last, since = now, time.time()
        elif time.time() - since >= quiet:
            return now
    raise AssertionError(f"the camera never went quiet; last seen: {last}")


def change(before, after):
    return {k: after[k] - before[k] for k in after}


def test_startup_inquiries_are_neither_dropped_nor_repeated(obs_world, sony_ptzsim, tmp_path):
    device_id = obs_world.device_ids["visca-udp-sony"]
    obs_world.wait_for_device_status(device_id, tmp_path / "status.json", lambda s: s["connected"] is True, timeout=10)

    stats = settled(sony_ptzsim)
    assert stats["requests"] > 0
    assert stats["dropped_too_soon"] == 0
    assert stats["retried_requests"] == 0
    assert stats["seq_errors"] == 0


def test_stays_connected_after_a_command_completes(obs_world, sony_ptzsim, tmp_path):
    device_id = obs_world.device_ids["visca-udp-sony"]
    status_file = tmp_path / "status.json"
    before = settled(sony_ptzsim)

    # Zoom completes as soon as it is ACKed, so nothing is left waiting for
    # the reply timer to guard, and it must not go off and mark the camera
    # disconnected, with nothing then to connect it again.
    obs_world.run_ui_test("move_device", device_id=device_id, mode="abs", zoom=0.5)
    delta = change(before, settled(sony_ptzsim))

    assert delta["zoom_direct_executed"] == 1
    assert delta["retried_requests"] == 0
    time.sleep(1)
    assert obs_world.device_status(device_id, status_file)["connected"] is True


def test_a_command_that_finds_the_camera_busy_is_sent_again(obs_world, sony_ptzsim, tmp_path):
    device_id = obs_world.device_ids["visca-udp-sony"]
    status_file = tmp_path / "status.json"
    before = settled(sony_ptzsim)

    # Each move keeps a command socket for 150ms, and there are two, so the
    # third finds none free
    for pan in (0.01, 0.02, 0.03):
        obs_world.run_ui_test("move_device", device_id=device_id, mode="abs", pan=pan, tilt=0)
    delta = change(before, settled(sony_ptzsim))

    assert delta["buffer_full"] >= 1, "the camera was never busy, so this proved nothing"
    assert delta["pan_tilt_abs_executed"] == 3
    assert delta["dropped_too_soon"] == 0
    assert obs_world.device_status(device_id, status_file)["connected"] is True
