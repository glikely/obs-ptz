#!/usr/bin/env python3
"""Checks that the controller, the device core and the drivers in src/ don't
include each other's headers, so that each can be a module of its own and
talk to the others only through the PTZ API.

Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
SPDX-License-Identifier: GPL-2.0-or-later

The rules, by directory under src/:
- controller/ includes neither core/ nor drivers/
- core/ includes neither controller/ nor drivers/
- drivers/<name>/ includes core/ but not controller/ or another driver,
  except that drivers/visca/ and drivers/pelco/ include drivers/serial/
- ptz.h, at the top of src/, may be included by anyone, and includes none
  of them
- ptz-module-all.cpp, which loads them all while they are still one module,
  may include anything

An include with a directory in it ("../core/x.hpp") is found relative to
the file that includes it; otherwise by its name, as each library has its
own directory on the include path, so two files on different sides may not
share a name. One in a comment or an #if is still checked. A <...> include
that isn't under src/ is a system, Qt or OBS header and is left alone; a
"..." one has to be found under src/ or shared/, or be generated (ui_*.h,
moc_*, *.moc), so that a typo can't hide a crossing.

Usage: python3 scripts/check-module-boundaries.py [--root DIR]
Prints file:line for each crossing or include it can't find and exits 1,
or exits 0.
"""
import argparse
import fnmatch
import os
import re
import sys
from pathlib import Path

SOURCES = {".c", ".cpp", ".h", ".hpp", ".m", ".mm"}
INCLUDE = re.compile(r'#\s*include\s*([<"])([^>"]+)[>"]')
# Who may include them, and what they may include
SHARED_HEADERS = {"ptz.h"}
INCLUDE_ANYTHING = {"ptz-module-all.cpp"}
SERIAL_USERS = {"visca", "pelco"}
# "..." includes that are made by the build, not found in the tree
GENERATED = ["ui_*.h", "moc_*", "*.moc"]


def side(rel):
    """The part of the tree a path under src/ is in"""
    parts = rel.parts
    if len(parts) > 2 and parts[0] == "drivers":
        return f"drivers/{parts[1]}"
    if len(parts) > 1:
        return parts[0]
    return "src"


def allowed(src, dst, header):
    if src == dst or (dst == "src" and header in SHARED_HEADERS):
        return True
    if src.startswith("drivers/"):
        return dst == "core" or (dst == "drivers/serial" and src.split("/")[1] in SERIAL_USERS)
    return False


class Tree:
    def __init__(self, root):
        self.root = root
        self.src = root / "src"
        self.files = sorted(p for p in self.src.rglob("*") if p.is_file() and p.suffix in SOURCES)
        self.by_name = {}
        for p in self.files:
            self.by_name.setdefault(p.name, []).append(p)
        shared = root / "shared"
        self.external = {p.name for p in shared.rglob("*") if p.is_file()} if shared.is_dir() else set()

    def duplicates(self):
        """Names that files on more than one side share"""
        out = []
        for name, paths in sorted(self.by_name.items()):
            sides = {side(p.relative_to(self.src)) for p in paths}
            if len(sides) > 1:
                where = ", ".join(p.relative_to(self.root).as_posix() for p in paths)
                out.append(f"duplicate name {name}: {where}")
        return out

    def resolve(self, name, includer):
        """The file under src/ an include names, "external" for one outside
        it, or None if it can't be found"""
        if "/" in name:
            path = Path(os.path.normpath(includer.parent / name))
            if path.is_file():
                try:
                    path.relative_to(self.src)
                except ValueError:
                    return "external"
                return path
            if name.startswith("."):
                return None
        norm = os.path.normpath(name)
        candidates = [c for c in self.by_name.get(Path(norm).name, []) if c.as_posix().endswith(norm)]
        for c in candidates:
            if c.parent == includer.parent:
                return c
        if candidates:
            return candidates[0]
        return None

    def generated(self, name):
        base = Path(name).name
        return any(fnmatch.fnmatch(base, g) for g in GENERATED)


def check(root):
    tree = Tree(root)
    if not tree.files:
        raise SystemExit(f"error: no sources found under {tree.src}")
    problems = tree.duplicates()
    for path in tree.files:
        rel = path.relative_to(tree.src)
        if rel.as_posix() in INCLUDE_ANYTHING:
            continue
        src = side(rel)
        where = path.relative_to(root).as_posix()
        for lineno, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            for m in INCLUDE.finditer(line):
                quote, name = m.group(1), m.group(2)
                target = tree.resolve(name, path)
                if target == "external":
                    continue
                if target is None:
                    if quote == "<" or tree.generated(name) or Path(name).name in tree.external:
                        continue
                    problems.append(f"{where}:{lineno}: unresolved include {name}")
                    continue
                dst = side(target.relative_to(tree.src))
                if not allowed(src, dst, target.name):
                    problems.append(f"{where}:{lineno}: includes {name} ({src} -> {dst})")
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent,
                        help="the checkout whose src/ to check (default: this one)")
    args = parser.parse_args()
    root = args.root.resolve()
    if not (root / "src").is_dir():
        print(f"error: {root} has no src/ directory", file=sys.stderr)
        return 2
    problems = check(root)
    for p in problems:
        print(p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
