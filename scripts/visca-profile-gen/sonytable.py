"""Rows of the tables in the Sony protocol HTML files, as lists of cell text"""
import re
from html.parser import HTMLParser

PACKET = re.compile(r'^[0-9A-Za-z]{2}( [0-9A-Za-z]{2})* (FF|ff)$')


class Rows(HTMLParser):
    def __init__(self):
        super().__init__()
        self.rows, self.row, self.cell, self.heading = [], None, None, ""
        self.in_h = False

    def handle_starttag(self, tag, attrs):
        if tag == "tr":
            self.row = []
        elif tag in ("td", "th"):
            self.cell = []
        elif tag == "br" and self.cell is not None:
            self.cell.append("\n")
        elif tag in ("h1", "h2", "h3", "h4"):
            self.in_h, self.heading = True, ""

    def handle_endtag(self, tag):
        if tag in ("td", "th") and self.row is not None and self.cell is not None:
            self.row.append(re.sub(r"[ \t]+", " ", "".join(self.cell)).strip())
            self.cell = None
        elif tag == "tr" and self.row is not None:
            self.rows.append((self.heading, self.row))
            self.row = None
        elif tag in ("h1", "h2", "h3", "h4"):
            self.in_h = False

    def handle_data(self, data):
        if self.cell is not None:
            self.cell.append(data)
        if self.in_h:
            self.heading += data.strip()


def rows(path):
    p = Rows()
    p.feed(open(path).read())
    return p.rows


def packets(cell):
    """The packets in a cell, one per line, normalised to upper-case tokens"""
    out = []
    for line in cell.split("\n"):
        line = re.sub(r"\s+", " ", line).strip()
        if PACKET.match(line):
            out.append(line.split(" "))
    return out


if __name__ == "__main__":
    import sys
    for h, r in rows(sys.argv[1]):
        print(h, "||", " | ".join(c.replace("\n", " / ") for c in r))
