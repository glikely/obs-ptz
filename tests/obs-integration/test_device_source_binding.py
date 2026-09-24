"""Covers how a PTZDevice binds to the OBS source it controls: that it
finds a source that didn't exist when the device was loaded, follows the
source being renamed, and copes with the source being removed and a new
one created in its place.

The device config is loaded from obs_module_load(), before OBS has loaded
its scene collection, so a device can never bind to its source at load
time. It keeps the configured source name and looks the source up by it
when it's next needed (PTZDevice::source(), src/ptz-device.cpp). These
tests are what would notice that regressing: every device here is
configured with the name of a source that doesn't exist yet (see
DEVICE_IDS in conftest.py), and the tests create the source afterward.

Driven through tests/ui-harness/device-source-test.cpp's
"get_device_source" test, see World.device_source() in conftest.py. Whether
a device is really bound to *the* source, rather than just carrying its
name around, is checked two ways: the device must become "live"
(PTZListModel::IsLiveRole) once its source is in the program scene, which
only happens if it holds the very source object that's in the scene; and
the UUID of the source it resolves to must stay the same across a rename
and change when the source is replaced by a new one of the same name.
(obs-websocket before 5.4 doesn't report an input's UUID, so it's the
device's own report of it that's compared.)
"""

import pytest
from obsws import ObsWebSocketError

# obs-websocket RequestStatus::ResourceNotFound
RESOURCE_NOT_FOUND = 600


class Sources:
    """Creates inputs in the world's OBS and removes them again, since
    removing a scene doesn't remove the inputs in it.

    Removing an input takes its scene item out of its scene first. Removing
    the input alone leaves the scene item holding a reference to it, and so
    the input alive, and its name taken, until the scene next gets drawn --
    which for a scene that isn't the program scene is never, and for the
    program scene on macOS is sometimes not for a good while. The tests that
    create a source under the name of one they just removed can't wait for
    that. See test_obs_removed_source.py."""

    def __init__(self, world):
        self.world = world
        self.names = set()
        self.items = {}

    def create(self, scene, name):
        """Adds a colour source called `name` to `scene`."""
        response = self.world.ws.call("CreateInput", {
            "sceneName": scene,
            "inputName": name,
            "inputKind": "color_source_v3",
            "inputSettings": {},
        })
        self.names.add(name)
        self.items[name] = (scene, response["sceneItemId"])

    def rename(self, old, new):
        self.world.ws.call("SetInputName", {"inputName": old, "newInputName": new})
        self.names.discard(old)
        self.names.add(new)
        self.items[new] = self.items.pop(old)

    def remove(self, name):
        scene, item = self.items.pop(name, (None, None))
        if scene is not None:
            try:
                self.world.ws.call("RemoveSceneItem", {"sceneName": scene, "sceneItemId": item})
            except ObsWebSocketError:
                pass  # its scene is gone, and the item with it
        try:
            self.world.ws.call("RemoveInput", {"inputName": name})
        except ObsWebSocketError as e:
            # Nothing else referenced the input, so it went with its scene
            # item. If something did (see hold_source), this is what marks
            # it as removed.
            if e.status.get("code") != RESOURCE_NOT_FOUND:
                raise
        self.names.discard(name)

    def cleanup(self):
        for name in list(self.names):
            try:
                self.remove(name)
            except ObsWebSocketError:
                pass


@pytest.fixture
def sources(obs_world):
    s = Sources(obs_world)
    yield s
    s.cleanup()


def make_program(world, scene):
    world.ws.call("SetCurrentProgramScene", {"sceneName": scene})


def test_unbound_device_keeps_its_configured_name(obs_world, tmp_path):
    """No source of the configured name exists yet: the device must not
    fall back to the default name, or its config would be lost."""
    device_id = obs_world.device_ids["source-late"]
    result = obs_world.device_source(device_id, tmp_path / "source.json")

    assert result["name"] == "sim-source-late"
    assert result["config_name"] == "sim-source-late"
    assert result["bound"] is False
    assert result["live"] is False


