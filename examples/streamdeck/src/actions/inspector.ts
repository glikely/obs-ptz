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

import { ptz, SELECTED } from "../obs-ptz";

/** What a menu sends to ask for its items: the name of its datasource */
export type InspectorRequest = { event: string; isRefresh?: boolean };

type Item = { value: string; label: string; disabled?: boolean };

/** How many presets a key on the selected camera can be set to, at least: as many as the dock has hotkeys for */
const PRESET_POSITIONS = 16;

/** The camera a key's settings name: SELECTED unless they name one */
export function cameraOf(settings: { device?: string }): string {
	return settings.device || SELECTED;
}

function unavailable(error: unknown): Item[] {
	const label = ptz.connected ? `Could not ask OBS: ${error instanceof Error ? error.message : error}` : "Not connected to OBS";
	return [{ value: "", label, disabled: true }];
}

/**
 * The selected camera, and each camera by the source it is on: a device's own
 * name is its filter's, which is the same for each
 */
export async function sendCameras(): Promise<void> {
	let items: Item[] = [{ value: SELECTED, label: "Selected camera" }];
	try {
		items = items.concat((await ptz.cameras()).map((c) => ({ value: c.device, label: c.source || c.name })));
	} catch (e) {
		items = items.concat(unavailable(e));
	}
	await streamDeck.ui.sendToPropertyInspector({ event: "getCameras", items });
}

/**
 * A camera's presets, by id. The selected camera's are by their position in
 * the dock's list instead, as the dock's hotkeys are, since which presets
 * there are changes with the camera: each named for the preset the camera
 * selected now has there, so that what is chosen can be seen.
 */
export async function sendPresets(device: string): Promise<void> {
	let items: Item[];
	if (device === SELECTED) {
		let names: string[] = [];
		try {
			names = ptz.selected ? (await ptz.presets(SELECTED)).presets.map((p) => p.name || p.id) : [];
		} catch {
			// The positions can be chosen without a camera to show them on
		}
		items = Array.from({ length: Math.max(PRESET_POSITIONS, names.length) }, (_, i) => ({
			value: String(i + 1),
			label: names[i] ? `${i + 1}: ${names[i]}` : String(i + 1),
		}));
	} else {
		try {
			items = (await ptz.presets(device)).presets.map((p) => ({ value: p.id, label: p.name || p.id }));
		} catch (e) {
			items = unavailable(e);
		}
	}
	await streamDeck.ui.sendToPropertyInspector({ event: "getPresets", items });
}

/** Fills the open property inspector's menus again, if there is one: for when OBS, or the selection, changes */
export async function refreshInspector(): Promise<void> {
	const open = streamDeck.ui.action;
	if (!open) {
		return;
	}
	await sendCameras();
	if (open.manifestId === "ca.secretlab.obs-ptz.preset") {
		await sendPresets(cameraOf((await open.getSettings()) as { device?: string }));
	}
}
