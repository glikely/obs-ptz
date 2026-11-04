"""Covers the procs of a device being callable from any thread
(docs/ptz-device-api.md, "Rules for calling"): the commands that change the
presets, which queue their work for the device's thread, the preset list
and thumbnail, which are read from a copy made before each preset signal, and
the statistics and the camera report, which are read under a lock.

Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
SPDX-License-Identifier: GPL-2.0-or-later

The procs are called from a thread of the caller's own by
tests/ui-harness/device-thread-procs-test.cpp's "device_procs_from_thread"
and "device_diagnostics_from_thread", which log what they found.
"""

import json
import time

from test_camera_report import report
from test_visca_profiles import camera
from test_websocket_api import VENDOR, devices
from test_websocket_threads import hold_main_thread

WRONG_THREAD = "called from wrong thread; ignored"


def entry(obs_world, backend="visca-tcp"):
    return devices(obs_world)[obs_world.device_names[backend]]


def run_from_thread(obs_world, device, cmd="device_procs_from_thread", **params):
    """Runs the harness's calls on a thread of its own, and returns what it logged"""
    log_before = len(obs_world.obs_log().read_text(errors="replace"))
    obs_world.run_ui_test(cmd, source=device["source"], filter=device["name"], **params)
    marker = f"[ptz-ui-test] {cmd}: "
    found = {}

    def logged():
        log = obs_world.obs_log().read_text(errors="replace")[log_before:]
        line = next((line for line in log.splitlines() if marker in line), None)
        if line:
            found["result"] = json.loads(line.split(marker, 1)[1])
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


def test_statistics_and_the_camera_report_are_read_from_another_thread(request, obs_world, tmp_path):
    """Read again and again from a thread of the caller's own while the device
    polls its camera and moves it, which change the statistics on the device's
    thread: each is answered every time, and the report is the one made"""
    _, _, uuid = camera(request, obs_world, tmp_path)
    assert report(obs_world, tmp_path, uuid)["report"] == "obs-ptz camera report"
    device = next(d for d in devices(obs_world).values() if d["device"] == uuid)
    obs_world.ws.vendor(VENDOR, "ptz_move", {"device": uuid, "pan": 0.5})
    try:
        result, log = run_from_thread(obs_world, device, cmd="device_diagnostics_from_thread", times=200)
    finally:
        obs_world.ws.vendor(VENDOR, "ptz_stop", {"device": uuid})
    assert WRONG_THREAD not in log
    assert result == {"times": 200, "statistics": 200, "reports": 200, "report": "obs-ptz camera report"}


def test_api_info_says_every_device_proc_may_be_called_from_any_thread(obs_world):
    uuid = entry(obs_world)["device"]
    procs = obs_world.ws.vendor(VENDOR, "ptz_get_api_info", {"device": uuid})["return"]["procs"]
    assert len(procs) > 20
    assert {p["name"]: p["thread"] for p in procs} == {p["name"]: "any" for p in procs}


def test_the_preset_list_is_read_while_the_main_thread_is_held(obs_world):
    """The binding runs a proc that may be called from any thread on obs-websocket's
    own: the list is answered at once, with the main thread busy, and as it was"""
    uuid = entry(obs_world)["device"]
    before = obs_world.ws.vendor(VENDOR, "ptz_preset_get_list", {"device": uuid})["return"]
    hold_main_thread(obs_world, 3)
    started = time.time()
    held = obs_world.ws.vendor(VENDOR, "ptz_preset_get_list", {"device": uuid})["return"]
    # a command is only queued, and answered at once too, and so is what is read under a lock
    obs_world.ws.vendor(VENDOR, "ptz_preset_refresh", {"device": uuid})
    assert "statistics" in obs_world.ws.vendor(VENDOR, "ptz_get_statistics", {"device": uuid})
    obs_world.ws.vendor(VENDOR, "ptz_get_camera_report", {"device": uuid})
    assert time.time() - started < 1
    time.sleep(3)
    assert held == before
