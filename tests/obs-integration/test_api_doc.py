"""Holds docs/ptz-device-api.md to the plugin.

The document is the specification of the PTZ API, and is written by hand, so
this test is what keeps it true. It reads the document's structure:

 * a "###" heading in a handler section, `` ### `void ptz_stop()` ``, is a proc or
   a signal, by the section it is in, with the declaration it is registered as;
 * a table in "State keys", "Config keys", "Features" or "Triggers" lists, in the
   backticks of its first column, the names it documents, and for the keys,
   the Type column and, for the state, the Present column.

and checks it against:

 * what the plugin registered: in a build with the UI test harness,
   ptz_proc_add() and ptz_signal_add() (src/core/ptz-device.cpp) log every
   proc and signal, a device's (PTZDevice) and a discovery instance's
   (src/core/ptz-discovery-kind.cpp), as an "[ptz-api] <scope> <declaration>"
   line in OBS's log, once per driver module, each of which has its own. The
   lines of all of them, once tests/ui-harness/api-version-test.cpp's
   "make_discovery_instances" test has made an instance of each discovery
   kind so that theirs are there too, are what is registered, so a proc added
   in the code and not here fails, and so does one here and not in the code;
 * real devices' state and config, whose keys have to be documented, and
   present, with the documented types;
 * the source, for the feature and trigger names, which the plugin doesn't
   register anywhere it can be asked;
 * what the API says of itself, in ptz_get_api_info: the declarations, and for
   each proc the thread it may be called from and what a remote caller needs to
   be let do to call it, which are a line each in the proc's section,
   "Any thread." or "Device thread.", and "Access: read." or "Access: none."
   where it is not the control that most procs are.
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
    "Per-discovery proc_handler": "discovery-proc",
    "Per-discovery signal_handler": "discovery-signal",
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


def documented_procs(title):
    """{declaration: (thread, access)} for a proc_handler section, from each
    proc's own "###" part of it: "any" or "device" by its "Any thread." or
    "Device thread." line, None if it has neither, and "read", "none" or, with
    no "Access:" line, "control"""
    parts = re.split(r"^### `(.+)`$", sections()[title], flags=re.MULTILINE)
    found = {}
    for declaration, body in zip(parts[1::2], parts[2::2]):
        thread = re.search(r"^(Any|Device) thread\.$", body, flags=re.MULTILINE)
        access = re.search(r"^Access: (read|none)\.$", body, flags=re.MULTILINE)
        found[declaration] = (thread.group(1).lower() if thread else None, access.group(1) if access else "control")
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


def registered_api(log):
    """{scope: {declaration, ...}} from the "[ptz-api]" lines in OBS's log, of
    every device and module"""
    registered = {}
    for scope, signature in re.findall(r"\[ptz-api\] (\S+) (.+)$", log.read_text(errors="replace"),
                                       flags=re.MULTILINE):
        registered.setdefault(scope, set()).add(signature.strip())
    return registered


def test_the_doc_lists_what_the_plugin_registers(obs_world, tmp_path):
    out = tmp_path / "made.json"
    obs_world.run_ui_test("make_discovery_instances", filename=str(out))
    obs_world.wait_for(out.exists)
    assert json.loads(out.read_text())["made"], "made no discovery instance"

    documented = {scope: set(signatures) for scope, signatures in documented_signatures().items()}
    # The log is written as the instances register
    obs_world.wait_for(lambda: {"discovery-proc", "discovery-signal"} <= set(registered_api(obs_world.obs_log())))
    registered = registered_api(obs_world.obs_log())

    assert set(registered) == set(documented)
    for scope in documented:
        assert registered[scope] - documented[scope] == set(), f"{scope}: registered, and not in the doc"
        assert documented[scope] - registered[scope] == set(), f"{scope}: in the doc, and not registered"


# --- what the API says of itself --------------------------------------------------


def api_infos(obs_world, tmp_path):
    """{"device": ..., "discovery": ...}: what ptz_get_api_info returns, of a device
    and of a discovery instance"""
    device = obs_world.call_proc(obs_world.device_names["visca-tcp"], "ptz_get_api_info", returns="data")
    assert device["called"], "a device has no ptz_get_api_info"
    out = tmp_path / "made.json"
    obs_world.run_ui_test("make_discovery_instances", filename=str(out))
    obs_world.wait_for(out.exists)
    made = json.loads(out.read_text())["made"]
    assert made and all(m.get("api") for m in made), "a discovery instance has no ptz_get_api_info"
    # every kind's is the same: they all get it from ptz_discovery_add_api()
    assert all(m["api"] == made[0]["api"] for m in made)
    return {"device": device["return"], "discovery": made[0]["api"]}


@pytest.mark.parametrize("kind,procs,signals", [
    ("device", "Per-device proc_handler", "Per-device signal_handler"),
    ("discovery", "Per-discovery proc_handler", "Per-discovery signal_handler"),
])
def test_api_info_lists_what_the_doc_lists(obs_world, tmp_path, kind, procs, signals):
    info = api_infos(obs_world, tmp_path)[kind]
    assert {p["decl"] for p in info["procs"]} == set(documented_procs(procs))
    assert {s["decl"] for s in info["signals"]} == set(documented_signatures()[SCOPES[signals]])
    for entry in info["procs"] + info["signals"]:
        assert re.search(r" %s\(" % re.escape(entry["name"]), entry["decl"]), entry


