"""Covers the PTZ API's version: that the ptz_get_api_version proc, on OBS's
own proc_handler and on each device's (src/ptz-device.cpp), reports the
version src/ptz.h declares, and that docs/ptz-device-api.md states it.

Driven through tests/ui-harness/api-version-test.cpp's "get_api_version"
test, which calls the proc the way another plugin would.
"""

import json
import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def declared_version():
    text = (REPO_ROOT / "src" / "ptz.h").read_text()
    return tuple(int(re.search(rf"^#define\s+PTZ_API_VERSION_{part}\s+(\d+)\s*$", text, re.MULTILINE).group(1))
                 for part in ("MAJOR", "MINOR"))


def test_the_plugin_reports_its_api_version(obs_world, tmp_path):
    out = tmp_path / "version.json"
    obs_world.run_ui_test("get_api_version", filename=str(out))
    obs_world.wait_for(out.exists)

    result = json.loads(out.read_text())
    assert result["called"] is True
    assert (result["major"], result["minor"]) == declared_version()


def test_each_device_reports_its_api_version(obs_world, tmp_path):
    for backend, device_id in obs_world.device_ids.items():
        out = tmp_path / f"{backend}.json"
        obs_world.run_ui_test("get_api_version", device_id=device_id, filename=str(out))
        obs_world.wait_for(out.exists)

        result = json.loads(out.read_text())
        assert result["called"] is True, backend
        assert (result["major"], result["minor"]) == declared_version(), backend


def test_the_doc_states_the_api_version():
    major, minor = declared_version()
    doc = (REPO_ROOT / "docs" / "ptz-device-api.md").read_text()
    assert f"This is version **{major}.{minor}** of the PTZ API" in doc
