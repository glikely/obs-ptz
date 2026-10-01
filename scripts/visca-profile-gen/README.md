# visca-profile-gen

Generates VISCA command sets (`src/visca-profiles/*.json`) from command
tables published elsewhere, rather than writing them by hand. Python 3
standard library only.

A camera's command set is the generic one (`src/ptz-visca-commands.cpp`)
without what the camera's table doesn't have: each command, inquiry,
action and trigger the generic one uses is looked for in the table, nibble
by nibble, where the generic one sends a value the table must have a
parameter (or rows for at least two values there, as some tables list
"on" and "off" separately), and an inquiry's reply must be as long as the values the
generic one reads from it, and none of them wider. Block inquiries' replies
are bit packed, which tables don't show reliably, so only that the camera
has them is checked. What the table has that the generic one doesn't isn't
added.

- `generic.py`: the generic command set's controls, actions and triggers
- `match.py`: whether a table's packet is one of them
- `support.py`: which of them a table has, and the command set without the rest
- `sonytable.py`, `sony.py`: Sony's tables, as Bitfocus' Sony VISCA
  Companion module has them (`protocol/Sony_*.html`, MIT licensed)
- `gen_sony.py PATH`: writes the Sony command sets, from a checkout of
  https://github.com/bitfocus/companion-module-sony-visca at PATH
- `mdtable.py`: the packets in markdown tables
- `gen_grafton.py PATH`: writes the PTZOptics Gen-2 and Axis command sets
  from grafton-visca's consolidated VISCA reference (`docs/visca_reference.md`,
  MIT or Apache-2.0 licensed), from a checkout of
  https://github.com/GrantSparks/grafton-visca at PATH

Look over what changes before committing it: a table can be wrong, and a
generated command set hasn't been tried on a camera.
