/*
 * A key that recalls one of a camera's presets. Its title is the preset's name,
 * and it lights up (its second state) while the camera is at the preset: from
 * ptz_preset_recalled saying the camera arrived there until the camera's
 * position, as ptz_state_changed reports it, leaves where it arrived, whatever
 * moved it there and whatever moves it away.
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

import { Data, Preset, ptz } from "../obs-ptz";
import { InspectorRequest, sendCameras, sendPresets } from "./inspector";

type PresetSettings = {
	device?: string;
	preset?: string;
};

const AXES = ["pan", "tilt", "zoom"] as const;
// How far an axis can be from where the camera arrived, of its range, and the
// camera still be at the preset: a camera reports a little settling after it
// arrives
const AT_PRESET_SLACK = 0.02;

type Arrival = { preset: string; position: Partial<Record<(typeof AXES)[number], number>> };

@action({ UUID: "ca.secretlab.obs-ptz.preset" })
export class PresetAction extends SingletonAction<PresetSettings> {
	// The preset each camera is at, by the device's UUID, with where it arrived
	#at = new Map<string, Arrival>();

	override async onWillAppear(ev: WillAppearEvent<PresetSettings>): Promise<void> {
		if (ev.action.isKey()) {
			await this.#refresh(ev.action, ev.payload.settings);
		}
	}

	override async onKeyDown(ev: KeyDownEvent<PresetSettings>): Promise<void> {
		const { device, preset } = ev.payload.settings;
		try {
			if (!device || !preset) {
				throw new Error("the key has no camera or preset");
			}
			await ptz.call("ptz_preset_recall", { device, id: preset });
		} catch (e) {
			streamDeck.logger.warn(String(e));
			await ev.action.showAlert();
		}
	}

	// The camera or the preset was changed in the property inspector
	override async onDidReceiveSettings(ev: DidReceiveSettingsEvent<PresetSettings>): Promise<void> {
		await sendPresets(ev.payload.settings.device);
		if (ev.action.isKey()) {
			await this.#refresh(ev.action, ev.payload.settings);
		}
	}

	override async onSendToPlugin(ev: SendToPluginEvent<InspectorRequest, PresetSettings>): Promise<void> {
		const event = ev.payload.event;
		if (event === "getCameras") {
			await sendCameras();
		} else if (event === "getPresets") {
			await sendPresets((await ev.action.getSettings()).device);
		}
	}

	/** Called when the connection to OBS is made, and when it is lost */
	async connectionChanged(connected: boolean): Promise<void> {
		if (!connected) {
			this.#at.clear();
		}
		await this.#refreshAll();
	}

	/** Called with each of obs-ptz's events */
	async event(name: string, data: Data): Promise<void> {
		const device = data.device as string;
		switch (name) {
			case "ptz_preset_recalled": {
				// Where it arrived is what it has to stay near to be at the preset
				const state = (await ptz.call("ptz_get_state", { device })).state as Data;
				const position: Arrival["position"] = {};
				for (const axis of AXES) {
					if (typeof state[axis] === "number") {
						position[axis] = state[axis] as number;
					}
				}
				this.#at.set(device, { preset: data.id as string, position });
				await this.#refreshAll(device);
				break;
			}
			case "ptz_state_changed": {
				const at = this.#at.get(device);
				const changed = data.changed as Data;
				const left = AXES.some(
					(axis) =>
						typeof changed[axis] === "number" &&
						at?.position[axis] !== undefined &&
						Math.abs((changed[axis] as number) - at.position[axis]) > AT_PRESET_SLACK,
				);
				if (left) {
					this.#at.delete(device);
					await this.#refreshAll(device);
				}
				break;
			}
			// A preset's name, or the camera's presets, changed: the titles follow
			case "ptz_preset_changed":
			case "ptz_preset_added":
			case "ptz_preset_removed":
			case "ptz_preset_list_reset":
				await this.#refreshAll(device);
				break;
			case "ptz_device_removed":
				this.#at.delete(device);
				await this.#refreshAll(device);
				break;
		}
	}

	/** Sets each key's title and state, or only those of `device`'s presets */
	async #refreshAll(device?: string): Promise<void> {
		// Each camera's list is asked for once, however many keys it has
		const lists = new Map<string, Promise<Preset[]>>();
		for (const key of this.actions) {
			if (!key.isKey()) {
				continue;
			}
			const settings = await key.getSettings();
			if (device === undefined || settings.device === device) {
				await this.#refresh(key, settings, lists);
			}
		}
	}

	async #refresh(
		key: KeyAction<PresetSettings>,
		settings: PresetSettings,
		lists = new Map<string, Promise<Preset[]>>(),
	): Promise<void> {
		const { device, preset } = settings;
		let title = "";
		if (device && preset && ptz.connected) {
			try {
				if (!lists.has(device)) {
					lists.set(device, ptz.presets(device));
				}
				const found = (await lists.get(device)!).find((p) => p.id === preset);
				title = found ? found.name || found.id : "(gone)";
			} catch {
				title = "(no camera)";
			}
		}
		await key.setTitle(title);
		await key.setState(device && this.#at.get(device)?.preset === preset ? 1 : 0);
	}
}
