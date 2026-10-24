/*
 * What a camera says its axes mean, in the units a person knows, and showing a
 * position in them: the "axes" of a camera's state, which docs/ptz-device-api.md
 * has. A camera that says nothing of an axis is shown the normalized position.
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/** One axis of the state's "axes" */
export type AxisInfo = {
	step?: number;
	unit?: string;
	points?: { position: number; value?: number }[];
};

export type Axes = Partial<Record<string, AxisInfo>>;

/**
 * The value of a position in the axis's unit, from its points, or undefined when
 * the camera hasn't any or the position is outside them. A focus distance is
 * interpolated as its inverse, as a lens's focus is, so that infinity, a point
 * with no value, is the same as a distance that is far enough. Infinity is
 * Infinity.
 */
export function valueOf(info: AxisInfo | undefined, position: number): number | undefined {
	const points = info?.points;
	if (!info || !points || points.length < 2) {
		return undefined;
	}
	const inverse = info.unit === "m";
	const at = (v: number | undefined) => (inverse ? (v === undefined ? 0 : 1 / v) : (v as number));
	for (let i = 1; i < points.length; i++) {
		const a = points[i - 1];
		const b = points[i];
		if (position >= a.position && position <= b.position) {
			const t = b.position === a.position ? 0 : (position - a.position) / (b.position - a.position);
			const value = at(a.value) + t * (at(b.value) - at(a.value));
			return inverse ? (value > 0 ? 1 / value : Infinity) : value;
		}
	}
	return undefined;
}

/** A value for showing, in its unit: 12.5°, ×4.2, 1.5 m, 15 cm, or ∞ */
export function format(unit: string | undefined, value: number): string {
	switch (unit) {
		case "deg":
			return `${value > 0 ? "+" : ""}${value.toFixed(1)}°`;
		case "x":
			return `×${value.toFixed(1)}`;
		case "m":
			if (!isFinite(value)) {
				return "∞";
			}
			return value < 1 ? `${(value * 100).toFixed(value < 0.1 ? 1 : 0)} cm` : `${value.toFixed(value < 10 ? 1 : 0)} m`;
		default:
			return value.toFixed(3);
	}
}

/**
 * A position for showing: in the axis's unit where the camera says what it is,
 * and the normalized position where it doesn't (a plus sign for a pan or a tilt,
 * which are either side of the middle)
 */
export function show(info: AxisInfo | undefined, position: number, signed: boolean): string {
	const value = valueOf(info, position);
	if (value !== undefined) {
		return format(info?.unit, value);
	}
	return `${signed && position > 0 ? "+" : ""}${position.toFixed(3)}`;
}
