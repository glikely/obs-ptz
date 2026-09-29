"""Covers the PTZ settings dialog showing a device's settings and its state
apart, stacked one above the other on a single scrolling page: a properties
view over what the device saves, and a PTZStateView over what the camera
reports, which is updated in place rather than redrawn.

Driven through tests/ui-harness/settings-dialog-test.cpp, which opens the real
dialog and reads what its two views hold (see World.settings_dialog() in
conftest.py).
"""

import time

# The filter fixtures live in test_filter_devices.py
from test_filter_devices import camera_sim, cameras  # noqa: F401


def open_dialog(obs_world, device_id):
    obs_world.run_ui_test("open_settings_dialog", device_id=device_id)


def test_status_view_shows_the_cameras_real_state(obs_world, tmp_path):
    device_id = obs_world.device_ids["visca-tcp"]
    out = tmp_path / "dialog.json"
    # Two different modes, so both are commands. An idle device that other
    # tests left marked disconnected only finds out it isn't by sending one.
    for mode in (1, 2):
        obs_world.run_ui_test("set_device_state", device_id=device_id, wb_mode=mode)
        obs_world.wait_for_device_state(device_id, tmp_path / "state.json", lambda r: r["state"].get("wb_mode") == mode)

    open_dialog(obs_world, device_id)

    # the white balance the camera reports, not whatever its list started on
    dialog = obs_world.wait_for_settings_dialog(out, lambda r: r["connected"] and r["wb_mode"] == 2)
    assert "wb_mode" in dialog["state_keys"]
    # both views are showing at once, not one at a time behind a tab
    assert dialog["apply_visible"] is True


def test_settings_view_holds_only_settings(obs_world, tmp_path):
    device_id = obs_world.device_ids["visca-tcp"]
    open_dialog(obs_world, device_id)

    dialog = obs_world.settings_dialog(tmp_path / "dialog.json")
    assert {"type", "preset_max"} <= dialog["settings_keys"]
    assert not {"wb_mode", "connected", "live"} & dialog["settings_keys"]
    assert not {"type", "preset_max"} & dialog["state_keys"]


def test_picking_a_white_balance_in_the_status_view_changes_the_camera(obs_world, tmp_path):
    device_id = obs_world.device_ids["visca-tcp"]
    open_dialog(obs_world, device_id)

    obs_world.run_ui_test("edit_dialog_state", wb_mode=1)
    obs_world.wait_for_device_state(device_id, tmp_path / "state.json", lambda r: r["state"]["wb_mode"] == 1)
    obs_world.run_ui_test("edit_dialog_state", wb_mode=2)
    obs_world.wait_for_device_state(device_id, tmp_path / "state.json", lambda r: r["state"]["wb_mode"] == 2)

    # and the dialog follows the camera to what it reports
    obs_world.wait_for_settings_dialog(tmp_path / "dialog.json", lambda r: r["wb_mode"] == 2)


def test_state_changes_update_only_the_status_view(obs_world, tmp_path):
    device_id = obs_world.device_ids["visca-tcp"]
    out = tmp_path / "dialog.json"
    open_dialog(obs_world, device_id)

    obs_world.run_ui_test("set_device_state", device_id=device_id, wb_mode=1)
    obs_world.wait_for_settings_dialog(out, lambda r: r["wb_mode"] == 1)
    obs_world.run_ui_test("set_device_state", device_id=device_id, wb_mode=2)
    dialog = obs_world.wait_for_settings_dialog(out, lambda r: r["wb_mode"] == 2)

    assert dialog["state_updates"] > 0
    assert dialog["settings_refreshes"] == 0


def test_settings_changes_redraw_only_the_settings_view(obs_world, tmp_path):
    device_id = obs_world.device_ids["visca-tcp"]
    out = tmp_path / "dialog.json"
    open_dialog(obs_world, device_id)
    time.sleep(0.5)
    open_dialog(obs_world, device_id)  # settle, and count from zero

    obs_world.run_ui_test("update_device", device_id=device_id)
    dialog = obs_world.wait_for_settings_dialog(out, lambda r: r["settings_refreshes"] > 0)

    assert dialog["state_updates"] == 0


def test_status_view_updates_in_place(obs_world, tmp_path):
    device_id = obs_world.device_ids["visca-tcp"]
    out = tmp_path / "dialog.json"
    open_dialog(obs_world, device_id)
    before = obs_world.wait_for_settings_dialog(out, lambda r: r["connected"] and "wb_mode" in r["state_keys"])
    assert before["wb_widget"], "the status view has no white balance list"

    # What changes as a camera works: what it reports of itself
    for mode in (1, 2, 1):
        obs_world.run_ui_test("set_device_state", device_id=device_id, wb_mode=mode)
        obs_world.wait_for_settings_dialog(out, lambda r, m=mode: r["wb_mode"] == m)

    # It changed, and every widget is the one that was there, none replaced
    after = obs_world.wait_for_settings_dialog(out, lambda r: r["state_updates"] >= 3)
    assert after["wb_widget"] == before["wb_widget"]
    assert after["state_widgets"] == before["state_widgets"]


def test_status_view_has_diagnostics_only_for_a_camera_with_them(obs_world, tmp_path):
    out = tmp_path / "dialog.json"
    open_dialog(obs_world, obs_world.device_ids["visca-tcp"])
    obs_world.wait_for_settings_dialog(out, lambda r: r["diagnostics_visible"])
    open_dialog(obs_world, obs_world.device_ids["pelco-d"])
    obs_world.wait_for_settings_dialog(out, lambda r: not r["diagnostics_visible"])


def test_scanning_inquiries_from_the_status_view(obs_world, cameras, tmp_path):  # noqa: F811
    """On a camera of its own: the scan keeps the device busy for a while"""
    cameras.add_source(obs_world.create_scene(), "dialog-scan-cam")
    cameras.add_filter("dialog-scan-cam")
    out = tmp_path / "device.json"
    device_id = obs_world.wait_for_device_by_name(
        "dialog-scan-cam", out, lambda r: r["found"] and r["bound"])["device_id"]
    state_out = tmp_path / "state.json"

    def sent():
        return obs_world.device_state(device_id, state_out)["state"]["statistics"].get("visca_sent_count", 0)

    obs_world.wait_for_device_state(device_id, state_out, lambda r: r["state"].get("connected") is True, timeout=10)
    open_dialog(obs_world, device_id)
    obs_world.wait_for_settings_dialog(tmp_path / "dialog.json", lambda r: r["diagnostics_visible"])
    before = sent()
    obs_world.run_ui_test("press_dialog_button", button="scanInquiries")
    obs_world.wait_for(lambda: sent() >= before + 0x7e, timeout=20)
