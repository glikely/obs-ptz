"""Covers what the tests' own obs-websocket client (obsws.py) does with events,
and that an event a plugin emits as its vendor reaches a client that subscribed
to obs-websocket's Vendors events, and no other, which is how the PTZ API's
signals are to reach one.

Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
SPDX-License-Identifier: GPL-2.0-or-later

Driven through tests/ui-harness/vendor-event-test.cpp's "emit_vendor_event"
test, which emits the event it is asked for as the harness's own vendor, in
the request, before it is answered.
"""


from conftest import HARNESS_VENDOR


def is_probe(name):
    return lambda d: d.get("vendorName") == HARNESS_VENDOR and d.get("eventType") == name


def test_vendor_event_reaches_a_vendors_subscriber(obs_world):
    obs_world.run_ui_test("emit_vendor_event", type="ptz_probe", data='{"n": 7}')
    event = obs_world.events.wait_event("VendorEvent", is_probe("ptz_probe"))
    assert event["eventData"] == {"n": 7}


def test_call_does_not_lose_an_event_seen_while_it_waits(obs_world):
    """An event that comes on the connection a request is waiting on, before the
    request's answer or after, is still there for wait_event()"""
    client = obs_world.events
    for i in range(20):
        obs_world.run_ui_test("emit_vendor_event", connection=client, type="ptz_probe_kept", data='{"n": %d}' % i)
        # Something else asked on the same connection, while the event is on its way
        client.call("GetVersion")
    seen = [client.wait_event("VendorEvent", is_probe("ptz_probe_kept"))["eventData"]["n"] for _ in range(20)]
    assert seen == list(range(20))


def test_vendor_raises_on_an_error_in_the_response(obs_world):
    import pytest
    from obsws import ObsWebSocketError

    with pytest.raises(ObsWebSocketError) as raised:
        obs_world.ws.vendor("obs-ptz", "ptz_stop", {"device": "no such device"})
    assert raised.value.status["error"] == "unknown_device"
    assert "no such device" in str(raised.value)


def test_vendor_event_does_not_reach_a_general_subscriber(obs_world):
    """obs-websocket has a subscription of its own for vendors' events, so a
    client has to ask for it: the General one, which is in the default, is
    not enough"""
    import pytest
    from obsws import EVENT_GENERAL, Client

    general = Client(obs_world.ws_url, password=obs_world.ws_password, event_subscriptions=EVENT_GENERAL)
    try:
        obs_world.run_ui_test("emit_vendor_event", type="ptz_probe_general", data="{}")
        obs_world.events.wait_event("VendorEvent", is_probe("ptz_probe_general"))
        with pytest.raises(TimeoutError):
            general.wait_event("VendorEvent", is_probe("ptz_probe_general"), timeout=1)
    finally:
        general.close()


def test_events_nobody_reads_do_not_stop_the_connection(obs_world):
    """A test that doesn't look at the events for a while, as most never do, is
    still sent the ones that come after: what is unread must not keep the
    connection from answering its pings, which closes it"""
    import time
    from obsws import EVENT_VENDORS, Client

    client = Client(obs_world.ws_url, password=obs_world.ws_password, event_subscriptions=EVENT_VENDORS, keepalive=1)
    try:
        for i in range(100):
            obs_world.run_ui_test("emit_vendor_event", type="ptz_probe_unread", data='{"n": %d}' % i)
        time.sleep(5)
        obs_world.run_ui_test("emit_vendor_event", type="ptz_probe_after", data="{}")
        client.wait_event("VendorEvent", is_probe("ptz_probe_after"))
        assert client.vendor("obs-ptz", "ptz_bridge_version")["major"] == 1
    finally:
        client.close()
