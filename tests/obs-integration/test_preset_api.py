"""Covers the PTZ API's presets, as docs/ptz-device-api.md describes them: a
preset has a string id that carries its store, `camera:<key>` for one the camera
keeps and `local:<key>` for one the device does, and the procs and signals
take and say that id.

Everything is called the way another plugin would, through the device's own
proc_handler and signal_handler (tests/ui-harness/preset-api-test.cpp's
"call_proc" and the signal recording), and checked against what the shared
ptzsim, the camera, has: a camera preset is in it, a local one is not.

- the list says what stores a device has, and what its camera does;
- a camera preset's id has the camera's own key in it (VISCA's slot, ONVIF's
  token), is made on the camera, and is recalled and deleted there;
- a local preset is made on any device that can move to a position, leaves
  the camera's own presets alone, saves every value, and a recall applies only
  the pan, tilt, zoom and focus it has;
- update, move and refresh change the list and say so with signals of ids;
- what the camera keeps is not saved in the source's settings, and what is
  kept is, and the legacy `presets` of an older version is read as annotations.
"""

import json
import time
import urllib.request

import pytest

# The filter fixtures live in test_filter_devices.py
from test_filter_devices import FILTER_KIND, camera_sim, cameras  # noqa: F401

ACTION_PRESET_RECALL = 2
ACTION_PRESET_SAVE = 5

POSITION_TOLERANCE = 0.03


def preset_list(world, device):
    result = world.call_proc(device, "ptz_preset_get_list", returns="data")
    assert result["called"], "the device has no ptz_preset_get_list"
    return result["return"]


def presets_by_id(world, device):
    return preset_list(world, device)["presets"]


def order_of(world, device):
    """The ids in display order"""
    return [entry["id"] for entry in preset_list(world, device)["order"]]


def position(world):
    state = world.state()
    return (state["pan"], state["tilt"], state["zoom"])


def settled_position(world, timeout=10):
    """Where the camera is once it has stopped going there"""
    deadline = time.time() + timeout
    last = position(world)
    stable_since = time.time()
    while time.time() < deadline:
        time.sleep(0.1)
        now = position(world)
        if max(abs(a - b) for a, b in zip(now, last)) > 0.001:
            stable_since = time.time()
        elif time.time() - stable_since > 0.4:
            return now
        last = now
    raise AssertionError(f"the camera never stopped moving; last at {last}")


def device_position(world, device):
    state = world.call_proc(device, "ptz_get_state", {"state": {}})["args"]["state"]
    return tuple(state.get(key, float("nan")) for key in ("pan", "tilt", "zoom"))


def settled_device_position(world, device, timeout=10):
    """Where the device says the camera is, once it has said so twice running:
    what a preset captures is that, and not where the camera is"""
    deadline = time.time() + timeout
    last = device_position(world, device)
    while time.time() < deadline:
        time.sleep(0.5)
        now = device_position(world, device)
        if max(abs(a - b) for a, b in zip(now, last)) < 0.001:  # not while it says nothing: nan
            return now
        last = now
    raise AssertionError(f"the device never settled on where the camera is; last {last}")


def go_to(world, device, pan, tilt, zoom):
    result = world.call_proc(device, "ptz_move_abs", {"pan": pan, "tilt": tilt, "zoom": zoom})
    assert result["called"]
    position = settled_position(world)
    settled_device_position(world, device)
    return position


def near(a, b, tolerance=POSITION_TOLERANCE):
    return all(abs(x - y) <= tolerance for x, y in zip(a, b))


class Presets:
    """The presets a test made, deleted again at the end: the camera is shared
    between all the tests"""

    def __init__(self, world, device):
        self.world = world
        self.device = device
        self.ids = []

    def create(self, name="", store="camera"):
        """The id of the preset made, once the device says it is, or "" if it made none"""
        preset_id = self.world.create_preset(self.device, name, store)
        if preset_id:
            self.ids.append(preset_id)
        return preset_id

    def call(self, proc, **args):
        result = self.world.call_proc(self.device, proc, args)
        assert result["called"], f"the device has no {proc}"
        return result

    def update(self, preset_id, **changes):
        result = self.world.call_proc(self.device, "ptz_preset_update", {"id": preset_id, "changes": changes})
        assert result["called"], "the device has no ptz_preset_update"

    def cleanup(self):
        for preset_id in self.ids:
            self.world.call_proc(self.device, "ptz_preset_delete", {"id": preset_id})


