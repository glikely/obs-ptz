#!/usr/bin/env python3
"""Shows OBS itself not releasing a removed input, and so not letting its
name be used again. Nothing here has anything to do with obs-ptz: it only uses
libobs's own behaviour, seen through obs-websocket, and it gives the same
answers with obs-ptz not loaded.

When an input is removed (obs-websocket's RemoveInput, i.e. obs_source_remove())
libobs only marks it as removed. The scene it is in keeps its scene item, and
so a reference to the input, until it next draws the scene and prunes it
(update_transforms_and_prune_sources(), from scene_video_render() in
libobs/obs-scene.c). The input is destroyed, and its name becomes available
again, only after that, and until then CreateInput fails with "A source already
exists by that input name" (ResourceAlreadyExists, 601). A scene that isn't
being drawn by anything doesn't prune, so an input removed from one is
kept for as long as OBS runs.

Each scenario creates an input, removes it, and then tries to create an input
of the same name until it succeeds or gives up, and passes if it succeeded. What
they show, on Ubuntu (OBS 30.0.2) and macOS (OBS 32.2.x):

  - removing the scene item first, or the whole scene, always releases the name;
  - so does drawing the scene once afterwards (obs-websocket's
    GetSourceScreenshot of it), wherever the scene is;
  - otherwise it depends on the scene being drawn soon enough. One that isn't
    the program scene (or shown in a preview) isn't drawn at all, so never on
    either. The program scene is drawn all the time on Ubuntu, but on macOS,
    with OBS launched by the test suite, it is intermittently not, even for
    the several seconds these wait: the same scenario has both passed and
    failed there;
  - it doesn't depend on the kind of source (a plain colour source, an async
    media source, an image);
  - a removed scene, as a control, is always released straight away.

They're all marked as expected failures, since which of them fail depends on the
setup; -rxX lists which did.

As a pytest module it runs against the suite's OBS (see conftest.py's
obs_world, and scripts/run-macos-integration-tests.sh for macOS). To run it
against any OBS with its websocket server enabled, without the suite:

    python3 test_obs_removed_source.py [--url ws://127.0.0.1:4455] [--password PW]
"""

import argparse
import sys
import time
import uuid

import pytest

from obsws import Client, ObsWebSocketError

# obs-websocket RequestStatus::ResourceAlreadyExists
RESOURCE_ALREADY_EXISTS = 601

TIMEOUT = 8
INTERVAL = 0.25

INPUT_SETTINGS = {
    "color_source_v3": {},
    "ffmpeg_source": {"is_local_file": True, "local_file": ""},
    "image_source": {"file": ""},
}


def wait_until_created(ws, create, timeout=TIMEOUT):
    """Calls create() until it stops failing with "already exists". Returns
    how many seconds that took, or None if it never did."""
    start = time.time()
    while True:
        try:
            create()
            return time.time() - start
        except ObsWebSocketError as e:
            if e.status.get("code") != RESOURCE_ALREADY_EXISTS:
                raise
        if time.time() - start > timeout:
            return None
        time.sleep(INTERVAL)


def removed_input_name_reuse(ws, kind="color_source_v3", in_program=False, remove_item_first=False,
                             remove_scene_first=False, render_scene=False):
    """Returns the seconds until the name of a removed input could be used
    again, or None if it couldn't within TIMEOUT."""
    tag = uuid.uuid4().hex[:8]
    scene, other, name = f"rs-scene-{tag}", f"rs-other-{tag}", f"rs-input-{tag}"
    made = []
    try:
        for s in (scene, other):
            ws.call("CreateScene", {"sceneName": s})
            made.append(s)

        def create_input(scene_name=other):
            ws.call("CreateInput", {"sceneName": scene_name, "inputName": name, "inputKind": kind,
                                    "inputSettings": INPUT_SETTINGS[kind]})

        item = ws.call("CreateInput", {"sceneName": scene, "inputName": name, "inputKind": kind,
                                       "inputSettings": INPUT_SETTINGS[kind]})["sceneItemId"]
        ws.call("SetCurrentProgramScene", {"sceneName": scene if in_program else other})

        if remove_item_first:
            ws.call("RemoveSceneItem", {"sceneName": scene, "sceneItemId": item})
        if remove_scene_first:
            ws.call("RemoveScene", {"sceneName": scene})
            made.remove(scene)
        ws.call("RemoveInput", {"inputName": name})
        if render_scene:
            # Draws the scene once, on request, whether or not OBS is
            # drawing it anyway
            ws.call("GetSourceScreenshot", {"sourceName": scene, "imageFormat": "png",
                                            "imageWidth": 64, "imageHeight": 64})

        return wait_until_created(ws, create_input)
    finally:
        for input_name in (name,):
            try:
                ws.call("RemoveInput", {"inputName": input_name})
            except ObsWebSocketError:
                pass
        for s in made:
            try:
                ws.call("RemoveScene", {"sceneName": s})
            except ObsWebSocketError:
                pass


