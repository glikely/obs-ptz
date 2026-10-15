"""Covers the UI driver (tests/ui-driver/), a plugin of its own that lets a
client of obs-websocket find and drive OBS's Qt widgets, through its vendor,
"ui-driver". It knows nothing of obs-ptz, so it is tested on OBS's own
widgets, with the effect of what it does read back through OBS's own requests.

Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
SPDX-License-Identifier: GPL-2.0-or-later

The widgets are the main window's, by the names they have in OBS's own
frontend/forms/OBSBasic.ui, the same in every OBS the suite runs on (32.1 and
32.2): the scene list "scenes" (a SceneTree), the transition list
"transitions" (a QComboBox), the Tools menu, "menuTools", and the Studio Mode
button, "modeSwitch". A source's Filters window (OBSBasicFilters) is one OBS
deletes when it is closed (WA_DeleteOnClose, in OBSBasic::OpenFilters), which
obs-websocket's OpenInputFiltersDialog opens.

A secret is the one widget that isn't OBS's own window's: OBS's properties
view, as the PTZ settings dialog shows it for the ONVIF device, whose password
is an OBS_TEXT_PASSWORD property. The view shows one in a QLineEdit that hides
what is typed in it, beside a checkable "Show" button that shows it, neither
with an objectName (OBSPropertiesView::AddText() in OBS's
shared/properties-view). Whether anything gave the secret away is worked out
before it is asserted, so that an assertion that fails doesn't print it.
"""

import json
import threading

import pytest
from conftest import SETTINGS_DIALOG
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


@pytest.fixture
def studio_mode_off(obs_world):
    """Studio Mode off, and put back as it was"""
    was = obs_world.ws.call("GetStudioModeEnabled")["studioModeEnabled"]
    obs_world.ws.call("SetStudioModeEnabled", {"studioModeEnabled": False})
    yield
    obs_world.ws.call("SetStudioModeEnabled", {"studioModeEnabled": was})


@pytest.fixture
def program_scene(obs_world):
    """The program scene, put back as it was"""
    was = obs_world.ws.call("GetCurrentProgramScene")["currentProgramSceneName"]
    yield
    obs_world.ws.call("SetCurrentProgramScene", {"sceneName": was})


def studio_mode(obs_world):
    return obs_world.ws.call("GetStudioModeEnabled")["studioModeEnabled"]


def test_set_writes_a_property(obs_world):
    obs_world.need_ui_driver()
    was = obs_world.ui.get1(MAIN, "scenes", name="toolTip")
    try:
        obs_world.ui.set(MAIN, "scenes", toolTip="set by the UI driver")
        assert obs_world.ui.get1(MAIN, "scenes", name="toolTip") == "set by the UI driver"
    finally:
        obs_world.ui.set(MAIN, "scenes", toolTip=was or "")


def test_set_refuses_a_value_of_the_wrong_type(obs_world):
    obs_world.need_ui_driver()
    with pytest.raises(UiDriverError) as raised:
        obs_world.ui.set(MAIN, "scenes", enabled={"a": 1})
    assert raised.value.code == "bad_value"
    assert obs_world.ui.get1(MAIN, "scenes", name="enabled") is True


def test_set_says_which_property_is_not_there(obs_world):
    obs_world.need_ui_driver()
    with pytest.raises(UiDriverError) as raised:
        obs_world.ui.set(MAIN, "scenes", noSuchProperty=1)
    assert raised.value.code == "no_property"


def test_invoke_calls_a_slot(obs_world, studio_mode_off):
    obs_world.need_ui_driver()
    obs_world.ui.invoke(MAIN, "modeSwitch", method="click")
    assert studio_mode(obs_world) is True


def test_invoke_queued_answers_before_the_call_runs(obs_world, studio_mode_off):
    obs_world.need_ui_driver()
    assert obs_world.ui.invoke(MAIN, "modeSwitch", method="click", queued=True) is None
    obs_world.wait_for(lambda: studio_mode(obs_world) is True)


