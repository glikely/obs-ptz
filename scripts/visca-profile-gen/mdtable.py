"""Packets in the markdown tables of a section of a document, such as
grafton-visca's docs/visca_reference.md"""
import itertools
import re

TOKEN = re.compile(r"^([0-9A-Za-z]{1,2})(/[0-9A-Za-z]{1,2})*$")


def section(path, first, last):
    """The lines from the heading that starts with `first` up to the one
    that starts with `last`"""
    lines = open(path).read().splitlines()
    start = next(i for i, l in enumerate(lines) if l.lstrip("#").strip().startswith(first))
    end = next(i for i, l in enumerate(lines[start + 1:], start + 1) if l.lstrip("#").strip().startswith(last))
    return lines[start:end]


def expand(code):
    """The packets a quoted packet stands for: `81 01 04 03 00/02/03 FF` is
    three. None if it isn't one."""
    tokens = code.split()
    if not tokens or tokens[-1].upper() != "FF" or not all(TOKEN.match(t) for t in tokens):
        return None
    choices = [t.split("/") for t in tokens]
    out = []
    for combo in itertools.product(*choices):
        # "X 09 00 02 FF": the address, as one character
        combo = ["8X" if i == 0 and len(t) == 1 else t for i, t in enumerate(combo)]
        if all(len(t) == 2 for t in combo):
            out.append(list(combo))
    return out


def rows(lines):
    for line in lines:
        if not line.startswith("|") or set(line) <= set("|-: "):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        yield [[p for code in re.findall(r"`([^`]*)`", c) for p in (expand(code) or [])] for c in cells]


def packets(lines):
    """commands, and inquiries with their replies (None where a table
    gives none), as the tables in `lines` have them"""
    commands, inquiries = [], []
    for cells in rows(lines):
        pkts = [p for cell in cells for p in cell]
        inq = [p for p in pkts if len(p) > 2 and p[1] == "09"]
        replies = [p for p in pkts if p[1] == "50"]
        if inq:
            inquiries += [(q, replies[0] if len(replies) == 1 else None) for q in inq]
        else:
            commands += [p for p in pkts if len(p) > 2 and p[1] == "01"]
    return commands, inquiries
