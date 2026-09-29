"""VISCA state from a camera that only has the single-value inquiries.

VISCA has "7e 7e xx" block inquiries that return a camera's lens and camera
control settings in one reply. A Sony has them; a BirdDog answers every one
with a syntax error and has only the single-value inquiries (zoom position,
focus position, focus mode, white balance mode, ...). PTZVisca asks for
zoom, focus, autofocus and white balance through the block inquiries, so it
used to show only pan, tilt and power for such a camera, and the settings
dialog's state view had nothing for the rest.

ptzsim's --visca-no-block-inquiries imitates it (birddog_ptzsim, wired to
DEVICE_IDS["visca-tcp-birddog"]). The first test looks at what is read at
startup, so it has to run before the others.
"""

import pytest

ZOOM = 0.5
WB_MODE = 2


@pytest.fixture
def birddog(obs_world, tmp_path):
    device_id = obs_world.device_ids["visca-tcp-birddog"]
    obs_world.wait_for_device_status(device_id, tmp_path / "status.json", lambda s: s["connected"] is True, timeout=10)
    return device_id


def test_reports_the_state_it_can_read_at_startup(obs_world, birddog, tmp_path):
    state = obs_world.wait_for_device_state(
        birddog, tmp_path / "state.json",
        lambda r: {"pan", "tilt", "zoom", "focus", "power_on", "wb_mode", "focus_af_enabled"} <= set(r["state"]),
        timeout=15)["state"]

    # ptzsim's camera starts with autofocus on, and reads it back as such
    assert state["focus_af_enabled"] is True


def test_zoom_follows_a_move(obs_world, birddog, tmp_path):
    obs_world.run_ui_test("move_device", device_id=birddog, mode="abs", zoom=ZOOM)
    obs_world.wait_for_device_state(
        birddog, tmp_path / "state.json", lambda r: abs(r["state"].get("zoom", -1) - ZOOM) < 0.01, timeout=10)


def test_white_balance_mode_is_read_back(obs_world, birddog, tmp_path):
    obs_world.run_ui_test("set_device_state", device_id=birddog, wb_mode=WB_MODE)
    obs_world.wait_for_device_status(birddog, tmp_path / "status.json", lambda s: s["wb_mode"] == WB_MODE, timeout=10)
