"""Covers the rolling backup of devices that have gone away, and adding and
removing devices from the settings dialog.

Deleting a source takes its "PTZ Control" filter along, and with it the
device's presets. So the controller keeps every device's settings in a backup
as OBS removes or destroys its filter, or removes its source
(src/controller/ptz-backup-store.cpp), and the settings
dialog's Add, which adds a filter to a source rather than a device of the
plugin's own, offers the backup of a device that was on a source of the same
name. Its Remove removes a filter's device by removing the filter.

The dialog is driven through tests/ui-harness/device-backup-test.cpp, which
answers the add dialog and the remove question as a user would. Filters are
pointed at a port nothing listens on: none of this needs a camera.
"""

import json
import time

import pytest
from obsws import ObsWebSocketError

from conftest import free_port

FILTER_KIND = "ca.secretlab.obs-ptz.visca"
SOFT_KIND = "ca.secretlab.obs-ptz.soft-ptz"

PRESETS = [{"id": 0, "name": "Wide"}, {"id": 1, "name": "Close"}]


class Sources:
    """Empty media sources standing in for cameras (see Cameras in
    test_filter_devices.py for why a media source), removed at the end."""

    def __init__(self, world):
        self.world = world
        self.scene = world.create_scene()
        self.names = set()
        self.port = free_port()

    def add(self, name, scene=None):
        self.world.ws.call("CreateInput", {
            "sceneName": scene or self.scene,
            "inputName": name,
            "inputKind": "ffmpeg_source",
            "inputSettings": {"is_local_file": True, "local_file": ""},
        })
        self.names.add(name)

    def remove(self, name):
        self.world.ws.call("RemoveInput", {"inputName": name})
        self.names.discard(name)

    def add_soft_filter(self, source):
        self.world.ws.call("CreateSourceFilter", {
            "sourceName": source,
            "filterName": "Soft PTZ",
            "filterKind": SOFT_KIND,
            "filterSettings": {},
        })

    def add_filter(self, source, presets=PRESETS):
        self.world.ws.call("CreateSourceFilter", {
            "sourceName": source,
            "filterName": "PTZ",
            "filterKind": FILTER_KIND,
            "filterSettings": {
                "type": "visca-over-tcp",
                "host": "127.0.0.1",
                "tcp_port": self.port,
                "presets": presets,
            },
        })

    def filters(self, source):
        return self.world.ws.call("GetSourceFilterList", {"sourceName": source})["filters"]

    def cleanup(self):
        for name in list(self.names):
            try:
                self.world.ws.call("RemoveInput", {"inputName": name})
            except ObsWebSocketError:
                pass


@pytest.fixture
def sources(obs_world):
    s = Sources(obs_world)
    yield s
    s.cleanup()


def backups(world, out):
    if out.exists():
        out.unlink()
    world.run_ui_test("get_device_backups", filename=str(out))
    world.wait_for(out.exists)
    return json.loads(out.read_text())["devices"]


def wait_for_backup(world, out, name):
    found = []

    def check():
        found[:] = [b for b in backups(world, out) if b["name"] == name]
        return bool(found)

    world.wait_for(check)
    return found[0]


def add_device(world, out, source, **choice):
    """Adds a device to `source` through the settings dialog's Add, choosing
    type= a protocol's new device type or restore= the source name of a
    backup, or, with neither, what the dialog picks for the source."""
    if out.exists():
        out.unlink()
    world.run_ui_test("add_device", filename=str(out), source=source, **choice)
    world.wait_for(out.exists)
    result = json.loads(out.read_text())
    result["sources"] = [s["name"] for s in result.get("sources", [])]
    result["headings"] = [s["name"] for s in result.get("choices", []) if s["heading"]]
    result["choices"] = [s["name"] for s in result.get("choices", []) if not s["heading"]]
    return result


def preset_names(world, device_name, out):
    saved = world.device_settings(device_name, out)["saved"]
    return [p.get("name") for p in saved.get("presets", [])]


