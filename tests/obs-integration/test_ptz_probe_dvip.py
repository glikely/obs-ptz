"""ptz-probe over Datavideo's DVIP.

scripts/ptz-probe/ptz-probe.py makes an obs-ptz camera report without OBS
(see test_ptz_probe.py, where it is). This is the part of it that needs
ptzsim's DVIP, which comes with the DVIP interface, so isn't there.
"""

import json
import subprocess
import sys

import pytest

from conftest import REPO_ROOT, free_port, output_log, wait_for_port

PROBE = REPO_ROOT / "scripts" / "ptz-probe" / "ptz-probe.py"


@pytest.mark.skipif(not PROBE.exists(), reason="ptz-probe isn't here yet")
def test_the_probe_reaches_a_camera_over_dvip(request, tmp_path):
    port, debug = free_port(), free_port()
    cmd = [sys.executable, "-m", "ptzsim", "--host", "127.0.0.1",
           "--no-visca-tcp", "--no-visca-udp", "--no-visca-serial", "--visca-dvip-port", str(port),
           "--no-onvif", "--no-pelco", "--debug-http-port", str(debug)]
    with output_log("ptzsim-dvip") as out:
        proc = subprocess.Popen(cmd, cwd=REPO_ROOT / "scripts", stdout=out, stderr=subprocess.STDOUT)
    request.addfinalizer(proc.terminate)
    wait_for_port("127.0.0.1", debug, timeout=15)
    wait_for_port("127.0.0.1", port, timeout=15)

    report = tmp_path / "probe.json"
    done = subprocess.run([sys.executable, str(PROBE), "127.0.0.1", "--dvip", str(port), "-o", str(report)],
                          capture_output=True, text=True, timeout=120)
    assert report.exists(), done.stderr
    probed = json.loads(report.read_text())
    assert probed["type"] == "visca-over-dvip"
    assert probed["camera"]["vendor_id"] == "0001"
    commands = {c["key"]: c["result"] for c in probed["commands"]}
    assert commands["wb_mode"] == "completed"
