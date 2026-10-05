"""ptzsim's Blender renderer: run by Blender, not Python, as

    blender [room.blend] --background --python client.py -- --url http://127.0.0.1:8080 --token T

It follows the simulated camera's state from ptzsim's /events, points a
Blender camera as that says (pan and tilt turn it, zoom sets its field of
view, focus its focus distance, so it has real depth of field), renders
frames, and posts them to ptzsim's /frame for the web view to show. ptzsim's
--blender starts this itself.

With no .blend, or "builtin" for --room, it builds a small test room.
"""

import argparse
import json
import math
import os
import sys
import tempfile
import threading
import time
import urllib.request

import bpy
from mathutils import Euler, Matrix

# The same mapping as the web view's (web/index.html)
PAN_RANGE, TILT_RANGE = 170.0, 45.0       # degrees at |pan| = 1, |tilt| = 1
FOV_WIDE, FOV_TELE = 70.0, 4.0            # horizontal field of view at zoom 0 and 1
FOCUS_NEAR, FOCUS_FAR = 0.5, 100.0        # metres at focus 0 and 1


def parse_args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ap = argparse.ArgumentParser(prog="client.py")
    ap.add_argument("--url", required=True, help="ptzsim's web view, e.g. http://127.0.0.1:8080")
    ap.add_argument("--token", default="", help="what ptzsim wants with a frame")
    ap.add_argument("--builtin", action="store_true", help="build the test room instead of using the scene")
    ap.add_argument("--camera", default=None, help="the camera object to point (default: the scene's)")
    ap.add_argument("--heading", type=float, default=0.0, help="degrees to turn pan 0 to the right")
    ap.add_argument("--engine", choices=("eevee", "workbench"), default="eevee")
    ap.add_argument("--samples", type=int, default=4, help="EEVEE samples per frame")
    ap.add_argument("--size", default="1280x720")
    ap.add_argument("--fps", type=float, default=15.0, help="the most frames a second to render")
    ap.add_argument("--fstop", type=float, default=2.8, help="aperture, for the depth of field")
    return ap.parse_args(argv)


# ---- A small room to test with ---------------------------------------------