def test_removing_a_source_keeps_a_backup_of_its_device(obs_world, sources, tmp_path):
    sources.add("backup-cam-deleted")
    sources.add_filter("backup-cam-deleted")
    obs_world.wait_for_device_by_name("backup-cam-deleted", tmp_path / "device.json", lambda r: r["found"])

    sources.remove("backup-cam-deleted")

    backup = wait_for_backup(obs_world, tmp_path / "backups.json", "backup-cam-deleted")
    assert backup["type"] == "visca-over-tcp"
    assert backup["tcp_port"] == sources.port
    assert [p["name"] for p in backup["presets"]] == ["Wide", "Close"]
    assert backup["backup_time"] > 0
    assert "id" not in backup


def test_add_restores_the_backup_of_a_deleted_source(obs_world, sources, tmp_path):
    scene = obs_world.create_scene()
    sources.add("backup-cam-restore", scene)
    sources.add_filter("backup-cam-restore")
    obs_world.wait_for_device_by_name("backup-cam-restore", tmp_path / "device.json", lambda r: r["found"])
    sources.remove("backup-cam-restore")
    wait_for_backup(obs_world, tmp_path / "backups.json", "backup-cam-restore")
    obs_world.ws.call("RemoveScene", {"sceneName": scene})

    # The camera's source is made again, with the same name, once OBS has let
    # go of the deleted one. OBS can hold on to the deleted source, and its
    # filter's device, for a while: that device, bound to nothing, mustn't
    # stand in the way.
    def recreate():
        sources.add("backup-cam-restore")
        return True
    obs_world.wait_for(recreate, timeout=20, interval=0.5)
    result = add_device(obs_world, tmp_path / "add.json", "backup-cam-restore")
    assert "backup-cam-restore" in result["sources"]
    assert "backup-cam-restore" in result["chosen"]

    filters = sources.filters("backup-cam-restore")
    assert [f["filterKind"] for f in filters] == [FILTER_KIND]
    device = obs_world.wait_for_device_by_name("backup-cam-restore", tmp_path / "device.json",
                                               lambda r: r["found"] and r["bound"])
    assert preset_names(obs_world, device["uuid"], tmp_path / "settings.json") == ["Wide", "Close"]
    settings = filters[0]["filterSettings"]
    assert settings["type"] == "visca-over-tcp"
    assert settings["tcp_port"] == sources.port
    # The filter knows its source; the backup's identity isn't carried over
    assert "name" not in settings and "backup_time" not in settings


def test_add_creates_a_filter_from_defaults(obs_world, sources, tmp_path):
    sources.add("backup-cam-new")
    sources.add("backup-cam-taken")
    sources.add_filter("backup-cam-taken")
    obs_world.wait_for_device_by_name("backup-cam-taken", tmp_path / "device.json", lambda r: r["found"])

    # With no backup for the source, the first protocol is picked, not the
    # heading over it
    result = add_device(obs_world, tmp_path / "add.json", "backup-cam-new")
    # A source that already has a device isn't offered
    assert "backup-cam-new" in result["sources"]
    assert "backup-cam-taken" not in result["sources"]
    # Each protocol is offered, but not the backup of a device that exists
    assert "VISCA PTZ Control" in result["choices"]
    assert result["chosen"] == "VISCA PTZ Control"
    assert result["headings"][0] == "New device"
    assert not any("backup-cam-taken" in s for s in result["choices"])

    filters = sources.filters("backup-cam-new")
    assert [f["filterKind"] for f in filters] == [FILTER_KIND]
    device = obs_world.wait_for_device_by_name("backup-cam-new", tmp_path / "device.json",
                                               lambda r: r["found"] and r["bound"])
    assert preset_names(obs_world, device["uuid"], tmp_path / "settings.json") == []


