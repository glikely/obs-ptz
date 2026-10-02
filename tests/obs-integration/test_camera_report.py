"""Camera reports: what a camera has, for its user to send in.

A device's "camera_report" trigger asks the camera what it has, and the
"ptz_get_camera_report" proc hands back the report, as JSON, for the user
to look at and send in themselves (see doc/visca-protocol.md). Nothing in
it may identify the camera, its user, or where either is.
"""

import json

from test_visca_profiles import camera


def test_there_is_no_report_until_one_is_made(request, obs_world, tmp_path):
    _, _, device_id = camera(request, obs_world, tmp_path)
    assert obs_world.camera_report(device_id, tmp_path / "report.json") is None


SONY = {"model": "0001:0000", "flags": ()}
# ptzsim's camera ID, 0xfedc, a nibble a byte as VISCA has it
CAMERA_ID = "0f0e0d0c"


def report(obs_world, tmp_path, device_id, timeout=60):
    """Makes the device's camera report, and hands it back"""
    obs_world.run_ui_test("trigger_device", device_id=device_id, name="camera_report")
    obs_world.wait_for_device_state(
        device_id, tmp_path / "started.json", lambda r: "camera_report" in r["state"], timeout=10)
    obs_world.wait_for_device_state(
        device_id, tmp_path / "done.json", lambda r: not r["state"]["camera_report"]["running"], timeout=timeout)
    return obs_world.camera_report(device_id, tmp_path / "report.json")


def test_a_report_has_what_the_camera_answered(request, obs_world, tmp_path):
    sim, _, device_id = camera(request, obs_world, tmp_path, sim_args=SONY, read={"wb_mode"})
    before = sim.state()
    made = report(obs_world, tmp_path, device_id)

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

    # the values it set are as they were: it changed nothing, and didn't move
    after = sim.state()
    assert after["visca"] == before["visca"]
    assert (after["pan"], after["tilt"], after["zoom"]) == (before["pan"], before["tilt"], before["zoom"])


def test_a_report_says_nothing_about_who_or_where(request, obs_world, tmp_path):
    """Not the camera's ID, which its user gives it, nor where the camera is,
    nor what the user calls it"""
    sim, state, device_id = camera(request, obs_world, tmp_path, sim_args=SONY, read={"wb_mode"})
    made = report(obs_world, tmp_path, device_id)
    text = json.dumps(made)
    assert CAMERA_ID not in text
    assert "127.0.0.1" not in text
    assert str(sim.tcp_port) not in text
    assert state["name"] not in text
    inquiries = {i["inquiry"]: i for i in made["inquiries"]}
    assert inquiries["81090422ff"] == {"inquiry": "81090422ff", "reply": "905000000000ff", "masked": ["camera_id"]}
    assert "camera_id" not in {c["key"] for c in made["commands"]}


def test_a_report_has_what_the_camera_doesnt_have(request, obs_world, tmp_path):
    """A BirdDog P100 has none of the block inquiries"""
    sim, _, device_id = camera(request, obs_world, tmp_path)
    made = report(obs_world, tmp_path, device_id)
    inquiries = {i["inquiry"]: i for i in made["inquiries"]}
    assert inquiries["81097e7e00ff"] == {"inquiry": "81097e7e00ff", "error": "syntax error"}
    assert made["command_set"] == "birddog-p100"


def test_a_report_says_which_commands_never_complete(request, obs_world, tmp_path):
    """A BirdDog backpack ACKs a command and never says it is done: the
    report is still made, with the commands it only ACKed"""
    sim, _, device_id = camera(request, obs_world, tmp_path, read={"wb_mode"},
                               sim_args={"model": "0001:0000", "flags": ("--visca-no-completions",)})
    made = report(obs_world, tmp_path, device_id)
    assert made["commands"]
    assert {c["result"] for c in made["commands"]} == {"ack"}