def test_invoke_says_when_there_is_no_such_method(obs_world):
    obs_world.need_ui_driver()
    with pytest.raises(UiDriverError) as raised:
        obs_world.ui.invoke(MAIN, "modeSwitch", method="noSuchMethod")
    assert raised.value.code == "no_method"


def test_invoke_passes_arguments(obs_world):
    obs_world.need_ui_driver()
    # QWidget::setWindowTitle(QString) is a slot, and the scene list isn't a
    # window, so its title shows nowhere
    was = obs_world.ui.get1(MAIN, "scenes", name="windowTitle")
    try:
        obs_world.ui.invoke(MAIN, "scenes", method="setWindowTitle", args=["from invoke"])
        assert obs_world.ui.get1(MAIN, "scenes", name="windowTitle") == "from invoke"
    finally:
        obs_world.ui.set(MAIN, "scenes", windowTitle=was or "")


def test_activate_chooses_a_transition_as_a_user_does(obs_world):
    obs_world.need_ui_driver()
    was = obs_world.ws.call("GetCurrentSceneTransition")["transitionName"]
    other = next(item["text"] for item in obs_world.ui.get1(MAIN, "transitions", name="items")
                 if item["text"] != was)
    try:
        index = obs_world.ui.activate(MAIN, "transitions", text=other)
        assert obs_world.ui.get1(MAIN, "transitions", name="currentIndex") == index
        obs_world.wait_for(lambda: obs_world.ws.call("GetCurrentSceneTransition")["transitionName"] == other)
    finally:
        obs_world.ws.call("SetCurrentSceneTransition", {"transitionName": was})


def test_activate_says_when_no_item_matches(obs_world):
    obs_world.need_ui_driver()
    with pytest.raises(UiDriverError) as raised:
        obs_world.ui.activate(MAIN, "transitions", text="no such transition")
    assert raised.value.code == "not_found"


def test_select_makes_a_scene_current(obs_world, studio_mode_off, program_scene):
    obs_world.need_ui_driver()
    first, second = obs_world.create_scene(), obs_world.create_scene()
    obs_world.ws.call("SetCurrentProgramScene", {"sceneName": first})
    obs_world.ui.select(MAIN, "scenes", value=second)
    obs_world.wait_for(lambda: obs_world.ws.call("GetCurrentProgramScene")["currentProgramSceneName"] == second)


def test_select_says_when_no_row_matches(obs_world):
    obs_world.need_ui_driver()
    with pytest.raises(UiDriverError) as raised:
        obs_world.ui.select(MAIN, "scenes", value="no such scene")
    assert raised.value.code == "not_found"


def test_select_says_when_the_model_has_no_such_role(obs_world):
    obs_world.need_ui_driver()
    with pytest.raises(UiDriverError) as raised:
        obs_world.ui.select(MAIN, "scenes", value="x", role="noSuchRole")
    assert raised.value.code == "bad_value"


def test_watch_counts_a_signal(obs_world, studio_mode_off, program_scene):
    obs_world.need_ui_driver()
    first, second = obs_world.create_scene(), obs_world.create_scene()
    obs_world.ws.call("SetCurrentProgramScene", {"sceneName": first})
    obs_world.wait_for(lambda: obs_world.ui.get1(MAIN, "scenes", name="currentRow") >= 0)
    watch = obs_world.ui.watch(MAIN, "scenes", signal="currentRowChanged")
    assert obs_world.ui.count(watch) == 0
    obs_world.ui.select(MAIN, "scenes", value=second)
    obs_world.ui.select(MAIN, "scenes", value=first)
    assert obs_world.ui.count(watch) == 2
    # watching it again counts from nothing, under the same id
    assert obs_world.ui.watch(MAIN, "scenes", signal="currentRowChanged") == watch
    assert obs_world.ui.count(watch) == 0