@pytest.fixture
def made(obs_world, request):
    """made(device) -> Presets, for the device by its name in the suite"""
    created = []

    def make(backend):
        presets = Presets(obs_world, obs_world.device_names[backend])
        created.append(presets)
        return presets

    yield make
    for presets in created:
        presets.cleanup()


# ---------------------------------------------------------------- the list


def test_visca_says_what_its_camera_keeps(obs_world):
    info = preset_list(obs_world, obs_world.device_names["visca-tcp"])
    assert set(info["stores"]) == {"camera", "local"}
    # VISCA has slots, and no names for them, and no way to ask which are used
    assert info["names_on_camera"] is False
    assert info["enumerable"] is False
    assert "camera_slots" not in info
    assert {"pan", "tilt", "zoom"} <= set(info["value_keys"])


def test_onvif_says_what_its_camera_keeps(obs_world):
    info = preset_list(obs_world, obs_world.device_names["onvif"])
    assert set(info["stores"]) == {"camera", "local"}
    # ONVIF names its presets itself, and says what it has
    assert info["names_on_camera"] is True
    assert info["enumerable"] is True
    assert "camera_slots" not in info


# ------------------------------------------------------ the camera's store


def test_a_visca_camera_preset_has_its_slot_in_its_id(obs_world, made):
    device = obs_world.device_names["visca-tcp"]
    presets = made("visca-tcp")
    preset_id = presets.create("Wide shot", store="camera")

    store, _, slot = preset_id.partition(":")
    assert store == "camera" and slot.isdigit()
    obs_world.wait_for(lambda: slot in obs_world.state()["presets"])
    preset = presets_by_id(obs_world, device)[preset_id]
    assert preset["store"] == "camera"
    # VISCA can't keep a name, so the device does
    assert preset["name"] == "Wide shot"
    assert preset["camera_name"] == ""


def test_a_visca_camera_with_all_its_slots_used_makes_no_more(obs_world, made):
    presets = made("visca-tcp")
    # preset_max is 16 unless set
    made_ids = [presets.create(f"Slot {n}", store="camera") for n in range(16)]
    assert all(made_ids) and len(set(made_ids)) == 16
    assert presets.create("One too many", store="camera") == ""
    # a local preset has no such limit
    assert presets.create("Local", store="local") != ""


@pytest.mark.parametrize("backend,store", [("visca-tcp", "camera"), ("onvif", "camera"), ("visca-udp", "local")])
def test_a_preset_is_asked_for_and_made_when_the_device_says_so(obs_world, made, backend, store):
    """ptz_preset_create does not wait for the camera: it returns the request, which is
    not a preset's id, and the preset is there when ptz_preset_create_done says so, after
    ptz_preset_added has announced it"""
    device = obs_world.device_names[backend]
    presets = made(backend)
    obs_world.record_preset_signals(device)

    result = obs_world.call_proc(device, "ptz_preset_create", {"name": "Asked for", "store": store},
                                 returns="string")
    request = result["return"]
    assert request and ":" not in request, "the request is not a preset id"

    def done_events():
        return [e for e in obs_world.preset_signals(with_create_done=True)
                if e["signal"] == "ptz_preset_create_done" and e["request"] == request]

    obs_world.wait_for(lambda: done_events())
    preset_id = done_events()[0]["id"]
    presets.ids.append(preset_id)
    assert preset_id.startswith(f"{store}:")
    signals = [e["signal"] for e in obs_world.preset_signals(with_create_done=True)]
    assert signals.index("ptz_preset_added") < signals.index("ptz_preset_create_done")
    assert presets_by_id(obs_world, device)[preset_id]["name"] == "Asked for"


