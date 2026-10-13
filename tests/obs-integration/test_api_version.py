"""Covers the PTZ API's version: that the ptz_get_api_version proc on each
device's proc_handler (src/core/ptz-device.cpp) reports the version src/ptz.h
declares, and that docs/ptz-device-api.md states it.

Asked over obs-websocket, as a client outside OBS asks (see "WebSocket
binding" in the document).
"""

import json
import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def declared_version():
    text = (REPO_ROOT / "src" / "ptz.h").read_text()
    return tuple(int(re.search(rf"^#define\s+PTZ_API_VERSION_{part}\s+(\d+)\s*$", text, re.MULTILINE).group(1))
                 for part in ("MAJOR", "MINOR"))


def test_each_device_reports_its_api_version(obs_world):
    for backend, device_name in obs_world.device_names.items():
        result = obs_world.api_version(device_name)
        assert result["called"] is True, backend
        assert (result["major"], result["minor"]) == declared_version(), backend


def test_the_doc_states_the_api_version():
    major, minor = declared_version()
    doc = (REPO_ROOT / "docs" / "ptz-device-api.md").read_text()
    assert f"This is version **{major}.{minor}** of the PTZ API" in doc
