"""Rooms ptzsim draws itself: no download, no licence, the same for everyone.
A room is built from boxes, cylinders and ring sectors into a .glb in the
cache, which --scene (three.js) and --ndi (pygfx) both read like any other glTF.
They are plain: flat colours, no textures. Units are metres, y up, the floor
at y=0, and pan 0 looks along -Z. Each has coloured markers on its walls so
that pan, tilt, zoom and focus can be judged.

    lecture-hall   a tiered lecture theatre: stage, whiteboard, ten rows of seats
    arena          an indoor arena: a court in a bowl of stands, a scoreboard
    sanctuary      a church nave: chancel and altar, pews, stained glass, a vaulted roof
"""

import json
import math
import os
import struct

VERSION = 1     # bump when a room changes, so the cached .glb is made again


class Builder:
    """Triangles by material, as flat-shaded quads"""

    def __init__(self):
        self.materials = {}     # name -> dict(color, emissive, roughness)
        self.geometry = {}      # material name -> ([x, y, z, ...], [nx, ny, nz, ...], [indices])

    def material(self, name, color, emissive=None, roughness=0.9):
        self.materials[name] = {"color": color, "emissive": emissive, "roughness": roughness}
        return name

    def quad(self, material, a, b, c, d):
        """a, b, c, d going round: the normal follows the right-hand rule. Materials are
        double-sided, so which way it faces only changes the lighting a little."""
        ux, uy, uz = (b[i] - a[i] for i in range(3))
        vx, vy, vz = (c[i] - a[i] for i in range(3))
        n = (uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx)
        length = math.sqrt(sum(v * v for v in n)) or 1.0
        n = tuple(v / length for v in n)
        positions, normals, indices = self.geometry.setdefault(material, ([], [], []))
        base = len(positions) // 3
        for p in (a, b, c, d):
            positions.extend(p)
            normals.extend(n)
        indices.extend((base, base + 1, base + 2, base, base + 2, base + 3))

    # ---- shapes

    def box(self, material, center, size, turn=0.0):
        """A box of size (x, y, z) about center, turned `turn` degrees about the vertical"""
        cx, cy, cz = center
        hx, hy, hz = (s / 2 for s in size)
        s, c = math.sin(math.radians(turn)), math.cos(math.radians(turn))

        def p(x, y, z):
            return (cx + x * c + z * s, cy + y, cz - x * s + z * c)
        corners = {(i, j, k): p(i * hx, j * hy, k * hz) for i in (-1, 1) for j in (-1, 1) for k in (-1, 1)}
        for face in (((-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1)),      # -z
                     ((1, -1, 1), (-1, -1, 1), (-1, 1, 1), (1, 1, 1)),          # +z
                     ((-1, -1, 1), (-1, -1, -1), (-1, 1, -1), (-1, 1, 1)),      # -x
                     ((1, -1, -1), (1, -1, 1), (1, 1, 1), (1, 1, -1)),          # +x
                     ((-1, 1, -1), (1, 1, -1), (1, 1, 1), (-1, 1, 1)),          # top
                     ((-1, -1, 1), (1, -1, 1), (1, -1, -1), (-1, -1, -1))):     # bottom
            self.quad(material, *(corners[k] for k in face))

    def ring_sector(self, material, center, r0, r1, y0, y1, a0, a1, segments=None):
        """Part of a ring (a0 to a1 degrees about the vertical; 0 is -Z, clockwise from above),
        from radius r0 to r1 and height y0 to y1. r0 = 0 is a wedge, 0 to 360 a disc or ring."""
        cx, cz = center[0], center[1]
        segments = segments or max(1, int(abs(a1 - a0) / 7.5))
        full = abs(abs(a1 - a0) - 360) < 1e-6

        def at(r, a, y):
            ar = math.radians(a)
            return (cx + r * math.sin(ar), y, cz - r * math.cos(ar))
        for i in range(segments):
            s0 = a0 + (a1 - a0) * i / segments
            s1 = a0 + (a1 - a0) * (i + 1) / segments
            self.quad(material, at(r1, s0, y0), at(r1, s1, y0), at(r1, s1, y1), at(r1, s0, y1))      # outside
            if r0 > 0:
                self.quad(material, at(r0, s1, y0), at(r0, s0, y0), at(r0, s0, y1), at(r0, s1, y1))  # inside
            self.quad(material, at(r0, s0, y1), at(r1, s0, y1), at(r1, s1, y1), at(r0, s1, y1))      # top
            self.quad(material, at(r0, s1, y0), at(r1, s1, y0), at(r1, s0, y0), at(r0, s0, y0))      # bottom
        if not full:
            self.quad(material, at(r0, a0, y0), at(r1, a0, y0), at(r1, a0, y1), at(r0, a0, y1))
            self.quad(material, at(r1, a1, y0), at(r0, a1, y0), at(r0, a1, y1), at(r1, a1, y1))

    def cylinder(self, material, center, radius, y0, y1, segments=16):
        self.ring_sector(material, center, 0, radius, y0, y1, 0, 360, segments)

    def vault(self, material, x_center, z0, z1, springing, radius, thickness=0.0, a0=-90, a1=90, segments=24):
        """A barrel vault along Z: half a cylinder of `radius` whose arc starts at height `springing`,
        seen from inside. thickness > 0 makes it a band standing that far out of the surface (a rib)."""
        def at(r, a, z):
            ar = math.radians(a)
            return (x_center + r * math.sin(ar), springing + r * math.cos(ar), z)
        r = radius - thickness
        for i in range(segments):
            s0 = a0 + (a1 - a0) * i / segments
            s1 = a0 + (a1 - a0) * (i + 1) / segments
            self.quad(material, at(r, s0, z0), at(r, s1, z0), at(r, s1, z1), at(r, s0, z1))
            if thickness > 0:       # the edges of a rib
                self.quad(material, at(radius, s0, z0), at(radius, s1, z0), at(r, s1, z0), at(r, s0, z0))
                self.quad(material, at(r, s0, z1), at(r, s1, z1), at(radius, s1, z1), at(radius, s0, z1))

    # ---- glTF

    def glb(self):
        names = list(self.geometry)
        materials, primitives = [], []
        binary = bytearray()
        views, accessors = [], []

        def add_view(data, target):
            while len(binary) % 4:
                binary.append(0)
            views.append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(data), "target": target})
            binary.extend(data)
            return len(views) - 1
        for index, name in enumerate(names):
            positions, normals, indices = self.geometry[name]
            spec = self.materials[name]
            material = {"name": name, "doubleSided": True,
                        "pbrMetallicRoughness": {"baseColorFactor": [*spec["color"], 1.0], "metallicFactor": 0.0,
                                                 "roughnessFactor": spec["roughness"]}}
            if spec["emissive"]:
                material["emissiveFactor"] = list(spec["emissive"])
            materials.append(material)
            count = len(positions) // 3
            lows = [min(positions[i::3]) for i in range(3)]
            highs = [max(positions[i::3]) for i in range(3)]
            accessors.append({"bufferView": add_view(struct.pack(f"<{len(positions)}f", *positions), 34962),
                              "componentType": 5126, "count": count, "type": "VEC3", "min": lows, "max": highs})
            accessors.append({"bufferView": add_view(struct.pack(f"<{len(normals)}f", *normals), 34962),
                              "componentType": 5126, "count": count, "type": "VEC3"})
            accessors.append({"bufferView": add_view(struct.pack(f"<{len(indices)}I", *indices), 34963),
                              "componentType": 5125, "count": len(indices), "type": "SCALAR"})
            primitives.append({"attributes": {"POSITION": index * 3, "NORMAL": index * 3 + 1},
                               "indices": index * 3 + 2, "material": index})
        document = {"asset": {"version": "2.0", "generator": "ptzsim genrooms"},
                    "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
                    "meshes": [{"primitives": primitives}], "materials": materials,
                    "accessors": accessors, "bufferViews": views,
                    "buffers": [{"byteLength": len(binary)}]}
        text = json.dumps(document, separators=(",", ":")).encode()
        text += b" " * (-len(text) % 4)
        binary.extend(b"\0" * (-len(binary) % 4))
        total = 12 + 8 + len(text) + 8 + len(binary)
        return (struct.pack("<4sII", b"glTF", 2, total) + struct.pack("<I4s", len(text), b"JSON") + text
                + struct.pack("<I4s", len(binary), b"BIN\0") + bytes(binary))


