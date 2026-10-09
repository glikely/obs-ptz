"""Covers the "Soft PTZ Control" filter (src/ptz-soft-cam.cpp): a PTZ device
with no camera, which pans, tilts and zooms by showing a part of the source it
is on.

Each test makes an image source of four coloured quadrants (red and green over
blue and yellow) in a scene, and adds the filter to it over obs-websocket. What
they check:

- the filter makes a device, bound to the source, and moves go there: absolute
  and relative, and by a preset, with the position the device reports
- pan and tilt are kept inside the frame at the zoom there is
- the picture is the part of the source the position says, which is looked at
  in a screenshot of the source (GetSourceScreenshot, which renders it with
  its filters)
- a source with no video, whose size is 0, is left alone and the device still
  answers
"""

import base64
import itertools
import json
import struct
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
    world.run_ui_test("move_device", device=device, mode=mode, **axes)


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
    if out.exists():
        out.unlink()
    world.run_ui_test("add_device", filename=str(out), source=source, **choice)
    world.wait_for(out.exists)
    return json.loads(out.read_text())


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

    obs_world.run_ui_test("remove_device", name=plain_source)
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
