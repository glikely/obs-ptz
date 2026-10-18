/*
 * What is known of each camera's state, kept up to date from ptz_state_changed,
 * for the keys that show it or act on it: read in full with ptz_get_state the
 * first time a camera is asked about, and then changed as its changes come.
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

import { Data, ptz } from "./obs-ptz";

class CameraStates {
	#states = new Map<string, Data>();

	/** The camera's state, as far as it is known: read from OBS if it isn't yet */
	async get(device: string): Promise<Data> {
		let state = this.#states.get(device);
		if (!state) {
			const read = (await ptz.call("ptz_get_state", { device })).state as Data;
			// A change that came while it was being read is newer
			state = this.#states.get(device) ?? read;
			this.#states.set(device, state);
		}
		return state;
	}

	/** Applies the `changed` of a ptz_state_changed, to a camera that is known */
	changed(device: string, changed: Data): void {
		const state = this.#states.get(device);
		if (state) {
			Object.assign(state, changed);
		}
	}

	delete(device: string): void {
		this.#states.delete(device);
	}

	clear(): void {
		this.#states.clear();
	}
}

export const cameraStates = new CameraStates();
