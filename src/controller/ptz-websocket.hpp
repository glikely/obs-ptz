/* Pan Tilt Zoom Controls - the PTZ API over obs-websocket
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Gives a client outside OBS the PTZ API of every device, as requests and
 * events of the "obs-ptz" obs-websocket vendor: see "WebSocket binding" in
 * docs/ptz-device-api.md. ptz_websocket_load() and ptz_websocket_unload(),
 * which start and stop it, are in ptz.h.
 */
#pragma once

#include "obs-websocket-api.h"

/* How much a client is let do: "off", nothing at all; "read-only", the procs
 * that only read a device; "control", all of them. Anything else is taken as
 * "control". Applies at once. Main thread. */
void ptz_websocket_set_access(const char *access);
