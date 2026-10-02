"""ptz-probe: an obs-ptz camera report without OBS.

scripts/ptz-probe/ptz-probe.py asks a camera what the plugin's own camera
report does, and writes the same report. These run it against ptzsim, over
each way there is to a camera, and against the plugin's report of the same
camera.
"""

import json
import subprocess
import sys
import time

import pytest

from conftest import REPO_ROOT, free_port, output_log, wait_for_port
from test_camera_report import CAMERA_ID, report
from test_visca_profiles import Sim, camera

PROBE = REPO_ROOT / "scripts" / "ptz-probe" / "ptz-probe.py"
P100 = ("--visca-no-block-inquiries", "--visca-no-green-tally")


def probe(tmp_path, *args):
    """Runs the probe: its report, or None, and what it said"""
    out = tmp_path / "probe.json"
    if out.exists():
        out.unlink()
    done = subprocess.run([sys.executable, str(PROBE), *args, "-o", str(out)],
                          capture_output=True, text=True, timeout=120)
    return (json.loads(out.read_text()) if out.exists() else None), done


class LinkSim:
    """ptzsim with only the way to it a test asks for: "udp" or "serial",
    and the probe's arguments to get to it"""

    def __init__(self, request, tmp_path, link, flags=()):
        port, debug = free_port(), free_port()
        self.debug_port = debug
        args = ["--no-visca-tcp", "--no-visca-udp", "--no-visca-serial"]
        if link == "udp":
            args = ["--no-visca-tcp", "--no-visca-serial", "--visca-udp-port", str(port)]
            self.probe_args = ["127.0.0.1", "--udp", str(port)]
        else:
            path = tmp_path / "visca-serial"
            args = ["--no-visca-tcp", "--no-visca-udp", "--visca-serial-path", str(path)]
            self.probe_args = [str(path), "--serial"]
        cmd = [sys.executable, "-m", "ptzsim", "--host", "127.0.0.1", *args, *flags,
               "--no-onvif", "--no-pelco", "--debug-http-port", str(debug)]
        with output_log(f"ptzsim-{link}") as out:
            self.proc = subprocess.Popen(cmd, cwd=REPO_ROOT / "scripts", stdout=out, stderr=subprocess.STDOUT)
        request.addfinalizer(self.stop)
        wait_for_port("127.0.0.1", debug, timeout=15)
        if link == "serial":
            deadline = time.monotonic() + 10
            while not (tmp_path / "visca-serial").exists():
                assert time.monotonic() < deadline, "ptzsim has no serial port"
                time.sleep(0.1)

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def test_the_probe_reports_what_the_plugin_does(request, obs_world, tmp_path):
    """Of a P100, which has neither block inquiries nor a green tally lamp:
    everything the plugin asked, the probe asked and was answered the same,
    and it drafts the same command set"""
    sim, _, device_id = camera(request, obs_world, tmp_path)
    made = report(obs_world, tmp_path, device_id)
    probed, done = probe(tmp_path, "127.0.0.1", "--tcp", str(sim.tcp_port))
    assert probed is not None, done.stderr

    assert probed["report"] == "obs-ptz camera report"
    assert probed["made_by"] == "ptz-probe"
    assert probed["type"] == "visca-over-tcp"
    for key in ("format", "plugin_version", "os", "protocol", "camera", "command_set", "buffer_full"):
        assert probed[key] == made[key], key
    asked = {i["inquiry"]: i for i in probed["inquiries"]}
    for inquiry in made["inquiries"]:
        assert asked[inquiry["inquiry"]] == inquiry
    assert {c["key"]: c for c in probed["commands"]} == {c["key"]: c for c in made["commands"]}
    drafted, plugins = dict(probed["draft_command_set"]), dict(made["draft_command_set"])
    assert drafted.pop("source").startswith("Drafted from a camera report by ptz-probe")
    plugins.pop("source")
    assert drafted == plugins


def test_the_probe_says_nothing_about_who_or_where(request, tmp_path):
    sim = Sim(model="0001:0000", flags=())
    request.addfinalizer(sim.stop)
    probed, done = probe(tmp_path, "127.0.0.1", "--tcp", str(sim.tcp_port))
    text = json.dumps(probed)
    assert CAMERA_ID not in text
    assert "127.0.0.1" not in text
    assert str(sim.tcp_port) not in text
    inquiries = {i["inquiry"]: i for i in probed["inquiries"]}
    assert inquiries["81090422ff"] == {"inquiry": "81090422ff", "reply": "905000000000ff", "masked": ["camera_id"]}
    # and changed nothing
    assert sim.state()["visca"]["camera_id"] == 0xfedc


@pytest.mark.parametrize("link,kind", [("udp", "visca-over-ip"), ("serial", "visca")])
def test_the_probe_reaches_a_camera_every_way_the_plugin_does(request, tmp_path, link, kind):
    sim = LinkSim(request, tmp_path, link)
    probed, done = probe(tmp_path, *sim.probe_args)
    assert probed is not None, done.stderr
    assert probed["type"] == kind
    assert probed["camera"]["vendor_id"] == "0001"
    commands = {c["key"]: c["result"] for c in probed["commands"]}
    assert commands["wb_mode"] == "completed"


def test_a_probes_report_replays(request, tmp_path):
    """ptzsim, replaying the probe's report of a camera, is that camera, to
    the probe too"""
    sim = Sim(flags=P100)
    request.addfinalizer(sim.stop)
    probed, _ = probe(tmp_path, "127.0.0.1", "--tcp", str(sim.tcp_port))
    path = tmp_path / "replayed.json"
    path.write_text(json.dumps(probed))
    replay = Sim(flags=("--visca-report", str(path)))
    request.addfinalizer(replay.stop)
    again, _ = probe(tmp_path, "127.0.0.1", "--tcp", str(replay.tcp_port))
    assert again["camera"] == probed["camera"]
    assert again["inquiries"] == probed["inquiries"]
    assert again["commands"] == probed["commands"]
