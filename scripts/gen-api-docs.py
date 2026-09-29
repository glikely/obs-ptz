#!/usr/bin/env python3
"""Generates docs/ptz-device-api.md from the proc_handler_add()/
signal_handler_add() call sites in src/ptz-device.cpp, and the API version
from src/ptz.h.

The proc_handler/signal_handler API is defined entirely by those call
sites (signature string literals, grouped by which handler variable they
register on) plus whatever comment sits directly above each group in the
source -- so rather than hand-maintain a copy of it in docs/ that drifts
the moment someone adds/renames/removes an entry, this script re-derives
the doc from the source itself. Run it after touching any
proc_handler_add()/signal_handler_add() call in src/ptz-device.cpp, and
commit the regenerated docs/ptz-device-api.md alongside the code change.

Usage: python3 scripts/gen-api-docs.py
"""
import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SOURCE = REPO_ROOT / "src" / "ptz-device.cpp"
VERSION_HEADER = REPO_ROOT / "src" / "ptz.h"
OUTPUT = REPO_ROOT / "docs" / "ptz-device-api.md"

CALL_RE = re.compile(r"\b(proc_handler_add|signal_handler_add)\s*\(")

# Which handler variable maps to which documented section, in the order
# they should appear in the generated doc.
SECTIONS = [
    ("ptz_ph", "Global PTZ proc_handler", "obtained via `ptz_get_proc_handler()`"),
    ("ph", "OBS main proc_handler", "obtained via `obs_get_proc_handler()`"),
    ("ptz_sh", "Global PTZ signal_handler", "obtained via `ptz_get_signal_handler()`"),
    ("handler", "Per-device proc_handler",
     "handed to listeners as `proc_handler` on the `ptz_device_create` signal"),
    ("sigs", "Per-device signal_handler",
     "handed to listeners as `signal_handler` on the `ptz_device_create` signal"),
]


def find_matching_paren(text, open_idx):
    """Returns the index of the ')' matching the '(' at open_idx, skipping
    over the contents of "..." string literals (signature strings like
    "void ptz_stop()" contain parens of their own)."""
    depth = 0
    i = open_idx
    while i < len(text):
        c = text[i]
        if c == '"':
            i += 1
            while i < len(text) and text[i] != '"':
                i += 1
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ValueError(f"unbalanced parens starting at {open_idx}")


def extract_comment_above(lines, start_line_idx):
    """start_line_idx is 0-based index of the line a call statement starts
    on. Walks upward over a contiguous block comment or //-comment run
    directly above it and returns its text, or None if the line directly
    above isn't a comment (e.g. it's blank, or other code)."""
    i = start_line_idx - 1
    if i < 0:
        return None
    stripped = lines[i].strip()
    if stripped.endswith("*/"):
        text_lines = []
        while i >= 0:
            text_lines.append(lines[i])
            if lines[i].lstrip().startswith("/*") or lines[i].lstrip().startswith("/**"):
                break
            i -= 1
        text_lines.reverse()
        body = "\n".join(text_lines)
        body = re.sub(r"^\s*/\*+\s*", "", body)
        body = re.sub(r"\s*\*/\s*$", "", body)
        body = "\n".join(re.sub(r"^\s*\*\s?", "", l) for l in body.splitlines())
        return " ".join(l.strip() for l in body.splitlines() if l.strip())
    if stripped.startswith("//"):
        text_lines = []
        while i >= 0 and lines[i].strip().startswith("//"):
            text_lines.append(lines[i].strip()[2:].strip())
            i -= 1
        text_lines.reverse()
        return " ".join(text_lines)
    return None


