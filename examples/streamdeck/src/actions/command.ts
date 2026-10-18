/*
 * A key that does one thing to a camera: stop it, send it home, save its home
 * position, focus it or set its white balance once, or turn its power on and
 * off. The camera is the one selected in the PTZ dock, unless the key names one.
 *
 * Setting the home position replaces the one the camera has, so it takes the
 * key being held for a moment, as saving a preset does.
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

import { cameraStates } from "../camera-state";
import { Data, ptz, SELECTED } from "../obs-ptz";
import { GROUND, INK, keyPicture, LIT_GROUND, LIT_INK } from "../pictures";
import { cameraOf, InspectorRequest, sendCameras } from "./inspector";

type Command = "stop" | "home" | "home-save" | "focus-onetouch" | "wb-onepush" | "power";

type CommandSettings = {
	/** The camera's UUID, or SELECTED (or nothing) for the one selected in the dock */
	device?: string;
	command?: Command;
};

const HOLD_MS = 1000;

const LABELS: Record<Command, string> = {
	stop: "Stop",
	home: "Home",
	"home-save": "Hold: set home",
	"focus-onetouch": "One-push AF",
	"wb-onepush": "One-push WB",
	power: "Power",
};

const GLYPHS: Record<Command, string> = {
	// a stop sign's octagon with a bar
	stop: '<path d="M52 20h40l32 32v40l-32 32H52L20 92V52z"/><path d="M52 72h40"/>',
	// a house
	home: '<path d="M20 70L72 24l52 46M32 62v54h80V62M60 116V84h24v32"/>',
	// a house with an arrow into it
	"home-save": '<path d="M20 70L72 24l52 46M32 62v54h80V62"/><path d="M72 48v40M56 76l16 16 16-16"/>',
	// focus brackets round a point, and a 1
	"focus-onetouch": '<path d="M26 54V26h28M90 26h28v28M118 90v28H90M54 118H26V90M64 62l8-6v32"/>',
	// a sun, for the white
	"wb-onepush": '<circle cx="72" cy="72" r="20"/><path d="M72 26v14M72 104v14M26 72h14M104 72h14M40 40l10 10M94 94l10 10M104 40L94 50M50 94L40 104"/>',
	// the power symbol
	power: '<path d="M72 24v44M46 46a40 40 0 1 0 52 0"/>',
};

@action({ UUID: "ca.secretlab.obs-ptz.command" })
export class CommandAction extends SingletonAction<CommandSettings> {
	// The keys held that will do their command if they are held for long enough
	#holding = new Map<string, NodeJS.Timeout>();

	override async onWillAppear(ev: WillAppearEvent<CommandSettings>): Promise<void> {
		if (ev.action.isKey()) {
			await this.#refresh(ev.action, ev.payload.settings);
		}
	}

	override onWillDisappear(ev: WillDisappearEvent<CommandSettings>): void {
		this.#cancelHold(ev.action.id);
	}

	override async onDidReceiveSettings(ev: DidReceiveSettingsEvent<CommandSettings>): Promise<void> {
		if (ev.action.isKey()) {
			await this.#refresh(ev.action, ev.payload.settings);
		}
	}

	override async onKeyDown(ev: KeyDownEvent<CommandSettings>): Promise<void> {
		const { command = "stop" } = ev.payload.settings;
		if (command === "home-save") {
			this.#cancelHold(ev.action.id);
			this.#holding.set(
				ev.action.id,
				setTimeout(() => {
					this.#holding.delete(ev.action.id);
					this.#run(ev.action, ev.payload.settings).catch((e) => streamDeck.logger.warn(String(e)));
				}, HOLD_MS),
			);
			return;
		}
		await this.#run(ev.action, ev.payload.settings);
	}

	override onKeyUp(ev: KeyUpEvent<CommandSettings>): void {
		this.#cancelHold(ev.action.id);
	}

	override async onSendToPlugin(ev: SendToPluginEvent<InspectorRequest, CommandSettings>): Promise<void> {
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
		if (name === "ptz_state_changed" && "power_on" in (data.changed as Data)) {
			await this.#refreshAll(data.device as string);
		}
	}

	#cancelHold(id: string): void {
		clearTimeout(this.#holding.get(id));
		this.#holding.delete(id);
	}

	#deviceOf(settings: CommandSettings): string {
		const camera = cameraOf(settings);
		return camera === SELECTED ? ptz.selected : camera;
	}

	async #run(key: KeyAction<CommandSettings>, settings: CommandSettings): Promise<void> {
		const { command = "stop" } = settings;
		const device = cameraOf(settings);
		try {
			switch (command) {
				case "stop":
					await ptz.call("ptz_stop", { device });
					break;
				case "home":
					await ptz.call("ptz_home_recall", { device });
					break;
				case "home-save":
					await ptz.call("ptz_home_save", { device });
					await key.showOk();
					break;
				case "focus-onetouch":
					await ptz.call("ptz_trigger", { device, name: "focus_onetouch" });
					break;
				case "wb-onepush":
					await ptz.call("ptz_trigger", { device, name: "wb_onepush" });
					break;
				case "power": {
					const uuid = this.#deviceOf(settings);
					if (!uuid) {
						throw new Error("no camera is selected");
					}
					const state = await cameraStates.get(uuid);
					await ptz.call("ptz_request_state", { device: uuid, state: { power_on: state.power_on !== true } });
					break;
				}
			}
		} catch (e) {
			streamDeck.logger.warn(String(e));
			await key.showAlert();
		}
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

	async #refresh(key: KeyAction<CommandSettings>, settings: CommandSettings): Promise<void> {
		const { command = "stop" } = settings;
		let lit = false;
		if (command === "power") {
			const device = ptz.connected ? this.#deviceOf(settings) : "";
			if (device) {
				try {
					lit = (await cameraStates.get(device)).power_on === true;
				} catch (e) {
					streamDeck.logger.warn(String(e));
				}
			}
		}
		await key.setImage(keyPicture(GLYPHS[command], lit ? LIT_GROUND : GROUND, lit ? LIT_INK : INK));
		await key.setTitle(LABELS[command]);
	}
}

