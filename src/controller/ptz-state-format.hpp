/* Pan Tilt Zoom Controls - a device's state as text to show
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <obs.h>
#include <QString>

/* An axis's position as text, in its unit where the state says what it is
 * (its "axes": the unit and points that turn a position into degrees, a zoom
 * multiplier or a focus distance), and as the position, to 3 places, where not.
 * Empty if the state has no such axis. */
QString ptzAxisText(obs_data_t *state, const char *axis);
