class Backend:
    """A protocol driver that speaks a PTZ wire protocol on top of a
    shared PTZState."""

    def start(self, loop):
        raise NotImplementedError

    def stop(self):
        pass