def _material(name, colour, emission=0.0):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    mat.diffuse_color = (*colour, 1.0)      # what Workbench draws with
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (*colour, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.6
    if emission:
        bsdf.inputs["Emission Color"].default_value = (*colour, 1.0)
        bsdf.inputs["Emission Strength"].default_value = emission
    return mat


def build_test_room():
    """A room 12 m by 12 m by 3.5 m with lettered pillars every 45 degrees round
    a camera in the middle, so pan, tilt, zoom and focus can all be seen"""
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene

    def box(name, location, size, material):
        bpy.ops.mesh.primitive_cube_add(location=location)
        ob = bpy.context.object
        ob.name = name
        ob.scale = [s / 2 for s in size]
        ob.data.materials.append(material)
        return ob

    floor = _material("floor", (0.35, 0.33, 0.30))
    wall = _material("wall", (0.75, 0.72, 0.66))
    box("floor", (0, 0, -0.05), (12, 12, 0.1), floor)
    box("ceiling", (0, 0, 3.55), (12, 12, 0.1), wall)
    for name, loc, size in (("wall-n", (0, 6.05, 1.75), (12.2, 0.1, 3.5)),
                            ("wall-s", (0, -6.05, 1.75), (12.2, 0.1, 3.5)),
                            ("wall-e", (6.05, 0, 1.75), (0.1, 12.2, 3.5)),
                            ("wall-w", (-6.05, 0, 1.75), (0.1, 12.2, 3.5))):
        box(name, loc, size, wall)

    colours = [(0.9, 0.1, 0.1), (0.95, 0.5, 0.05), (0.9, 0.85, 0.1), (0.2, 0.7, 0.2),
               (0.1, 0.6, 0.7), (0.15, 0.25, 0.85), (0.55, 0.15, 0.7), (0.85, 0.2, 0.55)]
    # Letters A.. every 45 degrees clockwise from straight ahead (+Y), nearer and farther alternately
    for i, letter in enumerate("ABCDEFGH"):
        angle = math.radians(i * 45)
        distance = 3.0 if i % 2 == 0 else 4.6
        x, y = distance * math.sin(angle), distance * math.cos(angle)
        pillar = box(f"pillar-{letter}", (x, y, 0.9), (0.6, 0.6, 1.8), _material(f"pillar-{letter}", colours[i]))
        curve = bpy.data.curves.new(f"text-{letter}", "FONT")
        curve.body, curve.size, curve.align_x = letter, 0.5, "CENTER"
        text = bpy.data.objects.new(f"text-{letter}", curve)
        scene.collection.objects.link(text)
        text.location = (x - 0.32 * math.sin(angle) , y - 0.32 * math.cos(angle), 1.5)
        text.rotation_euler = Euler((math.radians(90), 0, -angle), "XYZ")   # facing the middle
        text.data.materials.append(_material(f"ink-{letter}", (1, 1, 1), 1.0))
    # Every 15 degrees on the far wall's floor line: a ruler
    for i in range(-6, 7):
        box(f"tick-{i}", (i * 0.5, 5.95, 0.3), (0.04 if i % 2 else 0.08, 0.02, 0.6 if i % 2 else 1.0),
            _material(f"tick-{i}", (0.05, 0.05, 0.05)))

    light_data = bpy.data.lights.new("ceiling", "AREA")
    light_data.energy, light_data.size = 900, 6
    light = bpy.data.objects.new("ceiling", light_data)
    light.location = (0, 0, 3.4)
    scene.collection.objects.link(light)
    scene.world = bpy.data.worlds.new("world")
    scene.world.use_nodes = True
    scene.world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.3

    cam_data = bpy.data.cameras.new("PTZ")
    cam = bpy.data.objects.new("PTZ", cam_data)
    cam.location = (0, 0, 1.6)
    cam.rotation_euler = Euler((math.radians(90), 0, 0), "XYZ")         # looking along +Y
    scene.collection.objects.link(cam)
    scene.camera = cam
    return cam


# ---- State from ptzsim ------------------------------------------------------

class Follower(threading.Thread):
    """The latest state ptzsim has sent, over its Server-Sent Events stream"""

    def __init__(self, url):
        super().__init__(daemon=True)
        self.url = url.rstrip("/") + "/events"
        self.lock = threading.Lock()
        self.state = None

    def run(self):
        while True:
            try:
                with urllib.request.urlopen(self.url, timeout=10) as stream:
                    for line in stream:
                        if line.startswith(b"data:"):
                            state = json.loads(line[5:])
                            with self.lock:
                                self.state = state
            except Exception as e:
                print(f"[blender] events: {e}; trying again", flush=True)
                time.sleep(1)

    def latest(self):
        with self.lock:
            return self.state


def point_camera(cam, rest, state, heading, fstop):
    """Pan turns about the vertical axis, tilt about the camera's own, as a PTZ head does"""
    yaw = math.radians(state["pan"] * PAN_RANGE + heading)
    pitch = math.radians(state["tilt"] * TILT_RANGE)
    cam.matrix_world = (Matrix.Rotation(-yaw, 4, "Z") @ rest.to_3x3().to_4x4()
                        @ Matrix.Rotation(pitch, 4, "X"))
    cam.matrix_world.translation = rest.translation
    hfov = math.radians(FOV_WIDE * (FOV_TELE / FOV_WIDE) ** state["zoom"])
    data = cam.data
    data.sensor_fit, data.sensor_width = "HORIZONTAL", 36.0
    data.lens = (data.sensor_width / 2) / math.tan(hfov / 2)
    data.dof.use_dof = True
    data.dof.aperture_fstop = fstop
    data.dof.focus_distance = FOCUS_NEAR * (FOCUS_FAR / FOCUS_NEAR) ** state["focus"]


def main():
    args = parse_args()
    scene = bpy.context.scene
    if args.builtin or not any(ob.type == "CAMERA" for ob in bpy.data.objects):
        build_test_room()
        scene = bpy.context.scene
    cam = bpy.data.objects.get(args.camera) if args.camera else scene.camera
    if cam is None or cam.type != "CAMERA":
        cam = next((ob for ob in bpy.data.objects if ob.type == "CAMERA"), None)
    if cam is None:
        sys.exit("[blender] no camera in the scene")
    scene.camera = cam
    bpy.context.view_layer.update()      # a new object's matrix_world isn't set until then
    rest = cam.matrix_world.copy()
    print(f"[blender] camera '{cam.name}' in {bpy.data.filepath or 'the built-in room'}", flush=True)

    width, height = (int(v) for v in args.size.lower().split("x"))
    render = scene.render
    render.engine = "BLENDER_EEVEE" if args.engine == "eevee" else "BLENDER_WORKBENCH"
    render.resolution_x, render.resolution_y, render.resolution_percentage = width, height, 100
    render.image_settings.file_format = "JPEG"
    render.image_settings.quality = 85
    scene.view_settings.view_transform = "Standard"
    if args.engine == "eevee":
        scene.eevee.taa_render_samples = args.samples
    else:
        scene.display.shading.light = "STUDIO"
        scene.display.shading.color_type = "TEXTURE"

    follower = Follower(args.url)
    follower.start()
    path = os.path.join(tempfile.mkdtemp(prefix="ptzsim-blender-"), "frame.jpg")
    frame_url = args.url.rstrip("/") + "/frame"
    last_key, jpeg, last_sent = None, b"", 0.0
    interval = 1.0 / args.fps
    while True:
        started = time.time()
        state = follower.latest()
        if state is None:
            time.sleep(0.1)
            continue
        key = tuple(round(state[k], 5) for k in ("pan", "tilt", "zoom", "focus")) + (state["power"],)
        if key != last_key or not jpeg:
            if state["power"]:
                point_camera(cam, rest, state, args.heading, args.fstop)
                bpy.ops.render.render(write_still=False)
                bpy.data.images["Render Result"].save_render(path)
                with open(path, "rb") as f:
                    jpeg = f.read()
            last_key = key
        elif time.time() - last_sent < 1.0:
            time.sleep(0.05)       # nothing moved: just keep the stream alive
            continue
        try:
            request = urllib.request.Request(frame_url, data=jpeg, method="POST",
                                             headers={"Content-Type": "image/jpeg", "X-Token": args.token})
            urllib.request.urlopen(request, timeout=5).read()
            last_sent = time.time()
        except Exception as e:
            print(f"[blender] posting a frame: {e}", flush=True)
            time.sleep(1)
        time.sleep(max(0.0, interval - (time.time() - started)))


main()
