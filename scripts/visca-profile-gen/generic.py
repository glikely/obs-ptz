"""What the generic command set has, read from src/ptz-visca-commands.cpp"""
import re
from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / "src" / "ptz-visca-commands.cpp"


def load():
    s = open(SRC).read()
    defs = dict(re.findall(r'const (?:PTZCmd|PTZInq) (\w+)\(\s*"([0-9a-f]+)"', s))
    i = s.index("visca_controls = {")
    body = s[i:s.index("\n};", i)]
    # entries start with {"key", at one tab
    entries = re.split(r'\n\t\{(?=")', body)[1:]
    controls = []
    for e in entries:
        key = re.match(r'"(\w+)"', e).group(1)
        rest = e[len(key) + 3:]
        setto = re.findall(r'\{(\d+), (?:assuming\()?(VISCA_\w+)', rest)
        names = re.findall(r'VISCA_\w+', rest)
        ctrl = {"key": key, "set": None, "set_to": {}, "reads": []}
        if setto:
            ctrl["set_to"] = {v: n for v, n in setto}
            used = {n for _, n in setto}
            names = [n for n in names if n not in used]
        elif names and not rest.lstrip().startswith("std::nullopt"):
            ctrl["set"] = names[0]
            names = names[1:]
        ctrl["reads"] = names
        controls.append(ctrl)
    def table(name):
        j = s.index(name + " = {")
        return {k: v for k, v in re.findall(r'\{"(\w+)", (VISCA_\w+)\}', s[j:s.index("\n};", j)])}
    return defs, controls, table("visca_actions"), table("visca_triggers")


if __name__ == "__main__":
    d, c, a, t = load()
    for x in c:
        print(x)
    print(a)
    print(t)
