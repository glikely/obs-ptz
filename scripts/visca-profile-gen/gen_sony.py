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
for pid, name, models, doc, dagger, note in PROFILES:
    c, i = sony.packets(P + doc, dagger)
    prof = support.profile(support.supported(c, i), id=pid, name=name,
                           models=["0001:" + m.lower() for m in models],
                           source=SRC.format(doc=doc, note=note))
    open(OUT + pid + ".json", "w").write(json.dumps(prof, indent=2) + "\n")
    print(pid, len(prof["remove"]), len(prof["remove_inquiries"]), len(prof.get("controls", [])))
