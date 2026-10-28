"""Covers the "Soft PTZ Control" filter (src/drivers/soft/ptz-soft-cam.cpp): a PTZ device
with no camera, which pans, tilts and zooms by showing a part of the source it
is on.

Each test makes an image source of four coloured quadrants (red and green over
blue and yellow) in a scene, and adds the filter to it over obs-websocket. What
they check:

- the filter makes a device, bound to the source, and moves go there: absolute
  and relative, and by a preset, with the position the device reports
- a recall says it has arrived when the eased move has ended, and a move at a
  speed, stopped, is no arrival
- pan and tilt are kept inside the frame at the zoom there is
- the picture is the part of the source the position says, which is looked at
  in a screenshot of the source (GetSourceScreenshot, which renders it with
  its filters)
- a source with no video, whose size is 0, is left alone and the device still
  answers
"""

import base64
import configparser
import itertools
import json
import struct
import time
import zlib

import pytest

FILTER_KIND = "ca.secretlab.obs-ptz.soft-ptz"
# Removing a source is not at once, so each test makes its own by a new name
_names = itertools.count(1)

WIDTH, HEIGHT = 128, 72

RED, GREEN, BLUE, YELLOW = (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0)


def write_quadrant_png(path):
    """red | green over blue | yellow, as an 8-bit RGB PNG"""
    rows = bytearray()
    for y in range(HEIGHT):
        rows.append(0)
        top = y < HEIGHT // 2
        for x in range(WIDTH):
            left = x < WIDTH // 2
            colour = (RED if left else GREEN) if top else (BLUE if left else YELLOW)
            rows.extend(colour)

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows)))
    png += chunk(b"IEND", b"")
    path.write_bytes(png)


def decode_png(data):
    """(width, height, pixel(x, y) -> (r, g, b)) of an 8-bit RGB or RGBA,
    non-interlaced PNG, which is what obs-websocket sends"""
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos = 8
    idat = b""
    width = height = channels = 0
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
            assert depth == 8 and interlace == 0 and colour in (2, 6), (depth, colour, interlace)
            channels = 3 if colour == 2 else 4
        elif kind == b"IDAT":
            idat += body
    raw = zlib.decompress(idat)
    stride = width * channels
    image = []
    previous = bytearray(stride)
    offset = 0
    for _ in range(height):
        filter_type = raw[offset]
        line = bytearray(raw[offset + 1:offset + 1 + stride])
        offset += 1 + stride
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            upleft = previous[i - channels] if i >= channels else 0
            if filter_type == 1:
                line[i] = (line[i] + left) & 0xff
            elif filter_type == 2:
                line[i] = (line[i] + up) & 0xff
            elif filter_type == 3:
                line[i] = (line[i] + (left + up) // 2) & 0xff
            elif filter_type == 4:
                p = left + up - upleft
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - upleft)
                pred = left if pa <= pb and pa <= pc else (up if pb <= pc else upleft)
                line[i] = (line[i] + pred) & 0xff
        image.append(line)
        previous = line

    def pixel(x, y):
        base = x * channels
        return tuple(image[y][base:base + 3])

    return width, height, pixel


def nearest(rgb):
    """Which of the four colours a pixel is"""
    return min((RED, GREEN, BLUE, YELLOW), key=lambda c: sum((a - b) ** 2 for a, b in zip(c, rgb)))


@pytest.fixture
def soft_cam(obs_world, tmp_path):
    image = tmp_path / "quadrants.png"
    write_quadrant_png(image)
    source = f"soft-ptz-cam-{next(_names)}"
    scene = obs_world.create_scene()
    obs_world.ws.call("CreateInput", {
        "sceneName": scene,
        "inputName": source,
        "inputKind": "image_source",
        "inputSettings": {"file": str(image)},
    })
    obs_world.ws.call("CreateSourceFilter", {
        "sourceName": source,
        "filterName": "Soft PTZ",
        "filterKind": FILTER_KIND,
        "filterSettings": {"max_zoom": 4.0, "recall_seconds": 0.2},
    })
    out = tmp_path / "device.json"
    obs_world.wait_for_device_by_name(source, out, lambda r: r["found"] and r["bound"], timeout=10)
    yield source
    obs_world.ws.call("RemoveInput", {"inputName": source})


