#!/usr/bin/env python3
"""ptz-probe: make an obs-ptz camera report without OBS.

Asks a VISCA camera what it has, as the plugin's own "Create Camera
Report..." does (doc/visca-protocol.md, "Camera Reports"), and writes the
same report to a file, for its user to look over and send in on GitHub with
the plugin's camera report issue form. For anyone who can't run the plugin,
or whose camera it can't talk to at all.

It asks the camera for everything the plugin can ask any camera for, then
sends each value the camera can set back to it as it said it was, which
changes nothing. It doesn't move the camera. It talks to the camera, and to
nothing else: it never sends the report anywhere. The report has the
camera's make, model and firmware version, and what it answered; not its
address, nor the ID its user gave it, nor anything about its user.

    python3 ptz-probe.py 192.168.0.20                  VISCA over IP (UDP, port 52381)
    python3 ptz-probe.py 192.168.0.20 --tcp [PORT]     VISCA over TCP (port 5678)
    python3 ptz-probe.py 192.168.0.20 --dvip [PORT]    Datavideo DVIP (port 5002)
    python3 ptz-probe.py /dev/ttyUSB0 --serial         serial (Linux and macOS)

Quit OBS, or anything else talking to the camera, first. Python 3.8 or
later, and nothing else.
"""
import argparse
import json
import os
import platform
import select
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

