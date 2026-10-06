"""Covers the placeholder of a camera's blank Host: the host its source says it
receives from, which the device says to a controller as the "host:placeholder"
default in its source's settings, for the settings dialog to show in the empty
field, and changes when the source's URL does.

tests/ui-harness/third-party-device-test.cpp registers a stand-in for a
DistroAV NDI source, the id "ndi_source" with a "web_control_url" setting.
Defaults are not saved, so this must not be in the filter's own settings.
"""

import pytest
from obsws import ObsWebSocketError

from conftest import free_port

SOURCE = "placeholder-cam"


@pytest.fixture
def host_source(obs_world):
    scene = obs_world.create_scene()
    item = obs_world.ws.call("CreateInput", {
        "sceneName": scene,
        "inputName": SOURCE,
        "inputKind": "ndi_source",
        "inputSettings": {"web_control_url": "http://10.9.8.7/"},
    })["sceneItemId"]
    yield SOURCE
    obs_world.ws.call("RemoveSceneItem", {"sceneName": scene, "sceneItemId": item})
    try:
        obs_world.ws.call("RemoveInput", {"inputName": SOURCE})
    except ObsWebSocketError:
        pass


def placeholder(world, uuid, out):
    return world.device_settings(uuid, out)["filter_defaults"].get("host:placeholder")


def test_a_blank_host_has_the_sources_host_as_its_placeholder(obs_world, host_source, tmp_path):
    obs_world.ws.call("CreateSourceFilter", {
        "sourceName": host_source,
        "filterName": "PTZ",
        "filterKind": "ca.secretlab.obs-ptz.visca",
        "filterSettings": {"type": "visca-over-tcp", "host": "", "tcp_port": free_port()},
    })
    out = tmp_path / "device.json"
    uuid = obs_world.wait_for_device_by_name(host_source, out, lambda r: r["found"] and r["bound"])["uuid"]

    obs_world.wait_for(lambda: placeholder(obs_world, uuid, out) == "10.9.8.7")
    # A default is not saved
    assert "host:placeholder" not in obs_world.device_settings(uuid, out)["filter_keys"]

    obs_world.ws.call("SetInputSettings", {"inputName": host_source,
                                           "inputSettings": {"web_control_url": "http://10.1.1.1/"}})

    obs_world.wait_for(lambda: placeholder(obs_world, uuid, out) == "10.1.1.1")
