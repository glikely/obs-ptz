/* Owns the backend of one USB camera device
 *
 * PTZUSBCam finds out which camera it is bound to by asking the OBS source it
 * belongs to, so the answer can change (or the camera can go away) at any
 * time. The slot keeps the backend that matches the current answer, and
 * replaces it when that changes or the camera is lost.
 *
 * It is asked for the backend far more often (every tick) than it is worth
 * looking for a camera that isn't there: opening one means enumerating USB
 * devices. So a camera that can't be opened is only tried again once per
 * retry interval, though picking a different camera is always acted on at once.
 *
 * Self-contained (no OBS or Qt) so it can be unit tested with a fake factory.
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "ptz-usb-backend.hpp"

class PTZUsbBackendSlot {
public:
	using Clock = std::chrono::steady_clock;

	/* report says whether the factory should log why it failed. It is only
	 * true for a newly selected camera, not for the retries that follow. */
	using Factory = std::function<std::unique_ptr<PTZUsbBackend>(const std::string &device_id, bool report)>;

	explicit PTZUsbBackendSlot(Factory factory, Clock::duration retry_interval = std::chrono::seconds(1))
		: factory_(std::move(factory)),
		  retry_interval_(retry_interval)
	{
	}

	/* The camera the backend, or the last attempt to open one, was for */
	const std::string &deviceId() const { return device_id_; }

	/* The backend for device_id, or nullptr if there is none: no camera is
	 * selected, or it could not be opened. */
	PTZUsbBackend *get(const std::string &device_id, Clock::time_point now = Clock::now())
	{
		bool switched = device_id != device_id_;
		if (!switched) {
			if (backend_ && backend_->isValid())
				return backend_.get();
			if (device_id.empty() || now < next_attempt_)
				return nullptr;
		} else {
			next_attempt_ = Clock::time_point();
		}

		backend_.reset();
		device_id_ = device_id;
		if (device_id.empty())
			return nullptr;

		backend_ = factory_(device_id, switched);
		if (!backend_ || !backend_->isValid()) {
			backend_.reset();
			next_attempt_ = now + retry_interval_;
			return nullptr;
		}
		return backend_.get();
	}

private:
	Factory factory_;
	Clock::duration retry_interval_;
	Clock::time_point next_attempt_;
	std::string device_id_;
	std::unique_ptr<PTZUsbBackend> backend_;
};
