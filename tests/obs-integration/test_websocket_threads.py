"""Covers the threads the PTZ API's obs-websocket binding
(src/controller/ptz-websocket.cpp) is called on: obs-websocket's own, several at
once, while the procs that read a device have to run on OBS's main thread. A
request that can't be run in time is answered so, one that needs no main thread
is not held up by it, many at once are all answered, and OBS can still quit with
one waiting.

Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
SPDX-License-Identifier: GPL-2.0-or-later

The main thread is held busy by tests/ui-harness/vendor-event-test.cpp's
"hold_main_thread" test, which sleeps in it.
"""

import threading
import time

import pytest
from obsws import Client, ObsWebSocketError
from test_filter_devices import camera_sim, cameras  # noqa: F401
from test_websocket_api import VENDOR, device, devices

# How long the binding waits for the main thread, in ptz-websocket.cpp
MAIN_THREAD_WAIT = 5


def hold_main_thread(obs_world, seconds):
    obs_world.run_ui_test("hold_main_thread", ms=int(seconds * 1000))
    # the request is only queued for the main thread: let it get there
    time.sleep(0.3)


def test_a_device_thread_request_times_out_when_the_main_thread_is_held(obs_world):
    uuid = device(obs_world, "visca-tcp")
    hold_main_thread(obs_world, MAIN_THREAD_WAIT + 2)
    started = time.time()
    with pytest.raises(ObsWebSocketError) as raised:
        obs_world.ws.vendor(VENDOR, "ptz_get_state", {"device": uuid}, timeout=MAIN_THREAD_WAIT + 4)
    waited = time.time() - started
    assert raised.value.status.get("error") == "timeout"
    assert MAIN_THREAD_WAIT - 0.5 < waited < MAIN_THREAD_WAIT + 1
    # and once the main thread is free again, it is answered as ever
    obs_world.wait_for(
        lambda: obs_world.ws.vendor(VENDOR, "ptz_get_state", {"device": uuid})["state"]["connected"] is True,
        timeout=10)


def test_an_any_thread_request_is_not_held_by_the_main_thread(obs_world):
    uuid = device(obs_world, "visca-tcp")
    hold_main_thread(obs_world, 3)
    started = time.time()
    assert obs_world.ws.vendor(VENDOR, "ptz_stop", {"device": uuid}) == {}
    assert obs_world.ws.vendor(VENDOR, "ptz_get_api_version", {"device": uuid})["major"] == 0
    assert obs_world.ws.vendor(VENDOR, "ptz_list_devices")["devices"]
    assert time.time() - started < 1
    time.sleep(3)


def test_concurrent_clients_all_get_answers(obs_world, cameras):  # noqa: F811
    """Several clients at once, with the procs of both kinds of thread, on two
    devices, one of which is removed while they ask"""
    scene = obs_world.create_scene()
    for name in ("ws-cam-a", "ws-cam-b"):
        cameras.add_source(scene, name)
        cameras.add_filter(name)
    obs_world.wait_for(lambda: {"ws-cam-a", "ws-cam-b"} <= set(devices(obs_world)))
    targets = [devices(obs_world)[name]["device"] for name in ("ws-cam-a", "ws-cam-b")]
    log_before = len(obs_world.obs_log().read_text(errors="replace"))

    outcomes = []
    lock = threading.Lock()

    def ask(index):
        client = Client(obs_world.ws_url, password=obs_world.ws_password)
        try:
            for i in range(50):
                uuid = targets[(index + i) % 2]
                request, data = (("ptz_move", {"device": uuid, "pan": 0.1, "tilt": 0.0}) if i % 2
                                 else ("ptz_get_state", {"device": uuid}))
                try:
                    answer = client.vendor(VENDOR, request, data, timeout=20)
                    outcome = "state" if "state" in answer else "ok"
                except ObsWebSocketError as e:
                    outcome = e.status.get("error") or f"refused: {e}"
                except Exception as e:  # a timeout of the client's own, or a dropped connection
                    outcome = f"failed: {e!r}"
                with lock:
                    outcomes.append(outcome)
        finally:
            client.close()

    threads = [threading.Thread(target=ask, args=(i,)) for i in range(4)]
    for thread in threads:
        thread.start()
    time.sleep(0.2)
    cameras.remove_filter("ws-cam-b")
    for thread in threads:
        thread.join(timeout=120)
    assert not any(thread.is_alive() for thread in threads), "a client never got its answers"

    assert len(outcomes) == 200
    # the one that was removed is gone for whoever asks next, whenever the clients asked
    with pytest.raises(ObsWebSocketError) as gone:
        obs_world.ws.vendor(VENDOR, "ptz_get_state", {"device": targets[1]})
    assert gone.value.status.get("error") == "unknown_device"
    assert set(outcomes) <= {"ok", "state", "unknown_device"}, sorted(set(outcomes))
    # the device that stayed was answered throughout
    assert outcomes.count("ok") >= 50 and outcomes.count("state") >= 50
    log = obs_world.obs_log().read_text(errors="replace")[log_before:]
    assert "called from wrong thread; ignored" not in log


