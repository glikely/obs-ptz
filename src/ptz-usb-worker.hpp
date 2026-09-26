/* The thread that talks to one USB camera
 *
 * Talking to the camera means blocking calls into the OS (an ioctl, a COM call,
 * an IOKit transfer) that can take a while, or stall, if the camera does. They
 * must not run on OBS's graphics thread, which is where a tick callback runs,
 * nor on the UI thread the device's commands arrive on.
 *
 * So each camera gets a thread of its own, owned by this class, and the backend
 * is only ever created, used and destroyed there. That also means no locking
 * around the backend. Everything public here can be called from any thread: it
 * returns at once and the work happens on the worker's thread, in the order it
 * was asked for. Results come back as (queued) signals.
 *
 * The worker also moves the camera continuously while speeds are set, and keeps
 * track of whether the camera is there.
 *
 * Self-contained (no OBS) so it can be unit tested with a fake backend.
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QThread>
#include <QTimer>
#include <chrono>
#include <string>
#include <utility>

#include "ptz-usb-backend-slot.hpp"

class PTZUsbWorker : public QObject {
	Q_OBJECT

public:
	using Clock = PTZUsbBackendSlot::Clock;

	explicit PTZUsbWorker(PTZUsbBackendSlot::Factory factory,
			      Clock::duration retry_interval = std::chrono::seconds(1));
	/* Waits for the request in progress, if any, and closes the camera */
	~PTZUsbWorker() override;

	/* Which camera to talk to (see PTZUsbBackendSlot). Empty for none. */
	void setDeviceId(const std::string &device_id);

	/* Speeds of a continuous move, in the range of PTZDevice's pan_speed etc.
	 * and per second of the axis' range. All zero stops. */
	void setSpeeds(double pan, double tilt, double zoom, double focus);

	void pantiltAbs(double pan, double tilt);
	void pantiltRel(double pan, double tilt);
	void zoomAbs(double pos);
	void focusAbs(double pos);
	void setAutoFocus(bool enabled);
	/* Move everything to a saved position */
	void recall(const PtzUsbCamPos &pos);
	/* Ask where the camera is; answered by positionCaptured() with the same id */
	void capturePosition(int id);

signals:
	void connectedChanged(bool connected);
	void positionCaptured(int id, double pan, double tilt, double zoom, bool focusAuto, double focus);

private:
	/* Runs fn on the worker's thread, after everything posted before it */
	template<typename Fn> void post(Fn &&fn)
	{
		QMetaObject::invokeMethod(this, std::forward<Fn>(fn), Qt::QueuedConnection);
	}

	/* The rest only runs on the worker's thread */
	PTZUsbBackend *backend();
	void tick();
	void refreshConnection();
	void updateTimer();
	bool moving() const { return pan_speed_ != 0 || tilt_speed_ != 0 || zoom_speed_ != 0 || focus_speed_ != 0; }

	QThread thread_;
	QTimer *timer_ = nullptr;
	QElapsedTimer since_tick_;
	PTZUsbBackendSlot slot_;
	std::string device_id_;
	bool connected_ = false;
	double pan_speed_ = 0, tilt_speed_ = 0, zoom_speed_ = 0, focus_speed_ = 0;
};