def test_remove_removes_a_filter_device_and_backs_it_up(obs_world, sources, tmp_path):
    sources.add("backup-cam-remove")
    sources.add_filter("backup-cam-remove")
    obs_world.wait_for_device_by_name("backup-cam-remove", tmp_path / "device.json", lambda r: r["found"])

    obs_world.run_ui_test("remove_device", name="backup-cam-remove")

    obs_world.wait_for_device_by_name("backup-cam-remove", tmp_path / "device.json", lambda r: not r["found"])
    assert sources.filters("backup-cam-remove") == []
    backup = wait_for_backup(obs_world, tmp_path / "backups.json", "backup-cam-remove")
    assert [p["name"] for p in backup["presets"]] == ["Wide", "Close"]

    # The source is still there, and Add now offers its backup
    result = add_device(obs_world, tmp_path / "add.json", "backup-cam-remove")
    assert "backup-cam-remove" in result["chosen"]
    device = obs_world.wait_for_device_by_name("backup-cam-remove", tmp_path / "device.json",
                                               lambda r: r["found"] and r["bound"])
    assert preset_names(obs_world, device["uuid"], tmp_path / "settings.json") == ["Wide", "Close"]


def test_restoring_a_backup_brings_its_protocol(obs_world, sources, tmp_path):
    """A backup is a choice in the same list as the protocols, whatever its
    type, and can be put on a source of another name: its settings, type
    included, are what the new device gets."""
    sources.add("backup-cam-old")
    sources.add_filter("backup-cam-old")
    obs_world.wait_for_device_by_name("backup-cam-old", tmp_path / "device.json", lambda r: r["found"])
    obs_world.run_ui_test("remove_device", name="backup-cam-old")
    wait_for_backup(obs_world, tmp_path / "backups.json", "backup-cam-old")

    sources.add("backup-cam-other")
    result = add_device(obs_world, tmp_path / "add.json", "backup-cam-other", restore="backup-cam-old")
    assert "backup-cam-old" in result["chosen"]
    assert "VISCA" in result["chosen"]
    # (with the notes on detected devices in between)
    assert [h for h in result["headings"] if h in ("New device", "Restore a removed device")] == [
        "New device", "Restore a removed device"]

    filters = sources.filters("backup-cam-other")
    assert [f["filterKind"] for f in filters] == [FILTER_KIND]
    assert filters[0]["filterSettings"]["type"] == "visca-over-tcp"
    device = obs_world.wait_for_device_by_name("backup-cam-other", tmp_path / "device.json",
                                               lambda r: r["found"] and r["bound"])
    assert preset_names(obs_world, device["uuid"], tmp_path / "settings.json") == ["Wide", "Close"]


def backups_named(world, out, prefix):
    return [b for b in backups(world, out) if b["name"].startswith(prefix)]


def test_backup_records_kind(obs_world, sources, tmp_path):
    sources.add("backup-cam-kind")
    sources.add_filter("backup-cam-kind")
    obs_world.wait_for_device_by_name("backup-cam-kind", tmp_path / "device.json", lambda r: r["found"])

    sources.remove("backup-cam-kind")

    backup = wait_for_backup(obs_world, tmp_path / "backups.json", "backup-cam-kind")
    assert backup["kind"] == FILTER_KIND
    # The kind's own marker is a default, not something that was saved
    assert not any(key.startswith("ptz_api_version") for key in backup)


def test_parent_with_two_filters_backs_up_both(obs_world, sources, tmp_path):
    """Deleting a source takes every PTZ filter on it: one backup each (the
    two differ in type, so neither replaces the other)"""
    sources.add("backup-cam-two")
    sources.add_filter("backup-cam-two")
    sources.add_soft_filter("backup-cam-two")

    sources.remove("backup-cam-two")

    out = tmp_path / "backups.json"

    def both():
        return {b["kind"] for b in backups_named(obs_world, out, "backup-cam-two")} == {FILTER_KIND, SOFT_KIND}

    obs_world.wait_for(both)
    entries = backups_named(obs_world, out, "backup-cam-two")
    assert len(entries) == 2
    assert {b["type"] for b in entries} == {"visca-over-tcp", "soft-ptz"}


