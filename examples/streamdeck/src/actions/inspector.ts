/*
 * What the property inspectors' menus are filled with. An sdpi-components
 * menu with a datasource asks the plugin for its items by sending
 * {event: <datasource>}, and is answered, or updated later, with
 * {event: <datasource>, items: [{value, label}]}.
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

import streamDeck from "@elgato/streamdeck";

import { ptz } from "../obs-ptz";

/** What a menu sends to ask for its items: the name of its datasource */
export type InspectorRequest = { event: string; isRefresh?: boolean };

type Item = { value: string; label: string; disabled?: boolean };

function unavailable(error: unknown): Item[] {
	const label = ptz.connected ? `Could not ask OBS: ${error instanceof Error ? error.message : error}` : "Not connected to OBS";
	return [{ value: "", label, disabled: true }];
}

/** Each camera, by the source it is on: a device's own name is its filter's, which is the same for each */
export async function sendCameras(): Promise<void> {
	let items: Item[];
	try {
		items = (await ptz.cameras()).map((c) => ({ value: c.device, label: c.source || c.name }));
	} catch (e) {
		items = unavailable(e);
	}
	await streamDeck.ui.sendToPropertyInspector({ event: "getCameras", items });
}

/** Fills the open property inspector's menus again, if there is one: for when OBS comes or goes */
export async function refreshInspector(): Promise<void> {
	const open = streamDeck.ui.action;
	if (!open) {
		return;
	}
	await sendCameras();
	if (open.manifestId === "ca.secretlab.obs-ptz.preset") {
		await sendPresets(((await open.getSettings()) as { device?: string }).device);
	}
}

export async function sendPresets(device: string | undefined): Promise<void> {
	let items: Item[];
	if (!device) {
		items = [{ value: "", label: "Choose a camera first", disabled: true }];
	} else {
		try {
			items = (await ptz.presets(device)).map((p) => ({ value: p.id, label: p.name || p.id }));
		} catch (e) {
			items = unavailable(e);
		}
	}
	await streamDeck.ui.sendToPropertyInspector({ event: "getPresets", items });
}
