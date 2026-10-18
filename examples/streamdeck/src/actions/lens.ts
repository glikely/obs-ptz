/*
 * A Stream Deck + dial for one of a camera's axes: pan, tilt, zoom or focus,
 * which are the ones the PTZ API has a position for. Turning it moves the
 * axis, and its LCD shows where the axis is as a bar, from the position the
 * camera reports in ptz_state_changed. Pressing a focus dial turns the camera's
 * autofocus on or off.
 *
 * A camera that can go to a position (the pantilt_abs, zoom_abs or focus_abs
 * feature) is sent one, a step for each tick from where the dial last put it;
 * one that can only move at a speed is moved a little for each tick instead. The camera is the one
 * selected in the PTZ dock, unless the dial names one.
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

import streamDeck, {
	action,
	DialAction,
	DialDownEvent,
	DialRotateEvent,
	DidReceiveSettingsEvent,
	SendToPluginEvent,
	SingletonAction,
	WillAppearEvent,
	WillDisappearEvent,
} from "@elgato/streamdeck";

import { Data, ptz, SELECTED } from "../obs-ptz";
import { cameraOf, InspectorRequest, sendCameras } from "./inspector";

type Axis = "pan" | "tilt" | "zoom" | "focus";

const AXES: Record<Axis, { name: string; min: number; feature: string; absFeature: string }> = {
	pan: { name: "Pan", min: -1, feature: "pantilt", absFeature: "pantilt_abs" },
	tilt: { name: "Tilt", min: -1, feature: "pantilt", absFeature: "pantilt_abs" },
	zoom: { name: "Zoom", min: 0, feature: "zoom", absFeature: "zoom_abs" },
	focus: { name: "Focus", min: 0, feature: "focus", absFeature: "focus_abs" },
};

type LensSettings = {
	/** The camera's UUID, or SELECTED (or nothing) for the one selected in the dock */
	device?: string;
	axis?: Axis;
	/** Percent of the axis's whole range that a tick of the dial moves it */
	step?: number;
};

/** What is known of a camera's lens */
type Lens = {
	pan?: number;
	tilt?: number;
	zoom?: number;
	focus?: number;
	autofocus?: boolean;
	features: Data;
};

// Sent at most this often while the dial is turned, so that a fast turn is
// not a request for each tick
const SEND_MS = 60;
// The dial goes by the camera's own position again after it has been left
// alone this long, as the camera may not have gone where it was told
const SETTLE_MS = 1500;
// A camera that can only move at a speed is moved at this one for each tick,
// and stopped when the dial has been left alone this long
const SPEED = 0.4;
const STOP_MS = 200;

type Turn = {
	device: string;
	/** Where the dial has asked the camera to be, in the axis's own range */
	value: number;
	send?: NodeJS.Timeout;
	settle?: NodeJS.Timeout;
	stop?: NodeJS.Timeout;
};

// What each axis looks like on the dial's LCD, drawn in a 20 by 20 box
const PICTOGRAPHS: Record<Axis, string> = {
	// a horizontal double arrow
	pan: '<path d="M2 10h16M5 6l-4 4 4 4M15 6l4 4-4 4"/>',
	// a vertical one
	tilt: '<path d="M10 2v16M6 5l4-4 4 4M6 15l4 4 4-4"/>',
	// a magnifier with a plus
	zoom: '<circle cx="8.5" cy="8.5" r="6"/><path d="M13 13l6 6M5.5 8.5h6M8.5 5.5v6"/>',
	// a lens: focus brackets round a point
	focus: '<path d="M2 6V2h4M14 2h4v4M18 14v4h-4M6 18H2v-4"/><circle cx="10" cy="10" r="3"/>',
};
// With autofocus on, the dock's own AF icon: "AF" in the corners of a frame, in a 16 by 16 box
const AUTOFOCUS =
	'<path d="M 7.5608822,3.8106751 8.8160945,12.242993 H 6.5712957 L 6.4619204,10.727364 H 5.6754595 L 5.5452508,12.242993 H 3.2744103 L 4.3889971,3.8106751 Z M 6.3994202,9.2325672 Q 6.232753,7.800271 6.0660858,5.6960977 5.7327514,8.112772 5.6494178,9.2325672 Z"/>' +
	'<path d="m 9.1910955,3.8106751 h 3.7135535 v 1.6875053 h -1.520838 v 1.5989634 h 1.354171 V 8.7013155 H 11.383811 V 12.242993 H 9.1910955 Z"/>' +
	'<path d="M 0,3 V 0 H 3 V 1 H 1 v 2 z"/><path d="m 13,0 h 3 V 3 H 15 V 1 h -2 z"/>' +
	'<path d="m 16,13 v 3 h -3 v -1 h 2 v -2 z"/><path d="M 3,16 H 0 v -3 h 1 v 2 h 2 z"/>';

