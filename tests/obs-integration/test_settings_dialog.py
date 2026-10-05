"""Covers the PTZ settings dialog showing a device's settings and its state
apart, stacked one above the other on a single scrolling page: a properties
view over what the device saves, and a PTZStateView over what the camera
reports, which is updated in place rather than redrawn.

Driven through tests/ui-harness/settings-dialog-test.cpp, which opens the real
dialog and reads what its two views hold (see World.settings_dialog() in
conftest.py).
"""

import json
import time

ACTION_PAN_TILT = 3
ACTION_STOP = 4

# The filter fixtures live in test_filter_devices.py
from test_filter_devices import camera_sim, cameras  # noqa: F401


def open_dialog(obs_world, device_name):
    obs_world.run_ui_test("open_settings_dialog", device=device_name)


def test_status_view_shows_the_cameras_real_state(obs_world, tmp_path):
    device_name = obs_world.device_names["visca-tcp"]
    out = tmp_path / "dialog.json"
    # Two different modes, so both are commands. An idle device that other
    # tests left marked disconnected only finds out it isn't by sending one.
    for mode in (1, 2):
        obs_world.run_ui_test("set_device_state", device=device_name, wb_mode=mode)
        obs_world.wait_for_device_state(device_name, tmp_path / "state.json", lambda r: r["state"].get("wb_mode") == mode)

    open_dialog(obs_world, device_name)

    # the white balance the camera reports, not whatever its list started on
    dialog = obs_world.wait_for_settings_dialog(out, lambda r: r["connected"] and r["wb_mode"] == 2)
    assert "wb_mode" in dialog["state_keys"]
    # the state and the settings are tabs, state first, and nothing has been
    # edited, so there is nothing to apply
    assert [tab["name"] for tab in dialog["tabs"]][:2] == ["Status", "Settings"]
    assert dialog["apply_enabled"] is False


def test_status_view_shows_where_the_camera_is(obs_world, tmp_path):
    device_name = obs_world.device_names["visca-tcp"]
    out = tmp_path / "dialog.json"
    open_dialog(obs_world, device_name)
    obs_world.wait_for_settings_dialog(out, lambda r: {"pan", "tilt", "zoom", "focus"} <= r["state_keys"])

    # from the middle of its range, not wherever earlier tests left it: the
    # device reads where the camera really is, and a short move from far
    # outside the range the plugin shows would not show
    obs_world.run_ui_test("move_device", device=device_name, mode="abs", pan=0.0, tilt=0.0)
    before = obs_world.wait_for_settings_dialog(out, lambda r: abs(r["pan"]) < 0.01, timeout=10)["pan"]

    obs_world.trigger_action(device_name, ACTION_PAN_TILT, pan_speed=0.6, tilt_speed=0.0)
    time.sleep(0.5)
    obs_world.trigger_action(device_name, ACTION_STOP)

    # where the camera then reports itself to be
    obs_world.wait_for_settings_dialog(out, lambda r: r["pan"] != before, timeout=10)


def test_settings_view_holds_only_settings(obs_world, tmp_path):
    device_name = obs_world.device_names["visca-tcp"]
    open_dialog(obs_world, device_name)

    dialog = obs_world.settings_dialog(tmp_path / "dialog.json")
    assert {"type", "preset_max"} <= dialog["settings_keys"]
    assert not {"wb_mode", "connected", "live"} & dialog["settings_keys"]
    assert not {"type", "preset_max"} & dialog["state_keys"]


def test_settings_view_drops_the_last_devices_settings(obs_world, tmp_path):
    """Showing another device's settings starts from nothing: none of the
    last device's keys stay behind, even without their values"""
    open_dialog(obs_world, obs_world.device_names["onvif"])
    obs_world.wait_for_settings_dialog(tmp_path / "dialog.json", lambda d: "wb_mode" in d["settings_keys"])

    open_dialog(obs_world, obs_world.device_names["visca-tcp"])
    dialog = obs_world.wait_for_settings_dialog(tmp_path / "dialog.json",
                                                lambda d: "tcp_port" in d["settings_keys"])
    assert not {"wb_mode", "username", "password"} & dialog["settings_keys"]