# --- tables: written by gen_tables.py, don't edit ---
TABLES = json.loads(r'''
{
 "version": "0.19.0",
 "controls": [
  {
   "key": "power_on",
   "set": {
    "cmd": "8101040000ff",
    "args": [
     [
      "power_on",
      "flag",
      4,
      0,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81090400ff"
   ]
  },
  {
   "key": "wb_mode",
   "set": {
    "cmd": "8101043500ff",
    "args": [
     [
      "wb_mode",
      "int",
      4,
      15,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e01ff",
    "81090435ff"
   ]
  },
  {
   "key": "ae_mode",
   "set": {
    "cmd": "8101043900ff",
    "args": [
     [
      "ae_mode",
      "int",
      4,
      15,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e01ff",
    "81090439ff"
   ]
  },
  {
   "key": "exposure_comp",
   "set": {
    "cmd": "8101043e00ff",
    "args": [
     [
      "exposure_comp",
      "flag",
      4,
      0,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e01ff",
    "8109043eff"
   ]
  },
  {
   "key": "slow_shutter",
   "set": {
    "cmd": "8101045a00ff",
    "args": [
     [
      "slow_shutter",
      "flag",
      4,
      0,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e01ff",
    "8109045aff"
   ]
  },
  {
   "key": "focus_af_mode",
   "set": {
    "cmd": "8101045700ff",
    "args": [
     [
      "focus_af_mode",
      "int",
      4,
      15,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e00ff",
    "81090457ff"
   ]
  },
  {
   "key": "low_latency",
   "set": {
    "cmd": "81017e015a00ff",
    "args": [
     [
      "low_latency",
      "flag",
      5,
      0,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e015aff"
   ]
  },
  {
   "key": "dzoom_on",
   "set": {
    "cmd": "8101040600ff",
    "args": [
     [
      "dzoom_on",
      "flag",
      4,
      0,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e00ff",
    "81090406ff"
   ]
  },
  {
   "key": "focus_af_sensitivity",
   "set": {
    "cmd": "8101045800ff",
    "args": [
     [
      "focus_af_sensitivity",
      "flag",
      4,
      0,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e00ff",
    "81090458ff"
   ]
  },
  {
   "key": "focus_near_limit",
   "set": {
    "cmd": "8101042800000000ff",
    "args": [
     [
      "focus_near_limit",
      "int",
      4,
      252645135,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e00ff",
    "81090428ff"
   ]
  },
  {
   "key": "ir_correction",
   "set": {
    "cmd": "8101041100ff",
    "args": [
     [
      "ir_correction",
      "int",
      4,
      15,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81090411ff"
   ]
  },
  {
   "key": "r_gain",
   "set": {
    "cmd": "8101044300000000ff",
    "args": [
     [
      "r_gain",
      "int",
      6,
      3855,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e01ff",
    "81090443ff"
   ]
  },
  {
   "key": "b_gain",
   "set": {
    "cmd": "8101044400000000ff",
    "args": [
     [
      "b_gain",
      "int",
      6,
      3855,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e01ff",
    "81090444ff"
   ]
  },
  {
   "key": "shutter_pos",
   "set": {
    "cmd": "8101044a00000000ff",
    "args": [
     [
      "shutter_pos",
      "int",
      6,
      3855,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e01ff",
    "8109044aff"
   ]
  },
  {
   "key": "iris_pos",
   "set": {
    "cmd": "8101044b00000000ff",
    "args": [
     [
      "iris_pos",
      "int",
      6,
      3855,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e01ff",
    "8109044bff"
   ]
  },
  {
   "key": "gain_pos",
   "set": {
    "cmd": "8101044c00000000ff",
    "args": [
     [
      "gain_pos",
      "int",
      6,
      3855,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e01ff",
    "8109044cff"
   ]
  },
  {
   "key": "gain_limit",
   "set": {
    "cmd": "8101042c00ff",
    "args": [
     [
      "gain_limit",
      "int",
      4,
      15,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e03ff",
    "8109042cff"
   ]
  },
  {
   "key": "bright_pos",
   "set": {
    "cmd": "8101044d00000000ff",
    "args": [
     [
      "bright_pos",
      "int",
      6,
      3855,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e01ff",
    "8109044dff"
   ]
  },
  {
   "key": "exposure_comp_pos",
   "set": {
    "cmd": "8101044e00000000ff",
    "args": [
     [
      "exposure_comp_pos",
      "int",
      6,
      3855,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e01ff",
    "8109044eff"
   ]
  },
  {
   "key": "back_light",
   "set": {
    "cmd": "8101043300ff",
    "args": [
     [
      "back_light",
      "flag",
      4,
      0,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e01ff",
    "81090433ff"
   ]
  },
  {
   "key": "wd_mode",
   "set": {
    "cmd": "81017e040000ff",
    "args": [
     [
      "wd_mode",
      "int",
      5,
      15,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e0400ff"
   ]
  },
  {
   "key": "defog_mode",
   "set": {
    "cmd": "810104370000ff",
    "args": [
     [
      "defog_mode",
      "flag",
      4,
      0,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e04ff",
    "81090437ff"
   ]
  },
  {
   "key": "high_sensitivity",
   "set": {
    "cmd": "8101045e00ff",
    "args": [
     [
      "high_sensitivity",
      "flag",
      4,
      0,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e03ff",
    "8109045eff"
   ]
  },
  {
   "key": "aperture_gain",
   "set": {
    "cmd": "8101044200000000ff",
    "args": [
     [
      "aperture_gain",
      "int",
      6,
      3855,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e01ff",
    "81090442ff"
   ]
  },
  {
   "key": "high_resolution",
   "set": {
    "cmd": "8101045200ff",
    "args": [
     [
      "high_resolution",
      "flag",
      4,
      0,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e01ff",
    "81090452ff"
   ]
  },
  {
   "key": "nr_level",
   "set": {
    "cmd": "8101045300ff",
    "args": [
     [
      "nr_level",
      "int",
      4,
      15,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e03ff",
    "81090453ff"
   ]
  },
  {
   "key": "gamma",
   "set": {
    "cmd": "8101045b00ff",
    "args": [
     [
      "gamma",
      "int",
      4,
      15,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e03ff",
    "8109045bff"
   ]
  },
  {
   "key": "chroma_suppress",
   "set": {
    "cmd": "8101045f00ff",
    "args": [
     [
      "chroma_suppress",
      "int",
      4,
      255,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e03ff",
    "8109045fff"
   ]
  },
  {
   "key": "color_gain",
   "set": {
    "cmd": "8101044900000000ff",
    "args": [
     [
      "color_gain",
      "int",
      7,
      15,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e03ff",
    "81090449ff"
   ]
  },
  {
   "key": "color_hue",
   "set": {
    "cmd": "8101044f00000000ff",
    "args": [
     [
      "color_hue",
      "int",
      7,
      15,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e05ff",
    "8109044fff"
   ]
  },
  {
   "key": "picture_effect",
   "set": {
    "cmd": "8101046300ff",
    "args": [
     [
      "picture_effect",
      "int",
      4,
      15,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e02ff",
    "81090463ff"
   ]
  },
  {
   "key": "camera_id",
   "set": {
    "cmd": "8101042200000000ff",
    "args": [
     [
      "camera_id",
      "int",
      4,
      252645135,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e02ff",
    "81090422ff"
   ]
  },
  {
   "key": "video_format",
   "set": {
    "cmd": "81017e011e0000ff",
    "args": [
     [
      "video_format",
      "int",
      5,
      3855,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81090623ff"
   ]
  },
  {
   "key": "color_system",
   "set": {
    "cmd": "81017e01030000ff",
    "args": [
     [
      "color_system",
      "int",
      6,
      15,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e0103ff"
   ]
  },
  {
   "key": "info_display",
   "set": {
    "cmd": "81017e011800ff",
    "args": [
     [
      "info_display",
      "flag",
      5,
      0,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e0118ff"
   ]
  },
  {
   "key": "ir_receive",
   "set": {
    "cmd": "8101060800ff",
    "args": [
     [
      "ir_receive",
      "flag",
      4,
      0,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81090608ff"
   ]
  },
  {
   "key": "vendor_id",
   "set": null,
   "set_to": {},
   "reads": [
    "81090002ff"
   ]
  },
  {
   "key": "pan_pos",
   "set": null,
   "set_to": {},
   "reads": [
    "81090612ff"
   ]
  },
  {
   "key": "tilt_pos",
   "set": null,
   "set_to": {},
   "reads": [
    "81090612ff"
   ]
  },
  {
   "key": "pan_max_speed",
   "set": null,
   "set_to": {},
   "reads": [
    "81090611ff"
   ]
  },
  {
   "key": "focus_pos",
   "set": null,
   "set_to": {},
   "reads": [
    "81097e7e00ff",
    "81090448ff"
   ]
  },
  {
   "key": "zoom_pos",
   "set": null,
   "set_to": {},
   "reads": [
    "81097e7e00ff",
    "81090447ff"
   ]
  },
  {
   "key": "focus_af_enabled",
   "set": null,
   "set_to": {
    "1": "8101043802ff",
    "0": "8101043803ff"
   },
   "reads": [
    "81097e7e00ff",
    "81090438ff"
   ]
  },
  {
   "key": "dzoom_pos",
   "set": null,
   "set_to": {},
   "reads": [
    "81097e7e03ff"
   ]
  },
  {
   "key": "focus_af_move_time",
   "set": {
    "cmd": "8101042700000000ff",
    "args": [
     [
      "focus_af_move_time",
      "int",
      4,
      3855,
      false,
      0
     ],
     [
      "focus_af_interval_time",
      "int",
      6,
      3855,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e03ff",
    "81090427ff"
   ]
  },
  {
   "key": "focus_af_interval_time",
   "set": {
    "cmd": "8101042700000000ff",
    "args": [
     [
      "focus_af_move_time",
      "int",
      4,
      3855,
      false,
      0
     ],
     [
      "focus_af_interval_time",
      "int",
      6,
      3855,
      false,
      0
     ]
    ]
   },
   "set_to": {},
   "reads": [
    "81097e7e03ff",
    "81090427ff"
   ]
  },
  {
   "key": "menu_on",
   "set": null,
   "set_to": {
    "0": "8101060603ff"
   },
   "reads": [
    "81090606ff"
   ]
  },
  {
   "key": "ir_condition",
   "set": null,
   "set_to": {},
   "reads": [
    "81090634ff"
   ]
  },
  {
   "key": "pantilt_move_status",
   "set": null,
   "set_to": {},
   "reads": [
    "81090610ff"
   ]
  },
  {
   "key": "tally_on",
   "set": null,
   "set_to": {
    "1": "81017e010a0002ff",
    "0": "81017e010a0003ff"
   },
   "reads": [
    "81097e010aff"
   ]
  },
  {
   "key": "tally_preview",
   "set": null,
   "set_to": {
    "1": "81017e041a0002ff",
    "0": "81017e041a0003ff"
   },
   "reads": []
  }
 ],
 "inquiries": {
  "81090400ff": [
   [
    "power_on",
    "flag",
    2,
    0,
    false,
    0
   ]
  ],
  "81097e7e01ff": [
   [
    "r_gain",
    "int",
    2,
    3855,
    false,
    0
   ],
   [
    "b_gain",
    "int",
    4,
    3855,
    false,
    0
   ],
   [
    "wb_mode",
    "int",
    6,
    15,
    false,
    0
   ],
   [
    "aperture_gain",
    "int",
    7,
    15,
    false,
    0
   ],
   [
    "ae_mode",
    "int",
    8,
    15,
    false,
    0
   ],
   [
    "high_resolution",
    "bool",
    9,
    32,
    false,
    0
   ],
   [
    "back_light",
    "bool",
    9,
    4,
    false,
    0
   ],
   [
    "exposure_comp",
    "bool",
    9,
    2,
    false,
    0
   ],
   [
    "slow_shutter",
    "bool",
    9,
    1,
    false,
    0
   ],
   [
    "shutter_pos",
    "int",
    10,
    31,
    false,
    0
   ],
   [
    "iris_pos",
    "int",
    11,
    31,
    false,
    0
   ],
   [
    "gain_pos",
    "int",
    12,
    31,
    false,
    0
   ],
   [
    "bright_pos",
    "int",
    13,
    31,
    false,
    0
   ],
   [
    "exposure_comp_pos",
    "int",
    14,
    15,
    false,
    0
   ]
  ],
  "81090435ff": [
   [
    "wb_mode",
    "int",
    2,
    15,
    false,
    0
   ]
  ],
  "81090439ff": [
   [
    "ae_mode",
    "int",
    2,
    15,
    false,
    0
   ]
  ],
  "8109043eff": [
   [
    "exposure_comp",
    "flag",
    2,
    0,
    false,
    0
   ]
  ],
  "8109045aff": [
   [
    "slow_shutter",
    "flag",
    2,
    0,
    false,
    0
   ]
  ],
  "81097e7e00ff": [
   [
    "zoom_pos",
    "int",
    2,
    252645135,
    false,
    0
   ],
   [
    "focus_near_limit",
    "int",
    6,
    3855,
    false,
    8
   ],
   [
    "focus_pos",
    "int",
    8,
    252645135,
    false,
    0
   ],
   [
    "focus_af_mode",
    "int",
    13,
    24,
    false,
    0
   ],
   [
    "focus_af_sensitivity",
    "bool",
    13,
    4,
    false,
    0
   ],
   [
    "dzoom_on",
    "bool",
    13,
    2,
    false,
    0
   ],
   [
    "focus_af_enabled",
    "bool",
    13,
    1,
    false,
    0
   ],
   [
    "low_contrast",
    "bool",
    14,
    8,
    false,
    0
   ],
   [
    "memory_recall_running",
    "bool",
    14,
    4,
    false,
    0
   ],
   [
    "focus_command_running",
    "bool",
    14,
    2,
    false,
    0
   ],
   [
    "zoom_command_running",
    "bool",
    14,
    1,
    false,
    0
   ]
  ],
  "81090457ff": [
   [
    "focus_af_mode",
    "int",
    2,
    15,
    false,
    0
   ]
  ],
  "81097e015aff": [
   [
    "low_latency",
    "flag",
    2,
    0,
    false,
    0
   ]
  ],
  "81090406ff": [
   [
    "dzoom_on",
    "flag",
    2,
    0,
    false,
    0
   ]
  ],
  "81090458ff": [
   [
    "focus_af_sensitivity",
    "flag",
    2,
    0,
    false,
    0
   ]
  ],
  "81090428ff": [
   [
    "focus_near_limit",
    "int",
    2,
    252645135,
    false,
    0
   ]
  ],
  "81090411ff": [
   [
    "ir_correction",
    "int",
    2,
    15,
    false,
    0
   ]
  ],
  "81090443ff": [
   [
    "r_gain",
    "int",
    4,
    3855,
    false,
    0
   ]
  ],
  "81090444ff": [
   [
    "b_gain",
    "int",
    4,
    3855,
    false,
    0
   ]
  ],
  "8109044aff": [
   [
    "shutter_pos",
    "int",
    4,
    3855,
    false,
    0
   ]
  ],
  "8109044bff": [
   [
    "iris_pos",
    "int",
    4,
    3855,
    false,
    0
   ]
  ],
  "8109044cff": [
   [
    "gain_pos",
    "int",
    4,
    3855,
    false,
    0
   ]
  ],
  "81097e7e03ff": [
   [
    "dzoom_pos",
    "int",
    2,
    3855,
    false,
    0
   ],
   [
    "focus_af_move_time",
    "int",
    4,
    3855,
    false,
    0
   ],
   [
    "focus_af_interval_time",
    "int",
    6,
    3855,
    false,
    0
   ],
   [
    "color_gain",
    "int",
    11,
    120,
    false,
    0
   ],
   [
    "gamma",
    "int",
    13,
    112,
    false,
    0
   ],
   [
    "high_sensitivity",
    "bool",
    13,
    8,
    false,
    0
   ],
   [
    "nr_level",
    "int",
    13,
    7,
    false,
    0
   ],
   [
    "chroma_suppress",
    "int",
    14,
    112,
    false,
    0
   ],
   [
    "gain_limit",
    "int",
    14,
    15,
    false,
    0
   ]
  ],
  "8109042cff": [
   [
    "gain_limit",
    "int",
    2,
    15,
    false,
    0
   ]
  ],
  "8109044dff": [
   [
    "bright_pos",
    "int",
    4,
    3855,
    false,
    0
   ]
  ],
  "8109044eff": [
   [
    "exposure_comp_pos",
    "int",
    4,
    3855,
    false,
    0
   ]
  ],
  "81090433ff": [
   [
    "back_light",
    "flag",
    2,
    0,
    false,
    0
   ]
  ],
  "81097e0400ff": [
   [
    "wd_mode",
    "int",
    2,
    15,
    false,
    0
   ]
  ],
  "81097e7e04ff": [
   [
    "defog_mode",
    "bool",
    7,
    1,
    false,
    0
   ]
  ],
  "81090437ff": [
   [
    "defog_mode",
    "flag",
    2,
    0,
    false,
    0
   ]
  ],
  "8109045eff": [
   [
    "high_sensitivity",
    "flag",
    2,
    0,
    false,
    0
   ]
  ],
  "81090442ff": [
   [
    "aperture_gain",
    "int",
    4,
    3855,
    false,
    0
   ]
  ],
  "81090452ff": [
   [
    "high_resolution",
    "flag",
    2,
    0,
    false,
    0
   ]
  ],
  "81090453ff": [
   [
    "nr_level",
    "int",
    2,
    15,
    false,
    0
   ]
  ],
  "8109045bff": [
   [
    "gamma",
    "int",
    2,
    15,
    false,
    0
   ]
  ],
  "8109045fff": [
   [
    "chroma_suppress",
    "int",
    2,
    15,
    false,
    0
   ]
  ],
  "81090449ff": [
   [
    "color_gain",
    "int",
    5,
    15,
    false,
    0
   ]
  ],
  "81097e7e05ff": [
   [
    "color_hue",
    "int",
    2,
    15,
    false,
    0
   ]
  ],
  "8109044fff": [
   [
    "color_hue",
    "int",
    5,
    15,
    false,
    0
   ]
  ],
  "81097e7e02ff": [
   [
    "picture_effect",
    "int",
    5,
    15,
    false,
    0
   ],
   [
    "camera_id",
    "int",
    8,
    252645135,
    false,
    0
   ],
   [
    "video_50hz",
    "bool",
    12,
    1,
    false,
    0
   ]
  ],
  "81090463ff": [
   [
    "picture_effect",
    "int",
    2,
    15,
    false,
    0
   ]
  ],
  "81090422ff": [
   [
    "camera_id",
    "int",
    2,
    252645135,
    false,
    0
   ]
  ],
  "81090623ff": [
   [
    "video_format",
    "int",
    2,
    15,
    false,
    0
   ]
  ],
  "81097e0103ff": [
   [
    "color_system",
    "int",
    2,
    15,
    false,
    0
   ]
  ],
  "81097e0118ff": [
   [
    "info_display",
    "flag",
    2,
    0,
    false,
    0
   ]
  ],
  "81090608ff": [
   [
    "ir_receive",
    "flag",
    2,
    0,
    false,
    0
   ]
  ],
  "81090002ff": [
   [
    "vendor_id",
    "int",
    2,
    32767,
    false,
    0
   ],
   [
    "model_id",
    "int",
    4,
    32767,
    false,
    0
   ],
   [
    "vendor_name",
    "name",
    2,
    32767,
    false,
    0
   ],
   [
    "model_name",
    "name",
    2,
    2147483647,
    false,
    0
   ],
   [
    "rom_version",
    "int",
    6,
    65535,
    false,
    0
   ],
   [
    "socket_number",
    "int",
    8,
    255,
    false,
    0
   ]
  ],
  "81090612ff": [
   [
    "pan_pos",
    "int",
    2,
    252645135,
    true,
    0
   ],
   [
    "tilt_pos",
    "int",
    6,
    252645135,
    true,
    0
   ]
  ],
  "81090611ff": [
   [
    "pan_max_speed",
    "int",
    2,
    127,
    false,
    0
   ],
   [
    "tilt_max_speed",
    "int",
    3,
    127,
    false,
    0
   ]
  ],
  "81090448ff": [
   [
    "focus_pos",
    "int",
    2,
    252645135,
    false,
    0
   ]
  ],
  "81090447ff": [
   [
    "zoom_pos",
    "int",
    2,
    252645135,
    false,
    0
   ]
  ],
  "81090438ff": [
   [
    "focus_af_enabled",
    "flag",
    2,
    0,
    false,
    0
   ]
  ],
  "81090427ff": [
   [
    "focus_af_move_time",
    "int",
    2,
    3855,
    false,
    0
   ],
   [
    "focus_af_interval_time",
    "int",
    4,
    3855,
    false,
    0
   ]
  ],
  "81090606ff": [
   [
    "menu_on",
    "flag",
    2,
    0,
    false,
    0
   ]
  ],
  "81090634ff": [
   [
    "ir_condition",
    "int",
    2,
    15,
    false,
    0
   ]
  ],
  "81090610ff": [
   [
    "pantilt_init_status",
    "int",
    2,
    48,
    false,
    0
   ],
   [
    "pantilt_move_status",
    "int",
    2,
    12,
    false,
    0
   ],
   [
    "pantilt_tilt_error",
    "bool",
    2,
    3,
    false,
    0
   ],
   [
    "pantilt_pan_error",
    "bool",
    3,
    48,
    false,
    0
   ],
   [
    "pantilt_at_left_limit",
    "bool",
    3,
    1,
    false,
    0
   ],
   [
    "pantilt_at_right_limit",
    "bool",
    3,
    2,
    false,
    0
   ],
   [
    "pantilt_at_upper_limit",
    "bool",
    3,
    4,
    false,
    0
   ],
   [
    "pantilt_at_lower_limit",
    "bool",
    3,
    8,
    false,
    0
   ]
  ],
  "81097e010aff": [
   [
    "tally_on",
    "flag",
    2,
    0,
    false,
    0
   ]
  ]
 },
 "command_sets": [
  {
   "id": "birddog-p100",
   "models": [
    17375264
   ],
   "vendors": [],
   "inquiries": [],
   "power": null
  },
  {
   "id": "sony-srg-120dh",
   "models": [
    66833
   ],
   "vendors": [],
   "inquiries": [],
   "power": null
  }
 ],
 "vendors": {
  "1": "Sony",
  "3": "Everet",
  "16": "HuddleCamHD",
  "265": "Birddog",
  "544": "GlowStream",
  "9588": "AVer",
  "16728": "Axis"
 },
 "models": {
  "66575": "BRC-300",
  "66817": "BRC-H700",
  "66818": "BRU-H700",
  "66821": "BRC-Z700",
  "66823": "BRC-Z330",
  "66827": "BRC-H900",
  "66833": "SRG-120DH",
  "66835": "SRG-300H",
  "66838": "SRG-300SE/301SE/201SE",
  "66841": "BRC-X1000",
  "66842": "BRC-H800",
  "66843": "BRC-H780",
  "66844": "BRC-X400",
  "66845": "BRC-X401",
  "66846": "ILME-FR7",
  "66847": "BRC-AM7",
  "67076": "SRG-360SHE",
  "67077": "SRG-280SHE",
  "67095": "SRG-X400",
  "67096": "SRG-X120",
  "67098": "SRG-201M2",
  "67099": "SRG-HD1M2",
  "67100": "SRG-X402",
  "67103": "SRG-X40UH",
  "67104": "SRG-H40UH",
  "67105": "SRG-A40",
  "67106": "SRG-A12",
  "196610": "EVZ405N",
  "196923": "EVP212N",
  "1049858": "HC12X-HuddleView",
  "17375264": "P100",
  "35652881": "GS300-20x-NDI",
  "628361776": "CAM520 Pro2"
 }
}
''')
# --- end of tables ---

