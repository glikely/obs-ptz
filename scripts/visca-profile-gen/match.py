"""Whether a camera's documented packets match the generic command set's,
nibble by nibble: where ours writes a value, theirs must have a parameter"""
import re
from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / "src" / "ptz-visca-commands.cpp"

# nibbles each field type writes, as (byte offset from the field's, which:
# "lo" the low nibble only, "both" both nibbles, "any" a fixed byte that
# varies by row, such as a direction)
FIELD_NIBBLES = {
    "visca_u4": [(0, "lo")],
    "visca_u7": [(0, "both7")],
    "visca_u8": [(0, "lo"), (1, "lo")],
    "visca_u15": [(0, "both7"), (1, "both7")],
    "visca_u16": [(0, "lo"), (1, "lo"), (2, "lo"), (3, "lo")],
    "visca_s16": [(0, "lo"), (1, "lo"), (2, "lo"), (3, "lo")],
    "visca_u16_high": [(0, "lo"), (1, "lo")],
    "visca_flag": [(0, "lo")],
    "visca_s4": [(0, "lo-dir")],
    "visca_s7": [(0, "both7"), (2, "any")],
}


def field_nibbles(ftype, offset, mask=None):
    if ftype in FIELD_NIBBLES:
        return [(offset + d, how) for d, how in FIELD_NIBBLES[ftype]]
    # int_field / bool_field / string_lookup_field: by mask, low byte last
    out = []
    nbytes = max(1, (mask.bit_length() + 7) // 8)
    for i in range(nbytes):
        b = (mask >> (8 * (nbytes - 1 - i))) & 0xff
        if b & 0xf0 and b & 0x0f:
            out.append((offset + i, "both"))
        elif b & 0xf0:
            out.append((offset + i, "hi"))
        elif b & 0x0f:
            out.append((offset + i, "lo"))
    return out


def load_fields():
    """name -> the nibbles its fields write, for every definition"""
    s = open(SRC).read()
    out = {}
    for m in re.finditer(r'const (?:PTZCmd|PTZInq) (\w+)\(\s*"([0-9a-f]+)"(.*?)\);\n', s, re.S):
        name, body = m.group(1), m.group(3)
        nib = []
        for f in re.finditer(r'new (\w+)\("(\w+)",(?: PTZVisca::\w+,)? (\d+)(?:, (0x[0-9a-f]+|0b[01]+))?', body):
            mask = int(f.group(4), 0) if f.group(4) else None
            nib += [(o, h, f.group(2)) for o, h in field_nibbles(f.group(1), int(f.group(3)), mask)]
        out[name] = nib
    return out


def nibble_ok(doc, ours, how):
    """doc: the documented token, ours: the byte we send there"""
    hi, lo = doc[0], doc[1]
    letter = lambda c: c not in "0123456789ABCDEF"
    if how == "fixed":
        return (letter(hi) or hi == ours[0]) and (letter(lo) or lo == ours[1])
    if how == "any":
        return True
    if how == "lo":
        return letter(lo) and (letter(hi) or hi == ours[0])
    if how == "hi":
        return letter(hi)
    if how == "both":  # a byte, whose values can all be under 0x10
        return (letter(hi) or hi == "0") and letter(lo)
    if how == "both7":  # 7 bits: the high nibble may be shown as 0 for small ranges
        return letter(lo)
    if how == "lo-dir":  # a speed, and a direction in the high nibble
        return letter(lo)
    raise ValueError(how)


def matches(doc_tokens, hexs, written, loose=False):
    """Whether a documented packet is our command `hexs`, whose arguments
    write `written` [(byte, how)]. If `loose`, one with a value where we
    send one, rather than a parameter, is too."""
    ours = [hexs[i:i + 2].upper() for i in range(0, len(hexs), 2)]
    doc_tokens = [t if i else "8X" for i, t in enumerate(doc_tokens)]
    if len(doc_tokens) != len(ours):
        return False
    hows = {o: h for o, h, _ in written}
    for i, (d, o) in enumerate(zip(doc_tokens, ours)):
        if i == 0:
            continue  # the address
        if loose and i in hows:
            continue
        if not nibble_ok(d, o, hows.get(i, "fixed")):
            return False
    return True


def reply_ok(doc_reply, written, block=False):
    """Whether a documented reply has a parameter where each of our results
    reads one, and is long enough. In a reply anything but a digit is a
    parameter: tables name them A, B, C... A block inquiry's reply is bit
    packed, which the tables don't show reliably: only its length is
    checked."""
    if block:
        return True
    if not written:
        return True
    # A value can be fixed ("on only"), so values aren't checked, only that
    # the reply is long enough, and that none of them is wider than ours: the
    # byte after one has another parameter (a 5 digit pan position has the
    # same one again)
    if len(doc_reply) < max(o for o, _, _ in written) + 2:
        return False
    last, nbytes = {}, {}
    for o, how, key in written:
        last[key] = max(last.get(key, o), o)
        nbytes[key] = nbytes.get(key, 0) + (how == "lo")
    for key, o in last.items():
        if nbytes[key] < 2:
            continue  # a bit field, which tables name with one letter
        tok, nxt = doc_reply[o], doc_reply[o + 1]
        letters = lambda t: {c for c in t if not c.isdigit()}
        if letters(tok) and letters(tok) & letters(nxt) and nxt.upper() != "FF":
            return False
    return True
    if len(doc_reply) < max(o for o, _, _ in written) + 2:
        return False
    for o, how, _ in written:
        hi, lo = doc_reply[o][0], doc_reply[o][1]
        if how in ("lo", "both7", "lo-dir") and lo.isdigit():
            return False
        if how == "both" and hi.isdigit() and lo.isdigit():
            return False
        if how == "hi" and hi.isdigit():
            return False
    return True
