"""Camera reports replayed: ptzsim as the camera a report was made of.

Given a report with --visca-report, ptzsim answers what the camera answered
as it did, and refuses what it refused (see scripts/ptzsim/backends/
visca_report.py).
"""

import json

from test_camera_report import report
from test_visca_profiles import camera


def replaying(made, tmp_path):
    """ptzsim's arguments to be the camera `made` is a report of"""
    path = tmp_path / "replayed.json"
    path.write_text(json.dumps(made))
    return {"flags": ("--visca-report", str(path))}


def test_a_replayed_camera_answers_as_the_camera_did(request, obs_world, tmp_path):
    """A report of the replayed camera is the report it was replayed from"""
    sim, _, device_id = camera(request, obs_world, tmp_path)
    made = report(obs_world, tmp_path, device_id)

    (tmp_path / "replay").mkdir()
    replay, state, device_id = camera(request, obs_world, tmp_path / "replay", sim_args=replaying(made, tmp_path))
    assert (state["vendor_id"], state["model_id"]) == (0x0109, 0x2020)
    again = report(obs_world, tmp_path / "replay", device_id)

    assert again["camera"] == made["camera"]
    assert again["command_set"] == made["command_set"]
    assert {i["inquiry"]: i for i in again["inquiries"]} == {i["inquiry"]: i for i in made["inquiries"]}
    assert {c["key"]: c for c in again["commands"]} == {c["key"]: c for c in made["commands"]}
    assert again["draft_command_set"] == made["draft_command_set"]


def test_a_replayed_camera_refuses_what_the_camera_did(request, obs_world, tmp_path):
    """And doesn't answer what it didn't, and takes what it took, as ptzsim
    has it, so what is set shows in what is read back"""
    made = {
        "report": "obs-ptz camera report", "format": 1, "protocol": "visca",
        "camera": {"vendor_id": "0123", "model_id": "0009", "rom_version": "0001"},
        "inquiries": [
            {"inquiry": "81090002ff", "reply": "9050012300090001ff"},
            {"inquiry": "81090400ff", "reply": "905002ff"},
            {"inquiry": "81090435ff", "reply": "905000ff"},
            {"inquiry": "81090447ff", "reply": "905000000000ff"},
        ],
        "commands": [
            {"key": "wb_mode", "command": "8101043500ff", "result": "completed"},
            {"key": "ae_mode", "command": "8101043900ff", "result": "syntax error"},
        ],
    }
    sim, state, device_id = camera(request, obs_world, tmp_path, sim_args=replaying(made, tmp_path),
                                   read={"wb_mode", "zoom_pos"})
    assert (state["vendor_id"], state["model_id"]) == (0x0123, 0x0009)
    # what isn't in the report isn't there: ae_mode isn't read
    assert "ae_mode" not in state
    assert sim.syntax_errors() > 0

    obs_world.run_ui_test("set_device_state", device_id=device_id, wb_mode=1)
    obs_world.wait_for_device_state(device_id, tmp_path / "set.json", lambda r: r["state"]["wb_mode"] == 1,
                                    timeout=10)
    errors = sim.syntax_errors()
    obs_world.run_ui_test("set_device_state", device_id=device_id, ae_mode=3)
    sim.wait_for(lambda s: s["visca_syntax_errors"] > errors)
    assert sim.state()["visca"]["ae_mode"] == 0

