"""Covers the procs of a device that may be called from any thread
(docs/ptz-device-api.md, "Rules for calling"): the commands that change the
presets, which queue their work for the device's thread, and the preset list
and thumbnail, which are read from a copy made before each preset signal.

Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
SPDX-License-Identifier: GPL-2.0-or-later

The procs are called from a thread of the caller's own by
tests/ui-harness/device-thread-procs-test.cpp's "device_procs_from_thread",
which logs what it found.
"""

import json
import time

from test_websocket_api import VENDOR, devices
from test_websocket_threads import hold_main_thread

RESULT = "[ptz-ui-test] device_procs_from_thread: "
WRONG_THREAD = "called from wrong thread; ignored"

ANY_THREAD = {
    "ptz_request_state",
    "ptz_preset_get_list",
    "ptz_preset_get_thumbnail",
    "ptz_preset_create",
    "ptz_preset_delete",
    "ptz_preset_update",
    "ptz_preset_move",
    "ptz_preset_refresh",
}
DEVICE_THREAD = {"ptz_get_statistics", "ptz_get_camera_report"}


def entry(obs_world, backend="visca-tcp"):
    return devices(obs_world)[obs_world.device_names[backend]]


def run_from_thread(obs_world, device, **params):
    """Runs the harness's calls on a thread of its own, and returns what it logged"""
    log_before = len(obs_world.obs_log().read_text(errors="replace"))
    obs_world.run_ui_test("device_procs_from_thread", source=device["source"], filter=device["name"], **params)
    found = {}

    def logged():
        log = obs_world.obs_log().read_text(errors="replace")[log_before:]
        line = next((line for line in log.splitlines() if RESULT in line), None)
        if line:
            found["result"] = json.loads(line.split(RESULT, 1)[1])
            found["log"] = log
        return bool(line)

    obs_world.wait_for(logged, timeout=40)
    return found["result"], found["log"]


def test_preset_procs_work_from_a_thread_of_the_callers_own(obs_world):
    """Presets made, renamed, moved and deleted from another thread: each is done,
    says so with its signal, and the list read on that thread right after the
    signal has the change in it"""
    result, log = run_from_thread(obs_world, entry(obs_world))
    assert WRONG_THREAD not in log
    a, b = result["id_a"], result["id_b"]
    assert a and b, result
    assert result["refused"] == ""
    assert result["request_0"] and result["request_1"] and result["request_0"] != result["request_1"]
    assert result["listed_0"] and result["listed_1"]
    # what was asked, and not what the caller changed its object to after
    assert result["renamed"] == "thread-a renamed"
    assert result["first"] == b
    assert result["thumbnail_answered"] is True
    assert result["removed_listed"] is False
    # create_done comes after the preset is added, and the move queued before the
    # delete is done before it
    signals = result["signals"]
    assert signals.index(f"ptz_preset_added {a}") < signals.index(f"ptz_preset_create_done {result['request_0']}={a}")
    removed = signals.index(f"ptz_preset_removed {b}")
    assert signals[:removed].count("ptz_preset_order_changed ") == 2
    assert f"ptz_preset_removed {a}" in signals


def test_presets_made_from_another_thread_show_in_the_dock(obs_world):
    obs_world.need_ui_driver()
    device = entry(obs_world)
    obs_world.select_in_dock(device["source"])
    result, log = run_from_thread(obs_world, device, keep="1")
    assert WRONG_THREAD not in log
    try:
        obs_world.wait_for(lambda: "thread-a renamed" in obs_world.reorder_presets()["presets"])
        assert "thread-b" not in obs_world.reorder_presets()["presets"]
    finally:
        obs_world.ws.vendor(VENDOR, "ptz_preset_delete", {"device": device["device"], "id": result["id_a"]})
    obs_world.wait_for(lambda: "thread-a renamed" not in obs_world.reorder_presets()["presets"])


def test_api_info_says_which_procs_need_the_devices_thread(obs_world):
    uuid = entry(obs_world)["device"]
    procs = obs_world.ws.vendor(VENDOR, "ptz_get_api_info", {"device": uuid})["return"]["procs"]
    thread = {p["name"]: p["thread"] for p in procs}
    for name in ANY_THREAD:
        assert thread[name] == "any", name
    for name in DEVICE_THREAD:
        assert thread[name] == "device", name


def test_the_preset_list_is_read_while_the_main_thread_is_held(obs_world):
    """The binding runs a proc that may be called from any thread on obs-websocket's
    own: the list is answered at once, with the main thread busy, and as it was"""
    uuid = entry(obs_world)["device"]
    before = obs_world.ws.vendor(VENDOR, "ptz_preset_get_list", {"device": uuid})["return"]
    hold_main_thread(obs_world, 3)
    started = time.time()
    held = obs_world.ws.vendor(VENDOR, "ptz_preset_get_list", {"device": uuid})["return"]
    # a command is only queued, and answered at once too
    obs_world.ws.vendor(VENDOR, "ptz_preset_refresh", {"device": uuid})
    assert time.time() - started < 1
    time.sleep(3)
    assert held == before