# ---- rooms ---------------------------------------------------------------------------------

MARKERS = ((0.85, 0.15, 0.15), (0.95, 0.7, 0.1), (0.2, 0.65, 0.25), (0.15, 0.4, 0.85))


def _markers(b, name, positions, size, turn=0.0):
    """Coloured posts, for something to judge pan and focus against"""
    for i, (x, y, z) in enumerate(positions):
        material = b.material(f"{name}{i % 4}", MARKERS[i % 4])
        b.box(material, (x, y, z), size, turn)


def lecture_hall():
    b = Builder()
    wall = b.material("wall", (0.82, 0.8, 0.74))
    floor = b.material("floor", (0.45, 0.36, 0.28))
    step = b.material("step", (0.5, 0.48, 0.45))
    ceiling = b.material("ceiling", (0.9, 0.9, 0.88))
    light = b.material("light", (1, 1, 0.95), emissive=(1.0, 0.98, 0.9))
    board = b.material("board", (0.95, 0.95, 0.95), emissive=(0.35, 0.35, 0.35))
    frame = b.material("frame", (0.25, 0.25, 0.27))
    wood = b.material("wood", (0.55, 0.38, 0.2))
    door = b.material("door", (0.2, 0.14, 0.1))
    seats = [b.material("seat-a", (0.12, 0.3, 0.55)), b.material("seat-b", (0.1, 0.5, 0.5))]
    x0, x1, front, back, top = -10.0, 10.0, -8.0, 14.0, 7.5
    depth = back - front
    b.box(floor, (0, -0.05, front + depth / 2), (x1 - x0, 0.1, depth))
    b.box(ceiling, (0, top + 0.05, front + depth / 2), (x1 - x0, 0.1, depth))
    for x in (x0, x1):
        b.box(wall, (x, top / 2, front + depth / 2), (0.2, top, depth))
    b.box(wall, (0, top / 2, front), (x1 - x0, top, 0.2))
    b.box(wall, (0, top / 2, back), (x1 - x0, top, 0.2))
    # The stage, whiteboard, lectern
    b.box(wood, (0, 0.15, front + 3.5), (x1 - x0 - 0.4, 0.3, 7.0))
    b.box(frame, (0, 2.6, front + 0.15), (9.0, 2.9, 0.1))
    b.box(board, (0, 2.6, front + 0.22), (8.7, 2.6, 0.04))
    b.box(wood, (3.5, 0.3 + 0.55, front + 3.0), (1.0, 1.1, 0.7))
    b.box(frame, (3.5, 0.3 + 1.15, front + 3.0), (0.9, 0.04, 0.6), 15)
    # Ten rows of seats, each a step higher, in two blocks either side of an aisle
    for row in range(10):
        z = 1.0 + row * 1.2
        y = 0.3 + row * 0.35
        b.box(step, (0, y / 2, z), (x1 - x0 - 0.4, y, 1.2))
        material = seats[row % 2]
        for block in (-1, 1):
            for col in range(10):
                x = block * (1.4 + col * 0.72)
                b.box(material, (x, y + 0.45, z), (0.55, 0.1, 0.5))
                b.box(material, (x, y + 0.8, z + 0.26), (0.55, 0.6, 0.08))
            b.box(wood, (block * (1.4 + 4.5 * 0.72), y + 0.78, z - 0.5), (7.6, 0.05, 0.4))
    # Ceiling lights, doors, and posts on the side walls
    for i in range(4):
        for x in (-6.0, 0.0, 6.0):
            b.box(light, (x, top - 0.05, -5 + i * 6.0), (1.6, 0.06, 0.5))
    for x in (-3.0, 3.0):
        b.box(door, (x, 1.1, back - 0.12), (1.4, 2.2, 0.08))
    for side in (-1, 1):
        _markers(b, "post", [(side * (x1 - 0.25), 1.8, z) for z in range(-6, 14, 3)], (0.12, 3.6, 0.35))
    return b


