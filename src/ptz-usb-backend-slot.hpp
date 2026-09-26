/* Owns the backend of one USB camera device
 *
 * PTZUSBCam finds out which camera it is bound to by asking the OBS source it
 * belongs to, so the answer can change (or the camera can go away) at any
 * time. The slot keeps the backend that matches the current answer, and
 * replaces it when that changes or the camera is lost.
 *
 * Self-contained (no OBS or Qt) so it can be unit tested with a fake factory.
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "ptz-usb-backend.hpp"

class PTZUsbBackendSlot {
public:
	using Factory = std::function<std::unique_ptr<PTZUsbBackend>(const std::string &device_id)>;

	explicit PTZUsbBackendSlot(Factory factory) : factory_(std::move(factory)) {}

	/* The camera the backend, or the last attempt to open one, was for */
	const std::string &deviceId() const { return device_id_; }

	/* The backend for device_id, or nullptr if there is none: no camera is
	 * selected, or it could not be opened. */
	PTZUsbBackend *get(const std::string &device_id)
	{
		if (backend_ && backend_->isValid() && device_id_ == device_id)
			return backend_.get();

		backend_.reset();
		device_id_ = device_id;
		if (device_id.empty())
			return nullptr;

		backend_ = factory_(device_id);
		if (!backend_ || !backend_->isValid()) {
			backend_.reset();
			return nullptr;
		}
		return backend_.get();
	}

private:
	Factory factory_;
	std::string device_id_;
	std::unique_ptr<PTZUsbBackend> backend_;
};
