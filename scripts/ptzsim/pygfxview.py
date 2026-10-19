"""Draws a 3D scene from the simulated camera with pygfx (wgpu: Metal, Vulkan or
D3D12, no window) and hands the frames to whatever wants them: the web view's
MJPEG stream (blenderview.FrameStore) and/or an NDI sender (ndi.py).

--ndi takes what --scene does (a glTF scene) or what --backdrop does (an
equirectangular panorama, drawn on the inside of a sphere round the camera, at
one distance, so focus blurs all of it at once). Pan, tilt and zoom turn and zoom
the camera as in the web view (the constants are the same as web/index.html's);
focus is a depth-of-field pass, and the VISCA picture settings (white balance,
colour gain, brightness, picture effect) a colour pass, so what a client changes
shows in the picture. It needs the packages in REQUIREMENTS.
"""

import io
import math
import threading
import time

PANORAMA_DISTANCE = 10.0    # metres from the camera to the picture it is on the inside of
PAN_RANGE, TILT_RANGE = 170.0, 45.0     # degrees at |pan| = 1, |tilt| = 1: as web/index.html
FOV_WIDE, FOV_TELE = 70.0, 4.0          # horizontal field of view at zoom 0 and 1


class PygfxError(Exception):
    pass


# What --ndi needs: the module each is imported as, and the package that has it
REQUIREMENTS = (("numpy", "numpy"), ("PIL", "pillow"), ("imageio", "imageio"), ("gltflib", "gltflib"),
                ("pygfx", "pygfx"), ("rendercanvas", "rendercanvas"), ("cyndilib", "cyndilib"))


def check_requirements():
    """Says which packages --ndi needs that are missing, rather than a traceback from the first import"""
    import importlib.util
    missing = [package for module, package in REQUIREMENTS if importlib.util.find_spec(module) is None]
    if missing:
        raise PygfxError(
            f"--ndi needs Python packages that aren't installed: {', '.join(missing)}.\n"
            "    Install them in a virtual environment (a system Python often won't let pip install into it):\n"
            "        python3 -m venv ~/ptzsim-venv\n"
            f"        ~/ptzsim-venv/bin/pip install {' '.join(package for _, package in REQUIREMENTS)}\n"
            "    then run ptzsim with ~/ptzsim-venv/bin/python instead of python3.")


def _imports():
    try:
        import numpy as np
        import pygfx as gfx
        import pylinalg as la
        from rendercanvas.offscreen import RenderCanvas
        return np, gfx, la, RenderCanvas
    except ImportError as e:
        raise PygfxError(f"{e}: pip install pygfx gltflib imageio pillow numpy") from e


def view_of(snap, heading_deg):
    """(yaw, pitch, horizontal fov), in radians, of a PTZ snapshot: as the web view's"""
    return (math.radians(snap.pan * PAN_RANGE + heading_deg), math.radians(snap.tilt * TILT_RANGE),
            math.radians(FOV_WIDE * (FOV_TELE / FOV_WIDE) ** snap.zoom))


# VISCA CAM_WB modes, as ptzsim stores them: what Kelvin each stands for, or None for the gains
WB_KELVIN = {0: 5000.0, 1: 3200.0, 2: 5800.0, 3: 5000.0, 4: 5000.0}


def _kelvin_gains(kelvin):
    """Per-channel gains that make a light of this colour temperature look white
    (Tanner Helland's blackbody approximation), a neutral 5000 K being 1, 1, 1"""
    def rgb(k):
        t = k / 100.0
        r = 255.0 if t <= 66 else 329.698727446 * (t - 60) ** -0.1332047592
        g = 99.4708025861 * math.log(t) - 161.1195681661 if t <= 66 else 288.1221695283 * (t - 60) ** -0.0755148492
        b = 255.0 if t >= 66 else (0.0 if t <= 19 else 138.5177312231 * math.log(t - 10) - 305.0447927307)
        return [min(255.0, max(1.0, c)) / 255.0 for c in (r, g, b)]
    light, ref = rgb(kelvin), rgb(5000.0)
    # A camera set for this temperature divides the light's colour out
    gains = [r / l for r, l in zip(ref, light)]
    return gains


