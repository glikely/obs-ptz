#!/usr/bin/env python3
"""A sample client of the PTZ API over obs-websocket.

Lists the PTZ cameras OBS has, lets you choose one, and moves it and recalls
and saves its presets, through the "obs-ptz" vendor's requests ("WebSocket
binding" in docs/ptz-device-api.md). It is meant to be read as much as run:
everything a client needs is in the Ptz class below, and the rest is the
prompt that drives it.

Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
SPDX-License-Identifier: GPL-2.0-or-later

Needs Python 3.9 or later and the websockets package (pip install websockets),
and OBS with obs-websocket's server turned on (Tools > WebSocket Server
Settings) and obs-ptz's "websocket_access" left at "control".

    ptz-websocket-client.py                          # an interactive prompt
    ptz-websocket-client.py --password secret        # if the server has one
    ptz-websocket-client.py --camera "Stage" recall Wide    # one command, then exit

At the prompt, "help" lists the commands:

    cameras                     the PTZ cameras there are
    camera NAME|NUMBER          choose one
    move PAN TILT [ZOOM] [SECS] move at a speed (-1 to 1) for SECS (1), then stop
    goto PAN TILT [ZOOM]        move to a position: pan, tilt -1 to 1, zoom 0 to 1
    stop                        stop moving
    home                        go to the home position
    presets                     the camera's presets, in the dock's order
    recall NAME|NUMBER|ID       go to a preset
    save NAME|NUMBER|ID         save where the camera is into a preset
    state [all]                 where the camera is, and what it can do
"""

import argparse
import base64
import cmd
import hashlib
import itertools
import json
import sys
import threading
import time

try:
    from websockets.sync.client import connect
except ImportError:
    sys.exit("This needs the websockets package: pip install websockets")

VENDOR = "obs-ptz"

# obs-websocket's opcodes, and the bit of eventSubscriptions that asks for
# vendors' events, which a client is not sent unless it asks
OP_HELLO, OP_IDENTIFY, OP_IDENTIFIED, OP_EVENT, OP_REQUEST, OP_REQUEST_RESPONSE = 0, 1, 2, 5, 6, 7
EVENT_VENDORS = 1 << 9


class PtzError(Exception):
    pass


class Ptz:
    """Calls the obs-ptz vendor's requests over a connection to obs-websocket,
    and hands its events to on_event(name, data)"""

    def __init__(self, ws, password=None, on_event=None, timeout=10):
        self.timeout = timeout
        self.on_event = on_event or (lambda name, data: None)
        self._ws = ws

        # obs-websocket says Hello; the client answers with Identify, giving the
        # password's answer to the challenge if the server asks for one
        hello = json.loads(self._ws.recv(timeout=timeout))
        identify = {"rpcVersion": hello["d"]["rpcVersion"], "eventSubscriptions": EVENT_VENDORS}
        auth = hello["d"].get("authentication")
        if auth:
            if password is None:
                raise PtzError("the server needs a password: give it with --password")
            secret = base64.b64encode(hashlib.sha256((password + auth["salt"]).encode()).digest())
            identify["authentication"] = base64.b64encode(
                hashlib.sha256(secret + auth["challenge"].encode()).digest()).decode()
        self._ws.send(json.dumps({"op": OP_IDENTIFY, "d": identify}))
        if json.loads(self._ws.recv(timeout=timeout)).get("op") != OP_IDENTIFIED:
            raise PtzError("obs-websocket did not accept the client: is the password right?")

        # A camera that moves sends a great many events, which have to be read
        # as they come or the connection is closed, so a thread of its own reads
        # everything, and hands each answer to the request that is waiting for it
        self._ids = itertools.count(1)
        self._answers = {}
        self._answered = threading.Condition()
        self._closed = False
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        try:
            for message in self._ws:
                msg = json.loads(message)
                if msg["op"] == OP_REQUEST_RESPONSE:
                    with self._answered:
                        self._answers[msg["d"]["requestId"]] = msg["d"]
                        self._answered.notify_all()
                elif msg["op"] == OP_EVENT and msg["d"]["eventType"] == "VendorEvent":
                    event = msg["d"]["eventData"]
                    if event["vendorName"] == VENDOR:
                        self.on_event(event["eventType"], event["eventData"])
        finally:
            with self._answered:
                self._closed = True
                self._answered.notify_all()

    def call(self, request, **data):
        """Calls one of the vendor's requests, a proc of a device (with
        device=its UUID) or one of the binding's own, and returns its answer"""
        request_id = str(next(self._ids))
        self._ws.send(json.dumps({"op": OP_REQUEST, "d": {
            "requestType": "CallVendorRequest",
            "requestId": request_id,
            "requestData": {"vendorName": VENDOR, "requestType": request, "requestData": data},
        }}))
        with self._answered:
            self._answered.wait_for(lambda: request_id in self._answers or self._closed, timeout=self.timeout)
            if request_id not in self._answers:
                raise PtzError(f"{request}: " + ("OBS closed the connection" if self._closed else "no answer"))
            response = self._answers.pop(request_id)

        # obs-websocket's own failure: chiefly, no such request, which is what a
        # client is told when obs-ptz's websocket_access doesn't allow it
        status = response["requestStatus"]
        if not status["result"]:
            raise PtzError(f"{request}: {status.get('comment', status)}")
        # obs-ptz's: the request was made, but could not be done
        answer = response.get("responseData", {}).get("responseData", {})
        if "error" in answer:
            raise PtzError(f"{request}: {answer['error']}: {answer.get('message', '')}")
        return answer