def arena():
    b = Builder()
    concrete = b.material("concrete", (0.55, 0.55, 0.57))
    court = b.material("court", (0.7, 0.5, 0.3))
    line = b.material("line", (0.95, 0.95, 0.95))
    dark = b.material("dark", (0.16, 0.17, 0.2))
    roof = b.material("roof", (0.3, 0.32, 0.36))
    lamp = b.material("lamp", (1, 1, 0.9), emissive=(1.0, 0.97, 0.85))
    screen = b.material("screen", (0.05, 0.08, 0.2), emissive=(0.1, 0.25, 0.7))
    steel = b.material("steel", (0.6, 0.62, 0.65))
    b.cylinder(court, (0, 0), 18.5, -0.1, 0.0, 48)
    b.ring_sector(line, (0, 0), 2.6, 2.8, 0.0, 0.01, 0, 360, 48)
    b.ring_sector(line, (0, 0), 17.9, 18.1, 0.0, 0.01, 0, 360, 48)
    b.box(line, (0, 0.005, 0), (36, 0.01, 0.2))
    # Stands: 24 steps, a lower bowl and an upper one in different colours, in 12 blocks with aisles
    bowls = [[(0.75, 0.15, 0.15), (0.85, 0.5, 0.1)], [(0.15, 0.3, 0.7), (0.2, 0.6, 0.65)]]
    for tier in range(24):
        r0 = 19.5 + tier * 0.9
        y1 = 0.5 * (tier + 1)
        palette = bowls[tier // 12]
        for k in range(12):
            material = b.material(f"seat-{tier // 12}-{k % 2}", palette[k % 2])
            b.ring_sector(material, (0, 0), r0, r0 + 0.9, 0.0, y1, k * 30 + 1.5, (k + 1) * 30 - 1.5, 4)
            b.ring_sector(concrete, (0, 0), r0, r0 + 0.9, 0.0, y1, (k + 1) * 30 - 1.5, (k + 1) * 30 + 1.5, 1)
    r_out = 19.5 + 24 * 0.9
    b.ring_sector(dark, (0, 0), r_out, r_out + 0.4, 0.0, 17.0, 0, 360, 96)
    b.ring_sector(roof, (0, 0), 28.0, r_out + 3.0, 17.0, 17.4, 0, 360, 96)
    # Lamps under the roof, a scoreboard over the middle, posts and hoops
    for k in range(24):
        a = math.radians(k * 15)
        b.box(lamp, (30.0 * math.sin(a), 16.7, -30.0 * math.cos(a)), (1.4, 0.2, 1.4), k * 15)
    b.box(dark, (0, 12.0, 0), (6.5, 2.8, 6.5))
    for turn in (0, 90, 180, 270):
        a = math.radians(turn)
        b.box(screen, (3.3 * math.sin(a), 12.0, -3.3 * math.cos(a)), (5.0, 2.2, 0.05), turn)
    for sign in (-1, 1):
        b.box(steel, (sign * 16.6, 2.0, 0), (0.15, 4.0, 0.15))
        b.box(line, (sign * 16.0, 3.5, 0), (0.05, 1.05, 1.8))
    for k in range(12):
        a = math.radians(k * 30 + 15)
        material = b.material(f"post{k % 4}", MARKERS[k % 4])
        b.box(material, (19.2 * math.sin(a), 1.0, -19.2 * math.cos(a)), (0.3, 2.0, 0.3), k * 30 + 15)
    return b


def sanctuary():
    b = Builder()
    stone = b.material("stone", (0.8, 0.77, 0.7))
    floor = b.material("floor", (0.5, 0.42, 0.36))
    carpet = b.material("carpet", (0.5, 0.1, 0.12))
    wood = b.material("wood", (0.38, 0.22, 0.12))
    dark = b.material("darkwood", (0.24, 0.14, 0.08))
    cloth = b.material("cloth", (0.92, 0.9, 0.85))
    gold = b.material("gold", (0.85, 0.65, 0.2), emissive=(0.5, 0.36, 0.08))
    vault = b.material("vault", (0.88, 0.85, 0.78))
    lamp = b.material("lamp", (1, 0.9, 0.7), emissive=(1.0, 0.8, 0.45))
    glass = [b.material(f"glass{i}", c, emissive=e) for i, (c, e) in enumerate((
        ((0.7, 0.1, 0.1), (0.8, 0.12, 0.1)), ((0.1, 0.25, 0.75), (0.1, 0.3, 0.9)),
        ((0.1, 0.55, 0.25), (0.1, 0.65, 0.3)), ((0.9, 0.65, 0.1), (1.0, 0.75, 0.15))))]
    half, front, back, wall_h = 6.0, -16.0, 16.0, 7.0     # half the nave's width, its ends, and where the vault springs
    depth = back - front
    b.box(floor, (0, -0.05, front + depth / 2), (2 * half, 0.1, depth))
    for side in (-1, 1):
        b.box(stone, (side * (half + 0.2), wall_h / 2, front + depth / 2), (0.4, wall_h, depth))
    gable = wall_h + half + 0.5
    b.box(stone, (0, gable / 2, front - 0.2), (2 * half + 0.8, gable, 0.4))
    b.box(stone, (0, gable / 2, back + 0.2), (2 * half + 0.8, gable, 0.4))
    b.vault(vault, 0, front, back, wall_h, half + 0.01)
    for z in range(-12, 16, 4):     # ribs and the pillars under them
        b.vault(dark, 0, z - 0.15, z + 0.15, wall_h, half + 0.01, thickness=0.25)
        for side in (-1, 1):
            b.box(stone, (side * (half - 0.2), wall_h / 2, z), (0.7, wall_h, 0.7))
    # Stained glass between the pillars
    for i, z in enumerate(range(-14, 16, 4)):
        for side in (-1, 1):
            b.box(glass[(i + (side > 0)) % 4], (side * (half - 0.05), 4.4, z), (0.05, 3.4, 1.3))
    # The chancel, altar, cross, lectern
    b.box(floor, (0, 0.3, front + 2.5), (2 * half, 0.6, 5.0))
    b.box(carpet, (0, 0.61, front + 2.5), (3.0, 0.02, 4.8))
    b.box(wood, (0, 1.05, front + 1.2), (2.6, 0.9, 0.9))
    b.box(cloth, (0, 1.52, front + 1.2), (2.7, 0.04, 1.0))
    b.box(gold, (0, 5.2, front + 0.1), (0.35, 3.6, 0.1))
    b.box(gold, (0, 5.9, front + 0.1), (2.0, 0.35, 0.1))
    b.box(dark, (-3.6, 1.2, front + 2.8), (0.8, 1.2, 0.8))
    b.box(dark, (3.6, 0.9, front + 2.8), (1.2, 0.6, 1.2))
    # The aisle carpet and the pews: two blocks of rows, each with the back on the far side from the altar
    b.box(carpet, (0, 0.01, 2.0), (1.8, 0.02, 2 * 14.0))
    for row in range(16):
        z = -7.5 + row * 1.2
        for side in (-1, 1):
            x = side * 3.4
            b.box(wood, (x, 0.45, z), (4.0, 0.08, 0.5))
            b.box(dark, (x, 0.75, z + 0.3), (4.0, 0.9, 0.08))
            for end in (-1, 1):
                b.box(dark, (x + end * 1.95, 0.45, z + 0.05), (0.1, 0.9, 0.7))
    for z in (-12, 0, 12):      # hanging lamps
        for x in (-2.4, 2.4):
            b.cylinder(lamp, (x, z), 0.25, wall_h + 2.0, wall_h + 2.5, 8)
    b.box(dark, (0, 1.4, back - 0.1), (2.6, 2.8, 0.1))
    return b


ROOMS = {"lecture-hall": lecture_hall, "arena": arena, "sanctuary": sanctuary}


def generate(name, cache_dir):
    """(the directory, the file in it) of the .glb for this room, made if it isn't there"""
    if name not in ROOMS:
        raise KeyError(f"no generated room '{name}': {', '.join(ROOMS)}")
    directory = os.path.join(cache_dir, "generated")
    entry = f"{name}-v{VERSION}.glb"
    path = os.path.join(directory, entry)
    if not os.path.exists(path):
        os.makedirs(directory, exist_ok=True)
        data = ROOMS[name]().glb()
        with open(path + ".part", "wb") as f:
            f.write(data)
        os.replace(path + ".part", path)
        print(f"[room] built {name} ({len(data) // 1024} KiB)")
    return directory, entry


if __name__ == "__main__":
    import sys
    print(os.path.join(*generate(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else ".")))