def removed_scene_name_reuse(ws):
    """The same for a scene: remove it and create one of the same name."""
    name = f"rs-scene-{uuid.uuid4().hex[:8]}"
    ws.call("CreateScene", {"sceneName": name})
    ws.call("RemoveScene", {"sceneName": name})
    released = wait_until_created(ws, lambda: ws.call("CreateScene", {"sceneName": name}))
    try:
        ws.call("RemoveScene", {"sceneName": name})
    except ObsWebSocketError:
        pass
    return released


SCENARIOS = {
    "color source, not in the program scene": dict(kind="color_source_v3"),
    "color source, in the program scene": dict(kind="color_source_v3", in_program=True),
    "color source, scene item removed first": dict(kind="color_source_v3", in_program=True,
                                                    remove_item_first=True),
    "color source, scene removed first": dict(kind="color_source_v3", remove_scene_first=True),
    "color source, scene drawn once after": dict(kind="color_source_v3", render_scene=True),
    "color source, program scene drawn once after": dict(kind="color_source_v3", in_program=True,
                                                          render_scene=True),
    "media source, not in the program scene": dict(kind="ffmpeg_source"),
    "media source, in the program scene": dict(kind="ffmpeg_source", in_program=True),
    "image source, not in the program scene": dict(kind="image_source"),
}

XFAIL = pytest.mark.xfail(strict=False, reason="OBS keeps a removed input, and so its name, until its scene is next "
                                               "drawn, which may be never")


@pytest.fixture
def ws(obs_world):
    return obs_world.ws


@XFAIL
@pytest.mark.parametrize("scenario", SCENARIOS, ids=list(SCENARIOS))
def test_removed_input_name_is_released(ws, scenario):
    seconds = removed_input_name_reuse(ws, **SCENARIOS[scenario])
    assert seconds is not None, f"the name of the removed input still wasn't available after {TIMEOUT}s"


@XFAIL
def test_removed_scene_name_is_released(ws):
    seconds = removed_scene_name_reuse(ws)
    assert seconds is not None, f"the name of the removed scene still wasn't available after {TIMEOUT}s"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--url", default="ws://127.0.0.1:4455")
    parser.add_argument("--password")
    args = parser.parse_args()

    ws = Client(args.url, password=args.password)
    version = ws.call("GetVersion")
    print(f"OBS {version.get('obsVersion')}, obs-websocket {version.get('obsWebSocketVersion')}, "
          f"{version.get('platform')}\n")
    ws.call("SetCurrentSceneTransition", {"transitionName": "Cut"})

    unreleased = 0
    rows = [(name, lambda kw=kw: removed_input_name_reuse(ws, **kw)) for name, kw in SCENARIOS.items()]
    rows.append(("scene", lambda: removed_scene_name_reuse(ws)))
    for name, run in rows:
        seconds = run()
        unreleased += seconds is None
        print(f"{name:45} " + (f"name free again after {seconds:.2f}s" if seconds is not None
                                else f"name STILL TAKEN after {TIMEOUT}s"))
    ws.close()
    return 1 if unreleased else 0


if __name__ == "__main__":
    sys.exit(main())
