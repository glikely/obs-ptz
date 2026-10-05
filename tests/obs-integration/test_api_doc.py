"""Holds docs/ptz-device-api.md to the plugin.

The document is the specification of the PTZ API, and is written by hand, so
this test is what keeps it true. It reads the document's structure:

 * a "###" heading in a handler section, `` ### `void ptz_stop()` ``, is a proc or
   a signal, by the section it is in, with the declaration it is registered as;
 * a table in "State keys", "Config keys", "Features" or "Triggers" lists, in the
   backticks of its first column, the names it documents, and for the keys,
   the Type column and, for the state, the Present column.

and checks it against:

 * what the plugin registered: PTZDevice's ptz_proc_add() and ptz_signal_add()
   log every proc and signal, and tests/ui-harness/api-version-test.cpp's
   "get_registered_api" test hands the log back, so a proc added in the code and
   not here fails, and so does one here and not in the code;
 * real devices' state and config, whose keys have to be documented, and
   present, with the documented types;
 * the source, for the feature and trigger names, which the plugin doesn't
   register anywhere it can be asked.
"""

import json
import re
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
DOC = REPO_ROOT / "docs" / "ptz-device-api.md"

# The handler sections of the document, by what the plugin calls their scope
SCOPES = {
    "Per-device proc_handler": "device-proc",
    "Per-device signal_handler": "device-signal",
}

# obs_data_t's types, as they come back from its JSON
JSON_TYPES = {"bool": bool, "int": int, "number": (int, float), "string": str, "object": dict, "array": list}


def sections():
    """The document's "##" sections, by title"""
    parts = re.split(r"^## (.+)$", DOC.read_text(), flags=re.MULTILINE)
    return {title.strip(): body for title, body in zip(parts[1::2], parts[2::2])}


def documented_signatures():
    """{scope: [declaration, ...]} for each of the document's handler sections"""
    found = {}
    for title, scope in SCOPES.items():
        body = sections()[title]
        found[scope] = re.findall(r"^### `(.+)`$", body, flags=re.MULTILINE)
    return found


def table_rows(title):
    """The rows of the first table in the "##" section called title, as lists of
    cells, without the header"""
    rows = []
    for line in sections()[title].splitlines():
        if line.startswith("|"):
            rows.append([cell.strip() for cell in line.strip().strip("|").split("|")])
    return rows[2:]  # the header, and what separates it


def names(cell):
    return re.findall(r"`([^`]+)`", cell)


def documented_keys(title):
    """{key: (type, present)} for a keys table; "present" is "" if it has no such column"""
    keys = {}
    for row in table_rows(title):
        for key in names(row[0]):
            keys[key] = (row[1], row[2] if title == "State keys" else "")
    return keys


def check_type(value, type_name, where):
    expected = JSON_TYPES[type_name]
    # a bool is an int to Python, and no number to obs_data
    if type_name != "bool" and isinstance(value, bool):
        pytest.fail(f"{where} is a bool, documented as {type_name}")
    assert isinstance(value, expected), f"{where} is {type(value).__name__}, documented as {type_name}"


# --- what the plugin registers ---------------------------------------------------


def test_the_doc_has_each_handler_section():
    assert set(sections()) >= set(SCOPES)
    for scope, signatures in documented_signatures().items():
        assert signatures, scope
        assert len(signatures) == len(set(signatures)), f"{scope} has one documented twice"


def test_the_doc_lists_what_the_plugin_registers(obs_world, tmp_path):
    out = tmp_path / "registered.json"
    obs_world.run_ui_test("get_registered_api", filename=str(out))
    obs_world.wait_for(out.exists)

    registered = {}
    for item in json.loads(out.read_text())["registered"]:
        registered.setdefault(item["scope"], set()).add(item["signature"])
    documented = {scope: set(signatures) for scope, signatures in documented_signatures().items()}

    assert set(registered) == set(documented)
    for scope in documented:
        assert registered[scope] - documented[scope] == set(), f"{scope}: registered, and not in the doc"
        assert documented[scope] - registered[scope] == set(), f"{scope}: in the doc, and not registered"


