"""Camera reports: what a camera has, for its user to send in.

A device's "camera_report" trigger asks the camera what it has, and the
"ptz_get_camera_report" proc hands back the report, as JSON, for the user
to look at and send in themselves (see docs/visca-protocol.md). Nothing in
it may identify the camera, its user, or where either is.
"""

import json
import urllib.parse

from test_visca_profiles import camera


def test_there_is_no_report_until_one_is_made(request, obs_world, tmp_path):
    _, _, device_name = camera(request, obs_world, tmp_path)
    assert obs_world.camera_report(device_name, tmp_path / "report.json") is None


SONY = {"model": "0001:0000", "flags": ()}
# ptzsim's camera ID, 0xfedc, a nibble a byte as VISCA has it
CAMERA_ID = "0f0e0d0c"


def report(obs_world, tmp_path, device_name, timeout=60):
    """Makes the device's camera report, and hands it back"""
    obs_world.run_ui_test("trigger_device", device=device_name, name="camera_report")
    obs_world.wait_for_device_state(
        device_name, tmp_path / "started.json", lambda r: "camera_report" in r["state"], timeout=10)
    obs_world.wait_for_device_state(
        device_name, tmp_path / "done.json", lambda r: not r["state"]["camera_report"]["running"], timeout=timeout)
    return obs_world.camera_report(device_name, tmp_path / "report.json")


def test_a_report_has_what_the_camera_answered(request, obs_world, tmp_path):
    sim, _, device_name = camera(request, obs_world, tmp_path, sim_args=SONY, read={"wb_mode"})
    before = sim.state()
    made = report(obs_world, tmp_path, device_name)

    assert made["report"] == "obs-ptz camera report"
    assert made["protocol"] == "visca"
    assert made["type"] == "visca-over-tcp"
    assert made["camera"]["vendor_id"] == "0001"
    assert made["camera"]["model_id"] == "0000"
    inquiries = {i["inquiry"]: i for i in made["inquiries"]}
    assert inquiries["81090002ff"]["reply"].startswith("9050")
    assert inquiries["81090400ff"]["reply"] == "905002ff"
    commands = {c["key"]: c for c in made["commands"]}
    assert commands["wb_mode"] == {"key": "wb_mode", "command": "8101043500ff", "result": "completed"}
    assert "error" not in made["draft_command_set"]

    # the values it set are as they were: it changed nothing, and didn't move
    after = sim.state()
    assert after["visca"] == before["visca"]
    assert (after["pan"], after["tilt"], after["zoom"]) == (before["pan"], before["tilt"], before["zoom"])


def test_a_report_says_nothing_about_who_or_where(request, obs_world, tmp_path):
    """Not the camera's ID, which its user gives it, nor where the camera is,
    nor what the user calls it"""
    sim, _, device_name = camera(request, obs_world, tmp_path, sim_args=SONY, read={"wb_mode"})
    made = report(obs_world, tmp_path, device_name)
    text = json.dumps(made)
    assert CAMERA_ID not in text
    assert "127.0.0.1" not in text
    assert str(sim.tcp_port) not in text
    name = obs_world.device_state(device_name, tmp_path / "state.json")["state"]["source"]
    assert name and name not in text
    inquiries = {i["inquiry"]: i for i in made["inquiries"]}
    assert inquiries["81090422ff"] == {"inquiry": "81090422ff", "reply": "905000000000ff", "masked": ["camera_id"]}
    assert "camera_id" not in {c["key"] for c in made["commands"]}


def test_a_report_has_what_the_camera_doesnt_have(request, obs_world, tmp_path):
    """A BirdDog P100 has none of the block inquiries"""
    sim, _, device_name = camera(request, obs_world, tmp_path)
    made = report(obs_world, tmp_path, device_name)
    inquiries = {i["inquiry"]: i for i in made["inquiries"]}
    assert inquiries["81097e7e00ff"] == {"inquiry": "81097e7e00ff", "error": "syntax error"}
    assert made["command_set"] == "birddog-p100"