def move(world, device, mode="abs", **axes):
    world.move_device(device, mode=mode, **axes)


def state_of(world, device, tmp_path):
    return world.device_state(device, tmp_path / "state.json")["state"]


def wait_for_position(world, device, tmp_path, pan=None, tilt=None, zoom=None, timeout=5, tolerance=0.01):
    def near(s):
        return all(want is None or (key in s and abs(s[key] - want) <= tolerance)
                   for key, want in (("pan", pan), ("tilt", tilt), ("zoom", zoom)))

    return world.wait_for_device_state(device, tmp_path / "state.json", lambda r: near(r["state"]),
                                       timeout=timeout)["state"]


def screenshot(world, source):
    reply = world.ws.call("GetSourceScreenshot", {
        "sourceName": source, "imageFormat": "png", "imageWidth": WIDTH, "imageHeight": HEIGHT,
    })
    data = reply["imageData"].split(",", 1)[1]
    return decode_png(base64.b64decode(data))


def corners(world, source):
    """What colour each corner of the source's picture, and its centre, is"""
    width, height, pixel = screenshot(world, source)
    m = 4
    return {
        "tl": nearest(pixel(m, m)),
        "tr": nearest(pixel(width - 1 - m, m)),
        "bl": nearest(pixel(m, height - 1 - m)),
        "br": nearest(pixel(width - 1 - m, height - 1 - m)),
    }


def wait_for_corners(world, source, want, timeout=8):
    seen = {}

    def matches():
        seen.clear()
        seen.update(corners(world, source))
        return seen == want

    try:
        world.wait_for(matches, timeout=timeout)
    except Exception as e:
        raise AssertionError(f"the picture never showed {want}; last seen {seen}") from e


# ------------------------------------------------------------- the device


def test_an_absolute_move_converges(obs_world, soft_cam, tmp_path):
    move(obs_world, soft_cam, zoom=1.0)
    wait_for_position(obs_world, soft_cam, tmp_path, zoom=1.0)

    move(obs_world, soft_cam, pan=0.5, tilt=-0.5)
    wait_for_position(obs_world, soft_cam, tmp_path, pan=0.5, tilt=-0.5, zoom=1.0)


def test_pan_and_tilt_are_kept_in_the_frame_at_the_zoom_there_is(obs_world, soft_cam, tmp_path):
    # Zoomed in all the way, at most 1 - 1/4 of the way to the edge
    move(obs_world, soft_cam, pan=1.0, tilt=-1.0, zoom=1.0)
    wait_for_position(obs_world, soft_cam, tmp_path, pan=0.75, tilt=-0.75, zoom=1.0)

    # and zooming out takes the centre back in with it
    move(obs_world, soft_cam, zoom=0.0)
    wait_for_position(obs_world, soft_cam, tmp_path, pan=0.0, tilt=0.0, zoom=0.0)


def test_a_relative_move_adds(obs_world, soft_cam, tmp_path):
    move(obs_world, soft_cam, pan=0.2, tilt=0.0, zoom=1.0)
    wait_for_position(obs_world, soft_cam, tmp_path, pan=0.2, tilt=0.0, zoom=1.0)

    move(obs_world, soft_cam, mode="rel", pan=0.1, tilt=0.0)
    wait_for_position(obs_world, soft_cam, tmp_path, pan=0.3, tilt=0.0)


def test_a_local_preset_round_trips(obs_world, soft_cam, tmp_path):
    # Pan before zoom is the order a recall applies them in, from the whole frame
    move(obs_world, soft_cam, pan=0.5, tilt=-0.25, zoom=0.75)
    wait_for_position(obs_world, soft_cam, tmp_path, pan=0.5, tilt=-0.25, zoom=0.75)
    preset = obs_world.create_preset(soft_cam, "somewhere", store="local")
    assert preset

    move(obs_world, soft_cam, pan=0.0, tilt=0.0, zoom=0.0)
    wait_for_position(obs_world, soft_cam, tmp_path, pan=0.0, tilt=0.0, zoom=0.0)

    obs_world.call_proc(soft_cam, "ptz_preset_recall", {"id": preset})
    wait_for_position(obs_world, soft_cam, tmp_path, pan=0.5, tilt=-0.25, zoom=0.75)


