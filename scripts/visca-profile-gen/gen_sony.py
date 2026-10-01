"""Writes the Sony command sets in src/visca-profiles from the protocol tables
in Bitfocus' Sony VISCA Companion module. Run with the path of a checkout of
https://github.com/bitfocus/companion-module-sony-visca:

    python3 scripts/visca-profile-gen/gen_sony.py ../companion-module-sony-visca
"""
import json
import sys
from pathlib import Path

import sony
import support

P = str(Path(sys.argv[1]) / "protocol") + "/"
OUT = str(Path(__file__).resolve().parents[2] / "src" / "visca-profiles") + "/"
SRC = ("Generated from the protocol tables in Bitfocus' Sony VISCA Companion module "
       "(github.com/bitfocus/companion-module-sony-visca, protocol/{doc}, MIT licensed), "
       "{note}not yet verified against a camera.")
PROFILES = [
    ("sony-srg-120dh", "Sony SRG-120DH", ["0511"], "Sony_SRG-120DH.html", True, ""),
    ("sony-srg-300h", "Sony SRG-300H, NewTek PTZ1 NDI", ["0513"], "Sony_SRG-300H.html", True, ""),
    ("sony-srg-300se", "Sony SRG-300SE/301SE/201SE", ["0516"], "Sony_SRG-300SE.html", True, ""),
    ("sony-srg-360she", "Sony SRG-360SHE/280SHE", ["0604", "0605"], "Sony_SRG-360SHE.html", True, ""),
    ("sony-brc-x400", "Sony BRC-X400/X401", ["051C", "051D"], "Sony_BRC-X400.html", True, ""),
    ("sony-srg-x400", "Sony SRG-X400/X402/X120/201M2/HD1M2", ["0617", "061C", "0618", "061A", "061B"],
     "Sony_BRC-X400.html", False, "without what it marks as for the BRC-X400 and BRC-X401 only; "),
    ("sony-brc-x1000", "Sony BRC-X1000/H800/H780", ["0519", "051A", "051B"], "Sony_BRC-X1000.html", False,
     "without what it marks as not for the BRC-H780; "),
    ("sony-srg-x40uh", "Sony SRG-X40UH/H40UH", ["061F", "0620"], "Sony_SRG-X40UH.html", True, ""),
    ("sony-srg-a40", "Sony SRG-A40/A12", ["0621", "0622"], "Sony_SRG-A40.html", True, ""),
    ("sony-ilme-fr7", "Sony ILME-FR7", ["051E"], "Sony_ILME-FR7.html", True, ""),
    ("sony-brc-am7", "Sony BRC-AM7", ["051F"], "Sony_BRC-AM7.html", True, ""),
]


def h(n):
    return ("-" if n < 0 else "") + hex(abs(n))


def ranges(pan, tilt, zoom=0x4000):
    """From the tables, with the image flip or ceiling mode off; zoom to the
    optical tele end"""
    return {"pan": [h(pan[0]), h(pan[1])], "tilt": [h(tilt[0]), h(tilt[1])], "zoom": [h(0), h(zoom)]}


def position(pan, tilt):
    """Pan and tilt positions of `pan` and `tilt` nibbles"""
    t = {4: "s16", 5: "s20"}
    return [{"type": t[pan], "offset": 2, "key": "pan_pos"}, {"type": t[tilt], "offset": 2 + pan, "key": "tilt_pos"}]


def pantilt(op, pan, tilt, tilt_speed=True):
    """An absolute (op 2) or relative (3) move, with a tilt speed or not, and
    positions of `pan` and `tilt` nibbles"""
    fields = position(pan, tilt)
    for f in fields:
        f["offset"] += 4
        del f["key"]
    speeds = [{"type": "u7", "offset": 4}, {"type": "u7", "offset": 5} if tilt_speed else {"type": "none", "offset": 5}]
    return {"cmd": "810106%02x0000%sff" % (op, "00" * (pan + tilt)), "args": speeds + fields}


def moves(pan, tilt, tilt_speed=True):
    """A camera's own absolute and relative moves, and the position inquiry
    for them if its positions aren't 4 nibbles each"""
    out = {"actions": {"pantilt_abs": pantilt(2, pan, tilt, tilt_speed),
                       "pantilt_rel": pantilt(3, pan, tilt, tilt_speed)}}
    if (pan, tilt) != (4, 4):
        read = {"cmd": "81090612ff", "results": position(pan, tilt)}
        out["controls"] = [{"key": k, "reads": [read]} for k in ("pan_pos", "tilt_pos")]
    return out


SONY_PAN, SONY_TILT = (-0x2200, 0x2200), (-0x400, 0x1200)
# What a table has that the generic command set has another way
EXTRA = {
    "sony-srg-120dh": {"ranges": ranges((-0x1400, 0x1400), (-0x500, 0x500))},
    "sony-srg-300h": {"ranges": ranges(SONY_PAN, SONY_TILT)},
    "sony-srg-300se": {"ranges": ranges(SONY_PAN, SONY_TILT)},
    "sony-srg-360she": {"ranges": ranges((-0x15400, 0x15400), (-0x3c00, 0xb400)), **moves(5, 5, False)},
    "sony-brc-x400": {"ranges": ranges(SONY_PAN, SONY_TILT), **moves(4, 4, False)},
    "sony-srg-x400": {"ranges": ranges(SONY_PAN, SONY_TILT), **moves(4, 4, False)},
    # pan positions run right to left
    "sony-brc-x1000": {"ranges": ranges((0x9ca7, -0x9ca7), (-0x1ba5, 0x52ef)), **moves(5, 4)},
    "sony-srg-x40uh": {"ranges": ranges(SONY_PAN, SONY_TILT), **moves(4, 4, False)},
    "sony-srg-a40": {"ranges": ranges(SONY_PAN, SONY_TILT), **moves(4, 4, False)},
    "sony-ilme-fr7": {"ranges": ranges((-0x9ca7, 0x9ca7), (-0x1ba5, 0xb3b0)), **moves(5, 5)},
    "sony-brc-am7": {"ranges": ranges((-0x2ab98, 0x2ab98), (-0x7530, 0x33450)), **moves(5, 5)},
}

for pid, name, models, doc, dagger, note in PROFILES:
    c, i = sony.packets(P + doc, dagger)
    prof = support.profile(support.supported(c, i), id=pid, name=name,
                           models=["0001:" + m.lower() for m in models],
                           source=SRC.format(doc=doc, note=note))
    prof = support.merge(prof, EXTRA.get(pid, {}))
    open(OUT + pid + ".json", "w").write(json.dumps(prof, indent=2) + "\n")
    print(pid, len(prof["remove"]), len(prof["remove_inquiries"]), len(prof.get("controls", [])))