def test_requests_made_together_are_made_in_different_slots(obs_world, made):
    """The slot a request has is not in the list until it is made, but no other request
    has it in the meantime"""
    device = obs_world.device_names["visca-tcp"]
    presets = made("visca-tcp")
    obs_world.record_preset_signals(device)
    requests = []
    for n in range(3):
        result = obs_world.call_proc(device, "ptz_preset_create", {"name": f"Together {n}", "store": "camera"},
                                     returns="string")
        assert result["return"]
        requests.append(result["return"])

    def ids():
        done = {e["request"]: e["id"] for e in obs_world.preset_signals(with_create_done=True)
                if e["signal"] == "ptz_preset_create_done"}
        return [done.get(r) for r in requests]

    obs_world.wait_for(lambda: all(ids()))
    made_ids = ids()
    presets.ids.extend(made_ids)
    assert len(set(made_ids)) == 3


def test_a_request_the_device_refuses_at_once_gets_no_request_and_no_signal(obs_world, made):
    device = obs_world.device_names["visca-tcp"]
    presets = made("visca-tcp")
    for n in range(16):
        assert presets.create(f"Slot {n}", store="camera")
    obs_world.record_preset_signals(device)

    result = obs_world.call_proc(device, "ptz_preset_create", {"name": "No room", "store": "camera"},
                                 returns="string")
    unknown = obs_world.call_proc(device, "ptz_preset_create", {"name": "Nowhere", "store": "cloud"},
                                  returns="string")

    assert result["return"] == "" and unknown["return"] == ""
    time.sleep(1)
    # (the thumbnails of the presets made above may still be arriving, which is not it)
    events = obs_world.preset_signals(with_create_done=True)
    assert [e for e in events if e["signal"] in ("ptz_preset_added", "ptz_preset_create_done")] == []


def test_an_onvif_camera_preset_has_its_token_in_its_id(obs_world, made):
    device = obs_world.device_names["onvif"]
    presets = made("onvif")
    preset_id = presets.create("Wide shot", store="camera")

    store, _, token = preset_id.partition(":")
    assert store == "camera" and token
    obs_world.wait_for(lambda: token in obs_world.state()["presets"])
    # the camera has the name
    assert obs_world.state()["presets"][token]["name"] == "Wide shot"
    preset = presets_by_id(obs_world, device)[preset_id]
    assert preset["name"] == "Wide shot"
    assert preset["camera_name"] == "Wide shot"


@pytest.mark.parametrize("backend", ["visca-tcp", "onvif"])
def test_a_camera_preset_is_recalled_and_deleted_on_the_camera(obs_world, made, backend):
    device = obs_world.device_names[backend]
    presets = made(backend)
    saved = go_to(obs_world, device, 0.3, 0.2, 0.4)
    preset_id = presets.create("There", store="camera")
    key = preset_id.partition(":")[2]
    obs_world.wait_for(lambda: key in obs_world.state()["presets"])

    away = go_to(obs_world, device, -0.3, -0.2, 0.1)
    assert not near(saved, away, 0.1), "the camera didn't move away"
    presets.call("ptz_preset_recall", id=preset_id)
    obs_world.wait_for(lambda: near(position(obs_world), saved), timeout=10)

    presets.call("ptz_preset_delete", id=preset_id)
    obs_world.wait_for(lambda: key not in obs_world.state()["presets"])
    assert preset_id not in presets_by_id(obs_world, device)


# --------------------------------------------------------- the local store


@pytest.mark.parametrize("backend", ["visca-tcp", "visca-udp", "onvif"])
def test_a_local_preset_is_recalled_by_the_device(obs_world, made, backend):
    device = obs_world.device_names[backend]
    presets = made(backend)
    cameras_before = set(obs_world.state()["presets"])
    saved = go_to(obs_world, device, 0.3, 0.2, 0.4)
    preset_id = presets.create("Mine", store="local")

    store, _, key = preset_id.partition(":")
    assert store == "local" and key
    assert presets_by_id(obs_world, device)[preset_id]["store"] == "local"
    # the camera never heard of it
    assert set(obs_world.state()["presets"]) == cameras_before

    away = go_to(obs_world, device, -0.3, -0.2, 0.1)
    assert not near(saved, away, 0.1), "the camera didn't move away"
    presets.call("ptz_preset_recall", id=preset_id)
    obs_world.wait_for(lambda: near(position(obs_world), saved), timeout=10)


