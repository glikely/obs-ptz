/* PTZUsbWorker: that the camera is only ever touched on the worker's own
 * thread, without making the caller wait for it, and the things the worker
 * does on top of that (continuous moves, tracking whether the camera is there).
 *
 * SPDX-License-Identifier: GPLv2
 */
#include <catch_amalgamated.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QObject>
#include <QThread>

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ptz-usb-worker.hpp"

namespace {

using std::chrono::milliseconds;

/* The state of the pretend camera. Its backend is used on the worker's thread
 * and inspected on the test's, hence the mutex. */
struct Camera {
	using Call = std::pair<std::string, long>;

	std::mutex m;
	std::vector<Call> calls;
	std::set<QThread *> threads; /* every thread that created, used or destroyed a backend */
	int alive = 0;
	std::atomic<bool> present{true};   /* false while the camera is unplugged */
	std::atomic<int> probes{0};        /* how often the backend was asked if it's still there */
	std::atomic<int> delay_ms{0};      /* how long each request takes */
	std::atomic<bool> no_focus{false}; /* like a fixed-focus camera: no focus range */

	size_t count()
	{
		std::lock_guard<std::mutex> lock(m);
		return calls.size();
	}
	std::vector<Call> snapshot()
	{
		std::lock_guard<std::mutex> lock(m);
		return calls;
	}
	std::vector<std::string> names()
	{
		std::vector<std::string> out;
		for (auto &c : snapshot())
			out.push_back(c.first);
		return out;
	}
	long last(const std::string &name)
	{
		auto all = snapshot();
		for (auto it = all.rbegin(); it != all.rend(); ++it)
			if (it->first == name)
				return it->second;
		return -1000;
	}
	int aliveNow()
	{
		std::lock_guard<std::mutex> lock(m);
		return alive;
	}
	size_t threadCount()
	{
		std::lock_guard<std::mutex> lock(m);
		return threads.size();
	}
	bool ranOn(QThread *thread)
	{
		std::lock_guard<std::mutex> lock(m);
		return threads.count(thread) != 0;
	}
};

class FakeBackend : public PTZUsbBackend {
public:
	FakeBackend(Camera &camera, const std::string &id) : camera_(camera), alive_(camera.present)
	{
		device_path = id;
		min = {-100, -100, 0, 0};
		max = {100, 100, 100, camera.no_focus ? 0 : 100};
		step = {1, 1, 1, 1};
		std::lock_guard<std::mutex> lock(camera_.m);
		camera_.alive++;
		camera_.threads.insert(QThread::currentThread());
	}
	~FakeBackend() override
	{
		std::lock_guard<std::mutex> lock(camera_.m);
		camera_.alive--;
		camera_.threads.insert(QThread::currentThread());
	}

	bool internal_pan(long v) override { return record("pan", v); }
	bool internal_tilt(long v) override { return record("tilt", v); }
	bool internal_zoom(long v) override { return record("zoom", v); }
	bool internal_focus(bool auto_focus, long v) override { return record(auto_focus ? "autofocus" : "focus", v); }
	bool isValid() const override { return alive_; }
	bool checkAlive() override
	{
		camera_.probes++;
		if (!camera_.present)
			alive_ = false;
		return alive_;
	}

private:
	bool record(const char *what, long value)
	{
		if (int ms = camera_.delay_ms)
			QThread::msleep(ms);
		if (!camera_.present) {
			alive_ = false; /* the request failed, which is how a camera's absence shows */
			return false;
		}
		std::lock_guard<std::mutex> lock(camera_.m);
		camera_.calls.emplace_back(what, value);
		camera_.threads.insert(QThread::currentThread());
		return true;
	}

	Camera &camera_;
	bool alive_;
};

PTZUsbBackendSlot::Factory factoryFor(Camera &camera)
{
	return [&camera](const std::string &id, bool) -> std::unique_ptr<PTZUsbBackend> {
		return std::make_unique<FakeBackend>(camera, id);
	};
}

/* Keep the test thread's event loop going, for the signals coming back from the
 * worker, until pred() is true. */
template<typename Pred> bool waitFor(Pred pred, int timeout_ms = 5000)
{
	QElapsedTimer timer;
	timer.start();
	while (!pred()) {
		if (timer.elapsed() > timeout_ms)
			return false;
		QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
		QThread::msleep(5);
	}
	return true;
}

void settle(int ms)
{
	waitFor([] { return false; }, ms);
}

} // namespace

TEST_CASE("the camera is only touched on the worker's thread", "[usb-backend][worker]")
{
	Camera camera;
	auto worker = std::make_unique<PTZUsbWorker>(factoryFor(camera));

	worker->setDeviceId("cam");
	worker->pantiltAbs(0.5, -0.5);
	REQUIRE(waitFor([&] { return camera.count() >= 2; }));

	CHECK(camera.last("pan") == 50);
	CHECK(camera.last("tilt") == -50);
	CHECK(camera.threadCount() == 1);
	CHECK_FALSE(camera.ranOn(QThread::currentThread()));

	/* the camera is closed on that thread as well */
	worker.reset();
	CHECK(camera.aliveNow() == 0);
	CHECK(camera.threadCount() == 1);
}