def test_watch_says_when_there_is_no_such_signal(obs_world):
    obs_world.need_ui_driver()
    with pytest.raises(UiDriverError) as raised:
        obs_world.ui.watch(MAIN, "scenes", signal="noSuchSignal")
    assert raised.value.code == "no_method"


def test_count_of_a_destroyed_object_is_not_found(obs_world):
    obs_world.need_ui_driver()
    scene = obs_world.create_scene()
    source = f"{scene}-filters"
    obs_world.ws.call("CreateInput", {"sceneName": scene, "inputName": source, "inputKind": "color_source_v3",
                                      "inputSettings": {}})
    try:
        obs_world.ws.call("OpenInputFiltersDialog", {"inputName": source})
        obs_world.wait_for(lambda: obs_world.ui.find({"class": "OBSBasicFilters"}))
        [window] = obs_world.ui.find({"class": "OBSBasicFilters"})
        watch = obs_world.ui.watch({"id": window["id"]}, signal="windowTitleChanged")
        # QWidget::close() says whether the window closed
        assert obs_world.ui.invoke({"id": window["id"]}, method="close") is True

        def gone():
            try:
                obs_world.ui.find({"id": window["id"]})
            except UiDriverError as e:
                return e.code == "not_found"
            return False
        obs_world.wait_for(gone)
        with pytest.raises(UiDriverError) as raised:
            obs_world.ui.count(watch)
        assert raised.value.code == "not_found"
        assert obs_world.ws.call("GetVersion")["obsVersion"]
    finally:
        obs_world.ws.call("RemoveInput", {"inputName": source})


@pytest.mark.parametrize("value", [[], {}, None])
def test_a_list_an_object_or_nothing_is_no_value_to_match(obs_world, value):
    """Criteria are compared as numbers, true or false, or text: a list, an
    object or null is none of them, and matches nothing, not every object
    whose property is empty (a menu's separators have empty text)"""
    obs_world.need_ui_driver()
    with pytest.raises(UiDriverError) as raised:
        obs_world.ui.find(MAIN, {"class": "QAction", "text": value}, all=True)
    assert raised.value.code == "not_found"


SECRET = "not-for-the-driver-to-tell"
PROPERTIES = {"class": "OBSPropertiesView"}
# Qt::ImSurroundingText: what QLineEdit::inputMethodQuery() answers with its text
IM_SURROUNDING_TEXT = 0x10


def gives_away(answer):
    return SECRET in json.dumps(answer)


@pytest.fixture
def secret_field(obs_world):
    """The path of the ONVIF device's password field, holding SECRET, and
    hidden again, and emptied, after the test"""
    obs_world.need_ui_driver()
    obs_world.open_settings_dialog(obs_world.device_names["onvif"])
    obs_world.wait_for(lambda: "password" in (obs_world.ui.get1(SETTINGS_DIALOG, name="settingsKeys") or []))
    edits = obs_world.ui.find(SETTINGS_DIALOG, PROPERTIES, {"class": "QLineEdit"}, all=True)
    [field] = [edit for edit in edits if edit.get("secret")]
    path = ({"id": field["id"]},)
    # written as code does, which no "edited" signal says, so nothing is to apply
    obs_world.ui.set(*path, text=SECRET)
    yield path
    try:
        obs_world.ui.set(*path, text="")
        show = (SETTINGS_DIALOG, PROPERTIES, {"class": "QPushButton", "checkable": True})
        if obs_world.ui.get1(*show, name="checked"):
            obs_world.ui.invoke(*show, method="click")
    except UiDriverError as e:
        if e.code != "not_found":  # the view was drawn again
            raise


def show_secret(obs_world):
    """Clicks the password's "Show" button, as a user does"""
    obs_world.ui.invoke(SETTINGS_DIALOG, PROPERTIES, {"class": "QPushButton", "text": "Show"}, method="click")