def test_a_local_preset_saves_every_value(obs_world, made):
    device = obs_world.device_names["visca-tcp"]
    presets = made("visca-tcp")
    go_to(obs_world, device, 0.3, 0.2, 0.4)
    preset_id = presets.create("Mine", store="local")

    preset = presets_by_id(obs_world, device)[preset_id]
    assert {"pan", "tilt", "zoom", "focus"} <= set(preset["values"])
    # VISCA can't go to a focus position, but it can turn autofocus on and off
    assert set(preset["values"]["focus"]) == {"af_enabled"}


def test_recall_applies_the_values_a_preset_has(obs_world, made):
    device = obs_world.device_names["visca-tcp"]
    presets = made("visca-tcp")
    a = go_to(obs_world, device, 0.3, 0.2, 0.4)
    preset_id = presets.create("Mine", store="local")
    b = go_to(obs_world, device, -0.3, -0.2, 0.1)

    # a preset with only the pan: the tilt and zoom stay where they are
    pan = presets_by_id(obs_world, device)[preset_id]["values"]["pan"]
    presets.update(preset_id, values={"pan": pan})
    presets.call("ptz_preset_recall", id=preset_id)
    obs_world.wait_for(lambda: abs(position(obs_world)[0] - a[0]) < POSITION_TOLERANCE, timeout=10)
    now = settled_position(obs_world)
    assert abs(now[1] - b[1]) < POSITION_TOLERANCE and abs(now[2] - b[2]) < POSITION_TOLERANCE


def test_the_values_of_a_preset_can_change_without_saving_again(obs_world, made):
    device = obs_world.device_names["visca-tcp"]
    presets = made("visca-tcp")
    go_to(obs_world, device, 0.3, 0.2, 0.4)
    preset_id = presets.create("Mine", store="local")
    obs_world.record_preset_signals(device)

    presets.update(preset_id, values={"zoom": 0.25})

    assert presets_by_id(obs_world, device)[preset_id]["values"] == {"zoom": 0.25}
    changed = [e for e in obs_world.preset_signals() if e["signal"] == "ptz_preset_changed" and "values" in e["changed"]]
    assert [(e["id"], e["changed"]["values"]) for e in changed] == [(preset_id, {"zoom": 0.25})]


@pytest.mark.parametrize("store", ["the-cloud", "controller"])
def test_a_preset_cant_be_made_in_a_store_the_device_has_not(obs_world, made, store):
    """"controller" was the name of the local store, which it no longer is"""
    device = obs_world.device_names["visca-tcp"]
    presets = made("visca-tcp")
    before = list(presets_by_id(obs_world, device))
    assert presets.create("Nowhere", store=store) == ""
    assert list(presets_by_id(obs_world, device)) == before


def test_the_stores_are_named_camera_and_local(obs_world, made):
    device = obs_world.device_names["visca-tcp"]
    presets = made("visca-tcp")
    camera = presets.create("On the camera", store="camera")
    local = presets.create("On the device", store="local")

    info = preset_list(obs_world, device)
    assert set(info["stores"]) == {"camera", "local"}
    assert (info["presets"][camera]["store"], info["presets"][local]["store"]) == ("camera", "local")
    # and the store is the first part of the id
    assert (camera.partition(":")[0], local.partition(":")[0]) == ("camera", "local")


def test_recalling_an_id_the_device_has_not_moves_nothing(obs_world, made):
    device = obs_world.device_names["visca-tcp"]
    presets = made("visca-tcp")
    here = go_to(obs_world, device, 0.3, 0.2, 0.4)
    presets.call("ptz_preset_recall", id="local:nothing-of-the-sort")
    presets.call("ptz_preset_recall", id="camera:99")
    time.sleep(0.5)
    assert near(position(obs_world), here, 0.005)


