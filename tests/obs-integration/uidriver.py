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

    def find(self, *path, all=False):
        """The objects `path` names, described: each segment a str, an
        objectName, or a dict of criteria (see tests/ui-driver/README.md).
        With all, the last segment may name more than one."""
        return self.call("find", path=list(path), all=all)["objects"]

    def get(self, *path, properties):
        """The properties named, of the object `path` names, by name"""
        return self.call("get", path=list(path), properties=list(properties))["properties"]

    def get1(self, *path, name):
        """One property of the object `path` names"""
        return self.get(*path, properties=[name])[name]

    def tree(self, *path, depth=3):
        """The object `path` names, described, with its children to `depth`
        levels; with no path, a list of every window so"""
        data = {"path": list(path)} if path else {}
        return self.call("tree", depth=depth, **data)["tree"]

    def clipboard(self):
        """The text on the clipboard"""
        return self.call("clipboard")["text"]

    def set(self, *path, **properties):
        """Sets the properties given, of the object `path` names"""
        self.call("set", path=list(path), properties=properties)

    def invoke(self, *path, method, args=(), queued=False):
        """Calls a slot, or invokable method, of the object `path` names, and
        gives back what it returns. Queued, it is called once the driver has
        answered, which a method that opens a modal dialog needs."""
        return self.call("invoke", path=list(path), method=method, args=list(args), queued=queued).get("return")

    def activate(self, *path, data=None, text=None, role=None):
        """Chooses the item of the combo box `path` names whose data (of item data
        role `role`, Qt::UserRole by default), or text, is the one given, as a user
        would; gives back its index"""
        choice = {"data": data} if data is not None else {"text": text}
        if role is not None:
            choice["role"] = role
        return self.call("activate", path=list(path), **choice)["index"]

    def select(self, *path, value, role="display", column=0):
        """Selects, as a click does, the first row of the view `path` names whose
        `role` (a name from its model's roleNames()) is `value`; gives back the row.
        A value of None leaves no row current or selected, and gives back -1."""
        return self.call("select", path=list(path), value=value, role=role, column=column)["row"]

    def rows(self, *path, roles=("display",), column=0):
        """The rows of the view `path` names, [{role: value}...], with each of
        `roles` (names from its model's roleNames()); also the row that is current
        (-1 if none) and those that are selected: (rows, current, selected)"""
        answer = self.call("rows", path=list(path), roles=list(roles), column=column)
        return answer["rows"], answer["current"], answer["selected"]

    def set_data(self, *path, row, value, role="edit", column=0):
        """Sets the data of `role` of row `row` of the view `path` names, as an editor
        or a click on a control in the row does"""
        self.call("setdata", path=list(path), row=row, value=value, role=role, column=column)

    def move_row(self, *path, frm, to):
        """Moves row `frm` of the view `path` names to before row `to`, as the view's
        own moving of a row does (its model's moveRow()); whether it moved"""
        return self.call("moverow", path=list(path), **{"from": frm, "to": to})["moved"]

    def key(self, *path, key):
        """Presses and releases `key` ("Escape", "Return", "Ctrl+A") in the widget `path` names"""
        self.call("key", path=list(path), key=key)

    def watch(self, *path, signal):
        """Starts counting, from nothing, a signal of the object `path` names;
        gives back the watch's id, the same if it is watched again"""
        return self.call("watch", path=list(path), signal=signal)["watch"]

    def count(self, watch):
        """How often the watched signal has been sent since it was watched"""
        return self.call("count", watch=watch)["count"]

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