def parse_calls(text):
    lines = text.splitlines()
    # Byte offset of the start of each line, to map a regex match index
    # back to a 0-based line number.
    line_starts = []
    offset = 0
    for line in lines:
        line_starts.append(offset)
        offset += len(line) + 1

    def line_index_for(pos):
        lo, hi = 0, len(line_starts) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if line_starts[mid] <= pos:
                lo = mid
            else:
                hi = mid - 1
        return lo

    calls = []
    prev_end_line = None
    prev_description = None
    for m in CALL_RE.finditer(text):
        call_start_line = line_index_for(m.start())
        open_paren = text.index("(", m.end() - 1)
        close_paren = find_matching_paren(text, open_paren)
        args = text[open_paren + 1:close_paren]
        arg_m = re.match(r'\s*(\w+)\s*,\s*"([^"]*)"', args, re.DOTALL)
        if not arg_m:
            continue
        handler_var, signature = arg_m.group(1), arg_m.group(2)
        call_end_line = line_index_for(close_paren)

        if prev_end_line is not None and call_start_line - prev_end_line <= 1:
            description = prev_description
        else:
            description = extract_comment_above(lines, call_start_line)

        calls.append((handler_var, signature, description))
        prev_end_line = call_end_line
        prev_description = description
    return calls


def parse_version(text):
    """Returns the "major.minor" PTZ_API_VERSION_* defines in text."""
    parts = []
    for name in ("MAJOR", "MINOR"):
        m = re.search(rf"^#define\s+PTZ_API_VERSION_{name}\s+(\d+)\s*$", text, re.MULTILINE)
        if not m:
            raise ValueError(f"PTZ_API_VERSION_{name} not found in {VERSION_HEADER}")
        parts.append(m.group(1))
    return ".".join(parts)


def render(calls, version):
    major = version.split(".")[0]
    out = ["# PTZDevice proc_handler / signal_handler API", "",
           "Generated by `scripts/gen-api-docs.py` from the `proc_handler_add()`/",
           "`signal_handler_add()` call sites in `src/ptz-device.cpp` -- do not edit by",
           "hand, regenerate it instead. See [AGENTS.md](../AGENTS.md) for the design",
           "this API is part of.", "",
           "## API version", "",
           f"This is version **{version}** of the PTZ API. A caller asks the plugin",
           "which version it implements with `ptz_get_api_version` on OBS's own",
           "proc_handler, before relying on anything else here:", "",
           "```c",
           "calldata_t cd = {0};",
           "if (proc_handler_call(obs_get_proc_handler(), \"ptz_get_api_version\", &cd)) {",
           "\tlong long major = calldata_int(&cd, \"major\");",
           "\tlong long minor = calldata_int(&cd, \"minor\");",
           "}",
           "calldata_free(&cd);",
           "```", "",
           "The version covers the procs and signals below, and the calldata fields,",
           "state keys and trigger names they take. The minor version goes up when",
           "something is added, which an existing caller can't notice; the major",
           "version goes up, and the minor goes back to 0, when something is removed,",
           "renamed or changes meaning. So a caller written for version M.m works with",
           "any version M.n where n >= m, and isn't promised to with any other. For",
           f"example, a caller written for {major}.0 works with any {major}.x.", "",
           "If the call fails, the plugin predates API versioning, and its API isn't",
           "the one described here.", "",
           "Each device reports its own version too, with `ptz_get_api_version` on",
           "its own proc_handler. Another plugin can implement the per-device API,",
           "at another version than this plugin, so a caller checks each device's",
           "version, by the same rules, before relying on anything else that device",
           "has. A device with no `ptz_get_api_version` predates API versioning.", ""]

    for var, title, blurb in SECTIONS:
        entries = [(sig, desc) for (v, sig, desc) in calls if v == var]
        if not entries:
            continue
        out.append(f"## {title}")
        out.append("")
        out.append(f"_{blurb}._")
        out.append("")
        last_desc = object()  # sentinel that won't equal any real description
        for sig, desc in entries:
            if desc and desc != last_desc:
                if out[-1] != "":
                    out.append("")
                out.append(f"> {desc}")
                out.append("")
                last_desc = desc
            out.append(f"- `{sig}`")
        out.append("")
    return "\n".join(out).rstrip() + "\n"


def main():
    text = SOURCE.read_text()
    calls = parse_calls(text)
    OUTPUT.parent.mkdir(exist_ok=True)
    version = parse_version(VERSION_HEADER.read_text())
    OUTPUT.write_text(render(calls, version))
    print(f"wrote {OUTPUT.relative_to(REPO_ROOT)} ({len(calls)} entries)")


if __name__ == "__main__":
    main()
