"""Answering as the camera a camera report was made of did.

The plugin's camera report (doc/visca-protocol.md, "Camera Reports") has
every inquiry it asked a camera, with the reply or the error, and the
commands it sent back to it, with what came of them. ptzsim, given one with
--visca-report, is that camera, as far as the report goes:

- An inquiry the camera answered is answered with its reply. Where ptzsim
  has one of its own as long, its own goes instead, so what is set and
  moved shows in what is read back. The version inquiry's is always the
  camera's.
- An inquiry the camera answered with an error is answered with that error,
  or not at all if it never answered. One the report doesn't have is
  answered with a syntax error: the report has everything the plugin asks.
- A command like one the camera refused (the same command, with any value)
  is refused as it was; one it only ACKed is ACKed and never completed; and
  one it took is taken, even if ptzsim has nothing of its own for it. The
  rest, such as the moves, which a report doesn't try, are ptzsim's own.
"""

import json

ERRORS = {
    "message length error": 0x01,
    "syntax error": 0x02,
    "buffer full": 0x03,
    "cancelled": 0x04,
    "no socket": 0x05,
    "not executable": 0x41,
}


def error_code(result):
    """The VISCA error a report's result names, or None for no reply"""
    if result == "no reply":
        return None
    if result in ERRORS:
        return ERRORS[result]
    if result.startswith("error "):
        return int(result[len("error "):], 16)
    return ERRORS["syntax error"]


def shape(dg):
    """What makes a command the command it is, whatever its value: its
    category and command bytes, and the length"""
    return (len(dg), bytes(dg[1:5] if len(dg) > 2 and dg[2] == 0x7e else dg[1:4]))


class ViscaReportReplay:
    def __init__(self, report):
        if report.get("report") != "obs-ptz camera report" or report.get("protocol") != "visca":
            raise ValueError("not a VISCA camera report from obs-ptz")
        camera = report.get("camera", {})
        self.vendor_id = int(camera["vendor_id"], 16) if "vendor_id" in camera else None
        self.model_id = int(camera["model_id"], 16) if "model_id" in camera else None
        # by the inquiry, without its terminator, as ptzsim gets it
        self.inquiries = {bytes.fromhex(i["inquiry"])[:-1]: i for i in report.get("inquiries", [])}
        self.commands = {}
        for command in report.get("commands", []):
            self.commands[shape(bytes.fromhex(command["command"])[:-1])] = command["result"]

    @classmethod
    def load(cls, path):
        with open(path) as f:
            return cls(json.load(f))

    def handle(self, logic, dg):
        """Answers `dg` as the camera did, if the report says how; says
        whether it did"""
        dg = b'\x81' + dg[1:]
        if dg[1] == 0x09:
            return self.inquiry(logic, dg)
        return self.command(logic, dg)

    def inquiry(self, logic, dg):
        entry = self.inquiries.get(dg)
        if entry is None:
            logic.send_datagram(b'\x60\x02')
        elif "error" in entry:
            code = error_code(entry["error"])
            if code is not None:
                logic.send_datagram(bytes([0x60, code]))
        else:
            reply = bytes.fromhex(entry["reply"])
            own = logic.capture(dg)
            if dg[1:4] != b'\x09\x00\x02' and len(own) == 1 and len(own[0]) == len(reply) and own[0][1] == 0x50:
                reply = own[0]
            logic._send_datagram(reply)
        return True

    def command(self, logic, dg):
        result = self.commands.get(shape(dg))
        if result is None:
            return False
        if result in ("completed", "ack"):
            own = logic.capture(dg)
            if any(len(o) > 2 and o[1] == 0x60 for o in own):
                own = [b'\x90\x41\xff', b'\x90\x51\xff']
            for reply in own:
                # only ACKed: never completed
                if result == "ack" and (reply[1] & 0xf0) == 0x50:
                    continue
                logic._send_datagram(reply)
            return True
        code = error_code(result)
        if code is not None:
            logic.send_datagram(bytes([0x60, code]))
        return True
