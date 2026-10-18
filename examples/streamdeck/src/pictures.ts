/*
 * The pictures of the keys that are drawn in code, as a Stream Deck key takes
 * them: an SVG as a data URI, 144 by 144 on a plain ground.
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

export const GROUND = "#1f2430";
export const LIT_GROUND = "#163b26";
export const INK = "#e8eaf0";
export const LIT_INK = "#3ddc84";
export const OFFLINE_GROUND = "#3a2326";

/** A key's picture: `glyph`, SVG drawn in 144 by 144 with the current color for its stroke, on `ground` */
export function keyPicture(glyph: string, ground = GROUND, ink = INK): string {
	const svg =
		`<svg xmlns="http://www.w3.org/2000/svg" width="144" height="144" viewBox="0 0 144 144">` +
		`<rect width="144" height="144" fill="${ground}"/>` +
		`<g fill="none" stroke="${ink}" stroke-width="7" stroke-linecap="round" stroke-linejoin="round">${glyph}</g></svg>`;
	return `data:image/svg+xml;charset=utf8,${encodeURIComponent(svg)}`;
}
