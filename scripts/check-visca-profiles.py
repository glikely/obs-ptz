#!/usr/bin/env python3
"""Checks that the VISCA command sets in src/visca-profiles are laid out as
scripts/visca-profile-gen writes them (json.dumps(indent=2), and a newline
at the end), so that a regenerated one changes only what changed in it, and
a hand-written one reads like the rest.

What is in them is checked by tests/visca-profile-check, with the plugin's
own reader, on CI's Ubuntu builds.

Usage: python3 scripts/check-visca-profiles.py [--fix]
"""
import json
import sys
from pathlib import Path

PROFILES = Path(__file__).resolve().parent.parent / "src" / "visca-profiles"


def main():
    fix = "--fix" in sys.argv[1:]
    wrong = []
    for path in sorted(PROFILES.glob("*.json")):
        text = path.read_text()
        try:
            laid_out = json.dumps(json.loads(text), indent=2) + "\n"
        except json.JSONDecodeError as e:
            print(f"{path.name}: isn't JSON: {e}", file=sys.stderr)
            wrong.append(path)
            continue
        if text == laid_out:
            continue
        if fix:
            path.write_text(laid_out)
            print(f"{path.name}: laid out again")
        else:
            print(f"{path.name}: isn't laid out as json.dumps(indent=2) does it", file=sys.stderr)
            wrong.append(path)
    if wrong and not fix:
        print("Run python3 scripts/check-visca-profiles.py --fix to lay them out", file=sys.stderr)
    return 1 if wrong else 0


if __name__ == "__main__":
    sys.exit(main())
