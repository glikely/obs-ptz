"""End-to-end tests for absolute and relative pan/tilt/zoom moves --
PTZDevice::move_abs()/move_rel(), landing on PTZVisca::pantilt_abs()/
pantilt_rel()/zoom_abs() in src/ptz-visca.cpp.

There's no ptz_action_source action type for either of these -- its own
PTZ_ACTION_* enum (src/ptz-action-source.c) only covers continuous
pan/tilt, stop, presets and power -- so they're driven through
tests/ui-harness/move-device-test.cpp's "move_device" test instead of
World.trigger_action().

VISCA-only: Pelco never overrides pantilt_abs()/pantilt_rel()/zoom_abs()
(PTZDevice's own base implementations are empty no-ops -- see
src/ptz-device.hpp), so those calls would be silent no-ops there.

Pan-tiltDrive-AbsolutePosition/RelativePosition and CAM_Zoom-Direct
encode their pan/tilt/zoom arguments over much smaller wire ranges
(0x1400/0x500 for absolute, doubled for relative, 0x7ac0 for zoom -- see
PTZVisca::pantilt_abs()/pantilt_rel()/zoom_abs()) than the range ptzsim
decodes positions with (PT_POS_RANGE/ZF_POS_RANGE = 0xe500, see
scripts/ptzsim/backends/visca.py). Both sides read/write the same raw
wire value the same way, so e.g. a pan=1.0 request only ever lands
ptzsim's own reported "pan" at 0x1400/0xe500 =~ 0.087, not 1.0 -- these
tests assert that exact fraction rather than just "some" movement.
"""

import pytest

VISCA_BACKENDS = ["visca-tcp", "visca-udp", "visca-serial"]

POS_RANGE = 0xe500
ABS_PAN_RANGE = 0x1400
ABS_TILT_RANGE = 0x500
REL_PAN_RANGE = 0x1400 * 2
REL_TILT_RANGE = 0x500 * 2
ABS_ZOOM_RANGE = 0x7ac0


@pytest.mark.parametrize("backend", VISCA_BACKENDS)
def test_absolute_pan_tilt_move(obs_world, backend):
    device_id = obs_world.device_ids[backend]

    obs_world.run_ui_test("move_device", device_id=device_id, mode="abs", pan=1.0, tilt=-1.0)

    expected_pan = ABS_PAN_RANGE / POS_RANGE
    expected_tilt = -ABS_TILT_RANGE / POS_RANGE
    obs_world.wait_for_state(
        lambda s: abs(s["pan"] - expected_pan) < 0.005 and abs(s["tilt"] - expected_tilt) < 0.005, timeout=5)


@pytest.mark.parametrize("backend", VISCA_BACKENDS)
def test_absolute_zoom_move(obs_world, backend):
    device_id = obs_world.device_ids[backend]

    obs_world.run_ui_test("move_device", device_id=device_id, mode="abs", zoom=1.0)

    expected_zoom = ABS_ZOOM_RANGE / POS_RANGE
    obs_world.wait_for_state(lambda s: abs(s["zoom"] - expected_zoom) < 0.01, timeout=5)


@pytest.mark.parametrize("backend", VISCA_BACKENDS)
def test_relative_pan_tilt_move(obs_world, backend):
    device_id = obs_world.device_ids[backend]

    # An absolute move is a real position set, so this also gives the
    # relative move below a known, reproducible starting point --
    # otherwise the assertion would depend on whatever position an
    # earlier test left this shared device at.
    obs_world.run_ui_test("move_device", device_id=device_id, mode="abs", pan=0.0, tilt=0.0)
    obs_world.wait_for_state(lambda s: abs(s["pan"]) < 0.005 and abs(s["tilt"]) < 0.005, timeout=5)

    obs_world.run_ui_test("move_device", device_id=device_id, mode="rel", pan=1.0, tilt=-1.0)

    expected_pan = REL_PAN_RANGE / POS_RANGE
    expected_tilt = -REL_TILT_RANGE / POS_RANGE
    obs_world.wait_for_state(
        lambda s: abs(s["pan"] - expected_pan) < 0.01 and abs(s["tilt"] - expected_tilt) < 0.01, timeout=5)