# --- real devices' keys ----------------------------------------------------------


def test_every_state_key_a_device_always_has_is_there(obs_world, tmp_path):
    keys = documented_keys("State keys")
    always = {key for key, (_, present) in keys.items() if present == "always"}
    for backend, device_name in obs_world.device_names.items():
        state = obs_world.device_state(device_name, tmp_path / f"{backend}.json")["state"]
        assert always <= set(state), f"{backend} lacks {sorted(always - set(state))}"
        for key in always:
            check_type(state[key], keys[key][0], f"{backend} state {key}")


def test_the_state_keys_a_device_has_are_documented(obs_world, tmp_path):
    """Pelco's driver adds none of its own, so everything in its state is the
    common keys: the ones that are documented"""
    keys = documented_keys("State keys")
    state = obs_world.device_state(obs_world.device_names["pelco-d"], tmp_path / "state.json")["state"]
    # the harness adds the statistics, which the device doesn't have in its state
    state.pop("statistics", None)
    assert set(state) - set(keys) == set()
    for key, value in state.items():
        check_type(value, keys[key][0], f"state {key}")


def test_documented_state_keys_have_the_documented_types(obs_world, tmp_path):
    """VISCA has them all, once its camera has answered"""
    keys = documented_keys("State keys")
    state = obs_world.wait_for_device_state(
        obs_world.device_names["visca-tcp"], tmp_path / "state.json",
        lambda r: r["state"].get("connected") is True and "pan" in r["state"] and "focus_af_enabled" in r["state"],
        timeout=10)["state"]
    for key, (type_name, _) in keys.items():
        if key in state:
            check_type(state[key], type_name, f"state {key}")


def test_every_config_key_is_there_with_its_type(obs_world, tmp_path):
    keys = documented_keys("Config keys")
    for backend, device_name in obs_world.device_names.items():
        config = obs_world.device_settings(device_name, tmp_path / f"{backend}.json")["saved"]
        assert set(keys) <= set(config), f"{backend} lacks {sorted(set(keys) - set(config))}"
        for key, (type_name, _) in keys.items():
            check_type(config[key], type_name, f"{backend} config {key}")


def test_the_features_a_device_reports_are_documented(obs_world, tmp_path):
    documented = {name for row in table_rows("Features") for name in names(row[0])}
    for backend, device_name in obs_world.device_names.items():
        state = obs_world.device_state(device_name, tmp_path / f"{backend}.json")["state"]
        reported = set(state["features"])
        assert reported <= documented, f"{backend} reports {sorted(reported - documented)}"


# --- the source, for what isn't registered anywhere ---------------------------------


def test_the_doc_lists_the_features_in_the_source():
    source = (REPO_ROOT / "src" / "ptz-device.cpp").read_text()
    table = re.search(r"PTZDevice::featureNames\(\)\s*\{.*?names = \{(.*?)\};", source, re.DOTALL).group(1)
    in_source = set(re.findall(r'\{\w+, "(\w+)"\}', table))
    in_doc = {name for row in table_rows("Features") for name in names(row[0])}
    assert in_source, "found no features in the source"
    assert in_doc == in_source


def test_the_doc_lists_the_triggers_in_the_source():
    src = REPO_ROOT / "src"
    in_source = set()
    for path in ("ptz-device.cpp", "ptz-visca.cpp"):
        body = re.search(r"bool \w+::runTrigger\(const QString &name\)\s*\{(.*?)\n\}\n", (src / path).read_text(),
                         re.DOTALL).group(1)
        in_source |= set(re.findall(r'name == "(\w+)"', body))
    table = re.search(r"visca_triggers = \{(.*?)\};", (src / "ptz-visca-commands.cpp").read_text(), re.DOTALL).group(1)
    in_source |= set(re.findall(r'\{"(\w+)",', table))
    in_doc = {name for row in table_rows("Triggers") for name in names(row[0])}
    assert in_source, "found no triggers in the source"
    assert in_doc == in_source