def test_picking_a_white_balance_in_the_status_view_changes_the_camera(obs_world, tmp_path):
    device_name = obs_world.device_names["visca-tcp"]
    open_dialog(obs_world, device_name)

    obs_world.run_ui_test("edit_dialog_state", wb_mode=1)
    obs_world.wait_for_device_state(device_name, tmp_path / "state.json", lambda r: r["state"]["wb_mode"] == 1)
    obs_world.run_ui_test("edit_dialog_state", wb_mode=2)
    obs_world.wait_for_device_state(device_name, tmp_path / "state.json", lambda r: r["state"]["wb_mode"] == 2)

    # and the dialog follows the camera to what it reports
    obs_world.wait_for_settings_dialog(tmp_path / "dialog.json", lambda r: r["wb_mode"] == 2)


def test_state_changes_update_only_the_status_view(obs_world, tmp_path):
    device_name = obs_world.device_names["visca-tcp"]
    out = tmp_path / "dialog.json"
    open_dialog(obs_world, device_name)

    obs_world.run_ui_test("set_device_state", device=device_name, wb_mode=1)
    obs_world.wait_for_settings_dialog(out, lambda r: r["wb_mode"] == 1)
    obs_world.run_ui_test("set_device_state", device=device_name, wb_mode=2)
    dialog = obs_world.wait_for_settings_dialog(out, lambda r: r["wb_mode"] == 2)

    assert dialog["state_updates"] > 0
    assert dialog["settings_refreshes"] == 0


def test_settings_changes_redraw_only_the_settings_view(obs_world, tmp_path):
    device_name = obs_world.device_names["visca-tcp"]
    out = tmp_path / "dialog.json"
    open_dialog(obs_world, device_name)
    time.sleep(0.5)
    open_dialog(obs_world, device_name)  # settle, and count from zero

    obs_world.run_ui_test("update_device", device=device_name)
    dialog = obs_world.wait_for_settings_dialog(out, lambda r: r["settings_refreshes"] > 0)

    assert dialog["state_updates"] == 0


def test_status_view_updates_in_place(obs_world, tmp_path):
    device_name = obs_world.device_names["visca-tcp"]
    out = tmp_path / "dialog.json"
    open_dialog(obs_world, device_name)
    before = obs_world.wait_for_settings_dialog(out, lambda r: r["connected"] and "wb_mode" in r["state_keys"])
    assert before["wb_widget"], "the status view has no white balance list"

    # What changes as a camera works: what it reports of itself, and where it is
    for mode in (1, 2, 1):
        obs_world.run_ui_test("set_device_state", device=device_name, wb_mode=mode)
        obs_world.wait_for_settings_dialog(out, lambda r, m=mode: r["wb_mode"] == m)
    obs_world.trigger_action(device_name, ACTION_PAN_TILT, pan_speed=0.6, tilt_speed=0.0)
    time.sleep(0.5)
    obs_world.trigger_action(device_name, ACTION_STOP)

    # It changed, and every widget is the one that was there, none replaced
    after = obs_world.wait_for_settings_dialog(out, lambda r: r["state_updates"] >= 3)
    assert after["wb_widget"] == before["wb_widget"]
    assert after["state_widgets"] == before["state_widgets"]


def test_status_view_has_diagnostics_only_for_a_camera_with_them(obs_world, tmp_path):
    out = tmp_path / "dialog.json"
    open_dialog(obs_world, obs_world.device_names["visca-tcp"])
    obs_world.wait_for_settings_dialog(out, lambda r: r["diagnostics_visible"])
    open_dialog(obs_world, obs_world.device_names["pelco-d"])
    obs_world.wait_for_settings_dialog(out, lambda r: not r["diagnostics_visible"])


def test_status_view_shows_how_fast_the_camera_answers_polls(obs_world, tmp_path):
    out = tmp_path / "dialog.json"
    open_dialog(obs_world, obs_world.device_names["visca-tcp"])
    shown = obs_world.wait_for_settings_dialog(
        out, lambda r: r["shown"].get("polls_total", "-") not in ("-", "0"), timeout=15)["shown"]
    assert shown["poll_cycle"].endswith(" ms")
    # (the period is 200ms, and a timer may fire a millisecond or two early)
    assert 0 < float(shown["polls_rate"].split()[0]) <= 1000 / 190