function pictograph(axis: Axis, autofocus: boolean): string {
	const svg = autofocus
		? `<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 16 16" fill="#fff">${AUTOFOCUS}</svg>`
		: `<svg xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 20 20" fill="none" stroke="#fff" ` +
			`stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round">${PICTOGRAPHS[axis]}</svg>`;
	return `data:image/svg+xml;charset=utf8,${encodeURIComponent(svg)}`;
}

const clamp = (axis: Axis, v: number) => Math.min(1, Math.max(AXES[axis].min, v));

@action({ UUID: "ca.secretlab.obs-ptz.lens" })
export class LensAction extends SingletonAction<LensSettings> {
	// What is known of each camera's lens, by the device's UUID
	#lenses = new Map<string, Lens>();
	// The dials being turned, by the dial's id
	#turns = new Map<string, Turn>();

	override async onWillAppear(ev: WillAppearEvent<LensSettings>): Promise<void> {
		if (ev.action.isDial()) {
			await this.#refresh(ev.action, ev.payload.settings);
		}
	}

	override onWillDisappear(ev: WillDisappearEvent<LensSettings>): void {
		this.#endTurn(ev.action.id);
	}

	override async onDidReceiveSettings(ev: DidReceiveSettingsEvent<LensSettings>): Promise<void> {
		this.#endTurn(ev.action.id);
		if (ev.action.isDial()) {
			await this.#refresh(ev.action, ev.payload.settings);
		}
	}

	override async onDialRotate(ev: DialRotateEvent<LensSettings>): Promise<void> {
		const { axis = "zoom", step = 2 } = ev.payload.settings;
		const device = this.#deviceOf(ev.payload.settings);
		try {
			if (!device) {
				throw new Error("no camera is selected");
			}
			const lens = await this.#lens(device);
			const turn = this.#turns.get(ev.action.id);
			const ticks = ev.payload.ticks;
			if (lens.features[AXES[axis].absFeature]) {
				const base = turn?.device === device ? turn.value : lens[axis];
				if (base === undefined) {
					throw new Error(`the camera has not reported its ${axis}`);
				}
				const span = 1 - AXES[axis].min;
				this.#turn(ev.action, device, axis, clamp(axis, base + (ticks * step * span) / 100));
			} else if (lens.features[AXES[axis].feature]) {
				await this.#nudge(ev.action.id, device, axis, ticks);
			} else {
				throw new Error(`the camera cannot ${axis}`);
			}
		} catch (e) {
			streamDeck.logger.warn(String(e));
			await ev.action.showAlert();
		}
	}

	// Focus: autofocus on or off
	override async onDialDown(ev: DialDownEvent<LensSettings>): Promise<void> {
		if (ev.payload.settings.axis !== "focus") {
			return;
		}
		const device = this.#deviceOf(ev.payload.settings);
		try {
			const lens = device ? await this.#lens(device) : undefined;
			if (!device || !lens?.features.autofocus) {
				throw new Error("the camera has no autofocus");
			}
			await ptz.call("ptz_request_state", { device, state: { focus_af_enabled: !lens.autofocus } });
		} catch (e) {
			streamDeck.logger.warn(String(e));
			await ev.action.showAlert();
		}
	}

	override async onSendToPlugin(ev: SendToPluginEvent<InspectorRequest, LensSettings>): Promise<void> {
		if (ev.payload.event === "getCameras") {
			await sendCameras();
		}
	}

	/** Called when the connection to OBS is made, and when it is lost */
	async connectionChanged(_connected: boolean): Promise<void> {
		this.#lenses.clear();
		await this.#refreshAll();
	}

	/** Called when another camera is selected in the dock: the dials on the selected camera follow it */
	async selectionChanged(): Promise<void> {
		await this.#refreshAll();
	}

	/** Called with each of obs-ptz's events */
	async event(name: string, data: Data): Promise<void> {
		const device = data.device as string;
		if (name === "ptz_device_removed") {
			this.#lenses.delete(device);
		} else if (name === "ptz_state_changed") {
			const lens = this.#lenses.get(device);
			if (!lens) {
				return; // read in full when a dial first needs it
			}
			const changed = data.changed as Data;
			for (const axis of Object.keys(AXES) as Axis[]) {
				if (typeof changed[axis] === "number") {
					lens[axis] = changed[axis] as number;
				}
			}
			if (typeof changed.focus_af_enabled === "boolean") {
				lens.autofocus = changed.focus_af_enabled;
			}
			if (changed.features) {
				lens.features = changed.features as Data;
			}
		} else {
			return;
		}
		await this.#refreshAll(device);
	}

