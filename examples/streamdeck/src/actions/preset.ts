/*
 * A key that recalls one of a camera's presets. Its title is the preset's name,
 * and it lights up (its second state) while the camera is at the preset: from
 * ptz_preset_recalled saying the camera arrived there until the camera's
 * position, as ptz_state_changed reports it, leaves where it arrived, whatever
 * moved it there and whatever moves it away.
 *
 * The camera is the one selected in the PTZ dock, unless the key names one.
 * Which presets there are changes with the camera selected, so a key on the
 * selected camera is for a position in the dock's list, as the dock's own
 * hotkeys are, and its title and light follow the camera that is selected.
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

import { Data, Preset, PresetList, ptz, SELECTED } from "../obs-ptz";
import { cameraOf, InspectorRequest, sendCameras, sendPresets } from "./inspector";

type PresetSettings = {
	/** The camera's UUID, or SELECTED (or nothing) for the one selected in the dock */
	device?: string;
	/** The preset's id; on the selected camera, its position in the dock's list, from 1 */
	preset?: string;
};

const AXES = ["pan", "tilt", "zoom"] as const;
// How far an axis can be from where the camera arrived, of its range, and the
// camera still be at the preset: a camera reports a little settling after it
// arrives
const AT_PRESET_SLACK = 0.02;

type Arrival = { preset: string; position: Partial<Record<(typeof AXES)[number], number>> };

/** The UUID of the camera a key is for now, "" if it is for the selected camera and none is */
function deviceOf(settings: PresetSettings): string {
	const camera = cameraOf(settings);
	return camera === SELECTED ? ptz.selected : camera;
}

/** The preset of `presets` that a key is for, if the camera has it */
function presetOf(settings: PresetSettings, presets: Preset[]): Preset | undefined {
	if (cameraOf(settings) === SELECTED) {
		return presets[Number(settings.preset) - 1];
	}
	return presets.find((p) => p.id === settings.preset);
}

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
		try {
			// The selected camera's list says which camera it is of, and the
			// recall goes to that one, whatever is selected by then
			const list = await ptz.presets(cameraOf(ev.payload.settings));
			const preset = presetOf(ev.payload.settings, list.presets);
			if (!preset) {
				throw new Error("the camera has no such preset");
			}
			await ptz.call("ptz_preset_recall", { device: list.device, id: preset.id });
		} catch (e) {
			streamDeck.logger.warn(String(e));
			await ev.action.showAlert();
		}
	}

	// The camera or the preset was changed in the property inspector
	override async onDidReceiveSettings(ev: DidReceiveSettingsEvent<PresetSettings>): Promise<void> {
		await sendPresets(cameraOf(ev.payload.settings));
		if (ev.action.isKey()) {
			await this.#refresh(ev.action, ev.payload.settings);
		}
	}

	override async onSendToPlugin(ev: SendToPluginEvent<InspectorRequest, PresetSettings>): Promise<void> {
		const event = ev.payload.event;
		if (event === "getCameras") {
			await sendCameras();
		} else if (event === "getPresets") {
			await sendPresets(cameraOf(await ev.action.getSettings()));
		}
	}

	/** Called when the connection to OBS is made, and when it is lost */
	async connectionChanged(connected: boolean): Promise<void> {
		if (!connected) {
			this.#at.clear();
		}
		await this.#refreshAll();
	}

	/** Called when another camera is selected in the dock: the keys on the selected camera follow it */
	async selectionChanged(): Promise<void> {
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
			// A preset's name, the camera's presets, or their order, which is
			// what a key on the selected camera goes by, changed: the keys follow
			case "ptz_preset_changed":
			case "ptz_preset_added":
			case "ptz_preset_removed":
			case "ptz_preset_order_changed":
			case "ptz_preset_list_reset":
				await this.#refreshAll(device);
				break;
			case "ptz_device_removed":
				this.#at.delete(device);
				await this.#refreshAll(device);
				break;
		}
	}

	/** Sets each key's title and state, or only those of the keys that are for `device` now */
	async #refreshAll(device?: string): Promise<void> {
		// Each camera's list is asked for once, however many keys it has
		const lists = new Map<string, Promise<PresetList>>();
		for (const key of this.actions) {
			if (!key.isKey()) {
				continue;
			}
			const settings = await key.getSettings();
			if (device === undefined || deviceOf(settings) === device) {
				await this.#refresh(key, settings, lists);
			}
		}
	}

	async #refresh(
		key: KeyAction<PresetSettings>,
		settings: PresetSettings,
		lists = new Map<string, Promise<PresetList>>(),
	): Promise<void> {
		const device = deviceOf(settings);
		let title = "";
		let at = false;
		if (device && settings.preset && ptz.connected) {
			try {
				if (!lists.has(device)) {
					lists.set(device, ptz.presets(device));
				}
				const preset = presetOf(settings, (await lists.get(device)!).presets);
				// A position on the selected camera that it has no preset at is blank
				title = preset ? preset.name || preset.id : cameraOf(settings) === SELECTED ? "" : "(gone)";
				at = preset !== undefined && this.#at.get(device)?.preset === preset.id;
			} catch {
				title = "(no camera)";
			}
		}
		await key.setTitle(title);
		await key.setState(at ? 1 : 0);
	}
}
