"""Covers the UI driver (tests/ui-driver/), a plugin of its own that lets a
client of obs-websocket find and drive OBS's Qt widgets, through its vendor,
"ui-driver". It knows nothing of obs-ptz, so it is tested on OBS's own
widgets, with the effect of what it does read back through OBS's own requests.

Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
SPDX-License-Identifier: GPL-2.0-or-later

The widgets are the main window's, by the names they have in OBS's own
frontend/forms/OBSBasic.ui, the same in every OBS the suite runs on (32.1 and
32.2): the scene list "scenes" (a SceneTree), the transition list
"transitions" (a QComboBox) and the Tools menu, "menuTools".
"""

import threading

import pytest
from obsws import Client, ObsWebSocketError
from test_websocket_threads import hold_main_thread
from uidriver import UiDriver, UiDriverError

MAIN = {"class": "OBSBasic"}


def test_the_driver_answers(obs_world):
    obs_world.need_ui_driver()
    assert obs_world.ui.call("ping") == {"driver": 1}


def test_an_unknown_request_is_no_request(obs_world):
    obs_world.need_ui_driver()
    with pytest.raises(ObsWebSocketError) as raised:
        obs_world.ws.vendor("ui-driver", "no_such_request")
    assert raised.value.status.get("code") == 600


def test_find_names_the_main_window_by_class(obs_world):
    obs_world.need_ui_driver()
    [main] = obs_world.ui.find(MAIN)
    assert main["class"] == "OBSBasic" and main["visible"] is True and main["id"]


def test_an_object_keeps_its_id(obs_world):
    obs_world.need_ui_driver()
    [scenes] = obs_world.ui.find(MAIN, "scenes")
    assert scenes["class"] == "SceneTree"
    assert obs_world.ui.find({"id": scenes["id"]})[0]["id"] == scenes["id"]
    assert obs_world.ui.find(MAIN, "scenes")[0]["id"] == scenes["id"]


def test_a_segment_matches_on_any_property(obs_world):
    obs_world.need_ui_driver()
    # the Tools menu's entry for the PTZ settings, by its text: a QAction has no objectName
    [action] = obs_world.ui.find(MAIN, "menuTools", {"class": "QAction", "text": "PTZ Devices"})
    assert action["class"] == "QAction" and action["text"] == "PTZ Devices"


def test_find_reports_what_it_could_not_find(obs_world):
    obs_world.need_ui_driver()
    with pytest.raises(UiDriverError) as raised:
        obs_world.ui.find(MAIN, "noSuchWidget")
    assert raised.value.code == "not_found"


def test_a_segment_that_matches_more_than_one_is_ambiguous(obs_world):
    obs_world.need_ui_driver()
    with pytest.raises(UiDriverError) as raised:
        obs_world.ui.find(MAIN, {"class": "QAction"})
    assert raised.value.code == "ambiguous"
    assert len(obs_world.ui.find(MAIN, {"class": "QAction"}, all=True)) > 1


def test_get_reads_properties_and_the_combo_box_items(obs_world):
    obs_world.need_ui_driver()
    got = obs_world.ui.get(MAIN, "transitions", properties=["currentText", "items", "hidden"])
    current = obs_world.ws.call("GetCurrentSceneTransition")["transitionName"]
    assert got["currentText"] == current
    assert current in [item["text"] for item in got["items"]]
    assert got["hidden"] is False


def test_get_says_which_property_is_not_there(obs_world):
    obs_world.need_ui_driver()
    with pytest.raises(UiDriverError) as raised:
        obs_world.ui.get(MAIN, properties=["noSuchProperty"])
    assert raised.value.code == "no_property"


def test_tree_lists_the_windows(obs_world):
    obs_world.need_ui_driver()
    windows = obs_world.ui.tree()
    assert any(node["class"] == "OBSBasic" for node in windows)
    main = obs_world.ui.tree(MAIN, depth=6)
    assert main["class"] == "OBSBasic"

    def names(node):
        yield node["name"]
        for child in node.get("children", []):
            yield from names(child)
    assert {"scenes", "transitions", "menuTools"} <= set(names(main))


def test_requests_at_the_same_time_each_get_their_own_answer(obs_world):
    """Four clients at once, each asking again and again for an object of its
    own: every answer is about the object its own client asked for"""
    obs_world.need_ui_driver()
    ids = [o["id"] for o in obs_world.ui.find(MAIN, {"class": "QDockWidget"}, all=True)[:4]]
    assert len(set(ids)) == 4
    wrong = []
    errors = []

    def ask(id_):
        client = Client(obs_world.ws_url, password=obs_world.ws_password)
        try:
            driver = UiDriver(client)
            for _ in range(50):
                [found] = driver.find({"id": id_})
                if found["id"] != id_:
                    wrong.append((id_, found["id"]))
        except Exception as e:
            errors.append(e)
        finally:
            client.close()

    threads = [threading.Thread(target=ask, args=(id_,)) for id_ in ids]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(timeout=60)
    assert not any(thread.is_alive() for thread in threads), "a client was never answered"
    assert not errors, errors
    assert not wrong, wrong


def test_obs_quits_with_a_request_waiting(obs_world):
    """Closing OBS while the driver's requests keep coming, right through its
    closing: each is answered or given up, and OBS is not kept from quitting
    by one that waits for a main thread that has stopped running what it is
    sent. As test_websocket_threads.py's test of the PTZ binding does."""
    obs_world.need_ui_driver()
    obs_world.need_harness()  # for hold_main_thread
    clients = [Client(obs_world.ws_url, password=obs_world.ws_password) for _ in range(4)]
    outcomes = []
    lock = threading.Lock()

    def ask(client):
        driver = UiDriver(client)
        while True:
            try:
                outcome = "answered" if driver.find(MAIN) else "odd"
            except UiDriverError as e:
                outcome = e.code
            except ObsWebSocketError as e:
                # obs-websocket refuses a request once OBS has begun to close,
                # and the driver takes its requests away then
                outcome = {207: "not_ready", 600: "gone"}.get(e.status.get("code"), f"refused: {e}")
            except Exception:  # OBS closed the connection, or has gone
                with lock:
                    outcomes.append("closed")
                return
            with lock:
                outcomes.append(outcome)

    threads = [threading.Thread(target=ask, args=(client,)) for client in clients]
    for thread in threads:
        thread.start()
    # A main thread that is busy when it is told to close, so that requests are waiting
    hold_main_thread(obs_world, 1)
    # asserts that OBS quit by itself, and starts it again for the tests that follow
    obs_world.restart_obs()
    for thread in threads:
        thread.join(timeout=60)
    assert not any(thread.is_alive() for thread in threads), "a client was never answered"
    assert outcomes.count("closed") == 4
    assert set(outcomes) <= {"answered", "closed", "not_ready", "gone", "unavailable", "timeout"}, \
        sorted(set(outcomes))
    assert outcomes.count("answered") > 0
    obs_world.wait_for(lambda: obs_world.ui.find(MAIN), timeout=20)