# How long the eased moves of the tests of arrival take
EASE_SECONDS = 1.0
# PTZDevice::kRecallSettleMs and kRecallBackstopMs: when a recall is said to
# have arrived by a device that can't tell, and by one that can but never heard
RECALL_SETTLE = 3.0
RECALL_BACKSTOP = 15.0


def recalled_events(world):
    return [e for e in world.preset_signals() if e["signal"] == "ptz_preset_recalled"]


def eased_preset(world, source, tmp_path):
    """A preset away from where the viewport is, which takes EASE_SECONDS to get to"""
    move(world, source, pan=0.5, tilt=-0.25, zoom=0.75)
    wait_for_position(world, source, tmp_path, pan=0.5, tilt=-0.25, zoom=0.75)
    preset = world.create_preset(source, "there", store="local")
    assert preset
    move(world, source, pan=0.0, tilt=0.0, zoom=0.0)
    wait_for_position(world, source, tmp_path, pan=0.0, tilt=0.0, zoom=0.0)
    world.ws.call("SetSourceFilterSettings", {
        "sourceName": source, "filterName": "Soft PTZ", "filterSettings": {"recall_seconds": EASE_SECONDS},
    })
    return preset


def test_it_says_it_can_tell_when_it_has_arrived(obs_world, soft_cam):
    state = obs_world.call_proc(soft_cam, "ptz_get_state", {"state": {}})["args"]["state"]
    assert "move_completion" in state["features"]


def test_soft_recall_reports_arrival_when_ease_ends(obs_world, soft_cam, tmp_path):
    preset = eased_preset(obs_world, soft_cam, tmp_path)
    obs_world.record_preset_signals(soft_cam)

    called = obs_world.call_proc(soft_cam, "ptz_preset_recall", {"id": preset})
    obs_world.wait_for(lambda: recalled_events(obs_world), timeout=3)
    elapsed = recalled_events(obs_world)[0]["ms"] - called["ms"]
    # once the ease has ended, within a report and a few frames of it, and
    # well before the 3 s a device that can't tell waits
    assert EASE_SECONDS * 1000 - 50 <= elapsed <= EASE_SECONDS * 1000 + 500, elapsed
    state = state_of(obs_world, soft_cam, tmp_path)
    assert abs(state["pan"] - 0.5) < 0.01 and abs(state["zoom"] - 0.75) < 0.01
    # and once: past when the 3 s timer would have said so too. The backstop's
    # 15 s timer finds nothing pending once a recall has been reported, as
    # every timer of the device does.
    time.sleep(max(0, RECALL_SETTLE + 0.5 - elapsed / 1000))
    assert [e["id"] for e in recalled_events(obs_world)] == [preset]


def test_a_recall_that_moves_nothing_is_reported_by_the_backstop(obs_world, soft_cam, tmp_path):
    """A preset of nothing it has (focus): the viewport never moves, so only
    PTZDevice::kRecallBackstopMs says it has arrived. The one slow test here."""
    preset = obs_world.create_preset(soft_cam, "focus only", store="local")
    assert preset
    obs_world.call_proc(soft_cam, "ptz_preset_update",
                        {"id": preset, "changes": {"values": {"focus": {"position": 0.5}}}})
    obs_world.record_preset_signals(soft_cam)

    called = obs_world.call_proc(soft_cam, "ptz_preset_recall", {"id": preset})
    time.sleep(RECALL_SETTLE + 0.5)
    assert recalled_events(obs_world) == []
    obs_world.wait_for(lambda: recalled_events(obs_world), timeout=RECALL_BACKSTOP + 3)
    elapsed = recalled_events(obs_world)[0]["ms"] - called["ms"]
    assert elapsed >= RECALL_BACKSTOP * 1000 - 100, elapsed
    assert [e["id"] for e in recalled_events(obs_world)] == [preset]


