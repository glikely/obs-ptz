/* Pan Tilt Zoom Controls - the declaration of a proc or signal, read
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A proc or signal is registered with libobs by a declaration,
 * "string ptz_preset_create(string name, string store)", which libobs reads
 * and keeps to itself. The PTZ API gives each back in ptz_get_api_info, and
 * this reads one, for the websocket binding to know the type of each field.
 * It needs neither OBS nor Qt.
 */
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace ptzws {

/* The types libobs has for a calldata field */
enum class Type { Void, String, Int, Bool, Float, Ptr };

struct Param {
	std::string name;
	Type type;
	/* Declared "out": the proc sets it, and the caller reads it after */
	bool out;
};

struct Decl {
	std::string name;
	/* What it returns, as the calldata field `retName`: "return", or "" for void */
	Type ret;
	std::string retName;
	std::vector<Param> params;
};

/* Reads "<type> <name>([in|out] <type> <name>, ...)". Nothing if it isn't one:
 * a type libobs doesn't have, a field with no name, or anything left over. */
std::optional<Decl> parse_decl(const std::string &decl);

} // namespace ptzws
