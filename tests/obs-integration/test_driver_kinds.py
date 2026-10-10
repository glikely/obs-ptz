"""Covers how the controller finds the PTZ drivers: by walking OBS's filter
kinds for the ones whose defaults carry the PTZ API version marker (see
"Driver kinds" in docs/ptz-device-api.md), not from a table of its own, and
what a Pelco filter that doesn't say speaks.

Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
SPDX-License-Identifier: GPL-2.0-or-later

Driven through tests/ui-harness/driver-kinds-test.cpp's "driver_kinds" test,
which reports what ptz_driver_kinds() (src/ptz-driver-kinds.cpp) returns.
Expects the kinds of a build with every driver enabled, as
scripts/macos-dev.sh makes one.
"""

import json

import pytest
from obsws import ObsWebSocketError

PREFIX = "ca.secretlab.obs-ptz."
KINDS = {PREFIX + name for name in ("visca", "onvif", "pelco", "usb-cam", "soft-ptz")}
PELCO_KIND = PREFIX + "pelco"
# Defaults of a kind, never settings of a filter
NOT_SETTINGS = {"ptz_api_version_major", "ptz_api_version_minor"}


@pytest.fixture
def sources(obs_world):
    """Empty media sources standing in for cameras, removed at the end"""
    scene = obs_world.create_scene()
    names = []

    def add(name):
        obs_world.ws.call("CreateInput", {
            "sceneName": scene,
            "inputName": name,
            "inputKind": "ffmpeg_source",
            "inputSettings": {"is_local_file": True, "local_file": ""},
        })
        names.append(name)

    yield add
    for name in names:
        try:
            obs_world.ws.call("RemoveInput", {"inputName": name})
        except ObsWebSocketError:
            pass


def test_driver_kinds_listed(obs_world, tmp_path):
    out = tmp_path / "kinds.json"
    obs_world.run_ui_test("driver_kinds", filename=str(out))
    obs_world.wait_for(out.exists)

    kinds = {k["kind"]: k for k in json.loads(out.read_text(encoding="utf-8"))["kinds"]}
    assert set(kinds) == KINDS
    for kind, entry in kinds.items():
        assert entry["name"], kind
    # A Pelco filter whose settings don't say speaks Pelco-P, as it always has
    assert kinds[PELCO_KIND]["defaults"]["use_pelco_d"] is False


def saved_settings(obs_world, source, tmp_path):
    device = obs_world.wait_for_device_by_name(source, tmp_path / "device.json", lambda r: r["found"])
    return obs_world.device_settings(device["uuid"], tmp_path / "settings.json")


def test_pelco_filter_that_does_not_say_speaks_pelco_p(obs_world, sources, tmp_path):
    """As one made from OBS's Filters menu, over obs-websocket or migrated
    from a legacy entry without the key is"""
    sources("kinds-pelco-filter")
    obs_world.ws.call("CreateSourceFilter", {
        "sourceName": "kinds-pelco-filter",
        "filterName": "PTZ",
        "filterKind": PELCO_KIND,
        "filterSettings": {},
    })
    settings = saved_settings(obs_world, "kinds-pelco-filter", tmp_path)
    assert settings["saved"]["use_pelco_d"] is False
    assert not NOT_SETTINGS & set(settings["saved"])
    assert not NOT_SETTINGS & settings["filter_keys"]