def test_soft_speed_move_stop_is_not_arrival(obs_world, soft_cam, tmp_path):
    """A recall a speed has ended, then stopped with a zero speed, which cancels
    nothing itself: it never arrived. A recall after it still says so."""
    preset = eased_preset(obs_world, soft_cam, tmp_path)
    obs_world.record_preset_signals(soft_cam)

    obs_world.call_proc(soft_cam, "ptz_preset_recall", {"id": preset})
    time.sleep(0.3)
    obs_world.call_proc(soft_cam, "ptz_move", {"pan": 0.5, "tilt": 0.0})
    time.sleep(0.3)
    obs_world.call_proc(soft_cam, "ptz_move", {"pan": 0.0, "tilt": 0.0})
    time.sleep(EASE_SECONDS + 3.5)
    assert recalled_events(obs_world) == []

    obs_world.call_proc(soft_cam, "ptz_preset_recall", {"id": preset})
    obs_world.wait_for(lambda: recalled_events(obs_world), timeout=3)
    assert [e["id"] for e in recalled_events(obs_world)] == [preset]


def test_a_source_with_no_video_still_answers(obs_world, tmp_path):
    empty = f"soft-ptz-empty-{next(_names)}"
    scene = obs_world.create_scene()
    obs_world.ws.call("CreateInput", {
        "sceneName": scene, "inputName": empty, "inputKind": "image_source",
        "inputSettings": {"file": ""},
    })
    try:
        obs_world.ws.call("CreateSourceFilter", {
            "sourceName": empty, "filterName": "Soft PTZ", "filterKind": FILTER_KIND,
            "filterSettings": {"recall_seconds": 0.2},
        })
        out = tmp_path / "device.json"
        obs_world.wait_for_device_by_name(empty, out, lambda r: r["found"] and r["bound"], timeout=10)

        move(obs_world, empty, pan=0.5, tilt=0.0, zoom=1.0)
        wait_for_position(obs_world, empty, tmp_path, pan=0.5, zoom=1.0)
        # and OBS is still there to be asked
        assert obs_world.ws.call("GetVersion", {})["obsVersion"]
    finally:
        obs_world.ws.call("RemoveInput", {"inputName": empty})


# ------------------------------------------------------------ the picture


def test_the_whole_frame_is_shown_at_zoom_0(obs_world, soft_cam):
    wait_for_corners(obs_world, soft_cam, {"tl": RED, "tr": GREEN, "bl": BLUE, "br": YELLOW})


@pytest.mark.parametrize("pan,tilt,colour", [
    (0.75, 0.75, GREEN),    # right and up: the top right quadrant
    (-0.75, 0.75, RED),
    (-0.75, -0.75, BLUE),
    (0.75, -0.75, YELLOW),
])
def test_the_picture_is_the_part_the_position_says(obs_world, soft_cam, tmp_path, pan, tilt, colour):
    move(obs_world, soft_cam, pan=pan, tilt=tilt, zoom=1.0)
    wait_for_position(obs_world, soft_cam, tmp_path, pan=pan, tilt=tilt, zoom=1.0)
    wait_for_corners(obs_world, soft_cam, {"tl": colour, "tr": colour, "bl": colour, "br": colour})


def test_zoomed_in_on_the_middle_shows_where_the_quadrants_meet(obs_world, soft_cam, tmp_path):
    move(obs_world, soft_cam, pan=0.0, tilt=0.0, zoom=1.0)
    wait_for_position(obs_world, soft_cam, tmp_path, pan=0.0, tilt=0.0, zoom=1.0)
    wait_for_corners(obs_world, soft_cam, {"tl": RED, "tr": GREEN, "bl": BLUE, "br": YELLOW})


