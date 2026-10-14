/*
 * A Stream Deck plugin that moves the cameras of OBS's PTZ Controls and
 * recalls their presets, over obs-websocket. A sample of the PTZ API's
 * WebSocket binding (docs/ptz-device-api.md), not a finished product.
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

import streamDeck from "@elgato/streamdeck";

import { refreshInspector } from "./actions/inspector";
import { MoveAction } from "./actions/move";
import { PresetAction } from "./actions/preset";
import { ConnectionSettings, ptz } from "./obs-ptz";

const presets = new PresetAction();
const warn = (e: unknown) => streamDeck.logger.warn(String(e));
ptz.onEvent = (name, data) => presets.event(name, data).catch(warn);
ptz.onConnectionChanged = (connected) => {
	streamDeck.logger.info(connected ? "connected to OBS" : "not connected to OBS");
	presets.connectionChanged(connected).catch(warn);
	refreshInspector().catch(warn);
};

streamDeck.actions.registerAction(new MoveAction());
streamDeck.actions.registerAction(presets);

// Where OBS is, which every key's property inspector shows and changes
streamDeck.settings.onDidReceiveGlobalSettings<ConnectionSettings>((ev) => ptz.configure(ev.settings));

await streamDeck.connect();
ptz.configure(await streamDeck.settings.getGlobalSettings<ConnectionSettings>());