def test_a_report_says_which_commands_never_complete(request, obs_world, tmp_path):
    """A BirdDog backpack ACKs a command and never says it is done: the
    report is still made, with the commands it only ACKed"""
    sim, _, device_name = camera(request, obs_world, tmp_path, read={"wb_mode"},
                               sim_args={"model": "0001:0000", "flags": ("--visca-no-completions",)})
    made = report(obs_world, tmp_path, device_name)
    assert made["commands"]
    assert {c["result"] for c in made["commands"]} == {"ack"}


def test_a_report_drafts_a_command_set_for_the_camera(request, obs_world, tmp_path):
    """For its user to try, and to send in: the generic one, for the camera's
    model, without what the P100 doesn't have"""
    sim, _, device_name = camera(request, obs_world, tmp_path)
    made = report(obs_world, tmp_path, device_name)
    draft = made["draft_command_set"]
    assert draft["id"] == "my-camera-0109-2020"
    assert draft["models"] == ["0109:2020"]
    assert draft["extends"] == "generic"
    # the plugin can read it
    assert "error" not in draft
    failed = {i["inquiry"] for i in made["inquiries"] if "error" in i}
    assert "81097e7e00ff" in draft["remove_inquiries"]
    assert set(draft["remove_inquiries"]) <= failed
    # everything it can set, it took
    assert draft["controls"] == []


def dialog(obs_world, out_file, predicate=lambda d: True, timeout=60):
    """What the camera report dialog shows, once `predicate` says it shows
    what it should"""
    def read():
        if out_file.exists():
            out_file.unlink()
        obs_world.run_ui_test("camera_report_dialog", action="read", filename=str(out_file))
        obs_world.wait_for(out_file.exists)
        return json.loads(out_file.read_text())
    shown = None

    def shows():
        nonlocal shown
        shown = read()
        return shown["open"] and predicate(shown)
    obs_world.wait_for(shows, timeout=timeout)
    return shown


def test_the_dialog_shows_the_report_to_save_copy_or_send(request, obs_world, tmp_path):
    sim, _, device_name = camera(request, obs_world, tmp_path, sim_args=SONY, read={"wb_mode"})
    request.addfinalizer(lambda: obs_world.run_ui_test("camera_report_dialog", action="close"))
    obs_world.run_ui_test("camera_report_dialog", action="open", device=device_name)
    shown = dialog(obs_world, tmp_path / "dialog.json", lambda d: d["enabled"]["copy"])
    assert shown["enabled"] == {"save": True, "copy": True, "openIssue": True}
    assert shown["progress"] == shown["progress_max"]
    made = obs_world.camera_report(device_name, tmp_path / "report.json")
    assert json.loads(shown["report"]) == made
    assert "visca-profiles" in shown["status"]
    url = urllib.parse.urlsplit(shown["issue_url"])
    assert url[:3] == ("https", "github.com", "/glikely/obs-ptz/issues/new")
    query = urllib.parse.parse_qs(url.query)
    assert query["template"] == ["camera-report.yml"]
    assert query["camera"] == ["Sony"]
    assert query["plugin-version"] == [made["plugin_version"]]

    obs_world.run_ui_test("camera_report_dialog", action="click", button="copy")
    shown = dialog(obs_world, tmp_path / "copied.json", lambda d: d["clipboard"])
    assert json.loads(shown["clipboard"]) == made


def test_the_dialog_says_a_camera_that_isnt_connected_cant_be_asked(request, obs_world, tmp_path):
    sim, _, device_name = camera(request, obs_world, tmp_path, sim_args=SONY, read={"wb_mode"})
    sim.stop()
    obs_world.wait_for_device_state(
        device_name, tmp_path / "gone.json", lambda r: r["state"]["connected"] is False, timeout=20)
    request.addfinalizer(lambda: obs_world.run_ui_test("camera_report_dialog", action="close"))
    obs_world.run_ui_test("camera_report_dialog", action="open", device=device_name)
    shown = dialog(obs_world, tmp_path / "dialog.json", lambda d: "connected" in d["status"], timeout=10)
    assert shown["enabled"] == {"save": False, "copy": False, "openIssue": False}