# -------------------------------------------------- update, move and signals


def test_update_renames_and_announces_the_change(obs_world, made):
    device = obs_world.device_names["visca-tcp"]
    presets = made("visca-tcp")
    preset_id = presets.create("Before", store="local")
    obs_world.record_preset_signals(device)

    presets.update(preset_id, name="After")

    assert presets_by_id(obs_world, device)[preset_id]["name"] == "After"
    # (a thumbnail of the source can change meanwhile, which is a change too)
    changed = [e for e in obs_world.preset_signals() if e["signal"] == "ptz_preset_changed" and "name" in e["changed"]]
    assert [(e["id"], e["changed"]["name"]) for e in changed] == [(preset_id, "After")]


def test_a_rename_reaches_a_camera_that_keeps_names(obs_world, made):
    device = obs_world.device_names["onvif"]
    presets = made("onvif")
    preset_id = presets.create("Before", store="camera")
    token = preset_id.partition(":")[2]
    obs_world.wait_for(lambda: token in obs_world.state()["presets"])

    presets.update(preset_id, name="After")

    obs_world.wait_for(lambda: obs_world.state()["presets"][token]["name"] == "After")
    assert presets_by_id(obs_world, device)[preset_id]["camera_name"] == "After"


def test_move_reorders_the_list_over_both_stores(obs_world, made):
    device = obs_world.device_names["visca-tcp"]
    presets = made("visca-tcp")
    first = presets.create("First", store="local")
    second = presets.create("Second", store="camera")
    third = presets.create("Third", store="local")
    obs_world.record_preset_signals(device)

    presets.call("ptz_preset_move", id=third, index=0)

    order = order_of(obs_world, device)
    assert order.index(third) == 0
    assert order.index(first) < order.index(second)
    # and the presets themselves are the same, in no order
    assert set(order) == set(presets_by_id(obs_world, device))
    # (not the change of a thumbnail, which the source's video can make meanwhile)
    assert [e["signal"] for e in obs_world.preset_signals() if e["signal"] != "ptz_preset_changed"] == [
        "ptz_preset_order_changed"
    ]


def test_adding_and_deleting_announce_the_id(obs_world, made):
    device = obs_world.device_names["visca-tcp"]
    presets = made("visca-tcp")
    obs_world.record_preset_signals(device)

    preset_id = presets.create("Mine", store="local")
    assert order_of(obs_world, device)[-1] == preset_id
    presets.call("ptz_preset_delete", id=preset_id)
    assert preset_id not in order_of(obs_world, device)

    # (not the change of a thumbnail, which the source's video can make meanwhile)
    events = [(e["signal"], e["id"]) for e in obs_world.preset_signals()
              if e["signal"] in ("ptz_preset_added", "ptz_preset_removed")]
    assert events == [("ptz_preset_added", preset_id), ("ptz_preset_removed", preset_id)]


def test_refresh_reads_the_cameras_presets_again(obs_world, made, ptzsim_process):
    device = obs_world.device_names["onvif"]
    presets = made("onvif")
    obs_world.record_preset_signals(device)

    # something other than the plugin makes a preset on the camera
    envelope = (
        '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"><s:Body>'
        '<tptz:SetPreset xmlns:tptz="http://www.onvif.org/ver20/ptz/wsdl">'
        "<tptz:ProfileToken>profile_1</tptz:ProfileToken><tptz:PresetName>Behind its back</tptz:PresetName>"
        "</tptz:SetPreset></s:Body></s:Envelope>")
    request = urllib.request.Request(f"http://127.0.0.1:{ptzsim_process['ports']['onvif_http']}/onvif/ptz_service",
                                     data=envelope.encode(), headers={"Content-Type": "application/soap+xml"})
    urllib.request.urlopen(request, timeout=5).read()
    token = next(t for t, p in obs_world.state()["presets"].items() if p["name"] == "Behind its back")
    presets.ids.append(f"camera:{token}")

    presets.call("ptz_preset_refresh")

    obs_world.wait_for(lambda: f"camera:{token}" in presets_by_id(obs_world, device))
    assert "ptz_preset_list_reset" in [e["signal"] for e in obs_world.preset_signals()]


