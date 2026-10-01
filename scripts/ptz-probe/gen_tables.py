#!/usr/bin/env python3
"""Writes what ptz-probe.py asks a camera for, and how it reads the answers,
into ptz-probe.py itself, from the plugin's own source: the generic command
set and the vendor and model names in src/ptz-visca-commands.cpp, the
command sets shipped with the plugin in src/visca-profiles, and the plugin's
version in buildspec.json. So the probe stays one file, which needs nothing
but Python to run, and asks a camera what the plugin's own camera report
does.

Run it after changing any of those; CI runs it with --check, which fails if
ptz-probe.py's tables aren't what it would write.

Usage: python3 scripts/ptz-probe/gen_tables.py [--check]
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
COMMANDS = ROOT / "src" / "ptz-visca-commands.cpp"
PROFILES = ROOT / "src" / "visca-profiles"
PROBE = Path(__file__).resolve().parent / "ptz-probe.py"
BEGIN = "# --- tables: written by gen_tables.py, don't edit ---\n"
END = "# --- end of tables ---\n"

FIELD = re.compile(r'new (\w+)\("(\w+)",(?: PTZVisca::\w+,)? (\d+)(?:, (0x[0-9a-f]+|0b[01]+))?(?:, (true|false))?\)')

# Each of the plugin's field classes as the probe reads and writes it: a
# kind, and for an "int", its mask, whether it is signed, and how far what
# it reads is shifted (see src/ptz-visca-commands.cpp)
CLASSES = {
    "visca_u4": ("int", 0x0f, False, 0),
    "visca_u7": ("int", 0x7f, False, 0),
    "visca_u8": ("int", 0x0f0f, False, 0),
    "visca_u16": ("int", 0x0f0f0f0f, False, 0),
    "visca_s16": ("int", 0x0f0f0f0f, True, 0),
    "visca_u16_high": ("int", 0x0f0f, False, 8),
    "visca_flag": ("flag", 0, False, 0),
    "visca_s4": ("speed", 0, False, 0),
    "visca_s7": ("speed", 0, False, 0),
}


def field(cls, name, offset, mask, signed):
    """[name, kind, offset, mask, signed, shift]"""
    offset = int(offset)
    if cls in CLASSES:
        kind, m, s, shift = CLASSES[cls]
        return [name, kind, offset, m, s, shift]
    if cls == "int_field":
        return [name, "int", offset, int(mask, 0), signed == "true", 0]
    if cls == "bool_field":
        return [name, "bool", offset, int(mask, 0), False, 0]
    if cls == "string_lookup_field":
        return [name, "name", offset, int(mask, 0), False, 0]
    raise ValueError(f"no kind for {cls}")


def generic(source):
    """The generic command set's controls, each command and inquiry by its
    bytes, with its fields"""
    defs = {}
    for m in re.finditer(r'const (?:PTZCmd|PTZInq) (\w+)\(\s*"([0-9a-f]+)"(.*?)\);\n', source, re.S):
        defs[m.group(1)] = {"cmd": m.group(2), "fields": [field(*f.groups()) for f in FIELD.finditer(m.group(3))]}
    i = source.index("visca_controls = {")
    body = source[i:source.index("\n};", i)]
    controls = []
    for entry in re.split(r'\n\t\{(?=")', body)[1:]:
        key = re.match(r'"(\w+)"', entry).group(1)
        rest = entry[len(key) + 3:]
        set_to = re.findall(r'\{(\d+), (?:assuming\()?(VISCA_\w+)', rest)
        names = re.findall(r'VISCA_\w+', rest)
        control = {"key": key, "set": None, "set_to": {}, "reads": []}
        if set_to:
            control["set_to"] = {v: defs[n]["cmd"] for v, n in set_to}
            names = [n for n in names if n not in {n for _, n in set_to}]
        elif names and not rest.lstrip().startswith("std::nullopt"):
            control["set"] = {"cmd": defs[names[0]]["cmd"], "args": defs[names[0]]["fields"]}
            names = names[1:]
        control["reads"] = [defs[n]["cmd"] for n in names]
        controls.append(control)
    inquiries = {}
    for control in controls:
        for cmd in control["reads"]:
            inquiries[cmd] = next(d["fields"] for d in defs.values() if d["cmd"] == cmd)
    return controls, inquiries


def names(source, table):
    i = source.index(f"PTZVisca::{table} = {{")
    body = source[i:source.index("\n};", i)]
    return {str(int(k, 16)): v for k, v in re.findall(r'\{(0x[0-9a-f]+), "([^"]*)"\}', body)}


JSON_KINDS = {"flag": ("flag", 0), "u4": ("int", 0x0f), "u7": ("int", 0x7f)}


def command_sets(generic_controls):
    """Each shipped command set: the cameras it is chosen for, every
    inquiry its controls read with, and, for one whose camera can't be asked
    for everything in standby, how it asks whether the camera is on"""
    files = {json.loads(p.read_text())["id"]: json.loads(p.read_text()) for p in sorted(PROFILES.glob("*.json"))}
    generic_power = next(c for c in generic_controls if c["key"] == "power_on")["reads"]

    def resolve(cid):
        data = files[cid]
        parent = data.get("extends", "generic")
        if parent == "generic":
            reads, standby = {}, None
        else:
            reads, standby, _, _ = resolve(parent)
            reads = dict(reads)
        for control in data.get("controls", []):
            if "reads" in control:
                reads[control["key"]] = control["reads"]
        for gone in data.get("remove", []):
            reads.pop(gone, None)
        if "standby_reads" in data:
            standby = data["standby_reads"]
        models = [int(m.replace(":", ""), 16) for m in data.get("models", []) if not m.endswith(":*")]
        vendors = [int(m[:-2], 16) for m in data.get("models", []) if m.endswith(":*")]
        return reads, standby, models, vendors

    out = []
    for cid in files:
        reads, standby, models, vendors = resolve(cid)
        removed = set(files[cid].get("remove_inquiries", []))
        inquiries = sorted({r["cmd"] for rs in reads.values() for r in rs if r["cmd"] not in removed})
        power = None
        if standby is not None:
            power = []
            for r in reads.get("power_on", [{"cmd": c} for c in generic_power]):
                if "results" in r:
                    result = r["results"][0]
                    kind, mask = JSON_KINDS[result["type"]]
                    power.append({"cmd": r["cmd"], "field": ["power_on", kind, result["offset"], mask, False, 0]})
                else:
                    power.append({"cmd": r["cmd"]})
        out.append({"id": cid, "models": models, "vendors": vendors, "inquiries": inquiries,
                    "power": power})
    return out


def tables():
    source = COMMANDS.read_text()
    controls, inquiries = generic(source)
    return {
        "version": json.loads((ROOT / "buildspec.json").read_text())["version"],
        "controls": controls,
        "inquiries": inquiries,
        "command_sets": command_sets(controls),
        "vendors": names(source, "viscaVendors"),
        "models": names(source, "viscaModels"),
    }


def written():
    return BEGIN + "TABLES = json.loads(r'''\n" + json.dumps(tables(), indent=1) + "\n''')\n" + END


def main():
    text = PROBE.read_text()
    i, j = text.index(BEGIN), text.index(END) + len(END)
    new = text[:i] + written() + text[j:]
    if "--check" in sys.argv[1:]:
        if new != text:
            print("scripts/ptz-probe/ptz-probe.py's tables aren't the plugin's: "
                  "run python3 scripts/ptz-probe/gen_tables.py", file=sys.stderr)
            return 1
        return 0
    PROBE.write_text(new)
    return 0


if __name__ == "__main__":
    sys.exit(main())
