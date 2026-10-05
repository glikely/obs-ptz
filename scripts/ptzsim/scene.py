"""Finds a 3D scene for ptzsim's web view to put the camera in, and the
three.js it is drawn with.

--scene takes a glTF file (.glb or .gltf), a URL of one, or the name of a
Khronos sample model: "sponza", or "khronos:<Name>" for any model in
https://github.com/KhronosGroup/glTF-Sample-Assets. Models are downloaded
once to the cache, not bundled: check each one's licence (Sponza's is the
Cryengine Limited License). three.js (MIT) is downloaded the same way,
the first time a scene is used, and served to the page from the cache.
"""

import hashlib
import os
import re
import urllib.parse

from .backdrop import BackdropError, _download, _get_json

THREE_VERSION = "0.170.0"   # the last with a single three.module.min.js
THREE_URL = f"https://cdn.jsdelivr.net/npm/three@{THREE_VERSION}/"
# What the page imports, as {path in the cache's vendor dir: path in the package}
THREE_FILES = {
    "three.module.min.js": "build/three.module.min.js",
    "addons/loaders/GLTFLoader.js": "examples/jsm/loaders/GLTFLoader.js",
    "addons/utils/BufferGeometryUtils.js": "examples/jsm/utils/BufferGeometryUtils.js",
    "addons/environments/RoomEnvironment.js": "examples/jsm/environments/RoomEnvironment.js",
}
KHRONOS_API = "https://api.github.com/repos/KhronosGroup/glTF-Sample-Assets/contents/Models/"
ALIASES = {"sponza": "Sponza"}
NAME_RE = re.compile(r"^[A-Za-z0-9_-]+$")


def default_cache_dir():
    base = os.environ.get("XDG_CACHE_HOME") or os.path.join(os.path.expanduser("~"), ".cache")
    return os.path.join(base, "ptzsim")


def ensure_three(cache_dir=None):
    """The directory holding three.js, downloading what it lacks"""
    vendor = os.path.join(cache_dir or default_cache_dir(), "vendor", f"three-{THREE_VERSION}")
    for name, package_path in THREE_FILES.items():
        _download(THREE_URL + package_path, os.path.join(vendor, name))
    return vendor


def _khronos(name, cache_dir):
    out = os.path.join(cache_dir, "scenes", "khronos-" + name)
    if os.path.isdir(out) and os.path.exists(os.path.join(out, ".complete")):
        return out, _entry(out)
    for variant in ("glTF-Binary", "glTF"):
        try:
            listing = _get_json(f"{KHRONOS_API}{name}/{variant}")
        except BackdropError:
            continue
        if isinstance(listing, list):
            break
    else:
        raise BackdropError(f"the Khronos glTF sample assets have no model '{name}'")
    for item in listing:
        if item.get("type") == "file":
            _download(item["download_url"], os.path.join(out, item["name"]))
    open(os.path.join(out, ".complete"), "w").close()
    return out, _entry(out)


def _entry(directory):
    for ext in (".glb", ".gltf"):
        for name in sorted(os.listdir(directory)):
            if name.lower().endswith(ext):
                return name
    raise BackdropError(f"no .glb or .gltf in {directory}")


def resolve(spec, cache_dir=None):
    """(the directory to serve, the scene file in it) for what --scene names"""
    cache_dir = cache_dir or default_cache_dir()
    if os.path.isfile(spec):
        return os.path.dirname(os.path.abspath(spec)), os.path.basename(spec)
    if re.match(r"^https?://", spec):
        name = os.path.basename(urllib.parse.urlparse(spec).path) or "scene.glb"
        out = os.path.join(cache_dir, "scenes", hashlib.sha1(spec.encode()).hexdigest()[:16])
        _download(spec, os.path.join(out, name))
        return out, name
    name = ALIASES.get(spec.lower()) or (spec[len("khronos:"):] if spec.startswith("khronos:") else None)
    if name and NAME_RE.match(name):
        return _khronos(name, cache_dir)
    raise BackdropError(f"'{spec}' isn't a glTF file, a URL, \"sponza\" or \"khronos:<Name>\"")