PROGRAM = "ptz-probe"
VERSION_INQUIRY = "81090002ff"
# What a report never sends back, and never has, as the plugin's
UNSENT = {"video_format", "color_system", "low_latency", "camera_id"}
PRIVATE = {"camera_id"}
REPLY_TIMEOUT = 0.25
SETTLE = 2.0
BUSY_RETRIES = 20
ERRORS = {0x01: "message length error", 0x02: "syntax error", 0x03: "buffer full", 0x04: "cancelled",
          0x05: "no socket", 0x41: "not executable"}


# How the plugin reads and writes each kind of field (src/protocol-helpers.cpp,
# src/ptz-visca-commands.cpp)
def decode(f, msg):
    """The value `f` reads from `msg`, or None"""
    name, kind, offset, mask, signed, shift = f
    if kind == "flag":
        if len(msg) < offset + 1:
            return None
        return {0x02: True, 0x03: False}.get(msg[offset])
    if kind == "bool":
        return None if len(msg) < offset + 1 else (msg[offset] & mask) != 0
    if kind != "int":
        return None
    size = (mask.bit_length() + 7) // 8
    if len(msg) < offset + size:
        return None
    encoded = int.from_bytes(msg[offset:offset + size], "big")
    value, bit = 0, 0
    while mask:
        if mask & 1:
            value |= (encoded & 1) << bit
            bit += 1
        mask >>= 1
        encoded >>= 1
    if signed:
        value = (value ^ (1 << (bit - 1))) - (1 << (bit - 1))
    return value << shift