def pick(items, key, what, label):
    """The item of `items` that `key` names: its number in the list (from 1),
    or what `label` gives for it, exactly or else by the start of it"""
    if key.isdigit() and 1 <= int(key) <= len(items):
        return items[int(key) - 1]
    for match in (lambda s: s == key, lambda s: s.lower() == key.lower(), lambda s: s.lower().startswith(key.lower())):
        found = [i for i in items if any(match(s) for s in label(i))]
        if len(found) == 1:
            return found[0]
        if len(found) > 1:
            raise PtzError(f"{key!r} could be more than one {what}")
    raise PtzError(f"there is no {what} {key!r}")


def numbers(arg, least, most):
    try:
        values = [float(v) for v in arg.split()]
    except ValueError:
        values = []
    if not least <= len(values) <= most:
        raise PtzError("give numbers: see help")
    return values


class Prompt(cmd.Cmd):
    intro = 'Type "help" for the commands, "quit" to leave.'

    def __init__(self, ws, password):
        super().__init__()
        self.device = None  # the camera's entry in ptz_list_devices
        self.failed = False  # a command has failed
        self.ptz = Ptz(ws, password, on_event=self.event)
        version = self.ptz.call("ptz_bridge_version")
        print(f"Connected: the PTZ websocket binding is version {version['major']}.{version['minor']}")
        self.prompt = "ptz> "

    # A request that fails says why, and the prompt goes on
    def onecmd(self, line):
        try:
            return super().onecmd(line)
        except PtzError as e:
            print(f"error: {e}")
            self.failed = True
            return False

    def emptyline(self):
        pass

    def event(self, name, data):
        """Called on the reading thread, for each of obs-ptz's events. Most are
        about the chosen camera, which this tells of; the rest are ignored"""
        if name == "ptz_device_removed" and self.device and data["device"] == self.device["device"]:
            print(f"\n[{self.device['source']} has gone]")
            self.device = None
            self.prompt = "ptz> "
        elif not self.device or data.get("device") != self.device["device"]:
            return
        elif name == "ptz_preset_recalled":
            print(f"\n[arrived at preset {data['id']}]")
        elif name == "ptz_state_changed" and "connected" in data["changed"]:
            print(f"\n[{'connected to' if data['changed']['connected'] else 'lost'} the camera]")

    def cameras(self):
        return self.ptz.call("ptz_list_devices")["devices"]

    def camera(self):
        if not self.device:
            raise PtzError('choose a camera first: see "cameras" and "camera"')
        return self.device["device"]

    def presets(self):
        """The camera's presets, as a list in the order the dock shows them"""
        listed = self.ptz.call("ptz_preset_get_list", device=self.camera())["return"]
        return [listed["presets"][p["id"]] for p in listed["order"]]

    def preset(self, arg):
        if not arg:
            raise PtzError("which preset?")
        return pick(self.presets(), arg, "preset", lambda p: (p["id"], p["name"]))

    def do_cameras(self, arg):
        """cameras: the PTZ cameras there are, by the source each is on, and
        the device's own name, which for a filter is the filter's"""
        for n, d in enumerate(self.cameras(), 1):
            chosen = "*" if self.device and d["device"] == self.device["device"] else " "
            print(f"{chosen} {n:2}  {d['source'] or '(no source)':24} {d['name']}")

    def do_camera(self, arg):
        """camera NAME|NUMBER: choose the camera the other commands act on, by
        its source's name, its own, or its number in the list of "cameras" """
        if not arg:
            raise PtzError("which camera? see \"cameras\"")
        self.device = pick(self.cameras(), arg, "camera", lambda d: (d["source"], d["name"]))
        self.prompt = f"ptz {self.device['source'] or self.device['name']}> "

    def do_move(self, arg):
        """move PAN TILT [ZOOM] [SECONDS]: move at a speed, each -1 to 1, for
        SECONDS (1 if not given), then stop. A negative pan is left, tilt down,
        and zoom out"""
        v = numbers(arg, 2, 4)
        speeds = {"pan": v[0], "tilt": v[1]}
        if len(v) > 2:
            speeds["zoom"] = v[2]
        self.ptz.call("ptz_move", device=self.camera(), **speeds)
        # ptz_move goes on until it is told otherwise: stop it whatever happens
        try:
            time.sleep(v[3] if len(v) > 3 else 1.0)
        finally:
            self.ptz.call("ptz_stop", device=self.camera())

    def do_goto(self, arg):
        """goto PAN TILT [ZOOM]: move to a position, pan and tilt -1 to 1, zoom
        0 to 1. Needs a camera that can move to one"""
        v = numbers(arg, 2, 3)
        position = {"pan": v[0], "tilt": v[1]}
        if len(v) > 2:
            position["zoom"] = v[2]
        self.ptz.call("ptz_move_abs", device=self.camera(), **position)

    def do_stop(self, arg):
        """stop: stop moving"""
        self.ptz.call("ptz_stop", device=self.camera())

    def do_home(self, arg):
        """home: go to the home position"""
        self.ptz.call("ptz_home_recall", device=self.camera())

    def do_presets(self, arg):
        """presets: the camera's presets, in the order the dock shows them"""
        for n, p in enumerate(self.presets(), 1):
            print(f"{n:3}  {p['name'] or '(no name)':24} {p['id']}")

    def do_recall(self, arg):
        """recall NAME|NUMBER|ID: go to a preset"""
        preset = self.preset(arg)
        self.ptz.call("ptz_preset_recall", device=self.camera(), id=preset["id"])
        print(f"going to {preset['name'] or preset['id']}")

    def do_save(self, arg):
        """save NAME|NUMBER|ID: save where the camera is into a preset it has"""
        preset = self.preset(arg)
        self.ptz.call("ptz_preset_save", device=self.camera(), id=preset["id"])
        print(f"saved {preset['name'] or preset['id']}")

    def do_state(self, arg):
        """state [all]: where the camera is, and what it can do; with "all",
        every key the device has, its driver's own too"""
        state = self.ptz.call("ptz_get_state", device=self.camera())["state"]
        if arg == "all":
            print(json.dumps(state, indent=2, sort_keys=True))
            return
        print("connected:", state["connected"])
        # A position is there only once the camera has said where it is
        for axis in ("pan", "tilt", "zoom", "focus"):
            if axis in state:
                print(f"{axis + ':':10} {state[axis]:.3f}")
        print("features:", " ".join(sorted(f for f, has in state["features"].items() if has)))

    def do_quit(self, arg):
        """quit: leave"""
        return True

    do_EOF = do_quit


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=4455)
    parser.add_argument("--password", help="obs-websocket's password, if it has one")
    parser.add_argument("--camera", help="the camera to choose at once, by name or number")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="one command to run, instead of a prompt")
    args = parser.parse_args()

    try:
        with connect(f"ws://{args.host}:{args.port}") as ws:
            prompt = Prompt(ws, args.password)
            if args.camera:
                prompt.onecmd(f"camera {args.camera}")
                if not prompt.device:
                    return 1
            if args.command:
                prompt.onecmd(" ".join(args.command))
                return 1 if prompt.failed else 0
            prompt.cmdloop()
    except OSError as e:
        sys.exit(f"Could not connect to obs-websocket at {args.host}:{args.port}: {e}")
    except PtzError as e:
        sys.exit(str(e))
    except KeyboardInterrupt:
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