# ------------------------------------------------------- what is saved


def test_local_presets_are_saved_in_the_sources_settings(obs_world, made, tmp_path):
    device = obs_world.device_names["visca-tcp"]
    presets = made("visca-tcp")
    go_to(obs_world, device, 0.3, 0.2, 0.4)
    preset_id = presets.create("Mine", store="local")

    saved = obs_world.device_settings(device, tmp_path / "settings.json")["saved"]
    record = {p["id"]: p for p in saved["presets"]}[preset_id]
    assert record["name"] == "Mine"
    assert {"pan", "tilt", "zoom"} <= set(record["values"])


def test_what_the_camera_keeps_is_not_saved(obs_world, made, ptzsim_process, tmp_path):
    """The camera has the preset, its position and its name, and the device
    asks it: the settings have only what the device added, a thumbnail, and
    nothing of the camera's, its name least"""
    device = obs_world.device_names["onvif"]
    presets = made("onvif")
    preset_id = presets.create("On the camera", store="camera")

    saved = obs_world.device_settings(device, tmp_path / "settings.json")["saved"]
    for record in saved["presets"]:
        assert set(record) <= {"id", "name", "thumbnail"}, record
    assert "On the camera" not in json.dumps(saved["presets"])
    assert all("name" not in r for r in saved["presets"] if r["id"] == preset_id)


def test_the_names_a_camera_cant_keep_are_saved(obs_world, made, tmp_path):
    device = obs_world.device_names["visca-tcp"]
    presets = made("visca-tcp")
    preset_id = presets.create("Wide shot", store="camera")

    saved = obs_world.device_settings(device, tmp_path / "settings.json")["saved"]
    record = {p["id"]: p for p in saved["presets"]}[preset_id]
    assert record["name"] == "Wide shot"
    assert "values" not in record


def test_the_presets_of_an_older_version_are_read_as_annotations(obs_world, cameras, tmp_path):  # noqa: F811
    """Version 0.1 saved an int id for each preset, which is VISCA's slot"""
    cameras.add_source(obs_world.create_scene(), "legacy-presets-cam")
    obs_world.ws.call("CreateSourceFilter", {
        "sourceName": "legacy-presets-cam",
        "filterName": "PTZ",
        "filterKind": FILTER_KIND,
        "filterSettings": {
            "type": "visca-over-tcp",
            "host": "127.0.0.1",
            "tcp_port": cameras.sim.tcp_port,
            "presets": [{"id": 3, "name": "Wide"}, {"id": 7, "name": "Door"}],
        },
    })
    out = tmp_path / "device.json"
    uuid = obs_world.wait_for_device_by_name("legacy-presets-cam", out, lambda r: r["found"] and r["bound"])["uuid"]

    info = preset_list(obs_world, uuid)
    # in the order they were saved in
    assert order_of(obs_world, uuid) == ["camera:3", "camera:7"]
    assert [(i, info["presets"][i]["name"]) for i in ("camera:3", "camera:7")] == [("camera:3", "Wide"), ("camera:7", "Door")]


# --------------------------------------------------------- action source


def test_the_action_source_names_a_preset_by_its_id(obs_world):
    device = obs_world.device_names["visca-tcp"]
    saved = go_to(obs_world, device, 0.3, 0.2, 0.4)
    obs_world.trigger_action(device, ACTION_PRESET_SAVE, preset_id="camera:5")
    obs_world.wait_for(lambda: "5" in obs_world.state()["presets"])

    away = go_to(obs_world, device, -0.3, -0.2, 0.1)
    assert not near(saved, away, 0.1), "the camera didn't move away"
    obs_world.trigger_action(device, ACTION_PRESET_RECALL, preset_id="camera:5")
    obs_world.wait_for(lambda: near(position(obs_world), saved), timeout=10)

    obs_world.call_proc(device, "ptz_preset_delete", {"id": "camera:5"})