def test_binds_to_a_source_created_after_loading(obs_world, sources, tmp_path):
    device_id = obs_world.device_ids["source-late"]
    out = tmp_path / "source.json"

    scene = obs_world.create_scene()
    sources.create(scene, "sim-source-late")
    make_program(obs_world, scene)

    result = obs_world.wait_for_device_source(device_id, out, lambda r: r["bound"] and r["live"])
    assert result["source"] == "sim-source-late"
    uuid = result["source_uuid"]
    assert uuid
    assert result["name"] == "sim-source-late"
    # A live camera is locked out of being moved by the controls
    assert result["locked"] is True

    # Still bound once its scene stops being the program scene
    make_program(obs_world, obs_world.create_scene())
    result = obs_world.wait_for_device_source(device_id, out, lambda r: not r["live"])
    assert result["bound"] is True
    assert result["source_uuid"] == uuid
    assert result["locked"] is False


def test_follows_source_renames(obs_world, sources, tmp_path):
    device_id = obs_world.device_ids["source-rename"]
    out = tmp_path / "source.json"

    scene = obs_world.create_scene()
    sources.create(scene, "sim-source-rename")
    make_program(obs_world, scene)
    uuid = obs_world.wait_for_device_source(device_id, out, lambda r: r["bound"] and r["live"])["source_uuid"]

    sources.rename("sim-source-rename", "renamed-camera")

    # The device's name, and the name it will save, both follow the source
    result = obs_world.wait_for_device_source(
        device_id, out, lambda r: r["name"] == "renamed-camera" and r["config_name"] == "renamed-camera")
    assert result["source"] == "renamed-camera"
    assert result["source_uuid"] == uuid
    assert result["bound"] is True

    # ...and it's still bound to the same source, which is still in the
    # program scene, rather than having merely been given the new name
    make_program(obs_world, obs_world.create_scene())
    obs_world.wait_for_device_source(device_id, out, lambda r: not r["live"])
    make_program(obs_world, scene)
    result = obs_world.wait_for_device_source(device_id, out, lambda r: r["live"])
    assert result["source_uuid"] == uuid

    # Renaming again keeps following it
    sources.rename("renamed-camera", "renamed-camera-again")
    obs_world.wait_for_device_source(device_id, out, lambda r: r["name"] == "renamed-camera-again")


def test_survives_source_removed_and_recreated(obs_world, sources, tmp_path):
    device_id = obs_world.device_ids["source-recreate"]
    out = tmp_path / "source.json"

    scene = obs_world.create_scene()
    sources.create(scene, "sim-source-recreate")
    make_program(obs_world, scene)
    first_uuid = obs_world.wait_for_device_source(device_id, out, lambda r: r["bound"] and r["live"])["source_uuid"]
    assert first_uuid

    sources.remove("sim-source-recreate")

    # The device lets go of the source but keeps its name, so that it can
    # find the source again and its config isn't lost
    result = obs_world.wait_for_device_source(device_id, out, lambda r: not r["bound"], timeout=10)
    assert result["name"] == "sim-source-recreate"
    assert result["config_name"] == "sim-source-recreate"

    # A new source of the same name is a different object, which the
    # device must pick up rather than hold on to the old one
    sources.create(scene, "sim-source-recreate")
    make_program(obs_world, obs_world.create_scene())
    obs_world.wait_for_device_source(device_id, out, lambda r: not r["live"])
    make_program(obs_world, scene)
    result = obs_world.wait_for_device_source(device_id, out, lambda r: r["bound"] and r["live"])
    assert result["source_uuid"] != first_uuid
    assert result["name"] == "sim-source-recreate"

    # ...and it still follows renames of the new source
    sources.rename("sim-source-recreate", "recreated-and-renamed")
    obs_world.wait_for_device_source(device_id, out, lambda r: r["name"] == "recreated-and-renamed")


