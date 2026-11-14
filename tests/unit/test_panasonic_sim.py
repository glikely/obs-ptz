"""Tests for ptzsim's Panasonic HTTP CGI backend against hand-crafted commands.

SPDX-License-Identifier: GPL-2.0-or-later

Run with python3 -m unittest tests/unit/test_panasonic_sim.py (or pytest). Needs no OBS.
"""

import sys
import unittest
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

from ptzsim.backends.panasonic import PanasonicBackend, PanasonicCameraLogic  # noqa: E402
from ptzsim.state import PTZState  # noqa: E402


class TestPanasonicCameraLogic(unittest.TestCase):
    def test_pt_speed_and_stop(self):
        state = PTZState()
        cam = PanasonicCameraLogic(state)

        code, _ = cam.handle_ptz_cmd("#PTS9901")
        self.assertEqual(code, 200)
        snap = state.snapshot()
        self.assertAlmostEqual(snap.pan_speed, 1.0)
        self.assertAlmostEqual(snap.tilt_speed, -1.0)

        code, _ = cam.handle_ptz_cmd("#PTS5050")
        self.assertEqual(code, 200)
        snap = state.snapshot()
        self.assertEqual(snap.pan_speed, 0.0)
        self.assertEqual(snap.tilt_speed, 0.0)

    def test_zoom_and_focus_speed(self):
        state = PTZState()
        cam = PanasonicCameraLogic(state)

        self.assertEqual(cam.handle_ptz_cmd("#Z99")[0], 200)
        self.assertEqual(cam.handle_ptz_cmd("#F01")[0], 200)
        snap = state.snapshot()
        self.assertAlmostEqual(snap.zoom_speed, 1.0)
        self.assertAlmostEqual(snap.focus_speed, -1.0)

        self.assertEqual(cam.handle_ptz_cmd("#Z50")[0], 200)
        self.assertEqual(cam.handle_ptz_cmd("#F50")[0], 200)
        snap = state.snapshot()
        self.assertEqual(snap.zoom_speed, 0.0)
        self.assertEqual(snap.focus_speed, 0.0)

    def test_absolute_positions_and_camdata(self):
        state = PTZState()
        cam = PanasonicCameraLogic(state)

        self.assertEqual(cam.handle_ptz_cmd("#APC8000FFFF")[0], 200)
        self.assertEqual(cam.handle_ptz_cmd("#AXZ555")[0], 200)
        self.assertEqual(cam.handle_ptz_cmd("#AXFFFF")[0], 200)
        self.assertEqual(cam.handle_ptz_cmd("#D10")[0], 200)
        self.assertEqual(cam.handle_ptz_cmd("#O0")[0], 200)

        camdata = cam.render_camdata()
        self.assertIn("p0\r\n", camdata)
        self.assertIn("aPC8000FFFF\r\n", camdata)
        self.assertIn("gz555\r\n", camdata)
        self.assertIn("gfFFF\r\n", camdata)
        self.assertIn("d10\r\n", camdata)

    def test_presets_set_recall_delete(self):
        state = PTZState()
        cam = PanasonicCameraLogic(state)

        cam.handle_ptz_cmd("#APC80008000")
        cam.handle_ptz_cmd("#M01")
        cam.handle_ptz_cmd("#APCFFFF0001")
        self.assertAlmostEqual(state.snapshot().pan, 1.0, places=3)

        cam.handle_ptz_cmd("#R01")
        self.assertAlmostEqual(state.snapshot().pan, 0.0, places=3)

        cam.handle_ptz_cmd("#C01")
        self.assertNotIn("01", state.list_presets())

    def test_invalid_commands_rejected(self):
        state = PTZState()
        cam = PanasonicCameraLogic(state)

        self.assertEqual(cam.handle_ptz_cmd("PTS5050")[0], 400)
        self.assertEqual(cam.handle_ptz_cmd("#PTS0050")[0], 400)
        self.assertEqual(cam.handle_ptz_cmd("#Z00")[0], 400)
        self.assertEqual(cam.handle_cam_cmd("BOGUS")[0], 400)


class TestPanasonicHttpBackend(unittest.TestCase):
    def test_http_roundtrip(self):
        state = PTZState()
        backend = PanasonicBackend(state, host="127.0.0.1", http_port=0)
        backend.start()
        try:
            base = f"http://127.0.0.1:{backend.http_port}"
            with urllib.request.urlopen(f"{base}/cgi-bin/aw_ptz?cmd=%23APC80008000&res=1") as r:
                self.assertEqual(r.status, 200)
            with urllib.request.urlopen(f"{base}/cgi-bin/aw_cam?cmd=OSE:69:1&res=1") as r:
                self.assertEqual(r.status, 200)
            self.assertEqual(backend.logic.onepush_af_count, 1)
            with urllib.request.urlopen(f"{base}/live/camdata.html") as r:
                self.assertEqual(r.status, 200)
                body = r.read().decode("utf-8")
            self.assertIn("aPC80008000\r\n", body)
            self.assertIn("p1\r\n", body)
        finally:
            backend.stop()


if __name__ == "__main__":
    unittest.main()