def test_every_proc_is_run_on_a_thread_it_may_be_called_from(obs_world, cameras):  # noqa: F811
    """A proc that says it may be called from any thread is called on
    obs-websocket's own, where one that may not be is ignored by its device,
    which logs that and does nothing: so every proc there is a request for is
    called, on a camera of its own, and none is"""
    scene = obs_world.create_scene()
    cameras.add_source(scene, "ws-every-proc")
    cameras.add_filter("ws-every-proc")
    obs_world.wait_for(lambda: "ws-every-proc" in devices(obs_world))
    uuid = devices(obs_world)["ws-every-proc"]["device"]
    log_before = len(obs_world.obs_log().read_text(errors="replace"))

    procs = obs_world.ws.vendor(VENDOR, "ptz_get_api_info", {"device": uuid})["return"]["procs"]
    called = [p["name"] for p in procs if p["access"] != "none"]
    assert len(called) > 20
    for name in called:
        # with nothing but the device: every field of every proc can be left out
        obs_world.ws.vendor(VENDOR, name, {"device": uuid})
    # what was only queued for the device's thread has run by now
    time.sleep(1)
    log = obs_world.obs_log().read_text(errors="replace")[log_before:]
    assert "called from wrong thread; ignored" not in log


def test_obs_quits_while_clients_ask_for_the_main_thread(obs_world):
    """Closing OBS while requests that need the main thread keep coming, right
    through its closing: each is answered or given up, and OBS is not kept from
    quitting by one that waits for a main thread that has stopped running what
    it is sent"""
    uuid = device(obs_world, "visca-tcp")
    clients = [Client(obs_world.ws_url, password=obs_world.ws_password) for _ in range(4)]
    outcomes = []
    lock = threading.Lock()

    def ask(client):
        while True:
            try:
                answer = client.vendor(VENDOR, "ptz_get_state", {"device": uuid}, timeout=30)
                outcome = "state" if "state" in answer else f"odd: {answer}"
            except ObsWebSocketError as e:
                # obs-websocket itself refuses a request once OBS has begun to close
                # ...and the binding takes its requests away then, so there is none by that name
                outcome = e.status.get("error") or {207: "not_ready", 600: "gone"}.get(e.status.get("code"),
                                                                                   f"refused: {e}")
            except Exception:  # OBS closed the connection, or has gone
                with lock:
                    outcomes.append("closed")
                return
            with lock:
                outcomes.append(outcome)

    threads = [threading.Thread(target=ask, args=(client,)) for client in clients]
    for thread in threads:
        thread.start()
    time.sleep(0.5)
    # A main thread that is busy when it is told to close, so that requests are waiting
    hold_main_thread(obs_world, 1)
    # asserts that OBS quit by itself, and starts it again for the tests that follow
    obs_world.restart_obs()
    for thread in threads:
        thread.join(timeout=60)
    assert not any(thread.is_alive() for thread in threads), "a client was never answered"
    assert outcomes.count("closed") == 4
    assert set(outcomes) <= {"state", "closed", "not_ready", "gone", "unavailable", "timeout",
                             "unknown_device"}, sorted(set(outcomes))
    assert outcomes.count("state") > 0
    obs_world.wait_for(
        lambda: obs_world.ws.vendor(VENDOR, "ptz_get_state", {"device": device(obs_world, "visca-tcp")})["state"],
        timeout=20)


def test_the_binding_lets_go_of_obs_websocket_before_obs_unloads_it(obs_world):
    """OBS unloads its modules in the reverse of the order it loaded them, which
    for two in one directory is by name: obs-websocket is unloaded before
    obs-ptz, and what the binding was given by it (the vendor, its proc handler)
    is freed by then. So the binding takes its requests away when OBS says it is
    closing, while obs-websocket is still there, and asks nothing of it after."""
    quit_log = obs_world.restart_obs()
    assert obs_world.exit_code == 0, f"OBS exited with {obs_world.exit_code}"
    lines = quit_log.read_text(errors="replace").splitlines()

    def line_of(text):
        found = [i for i, line in enumerate(lines) if text in line]
        assert found, f"OBS's log has no {text!r}"
        return found[-1]

    let_go = line_of("[obs-ptz] websocket: let go of obs-websocket")
    assert let_go < line_of("[obs_module_unload] Shutting down")
    # and nothing of the binding's is said after obs-websocket has gone
    assert not any("[obs-ptz] websocket" in line for line in lines[line_of("[obs_module_unload] Shutting down"):])
