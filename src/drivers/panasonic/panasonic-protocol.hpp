/* Panasonic HTTP CGI protocol helpers (OBS- and Qt-free)
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace panasonic_protocol {

inline constexpr int kZoomFocusMin = 0x555;
inline constexpr int kZoomFocusMax = 0xFFF;
inline constexpr int kZoomFocusSpan = kZoomFocusMax - kZoomFocusMin;

inline int speedToPanasonic(double val)
{
	return 50 + std::clamp((int)std::round(val * 49.0), -49, 49);
}

inline uint16_t normToPanTiltHex(double val)
{
	double clamped = std::clamp(val, -1.0, 1.0);
	int raw = 0x8000 + (int)std::round(clamped * 0x7FFF);
	return (uint16_t)std::clamp(raw, 0x0000, 0xFFFF);
}

inline double panTiltHexToNorm(int raw)
{
	return std::clamp((raw - 0x8000) / (double)0x7FFF, -1.0, 1.0);
}

inline uint16_t normToZoomFocusHex(double val)
{
	double clamped = std::clamp(val, 0.0, 1.0);
	int raw = kZoomFocusMin + (int)std::round(clamped * kZoomFocusSpan);
	return (uint16_t)std::clamp(raw, kZoomFocusMin, kZoomFocusMax);
}

inline double zoomFocusHexToNorm(int raw)
{
	return std::clamp((raw - kZoomFocusMin) / (double)kZoomFocusSpan, 0.0, 1.0);
}

inline bool iequals(std::string_view a, std::string_view b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); ++i) {
		if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
			return false;
	}
	return true;
}

inline bool istartsWith(std::string_view s, std::string_view prefix)
{
	return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

inline std::optional<int> parseHex(std::string_view s)
{
	if (s.empty())
		return std::nullopt;
	int val = 0;
	for (unsigned char c : s) {
		val <<= 4;
		if (c >= '0' && c <= '9')
			val |= (c - '0');
		else if (c >= 'a' && c <= 'f')
			val |= (c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			val |= (c - 'A' + 10);
		else
			return std::nullopt;
	}
	return val;
}

inline std::string_view trimAscii(std::string_view s)
{
	while (!s.empty() && std::isspace((unsigned char)s.front()))
		s.remove_prefix(1);
	while (!s.empty() && std::isspace((unsigned char)s.back()))
		s.remove_suffix(1);
	return s;
}

struct CamdataUpdate {
	std::optional<double> pan;
	std::optional<double> tilt;
	std::optional<double> zoom;
	std::optional<double> focus;
	std::optional<bool> focusAfEnabled;
	std::optional<bool> powerOn;
};

inline CamdataUpdate parseCamdataBody(std::string_view body)
{
	CamdataUpdate out;
	size_t pos = 0;
	while (pos < body.size()) {
		size_t end = body.find("\r\n", pos);
		std::string_view rawLine = (end == std::string_view::npos) ? body.substr(pos) : body.substr(pos, end - pos);
		pos = (end == std::string_view::npos) ? body.size() : end + 2;

		std::string_view line = trimAscii(rawLine);
		if (line.empty())
			continue;

		if (istartsWith(line, "aPC") && line.size() >= 11) {
			if (auto panRaw = parseHex(line.substr(3, 4)))
				out.pan = panTiltHexToNorm(*panRaw);
			if (auto tiltRaw = parseHex(line.substr(7, 4)))
				out.tilt = panTiltHexToNorm(*tiltRaw);
		} else if (istartsWith(line, "gz") && line.size() >= 5) {
			if (auto zoomRaw = parseHex(line.substr(2, 3)))
				out.zoom = zoomFocusHexToNorm(*zoomRaw);
		} else if (istartsWith(line, "axz") && line.size() >= 6) {
			if (auto zoomRaw = parseHex(line.substr(3, 3)))
				out.zoom = zoomFocusHexToNorm(*zoomRaw);
		} else if (istartsWith(line, "gf") && line.size() >= 5) {
			if (auto focusRaw = parseHex(line.substr(2, 3)))
				out.focus = zoomFocusHexToNorm(*focusRaw);
		} else if (istartsWith(line, "axf") && line.size() >= 6) {
			if (auto focusRaw = parseHex(line.substr(3, 3)))
				out.focus = zoomFocusHexToNorm(*focusRaw);
		} else if (iequals(line, "d10") || iequals(line, "d11")) {
			out.focusAfEnabled = (line.back() == '1');
		} else if (iequals(line, "p0") || iequals(line, "p1")) {
			out.powerOn = (line.back() == '1');
		}
	}
	return out;
}

} // namespace panasonic_protocol

