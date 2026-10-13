/* Tests for ptzws::parse_decl (src/controller/ptz-ws-decl.hpp): reading the
 * declaration a proc or signal of the PTZ API is registered with.
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <catch_amalgamated.hpp>
#include "ptz-ws-decl.hpp"

using ptzws::parse_decl;
using ptzws::Type;

TEST_CASE("ws-decl: a_proc_with_nothing", "[ws-decl]")
{
	auto decl = parse_decl("void ptz_stop()");
	REQUIRE(decl);
	REQUIRE(decl->name == "ptz_stop");
	REQUIRE(decl->ret == Type::Void);
	REQUIRE(decl->params.empty());
}

TEST_CASE("ws-decl: a_ptr_in_and_a_ptr_returned", "[ws-decl]")
{
	auto decl = parse_decl("ptr ptz_get_state(ptr state)");
	REQUIRE(decl);
	REQUIRE(decl->name == "ptz_get_state");
	REQUIRE(decl->ret == Type::Ptr);
	REQUIRE(decl->params.size() == 1);
	REQUIRE(decl->params[0].name == "state");
	REQUIRE(decl->params[0].type == Type::Ptr);
	REQUIRE_FALSE(decl->params[0].out);
}

TEST_CASE("ws-decl: out_params", "[ws-decl]")
{
	auto decl = parse_decl("void ptz_get_api_version(out int major, out int minor)");
	REQUIRE(decl);
	REQUIRE(decl->params.size() == 2);
	REQUIRE(decl->params[0].name == "major");
	REQUIRE(decl->params[0].type == Type::Int);
	REQUIRE(decl->params[0].out);
	REQUIRE(decl->params[1].name == "minor");
	REQUIRE(decl->params[1].out);
}

TEST_CASE("ws-decl: an_in_param_says_so_or_not", "[ws-decl]")
{
	auto decl = parse_decl("void f(in string a, string b)");
	REQUIRE(decl);
	REQUIRE(decl->params.size() == 2);
	REQUIRE_FALSE(decl->params[0].out);
	REQUIRE(decl->params[0].name == "a");
	REQUIRE_FALSE(decl->params[1].out);
}

TEST_CASE("ws-decl: every_type", "[ws-decl]")
{
	auto decl = parse_decl("string ptz_preset_create(string name, string store)");
	REQUIRE(decl);
	REQUIRE(decl->ret == Type::String);
	REQUIRE(decl->params[1].type == Type::String);

	decl = parse_decl("void ptz_preset_capture_thumbnail(string id, bool if_arrived)");
	REQUIRE(decl);
	REQUIRE(decl->params[1].type == Type::Bool);
	REQUIRE(decl->params[1].name == "if_arrived");

	decl = parse_decl("bool f(float speed, int index)");
	REQUIRE(decl);
	REQUIRE(decl->ret == Type::Bool);
	REQUIRE(decl->params[0].type == Type::Float);
	REQUIRE(decl->params[1].type == Type::Int);
}

TEST_CASE("ws-decl: a_signal_with_many", "[ws-decl]")
{
	auto decl = parse_decl("void ptz_device_found(ptr source, string kind, string id, string name, "
			       "string address, ptr settings)");
	REQUIRE(decl);
	REQUIRE(decl->name == "ptz_device_found");
	REQUIRE(decl->params.size() == 6);
	REQUIRE(decl->params[0].name == "source");
	REQUIRE(decl->params[5].name == "settings");
	REQUIRE(decl->params[5].type == Type::Ptr);
}

TEST_CASE("ws-decl: spaces_are_not_counted", "[ws-decl]")
{
	auto decl = parse_decl("  void   ptz_move ( float pan ,float tilt )  ");
	REQUIRE(decl);
	REQUIRE(decl->name == "ptz_move");
	REQUIRE(decl->params.size() == 2);
	REQUIRE(decl->params[1].name == "tilt");
}

TEST_CASE("ws-decl: what_is_not_a_declaration", "[ws-decl]")
{
	REQUIRE_FALSE(parse_decl(""));
	REQUIRE_FALSE(parse_decl("oops"));
	REQUIRE_FALSE(parse_decl("void ptz_x(int"));
	REQUIRE_FALSE(parse_decl("void ptz_x(int)"));
	REQUIRE_FALSE(parse_decl("void ptz_x(int a,)"));
	REQUIRE_FALSE(parse_decl("void f(vec3 v)"));
	REQUIRE_FALSE(parse_decl("vec3 f()"));
	REQUIRE_FALSE(parse_decl("void (int a)"));
	REQUIRE_FALSE(parse_decl("void f(int a) trailing"));
	REQUIRE_FALSE(parse_decl("void f(out void a)"));
}
