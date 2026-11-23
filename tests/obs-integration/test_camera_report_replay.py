"""Camera reports replayed: ptzsim as the camera a report was made of.

Given a report with --visca-report, ptzsim answers what the camera answered
as it did, and refuses what it refused (see scripts/ptzsim/backends/
visca_report.py). So each report sent in, kept in tests/camera-reports/,
is a camera the plugin is tested against: the command set the plugin
chooses for it must ask it only for what it has.
"""

import json
import time
from pathlib import Path

import pytest

from conftest import REPO_ROOT, SETTINGS_DIALOG, STATE_VIEW
from test_camera_report import report
from test_visca_profiles import Sim, camera

REPORTS = sorted((REPO_ROOT / "tests" / "camera-reports").glob("*.json"))


def replaying(made, tmp_path):
    """ptzsim's arguments to be the camera `made` is a report of"""
    path = tmp_path / "replayed.json"
    path.write_text(json.dumps(made))
    return {"flags": ("--visca-report", str(path))}


def test_a_replayed_camera_answers_as_the_camera_did(request, obs_world, tmp_path):
    """A report of the replayed camera is the report it was replayed from"""
    sim, _, device_name = camera(request, obs_world, tmp_path)
    made = report(obs_world, tmp_path, device_name)

    (tmp_path / "replay").mkdir()
    replay, state, device_name = camera(request, obs_world, tmp_path / "replay", sim_args=replaying(made, tmp_path))
    assert (state["vendor_id"], state["model_id"]) == (0x0109, 0x2020)
    again = report(obs_world, tmp_path / "replay", device_name)

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
    sim, state, device_name = camera(request, obs_world, tmp_path, sim_args=replaying(made, tmp_path),
                                   read={"wb_mode", "zoom_pos"})
    assert (state["vendor_id"], state["model_id"]) == (0x0123, 0x0009)
    # what isn't in the report isn't there: ae_mode isn't read
    assert "ae_mode" not in state
    assert sim.syntax_errors() > 0

    obs_world.set_device_state(device_name, wb_mode=1)
    obs_world.wait_for_device_state(device_name, tmp_path / "set.json", lambda r: r["state"]["wb_mode"] == 1,
                                    timeout=10)
    errors = sim.syntax_errors()
    obs_world.set_device_state(device_name, ae_mode=3)
    sim.wait_for(lambda s: s["visca_syntax_errors"] > errors)
    assert sim.state()["visca"]["ae_mode"] == 0


def refusing_gain(result):
    """A camera report of one that answers Gain Direct with `result`, and
    takes the white balance mode"""
    return {
        "report": "obs-ptz camera report", "format": 1, "protocol": "visca",
        "camera": {"vendor_id": "0123", "model_id": "0009", "rom_version": "0001"},
        "inquiries": [
            {"inquiry": "81090002ff", "reply": "9050012300090001ff"},
            {"inquiry": "81090400ff", "reply": "905002ff"},
            {"inquiry": "81090435ff", "reply": "905000ff"},
            {"inquiry": "8109044cff", "reply": "905000000005ff"},
        ],
        "commands": [
            {"key": "wb_mode", "command": "8101043500ff", "result": "completed"},
            {"key": "gain_pos", "command": "8101044c00000005ff", "result": result},
        ],
    }


