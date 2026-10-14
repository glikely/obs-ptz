"""A client of the UI driver (tests/ui-driver/), the plugin that lets a client
of obs-websocket find and drive OBS's Qt widgets, over an obsws.Client.

Each request's data, and its answer, is one string field, "json", holding a
JSON object, as obs-websocket can't pass a vendor an array of plain values:
this hides that.

Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
SPDX-License-Identifier: GPL-2.0-or-later
"""

import json

from obsws import ObsWebSocketError

VENDOR = "ui-driver"
# obs-websocket's ResourceNotFound: there is no such vendor, or request
OBS_NOT_FOUND = 600


class UiDriverError(Exception):
    """A request the driver could not do: .code is its error, "not_found", say"""

    def __init__(self, code, message):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


class UiDriver:
    def __init__(self, ws):
        self.ws = ws

    def call(self, request, **data):
        """The driver's answer to `request`, with `data` as its JSON"""
        answer = self.ws.vendor(VENDOR, request, {"json": json.dumps(data)})
        result = json.loads(answer.get("json") or "{}")
        if "error" in result:
            raise UiDriverError(result["error"], result.get("message", ""))
        return result

    def available(self):
        """Whether OBS has the driver: it is a plugin of its own, and only
        answers when OBS was started with OBS_UI_DRIVER=1"""
        try:
            self.call("ping")
        except ObsWebSocketError as e:
            if e.status.get("code") == OBS_NOT_FOUND:
                return False
            raise
        return True
