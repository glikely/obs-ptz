"""Sends ptzsim's picture as an NDI source, with cyndilib (`pip install cyndilib`,
which bundles the NDI runtime), so OBS's NDI source path can be tested against it.
"""


from fractions import Fraction


class NdiError(Exception):
    pass


class NdiSender:
    def __init__(self, name, width, height, fps, state=None):
        try:
            from cyndilib.sender import Sender
            from cyndilib.video_frame import VideoSendFrame
            from cyndilib.wrapper.ndi_structs import FourCC
        except ImportError as e:
            raise NdiError(f"{e}: pip install cyndilib") from e
        frame = VideoSendFrame()
        frame.set_resolution(width, height)
        frame.set_frame_rate(Fraction(round(fps * 1000), 1000))
        frame.set_fourcc(FourCC.BGRA)
        self.sender = Sender(name, clock_video=False)
        self.sender.set_video_frame(frame)
        self.name, self.state = name, state
        self.size = (width, height)
        self._tally = (False, False)
        self.sender.open()
        print(f"[ndi] sending '{name}' ({width}x{height} at {fps:g} fps)")

    def __call__(self, bgra, width, height):
        if (width, height) != self.size:
            return
        self.sender.write_video(self._array(bgra))
        if self.state is not None:
            # OBS lighting a source's program or preview tally lights the simulated lamp
            self.sender.update_tally(0)
            now = (bool(self.sender.program_tally), bool(self.sender.preview_tally))
            if now != self._tally:      # only a change, so VISCA's own tally commands still count
                self._tally = now
                with self.state._lock:
                    self.state.tally["red"], self.state.tally["green"] = now

    @staticmethod
    def _array(bgra):
        import numpy as np
        return np.frombuffer(bgra, np.uint8).copy()   # cyndilib wants a writable buffer

    def stop(self):
        self.sender.close()
