/* PTZUsbBackendSlot: which backend is kept, and when it is replaced or freed.
 *
 * SPDX-License-Identifier: GPLv2
 */
#include <catch_amalgamated.hpp>

#include "ptz-usb-backend-slot.hpp"

namespace {

struct Cameras {
	int created = 0;
	int alive = 0;
	bool present = true; /* whether a camera can be opened at all */
};

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
	return [&cameras](const std::string &id) -> std::unique_ptr<PTZUsbBackend> {
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

	CHECK(slot.get("cam-a") == nullptr);
	CHECK(cameras.alive == 0); /* the failed backend isn't kept */
}

TEST_CASE("a lost camera is reopened", "[usb-backend][slot]")
{
	Cameras cameras;
	PTZUsbBackendSlot slot(factoryFor(cameras));

	auto *cam = static_cast<SlotFakeBackend *>(slot.get("cam-a"));
	REQUIRE(cam != nullptr);
	cam->unplug();

	cameras.present = false;
	CHECK(slot.get("cam-a") == nullptr);
	CHECK(cameras.alive == 0);

	cameras.present = true;
	CHECK(slot.get("cam-a") != nullptr);
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
