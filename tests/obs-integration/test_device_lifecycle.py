"""Covers ptz_device_remove as the one signal that a device has gone (see
"Finding devices" in docs/ptz-device-api.md): a device says it when its filter
is taken off its source and when its source is deleted, even while something
holds a reference that keeps OBS from destroying it, and says ptz_device_add
again when the filter is put back.

A controller that holds a strong reference to a device's source, as an
NDI-style one does, never hears its "destroy" while it holds it. The UI test
harness holds one here (tests/ui-harness/device-lifecycle-test.cpp), so that
nothing but ptz_device_remove can tell the dock or the websocket binding that
the device has gone: OBS's own "destroy" can't make these pass.
"""

import itertools
import json

import pytest
from conftest import OBS_MAIN, PTZ_DOCK
from obsws import ObsWebSocketError

VIRTUAL_KIND = "ca.secretlab.obs-ptz.virtual-ptz"
FILTER = "Virtual PTZ"

# A name of its own for each test's camera, so that no backup is another's
_cameras = itertools.count(1)


@pytest.fixture
def camera(obs_world):
    """A source with a virtual PTZ filter, and the scene it is in. Whatever
    the harness held is let go of at the end, and the source removed."""
    obs_world.need_harness()
    obs_world.need_ui_driver()
    name = f"lifecycle-cam-{next(_cameras)}"
    scene = obs_world.create_scene()
    item = obs_world.ws.call("CreateInput", {
        "sceneName": scene,
        "inputName": name,
        "inputKind": "color_source_v3",
        "inputSettings": {},
    })["sceneItemId"]
    obs_world.ws.call("CreateSourceFilter", {
        "sourceName": name,
        "filterName": FILTER,
        "filterKind": VIRTUAL_KIND,
        "filterSettings": {},
    })
    yield {"name": name, "scene": scene, "item": item}
    obs_world.run_ui_test("release_held")
    for call, args in (("RemoveSceneItem", {"sceneName": scene, "sceneItemId": item}),
                       ("RemoveInput", {"inputName": name})):
        try:
            obs_world.ws.call(call, args)
        except ObsWebSocketError:
            pass


def said(event, uuid):
    return lambda e: e.get("eventType") == event and e.get("eventData", {}).get("device") == uuid


def in_dock(world, uuid):
    rows, _, _ = world.ui.rows(OBS_MAIN, PTZ_DOCK, "deviceList", roles=["uuid"])
    return uuid in [r["uuid"] for r in rows]


def listed(world, uuid):
    return uuid in [d["device"] for d in world.ws.vendor("obs-ptz", "ptz_list_devices")["devices"]]


def wait_for_device(world, camera, tmp_path):
    uuid = world.wait_for_device_by_name(camera["name"], tmp_path / "device.json", lambda r: r["found"])["uuid"]
    world.wait_for(lambda: in_dock(world, uuid))
    return uuid


def assert_gone(world, uuid):
    """The binding said so, the dock dropped it, and the binding no longer lists it"""
    world.events.wait_event("VendorEvent", said("ptz_device_remove", uuid))
    world.wait_for(lambda: not in_dock(world, uuid))
    assert not listed(world, uuid)


def test_taking_a_held_filter_off_its_source_removes_the_device(obs_world, camera, tmp_path):
    uuid = wait_for_device(obs_world, camera, tmp_path)
    obs_world.run_ui_test("hold_source", source=camera["name"], filter=FILTER)

    obs_world.ws.call("RemoveSourceFilter", {"sourceName": camera["name"], "filterName": FILTER})

    assert_gone(obs_world, uuid)


def test_putting_a_filter_back_brings_the_device_back(obs_world, camera, tmp_path):
    """The same filter, which OBS has not destroyed, and whose device was built
    the first time it was put on a source"""
    uuid = wait_for_device(obs_world, camera, tmp_path)
    obs_world.run_ui_test("hold_source", source=camera["name"], filter=FILTER)

    for _ in range(2):
        obs_world.ws.call("RemoveSourceFilter", {"sourceName": camera["name"], "filterName": FILTER})
        assert_gone(obs_world, uuid)

        obs_world.run_ui_test("add_held_filter", source=camera["name"])

        obs_world.events.wait_event("VendorEvent", said("ptz_device_add", uuid))
        obs_world.wait_for(lambda: in_dock(obs_world, uuid))
        assert listed(obs_world, uuid)


def test_deleting_a_held_source_removes_its_device(obs_world, camera, tmp_path):
    uuid = wait_for_device(obs_world, camera, tmp_path)
    obs_world.run_ui_test("hold_source", source=camera["name"])

    obs_world.ws.call("RemoveSceneItem", {"sceneName": camera["scene"], "sceneItemId": camera["item"]})
    obs_world.ws.call("RemoveInput", {"inputName": camera["name"]})

    assert_gone(obs_world, uuid)


def test_a_remove_and_then_destroy_leave_one_backup(obs_world, camera, tmp_path):
    """The device says ptz_device_remove as its filter comes off, and its filter
    is destroyed later, when the harness lets go of it: one backup, as the
    filter was when it came off"""
    file = obs_world.config_dir / "device-backups.json"

    def backups():
        entries = json.loads(file.read_text(encoding="utf-8"))["devices"] if file.exists() else []
        return [b for b in entries if b["name"] == camera["name"]]

    uuid = wait_for_device(obs_world, camera, tmp_path)
    obs_world.run_ui_test("hold_source", source=camera["name"], filter=FILTER)
    obs_world.ws.call("RemoveSourceFilter", {"sourceName": camera["name"], "filterName": FILTER})
    assert_gone(obs_world, uuid)
    obs_world.wait_for(lambda: len(backups()) == 1)
    removed = backups()[0]

    # By its answer, OBS has destroyed the filter
    obs_world.run_ui_test("release_held")

    after = backups()
    assert len(after) == 1
    assert {k: v for k, v in after[0].items() if k != "backup_time"} == \
        {k: v for k, v in removed.items() if k != "backup_time"}


def test_disabling_the_filter_removes_the_device_until_it_is_enabled(obs_world, camera, tmp_path):
    """Its checkbox in OBS's Filters dialog: OBS says "enable", and not "update",
    which the dock reads a device's state on"""
    uuid = wait_for_device(obs_world, camera, tmp_path)

    def set_enabled(enabled):
        obs_world.ws.call("SetSourceFilterEnabled", {
            "sourceName": camera["name"], "filterName": FILTER, "filterEnabled": enabled})

    for _ in range(2):
        set_enabled(False)
        assert_gone(obs_world, uuid)

        set_enabled(True)
        obs_world.events.wait_event("VendorEvent", said("ptz_device_add", uuid))
        obs_world.wait_for(lambda: in_dock(obs_world, uuid))
        assert listed(obs_world, uuid)