def look_of(visca):
    """The colour treatment a VISCA settings dict asks for, as the pass's uniforms:
    (r, g, b gains, saturation, brightness in stops, effect)"""
    mode = visca.get("wb_mode", 0)
    if mode == 5:       # manual: 0x80 is neutral
        r, b = visca.get("r_gain", 0x80) / 128.0, visca.get("b_gain", 0x80) / 128.0
        gains = [r, 1.0, b]
    else:
        gains = _kelvin_gains(WB_KELVIN.get(mode, 5000.0))
    # CAM_ColorGain 4 is 100%: 0 is 60%, 0xe is 200%
    saturation = 0.6 + 1.4 * visca["color_gain"] / 14.0 if "color_gain" in visca else 1.0
    comp = (visca.get("exposure_comp_pos", 7) - 7) * 0.5 if visca.get("exposure_comp") else 0.0
    stops = (visca.get("bright_pos", 0x10) - 0x10) / 8.0 + comp
    return gains, saturation, stops, visca.get("picture_effect", 0)


class PygfxRenderer:
    def __init__(self, state, scene_path=None, heading=0.0, camera_pos=None, size="1280x720", fps=15.0,
                 exposure=1.0, sinks=(), panorama=None, credit=None):
        self.np, self.gfx, self.la, RenderCanvas = _imports()
        self.state, self.heading, self.fps = state, heading, fps
        self.camera_pos, self.exposure = camera_pos, exposure
        if bool(scene_path) == bool(panorama):
            raise PygfxError("give a glTF scene or a panorama, one of the two")
        self.scene_path, self.panorama = scene_path, panorama
        self.credit = credit            # a line drawn in the corner of every frame, or None
        self.width, self.height = (int(v) for v in size.lower().split("x"))
        self.sinks = list(sinks)         # called with (bgra bytes, width, height) at fps
        self.stop_event = threading.Event()
        self.thread = None
        self.error = None
        self.frames_rendered = self.frames_sent = 0
        self.render_seconds = 0.0

    def start(self):
        self.thread = threading.Thread(target=self._run, name="pygfx", daemon=True)
        self.thread.start()

    def stop(self):
        self.stop_event.set()
        if self.thread:
            self.thread.join(5)

    def poll(self):
        """None while rendering, else a code"""
        return None if self.thread and self.thread.is_alive() else (1 if self.error else 0)

    # ---- everything below runs on the render thread: wgpu isn't shared between threads

    def _run(self):
        try:
            self._build()
            self._loop()
        except Exception as e:      # shown by the caller's watcher
            self.error = e
            import traceback
            traceback.print_exc()

    def _build(self):
        np, gfx, la = self.np, self.gfx, self.la
        from rendercanvas.offscreen import RenderCanvas
        self.canvas = RenderCanvas(size=(self.width, self.height), pixel_ratio=1)
        self.renderer = gfx.renderers.WgpuRenderer(self.canvas, ppaa="none")
        self.scene = gfx.Scene()
        if self.panorama:
            self._build_panorama()
        else:
            self._build_scene()
        self.camera.depth_range = (0.1, self.far)
        self.dof, self.color = _passes(gfx, self.far)
        self.renderer.effect_passes = (self.dof, self.color)
        self._make_credit()
        what = self.panorama or self.scene_path
        print(f"[pygfx] {what}: camera at {tuple(round(float(v), 2) for v in self.camera.local.position)}, "
              f"{self.width}x{self.height} at up to {self.fps:g} fps")

    def _build_scene(self):
        np, gfx = self.np, self.gfx
        gltf = gfx.load_gltf(self.scene_path, quiet=True)
        self.scene.add(gltf.scene)
        self.scene.add(gfx.AmbientLight(intensity=1.5 * self.exposure))
        self.scene.add(gfx.DirectionalLight(intensity=2.0 * self.exposure))
        (x0, y0, z0), (x1, y1, z1) = self.scene.get_world_bounding_box()
        self.scene.add(gfx.Background.from_color("#8fa0b5"))     # a sky, for what an open scene shows above
        self.camera = gfx.PerspectiveCamera(60, self.width / self.height)
        self.camera.local.position = self.camera_pos or ((x0 + x1) / 2, y0 + (y1 - y0) / 3, (z0 + z1) / 2)
        self.far = max(50.0, 2.0 * math.dist((x0, y0, z0), (x1, y1, z1)))

    def _build_panorama(self):
        np, gfx = self.np, self.gfx
        from .hdr import load_panorama
        picture = load_panorama(self.panorama, self.exposure)
        # The sphere's texture runs backwards both ways seen from inside, and a quarter turn
        # round from where pan 0 should be: the middle of the picture, as in the web view
        picture = np.roll(picture[::-1, ::-1], picture.shape[1] // 4, axis=1)
        texture = gfx.Texture(np.ascontiguousarray(picture), dim=2, colorspace="srgb")
        radius = PANORAMA_DISTANCE
        self.scene.add(gfx.Mesh(gfx.sphere_geometry(radius, 96, 48),
                                gfx.MeshBasicMaterial(map=texture, side="back")))
        self.camera = gfx.PerspectiveCamera(60, self.width / self.height)
        self.camera.local.position = (0, 0, 0)
        self.far = radius * 2

    def _make_credit(self):
        """Draws self.credit into a patch to lay over the bottom-left of each frame"""
        self._credit_patch = None
        if not self.credit:
            return
        np = self.np
        from PIL import Image, ImageDraw, ImageFont
        size = max(12, self.height // 38)
        font = ImageFont.load_default(size)
        probe = ImageDraw.Draw(Image.new("L", (1, 1)))
        left, top, right, bottom = probe.textbbox((0, 0), self.credit, font=font)
        pad = size // 2
        w, h = right - left + 2 * pad, bottom - top + 2 * pad
        # Fit it: a credit longer than the picture is wide is cut, not run off
        w = min(w, self.width - 2 * pad)
        patch = Image.new("RGBA", (w, h), (0, 0, 0, 150))
        ImageDraw.Draw(patch).text((pad - left, pad - top), self.credit, fill=(255, 255, 255, 255), font=font)
        rgba = np.asarray(patch).astype(np.float32)
        alpha = rgba[..., 3:4] / 255.0
        bgr = rgba[..., [2, 1, 0]]
        y0, x0 = self.height - h - pad, pad
        self._credit_patch = (y0, x0, bgr, alpha)

    def _burn_credit(self, frame):
        if not self._credit_patch:
            return
        y0, x0, bgr, alpha = self._credit_patch
        h, w = bgr.shape[:2]
        region = frame[y0:y0 + h, x0:x0 + w, :3]
        region[:] = (region * (1.0 - alpha) + bgr * alpha).astype(self.np.uint8)

    def _aim(self, snap):
        np, la = self.np, self.la
        yaw, pitch, hfov = view_of(snap, self.heading)
        aspect = self.width / self.height
        self.camera.fov = math.degrees(2 * math.atan(math.tan(hfov / 2) / aspect))
        # y up, looking down -Z at pan 0: pan right turns clockwise seen from above
        self.camera.local.rotation = la.quat_from_euler((-yaw, pitch, 0.0), order="YXZ")
        # Defocus grows with zoom as a longer lens's depth of field shrinks: as the web view
        zoomed = math.tan(math.radians(FOV_WIDE) / 2) / math.tan(hfov / 2)
        self.dof.set(focus_dist=0.5 * 200.0 ** snap.focus, coc_scale=4.0 * zoomed * self.height / 720.0)

    def _loop(self):
        np = self.np
        interval = 1.0 / self.fps
        last_key, bgra, next_at = None, None, time.monotonic()
        while not self.stop_event.is_set():
            snap = self.state.snapshot()
            with self.state._lock:
                visca, tally = dict(self.state.visca), dict(self.state.tally)
            key = (snap.pan, snap.tilt, snap.zoom, snap.focus, snap.power, tuple(sorted(tally.items())),
                   repr(sorted(visca.items())))
            if key != last_key or bgra is None:
                began = time.perf_counter()
                bgra = self._render(snap, visca, tally)
                self.render_seconds += time.perf_counter() - began
                self.frames_rendered += 1
                last_key = key
            for sink in self.sinks:
                sink(bgra, self.width, self.height)
            self.frames_sent += 1
            next_at += interval
            delay = next_at - time.monotonic()
            if delay > 0:
                self.stop_event.wait(delay)
            else:
                next_at = time.monotonic()      # behind: don't try to catch up

    def _render(self, snap, visca, tally):
        np = self.np
        if not snap.power:
            frame = np.zeros((self.height, self.width, 4), np.uint8)
            frame[..., 3] = 255
            self._burn_credit(frame)
            return frame.tobytes()
        self._aim(snap)
        gains, saturation, stops, effect = look_of(visca)
        self.color.set(gains=gains, saturation=saturation, gain=2.0 ** stops, effect=effect)
        self.renderer.render(self.scene, self.camera)
        rgba = np.asarray(self.canvas.draw())
        frame = np.ascontiguousarray(rgba[..., [2, 1, 0, 3]])      # NDI's BGRA
        border = max(2, self.height // 90)
        for name, bgr in (("green", (60, 200, 60)), ("red", (50, 50, 230))):
            if tally.get(name):
                frame[:border, :, :3] = frame[-border:, :, :3] = bgr
                frame[:, :border, :3] = frame[:, -border:, :3] = bgr
        self._burn_credit(frame)
        return frame.tobytes()


def _passes(gfx, far):
    """The depth-of-field and colour passes, as gfx EffectPasses with a .set(**uniforms)"""
    class Dof(gfx.renderers.wgpu.EffectPass):
        USES_DEPTH = True
        uniform_type = dict(gfx.renderers.wgpu.EffectPass.uniform_type,
                            focus_dist="f4", coc_scale="f4", near="f4", far="f4")
        # A gather blur: each pixel's circle of confusion is how far its distance is from the focus
        # distance in 1/m, as a lens's is; it averages a disc of that radius, skipping neighbours
        # that are in focus and in front, so a sharp foreground doesn't smear over a blurred back
        wgsl = """
        fn linear_depth(d: f32) -> f32 {
            return u_effect.near * u_effect.far / (u_effect.far - d * (u_effect.far - u_effect.near));
        }
        fn coc_at(p: vec2i) -> f32 {
            let dims = vec2i(textureDimensions(colorTex));
            let q = clamp(p, vec2i(0), dims - 1);
            let d = linear_depth(textureLoad(depthTex, q, 0));
            return min(u_effect.coc_scale * abs(1.0 / d - 1.0 / u_effect.focus_dist), 24.0);
        }
        @fragment
        fn fs_main(varyings: Varyings) -> @location(0) vec4<f32> {
            let p = vec2i(varyings.position.xy);
            let dims = vec2i(textureDimensions(colorTex));
            let centre = textureLoad(colorTex, p, 0);
            let coc = coc_at(p);
            if (coc < 0.6) { return centre; }
            var sum = centre.rgb;
            var weight = 1.0;
            for (var i = 1; i < 32; i = i + 1) {
                let r = coc * sqrt(f32(i) / 32.0);
                let a = f32(i) * 2.39996323;
                let q = clamp(p + vec2i(i32(round(r * cos(a))), i32(round(r * sin(a)))), vec2i(0), dims - 1);
                let w = select(0.0, 1.0, coc_at(q) >= r * 0.8);
                sum = sum + w * textureLoad(colorTex, q, 0).rgb;
                weight = weight + w;
            }
            return vec4<f32>(sum / weight, centre.a);
        }
        """

        def __init__(self):
            super().__init__()
            self._uniform_data["near"], self._uniform_data["far"] = 0.1, far
            self.set(focus_dist=10.0, coc_scale=4.0)

        def set(self, **values):
            for name, value in values.items():
                self._uniform_data[name] = float(value)

    class Color(gfx.renderers.wgpu.EffectPass):
        uniform_type = dict(gfx.renderers.wgpu.EffectPass.uniform_type,
                            gains="4xf4", saturation="f4", gain="f4", effect="f4")
        wgsl = """
        @fragment
        fn fs_main(varyings: Varyings) -> @location(0) vec4<f32> {
            let p = vec2i(varyings.position.xy);
            let c = textureLoad(colorTex, p, 0);
            var rgb = c.rgb * u_effect.gains.rgb * u_effect.gain;
            let luma = dot(rgb, vec3<f32>(0.2126, 0.7152, 0.0722));
            rgb = vec3<f32>(luma) + (rgb - vec3<f32>(luma)) * u_effect.saturation;
            if (u_effect.effect > 3.5) { rgb = vec3<f32>(luma); }       // black and white
            else if (u_effect.effect > 1.5) { rgb = vec3<f32>(1.0) - rgb; }   // negative
            return vec4<f32>(rgb, c.a);
        }
        """

        def __init__(self):
            super().__init__()
            self.set(gains=(1, 1, 1), saturation=1.0, gain=1.0, effect=0)

        def set(self, gains=None, **values):
            if gains is not None:
                self._uniform_data["gains"] = (*[float(g) for g in gains], 1.0)
            for name, value in values.items():
                self._uniform_data[name] = float(value)

    return Dof(), Color()


def jpeg_sink(frame_store, quality=80):
    """A sink that puts each frame in a FrameStore, as JPEG, for the web view's MJPEG stream"""
    from PIL import Image
    import numpy as np

    last = [None]

    def sink(bgra, width, height):
        if bgra is last[0]:     # the same picture again: already sent
            return
        last[0] = bgra
        rgb = np.frombuffer(bgra, np.uint8).reshape(height, width, 4)[..., [2, 1, 0]]
        out = io.BytesIO()
        Image.fromarray(rgb).save(out, "JPEG", quality=quality)
        frame_store.put(out.getvalue())
    return sink
