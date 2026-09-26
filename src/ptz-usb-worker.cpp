/* The thread that talks to one USB camera. See ptz-usb-worker.hpp.
 *
 * SPDX-License-Identifier: GPLv2
 */
#include "ptz-usb-worker.hpp"

#include <algorithm>

namespace {

/* How often to move the camera while it is being driven. */
constexpr int MOVE_INTERVAL_MS = 30;
/* How often to check the camera is still there otherwise. */
constexpr int IDLE_INTERVAL_MS = 250;
/* A move never counts for more time than this, however long the last step took */
constexpr double MAX_STEP_SECONDS = 0.25;

} // namespace

PTZUsbWorker::PTZUsbWorker(PTZUsbBackendSlot::Factory factory, Clock::duration retry_interval)
	: slot_(std::move(factory), retry_interval)
{
	thread_.setObjectName("ptz-usb");
	moveToThread(&thread_);
	thread_.start();
	/* The timer belongs to the worker's thread, so make it there */
	post([this] {
		since_tick_.start();
		timer_ = new QTimer(this);
		connect(timer_, &QTimer::timeout, this, &PTZUsbWorker::tick);
		updateTimer();
	});
}

PTZUsbWorker::~PTZUsbWorker()
{
	/* Tear down on the worker's thread: a QTimer can't be stopped from another
	 * one, and the camera is closed where it was opened. The worker never waits
	 * for the thread that is destroying it, so this can't deadlock. */
	QMetaObject::invokeMethod(
		this,
		[this] {
			delete timer_;
			timer_ = nullptr;
			slot_.get(std::string());
		},
		Qt::BlockingQueuedConnection);
	thread_.quit();
	thread_.wait();
}

void PTZUsbWorker::setDeviceId(const std::string &device_id)
{
	post([this, device_id] {
		device_id_ = device_id;
		refreshConnection();
	});
}

void PTZUsbWorker::setSpeeds(double pan, double tilt, double zoom, double focus)
{
	post([this, pan, tilt, zoom, focus] {
		bool was_moving = moving();
		pan_speed_ = pan;
		tilt_speed_ = tilt;
		zoom_speed_ = zoom;
		focus_speed_ = focus;
		if (moving() && !was_moving)
			since_tick_.restart(); /* don't count the time spent standing still */
		updateTimer();
	});
}

void PTZUsbWorker::pantiltAbs(double pan, double tilt)
{
	post([this, pan, tilt] {
		if (auto *b = backend()) {
			b->pan(pan);
			b->tilt(tilt);
		}
	});
}

void PTZUsbWorker::pantiltRel(double pan, double tilt)
{
	post([this, pan, tilt] {
		if (auto *b = backend()) {
			b->pan(b->getPan() + pan);
			b->tilt(b->getTilt() + tilt);
		}
	});
}

void PTZUsbWorker::zoomAbs(double pos)
{
	post([this, pos] {
		if (auto *b = backend())
			b->zoom(pos);
	});
}

void PTZUsbWorker::focusAbs(double pos)
{
	post([this, pos] {
		if (auto *b = backend())
			b->focus(pos);
	});
}

void PTZUsbWorker::setAutoFocus(bool enabled)
{
	post([this, enabled] {
		if (auto *b = backend())
			b->setAutoFocus(enabled);
	});
}

void PTZUsbWorker::recall(const PtzUsbCamPos &pos)
{
	post([this, pos] {
		auto *b = backend();
		if (!b)
			return;
		b->pan(pos.pan);
		b->tilt(pos.tilt);
		b->zoom(pos.zoom);
		b->setAutoFocus(pos.focusAuto);
		if (!pos.focusAuto)
			b->focus(pos.focus);
	});
}

void PTZUsbWorker::capturePosition(int id)
{
	post([this, id] {
		if (auto *b = backend()) {
			PtzUsbCamPos pos = b->getPosition();
			emit positionCaptured(id, pos.pan, pos.tilt, pos.zoom, pos.focusAuto, pos.focus);
		}
	});
}

PTZUsbBackend *PTZUsbWorker::backend()
{
	auto *b = slot_.get(device_id_);
	if ((b != nullptr) != connected_)
		refreshConnection();
	return b;
}

void PTZUsbWorker::refreshConnection()
{
	bool now_connected = slot_.get(device_id_) != nullptr;
	if (now_connected == connected_)
		return;
	connected_ = now_connected;
	emit connectedChanged(connected_);
}

void PTZUsbWorker::tick()
{
	double elapsed = since_tick_.restart() / 1000.0;
	if (!moving()) {
		/* Nothing is being sent that would show the camera gone. While it is
		 * being driven, the requests do. */
		if (auto *b = slot_.get(device_id_))
			b->checkAlive();
		refreshConnection();
		return;
	}
	refreshConnection();
	auto *b = slot_.get(device_id_);
	if (!b)
		return;

	double step = std::min(elapsed, MAX_STEP_SECONDS);
	if (pan_speed_ != 0 || tilt_speed_ != 0) {
		b->pan(b->getPan() + pan_speed_ * step);
		b->tilt(b->getTilt() + tilt_speed_ * step);
	}
	if (zoom_speed_ != 0)
		b->zoom(b->getZoom() + zoom_speed_ * step);
	if (focus_speed_ != 0)
		b->focus(b->getFocus() + focus_speed_ * step);
}

void PTZUsbWorker::updateTimer()
{
	if (!timer_)
		return;
	int interval = moving() ? MOVE_INTERVAL_MS : IDLE_INTERVAL_MS;
	if (timer_->interval() != interval || !timer_->isActive())
		timer_->start(interval);
}