@pytest.mark.parametrize("kind,title", [
    ("device", "Per-device proc_handler"),
    ("discovery", "Per-discovery proc_handler"),
])
def test_each_proc_is_tagged_as_the_doc_says(obs_world, tmp_path, kind, title):
    documented = documented_procs(title)
    info = api_infos(obs_world, tmp_path)[kind]
    for proc in info["procs"]:
        thread, access = documented[proc["decl"]]
        assert thread is not None, f"{proc['name']}: the doc doesn't say which thread"
        assert proc["thread"] == thread, proc["name"]
        assert proc["access"] == access, proc["name"]


def test_the_procs_a_remote_caller_may_only_read_with(obs_world, tmp_path):
    """The ones that neither move the camera nor change what the device keeps"""
    info = api_infos(obs_world, tmp_path)["device"]
    by_access = {}
    for proc in info["procs"]:
        by_access.setdefault(proc["access"], set()).add(proc["name"])
    assert by_access["read"] == {"ptz_get_api_version", "ptz_get_api_info", "ptz_get_state", "ptz_get_statistics",
                                 "ptz_get_camera_report", "ptz_preset_get_list"}
    assert by_access["none"] == {"ptz_get_parent_source"}


# --- the binding to obs-websocket ------------------------------------------------

BINDING = "WebSocket binding"


def binding_table(heading):
    """The names in the first column of the table under a "###" heading of the binding's section"""
    parts = re.split(r"^### (.+)$", sections()[BINDING], flags=re.MULTILINE)
    body = dict(zip(parts[1::2], parts[2::2]))[heading]
    rows = [line.strip().strip("|").split("|") for line in body.splitlines() if line.startswith("|")][2:]
    return {name for row in rows for name in names(row[0])}


def test_the_bridge_exposes_what_the_doc_lists(obs_world):
    """Every proc and signal the document has is a request or an event of the
    vendor, and nothing else is, but for the procs that can't be called from
    outside OBS"""
    # A discovery instance's are there once there is one
    assert obs_world.ws.vendor("obs-ptz", "ptz_list_discovery")["instances"]
    exposed = obs_world.ws.vendor("obs-ptz", "ptz_list_api")
    for title, scope in SCOPES.items():
        if scope.endswith("-proc"):
            documented = {re.search(r" (\w+)\(", decl).group(1)
                          for decl, (_, access) in documented_procs(title).items() if access != "none"}
        else:
            documented = {re.search(r" (\w+)\(", decl).group(1) for decl in documented_signatures()[scope]}
        assert {entry["name"] for entry in exposed[scope]} == documented, scope


def test_the_doc_lists_the_bindings_own_requests_and_events(obs_world):
    source = (REPO_ROOT / "src" / "controller" / "ptz-websocket.cpp").read_text()
    requests = set(re.findall(r'addRequest\("(\w+)"', source))
    assert requests and binding_table("Requests of the binding's own") == requests
    events = set(re.findall(r'emitEvent\("(\w+)"', source))
    assert events and binding_table("Events of the binding's own") == events
    # and each of them is really there
    for request in requests:
        obs_world.ws.vendor("obs-ptz", request)


def test_the_doc_lists_the_errors_in_the_source():
    source = (REPO_ROOT / "src" / "controller" / "ptz-websocket.cpp").read_text()
    in_source = set(re.findall(r'setError\(response, "(\w+)"', source))
    assert in_source and binding_table("Errors") == in_source


def test_the_doc_says_how_a_client_reaches_the_binding():
    body = sections()[BINDING]
    for said in ("`obs-ptz`", "`CallVendorRequest`", "`device`", "`VendorEvent`", "`websocket_access`",
                 "`off`", "`read-only`", "`control`"):
        assert said in body, said
    # the subscription a client has to ask for, which is not in what it gets by default
    assert "Vendors" in body and "1 << 9" in body
    # the version of the binding, which the plugin answers with
    major, minor = re.search(r"version \*\*(\d+)\.(\d+)\*\* of the binding", body).groups()
    source = (REPO_ROOT / "src" / "controller" / "ptz-websocket.cpp").read_text()
    assert re.search(r"kBridgeVersionMajor = %s;" % major, source)
    assert re.search(r"kBridgeVersionMinor = %s;" % minor, source)


def test_the_doc_says_how_json_becomes_calldata():
    """One row for each type a declaration can have"""
    assert binding_table("Fields") == {"string", "int", "float", "bool", "ptr"}


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
    source = (REPO_ROOT / "src" / "core" / "ptz-device.cpp").read_text()
    table = re.search(r"PTZDevice::featureNames\(\)\s*\{.*?names = \{(.*?)\};", source, re.DOTALL).group(1)
    in_source = set(re.findall(r'\{\w+, "(\w+)"\}', table))
    in_doc = {name for row in table_rows("Features") for name in names(row[0])}
    assert in_source, "found no features in the source"
    assert in_doc == in_source


def test_the_doc_lists_the_triggers_in_the_source():
    src = REPO_ROOT / "src"
    in_source = set()
    for path in ("core/ptz-device.cpp", "drivers/visca/ptz-visca.cpp"):
        body = re.search(r"bool \w+::runTrigger\(const QString &name\)\s*\{(.*?)\n\}\n", (src / path).read_text(),
                         re.DOTALL).group(1)
        in_source |= set(re.findall(r'name == "(\w+)"', body))
    table = re.search(r"visca_triggers = \{(.*?)\};", (src / "drivers" / "visca" / "ptz-visca-commands.cpp").read_text(), re.DOTALL).group(1)
    in_source |= set(re.findall(r'\{"(\w+)",', table))
    in_doc = {name for row in table_rows("Triggers") for name in names(row[0])}
    assert in_source, "found no triggers in the source"
    assert in_doc == in_source