def test_obs_quits_cleanly_after_a_zoomed_picture(obs_world, soft_cam, tmp_path):
    """A zoomed picture is drawn to a texture of the filter's own, which is
    freed as the filter is destroyed, with the graphics there still. The device
    itself goes later: for a source something keeps past OBS clearing its
    scenes, as an NDI output does, after OBS has shut its graphics down, and
    freeing the texture then crashed OBS (#428)"""
    obs_world.need_ui_driver()
    obs_world.ui.hold_source(soft_cam)
    move(obs_world, soft_cam, pan=0.75, tilt=0.75, zoom=1.0)
    wait_for_position(obs_world, soft_cam, tmp_path, pan=0.75, tilt=0.75, zoom=1.0)
    wait_for_corners(obs_world, soft_cam, {"tl": GREEN, "tr": GREEN, "bl": GREEN, "br": GREEN})

    obs_world.restart_obs()
    assert obs_world.exit_code == 0, f"OBS exited with {obs_world.exit_code}"


# ------------------------------------------------------------ Studio Mode


def set_edit_properties_mode(world, enabled):
    """Studio Mode's "Duplicate Sources" (OBS's editPropertiesMode), which OBS
    reads from user.ini as it starts: restarts OBS with it so"""
    user_ini = world.config_dir.parent.parent / "user.ini"

    def edit():
        config = configparser.ConfigParser(strict=False, interpolation=None)
        config.optionxform = str
        config.read(user_ini, encoding="utf-8-sig")
        if not config.has_section("BasicWindow"):
            config.add_section("BasicWindow")
        config.set("BasicWindow", "EditPropertiesMode", "true" if enabled else "false")
        with open(user_ini, "w", encoding="utf-8") as f:
            config.write(f, space_around_delimiters=False)

    world.restart_obs(between=edit)


@pytest.fixture
def duplicating_studio_mode(obs_world):
    """Studio Mode with "Duplicate Sources", in which each scene sent to program
    is a private copy of it, with copies of its sources and their filters"""
    set_edit_properties_mode(obs_world, True)
    obs_world.ws.call("SetStudioModeEnabled", {"studioModeEnabled": True})
    obs_world.wait_for(lambda: obs_world.ws.call("GetStudioModeEnabled")["studioModeEnabled"], timeout=5)
    yield
    obs_world.ws.call("SetStudioModeEnabled", {"studioModeEnabled": False})
    set_edit_properties_mode(obs_world, False)


def test_studio_mode_copies_of_its_source_dont_crash_obs(obs_world, duplicating_studio_mode, tmp_path):
    """OBS puts the source's filter on each copy of the source it sends to
    program as well, as the filter may not be duplicated, and freeing the copy
    takes the filter's parent and target away from under the source still
    drawing it. Drawing it then crashed OBS (#428): now it draws nothing. The
    source shows nothing from then on, until the filter is added again."""
    image = tmp_path / "quadrants.png"
    write_quadrant_png(image)
    source = f"soft-ptz-cam-{next(_names)}"
    cam_scene = obs_world.create_scene()
    other_scene = obs_world.create_scene()
    obs_world.ws.call("CreateInput", {
        "sceneName": cam_scene, "inputName": source, "inputKind": "image_source",
        "inputSettings": {"file": str(image)},
    })
    obs_world.ws.call("CreateSourceFilter", {
        "sourceName": source, "filterName": "Soft PTZ", "filterKind": FILTER_KIND,
        "filterSettings": {"max_zoom": 4.0, "recall_seconds": 0.2},
    })
    obs_world.wait_for_device_by_name(source, tmp_path / "device.json", lambda r: r["found"] and r["bound"],
                                      timeout=10)
    move(obs_world, source, pan=0.75, tilt=0.75, zoom=1.0)
    wait_for_position(obs_world, source, tmp_path, pan=0.75, tilt=0.75, zoom=1.0)

    # Each transition sends a new copy of the scene to program and frees the last
    for scene in (cam_scene, other_scene) * 3:
        obs_world.ws.call("SetCurrentPreviewScene", {"sceneName": scene})
        obs_world.ws.call("TriggerStudioModeTransition")
        time.sleep(1)
        # OBS is still there, drawing the source
        screenshot(obs_world, source)
    obs_world.ws.call("RemoveInput", {"inputName": source})


# ----------------------------------------------------- preset thumbnails