def test_filter_removed_by_hand_is_backed_up(obs_world, sources, tmp_path):
    sources.add("backup-cam-hand")
    sources.add_filter("backup-cam-hand")
    obs_world.wait_for_device_by_name("backup-cam-hand", tmp_path / "device.json", lambda r: r["found"])

    obs_world.ws.call("RemoveSourceFilter", {"sourceName": "backup-cam-hand", "filterName": "PTZ"})

    backup = wait_for_backup(obs_world, tmp_path / "backups.json", "backup-cam-hand")
    assert backup["kind"] == FILTER_KIND
    assert backup["type"] == "visca-over-tcp"
    assert [p["name"] for p in backup["presets"]] == ["Wide", "Close"]
    assert sources.filters("backup-cam-hand") == []


def test_entry_without_kind_still_restores(obs_world, sources, tmp_path):
    """A backup made before entries carried a kind is offered, and restored
    to the kind its type belongs to"""
    old = {"devices": [{
        "name": "backup-cam-legacy",
        "type": "visca-over-tcp",
        "host": "127.0.0.1",
        "tcp_port": sources.port,
        "presets": PRESETS,
        "backup_time": 1700000000,
    }]}
    (obs_world.config_dir / "device-backups.json").write_text(json.dumps(old))
    obs_world.run_ui_test("reload_device_backups")
    assert [b["name"] for b in backups(obs_world, tmp_path / "backups.json")] == ["backup-cam-legacy"]

    sources.add("backup-cam-legacy")
    result = add_device(obs_world, tmp_path / "add.json", "backup-cam-legacy")
    assert "backup-cam-legacy" in result["chosen"]

    filters = sources.filters("backup-cam-legacy")
    assert [f["filterKind"] for f in filters] == [FILTER_KIND]
    device = obs_world.wait_for_device_by_name("backup-cam-legacy", tmp_path / "device.json",
                                               lambda r: r["found"] and r["bound"])
    assert preset_names(obs_world, device["uuid"], tmp_path / "settings.json") == ["Wide", "Close"]


# Backups made by switching scene collection with four devices, three on a
# source each and one source with two (visca and soft): what the
# implementation that backed up from inside the device made. See the commit
# that introduced src/controller/ptz-backup-store.cpp.
COLLECTION_SWITCH_BACKUPS = 5


def test_collection_switch_backs_up_no_more_than_before(obs_world, sources, tmp_path):
    ws = obs_world.ws
    original = ws.call("GetSceneCollectionList")["currentSceneCollectionName"]
    names = [f"switch-cam-{i}" for i in range(3)]
    for name in names:
        sources.add(name)
        sources.add_filter(name)
    sources.add("switch-cam-pair")
    sources.add_filter("switch-cam-pair")
    sources.add_soft_filter("switch-cam-pair")
    for name in names + ["switch-cam-pair"]:
        obs_world.wait_for_device_by_name(name, tmp_path / "device.json", lambda r: r["found"])
    out = tmp_path / "backups.json"
    assert backups_named(obs_world, out, "switch-cam-") == []

    ws.call("CreateSceneCollection", {"sceneCollectionName": "backup-switch-target"})
    try:
        obs_world.wait_for(lambda: ws.call("GetSceneCollectionList")["currentSceneCollectionName"] ==
                           "backup-switch-target", timeout=20)
        # Settle: whatever the switch backs up, it has by now
        time.sleep(2)
        count = len(backups_named(obs_world, out, "switch-cam-"))
    finally:
        ws.call("SetCurrentSceneCollection", {"sceneCollectionName": original})
        obs_world.wait_for(lambda: ws.call("GetSceneCollectionList")["currentSceneCollectionName"] == original,
                           timeout=20)
    assert count == COLLECTION_SWITCH_BACKUPS
