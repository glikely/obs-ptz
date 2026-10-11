"""Tests for scripts/check-module-boundaries.py, which keeps the controller,
the device core and each driver from including each other's headers.

Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
SPDX-License-Identifier: GPL-2.0-or-later

Run with python3 -m pytest tests/unit/test_boundaries.py. Needs no OBS.
"""
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CHECKER = REPO_ROOT / "scripts" / "check-module-boundaries.py"


def check(root=None):
    args = [sys.executable, str(CHECKER)]
    if root is not None:
        args += ["--root", str(root)]
    return subprocess.run(args, capture_output=True, text=True)


def tree(root, files):
    """Writes a source tree, {path under root: contents}"""
    for path, text in files.items():
        (root / path).parent.mkdir(parents=True, exist_ok=True)
        (root / path).write_text(text)
    return root


CLEAN = {
    "src/ptz.h": "#pragma once\n",
    "src/ptz-module-all.cpp": '#include "ptz-list-model.hpp"\n#include "ptz-visca.hpp"\n#include "ptz-device.hpp"\n',
    "src/controller/ptz-list-model.hpp": '#include "ptz.h"\n',
    "src/controller/ptz-list-model.cpp": '#include "ptz-list-model.hpp"\n#include "ptz.h"\n#include <QString>\n',
    "src/core/ptz-device.hpp": '#include "ptz.h"\n#include "ptz-lifecycle.hpp"\n',
    "src/core/ptz-lifecycle.hpp": "#pragma once\n",
    "src/drivers/serial/uart-wrapper.hpp": '#include "ptz.h"\n',
    "src/drivers/visca/ptz-visca.hpp": '#include "ptz-device.hpp"\n#include "uart-wrapper.hpp"\n',
    "src/drivers/visca/ptz-visca.cpp": '#include "ptz-visca.hpp"\n#include "ui_ptz-visca.h"\n',
    "src/drivers/pelco/ptz-pelco.hpp": '#include "ptz-device.hpp"\n#include "uart-wrapper.hpp"\n',
}


def test_a_clean_tree_passes(tmp_path):
    result = check(tree(tmp_path, CLEAN))
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout == ""


def test_the_controller_including_a_driver_fails(tmp_path):
    files = dict(CLEAN)
    files["src/controller/settings.cpp"] = '#include "ptz.h"\n\n#include "ptz-visca.hpp"\n'
    result = check(tree(tmp_path, files))
    assert result.returncode == 1
    assert result.stdout.splitlines() == [
        "src/controller/settings.cpp:3: includes ptz-visca.hpp (controller -> drivers/visca)"
    ]


def test_each_crossing_is_reported(tmp_path):
    files = dict(CLEAN)
    files["src/controller/a.cpp"] = '#include "ptz-device.hpp"\n'
    files["src/core/b.cpp"] = '#include "ptz-list-model.hpp"\n#include "ptz-visca.hpp"\n'
    files["src/drivers/onvif/c.cpp"] = '#include "ptz-pelco.hpp"\n#include "uart-wrapper.hpp"\n'
    files["src/drivers/visca/d.cpp"] = '#include "ptz-list-model.hpp"\n'
    result = check(tree(tmp_path, files))
    assert result.returncode == 1
    assert sorted(result.stdout.splitlines()) == [
        "src/controller/a.cpp:1: includes ptz-device.hpp (controller -> core)",
        "src/core/b.cpp:1: includes ptz-list-model.hpp (core -> controller)",
        "src/core/b.cpp:2: includes ptz-visca.hpp (core -> drivers/visca)",
        "src/drivers/onvif/c.cpp:1: includes ptz-pelco.hpp (drivers/onvif -> drivers/pelco)",
        "src/drivers/onvif/c.cpp:2: includes uart-wrapper.hpp (drivers/onvif -> drivers/serial)",
        "src/drivers/visca/d.cpp:1: includes ptz-list-model.hpp (drivers/visca -> controller)",
    ]


def test_an_include_that_is_commented_or_conditional_still_counts(tmp_path):
    files = dict(CLEAN)
    files["src/controller/e.cpp"] = '#if 0\n#include "ptz-device.hpp"\n#endif\n// #include "ptz-visca.hpp"\n'
    result = check(tree(tmp_path, files))
    assert result.returncode == 1
    assert result.stdout.splitlines() == [
        "src/controller/e.cpp:2: includes ptz-device.hpp (controller -> core)",
        "src/controller/e.cpp:4: includes ptz-visca.hpp (controller -> drivers/visca)",
    ]


def test_the_shared_header_includes_no_side(tmp_path):
    files = dict(CLEAN)
    files["src/ptz.h"] = '#include "ptz-device.hpp"\n'
    result = check(tree(tmp_path, files))
    assert result.returncode == 1
    assert result.stdout.splitlines() == ["src/ptz.h:1: includes ptz-device.hpp (src -> core)"]


def test_a_crossing_by_a_relative_path_fails(tmp_path):
    files = dict(CLEAN)
    files["src/controller/f.cpp"] = '#include "../core/ptz-device.hpp"\n'
    result = check(tree(tmp_path, files))
    assert result.returncode == 1
    assert result.stdout.splitlines() == [
        "src/controller/f.cpp:1: includes ../core/ptz-device.hpp (controller -> core)"
    ]


def test_an_include_that_cannot_be_found_fails(tmp_path):
    files = dict(CLEAN)
    files["src/controller/g.cpp"] = (
        '#include "ptz-devcie.hpp"\n#include "../core/nothing.hpp"\n#include <QString>\n'
        '#include "ui_g.h"\n#include "moc_g.cpp"\n#include "g.moc"\n#include "display-helpers.hpp"\n'
    )
    files["shared/qt/display/display-helpers.hpp"] = "#pragma once\n"
    result = check(tree(tmp_path, files))
    assert result.returncode == 1
    assert result.stdout.splitlines() == [
        "src/controller/g.cpp:1: unresolved include ptz-devcie.hpp",
        "src/controller/g.cpp:2: unresolved include ../core/nothing.hpp",
    ]


def test_a_root_with_no_sources_fails(tmp_path):
    result = check(tmp_path)
    assert result.returncode != 0
    assert "no src/" in result.stderr
    (tmp_path / "src").mkdir()
    result = check(tmp_path)
    assert result.returncode != 0
    assert "no sources" in result.stderr


def test_a_name_on_two_sides_fails(tmp_path):
    files = dict(CLEAN)
    files["src/drivers/visca/ptz-lifecycle.hpp"] = "#pragma once\n"
    result = check(tree(tmp_path, files))
    assert result.returncode == 1
    assert result.stdout.splitlines() == [
        "duplicate name ptz-lifecycle.hpp: src/core/ptz-lifecycle.hpp, src/drivers/visca/ptz-lifecycle.hpp"
    ]


def test_the_real_tree_passes():
    result = check()
    assert result.returncode == 0, result.stdout + result.stderr