def encode(f, msg, value):
    """`msg`, with `value` written where `f` is"""
    name, kind, offset, mask, signed, shift = f
    msg = bytearray(msg)
    if kind == "flag":
        if len(msg) >= offset + 1:
            msg[offset] = 0x02 if value else 0x03
    elif kind == "bool":
        if len(msg) >= offset + 1:
            msg[offset] = (msg[offset] & ~mask & 0xff) | (mask if value else 0)
    elif kind == "int":
        size = (mask.bit_length() + 7) // 8
        if len(msg) >= offset + size:
            encoded, bit, m = 0, 0, mask
            while m:
                if m & 1:
                    encoded |= (value & 1) << bit
                    value >>= 1
                bit += 1
                m >>= 1
            for i in range(size - 1, -1, -1):
                msg[offset + i] = ((~mask & msg[offset + i]) | encoded) & 0xff
                mask >>= 8
                encoded >>= 8
    return bytes(msg)


# The ways to a camera: each sends a VISCA packet and hands back the ones
# that come back, as they are, without their framing
class Udp:
    """Sony's VISCA over IP: an 8 byte header, with a sequence number"""
    type = "visca-over-ip"
    gap = 0.02

    def __init__(self, host, port):
        self.address = (socket.gethostbyname(host), port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # A Sony answers to 52381 whatever port a request came from. Not
        # shared: the answers to OBS's requests, if it has the port, aren't
        # the probe's
        try:
            self.sock.bind(("", 52381))
        except OSError:
            print("UDP port 52381 is taken, by OBS perhaps: a Sony camera won't be heard. "
                  "Quit whatever has it first.", file=sys.stderr)
            self.sock.bind(("", 0))
        self.seq = 0
        self.sock.sendto(struct.pack(">HHI", 0x0200, 1, 0) + b"\x01", self.address)
        self.receive(0.3)

    def send(self, packet):
        self.seq += 1
        kind = 0x0110 if packet[1] == 0x09 else 0x0100
        self.sock.sendto(struct.pack(">HHI", kind, len(packet), self.seq) + packet, self.address)

    def receive(self, timeout):
        ready, _, _ = select.select([self.sock], [], [], timeout)
        if not ready:
            return []
        data, sender = self.sock.recvfrom(2048)
        if sender[0] != self.address[0] or len(data) < 8 or struct.unpack(">H", data[:2])[0] != 0x0111:
            return []
        return [data[8:]]


class Stream:
    """VISCA over TCP, a packet ending in ff, or with `dvip`, Datavideo's
    DVIP: each packet after its length, the length's own 2 bytes included"""
    gap = 0

    def __init__(self, host, port, dvip=False):
        self.dvip = dvip
        self.type = "visca-over-dvip" if dvip else "visca-over-tcp"
        self.sock = socket.create_connection((host, port), timeout=5)
        self.buf = b""

    def send(self, packet):
        self.sock.sendall((len(packet) + 2).to_bytes(2, "big") + packet if self.dvip else packet)

    def read(self, timeout):
        ready, _, _ = select.select([self.sock], [], [], timeout)
        if ready:
            data = self.sock.recv(4096)
            if not data:
                raise ConnectionError("the camera closed the connection")
            self.buf += data

    def receive(self, timeout):
        self.read(timeout)
        out = []
        while True:
            if self.dvip:
                if len(self.buf) < 2 or len(self.buf) < int.from_bytes(self.buf[:2], "big"):
                    break
                length = int.from_bytes(self.buf[:2], "big")
                out.append(self.buf[2:length])
                self.buf = self.buf[length:]
            else:
                end = self.buf.find(b"\xff")
                if end < 0:
                    break
                out.append(self.buf[:end + 1])
                self.buf = self.buf[end + 1:]
        return out


class Serial(Stream):
    """A serial port, on Linux or macOS, raw, at `baud`"""
    type = "visca"

    def __init__(self, path, baud):
        import termios
        import tty
        self.dvip = False
        self.buf = b""
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(self.fd)
        attrs = termios.tcgetattr(self.fd)
        speed = getattr(termios, f"B{baud}")
        attrs[4] = attrs[5] = speed
        attrs[2] |= termios.CLOCAL | termios.CREAD
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        # address the cameras on the bus, as the plugin does
        os.write(self.fd, b"\x88\x30\x01\xff")
        self.receive(0.5)

    def send(self, packet):
        os.write(self.fd, packet)

    def read(self, timeout):
        ready, _, _ = select.select([self.fd], [], [], timeout)
        if ready:
            self.buf += os.read(self.fd, 4096)


class Probe:
    """Asks the camera at `address` on `link` one thing at a time, as the
    plugin's report does (src/ptz-visca.cpp, start_report())"""

    def __init__(self, link, address=1):
        self.link = link
        self.address = address
        self.buffer_full = 0
        # commands the camera ACKed, by socket, for their completions
        self.acked = {}
        self.inbox = []

    def readdress(self, packet):
        return bytes([0x80 | self.address]) + packet[1:]

    def next(self, timeout):
        """The next packet from the camera, waiting up to `timeout`"""
        if not self.inbox:
            self.inbox += self.link.receive(timeout)
        return self.inbox.pop(0) if self.inbox else None

    def ask(self, packet, record=None):
        """Sends `packet` (as to camera 1), and waits for the camera's first
        answer: (result, reply), the result "reply" for an inquiry's,
        "completed" or "ack" for a command, or the error. `record` is where
        a completion that comes after an ACK goes. One that isn't answered
        is asked once more, as the plugin does."""
        attempts = busy = 0
        while True:
            time.sleep(self.link.gap)
            self.link.send(self.readdress(packet))
            deadline = time.monotonic() + REPLY_TIMEOUT
            answer = None
            while answer is None and time.monotonic() < deadline:
                msg = self.next(max(0, deadline - time.monotonic()))
                if msg is not None:
                    answer = self.answer(msg, packet, record)
            if answer == "busy":
                self.buffer_full += 1
                busy += 1
                if busy > BUSY_RETRIES:
                    return "buffer full", None
                time.sleep(0.05)
                continue
            if answer:
                return answer
            attempts += 1
            if attempts == 2:
                return "no reply", None

    def answer(self, msg, packet, record):
        """What `msg` says about `packet`, or about a command it ACKed
        before, which is noted where it goes"""
        if len(msg) < 3 or (msg[0] >> 4) & 0x7 != self.address:
            return None
        msg = b"\x90" + msg[1:]
        socket_number = msg[1] & 0x0f
        kind = msg[1] & 0xf0
        if kind == 0x40:
            if record is not None:
                self.acked[socket_number] = record
            return "ack", None
        if kind == 0x50:
            if socket_number and socket_number in self.acked:
                self.acked.pop(socket_number)["result"] = "completed"
                return None
            return ("reply" if packet[1] == 0x09 else "completed"), msg
        if kind == 0x60:
            code = msg[2]
            if code == 0x03:
                return "busy"
            error = ERRORS.get(code, f"error {code:02x}")
            if socket_number and socket_number in self.acked:
                self.acked.pop(socket_number)["result"] = error
                return None
            return error, None
        return None

    def settle(self):
        """Waits for the commands the camera ACKed to complete, a while"""
        deadline = time.monotonic() + SETTLE
        while self.acked and time.monotonic() < deadline:
            msg = self.next(max(0, deadline - time.monotonic()))
            if msg is not None:
                self.answer(msg, b"\x81\x01", None)


def hex4(value):
    return f"{value:04x}"


def command_set_for(vendor, model):
    """The shipped command set the plugin would choose, by its model, then
    by any of its vendor's, as src/ptz-visca-commands.cpp does"""
    sets = TABLES["command_sets"]
    for s in sets:
        if (vendor << 16 | model) in s["models"]:
            return s
    for s in sets:
        if vendor in s["vendors"]:
            return s
    return None


def is_on(probe, command_set):
    """Whether the camera is on, asked as its command set asks, for one
    whose camera can't be asked for everything in standby"""
    reads = (command_set or {}).get("power") or [{"cmd": "81090400ff"}]
    for read in reads:
        f = read.get("field") or next(f for f in TABLES["inquiries"][read["cmd"]] if f[0] == "power_on")
        result, reply = probe.ask(bytes.fromhex(read["cmd"]))
        if result == "reply":
            return decode(f, reply) is not False
    return True


def plugin_version():
    """What the plugin calls its version: the `git describe --dirty` its build
    has, which is what this has when it is run from a checkout of the plugin;
    and the release the tables were written for when it isn't"""
    root = Path(__file__).resolve().parents[2]
    if (root / "buildspec.json").exists():
        try:
            done = subprocess.run(["git", "-C", str(root), "describe", "--dirty"], capture_output=True, text=True,
                                  timeout=10)
            if done.returncode == 0 and done.stdout.strip():
                return done.stdout.strip()
        except (OSError, subprocess.SubprocessError):
            pass
    return TABLES["version"]


def make_report(probe, progress=lambda done, total: None):
    """The report, or None, with why, if it can't be made"""
    result, reply = probe.ask(bytes.fromhex(VERSION_INQUIRY))
    if result != "reply":
        return None, f"the camera didn't say what it is: {result}"
    version = {f[0]: decode(f, reply) for f in TABLES["inquiries"][VERSION_INQUIRY]}
    vendor, model = version["vendor_id"], version["model_id"]
    command_set = command_set_for(vendor, model)
    if command_set and command_set["power"] is not None and not is_on(probe, command_set):
        return None, "the camera is in standby: turn it on, then try again"

    # everything the plugin can ask any camera for: the generic command
    # set's, then what the ones shipped with it ask for besides
    inquiries = list(TABLES["inquiries"])
    for s in TABLES["command_sets"]:
        inquiries += [i for i in s["inquiries"] if i not in inquiries]
    probes = [{"cmd": cmd} for cmd in inquiries]
    for i, p in enumerate(probes):
        progress(i, len(probes))
        p["result"], p["reply"] = probe.ask(bytes.fromhex(p["cmd"]))

    # each value the camera can set, sent back as it said it was
    values = {}
    for p in probes:
        if p["result"] == "reply" and p["cmd"] in TABLES["inquiries"]:
            for f in TABLES["inquiries"][p["cmd"]]:
                value = decode(f, p["reply"])
                if value is not None:
                    values[f[0]] = int(value)
    commands, sent = [], set()
    for control in TABLES["controls"]:
        key = control["key"]
        if not control["reads"] or key in UNSENT or key not in values:
            continue
        if control["set"]:
            if any(f[0] not in values for f in control["set"]["args"]):
                continue
            cmd = bytes.fromhex(control["set"]["cmd"])
            for f in control["set"]["args"]:
                cmd = encode(f, cmd, values[f[0]])
        elif str(values[key]) in control["set_to"]:
            cmd = bytes.fromhex(control["set_to"][str(values[key])])
        else:
            continue
        if cmd in sent:
            continue
        sent.add(cmd)
        commands.append({"key": key, "command": cmd.hex()})
    total = len(probes) + len(commands)
    for i, c in enumerate(commands):
        progress(len(probes) + i, total)
        c["result"], _ = probe.ask(bytes.fromhex(c["command"]), c)
    probe.settle()
    progress(total, total)

    camera = {"vendor_id": hex4(vendor), "model_id": hex4(model), "rom_version": hex4(version["rom_version"])}
    if str(vendor) in TABLES["vendors"]:
        camera["vendor_name"] = TABLES["vendors"][str(vendor)]
    if str(vendor << 16 | model) in TABLES["models"]:
        camera["model_name"] = TABLES["models"][str(vendor << 16 | model)]

    report_inquiries = []
    for p in probes:
        entry = {"inquiry": p["cmd"]}
        if p["result"] != "reply":
            entry["error"] = p["result"]
        else:
            reply, masked = p["reply"], []
            for f in TABLES["inquiries"].get(p["cmd"], []):
                if f[0] in PRIVATE and f[0] not in masked:
                    reply = encode(f, reply, 0)
                    masked.append(f[0])
            entry["reply"] = reply.hex()
            if masked:
                entry["masked"] = masked
        report_inquiries.append(entry)

    report = {
        "report": "obs-ptz camera report",
        "format": 1,
        "made_by": PROGRAM,
        "plugin_version": plugin_version(),
        "os": {"Darwin": "macOS"}.get(platform.system(), platform.system()),
        "type": probe.link.type,
        "protocol": "visca",
        "camera": camera,
        "command_set": command_set["id"] if command_set else "generic",
        "inquiries": report_inquiries,
        "commands": commands,
        "buffer_full": probe.buffer_full,
    }
    report["draft_command_set"] = draft(report, probes, commands)
    return report, None


def draft(report, probes, commands):
    """A command set for the camera, as the plugin drafts one
    (src/ptz-visca.cpp, report_draft())"""
    results = {p["cmd"]: p["result"] for p in probes}
    results.update({c["command"]: c["result"] for c in commands})

    def failed(cmd):
        return cmd in results and results[cmd] not in ("reply", "ack", "completed")

    unanswered, remove, controls = set(), [], []
    for control in TABLES["controls"]:
        readable = False
        for cmd in control["reads"]:
            if failed(cmd):
                unanswered.add(cmd)
            else:
                readable = True
        unsettable = any(c["key"] == control["key"] and c["result"] == "syntax error" for c in commands)
        settable = bool(control["set"] or control["set_to"]) and not unsettable
        if control["reads"] and not readable and not settable:
            remove.append(control["key"])
        elif unsettable:
            controls.append({"key": control["key"], "set": None})
    camera = report["camera"]
    name = " ".join(camera[k] for k in ("vendor_name", "model_name") if k in camera)
    return {
        "id": f"my-camera-{camera['vendor_id']}-{camera['model_id']}",
        "name": name or "My camera",
        "models": [f"{camera['vendor_id']}:{camera['model_id']}"],
        "extends": "generic",
        "source": f"Drafted from a camera report by {PROGRAM}, from obs-ptz {TABLES['version']}",
        "remove_inquiries": sorted(unanswered),
        "remove": remove,
        "controls": controls,
    }


def main():
    parser = argparse.ArgumentParser(
        description="Make an obs-ptz camera report of a VISCA camera, without OBS. "
                    "It talks only to the camera, and never sends the report anywhere.")
    parser.add_argument("camera", help="the camera's address, or its serial port with --serial")
    how = parser.add_mutually_exclusive_group()
    how.add_argument("--udp", metavar="PORT", nargs="?", type=int, const=52381,
                     help="VISCA over IP, Sony's (the default, on port 52381)")
    how.add_argument("--tcp", metavar="PORT", nargs="?", type=int, const=5678, help="VISCA over TCP (port 5678)")
    how.add_argument("--dvip", metavar="PORT", nargs="?", type=int, const=5002, help="Datavideo DVIP (port 5002)")
    how.add_argument("--serial", action="store_true", help="a serial port (Linux and macOS)")
    parser.add_argument("--baud", type=int, default=9600, help="the serial port's speed (9600)")
    parser.add_argument("--address", type=int, default=1, choices=range(1, 8), metavar="N",
                        help="the camera's address on a serial bus (1)")
    parser.add_argument("-o", "--output", default="camera-report.json", help="where to write the report")
    args = parser.parse_args()

    try:
        if args.serial:
            link = Serial(args.camera, args.baud)
        elif args.tcp:
            link = Stream(args.camera, args.tcp)
        elif args.dvip:
            link = Stream(args.camera, args.dvip, dvip=True)
        else:
            link = Udp(args.camera, args.udp or 52381)
    except (OSError, AttributeError, ImportError) as e:
        print(f"Can't reach the camera: {e}", file=sys.stderr)
        return 2

    def progress(done, total):
        if sys.stderr.isatty():
            print(f"\rAsking the camera... {done} of {total}", end="", file=sys.stderr, flush=True)

    probe = Probe(link, address=args.address if args.serial else 1)
    try:
        report, why = make_report(probe, progress)
    except (OSError, ConnectionError) as e:
        report, why = None, str(e)
    if sys.stderr.isatty():
        print(file=sys.stderr)
    if report is None:
        print(f"No report: {why}", file=sys.stderr)
        return 1
    with open(args.output, "w") as f:
        f.write(json.dumps(report, indent=4, sort_keys=True) + "\n")
    print(f"Wrote {args.output}. Look it over, then send it in with the camera report form at\n"
          "https://github.com/glikely/obs-ptz/issues/new?template=camera-report.yml\n"
          "Its draft_command_set is a command set for the camera the plugin can try: "
          "save it on its own as a .json file in the plugin's visca-profiles folder.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
