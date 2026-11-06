"""Holds docs/ptz-device-api.md to the plugin.

The document is the specification of the PTZ API, and is written by hand, so
this test is what keeps it true. It reads the document's structure:

 * a "###" heading in a handler section, `` ### `void ptz_stop()` ``, is a proc or
   a signal, by the section it is in, with the declaration it is registered as;
 * a table in "State keys", "Config keys", "Features" or "Triggers" lists, in the
   backticks of its first column, the names it documents, and for the keys,
   the Type column and, for the state, the Present column.

and checks it against:

 * the plugin's tables of the API (src/ptz-api.cpp), which the UI
   harness's api_tables gives, as it links them too, and of which the
   websocket binding's ptz_list_api gives a device's: every proc and signal,
   with its declaration
   and, for a proc, what a remote caller needs to be let do to call it, which
   is a line in the proc's section, "Access: read." or "Access: none.", where
   it is not the control that most procs are. So a proc added to a table
   and not here fails, and so does one here and not in its table. Every proc
   may be called from any thread, which "Rules for calling" says once, and no
   proc's section says again;
 * what the plugin registers: ptz_proc_add() and ptz_signal_add()
   (src/core/ptz-device.cpp) log an error for a proc or signal that its table
   hasn't, a device's (PTZDevice) or a discovery instance's
   (src/core/ptz-discovery-kind.cpp), which no device or discovery instance
   of any kind logs, once the UI harness's discover has made one of each;
 * real devices' state and config, whose keys have to be documented, and
   present, with the documented types;
 * the source, for the feature and trigger names, which the plugin doesn't
   register anywhere it can be asked.
"""

import json
import re
from pathlib import Path

import pytest
from obsws import ObsWebSocketError

REPO_ROOT = Path(__file__).resolve().parents[2]
DOC = REPO_ROOT / "docs" / "ptz-device-api.md"

# The handler sections of the document, by what the plugin calls their scope
SCOPES = {
    "Per-device proc_handler": "device-proc",
    "Per-device signal_handler": "device-signal",
    "Per-discovery proc_handler": "discovery-proc",
    "Per-discovery signal_handler": "discovery-signal",
}

# obs-websocket's RequestStatus::ResourceNotFound, for a request nothing registered
NOT_FOUND = 600

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
    """{declaration: access} for a proc_handler section, from each proc's own
    "###" part of it: "read", "none" or, with no "Access:" line, "control"""
    parts = re.split(r"^### `(.+)`$", sections()[title], flags=re.MULTILINE)
    found = {}
    for declaration, body in zip(parts[1::2], parts[2::2]):
        access = re.search(r"^Access: (read|none)\.$", body, flags=re.MULTILINE)
        found[declaration] = access.group(1) if access else "control"
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


def make_discovery_instances(obs_world, tmp_path):
    """Has the UI harness make a discovery instance of each kind, which
    registers its procs and signals, and let it go once it has finished: the
    websocket binding has none"""
    out = tmp_path / "discover.json"
    obs_world.run_ui_test("discover", filename=str(out))
    obs_world.wait_for(out.exists, timeout=20)
    assert json.loads(out.read_text())["instances"], "made no discovery instance"


def api_table(obs_world, tmp_path):
    """{scope: [entry, ...]}: the plugin's tables of the API, as the UI harness,
    which links them too, gives them"""
    out = tmp_path / "api-tables.json"
    obs_world.run_ui_test("api_tables", filename=str(out))
    obs_world.wait_for(out.exists, timeout=10)
    listed = json.loads(out.read_text())
    return {scope: listed[scope] for scope in SCOPES.values()}


def test_the_doc_lists_what_the_table_has(obs_world, tmp_path):
    documented = {scope: set(signatures) for scope, signatures in documented_signatures().items()}
    table = {scope: {entry["decl"] for entry in entries}
             for scope, entries in api_table(obs_world, tmp_path).items()}

    assert set(table) == set(documented)
    for scope in documented:
        assert table[scope] - documented[scope] == set(), f"{scope}: in the table, and not in the doc"
        assert documented[scope] - table[scope] == set(), f"{scope}: in the doc, and not in the table"


