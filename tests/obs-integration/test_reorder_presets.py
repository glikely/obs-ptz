"""Covers reordering the presets in the PTZ Controls dock.

The presets are moved two ways, both through tests/ui-harness/reorder-
presets-test.cpp's "reorder_presets" test: with the model's moveRow(), which is
what the move up and down buttons do, and by doing to the list view what a drop
does (CircularListView::dropCurrentRowAt()), which is what dragging a preset
does once the mouse is let go. A drag itself can't be made without a real
pointer, so that the list lets a preset be dragged is checked instead.
"""

import pytest

NAMES = ["Kitchen", "Garden", "Hall", "Porch"]


@pytest.fixture
def presets(obs_world, tmp_path):
    """A camera with four local presets, which is selected in the dock, and
    the name to select it with. They are deleted at the end, as the camera is
    shared between all the tests"""
    device = obs_world.device_names["visca-udp"]
    # Other tests leave presets on the shared camera, and the moves count from the top
    left = obs_world.call_proc(device, "ptz_preset_get_list", returns="data")["return"]["presets"]
    for preset_id in list(left):
        obs_world.call_proc(device, "ptz_preset_delete", {"id": preset_id})
    made = []
    for name in NAMES:
        result = obs_world.call_proc(device, "ptz_preset_create", {"name": name, "store": "local"}, returns="string")
        assert result["return"], f"no preset made for {name}"
        made.append(result["return"])
    out = tmp_path / "order.json"
    obs_world.wait_for(lambda: obs_world.reorder_presets(out, select=device)["presets"] == NAMES)
    yield device, out
    for preset_id in made:
        obs_world.call_proc(device, "ptz_preset_delete", {"id": preset_id})


def test_a_preset_moves_down(obs_world, presets):
    device, out = presets
    result = obs_world.reorder_presets(out, select=device, **{"from": 0, "to": 3})

    assert result["moved"] is True
    assert result["presets"] == ["Garden", "Hall", "Kitchen", "Porch"]


def test_a_preset_moves_up(obs_world, presets):
    device, out = presets
    result = obs_world.reorder_presets(out, select=device, **{"from": 3, "to": 1})

    assert result["moved"] is True
    assert result["presets"] == ["Kitchen", "Porch", "Garden", "Hall"]


def test_a_preset_moved_onto_its_own_place_is_not_moved(obs_world, presets):
    device, out = presets
    for to in (2, 3):
        result = obs_world.reorder_presets(out, select=device, **{"from": 2, "to": to})
        assert result["moved"] is False
        assert result["presets"] == NAMES


def test_a_preset_cannot_be_moved_off_the_ends(obs_world, presets):
    device, out = presets
    for src, dst in ((4, 0), (-1, 0), (0, 5), (0, -1)):
        result = obs_world.reorder_presets(out, select=device, **{"from": src, "to": dst})
        assert result["moved"] is False
        assert result["presets"] == NAMES


def test_dropping_a_preset_on_another_puts_it_after(obs_world, presets):
    device, out = presets
    result = obs_world.reorder_presets(out, select=device, via="drop", **{"from": 0, "to": 2})

    assert result["moved"] is True
    assert result["presets"] == ["Garden", "Hall", "Kitchen", "Porch"]
    assert result["selected_row"] == 2


def test_dropping_a_preset_past_the_last_puts_it_last(obs_world, presets):
    device, out = presets
    result = obs_world.reorder_presets(out, select=device, via="drop", **{"from": 0, "to": 4})

    assert result["presets"] == ["Garden", "Hall", "Porch", "Kitchen"]


def test_dropping_a_preset_where_it_is_does_nothing(obs_world, presets):
    device, out = presets
    result = obs_world.reorder_presets(out, select=device, via="drop", **{"from": 2, "to": 2})

    assert result["moved"] is False
    assert result["presets"] == NAMES


def test_the_new_order_is_the_devices(obs_world, presets):
    """The device keeps the order, which the API reports as it is shown"""
    device, out = presets
    obs_world.reorder_presets(out, select=device, **{"from": 0, "to": 4})

    listed = obs_world.call_proc(device, "ptz_preset_get_list", returns="data")["return"]
    names = [listed["presets"][entry["id"]]["name"] for entry in listed["order"]]
    assert names[-4:] == ["Garden", "Hall", "Porch", "Kitchen"]


def test_the_presets_can_be_dragged(obs_world, presets):
    device, out = presets
    result = obs_world.reorder_presets(out, select=device)

    assert result["presets_drag"] is True
    assert result["preset_flags_drag"] is True
