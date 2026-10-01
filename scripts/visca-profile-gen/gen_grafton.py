"""Writes the PTZOptics and Axis command sets in src/visca-profiles from the
command tables in grafton-visca's consolidated VISCA reference. Run with the
path of a checkout of https://github.com/GrantSparks/grafton-visca:

    python3 scripts/visca-profile-gen/gen_grafton.py ../grafton-visca
"""
import json
import sys
from pathlib import Path

import mdtable
import support

REFERENCE = Path(sys.argv[1]) / "docs" / "visca_reference.md"
OUT = Path(__file__).resolve().parents[2] / "src" / "visca-profiles"
SRC = ("Generated from the command tables in grafton-visca's VISCA reference "
       "(github.com/GrantSparks/grafton-visca, docs/visca_reference.md {sections}, "
       "MIT or Apache-2.0 licensed), not yet verified against a camera.")
PROFILES = [
    # Their version reply's vendor and model ID bytes are a factory code
    # and hardware and firmware versions, which say nothing reliable about
    # the camera: it can only be chosen by hand
    ("ptzoptics-gen2", "PTZOptics Gen-2 NDI (PT12X/PT20X/PT30X-NDI)", [],
     "7. PTZOptics validated baseline", "7.12", "section 7"),
    # "AXV" and the product number, for every Axis camera
    ("axis", "Axis", ["4158:*"], "8. Axis VISCA profile", "8.6", "section 8"),
]
for pid, name, models, first, last, sections in PROFILES:
    commands, inquiries = mdtable.packets(mdtable.section(REFERENCE, first, last))
    prof = support.profile(support.supported(commands, inquiries), id=pid, name=name, models=models,
                           source=SRC.format(sections=sections))
    (OUT / (pid + ".json")).write_text(json.dumps(prof, indent=2) + "\n")
    print(pid, len(prof["remove"]), len(prof["remove_inquiries"]), len(prof.get("controls", [])))
