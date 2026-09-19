"""Optional live RTSP video feed that overlays the shared PTZState.

ffmpeg can't act as an RTSP server on its own, so this uses MediaMTX
(https://github.com/bluenviron/mediamtx) as a tiny RTSP relay: ffmpeg
pushes a generated test pattern to it, MediaMTX re-serves it to OBS (or
any RTSP client). The overlay text is refreshed on disk periodically and
picked up by ffmpeg's drawtext filter, so the stream visibly reflects
whatever backend (VISCA, ONVIF, ...) last moved the camera.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

MEDIAMTX_HELP = (
    "MediaMTX not found. Download a release binary from\n"
    "  https://github.com/bluenviron/mediamtx/releases/latest\n"
    "extract it, and either put it on your $PATH or pass --mediamtx /path/to/mediamtx."
)


def find_mediamtx(explicit_path=None):
    if explicit_path:
        if os.path.exists(explicit_path) and os.access(explicit_path, os.X_OK):
            return explicit_path
        return None
    return shutil.which("mediamtx")


class VideoFeed:
    def __init__(self, state, host, rtsp_port, state_file,
                 mediamtx_path=None, width=1280, height=720, fps=15):
        self.state = state
        self.host = host
        self.rtsp_port = rtsp_port
        self.state_file = state_file
        self.mediamtx_path = mediamtx_path
        self.width = width
        self.height = height
        self.fps = fps
        self._procs = []
        self._overlay_thread = None
        self._running = threading.Event()

    def _write_overlay(self):
        try:
            tmp = self.state_file + ".tmp"
            with open(tmp, "w") as f:
                f.write("\n".join(self.state.status_lines()))
            os.replace(tmp, self.state_file)
        except OSError as e:
            print(f"[video] overlay write failed: {e}", file=sys.stderr)

    def _overlay_loop(self):
        while self._running.is_set():
            self._write_overlay()
            time.sleep(0.1)

    def _write_mediamtx_config(self):
        cfg_path = os.path.join(tempfile.gettempdir(), "ptzsim-mediamtx.yml")
        with open(cfg_path, "w") as f:
            f.write(
                f"rtspAddress: :{self.rtsp_port}\n"
                "hls: no\n"
                "webrtc: no\n"
                "rtmp: no\n"
                "srt: no\n"
                "api: no\n"
                "metrics: no\n"
                "logLevel: warn\n"
                "paths:\n"
                "  all_others:\n"
            )
        return cfg_path

    def start(self):
        """Best-effort: prints instructions and returns without a video
        feed if ffmpeg/MediaMTX aren't available."""
        if shutil.which("ffmpeg") is None:
            print("[video] ffmpeg not on PATH; skipping video. (apt install ffmpeg)")
            return False

        mediamtx = find_mediamtx(self.mediamtx_path)
        if mediamtx is None:
            print("[video] " + MEDIAMTX_HELP)
            return False

        self._running.set()
        self._write_overlay()
        cfg = self._write_mediamtx_config()
        print(f"[video] starting {mediamtx} (config: {cfg})")
        mtx_proc = subprocess.Popen([mediamtx, cfg])
        self._procs.append(mtx_proc)

        time.sleep(0.6)  # give MediaMTX a moment to open its listening socket

        push_url = f"rtsp://127.0.0.1:{self.rtsp_port}/stream"
        ff_cmd = [
            "ffmpeg",
            "-hide_banner",
            "-loglevel", "warning",
            "-re",
            "-f", "lavfi",
            "-i", f"testsrc2=size={self.width}x{self.height}:rate={self.fps}",
            "-vf",
            f"drawtext=textfile={self.state_file}:reload=5:"
            "x=20:y=20:fontsize=28:fontcolor=white:box=1:boxcolor=black@0.6",
            "-c:v", "libx264",
            "-preset", "ultrafast",
            "-tune", "zerolatency",
            "-g", "30",
            "-f", "rtsp",
            "-rtsp_transport", "tcp",
            push_url,
        ]
        print(f"[video] $ {' '.join(ff_cmd)}")
        self._procs.append(subprocess.Popen(ff_cmd))

        self._overlay_thread = threading.Thread(target=self._overlay_loop, daemon=True)
        self._overlay_thread.start()

        print(f"[video] OBS can now pull rtsp://{self.host}:{self.rtsp_port}/stream")
        return True

    def poll(self):
        """Drop any subprocess that has exited; returns list of (proc, code)."""
        exited = []
        for p in list(self._procs):
            code = p.poll()
            if code is not None:
                exited.append((p, code))
                self._procs.remove(p)
        return exited

    def stop(self):
        self._running.clear()
        for p in self._procs:
            if p.poll() is None:
                p.terminate()
        self._procs = []