def test_a_field_that_hides_its_text_is_a_secret(obs_world, secret_field):
    [field] = obs_world.ui.find(*secret_field)
    assert field["secret"] is True and "text" not in field
    # the other fields of the view are not
    edits = obs_world.ui.find(SETTINGS_DIALOG, PROPERTIES, {"class": "QLineEdit"}, all=True)
    assert any("text" in edit and not edit.get("secret") for edit in edits)


def test_tree_gives_away_no_secret(obs_world, secret_field):
    leaked = gives_away(obs_world.ui.tree(depth=100))
    assert not leaked


def test_get_reads_nothing_of_what_is_in_a_secret(obs_world, secret_field):
    for name in ("text", "displayText", "selectedText", "uiDriverSecret"):
        with pytest.raises(UiDriverError) as raised:
            obs_world.ui.get(*secret_field, properties=[name])
        leaked = gives_away(raised.value.message)
        assert raised.value.code == "secret" and not leaked, name
    got = obs_world.ui.get(*secret_field, properties=["enabled", "hidden", "echoMode", "readOnly"])
    assert got["enabled"] is True and got["hidden"] is False


def test_a_secret_is_not_found_by_its_text(obs_world, secret_field):
    with pytest.raises(UiDriverError) as raised:
        obs_world.ui.find(SETTINGS_DIALOG, PROPERTIES, {"class": "QLineEdit", "text": SECRET})
    # as a wrong guess is (its message says what was looked for, which was the guess)
    assert raised.value.code == "not_found"
    # but by what isn't its text
    [field] = obs_world.ui.find(*secret_field)
    assert obs_world.ui.find(SETTINGS_DIALOG, PROPERTIES, {"class": "QLineEdit", "readOnly": False,
                                                           "id": field["id"]})


def test_a_secret_shown_stays_a_secret(obs_world, secret_field):
    hidden = obs_world.ui.get1(*secret_field, name="echoMode")
    show_secret(obs_world)
    assert obs_world.ui.get1(*secret_field, name="echoMode") != hidden
    [field] = obs_world.ui.find(*secret_field)
    assert field["secret"] is True and "text" not in field
    with pytest.raises(UiDriverError) as raised:
        obs_world.ui.get(*secret_field, properties=["text"])
    assert raised.value.code == "secret"
    leaked = gives_away(obs_world.ui.tree(depth=100))
    assert not leaked


def test_a_secret_is_not_shown_by_setting_its_echo_mode(obs_world, secret_field):
    hidden = obs_world.ui.get1(*secret_field, name="echoMode")
    with pytest.raises(UiDriverError) as raised:
        obs_world.ui.set(*secret_field, echoMode=0)
    assert raised.value.code == "secret"
    assert obs_world.ui.get1(*secret_field, name="echoMode") == hidden


def test_invoke_gives_back_nothing_of_a_secret(obs_world, secret_field):
    with pytest.raises(UiDriverError) as raised:
        obs_world.ui.invoke(*secret_field, method="inputMethodQuery", args=[IM_SURROUNDING_TEXT, 0])
    leaked = gives_away(raised.value.message)
    assert raised.value.code == "secret" and not leaked
    # a method that gives nothing back is called
    obs_world.ui.invoke(*secret_field, method="selectAll")
    assert obs_world.ui.get1(*secret_field, name="enabled") is True


def test_a_secret_is_not_copied_even_shown(obs_world, secret_field):
    show_secret(obs_world)
    for method in ("copy", "cut"):
        with pytest.raises(UiDriverError) as raised:
            obs_world.ui.invoke(*secret_field, method=method)
        assert raised.value.code == "secret", method


def test_nothing_is_pasted(obs_world, secret_field):
    """Not even into a field that isn't a secret: what is pasted may be one"""
    edits = obs_world.ui.find(SETTINGS_DIALOG, PROPERTIES, {"class": "QLineEdit"}, all=True)
    other = next(edit for edit in edits if not edit.get("secret"))
    with pytest.raises(UiDriverError) as raised:
        obs_world.ui.invoke({"id": other["id"]}, method="paste")
    assert raised.value.code == "secret"
