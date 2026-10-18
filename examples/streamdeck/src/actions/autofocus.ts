/*
 * A key that turns a camera's autofocus on and off, and lights up (its second
 * state) while it is on, from the focus_af_enabled state the camera reports.
 * The camera is the one selected in the PTZ dock, unless the key names one.
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

import streamDeck, {
	action,
	DidReceiveSettingsEvent,
	KeyAction,
	KeyDownEvent,
	SendToPluginEvent,
	SingletonAction,
	WillAppearEvent,
} from "@elgato/streamdeck";

import { Data, ptz, SELECTED } from "../obs-ptz";
import { cameraOf, InspectorRequest, sendCameras } from "./inspector";

type AutofocusSettings = {
	/** The camera's UUID, or SELECTED (or nothing) for the one selected in the dock */
	device?: string;
};

@action({ UUID: "ca.secretlab.obs-ptz.autofocus" })
export class AutofocusAction extends SingletonAction<AutofocusSettings> {
	// Whether each camera's autofocus is on, by the device's UUID, as last known
	#on = new Map<string, boolean>();

	override async onWillAppear(ev: WillAppearEvent<AutofocusSettings>): Promise<void> {
		if (ev.action.isKey()) {
			await this.#refresh(ev.action, ev.payload.settings);
		}
	}

	override async onDidReceiveSettings(ev: DidReceiveSettingsEvent<AutofocusSettings>): Promise<void> {
		if (ev.action.isKey()) {
			await this.#refresh(ev.action, ev.payload.settings);
		}
	}

	override async onKeyDown(ev: KeyDownEvent<AutofocusSettings>): Promise<void> {
		const device = this.#deviceOf(ev.payload.settings);
		try {
			if (!device) {
				throw new Error("no camera is selected");
			}
			// Asked, not assumed, so that a key pressed before the camera's first report toggles what is true
			const state = (await ptz.call("ptz_get_state", { device })).state as Data;
			if (!(state.features as Data | undefined)?.autofocus) {
				throw new Error("the camera has no autofocus");
			}
			await ptz.call("ptz_request_state", { device, state: { focus_af_enabled: state.focus_af_enabled !== true } });
		} catch (e) {
			streamDeck.logger.warn(String(e));
			await ev.action.showAlert();
		}
	}

	override async onSendToPlugin(ev: SendToPluginEvent<InspectorRequest, AutofocusSettings>): Promise<void> {
		if (ev.payload.event === "getCameras") {
			await sendCameras();
		}
	}

	/** Called when the connection to OBS is made, and when it is lost */
	async connectionChanged(_connected: boolean): Promise<void> {
		this.#on.clear();
		await this.#refreshAll();
	}

	/** Called when another camera is selected in the dock: the keys on the selected camera follow it */
	async selectionChanged(): Promise<void> {
		await this.#refreshAll();
	}

	/** Called with each of obs-ptz's events */
	async event(name: string, data: Data): Promise<void> {
		const device = data.device as string;
		if (name === "ptz_state_changed") {
			const on = (data.changed as Data).focus_af_enabled;
			if (typeof on !== "boolean") {
				return;
			}
			this.#on.set(device, on);
		} else if (name === "ptz_device_removed") {
			this.#on.delete(device);
		} else {
			return;
		}
		await this.#refreshAll(device);
	}

	#deviceOf(settings: AutofocusSettings): string {
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

	async #refresh(key: KeyAction<AutofocusSettings>, settings: AutofocusSettings): Promise<void> {
		const device = ptz.connected ? this.#deviceOf(settings) : "";
		let on = false;
		if (device) {
			if (!this.#on.has(device)) {
				try {
					const state = (await ptz.call("ptz_get_state", { device })).state as Data;
					this.#on.set(device, state.focus_af_enabled === true);
				} catch (e) {
					streamDeck.logger.warn(String(e));
				}
			}
			on = this.#on.get(device) === true;
		}
		await key.setState(on ? 1 : 0);
	}
}