def preset_thumbnails(world, device):
    presets = world.call_proc(device, "ptz_preset_get_list", returns="data")["return"].get("presets", {})
    return {preset_id: preset.get("thumbnail", "") for preset_id, preset in presets.items()}


def thumbnail_name(path):
    """The file name of a thumbnail the preset list gives the path of, on any OS"""
    return path.replace("\\", "/").rsplit("/", 1)[-1]


def thumbnail_image(world, device, preset_id):
    """The preset's thumbnail as the device reads it from its file: b"" if it has none,
    or its file is gone. Not the file itself, whose path is OBS's, relative to where OBS runs."""
    answer = world.ws.vendor("obs-ptz", "ptz_preset_get_thumbnail",
                             {"device": world.device_uuid(device), "id": preset_id})
    return base64.b64decode(answer["data"])


def test_a_thumbnail_is_named_for_its_filter(obs_world, soft_cam):
    """A preset's thumbnail file is named "<filter uuid>_<random uuid>", so a
    copy of the filter, with a uuid of its own, can tell the thumbnails it was
    given from its own"""
    preset = obs_world.create_preset(soft_cam, "pictured", store="local")
    obs_world.wait_for(lambda: thumbnail_image(obs_world, soft_cam, preset), timeout=10)
    name = thumbnail_name(preset_thumbnails(obs_world, soft_cam)[preset])
    assert name.startswith(obs_world.device_uuid(soft_cam) + "_"), name


# ------------------------------------------------- beyond the source's bounds


def scene_pixel(world, scene, x, y):
    """The colour of a pixel of the whole scene, as it is drawn with its
    items: what is outside every item is its background"""
    reply = world.ws.call("GetSourceScreenshot", {"sourceName": scene, "imageFormat": "png"})
    _, _, pixel = decode_png(base64.b64decode(reply["imageData"].split(",", 1)[1]))
    return pixel(x, y)


def test_a_zoomed_picture_stays_inside_its_source_in_the_scene(obs_world, tmp_path):
    """A source that is a small part of the scene, as a picture-in-picture
    camera is, mustn't have what is zoomed in on spill over the rest"""
    image = tmp_path / "quadrants.png"
    write_quadrant_png(image)
    name = f"soft-ptz-pip-{next(_names)}"
    scene = obs_world.create_scene()
    obs_world.ws.call("CreateInput", {
        "sceneName": scene, "inputName": name, "inputKind": "image_source",
        "inputSettings": {"file": str(image)},
    })
    try:
        obs_world.ws.call("CreateSourceFilter", {
            "sourceName": name, "filterName": "Soft PTZ", "filterKind": FILTER_KIND,
            "filterSettings": {"max_zoom": 4.0, "recall_seconds": 0.2},
        })
        out = tmp_path / "device.json"
        obs_world.wait_for_device_by_name(name, out, lambda r: r["found"] and r["bound"], timeout=10)
        # The top right quadrant, at 4x: drawn at 4 times the size of the item, which
        # reaches far below it (and the item is at the scene's top left, 128x72)
        move(obs_world, name, pan=0.75, tilt=0.75, zoom=1.0)
        wait_for_position(obs_world, name, tmp_path, pan=0.75, tilt=0.75, zoom=1.0)

        def inside():
            return nearest(scene_pixel(obs_world, scene, 50, 30)) == GREEN

        obs_world.wait_for(inside, timeout=8)
        assert scene_pixel(obs_world, scene, 50, 150) == (0, 0, 0), "the picture spilled below its source"
        assert scene_pixel(obs_world, scene, 200, 30) == (0, 0, 0), "the picture spilled beside its source"
    finally:
        obs_world.ws.call("RemoveInput", {"inputName": name})


# ------------------------------------------------- the dock, and persistence


def run_add_device(world, out, source, **choice):
    return world.add_device(source, **choice)


def filter_kinds(world, source):
    reply = world.ws.call("GetSourceFilterList", {"sourceName": source})
    return [f["filterKind"] for f in reply["filters"]]


