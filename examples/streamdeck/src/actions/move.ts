/*
 * A key that moves a camera for as long as it is held: ptz_move at a speed on
 * the key going down, and the same axes back to 0 on its coming up. The camera
 * is the one selected in the PTZ dock, unless the key names one.
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

import streamDeck, {
	action,
	DidReceiveSettingsEvent,
	KeyDownEvent,
	KeyUpEvent,
	SendToPluginEvent,
	SingletonAction,
	WillAppearEvent,
	WillDisappearEvent,
} from "@elgato/streamdeck";

import { Data, ptz } from "../obs-ptz";
import { cameraOf, InspectorRequest, sendCameras } from "./inspector";

type Direction =
	| "left"
	| "right"
	| "up"
	| "down"
	| "up-left"
	| "up-right"
	| "down-left"
	| "down-right"
	| "zoom-in"
	| "zoom-out"
	| "focus-near"
	| "focus-far";

type MoveSettings = {
	/** The camera's UUID, or SELECTED (or nothing) for the one selected in the dock */
	device?: string;
	direction?: Direction;
	/** Percent of the camera's top speed */
	speed?: number;
};

// [pan, tilt] for each direction, or [zoom] for a zoom: a negative pan is
// left, tilt down, and zoom out, as ptz_move has it
const PANTILT: Partial<Record<Direction, [number, number]>> = {
	left: [-1, 0],
	right: [1, 0],
	up: [0, 1],
	down: [0, -1],
	"up-left": [-1, 1],
	"up-right": [1, 1],
	"down-left": [-1, -1],
	"down-right": [1, -1],
};
const ZOOM: Partial<Record<Direction, number>> = { "zoom-in": 1, "zoom-out": -1 };
// Near is positive and far negative, as the PTZ dock has them
const FOCUS: Partial<Record<Direction, number>> = { "focus-near": 1, "focus-far": -1 };

/** The speeds a direction moves at, or the axes it moves set to 0 to stop them */
function speeds(direction: Direction, speed: number): Record<string, number> {
	const pantilt = PANTILT[direction];
	if (pantilt) {
		return { pan: pantilt[0] * speed, tilt: pantilt[1] * speed };
	}
	if (direction in FOCUS) {
		return { focus: (FOCUS[direction] ?? 0) * speed };
	}
	return { zoom: (ZOOM[direction] ?? 0) * speed };
}

// How far an arrow is turned from pointing up, in degrees clockwise
const ANGLE: Partial<Record<Direction, number>> = {
	up: 0,
	"up-right": 45,
	right: 90,
	"down-right": 135,
	down: 180,
	"down-left": 225,
	left: 270,
	"up-left": 315,
};

/** The key's picture for a direction: an arrow pointing that way, or a magnifier with a + or - for a zoom */
function keyImage(direction: Direction): string {
	const stroke = 'fill="none" stroke="#e8eaf0" stroke-width="7" stroke-linecap="round" stroke-linejoin="round"';
	let glyph: string;
	if (direction in ZOOM) {
		const sign = direction === "zoom-in" ? "M48 64h32M64 48v32" : "M48 64h32";
		glyph = `<g ${stroke}><circle cx="64" cy="64" r="34"/><path d="M88 88l28 28"/><path d="${sign}"/></g>`;
	} else if (direction in FOCUS) {
		// Focus brackets round a point that is big for near and small for far
		const radius = direction === "focus-near" ? 22 : 9;
		glyph =
			`<g ${stroke}><path d="M26 54V26h28M90 26h28v28M118 90v28H90M54 118H26V90"/>` +
			`<circle cx="72" cy="72" r="${radius}"/></g>`;
	} else {
		glyph = `<g ${stroke} transform="rotate(${ANGLE[direction] ?? 0} 72 72)"><path d="M72 112V32M44 60l28-28 28 28"/></g>`;
	}
	const svg =
		`<svg xmlns="http://www.w3.org/2000/svg" width="144" height="144" viewBox="0 0 144 144">` +
		`<rect width="144" height="144" fill="#1f2430"/>${glyph}</svg>`;
	return `data:image/svg+xml;charset=utf8,${encodeURIComponent(svg)}`;
}

@action({ UUID: "ca.secretlab.obs-ptz.move" })
export class MoveAction extends SingletonAction<MoveSettings> {
	// The keys being held, by the key's id, with what to send to stop the move
	// each started: not ptz_stop, which would stop a zoom that another key held
	// at the same time had started, but its own axes, set to 0. It is known
	// once the move is answered, which a quick press can come up before.
	#held = new Map<string, Promise<Data | undefined>>();

	override async onWillAppear(ev: WillAppearEvent<MoveSettings>): Promise<void> {
		if (ev.action.isKey()) {
			await ev.action.setImage(keyImage(ev.payload.settings.direction ?? "left"));
		}
	}

	// The direction was changed in the property inspector
	override async onDidReceiveSettings(ev: DidReceiveSettingsEvent<MoveSettings>): Promise<void> {
		if (ev.action.isKey()) {
			await ev.action.setImage(keyImage(ev.payload.settings.direction ?? "left"));
		}
	}

	override async onKeyDown(ev: KeyDownEvent<MoveSettings>): Promise<void> {
		const { direction = "left", speed = 50 } = ev.payload.settings;
		const device = cameraOf(ev.payload.settings);
		const moving = ptz.call("ptz_move", { device, ...speeds(direction, speed / 100) }).then(
			// The camera that was selected is the one to stop, whatever is
			// selected by the time the key comes up: the answer says which
			(answer) => ({ device: answer.device ?? device, ...speeds(direction, 0) }),
			async (e) => {
				streamDeck.logger.warn(String(e));
				await ev.action.showAlert();
				return undefined;
			},
		);
		this.#held.set(ev.action.id, moving);
		await moving;
	}

	override async onKeyUp(ev: KeyUpEvent<MoveSettings>): Promise<void> {
		await this.#stop(ev.action.id);
	}

	// A key let go of on another page, or in a profile that has been switched
	// away from, never comes up: stop its move as it goes
	override async onWillDisappear(ev: WillDisappearEvent<MoveSettings>): Promise<void> {
		await this.#stop(ev.action.id);
	}

	override async onSendToPlugin(ev: SendToPluginEvent<InspectorRequest, MoveSettings>): Promise<void> {
		if (ev.payload.event === "getCameras") {
			await sendCameras();
		}
	}

	async #stop(id: string): Promise<void> {
		const moving = this.#held.get(id);
		this.#held.delete(id);
		const stop = await moving;
		if (!stop) {
			return;
		}
		try {
			await ptz.call("ptz_move", stop);
		} catch (e) {
			streamDeck.logger.warn(String(e));
		}
	}
}
