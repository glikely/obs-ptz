"""Finds the picture ptzsim's web view shows as the room around the camera.

--backdrop takes a file, a URL, or the key of a Poly Haven HDRI
(https://polyhaven.com/hdris, CC0: "chapel_day" is
https://polyhaven.com/a/chapel_day), which is downloaded once to the
cache directory and reused. It must be an equirectangular panorama:
the web view reads a Radiance .hdr, or a JPEG or PNG.
"""

import hashlib
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

API = "https://api.polyhaven.com"
# Poly Haven asks API users to identify themselves
USER_AGENT = "ptzsim (obs-ptz test tool)"
KEY_RE = re.compile(r"^[a-z0-9_]+$")


class BackdropError(Exception):
    pass


def default_cache_dir():
    base = os.environ.get("XDG_CACHE_HOME") or os.path.join(os.path.expanduser("~"), ".cache")
    return os.path.join(base, "ptzsim", "backdrops")


def _get(url):
    return urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": USER_AGENT}), timeout=30)


def _get_json(url):
    try:
        with _get(url) as r:
            return json.load(r)
    except (urllib.error.URLError, OSError, ValueError) as e:
        raise BackdropError(f"can't get {url}: {e}")


def _download(url, dest, md5=None):
    if os.path.exists(dest):
        return dest
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    tmp = dest + ".part"
    print(f"[backdrop] downloading {url}")
    digest = hashlib.md5()
    try:
        with _get(url) as r, open(tmp, "wb") as f:
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                digest.update(chunk)
                f.write(chunk)
    except (urllib.error.URLError, OSError) as e:
        if os.path.exists(tmp):
            os.remove(tmp)
        raise BackdropError(f"can't download {url}: {e}")
    if md5 and digest.hexdigest() != md5:
        os.remove(tmp)
        raise BackdropError(f"{url} downloaded wrong: its checksum isn't the {md5} Poly Haven lists")
    os.replace(tmp, dest)
    return dest


def _polyhaven(key, resolution, cache_dir):
    files = _get_json(f"{API}/files/{key}")
    sizes = files.get("hdri") if isinstance(files, dict) else None
    if not sizes:
        raise BackdropError(f"Poly Haven has no HDRI called '{key}' (see https://polyhaven.com/hdris)")
    if resolution not in sizes:
        raise BackdropError(f"'{key}' has no {resolution} version; it has {', '.join(sorted(sizes, key=lambda s: int(s[:-1])))}")
    hdr = sizes[resolution]["hdr"]
    info = _get_json(f"{API}/info/{key}")
    authors = ", ".join(info.get("authors", {})) or "unknown"
    print(f"[backdrop] {info.get('name', key)} by {authors}, CC0, https://polyhaven.com/a/{key}")
    return _download(hdr["url"], os.path.join(cache_dir, f"{key}_{resolution}.hdr"), hdr.get("md5"))


def resolve(spec, resolution="4k", cache_dir=None):
    """The path of a local file holding the picture `spec` names"""
    cache_dir = cache_dir or default_cache_dir()
    if os.path.isfile(spec):
        return spec
    if re.match(r"^https?://", spec):
        ext = os.path.splitext(urllib.parse.urlparse(spec).path)[1].lower() or ".img"
        return _download(spec, os.path.join(cache_dir, hashlib.sha1(spec.encode()).hexdigest()[:16] + ext))
    if KEY_RE.match(spec):
        return _polyhaven(spec, resolution, cache_dir)
    raise BackdropError(f"'{spec}' isn't a file, a URL, or a Poly Haven key")


if __name__ == "__main__":
    print(resolve(sys.argv[1], *(sys.argv[2:3] or ["4k"])))
