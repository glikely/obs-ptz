"""Which of the generic command set's definitions a camera's documented
packets have"""
import generic
import match


def supported(commands, inquiries):
    """commands: [tokens], inquiries: [(tokens, reply tokens or None)].
    Returns the set of definition names the camera has."""
    fields = match.load_fields()
    defs, controls, actions, triggers = generic.load()
    have = set()
    for name, hexs in defs.items():
        written = fields.get(name, [])
        if hexs[2:4] == "09":
            for inq, reply in inquiries:
                block = hexs.startswith("81097e7e")
                if match.matches(inq, hexs, []) and (reply is None or match.reply_ok(reply, written, block)):
                    have.add(name)
                    break
        else:
            for cmd in commands:
                if match.matches(cmd, hexs, written):
                    have.add(name)
                    break
    return have


def profile(have, **meta):
    """A command set extending the generic one, without what isn't in `have`"""
    defs, controls, actions, triggers = generic.load()
    used_inqs = {r for c in controls for r in c["reads"]}
    out = dict(meta)
    remove, changed = [], []
    for c in controls:
        reads = [r for r in c["reads"] if r in have]
        set_ok = c["set"] in have if c["set"] else False
        set_to = {v: n for v, n in c["set_to"].items() if n in have}
        if not reads and not set_ok and not set_to:
            remove.append(c["key"])
            continue
        entry = {}
        if c["set"] and not set_ok:
            entry["set"] = None
        if c["set_to"] and set_to != c["set_to"]:
            if set_to:
                entry["set_to"] = {v: defs[n] for v, n in set_to.items()}
            else:
                entry["set"] = None
        if entry:
            changed.append({"key": c["key"], **entry})
    remove += [a for a, n in actions.items() if n not in have]
    remove += [t for t, n in triggers.items() if n not in have]
    out["remove_inquiries"] = sorted({defs[n] for n in used_inqs if n not in have})
    out["remove"] = remove
    if changed:
        out["controls"] = changed
    return out
