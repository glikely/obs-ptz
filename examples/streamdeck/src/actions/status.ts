/*
 * A key that shows where a camera is: its pan, tilt, zoom and focus as its title,
 * from the positions the camera reports, and whether the camera is there to
 * ask at all: it goes red, and says so, when the camera's link is down or OBS
 * can't be reached. The camera is the one selected in the PTZ dock, unless the
 * key names one.
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

import streamDeck, {
	action,
	DidReceiveSettingsEvent,
	KeyAction,
	SendToPluginEvent,
	SingletonAction,
	WillAppearEvent,
} from "@elgato/streamdeck";

import { cameraStates } from "../camera-state";
import { Data, ptz, SELECTED } from "../obs-ptz";
import { GROUND, INK, keyPicture, OFFLINE_GROUND } from "../pictures";
import { cameraOf, InspectorRequest, sendCameras } from "./inspector";
import { Axes, show } from "../units";

type StatusSettings = {
	/** The camera's UUID, or SELECTED (or nothing) for the one selected in the dock */
	device?: string;
};

const LIVE = keyPicture("", GROUND, INK);
const OFFLINE = keyPicture("", OFFLINE_GROUND, INK);

@action({ UUID: "ca.secretlab.obs-ptz.status" })
export class StatusAction extends SingletonAction<StatusSettings> {
	override async onWillAppear(ev: WillAppearEvent<StatusSettings>): Promise<void> {
		if (ev.action.isKey()) {
			await this.#refresh(ev.action, ev.payload.settings);
		}
	}

	override async onDidReceiveSettings(ev: DidReceiveSettingsEvent<StatusSettings>): Promise<void> {
		if (ev.action.isKey()) {
			await this.#refresh(ev.action, ev.payload.settings);
		}
	}

	override async onSendToPlugin(ev: SendToPluginEvent<InspectorRequest, StatusSettings>): Promise<void> {
		if (ev.payload.event === "getCameras") {
			await sendCameras();
		}
	}

	/** Called when the connection to OBS is made, and when it is lost */
	async connectionChanged(_connected: boolean): Promise<void> {
		await this.#refreshAll();
	}

	/** Called when another camera is selected in the dock */
	async selectionChanged(): Promise<void> {
		await this.#refreshAll();
	}

	/** Called with each of obs-ptz's events, after cameraStates has them */
	async event(name: string, data: Data): Promise<void> {
		// Only what the key shows: the camera says much besides
		const shown = ["pan", "tilt", "zoom", "focus", "axes", "connected"];
		if (name === "ptz_device_removed" || (name === "ptz_state_changed" && shown.some((key) => key in (data.changed as Data)))) {
			await this.#refreshAll(data.device as string);
		}
	}

	#deviceOf(settings: StatusSettings): string {
		const camera = cameraOf(settings);
		return camera === SELECTED ? ptz.selected : camera;
	}

	async #refreshAll(device?: string): Promise<void> {
		for (const key of this.actions) {
			if (!key.isKey()) {
				continue;
			}
			const settings = await key.getSettings();
			if (device === undefined || this.#deviceOf(settings) === device) {
				await this.#refresh(key, settings);
			}
		}
	}

	async #refresh(key: KeyAction<StatusSettings>, settings: StatusSettings): Promise<void> {
		const device = ptz.connected ? this.#deviceOf(settings) : "";
		let title = "No OBS";
		let live = false;
		if (device) {
			try {
				const state = await cameraStates.get(device);
				live = state.connected !== false;
				title = live
					? ["Pan", "Tilt", "Zoom", "Focus"]
							.map((name) => {
								const axis = name.toLowerCase();
								const position = state[axis];
								const text = typeof position === "number" ? show((state.axes as Axes | undefined)?.[axis], position, axis === "pan" || axis === "tilt") : "-";
								return `${name} ${text}`;
							})
							.join("\n")
					: "Camera\noffline";
			} catch {
				title = "No camera";
			}
		} else if (ptz.connected) {
			title = "No camera";
		}
		await key.setImage(live ? LIVE : OFFLINE);
		await key.setTitle(title);
	}
}