	#deviceOf(settings: LensSettings): string {
		const camera = cameraOf(settings);
		return camera === SELECTED ? ptz.selected : camera;
	}

	async #lens(device: string): Promise<Lens> {
		let lens = this.#lenses.get(device);
		if (!lens) {
			const state = (await ptz.call("ptz_get_state", { device })).state as Data;
			lens = {
				pan: typeof state.pan === "number" ? state.pan : undefined,
				tilt: typeof state.tilt === "number" ? state.tilt : undefined,
				zoom: typeof state.zoom === "number" ? state.zoom : undefined,
				focus: typeof state.focus === "number" ? state.focus : undefined,
				autofocus: state.focus_af_enabled === true,
				features: (state.features ?? {}) as Data,
			};
			// A state_changed that came while it was being read is newer
			this.#lenses.set(device, this.#lenses.get(device) ?? lens);
			lens = this.#lenses.get(device)!;
		}
		return lens;
	}

	/** Shows `value` now, and sends it to the camera soon: the latest one, if the dial is turned on meanwhile */
	#turn(dial: DialAction<LensSettings>, device: string, axis: Axis, value: number): void {
		const turn = this.#turns.get(dial.id) ?? { device, value };
		turn.device = device;
		turn.value = value;
		this.#turns.set(dial.id, turn);
		clearTimeout(turn.settle);
		turn.settle = setTimeout(() => {
			this.#turns.delete(dial.id);
			dial.getSettings().then((s) => this.#refresh(dial, s)).catch(() => {});
		}, SETTLE_MS);
		if (!turn.send) {
			turn.send = setTimeout(() => {
				turn.send = undefined;
				// A pan or a tilt is sent with the other, which is taken as 0 if it is left out
				const position: Data = { device, [axis]: turn.value };
				const lens = this.#lenses.get(device);
				if (axis === "pan" || axis === "tilt") {
					const other = axis === "pan" ? "tilt" : "pan";
					position[other] = lens?.[other] ?? 0;
				}
				ptz.call("ptz_move_abs", position).catch((e) => streamDeck.logger.warn(String(e)));
			}, SEND_MS);
		}
		dial.getSettings().then((s) => this.#show(dial, s, value)).catch(() => {});
	}

	/** Moves a camera that can only move at a speed, in the direction of the turn, until the dial is left alone */
	async #nudge(id: string, device: string, axis: Axis, ticks: number): Promise<void> {
		const turn = this.#turns.get(id) ?? { device, value: 0 };
		this.#turns.set(id, turn);
		clearTimeout(turn.stop);
		turn.stop = setTimeout(() => {
			this.#turns.delete(id);
			ptz.call("ptz_move", { device, ...(axis === "pan" || axis === "tilt" ? { pan: 0, tilt: 0 } : { [axis]: 0 }) }).catch((e) => streamDeck.logger.warn(String(e)));
		}, STOP_MS);
		const speed = Math.sign(ticks) * SPEED;
		await ptz.call("ptz_move", { device, ...(axis === "pan" || axis === "tilt" ? { pan: 0, tilt: 0 } : {}), [axis]: speed });
	}

	#endTurn(id: string): void {
		const turn = this.#turns.get(id);
		if (turn) {
			clearTimeout(turn.send);
			clearTimeout(turn.settle);
			clearTimeout(turn.stop);
			this.#turns.delete(id);
		}
	}

	async #refreshAll(device?: string): Promise<void> {
		for (const dial of this.actions) {
			if (!dial.isDial()) {
				continue;
			}
			const settings = await dial.getSettings();
			if (device === undefined || this.#deviceOf(settings) === device) {
				// A dial being turned shows what it asked for, not what the camera
				// reported on the way there
				if (!this.#turns.has(dial.id)) {
					await this.#refresh(dial, settings);
				}
			}
		}
	}

	async #refresh(dial: DialAction<LensSettings>, settings: LensSettings): Promise<void> {
		const device = ptz.connected ? this.#deviceOf(settings) : "";
		let position: number | undefined;
		let af = false;
		if (device) {
			try {
				const lens = await this.#lens(device);
				position = lens[settings.axis ?? "zoom"];
				af = settings.axis === "focus" && lens.autofocus === true;
			} catch (e) {
				streamDeck.logger.warn(String(e));
			}
		}
		await this.#show(dial, settings, position, ptz.connected ? undefined : "No OBS", af);
	}

	async #show(
		dial: DialAction<LensSettings>,
		settings: LensSettings,
		position: number | undefined,
		note?: string,
		af = false,
	): Promise<void> {
		const axis = settings.axis ?? "zoom";
		const { name, min } = AXES[axis];
		const fraction = position === undefined ? 0 : (position - min) / (1 - min);
		// A pan or a tilt is shown from the middle of its range: +25% is a quarter of the way to the end
		const shown = position === undefined ? "-" : `${min < 0 ? Math.round(position * 100) : Math.round(fraction * 100)}%`;
		await dial.setFeedback({
			title: af ? `${name} (AF)` : name,
			icon: pictograph(axis, af),
			value: note ?? shown,
			indicator: { value: Math.round(fraction * 100) },
		});
	}
}
