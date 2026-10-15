"""Covers the PTZ API's signals over obs-websocket ("WebSocket binding" in
docs/ptz-device-api.md, src/controller/ptz-websocket.cpp): each signal of a
device, and of a discovery instance, as an event of the "obs-ptz" vendor that
names the device it is from, and the binding's own event for a device that has
gone.

Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
SPDX-License-Identifier: GPL-2.0-or-later
"""

import time

from test_filter_devices import camera_sim, cameras  # noqa: F401
from test_websocket_api import VENDOR, access, device, devices  # noqa: F401

ACTION_PAN_TILT = 3
ACTION_STOP = 4


def ptz_event(name, **fields):
    """A predicate for wait_event(): the vendor's event `name` whose data has `fields`"""
    def matches(event):
        data = event.get("eventData") or {}
        return (event.get("vendorName") == VENDOR and event.get("eventType") == name
                and all(data.get(key) == value for key, value in fields.items()))
    return matches


def forget_events(obs_world):
    """Drops what has come so far, so that what is waited for next was caused by the test"""
    obs_world.events.events("VendorEvent", settle=0.3)


def ptz_events(obs_world, settle=1.0):
    """The vendor's events for the API's signals that have come, and come in the next `settle` seconds"""
    return [e for e in obs_world.events.events("VendorEvent", settle=settle)
            if e.get("vendorName") == VENDOR and e["eventType"].startswith("ptz_")
            and e["eventType"] != "ptz_probe" and "device" in (e.get("eventData") or {})]


def test_a_state_change_is_an_event_that_names_its_device(obs_world):
    uuid = device(obs_world, "visca-tcp")
    obs_world.ws.vendor(VENDOR, "ptz_move_abs", {"device": uuid, "pan": 0.0, "tilt": 0.0})
    time.sleep(1)
    forget_events(obs_world)
    obs_world.ws.vendor(VENDOR, "ptz_move_abs", {"device": uuid, "pan": 0.4, "tilt": 0.0})
    event = obs_world.events.wait_event(
        "VendorEvent", lambda e: ptz_event("ptz_state_changed", device=uuid)(e) and "pan" in e["eventData"]["changed"])
    data = event["eventData"]
    # the source is told as the device, and what is not data is not sent
    assert set(data) == {"device", "changed"}
    assert isinstance(data["changed"]["pan"], (int, float))


def test_preset_create_done_arrives_whichever_side_of_the_answer_it_is_on(obs_world):
    uuid = device(obs_world, "visca-tcp")
    forget_events(obs_world)
    # The same connection as the events are on: the event may come before the answer
    request = obs_world.events.vendor(VENDOR, "ptz_preset_create",
                                      {"device": uuid, "name": "over the wire", "store": "camera"})["return"]
    assert request
    done = obs_world.events.wait_event("VendorEvent", ptz_event("ptz_preset_create_done", device=uuid,
                                                                request=request))["eventData"]
    assert done["id"]
    listed = obs_world.ws.vendor(VENDOR, "ptz_preset_get_list", {"device": uuid})["return"]["presets"]
    assert listed[done["id"]]["name"] == "over the wire"
    obs_world.ws.vendor(VENDOR, "ptz_preset_delete", {"device": uuid, "id": done["id"]})
    obs_world.events.wait_event("VendorEvent", ptz_event("ptz_preset_removed", device=uuid, id=done["id"]))


def test_a_new_device_is_bridged_and_a_removed_one_is_announced(obs_world, cameras):  # noqa: F811
    scene = obs_world.create_scene()
    forget_events(obs_world)
    cameras.add_source(scene, "ws-event-cam")
    cameras.add_filter("ws-event-cam")
    obs_world.wait_for(lambda: "ws-event-cam" in devices(obs_world))
    uuid = devices(obs_world)["ws-event-cam"]["device"]
    assert obs_world.ws.vendor(VENDOR, "ptz_get_api_version", {"device": uuid})["major"] == 0
    # what it says of itself as it starts is heard, without anything being asked of it
    obs_world.events.wait_event("VendorEvent", ptz_event("ptz_state_changed", device=uuid))

    cameras.remove_filter("ws-event-cam")
    removed = obs_world.events.wait_event("VendorEvent", ptz_event("ptz_device_removed", device=uuid))
    assert removed["eventData"] == {"device": uuid}
    obs_world.wait_for(lambda: "ws-event-cam" not in devices(obs_world))


def test_selecting_another_camera_in_the_dock_is_an_event(obs_world):
    first, second = device(obs_world, "visca-tcp"), device(obs_world, "visca-udp")
    obs_world.select_in_dock(first)
    forget_events(obs_world)
    obs_world.select_in_dock(second)
    changed = obs_world.events.wait_event("VendorEvent", ptz_event("ptz_selected_changed", device=second))
    assert changed["eventData"] == {"device": second}
    # selecting the one that is selected changes nothing, and says nothing
    obs_world.select_in_dock(second)
    assert not [e for e in obs_world.events.events("VendorEvent", settle=0.5)
                if e.get("eventType") == "ptz_selected_changed"]


def test_a_discovery_instance_says_when_it_has_finished(obs_world):
    instances = obs_world.ws.vendor(VENDOR, "ptz_list_discovery")["instances"]
    forget_events(obs_world)
    for instance in instances:
        obs_world.ws.vendor(VENDOR, "ptz_discovery_start", {"device": instance["device"]})
    # In whatever order they finish: an event that is not the one waited for is dropped
    waiting = {instance["device"] for instance in instances}
    while waiting:
        finished = obs_world.events.wait_event(
            "VendorEvent", lambda e: ptz_event("ptz_discovery_finished")(e) and e["eventData"]["device"] in waiting,
            timeout=30)
        assert set(finished["eventData"]) == {"device"}
        waiting.discard(finished["eventData"]["device"])


def move_by_action(obs_world):
    """Moves a camera and stops it, without the websocket binding: as a scene does"""
    name = obs_world.device_names["visca-tcp"]
    obs_world.trigger_action(name, ACTION_PAN_TILT, pan_speed=0.5, tilt_speed=0.0)
    obs_world.wait_for_state(lambda s: s["pan_speed"] > 0.4)
    time.sleep(0.5)
    obs_world.trigger_action(name, ACTION_STOP)
    obs_world.wait_for_state(lambda s: s["pan_speed"] == 0.0)


def test_access_off_sends_no_event_and_read_only_sends_them(obs_world, access):  # noqa: F811
    uuid = device(obs_world, "visca-tcp")
    access("off")
    forget_events(obs_world)
    move_by_action(obs_world)
    assert ptz_events(obs_world) == []

    access("read-only")
    forget_events(obs_world)
    move_by_action(obs_world)
    assert any(e["eventData"]["device"] == uuid for e in ptz_events(obs_world))
