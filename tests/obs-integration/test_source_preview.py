"""The settings dialog's preview of a source, by the pixels of its own window."""

import itertools

import pytest
from conftest import SETTINGS_DIALOG
from test_virtual_ptz import FILTER_KIND
from uidriver import UiDriverError

PREVIEW = [SETTINGS_DIALOG, "sourcePreview"]
_names = itertools.count(1)


def frame(world, **request):
    """What the preview draws, once it draws something. Skips where no pixels can be read."""
    found = {}

    def read():
        try:
            found.update(world.ui.call("display_frame", path=PREVIEW, **request))
            return True
        except UiDriverError as exc:
            if exc.code == "unsupported":
                pytest.skip(str(exc))
            if exc.code != "no_frame":
                raise
            return None

    world.wait_for(read, timeout=6)
    return found


@pytest.fixture
def previewed_text(obs_world):
    """White text with nothing behind it, on Virtual PTZ, in the settings dialog."""
    source = f"preview-text-{next(_names)}"
    scene = obs_world.create_scene()
    kinds = obs_world.ws.call("GetInputKindList")["inputKinds"]
    kind = next((k for k in kinds if k.startswith(("text_ft2_source", "text_gdiplus"))), None)
    if kind is None:
        pytest.skip("no text source here")
    obs_world.ws.call("CreateInput", {
        "sceneName": scene, "inputName": source, "inputKind": kind,
        "inputSettings": {"text": "aaaaa", "font": {"face": "Sans", "size": 120}},
    })
    obs_world.ws.call("CreateSourceFilter", {
        "sourceName": source, "filterName": "Virtual PTZ", "filterKind": FILTER_KIND,
        "filterSettings": {"max_zoom": 4.0},
    })
    obs_world.wait_for(lambda: obs_world.device_uuid(source))
    obs_world.open_settings_dialog(source)
    yield source
    obs_world.ui.invoke(SETTINGS_DIALOG, method="hide")
    obs_world.ws.call("RemoveInput", {"inputName": source})


def test_transparent_text_is_not_a_white_block(obs_world, previewed_text):
    pixels = frame(obs_world)
    assert pixels["dark"] > 0.2, pixels
    assert 0.001 < pixels["white"] < 0.5, pixels


def test_it_draws_the_same_whatever_was_drawn_before_it(obs_world, previewed_text):
    clean = frame(obs_world)
    littered = frame(obs_world, litter=True)
    assert littered["white"] == pytest.approx(clean["white"], abs=0.02), (clean, littered)
    assert littered["dark"] == pytest.approx(clean["dark"], abs=0.02), (clean, littered)