def remove_held_source(obs_world, sources, tmp_path, device_id, name):
    """Binds `device_id` to a new source called `name`, holds a reference
    to it the way another plugin, dock or script might, and removes it.
    Returns the device's report on its (former) source, the scene the
    source was in, the source's UUID, and a function that drops the held
    reference. A removed source isn't destroyed until everything that
    references it lets go, and meanwhile it's still findable by name --
    which references outlive the removal, and for how long, differs
    between platforms and setups, so hold one ourselves rather than hope
    for it."""
    out = tmp_path / "source.json"
    hold = tmp_path / "hold.json"

    scene = obs_world.create_scene()
    sources.create(scene, name)
    make_program(obs_world, scene)
    uuid = obs_world.wait_for_device_source(device_id, out, lambda r: r["bound"] and r["live"])["source_uuid"]

    obs_world.hold_source(name, hold)
    sources.remove(name)
    result = obs_world.wait_for_device_source(device_id, out, lambda r: not r["bound"], timeout=10)
    return result, scene, uuid, lambda: obs_world.hold_source(name, hold, release=True)


def test_removed_source_that_is_still_referenced_is_let_go(obs_world, sources, tmp_path):
    """The device must treat a source as gone as soon as it is removed, not
    when it is finally destroyed."""
    device_id = obs_world.device_ids["source-held"]
    result, _, _, release = remove_held_source(obs_world, sources, tmp_path, device_id, "sim-source-held")
    try:
        assert result["bound"] is False
        assert result["name"] == "sim-source-held"
    finally:
        release()


def test_replaces_removed_source_once_it_is_released(obs_world, sources, tmp_path):
    """...and then binds to whatever takes the removed source's name."""
    device_id = obs_world.device_ids["source-held-replaced"]
    out = tmp_path / "source.json"
    _, scene, first_uuid, release = remove_held_source(
        obs_world, sources, tmp_path, device_id, "sim-source-held-replaced")
    release()

    sources.create(scene, "sim-source-held-replaced")
    make_program(obs_world, obs_world.create_scene())
    obs_world.wait_for_device_source(device_id, out, lambda r: not r["live"])
    make_program(obs_world, scene)
    result = obs_world.wait_for_device_source(device_id, out, lambda r: r["bound"] and r["live"])
    assert result["source_uuid"] != first_uuid


DEFAULT_NAME = "PTZ Device"


def test_device_with_no_source_has_a_default_name(obs_world, tmp_path):
    """A device that was never given a source has no name of its own to
    show, so everything that lists devices has to make one up."""
    result = obs_world.device_source(obs_world.device_ids["unnamed"], tmp_path / "source.json")

    assert result["bound"] is False
    assert result["config_name"] == ""
    assert result["name"].startswith(DEFAULT_NAME)


def test_action_source_lists_a_device_with_no_source_by_a_default_name(obs_world, sources):
    """The action source's list of cameras is built from the devices' saved
    configs, in which a device with no source has an empty name, and it
    must not show that as a blank entry."""
    obs_world.ws.call("CreateInput", {
        "sceneName": obs_world.create_scene(),
        "inputName": "device-list-action",
        "inputKind": "ptz_action_source",
        "inputSettings": {"trigger": 1},  # not on program, so nothing fires
    })
    sources.names.add("device-list-action")

    items = obs_world.ws.call("GetInputPropertiesListPropertyItems", {
        "inputName": "device-list-action",
        "propertyName": "device_id",
    })["propertyItems"]

    labels = {item["itemValue"]: item["itemName"] for item in items}
    assert obs_world.device_ids["unnamed"] in labels
    assert all(label.strip() for label in labels.values()), labels
    assert labels[obs_world.device_ids["unnamed"]].startswith(DEFAULT_NAME)
