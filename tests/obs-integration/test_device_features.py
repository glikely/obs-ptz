"""What a device can do: the "features" in its state (PTZDevice::features()),
and the PTZ Controls dock enabling only the controls for them, through
tests/ui-harness/dock-controls-test.cpp's "get_dock_controls" test.
"""

import pytest

# What each device's camera can do, as ptzsim imitates it
FEATURES = {
    "visca-tcp": {"pantilt", "pantilt_abs", "pantilt_rel", "home", "zoom", "zoom_abs", "focus", "focus_onetouch",
                  "autofocus", "presets", "power", "wb_onepush", "diagnostics"},
    "pelco-d": {"pantilt", "zoom", "focus", "home", "presets"},
}
# ...and at least this, for ONVIF, which has focus too if ptzsim has an
# imaging service
ONVIF_FEATURES = {"pantilt", "pantilt_abs", "pantilt_rel", "zoom", "zoom_abs", "home", "home_set", "presets"}


def features(state):
    return {name for name, on in state.get("features", {}).items() if on}


@pytest.mark.parametrize("backend", FEATURES)
def test_a_device_says_what_it_can_do(obs_world, backend, tmp_path):
    obs_world.wait_for_device_state(
        obs_world.device_ids[backend], tmp_path / "state.json",
        lambda r: features(r["state"]) == FEATURES[backend], timeout=10)


def test_an_onvif_device_says_what_it_can_do(obs_world, tmp_path):
    obs_world.wait_for_device_state(
        obs_world.device_ids["onvif"], tmp_path / "state.json",
        lambda r: ONVIF_FEATURES <= features(r["state"]), timeout=10)


def test_the_dock_offers_only_what_pelco_can_do(obs_world, tmp_path):
    enabled = obs_world.wait_for_dock_controls(
        obs_world.device_ids["pelco-d"], tmp_path / "dock.json",
        lambda e: e["panTiltButton_up"] and e["panTiltTouch"] and e["panTiltButton_home"]
        and e["zoomButton_tele"] and e["presetListView"])
    # no autofocus, so focusing is always by hand, but no one-push focus
    assert enabled["focusButton_auto"] is False
    assert enabled["focusButton_near"] is True
    assert enabled["focusButton_onetouch"] is False


def test_the_dock_offers_autofocus_for_visca(obs_world, tmp_path):
    obs_world.wait_for_dock_controls(
        obs_world.device_ids["visca-tcp"], tmp_path / "dock.json",
        lambda e: e["focusButton_auto"] and e["panTiltButton_up"] and e["zoomButton_tele"])
