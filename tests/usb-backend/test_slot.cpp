/* PTZUsbBackendSlot: which backend is kept, and when it is replaced or freed.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <catch_amalgamated.hpp>

#include <vector>

#include "ptz-usb-backend-slot.hpp"

namespace {

struct Cameras {
	int created = 0;
	int alive = 0;
	bool present = true;        /* whether a camera can be opened at all */
	std::vector<bool> reported; /* the report argument of each attempt */
};

using Clock = PTZUsbBackendSlot::Clock;
using std::chrono::milliseconds;
using std::chrono::seconds;

class SlotFakeBackend : public PTZUsbBackend {
public:
	SlotFakeBackend(Cameras &cameras, const std::string &id, bool valid) : cameras_(cameras), valid_(valid)
	{
		device_path = id;
		cameras_.created++;
		cameras_.alive++;
	}
	~SlotFakeBackend() override { cameras_.alive--; }

	bool internal_pan(long) override { return true; }
	bool internal_tilt(long) override { return true; }
	bool internal_zoom(long) override { return true; }
	bool internal_focus(bool, long) override { return true; }
	bool isValid() const override { return valid_; }

	void unplug() { valid_ = false; }

private:
	Cameras &cameras_;
	bool valid_;
};

PTZUsbBackendSlot::Factory factoryFor(Cameras &cameras)
{
	return [&cameras](const std::string &id, bool report) -> std::unique_ptr<PTZUsbBackend> {
		cameras.reported.push_back(report);
		return std::make_unique<SlotFakeBackend>(cameras, id, cameras.present);
	};
}

} // namespace

TEST_CASE("the slot keeps the backend while the camera is unchanged", "[usb-backend][slot]")
{
	Cameras cameras;
	PTZUsbBackendSlot slot(factoryFor(cameras));

	auto *first = slot.get("cam-a");
	REQUIRE(first != nullptr);
	CHECK(slot.get("cam-a") == first);
	CHECK(slot.get("cam-a") == first);
	CHECK(cameras.created == 1);
	CHECK(slot.deviceId() == "cam-a");
}

TEST_CASE("no camera selected means no backend", "[usb-backend][slot]")
{
	Cameras cameras;
	PTZUsbBackendSlot slot(factoryFor(cameras));

	CHECK(slot.get("") == nullptr);
	CHECK(cameras.created == 0);
}

TEST_CASE("switching cameras replaces the backend and frees the old one", "[usb-backend][slot]")
{
	Cameras cameras;
	PTZUsbBackendSlot slot(factoryFor(cameras));

	REQUIRE(slot.get("cam-a") != nullptr);
	auto *b = slot.get("cam-b");
	REQUIRE(b != nullptr);
	CHECK(b->getDevicePath() == "cam-b");
	CHECK(cameras.alive == 1);

	CHECK(slot.get("") == nullptr);
	CHECK(cameras.alive == 0);
}

TEST_CASE("a camera that can't be opened gives no backend", "[usb-backend][slot]")
{
	Cameras cameras;
	cameras.present = false;
	PTZUsbBackendSlot slot(factoryFor(cameras));

	CHECK(slot.get("cam-a", Clock::now()) == nullptr);
	CHECK(cameras.alive == 0); /* the failed backend isn't kept */
}

TEST_CASE("a lost camera is reopened", "[usb-backend][slot]")
{
	Cameras cameras;
	PTZUsbBackendSlot slot(factoryFor(cameras));
	auto t = Clock::now();

	auto *cam = static_cast<SlotFakeBackend *>(slot.get("cam-a", t));
	REQUIRE(cam != nullptr);
	cam->unplug();

	cameras.present = false;
	CHECK(slot.get("cam-a", t) == nullptr);
	CHECK(cameras.alive == 0);

	cameras.present = true;
	CHECK(slot.get("cam-a", t + seconds(1)) != nullptr);
	CHECK(cameras.alive == 1);
}

