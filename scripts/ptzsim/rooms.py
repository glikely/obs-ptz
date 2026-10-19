"""Ready-made rooms for ptzsim's web view: a Poly Haven backdrop or a 3D
scene, and cameras standing in it. --room NAME picks one, --camera NAME one
of its cameras (the first by default), --list-rooms says which there are.

A camera has a heading, the direction pan 0 looks in, in degrees to the
right of the picture's or scene's own forward; a view, where the camera
starts and goes home to (pan and tilt -1..1, zoom 0..1); and, in a 3D
scene, a position. A backdrop is one spot, so its cameras are different
ways of looking from it.
"""


class RoomError(Exception):
    pass


# Pinned to a three.js release, so a model moved or changed later doesn't break these
THREE_MODELS = "https://raw.githubusercontent.com/mrdoob/three.js/r170/examples/models/gltf/"

ROOMS = {
    "sponza": {
        "title": "Sponza atrium (Khronos glTF sample; Cryengine Limited License)",
        "scene": "sponza",
        "credit": "Sponza: Khronos glTF Sample Assets, from Crytek's Sponza, Cryengine Limited License",
        # Units are metres, y up; pan 0 is -Z, so a heading of 90 looks along +X.
        # The hall runs along X, about 9 m across, with galleries at 4.6 m.
        "cameras": {
            "west-end": {"position": (-6, 3.2, 0), "heading": 90},
            "east-end": {"position": (6, 3.2, 0), "heading": -90},
            "north-gallery": {"position": (0, 5.5, -4.4), "heading": 180},
            "south-gallery": {"position": (0, 5.5, 4.1), "heading": 0},
        },
    },
    "hallway": {
        "title": "Space ship hallway (by yeeyeeman, Creative Commons Attribution on Sketchfab; via three.js examples)",
        "scene": THREE_MODELS + "space_ship_hallway.glb",
        "credit": "Space Ship Hallway by yeeyeeman, CC BY, https://sketchfab.com/3d-models/space-ship-hallway-",
        # A corridor 8 m wide and 29 m long, along Z, its floor at y=-3
        "cameras": {
            "mid": {"position": (20.1, 0.5, -11), "heading": 0},
            "near-end": {"position": (20.1, 0.5, 2), "heading": 0},
            "far-end": {"position": (20.1, 0.5, -23), "heading": 180},
        },
    },
    "dungeon": {
        "title": "Dungeon (Low Poly Game Level Challenge, by Warkarma; its licence isn't confirmed: "
                 "check it on Sketchfab before sharing; via three.js examples)",
        "scene": THREE_MODELS + "dungeon_warkarma.glb",
        "exposure": 3,      # it comes unlit
        "cameras": {
            "hall": {"position": (2.6, 3.0, 0.8), "heading": 0},
            "arches": {"position": (2.6, 3.0, 0.8), "heading": 90},
        },
    },
    "chapel": {
        "title": "Chapel (Poly Haven chapel_day, CC0)",
        "backdrop": "chapel_day",
        "cameras": {
            "pews": {"heading": 0},
            "platform": {"heading": -140},
            "windows": {"heading": 90},
        },
    },
    "church": {
        "title": "Church (Poly Haven afrikaans_church_interior, CC0)",
        "backdrop": "afrikaans_church_interior",
        "cameras": {
            "platform": {"heading": 100},
            "aisle": {"heading": -90},
        },
    },
    "gallery": {
        "title": "Gallery (Poly Haven ballroom, CC0)",
        "backdrop": "ballroom",
        "cameras": {
            "hall": {"heading": 0},
            "doorway": {"heading": 170},
        },
    },
    # Panoramas of the three kinds of room a camera is wanted in, from Poly Haven (CC0):
    # a sanctuary, a lecture hall and an auditorium, an arena and a stadium
    "sanctuary": {
        "title": "Sanctuary (Poly Haven graaff_reinet_groote_kerk, CC0)",
        "backdrop": "graaff_reinet_groote_kerk",
        "cameras": {
            "nave": {"heading": 35},
            "chancel": {"heading": -150},
            "gallery": {"heading": 100},
        },
    },
    "lecture-hall": {
        "title": "Lecture hall (Poly Haven school_hall, CC0)",
        "backdrop": "school_hall",
        "cameras": {
            "audience": {"heading": 80},
            "piano-side": {"heading": 165},
        },
    },
    "auditorium": {
        "title": "Auditorium (Poly Haven theater_02, CC0)",
        "backdrop": "theater_02",
        "cameras": {
            "stage": {"heading": 0},
            "stalls-left": {"heading": -150},
            "stalls-right": {"heading": 150},
        },
    },
    "arena": {
        "title": "Arena (Poly Haven circus_arena, CC0)",
        "backdrop": "circus_arena",
        "cameras": {
            "centre": {"heading": 0},
            "stands-left": {"heading": -115},
            "stands-right": {"heading": 115},
        },
    },
    "stadium": {
        "title": "Stadium (Poly Haven stadium_01, CC0)",
        "backdrop": "stadium_01",
        "cameras": {
            "pitch": {"heading": 0},
            "west-stand": {"heading": -80},
            "east-stand": {"heading": 85},
        },
    },
    # Rooms ptzsim builds itself (genrooms.py), which unlike a panorama a camera can stand
    # in different places in: nothing is downloaded
    "sanctuary-3d": {
        "title": "Sanctuary, a church nave built by ptzsim: chancel, pews, stained glass",
        "scene": "gen:sanctuary",
        "cameras": {
            "pulpit": {"position": (0, 2.3, -11.5), "heading": 180},
            "rear": {"position": (0, 1.7, 15), "heading": 0},
            "side-pew": {"position": (-4.8, 1.3, 3), "heading": 90},
        },
    },
    "lecture-hall-3d": {
        "title": "Lecture hall built by ptzsim: stage, whiteboard, ten rows of seats",
        "scene": "gen:lecture-hall",
        "cameras": {
            "lectern": {"position": (0, 1.8, -5.5), "heading": 180},
            "back-row": {"position": (0, 5.1, 13.4), "heading": 0},
            "side": {"position": (-9.4, 3.2, 4), "heading": 90},
        },
    },
    "arena-3d": {
        "title": "Arena built by ptzsim: a court in a bowl of stands, with a scoreboard",
        "scene": "gen:arena",
        "cameras": {
            "centre": {"position": (0, 1.7, 0), "heading": 0},
            "floor-edge": {"position": (0, 1.7, 15), "heading": 180},
            "upper-deck": {"position": (0, 13.2, 40), "heading": 0},
        },
    },
}


def pick(name, camera=None):
    """What --room NAME --camera CAMERA mean: the room's backdrop or scene,
    and the camera's position, heading and starting view"""
    room = ROOMS.get(name)
    if not room:
        raise RoomError(f"no room '{name}': {', '.join(ROOMS)} (see --list-rooms)")
    cameras = room["cameras"]
    camera = camera or next(iter(cameras))
    if camera not in cameras:
        raise RoomError(f"room '{name}' has no camera '{camera}': {', '.join(cameras)}")
    chosen = dict(cameras[camera])
    result = {"title": room["title"], "camera": camera, "backdrop": room.get("backdrop"),
              "scene": room.get("scene"), "position": chosen.get("position"),
              "heading": chosen.get("heading", 0.0), "view": chosen.get("view"),
              "exposure": room.get("exposure", 1), "credit": room.get("credit")}
    return result


def describe():
    lines = []
    for name, room in ROOMS.items():
        lines.append(f"{name}: {room['title']}")
        lines.append("    cameras: " + ", ".join(room["cameras"]) + " (the first is the default)")
    return "\n".join(lines)