def test_the_table_names_each_entry_by_its_declaration(obs_world, tmp_path):
    for scope, entries in api_table(obs_world, tmp_path).items():
        assert len({entry["name"] for entry in entries}) == len(entries), f"{scope} has one twice"
        for entry in entries:
            assert re.search(r" %s\(" % re.escape(entry["name"]), entry["decl"]), entry


def test_the_plugin_registers_nothing_the_table_hasnt(obs_world, tmp_path):
    """Every device of the plugin, and a discovery instance of each kind, has
    registered its procs and signals by now, and ptz_proc_add() and
    ptz_signal_add() said nothing of any of them"""
    assert obs_world.ws.vendor("obs-ptz", "ptz_list_devices")["devices"]
    make_discovery_instances(obs_world, tmp_path)
    log = obs_world.obs_log().read_text(errors="replace")
    assert "is not in the PTZ API table" not in log


@pytest.mark.parametrize("title", ["Per-device proc_handler", "Per-discovery proc_handler"])
def test_each_proc_is_tagged_as_the_doc_says(obs_world, tmp_path, title):
    documented = documented_procs(title)
    for proc in api_table(obs_world, tmp_path)[SCOPES[title]]:
        assert proc["access"] == documented[proc["decl"]], proc["name"]


def test_the_procs_a_remote_caller_may_only_read_with(obs_world, tmp_path):
    """The ones that neither move the camera nor change what the device keeps"""
    by_access = {}
    for proc in api_table(obs_world, tmp_path)["device-proc"]:
        by_access.setdefault(proc["access"], set()).add(proc["name"])
    assert by_access["read"] == {"ptz_get_api_version", "ptz_get_state", "ptz_get_statistics",
                                 "ptz_get_camera_report", "ptz_preset_get_list", "ptz_preset_get_thumbnail"}
    assert by_access["none"] == {"ptz_get_parent_source"}


# --- the binding to obs-websocket ------------------------------------------------

BINDING = "WebSocket binding"


def binding_table(heading):
    """The names in the first column of the table under a "###" heading of the binding's section"""
    parts = re.split(r"^### (.+)$", sections()[BINDING], flags=re.MULTILINE)
    body = dict(zip(parts[1::2], parts[2::2]))[heading]
    rows = [line.strip().strip("|").split("|") for line in body.splitlines() if line.startswith("|")][2:]
    return {name for row in rows for name in names(row[0])}


def test_the_bridge_lists_a_devices_table(obs_world, tmp_path):
    """ptz_list_api gives a device's tables, and only those: discovery is not on the binding"""
    table = api_table(obs_world, tmp_path)
    assert obs_world.ws.vendor("obs-ptz", "ptz_list_api") == {
        "device-proc": table["device-proc"], "device-signal": table["device-signal"]}


def test_the_bridge_has_a_request_for_each_proc_the_doc_lists(obs_world):
    """Every proc of a device the document has is a request of the vendor, but
    for the procs that can't be called from outside OBS, which aren't, and a
    discovery instance's aren't either: a request there is answers that it has
    no such device, and obs-websocket answers one that isn't with its own
    ResourceNotFound"""
    device_procs = documented_procs("Per-device proc_handler")
    for title in ("Per-device proc_handler", "Per-discovery proc_handler"):
        for decl, access in documented_procs(title).items():
            name = re.search(r" (\w+)\(", decl).group(1)
            with pytest.raises(ObsWebSocketError) as raised:
                obs_world.ws.vendor("obs-ptz", name, {"device": "nope"})
            if device_procs.get(decl, "none") == "none":
                assert raised.value.status.get("code") == NOT_FOUND, name
            else:
                assert raised.value.status.get("error") == "unknown_device", name


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
    assert in_source, "found no features in the source"
    # ...and the ones saveFeatures() says for two of them together
    pairs = re.search(r"PTZDevice::saveFeatures\(.*?pairs = \{(.*?)\};", source, re.DOTALL).group(1)
    in_source |= set(re.findall(r'\{\w+ \| \w+, "(\w+)"\}', pairs))
    in_doc = {name for row in table_rows("Features") for name in names(row[0])}
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
