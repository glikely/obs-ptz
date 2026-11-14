"""UI integration tests for the Joystick Control section in PTZ Settings -> General.

Uses the UI driver (tests/ui-driver/) to open PTZ Settings, switch to the
General tab, and exercise joystickGroupBox, joystickNamesListView, and the
axis/button mapping widgets (skipping cleanly if ENABLE_JOYSTICK=OFF).
"""

import pytest

from conftest import SETTINGS_DIALOG


def _open_general_tab(obs_world):
    obs_world.need_ui_driver()
    obs_world._open_settings_window()
    pages = [tab["page"] for tab in obs_world.ui.get1(SETTINGS_DIALOG, "tabWidget", name="tabs")]
    obs_world.ui.set(SETTINGS_DIALOG, "tabWidget", currentIndex=pages.index("generalTab"))
    if not obs_world.ui.get1(SETTINGS_DIALOG, "joystickGroupBox", name="visible"):
        pytest.skip("built without ENABLE_JOYSTICK=ON")


def test_joystick_groupbox_toggles_device_list(obs_world):
    _open_general_tab(obs_world)
    ui = obs_world.ui

    assert ui.get1(SETTINGS_DIALOG, "joystickGroupBox", name="checkable") is True
    ui.set(SETTINGS_DIALOG, "joystickGroupBox", checked=True)
    assert ui.get1(SETTINGS_DIALOG, "joystickNamesListView", name="enabled") is True

    ui.set(SETTINGS_DIALOG, "joystickGroupBox", checked=False)
    assert ui.get1(SETTINGS_DIALOG, "joystickNamesListView", name="enabled") is False


def test_joystick_selection_populates_axes_without_crashing(obs_world):
    _open_general_tab(obs_world)
    ui = obs_world.ui

    ui.set(SETTINGS_DIALOG, "joystickGroupBox", checked=True)
    rows, current, _ = ui.rows(SETTINGS_DIALOG, "joystickNamesListView")
    if not rows:
        pytest.skip("no joystick devices present in this environment")
    assert current >= 0, "expected an initial joystick device to be selected"

    for row in rows:
        name = row["display"]
        ui.select(SETTINGS_DIALOG, "joystickNamesListView", value=name)
        combos = ui.find(SETTINGS_DIALOG, "joystickGroupBox", {"class": "QComboBox"}, all=True)
        assert len(combos) > 0, f"expected axis combo boxes after selecting {name!r}"
