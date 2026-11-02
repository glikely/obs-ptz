"""Covers ptzsim's Pelco camera against frames worked out by hand, not by the
plugin, so that the camera the rest of the suite drives can't share a mistake
of the plugin's checksum (see test_driver_kinds.py for that one).

SPDX-License-Identifier: GPL-2.0-or-later

Run with python3 -m pytest tests/unit/test_pelco_frames.py. Needs no OBS.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

from ptzsim.backends.pelco import PelcoCameraLogic  # noqa: E402
from ptzsim.state import PTZState  # noqa: E402


def test_d_frame_moves_the_camera():
    state = PTZState()
    PelcoCameraLogic(state).handle_frame(bytes.fromhex("ff 01 00 12 3f 3f 91"))
    assert state.snapshot().pan_speed != 0.0
    assert state.snapshot().tilt_speed != 0.0


def test_d_frame_with_the_wrong_checksum_is_ignored():
    state = PTZState()
    # 0x91 % 100, the checksum the driver once sent
    PelcoCameraLogic(state).handle_frame(bytes.fromhex("ff 01 00 12 3f 3f 2d"))
    assert state.snapshot().pan_speed == 0.0
