/* Pan Tilt Zoom Controls - the declaration of a proc or signal, read
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ptz-ws-decl.hpp"

#include <cctype>

namespace ptzws {

namespace {

/* Walks a declaration a word or a mark at a time, over any space between */
struct Reader {
	const std::string &text;
	size_t at = 0;

	void skipSpace()
	{
		while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])))
			at++;
	}

	/* The next name (letters, digits and underscores), or "" if a mark is next */
	std::string word()
	{
		skipSpace();
		size_t start = at;
		while (at < text.size() && (std::isalnum(static_cast<unsigned char>(text[at])) || text[at] == '_'))
			at++;
		return text.substr(start, at - start);
	}

	bool mark(char c)
	{
		skipSpace();
		if (at >= text.size() || text[at] != c)
			return false;
		at++;
		return true;
	}

	bool done()
	{
		skipSpace();
		return at == text.size();
	}
};

std::optional<Type> typeNamed(const std::string &name)
{
	if (name == "void")
		return Type::Void;
	if (name == "string")
		return Type::String;
	if (name == "int")
		return Type::Int;
	if (name == "bool")
		return Type::Bool;
	if (name == "float")
		return Type::Float;
	if (name == "ptr")
		return Type::Ptr;
	return std::nullopt;
}

} // namespace

std::optional<Decl> parse_decl(const std::string &text)
{
	Reader reader{text};
	Decl decl;

	auto ret = typeNamed(reader.word());
	if (!ret)
		return std::nullopt;
	decl.ret = *ret;
	decl.retName = decl.ret == Type::Void ? "" : "return";

	decl.name = reader.word();
	if (decl.name.empty() || !reader.mark('('))
		return std::nullopt;

	if (!reader.mark(')')) {
		do {
			Param param;
			std::string word = reader.word();
			param.out = word == "out";
			if (word == "in" || word == "out")
				word = reader.word();
			auto type = typeNamed(word);
			if (!type || *type == Type::Void)
				return std::nullopt;
			param.type = *type;
			param.name = reader.word();
			if (param.name.empty())
				return std::nullopt;
			decl.params.push_back(param);
		} while (reader.mark(','));
		if (!reader.mark(')'))
			return std::nullopt;
	}
	if (!reader.done())
		return std::nullopt;
	return decl;
}

} // namespace ptzws
