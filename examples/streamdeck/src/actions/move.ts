/*
 * A key that moves a camera for as long as it is held: ptz_move at a speed on
 * the key going down, and the same axes back to 0 on its coming up.
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

import streamDeck, {
	action,
	KeyDownEvent,
	KeyUpEvent,
	SendToPluginEvent,
	SingletonAction,
	WillDisappearEvent,
} from "@elgato/streamdeck";

import { ptz } from "../obs-ptz";
import { InspectorRequest, sendCameras } from "./inspector";

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
	| "zoom-out";

type MoveSettings = {
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

/** The speeds a direction moves at, or the axes it moves set to 0 to stop them */
function speeds(direction: Direction, speed: number): Record<string, number> {
	const pantilt = PANTILT[direction];
	if (pantilt) {
		return { pan: pantilt[0] * speed, tilt: pantilt[1] * speed };
	}
	return { zoom: (ZOOM[direction] ?? 0) * speed };
}

@action({ UUID: "ca.secretlab.obs-ptz.move" })
export class MoveAction extends SingletonAction<MoveSettings> {
	// The keys being held, by the key's id, with what to send to stop the move
	// each started: not ptz_stop, which would stop a zoom that another key held
	// at the same time had started, but its own axes, set to 0
	#held = new Map<string, Record<string, unknown>>();

	override async onKeyDown(ev: KeyDownEvent<MoveSettings>): Promise<void> {
		const { device, direction = "left", speed = 50 } = ev.payload.settings;
		if (!device) {
			await ev.action.showAlert();
			return;
		}
		const stop = { device, ...speeds(direction, 0) };
		this.#held.set(ev.action.id, stop);
		try {
			await ptz.call("ptz_move", { device, ...speeds(direction, speed / 100) });
		} catch (e) {
			this.#held.delete(ev.action.id);
			streamDeck.logger.warn(String(e));
			await ev.action.showAlert();
		}
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
		const stop = this.#held.get(id);
		if (!stop) {
			return;
		}
		this.#held.delete(id);
		try {
			await ptz.call("ptz_move", stop);
		} catch (e) {
			streamDeck.logger.warn(String(e));
		}
	}
}
