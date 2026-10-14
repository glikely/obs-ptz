"""Covers the UI driver (tests/ui-driver/), a plugin of its own that lets a
client of obs-websocket find and drive OBS's Qt widgets, through its vendor,
"ui-driver". It knows nothing of obs-ptz, so it is tested on OBS's own
widgets, with the effect of what it does read back through OBS's own requests.

Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
SPDX-License-Identifier: GPL-2.0-or-later
"""

import pytest
from obsws import ObsWebSocketError


def test_the_driver_answers(obs_world):
    obs_world.need_ui_driver()
    assert obs_world.ui.call("ping") == {"driver": 1}


def test_an_unknown_request_is_no_request(obs_world):
    obs_world.need_ui_driver()
    with pytest.raises(ObsWebSocketError) as raised:
        obs_world.ws.vendor("ui-driver", "no_such_request")
    assert raised.value.status.get("code") == 600