def test_status_view_shows_the_traffic_to_and_from_the_camera(obs_world, tmp_path):
    """In a table of a total and a rate for each of what is counted"""
    out = tmp_path / "dialog.json"
    open_dialog(obs_world, obs_world.device_names["visca-tcp"])
    shown = obs_world.wait_for_settings_dialog(
        out, lambda r: r["shown"].get("recv_bytes_rate", "-").endswith(" B/s")
        and r["shown"]["recv_bytes_rate"] != "0 B/s", timeout=15)["shown"]
    for row in ("sent_packets", "recv_packets", "sent_bytes", "recv_bytes"):
        assert shown[f"{row}_total"] not in ("-", "0"), row
    assert shown["sent_packets_rate"].endswith(" /s")
    assert shown["recv_packets_rate"].endswith(" /s")
    assert shown["sent_bytes_rate"].endswith(" B/s")
    # no errors is a total of none, not of nothing
    assert shown["errors_total"] != "-"
    assert shown["errors_rate"].endswith(" /s")


def test_creating_a_camera_report_from_the_status_view(request, obs_world, cameras, tmp_path):  # noqa: F811
    """The report dialog opens on the device shown, and makes its report"""
    cameras.add_source(obs_world.create_scene(), "dialog-report-cam")
    cameras.add_filter("dialog-report-cam")
    out = tmp_path / "device.json"
    device_name = obs_world.wait_for_device_by_name(
        "dialog-report-cam", out, lambda r: r["found"] and r["bound"])["uuid"]
    obs_world.wait_for_device_state(device_name, tmp_path / "state.json",
                                    lambda r: r["state"].get("connected") is True, timeout=10)
    open_dialog(obs_world, device_name)
    obs_world.wait_for_settings_dialog(tmp_path / "dialog.json", lambda r: r["diagnostics_visible"])
    request.addfinalizer(lambda: obs_world.run_ui_test("camera_report_dialog", action="close"))
    obs_world.run_ui_test("press_dialog_button", button="cameraReport")
    report = tmp_path / "report.json"

    def made():
        if report.exists():
            report.unlink()
        obs_world.run_ui_test("camera_report_dialog", action="read", filename=str(report))
        obs_world.wait_for(report.exists)
        shown = json.loads(report.read_text())
        return shown["open"] and shown["enabled"]["copy"]
    obs_world.wait_for(made, timeout=60)


def assert_obs_alive(obs_world):
    assert obs_world.ws.call("GetVersion")["obsVersion"]


def test_dialog_survives_its_device_going_away(obs_world, cameras, tmp_path):  # noqa: F811
    cameras.add_source(obs_world.create_scene(), "dialog-gone-cam")
    cameras.add_filter("dialog-gone-cam")
    out = tmp_path / "device.json"
    device_name = obs_world.wait_for_device_by_name("dialog-gone-cam", out, lambda r: r["found"] and r["bound"])["uuid"]
    open_dialog(obs_world, device_name)
    obs_world.settings_dialog(tmp_path / "dialog.json")

    cameras.remove_filter("dialog-gone-cam")
    obs_world.wait_for_device_by_name("dialog-gone-cam", out, lambda r: not r["found"], timeout=10)

    # still there, and still answering, with nothing to show
    obs_world.run_ui_test("edit_dialog_state", wb_mode=1)
    obs_world.settings_dialog(tmp_path / "dialog.json")
    assert_obs_alive(obs_world)


def test_dialog_survives_its_device_changing_interface(obs_world, cameras, tmp_path):  # noqa: F811
    cameras.add_source(obs_world.create_scene(), "dialog-iface-cam")
    cameras.add_filter("dialog-iface-cam")
    out = tmp_path / "device.json"
    device_name = obs_world.wait_for_device_by_name("dialog-iface-cam", out, lambda r: r["found"] and r["bound"])["uuid"]
    open_dialog(obs_world, device_name)

    obs_world.run_ui_test("update_device", device=device_name, type="visca-over-ip", host="127.0.0.1", udp_port=9)
    # give the change, and whatever the dialog does about it, time to happen
    time.sleep(1.0)

    # the dialog is still driveable, and so is what is under it
    obs_world.run_ui_test("edit_dialog_state", wb_mode=1)
    dialog = obs_world.settings_dialog(tmp_path / "dialog.json")
    assert {"wb_mode", "connected"} <= dialog["state_keys"]
    assert_obs_alive(obs_world)
    assert obs_world.device_by_name("dialog-iface-cam", out)["found"]
