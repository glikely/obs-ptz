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
	KeyUpEvent,
	SendToPluginEvent,
	SingletonAction,
	WillAppearEvent,
	WillDisappearEvent,
} from "@elgato/streamdeck";

import { Data, Preset, PresetList, ptz, SELECTED } from "../obs-ptz";
import { cameraOf, InspectorRequest, sendCameras, sendPresets } from "./inspector";

type PresetSettings = {
	/** The camera's UUID, or SELECTED (or nothing) for the one selected in the dock */
	device?: string;
	/** The preset's id; on the selected camera, its position in the dock's list, from 1 */
	preset?: string;
};

// How long a preset's key is held to save into it
const HOLD_MS = 1000;

// Thumbnails asked for, by the path the preset list gives: each image is
// written under a new name, so the same path is the same picture
const thumbnails = new Map<string, string>();

/**
 * The picture for a key of a preset that has a thumbnail: the thumbnail, a
 * little dimmed so that the title can be read over it, with a green frame while
 * the camera is at the preset. The image comes from OBS over the connection, so
 * it is there when OBS is on another machine too.
 */
async function thumbnailPicture(device: string, preset: Preset | undefined, at: boolean): Promise<string | undefined> {
	const path = preset?.thumbnail;
	if (!preset || !path) {
		return undefined;
	}
	try {
		let uri = thumbnails.get(path);
		if (!uri) {
			const image = await ptz.call("ptz_preset_get_thumbnail", { device, id: preset.id });
			if (!image.data) {
				return undefined;
			}
			uri = `data:${image.mime};base64,${image.data}`;
			thumbnails.set(path, uri);
		}
		const svg =
			`<svg xmlns="http://www.w3.org/2000/svg" width="144" height="144" viewBox="0 0 144 144">` +
			`<image href="${uri}" width="144" height="144" preserveAspectRatio="xMidYMid slice"/>` +
			`<rect width="144" height="144" fill="#000" opacity="0.3"/>` +
			(at ? `<rect x="5" y="5" width="134" height="134" rx="14" fill="none" stroke="#3ddc84" stroke-width="10"/>` : "") +
			`</svg>`;
		return `data:image/svg+xml;charset=utf8,${encodeURIComponent(svg)}`;
	} catch {
		// An OBS without the request: the key keeps its plain pictures
		return undefined;
	}
}

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
	// The keys held that will save their preset if held long enough, and those that have
	#holding = new Map<string, NodeJS.Timeout>();
	#saved = new Set<string>();

	override async onWillAppear(ev: WillAppearEvent<PresetSettings>): Promise<void> {
		if (ev.action.isKey()) {
			await this.#refresh(ev.action, ev.payload.settings);
		}
	}

	// A short press recalls the preset, on the key's coming up; one held for
	// HOLD_MS saves the camera's position into it instead, as the hold is up
	override onKeyDown(ev: KeyDownEvent<PresetSettings>): void {
		this.#cancelHold(ev.action.id);
		this.#holding.set(
			ev.action.id,
			setTimeout(() => {
				this.#holding.delete(ev.action.id);
				this.#saved.add(ev.action.id);
				this.#do("ptz_preset_save", ev.action, ev.payload.settings).catch(() => {});
			}, HOLD_MS),
		);
	}

	override async onKeyUp(ev: KeyUpEvent<PresetSettings>): Promise<void> {
		if (this.#saved.delete(ev.action.id)) {
			return;
		}
		if (this.#holding.has(ev.action.id)) {
			this.#cancelHold(ev.action.id);
			await this.#do("ptz_preset_recall", ev.action, ev.payload.settings);
		}
	}

	override onWillDisappear(ev: WillDisappearEvent<PresetSettings>): void {
		this.#cancelHold(ev.action.id);
		this.#saved.delete(ev.action.id);
	}

	#cancelHold(id: string): void {
		clearTimeout(this.#holding.get(id));
		this.#holding.delete(id);
	}

	/** Recalls or saves the key's preset */
	async #do(request: string, key: KeyAction<PresetSettings>, settings: PresetSettings): Promise<void> {
		try {
			// The selected camera's list says which camera it is of, and the
			// request goes to that one, whatever is selected by then
			const list = await ptz.presets(cameraOf(settings));
			const preset = presetOf(settings, list.presets);
			if (!preset) {
				throw new Error("the camera has no such preset");
			}
			await ptz.call(request, { device: list.device, id: preset.id });
			if (request === "ptz_preset_save") {
				await key.showOk();
			}
		} catch (e) {
			streamDeck.logger.warn(String(e));
			await key.showAlert();
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
		let thumbnail: Preset | undefined;
		if (device && settings.preset && ptz.connected) {
			try {
				if (!lists.has(device)) {
					lists.set(device, ptz.presets(device));
				}
				const preset = presetOf(settings, (await lists.get(device)!).presets);
				// A position on the selected camera that it has no preset at is blank
				title = preset ? preset.name || preset.id : cameraOf(settings) === SELECTED ? "" : "(gone)";
				at = preset !== undefined && this.#at.get(device)?.preset === preset.id;
				thumbnail = preset;
			} catch {
				title = "(no camera)";
			}
		}
		await key.setTitle(title);
		// Changing the state puts back its own picture, so the thumbnail goes on after
		await key.setState(at ? 1 : 0);
		await key.setImage(await thumbnailPicture(device, thumbnail, at));
	}
}
