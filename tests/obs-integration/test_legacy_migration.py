"""Covers the migration of the devices of the old self-managed backend.

This suite's own config (write_ptz_plugin_config() in conftest.py) is such a
config.json: a device each, with an id, bound to a source by name, none of
which exist when OBS starts. Once OBS has loaded its scene collection the
plugin makes each of them a "PTZ Control" filter (src/ptz-legacy-migration.cpp),
on a placeholder source here, as there are no sources of those names.

These tests look at what that left behind: the filters, that their devices
have the ids they had, the copy of config.json kept in case it went wrong, and
that config.json no longer has what was migrated.
"""

import json

import pytest

from conftest import DEVICE_IDS

FILTER_KINDS = {
    "visca": "ca.secretlab.obs-ptz.visca",
    "visca-over-tcp": "ca.secretlab.obs-ptz.visca",
    "visca-over-ip": "ca.secretlab.obs-ptz.visca",
    "pelco": "ca.secretlab.obs-ptz.pelco",
    "onvif": "ca.secretlab.obs-ptz.onvif",
}


def legacy_devices(world):
    backup = world.config_dir / "config.json.pre-filter-migration"
    assert backup.exists(), "config.json was not copied before being migrated"
    return json.loads(backup.read_text())["devices"]


def source_name(device):
    return device["name"] or f"PTZ Device {device['id']}"


def test_config_json_was_kept(obs_world):
    devices = legacy_devices(obs_world)
    assert {d["id"] for d in devices} == set(DEVICE_IDS.values())


def test_each_device_is_a_filter_on_its_source(obs_world):
    inputs = {i["inputName"] for i in obs_world.ws.call("GetInputList")["inputs"]}
    for device in legacy_devices(obs_world):
        name = source_name(device)
        assert name in inputs, f"no source for device {device['id']}"
        filters = obs_world.ws.call("GetSourceFilterList", {"sourceName": name})["filters"]
        kinds = {f["filterKind"] for f in filters}
        assert FILTER_KINDS[device["type"]] in kinds, f"{name} has {kinds}"


def test_devices_keep_their_ids(obs_world, tmp_path):
    out = tmp_path / "device.json"
    for device in legacy_devices(obs_world):
        found = obs_world.device_by_name(source_name(device), out)
        assert found["found"], f"no device for {source_name(device)}"
        assert found["device_id"] == device["id"]
        assert found["bound"]


def test_migrated_devices_are_gone_from_config_json(obs_world):
    config = json.loads((obs_world.config_dir / "config.json").read_text())
    assert not config.get("devices")
