/* Edge cases for ptz_preset_io::importPresets() (src/ptz-preset-io.cpp),
 * plus one test pinning down a pre-existing protocol-helpers.cpp gap that
 * matters for preset export fidelity. See test_preset_io.cpp for the
 * round-trip/ordering tests.
 *
 * SPDX-License-Identifier: GPLv2
 */
#include "protocol-helpers.hpp"
#include "ptz-preset-io.hpp"

#include <catch_amalgamated.hpp>

TEST_CASE("importPresets keeps only the first occurrence of a duplicate id", "[preset-io][import]")
{
	/* Not something exportPresets() itself can produce - it stamps each
	 * array entry from a QMap key, which can't repeat - but a
	 * hand-edited or corrupted file could still have two entries with
	 * the same "id". */
	OBSDataAutoRelease config = obs_data_create();
	OBSDataArrayAutoRelease array = obs_data_array_create();
	{
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_int(item, "id", 3);
		obs_data_set_string(item, "name", "First");
		obs_data_array_push_back(array, item);
	}
	{
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_int(item, "id", 3);
		obs_data_set_string(item, "name", "Second");
		obs_data_array_push_back(array, item);
	}
	obs_data_set_array(config, "presets", array);
	obs_data_set_int(config, "preset_max", 10);

	QMap<size_t, QVariantMap> presets;
	QList<size_t> displayOrder;
	ptz_preset_io::importPresets(config, 16, presets, displayOrder);

	REQUIRE(displayOrder == QList<size_t>{3});
	CHECK(presets[3]["name"].toString() == "First");
}

TEST_CASE("importPresets clamps preset_max to [1, 128]", "[preset-io][import]")
{
	auto importedMax = [](long long presetMax) {
		OBSDataAutoRelease config = obs_data_create();
		obs_data_set_int(config, "preset_max", presetMax);
		QMap<size_t, QVariantMap> presets;
		QList<size_t> displayOrder;
		return ptz_preset_io::importPresets(config, 16, presets, displayOrder);
	};

	CHECK(importedMax(0) == 1);
	CHECK(importedMax(42) == 42);
	CHECK(importedMax(128) == 128);
	CHECK(importedMax(99999) == 128);
	/* -5 does NOT clamp to the lower bound: std::clamp<size_t>(...) forces
	 * its (long long) argument to size_t first, so a negative value
	 * wraps around to a huge unsigned one and clamps to the *upper*
	 * bound instead. Pre-existing behavior (unchanged by this refactor)
	 * - pinned down here so it isn't "fixed" by accident under the
	 * mistaken assumption that any out-of-range value clamps low. */
	CHECK(importedMax(-5) == 128);
}

TEST_CASE("importPresets keeps the caller's current max when preset_max is absent", "[preset-io][import]")
{
	/* A hand-edited or older export file might not carry "preset_max" at
	 * all - importPresets() must not silently reset the device to some
	 * arbitrary default in that case (see PTZDevice::getDefaults(),
	 * which is a separate, deliberate default of 16 for brand-new
	 * devices - not this function's job to duplicate). */
	OBSDataAutoRelease config = obs_data_create();
	OBSDataArrayAutoRelease array = obs_data_array_create();
	obs_data_set_array(config, "presets", array);

	QMap<size_t, QVariantMap> presets;
	QList<size_t> displayOrder;
	size_t maxPresets = ptz_preset_io::importPresets(config, 7, presets, displayOrder);

	CHECK(maxPresets == 7);
}

TEST_CASE("variantMapToOBSData only preserves int, float, and string preset fields", "[preset-io][limitation]")
{
	/* Pre-existing protocol-helpers.cpp behavior (unchanged by the
	 * preset export/import feature), not a regression: QMetaType::Bool
	 * and QMetaType::Double have no case in variantMapToOBSData()'s
	 * switch, so a bool or a plain C++ double (as opposed to an
	 * explicit float) silently vanishes. Real presets never store either
	 * today - only strings ("name", the ONVIF "token") - but this is
	 * worth pinning down so a future preset field of one of those types
	 * doesn't silently fail to export. */
	QVariantMap map;
	map["name"] = QString("Wide");
	map["flag"] = true;
	map["position"] = 3.14; // QMetaType::Double, not ::Float

	OBSDataAutoRelease data = variantMapToOBSData(map);

	CHECK(obs_data_has_user_value(data, "name"));
	CHECK_FALSE(obs_data_has_user_value(data, "flag"));
	CHECK_FALSE(obs_data_has_user_value(data, "position"));
}