@pytest.fixture
def plain_source(obs_world):
    name = f"soft-ptz-plain-{next(_names)}"
    scene = obs_world.create_scene()
    obs_world.ws.call("CreateInput", {
        "sceneName": scene, "inputName": name, "inputKind": "image_source", "inputSettings": {"file": ""},
    })
    yield name
    obs_world.ws.call("RemoveInput", {"inputName": name})


def test_the_add_dialog_offers_it(obs_world, plain_source, tmp_path):
    run_add_device(obs_world, tmp_path / "add.json", plain_source, type="soft-ptz")
    obs_world.wait_for(lambda: filter_kinds(obs_world, plain_source) == [FILTER_KIND])
    obs_world.wait_for_device_by_name(plain_source, tmp_path / "device.json", lambda r: r["found"] and r["bound"])


def test_a_removed_one_is_offered_back_with_its_presets(obs_world, plain_source, tmp_path):
    obs_world.ws.call("CreateSourceFilter", {
        "sourceName": plain_source, "filterName": "Soft PTZ", "filterKind": FILTER_KIND,
        "filterSettings": {"recall_seconds": 0.2},
    })
    obs_world.wait_for_device_by_name(plain_source, tmp_path / "device.json", lambda r: r["found"] and r["bound"])
    move(obs_world, plain_source, pan=0.0, tilt=0.0, zoom=0.5)
    wait_for_position(obs_world, plain_source, tmp_path, zoom=0.5)
    preset = obs_world.create_preset(plain_source, "kept", store="local")
    assert preset

    obs_world.remove_device(plain_source)
    obs_world.wait_for(lambda: filter_kinds(obs_world, plain_source) == [])

    run_add_device(obs_world, tmp_path / "add.json", plain_source, restore=plain_source)
    obs_world.wait_for(lambda: filter_kinds(obs_world, plain_source) == [FILTER_KIND])
    device = obs_world.wait_for_device_by_name(plain_source, tmp_path / "device.json",
                                               lambda r: r["found"] and r["bound"])
    saved = obs_world.device_settings(device["uuid"], tmp_path / "settings.json")["saved"]
    assert [p["id"] for p in saved.get("presets", [])] == [preset]


def test_where_it_is_looking_is_kept_with_its_settings(obs_world, plain_source, tmp_path):
    obs_world.ws.call("CreateSourceFilter", {
        "sourceName": plain_source, "filterName": "Soft PTZ", "filterKind": FILTER_KIND,
        "filterSettings": {"recall_seconds": 0.2, "max_zoom": 4.0},
    })
    obs_world.wait_for_device_by_name(plain_source, tmp_path / "device.json", lambda r: r["found"] and r["bound"])
    move(obs_world, plain_source, pan=0.5, tilt=-0.25, zoom=0.75)
    wait_for_position(obs_world, plain_source, tmp_path, pan=0.5, tilt=-0.25, zoom=0.75)

    def kept():
        settings = obs_world.ws.call("GetSourceFilter", {"sourceName": plain_source, "filterName": "Soft PTZ"})
        pose = settings["filterSettings"].get("viewport") or {}
        return abs(pose.get("pan", 9) - 0.5) < 0.01 and abs(pose.get("tilt", 9) + 0.25) < 0.01 \
            and abs(pose.get("zoom", 9) - 0.75) < 0.01

    obs_world.wait_for(kept, timeout=8)


def test_it_starts_where_it_was_left(obs_world, plain_source, tmp_path):
    """What a restart, or a scene collection that is loaded again, does: makes the
    filter from its saved settings"""
    obs_world.ws.call("CreateSourceFilter", {
        "sourceName": plain_source, "filterName": "Soft PTZ", "filterKind": FILTER_KIND,
        "filterSettings": {"recall_seconds": 0.2, "max_zoom": 4.0,
                           "viewport": {"pan": 0.5, "tilt": -0.25, "zoom": 0.75}},
    })
    obs_world.wait_for_device_by_name(plain_source, tmp_path / "device.json", lambda r: r["found"] and r["bound"])
    wait_for_position(obs_world, plain_source, tmp_path, pan=0.5, tilt=-0.25, zoom=0.75)
