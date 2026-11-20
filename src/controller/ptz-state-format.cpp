/* Pan Tilt Zoom Controls - a device's state as text to show
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ptz-state-format.hpp"

#include <cmath>
#include <cstring>
#include <obs.hpp>

/* A value in its unit for showing: 12.5°, 4.2×, 1.5 m, 15 cm, or ∞ */
static QString formatAxisValue(const QString &unit, double value)
{
	if (unit == "deg")
		return QString("%1%2\u00b0").arg(value > 0 ? "+" : "").arg(value, 0, 'f', 1);
	if (unit == "x")
		return QString("\u00d7%1").arg(value, 0, 'f', 1);
	if (unit == "m") {
		if (!std::isfinite(value))
			return QString::fromUtf8("\u221e");
		if (value < 1.0)
			return QString("%1 cm").arg(value * 100.0, 0, 'f', value < 0.1 ? 1 : 0);
		return QString("%1 m").arg(value, 0, 'f', value < 10.0 ? 1 : 0);
	}
	return QString::asprintf("%.3f", value);
}

QString ptzAxisText(obs_data_t *state, const char *axis)
{
	if (!obs_data_has_user_value(state, axis))
		return QString();
	const double position = obs_data_get_double(state, axis);
	OBSDataAutoRelease axes = obs_data_get_obj(state, "axes");
	OBSDataAutoRelease info = axes ? obs_data_get_obj(axes, axis) : nullptr;
	OBSDataArrayAutoRelease points = info ? obs_data_get_array(info, "points") : nullptr;
	const QString unit = info ? QString::fromUtf8(obs_data_get_string(info, "unit")) : QString();
	const size_t count = points ? obs_data_array_count(points) : 0;
	/* A focus distance goes by its inverse, as a lens's focus does, so that
	 * infinity, a point with no value, is a distance that is far enough */
	const bool inverse = unit == "m";
	auto valueAt = [&](obs_data_t *point) {
		if (!obs_data_has_user_value(point, "value"))
			return inverse ? 0.0 : std::nan("");
		double value = obs_data_get_double(point, "value");
		return inverse ? 1.0 / value : value;
	};
	for (size_t i = 1; !unit.isEmpty() && i < count; i++) {
		OBSDataAutoRelease a = obs_data_array_item(points, i - 1);
		OBSDataAutoRelease b = obs_data_array_item(points, i);
		const double pa = obs_data_get_double(a, "position"), pb = obs_data_get_double(b, "position");
		if (position < pa || position > pb)
			continue;
		const double t = pb == pa ? 0.0 : (position - pa) / (pb - pa);
		const double va = valueAt(a), vb = valueAt(b);
		if (std::isnan(va) || std::isnan(vb))
			break;
		double value = va + t * (vb - va);
		if (inverse)
			value = value > 0.0 ? 1.0 / value : INFINITY;
		return formatAxisValue(unit, value);
	}
	/* Not said what it is: the position, with a sign for what has two sides */
	return QString("%1%2")
		.arg(!strcmp(axis, "pan") || !strcmp(axis, "tilt") ? (position > 0 ? "+" : "") : "")
		.arg(position, 0, 'f', 3);
}