TEST_CASE("a slow camera does not hold up whoever asked", "[usb-backend][worker]")
{
	Camera camera;
	camera.delay_ms = 200;
	PTZUsbWorker worker(factoryFor(camera));
	worker.setDeviceId("cam");

	QElapsedTimer timer;
	timer.start();
	for (int i = 0; i < 5; i++)
		worker.zoomAbs(0.1 * (i + 1));
	CHECK(timer.elapsed() < 100); /* they take 200ms each */

	REQUIRE(waitFor([&] { return camera.count() >= 5; }, 10000));
}

TEST_CASE("commands are carried out in the order they were given", "[usb-backend][worker]")
{
	Camera camera;
	PTZUsbWorker worker(factoryFor(camera));

	worker.setDeviceId("cam");
	for (double z : {0.2, 0.4, 0.6, 1.0})
		worker.zoomAbs(z);
	REQUIRE(waitFor([&] { return camera.count() >= 4; }));

	auto calls = camera.snapshot();
	CHECK(calls == std::vector<Camera::Call>{{"zoom", 20}, {"zoom", 40}, {"zoom", 60}, {"zoom", 100}});
}

TEST_CASE("nothing happens without a camera", "[usb-backend][worker]")
{
	Camera camera;
	PTZUsbWorker worker(factoryFor(camera));

	worker.pantiltAbs(0.5, 0.5);
	worker.setSpeeds(1, 0, 0, 0);
	worker.setDeviceId("");
	settle(150);

	CHECK(camera.count() == 0);
	CHECK(camera.aliveNow() == 0);
}

TEST_CASE("the worker says when the camera comes and goes", "[usb-backend][worker]")
{
	Camera camera;
	PTZUsbWorker worker(factoryFor(camera), milliseconds(50));

	QObject context;
	std::vector<bool> states;
	QObject::connect(&worker, &PTZUsbWorker::connectedChanged, &context, [&](bool c) { states.push_back(c); });

	worker.setDeviceId("cam");
	REQUIRE(waitFor([&] { return states.size() == 1; }));
	CHECK(states.back());

	camera.present = false; /* unplugged */
	REQUIRE(waitFor([&] { return states.size() == 2; }));
	CHECK_FALSE(states.back());

	camera.present = true;
	REQUIRE(waitFor([&] { return states.size() == 3; }));
	CHECK(states.back());
}

TEST_CASE("an idle camera that is unplugged is noticed", "[usb-backend][worker]")
{
	Camera camera;
	PTZUsbWorker worker(factoryFor(camera), milliseconds(50));

	QObject context;
	std::vector<bool> states;
	QObject::connect(&worker, &PTZUsbWorker::connectedChanged, &context, [&](bool c) { states.push_back(c); });

	worker.setDeviceId("cam");
	REQUIRE(waitFor([&] { return states.size() == 1; }));

	camera.present = false;
	REQUIRE(waitFor([&] { return states.size() == 2; }));
	CHECK_FALSE(states.back());
	/* nothing was sent to it: it was noticed by looking */
	CHECK(camera.count() == 0);
	CHECK(camera.probes > 0);
}

TEST_CASE("a camera being driven is not probed as well", "[usb-backend][worker]")
{
	Camera camera;
	PTZUsbWorker worker(factoryFor(camera));
	worker.setDeviceId("cam");
	settle(300); /* a probe or two while idle */

	worker.setSpeeds(1.0, 0, 0, 0);
	REQUIRE(waitFor([&] { return camera.count() >= 3; }));
	int probes = camera.probes;
	settle(300);
	CHECK(camera.probes == probes);
	worker.setSpeeds(0, 0, 0, 0);
}

TEST_CASE("a continuous move covers ground with time, and stops when told to", "[usb-backend][worker]")
{
	Camera camera;
	PTZUsbWorker worker(factoryFor(camera));
	worker.setDeviceId("cam");

	worker.setSpeeds(1.0, 0, 0, 0); /* the whole range of pan in a second */
	REQUIRE(waitFor([&] { return camera.last("pan") >= 40; }));
	CHECK(camera.last("pan") < 100); /* not there instantly */

	/* it went there in steps. (A step counts for at most a quarter of a
	 * second, however late it runs, so a machine that is very busy still
	 * takes more than one.) */
	size_t pans = 0;
	for (auto &n : camera.names())
		pans += n == "pan";
	CHECK(pans >= 2);

	worker.setSpeeds(0, 0, 0, 0);
	settle(150); /* a step in progress */
	size_t stopped_at = camera.count();
	settle(300);
	CHECK(camera.count() == stopped_at);
}

TEST_CASE("zoom can be driven continuously too", "[usb-backend][worker]")
{
	Camera camera;
	PTZUsbWorker worker(factoryFor(camera));
	worker.setDeviceId("cam");

	worker.setSpeeds(0, 0, 1.0, 0);
	REQUIRE(waitFor([&] { return camera.last("zoom") >= 20; }));
	worker.setSpeeds(0, 0, 0, 0);
}