def test_a_value_the_camera_cant_be_set_isnt_sent(request, obs_world, tmp_path):
    """The plugin sets each value it reads to what the camera says it is,
    which changes nothing. One whose command the camera refuses with a syntax
    error isn't sent again, whatever it is set to; one it takes still is."""
    sim, state, device_name = camera(request, obs_world, tmp_path, sim_args=replaying(refusing_gain("syntax error"),
                                                                                      tmp_path),
                                   read={"wb_mode", "gain_pos"})
    # the test of Gain Direct, at what the camera has, is refused: the
    # inquiries the report hasn't were too, once each, when first read
    gain = sim.state()["visca"]["gain_pos"]
    state = obs_world.wait_for_device_state(device_name, tmp_path / "read-only.json",
                                            lambda r: "read_only" in r["state"], timeout=15)["state"]
    assert state["read_only"] == {"gain_pos": True}
    errors = sim.syntax_errors()
    assert errors > 0

    obs_world.set_device_state(device_name, gain_pos=gain + 2)
    obs_world.set_device_state(device_name, wb_mode=1)
    sim.wait_for(lambda s: s["visca"]["wb_mode"] == 1)
    assert sim.syntax_errors() == errors
    assert sim.state()["visca"]["gain_pos"] == gain

    # and the settings show it, but don't offer to change it
    obs_world.open_settings_dialog(device_name)
    obs_world.wait_for_settings_dialog(tmp_path / "dialog.json", lambda r: r["shown"].get("gain_pos") == gain,
                                       timeout=15)
    assert not obs_world.ui.get1(SETTINGS_DIALOG, STATE_VIEW, "gain_pos", name="enabled")
    assert obs_world.ui.get1(SETTINGS_DIALOG, STATE_VIEW, "wb_mode", name="enabled")


def test_a_value_the_camera_wont_set_now_isnt_read_only(request, obs_world, tmp_path):
    """"Not executable" is the camera not doing it in the mode it is in, which
    says nothing of whether it can: it is still sent"""
    sim, _, device_name = camera(request, obs_world, tmp_path, sim_args=replaying(refusing_gain("not executable"),
                                                                                   tmp_path),
                                 read={"wb_mode", "gain_pos"})
    # long enough for each value to have been tested
    time.sleep(5)
    assert "read_only" not in obs_world.wait_for_device_state(
        device_name, tmp_path / "state.json", lambda r: "gain_pos" in r["state"])["state"]
    def commands_sent():
        return obs_world.device_state(device_name, tmp_path / "statistics.json")["state"]["statistics"].get(
            "visca_command_count", 0)
    sent = commands_sent()
    gain = sim.state()["visca"]["gain_pos"]
    obs_world.set_device_state(device_name, gain_pos=gain + 2)
    obs_world.wait_for(lambda: commands_sent() > sent, timeout=5)


def test_another_camera_is_tested_again(request, obs_world, tmp_path):
    """What the test found is of the camera it found it of: pointed at one that
    takes Gain Direct, the device has nothing read-only once it has tested it"""
    sim, _, device_name = camera(request, obs_world, tmp_path, sim_args=replaying(refusing_gain("syntax error"),
                                                                                   tmp_path),
                                 read={"wb_mode", "gain_pos"})
    obs_world.wait_for_device_state(device_name, tmp_path / "state.json",
                                    lambda r: r["state"].get("read_only") == {"gain_pos": True}, timeout=15)

    other = Sim(model="0123:0009", flags=())
    request.addfinalizer(other.stop)
    obs_world.update_device(device_name, tcp_port=other.tcp_port)
    obs_world.wait_for_device_state(device_name, tmp_path / "state.json",
                                    lambda r: r["state"].get("read_only", {}) == {}, timeout=15)
    # and it can be set again
    gain = other.state()["visca"]["gain_pos"]
    obs_world.set_device_state(device_name, gain_pos=gain + 2)
    other.wait_for(lambda s: s["visca"]["gain_pos"] == gain + 2)


@pytest.mark.parametrize("path", REPORTS, ids=[p.stem for p in REPORTS])
def test_the_plugin_asks_a_reported_camera_only_for_what_it_has(request, obs_world, tmp_path, path):
    """The command set the plugin chooses for each camera that has been
    reported asks it for nothing it doesn't have: when it first reads
    everything, and then one inquiry a second. One the plugin has no command
    set for gets the generic one,
    which finds out what the camera doesn't have by asking."""
    made = json.loads(Path(path).read_text())
    sim, state, device_name = camera(request, obs_world, tmp_path, read={"model_id"},
                                   sim_args={"flags": ("--visca-report", str(path))})
    if state.get("model_name") is None or made.get("command_set") == "generic":
        pytest.skip("the plugin has no command set for it")
    # from the first request: one the camera doesn't have is asked once,
    # and not again; everything is read when the device connects, then one
    # inquiry a second
    time.sleep(12)
    assert sim.syntax_errors() == 0
