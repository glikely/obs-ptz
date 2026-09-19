"""End-to-end tests: a real OBS Studio + obs-ptz plugin talking to a real
ptzsim instance over each wire protocol.

Each test triggers a `ptz_action_source` (see src/ptz-action-source.c)
through obs-websocket, which calls straight through obs-ptz's proc
handler into the real PTZDevice for that backend -- the same code path
a joystick move or a hotkey would use -- and then asserts on ptzsim's
debug state that the expected wire command actually arrived and was
decoded correctly.
"""

import time

import pytest

ACTION_PRESET_RECALL = 2
ACTION_PAN_TILT = 3
ACTION_STOP = 4
ACTION_PRESET_SAVE = 5

BACKENDS = ["visca-tcp", "visca-udp", "visca-serial", "pelco-d", "pelco-p"]


@pytest.mark.parametrize("backend", BACKENDS)
def test_pan_tilt_and_stop(obs_world, backend):
    device_id = obs_world.device_ids[backend]

    obs_world.trigger_action(device_id, ACTION_PAN_TILT, pan_speed=0.5, tilt_speed=-0.3)
    obs_world.wait_for_state(
        lambda s: abs(s["pan_speed"] - 0.5) < 0.05 and abs(s["tilt_speed"] + 0.3) < 0.05)

    obs_world.trigger_action(device_id, ACTION_STOP)
    obs_world.wait_for_state(lambda s: s["pan_speed"] == 0.0 and s["tilt_speed"] == 0.0)


@pytest.mark.parametrize("backend", BACKENDS)
def test_preset_save_and_recall(obs_world, backend):
    device_id = obs_world.device_ids[backend]

    # Move to a known, non-default position and stop there.
    obs_world.trigger_action(device_id, ACTION_PAN_TILT, pan_speed=0.6, tilt_speed=0.0)
    time.sleep(0.5)
    obs_world.trigger_action(device_id, ACTION_STOP)
    saved = obs_world.wait_for_state(lambda s: s["pan_speed"] == 0.0)
    saved_pan = saved["pan"]
    assert abs(saved_pan) > 0.01, "camera didn't actually move before saving the preset"

    obs_world.trigger_action(device_id, ACTION_PRESET_SAVE, preset_id=1)
    time.sleep(0.2)

    # Move away from the saved position.
    obs_world.trigger_action(device_id, ACTION_PAN_TILT, pan_speed=-0.6, tilt_speed=0.0)
    time.sleep(0.5)
    obs_world.trigger_action(device_id, ACTION_STOP)
    obs_world.wait_for_state(lambda s: s["pan_speed"] == 0.0 and abs(s["pan"] - saved_pan) > 0.05)

    obs_world.trigger_action(device_id, ACTION_PRESET_RECALL, preset_id=1)
    obs_world.wait_for_state(lambda s: abs(s["pan"] - saved_pan) < 0.02, timeout=5)
