"""Sony command sets, from the protocol tables in Bitfocus' Sony VISCA
Companion module (protocol/Sony_*.html, MIT licensed)"""
import json
import sys
import sonytable
import support


def packets(path, dagger=True):
    """dagger: whether to have what the table marks with a dagger, which is
    only some of the models it is for"""
    commands, inquiries = [], []
    for heading, row in sonytable.rows(path):
        if not dagger and any("\u2020" in c for c in row):
            continue
        pkts = [p for cell in row for p in sonytable.packets(cell)]
        inq = [p for p in pkts if len(p) > 2 and p[1] == "09"]
        if inq:
            reply = next((p for p in pkts if p[0].upper().startswith("Y") and p[1] == "50"), None)
            inquiries += [(q, reply) for q in inq]
        else:
            commands += [p for p in pkts if len(p) > 2 and p[1] == "01"]
    return commands, inquiries


if __name__ == "__main__":
    c, i = packets(sys.argv[1])
    have = support.supported(c, i)
    print(len(c), len(i), file=sys.stderr)
    print(json.dumps(support.profile(have), indent=2))
