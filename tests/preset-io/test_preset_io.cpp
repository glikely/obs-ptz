/* Round-trip fidelity tests for ptz_preset_io::exportPresets()/
 * importPresets() (src/ptz-preset-io.cpp) - the logic behind the preset
 * export/import feature (issue #78). Real presets only ever carry string
 * fields ("name", the ONVIF "token"), so that's what's exercised here;
 * see the "known limitation" test in test_preset_io_edge_cases.cpp for
 * what happens with other field types.
 *
 * SPDX-License-Identifier: GPLv2
 */
#include "ptz-preset-io.hpp"

#include <catch_amalgamated.hpp>

TEST_CASE("exportPresets writes presets in display order, not map key order", "[preset-io][export]")
{
	/* QMap<size_t, ...> iterates in ascending key order (2, 5, 9); the
	 * display order the UI actually shows is independent of that and is
	 * exactly what a re-imported file must reproduce. */
	QMap<size_t, QVariantMap> presets;
	presets[9]["name"] = QString("Wide");
	presets[2]["name"] = QString("Tight");
	presets[5]["token"] = QString("abc123");
	QList<size_t> displayOrder{9, 2, 5};

	OBSDataAutoRelease config = obs_data_create();
	ptz_preset_io::exportPresets(config, 24, presets, displayOrder);

	REQUIRE(obs_data_get_int(config, "preset_max") == 24);

	OBSDataArrayAutoRelease array = obs_data_get_array(config, "presets");
	REQUIRE(obs_data_array_count(array) == 3);

	for (size_t i = 0; i < 3; i++) {
		OBSDataAutoRelease item = obs_data_array_item(array, i);
		REQUIRE((size_t)obs_data_get_int(item, "id") == displayOrder[(int)i]);
	}

	OBSDataAutoRelease first = obs_data_array_item(array, 0);
	OBSDataAutoRelease second = obs_data_array_item(array, 1);
	OBSDataAutoRelease third = obs_data_array_item(array, 2);
	CHECK(QString(obs_data_get_string(first, "name")) == "Wide");
	CHECK(QString(obs_data_get_string(second, "name")) == "Tight");
	CHECK(QString(obs_data_get_string(third, "token")) == "abc123");
}

TEST_CASE("exportPresets with no presets still writes a valid empty array", "[preset-io][export]")
{
	QMap<size_t, QVariantMap> presets;
	QList<size_t> displayOrder;

	OBSDataAutoRelease config = obs_data_create();
	ptz_preset_io::exportPresets(config, 16, presets, displayOrder);

	REQUIRE(obs_data_get_int(config, "preset_max") == 16);
	OBSDataArrayAutoRelease array = obs_data_get_array(config, "presets");
	REQUIRE(obs_data_array_count(array) == 0);
}

TEST_CASE("importPresets reads back what exportPresets wrote", "[preset-io][import]")
{
	QMap<size_t, QVariantMap> original;
	original[9]["name"] = QString("Wide");
	original[2]["name"] = QString("Tight");
	original[5]["token"] = QString("abc123");
	QList<size_t> originalOrder{9, 2, 5};

	OBSDataAutoRelease config = obs_data_create();
	ptz_preset_io::exportPresets(config, 24, original, originalOrder);

	/* currentMax deliberately differs from the file's preset_max (24), to
	 * make sure the file's own value wins when present. */
	QMap<size_t, QVariantMap> presets;
	QList<size_t> displayOrder;
	size_t maxPresets = ptz_preset_io::importPresets(config, 99, presets, displayOrder);

	REQUIRE(maxPresets == 24);
	REQUIRE(displayOrder == QList<size_t>{9, 2, 5});
	CHECK(presets[9]["name"].toString() == "Wide");
	CHECK(presets[2]["name"].toString() == "Tight");
	CHECK(presets[5]["token"].toString() == "abc123");

	/* exportPresets() stamps each array entry with its own "id" field
	 * (that's how importPresets() recovers the QMap key from a flat
	 * array); OBSDataToVariantMap() has no way to tell that field apart
	 * from a real preset property, so it comes back as part of the
	 * QVariantMap too. Not a bug to "fix" here - PTZDevice never reads a
	 * preset's own "id" property - but worth pinning down so nobody is
	 * surprised that a round-tripped preset has one extra key. */
	CHECK(presets[9]["id"].toULongLong() == 9);
}

TEST_CASE("importPresets clears any presets already present before loading", "[preset-io][import]")
{
	QMap<size_t, QVariantMap> presets;
	presets[1]["name"] = QString("Stale");
	QList<size_t> displayOrder{1};

	OBSDataAutoRelease config = obs_data_create();
	OBSDataArrayAutoRelease emptyArray = obs_data_array_create();
	obs_data_set_array(config, "presets", emptyArray);
	obs_data_set_int(config, "preset_max", 5);

	size_t maxPresets = ptz_preset_io::importPresets(config, 16, presets, displayOrder);

	CHECK(maxPresets == 5);
	CHECK(presets.isEmpty());
	CHECK(displayOrder.isEmpty());
}