TEST_CASE("destroying the slot frees the backend", "[usb-backend][slot]")
{
	Cameras cameras;
	{
		PTZUsbBackendSlot slot(factoryFor(cameras));
		REQUIRE(slot.get("cam-a") != nullptr);
		REQUIRE(cameras.alive == 1);
	}
	CHECK(cameras.alive == 0);
}

TEST_CASE("a missing camera is not looked for on every call", "[usb-backend][slot]")
{
	Cameras cameras;
	cameras.present = false;
	PTZUsbBackendSlot slot(factoryFor(cameras));
	auto t = Clock::now();

	CHECK(slot.get("cam-a", t) == nullptr);
	CHECK(cameras.created == 1);

	/* a tick is ~30ms; nothing is retried until the interval is up */
	for (int ms = 30; ms < 1000; ms += 30)
		CHECK(slot.get("cam-a", t + milliseconds(ms)) == nullptr);
	CHECK(cameras.created == 1);

	CHECK(slot.get("cam-a", t + milliseconds(1000)) == nullptr);
	CHECK(cameras.created == 2);
	CHECK(slot.get("cam-a", t + milliseconds(1500)) == nullptr);
	CHECK(cameras.created == 2);
	CHECK(slot.get("cam-a", t + milliseconds(2000)) == nullptr);
	CHECK(cameras.created == 3);
}

TEST_CASE("a camera that shows up is picked up on the next retry", "[usb-backend][slot]")
{
	Cameras cameras;
	cameras.present = false;
	PTZUsbBackendSlot slot(factoryFor(cameras));
	auto t = Clock::now();

	CHECK(slot.get("cam-a", t) == nullptr);

	cameras.present = true;
	CHECK(slot.get("cam-a", t + milliseconds(500)) == nullptr); /* still backing off */
	auto *cam = slot.get("cam-a", t + seconds(1));
	REQUIRE(cam != nullptr);

	/* and once it's there, it is simply kept */
	CHECK(slot.get("cam-a", t + seconds(60)) == cam);
	CHECK(cameras.created == 2);
}

TEST_CASE("choosing another camera is never delayed", "[usb-backend][slot]")
{
	Cameras cameras;
	cameras.present = false;
	PTZUsbBackendSlot slot(factoryFor(cameras));
	auto t = Clock::now();

	CHECK(slot.get("cam-a", t) == nullptr); /* now backing off */

	cameras.present = true;
	CHECK(slot.get("cam-b", t + milliseconds(10)) != nullptr);
}

TEST_CASE("a lost camera is retried at once, then throttled", "[usb-backend][slot]")
{
	Cameras cameras;
	PTZUsbBackendSlot slot(factoryFor(cameras));
	auto t = Clock::now();

	auto *cam = static_cast<SlotFakeBackend *>(slot.get("cam-a", t));
	REQUIRE(cam != nullptr);
	cam->unplug();
	cameras.present = false;

	CHECK(slot.get("cam-a", t + milliseconds(30)) == nullptr);
	CHECK(cameras.created == 2);
	CHECK(slot.get("cam-a", t + milliseconds(60)) == nullptr);
	CHECK(cameras.created == 2);
}

TEST_CASE("failures are only reported for a newly chosen camera", "[usb-backend][slot]")
{
	Cameras cameras;
	cameras.present = false;
	PTZUsbBackendSlot slot(factoryFor(cameras));
	auto t = Clock::now();

	slot.get("cam-a", t);
	slot.get("cam-a", t + seconds(1));
	slot.get("cam-a", t + seconds(2));
	slot.get("cam-b", t + seconds(2));
	slot.get("cam-b", t + seconds(3));
	slot.get("cam-a", t + seconds(3));

	CHECK(cameras.reported == std::vector<bool>{true, false, false, true, false, true});
}

TEST_CASE("the retry interval can be changed", "[usb-backend][slot]")
{
	Cameras cameras;
	cameras.present = false;
	PTZUsbBackendSlot slot(factoryFor(cameras), seconds(10));
	auto t = Clock::now();

	slot.get("cam-a", t);
	slot.get("cam-a", t + seconds(9));
	CHECK(cameras.created == 1);
	slot.get("cam-a", t + seconds(10));
	CHECK(cameras.created == 2);
}
