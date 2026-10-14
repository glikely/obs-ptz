/*
 * A connection to obs-websocket for the PTZ API ("WebSocket binding" in
 * docs/ptz-device-api.md), kept open for as long as the plugin runs and made
 * again whenever it drops, so that a key press is one request on a connection
 * that is already there.
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

import { createHash } from "node:crypto";

const VENDOR = "obs-ptz";

// obs-websocket's opcodes, and the bit of eventSubscriptions that asks for
// vendors' events, which a client is not sent unless it asks
const OP_HELLO = 0;
const OP_IDENTIFY = 1;
const OP_IDENTIFIED = 2;
const OP_EVENT = 5;
const OP_REQUEST = 6;
const OP_REQUEST_RESPONSE = 7;
const EVENT_VENDORS = 1 << 9;

const REQUEST_TIMEOUT_MS = 10_000;
const RECONNECT_MS = 3_000;

export type ConnectionSettings = {
	host?: string;
	port?: string | number;
	password?: string;
};

/** An entry of ptz_list_devices */
export type Camera = {
	device: string;
	name: string;
	source: string;
	api: { major: number; minor: number };
};

/** A preset, as ptz_preset_get_list lists it */
export type Preset = {
	id: string;
	store: "camera" | "local";
	name: string;
	camera_name: string;
};

export type Data = Record<string, unknown>;

export class PtzError extends Error {}

type Pending = {
	request: string;
	resolve: (answer: Data) => void;
	reject: (error: Error) => void;
	timer: NodeJS.Timeout;
};

export class ObsPtz {
	/** Called with each of obs-ptz's events: its name, and its data */
	onEvent: (name: string, data: Data) => void = () => {};
	/** Called when the connection is made, and when it is lost */
	onConnectionChanged: (connected: boolean) => void = () => {};

	#url = "";
	#password = "";
	#ws?: WebSocket;
	#connected = false;
	#retry?: NodeJS.Timeout;
	#nextId = 1;
	#pending = new Map<string, Pending>();

	get connected(): boolean {
		return this.#connected;
	}

	/** Connects to the server the settings name, if it isn't the one there is a connection to already */
	configure(settings: ConnectionSettings): void {
		const url = `ws://${settings.host || "127.0.0.1"}:${Number(settings.port) || 4455}`;
		const password = settings.password ?? "";
		if (url === this.#url && password === this.#password && this.#ws) {
			return;
		}
		this.#url = url;
		this.#password = password;
		this.#reconnect(0);
	}

	/**
	 * Calls one of the vendor's requests: a proc of a device, with `device` its
	 * UUID in the data, or one of the binding's own. Resolves with the answer,
	 * and rejects with a PtzError when it could not be done.
	 */
	call(request: string, data: Data = {}): Promise<Data> {
		if (!this.#connected || !this.#ws) {
			return Promise.reject(new PtzError(`${request}: not connected to OBS`));
		}
		const requestId = String(this.#nextId++);
		const answer = new Promise<Data>((resolve, reject) => {
			const timer = setTimeout(() => {
				this.#pending.delete(requestId);
				reject(new PtzError(`${request}: no answer from OBS`));
			}, REQUEST_TIMEOUT_MS);
			this.#pending.set(requestId, { request, resolve, reject, timer });
		});
		this.#send(OP_REQUEST, {
			requestType: "CallVendorRequest",
			requestId,
			requestData: { vendorName: VENDOR, requestType: request, requestData: data },
		});
		return answer;
	}

	async cameras(): Promise<Camera[]> {
		return (await this.call("ptz_list_devices")).devices as Camera[];
	}

	/** A camera's presets, in the order the dock shows them */
	async presets(device: string): Promise<Preset[]> {
		const list = (await this.call("ptz_preset_get_list", { device })).return as {
			presets: Record<string, Preset>;
			order: { id: string }[];
		};
		return list.order.map((p) => list.presets[p.id]);
	}

	#reconnect(delay: number): void {
		clearTimeout(this.#retry);
		const old = this.#ws;
		this.#ws = undefined;
		if (old) {
			old.onclose = null;
			old.close();
			this.#lost();
		}
		this.#retry = setTimeout(() => this.#open(), delay);
	}

	#open(): void {
		const ws = new WebSocket(this.#url);
		this.#ws = ws;
		ws.onmessage = (ev) => this.#receive(JSON.parse(String(ev.data)));
		// Whatever closed it, OBS quitting or never having been there, try again
		// until it is there: a Stream Deck usually starts before OBS does
		ws.onclose = () => {
			if (this.#ws === ws) {
				this.#lost();
				this.#reconnect(RECONNECT_MS);
			}
		};
		ws.onerror = () => {}; // a close follows, which is what is acted on
	}

	#lost(): void {
		for (const [id, pending] of this.#pending) {
			clearTimeout(pending.timer);
			pending.reject(new PtzError(`${pending.request}: the connection to OBS was lost`));
			this.#pending.delete(id);
		}
		if (this.#connected) {
			this.#connected = false;
			this.onConnectionChanged(false);
		}
	}

	#send(op: number, d: Data): void {
		this.#ws?.send(JSON.stringify({ op, d }));
	}

	#receive(msg: { op: number; d: Data }): void {
		switch (msg.op) {
			case OP_HELLO: {
				// obs-websocket says Hello; the client answers with Identify, giving
				// the password's answer to the challenge if the server asks for one
				const identify: Data = { rpcVersion: msg.d.rpcVersion, eventSubscriptions: EVENT_VENDORS };
				const auth = msg.d.authentication as { salt: string; challenge: string } | undefined;
				if (auth) {
					const sha256 = (s: string) => createHash("sha256").update(s).digest("base64");
					identify.authentication = sha256(sha256(this.#password + auth.salt) + auth.challenge);
				}
				this.#send(OP_IDENTIFY, identify);
				break;
			}
			case OP_IDENTIFIED:
				this.#connected = true;
				this.onConnectionChanged(true);
				break;
			case OP_REQUEST_RESPONSE:
				this.#answer(msg.d);
				break;
			case OP_EVENT: {
				const event = msg.d.eventData as { vendorName: string; eventType: string; eventData: Data };
				if (msg.d.eventType === "VendorEvent" && event.vendorName === VENDOR) {
					this.onEvent(event.eventType, event.eventData);
				}
				break;
			}
		}
	}

	#answer(d: Data): void {
		const pending = this.#pending.get(d.requestId as string);
		if (!pending) {
			return;
		}
		this.#pending.delete(d.requestId as string);
		clearTimeout(pending.timer);

		// obs-websocket's own failure: chiefly, no such request, which is what a
		// client is told when obs-ptz's websocket_access doesn't allow it
		const status = d.requestStatus as { result: boolean; comment?: string };
		if (!status.result) {
			pending.reject(new PtzError(`${pending.request}: ${status.comment ?? "failed"}`));
			return;
		}
		// obs-ptz's: the request was made, but could not be done
		const answer = ((d.responseData as Data | undefined)?.responseData ?? {}) as Data;
		if ("error" in answer) {
			pending.reject(new PtzError(`${pending.request}: ${answer.error}: ${answer.message ?? ""}`));
			return;
		}
		pending.resolve(answer);
	}
}

/** The one connection the plugin's actions share */
export const ptz = new ObsPtz();