TEST_CASE("the worker reports where the camera is", "[usb-backend][worker]")
{
	Camera camera;
	PTZUsbWorker worker(factoryFor(camera));

	QObject context;
	struct Answer {
		int id = -1;
		double pan = 0, tilt = 0, zoom = 0;
	} answer;
	bool answered = false;
	QObject::connect(&worker, &PTZUsbWorker::positionCaptured, &context,
			 [&](int id, double pan, double tilt, double zoom, bool, double) {
				 answer = {id, pan, tilt, zoom};
				 answered = true;
			 });

	worker.setDeviceId("cam");
	worker.pantiltAbs(0.5, 0.25);
	worker.zoomAbs(0.75);
	worker.capturePosition(7);
	REQUIRE(waitFor([&] { return answered; }));

	CHECK(answer.id == 7);
	CHECK(answer.pan == 0.5);
	CHECK(answer.tilt == 0.25);
	CHECK(answer.zoom == 0.75);
}

TEST_CASE("the worker reports its state, for the axes the camera has", "[usb-backend][worker]")
{
	Camera camera;
	PTZUsbWorker worker(factoryFor(camera));

	QObject context;
	struct Answer {
		PtzUsbCamPos pos;
		bool hasPan = false, hasTilt = false, hasZoom = false, hasFocus = false;
	} answer;
	bool answered = false;
	QObject::connect(&worker, &PTZUsbWorker::stateCaptured, &context,
			 [&](PtzUsbCamPos pos, bool hasPan, bool hasTilt, bool hasZoom, bool hasFocus) {
				 answer = {pos, hasPan, hasTilt, hasZoom, hasFocus};
				 answered = true;
			 });

	worker.setDeviceId("cam");
	worker.pantiltAbs(0.5, 0.25);
	worker.zoomAbs(0.75);
	worker.focusAbs(0.4);
	worker.captureState();
	REQUIRE(waitFor([&] { return answered; }));

	CHECK(answer.hasPan);
	CHECK(answer.hasTilt);
	CHECK(answer.hasZoom);
	CHECK(answer.hasFocus);
	CHECK(answer.pos.pan == 0.5);
	CHECK(answer.pos.tilt == 0.25);
	CHECK(answer.pos.zoom == 0.75);
	CHECK(answer.pos.focus == 0.4);
}

TEST_CASE("a camera without a focus range reports no focus in its state", "[usb-backend][worker]")
{
	Camera camera;
	camera.no_focus = true;
	PTZUsbWorker worker(factoryFor(camera));

	bool hasFocus = true;
	bool answered = false;
	QObject context;
	QObject::connect(&worker, &PTZUsbWorker::stateCaptured, &context,
			 [&](PtzUsbCamPos, bool, bool, bool, bool focus) {
				 hasFocus = focus;
				 answered = true;
			 });

	worker.setDeviceId("cam");
	worker.captureState();
	REQUIRE(waitFor([&] { return answered; }));
	CHECK_FALSE(hasFocus);
}

TEST_CASE("captureState does nothing without a camera", "[usb-backend][worker]")
{
	Camera camera;
	PTZUsbWorker worker(factoryFor(camera));

	bool answered = false;
	QObject context;
	QObject::connect(&worker, &PTZUsbWorker::stateCaptured, &context,
			 [&](PtzUsbCamPos, bool, bool, bool, bool) { answered = true; });

	/* no setDeviceId(): there is no camera to ask */
	worker.captureState();
	QThread::msleep(100);
	QCoreApplication::processEvents();
	CHECK_FALSE(answered);
}

TEST_CASE("a saved position is recalled axis by axis", "[usb-backend][worker]")
{
	Camera camera;
	PTZUsbWorker worker(factoryFor(camera));
	worker.setDeviceId("cam");

	PtzUsbCamPos pos;
	pos.pan = 0.1;
	pos.tilt = -0.2;
	pos.zoom = 0.3;
	pos.focusAuto = false;
	pos.focus = 0.4;
	worker.recall(pos);
	REQUIRE(waitFor([&] { return camera.count() >= 5; }));

	CHECK(camera.names() == std::vector<std::string>{"pan", "tilt", "zoom", "focus", "focus"});
	CHECK(camera.last("pan") == 10);
	CHECK(camera.last("tilt") == -20);
	CHECK(camera.last("zoom") == 30);
	CHECK(camera.last("focus") == 40);

	pos.focusAuto = true;
	worker.recall(pos);
	REQUIRE(waitFor([&] { return camera.count() >= 9; }));
	CHECK(camera.snapshot().back().first == "autofocus");
}

TEST_CASE("destroying the worker waits for the camera and closes it", "[usb-backend][worker]")
{
	Camera camera;
	camera.delay_ms = 100;
	auto worker = std::make_unique<PTZUsbWorker>(factoryFor(camera));
	worker->setDeviceId("cam");
	worker->zoomAbs(0.5);
	REQUIRE(waitFor([&] { return camera.aliveNow() == 1; }));

	QElapsedTimer timer;
	timer.start();
	worker.reset();
	CHECK(timer.elapsed() < 2000);
	CHECK(camera.aliveNow() == 0);
}
