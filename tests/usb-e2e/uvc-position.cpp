/* Asks a USB camera where it is, through the same backend the plugin uses, in
 * the camera's own units. (What it answers is up to the camera: some report
 * where the motor really is, and some the last position they were told to go
 * to.) For tests/usb-e2e/, which can't ask the
 * plugin: it keeps the camera's USB device open, and only one process can.
 *
 * usage: usb-uvc-position <AVFoundation unique ID> [--home]
 *
 * prints
 *   RANGES pan=<min>:<max>:<step> tilt=... zoom=... focus=...
 *   POS pan=<n> tilt=<n> zoom=<n>
 * --home then sends the camera to pan, tilt and zoom 0 (its home), for
 * cleaning up after a test that stopped half way.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <cmath>
#include <cstdio>
#include <cstring>

#include "ptz-usb-backend.hpp"

int main(int argc, char *argv[])
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <AVFoundation unique ID> [--home]\n", argv[0]);
		return 2;
	}
	auto backend = ptz_usb_backend_create(argv[1], true);
	if (!backend->isValid()) {
		printf("OPENFAIL\n");
		return 1;
	}
	if (!backend->refreshPosition()) {
		printf("READFAIL\n");
		return 1;
	}

	auto min = backend->getMin(), max = backend->getMax(), step = backend->getStep();
	auto pos = backend->getPosition();
	printf("RANGES pan=%ld:%ld:%ld tilt=%ld:%ld:%ld zoom=%ld:%ld:%ld focus=%ld:%ld:%ld\n", min.pan, max.pan,
	       step.pan, min.tilt, max.tilt, step.tilt, min.zoom, max.zoom, step.zoom, min.focus, max.focus,
	       step.focus);
	printf("POS pan=%ld tilt=%ld zoom=%ld\n", std::lround(pos.pan * max.pan), std::lround(pos.tilt * max.tilt),
	       std::lround(pos.zoom * max.zoom));

	if (argc > 2 && strcmp(argv[2], "--home") == 0) {
		backend->pan(0);
		backend->tilt(0);
		backend->zoom(0);
	}
	return 0;
}
