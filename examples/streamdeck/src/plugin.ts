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
import { cameraStates } from "./camera-state";
import { CommandAction } from "./actions/command";
import { StatusAction } from "./actions/status";
import { AutofocusAction } from "./actions/autofocus";
import { LensAction } from "./actions/lens";
import { ConnectionSettings, ptz } from "./obs-ptz";

const presets = new PresetAction();
const lens = new LensAction();
const autofocus = new AutofocusAction();
const commands = new CommandAction();
const status = new StatusAction();
const warn = (e: unknown) => streamDeck.logger.warn(String(e));
ptz.onEvent = (name, data) => {
	// The state the keys go by is brought up to date before they are told
	if (name === "ptz_state_changed") {
		cameraStates.changed(data.device as string, data.changed as Record<string, unknown>);
	} else if (name === "ptz_device_removed") {
		cameraStates.delete(data.device as string);
	}
	commands.event(name, data).catch(warn);
	status.event(name, data).catch(warn);
	presets.event(name, data).catch(warn);
	lens.event(name, data).catch(warn);
	autofocus.event(name, data).catch(warn);
};
ptz.onConnectionChanged = (connected) => {
	streamDeck.logger.info(connected ? "connected to OBS" : "not connected to OBS");
	cameraStates.clear();
	commands.connectionChanged(connected).catch(warn);
	status.connectionChanged(connected).catch(warn);
	presets.connectionChanged(connected).catch(warn);
	lens.connectionChanged(connected).catch(warn);
	autofocus.connectionChanged(connected).catch(warn);
	refreshInspector().catch(warn);
};
ptz.onSelectionChanged = () => {
	commands.selectionChanged().catch(warn);
	status.selectionChanged().catch(warn);
	presets.selectionChanged().catch(warn);
	lens.selectionChanged().catch(warn);
	autofocus.selectionChanged().catch(warn);
	refreshInspector().catch(warn);
};

streamDeck.actions.registerAction(new MoveAction());
streamDeck.actions.registerAction(presets);
streamDeck.actions.registerAction(lens);
streamDeck.actions.registerAction(autofocus);
streamDeck.actions.registerAction(commands);
streamDeck.actions.registerAction(status);

// Where OBS is, which every key's property inspector shows and changes
streamDeck.settings.onDidReceiveGlobalSettings<ConnectionSettings>((ev) => ptz.configure(ev.settings));

await streamDeck.connect();
ptz.configure(await streamDeck.settings.getGlobalSettings<ConnectionSettings>());
