/* obs-ui-driver: what an OBS display draws
 *
 * Copyright 2026 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A screenshot of a source is a render pass of its own, and is right while
 * the window showing the source is not. So this reads the window's own pixels,
 * from a draw callback, which an OBS display runs after those there are.
 * OBSQTDisplay is OBS's class, of which this repository holds a copy.
 */

#include "ui-driver.hpp"
#include "ui-driver-objects.hpp"
#include "../../shared/qt/display/OBSQTDisplay.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace {

struct Frame {
	std::mutex mutex;
	std::condition_variable ready;
	bool done = false;
	bool littered = false;
	QJsonObject answer;
};

using ReadPixels = void (*)(int, int, int, int, unsigned int, unsigned int, void *);

/* What a render pass that doesn't clean up after itself leaves behind */
void litterState(void *)
{
	/* A task is run with no graphics context */
	obs_enter_graphics();
	gs_blend_state_push();
	gs_matrix_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	gs_matrix_scale3f(0.5f, 0.5f, 1.0f);
	obs_leave_graphics();
}

void tidyState()
{
	gs_matrix_pop();
	gs_blend_state_pop();
}

void readFrame(void *param, uint32_t cx, uint32_t cy)
{
	auto *frame = static_cast<Frame *>(param);
	std::lock_guard<std::mutex> lock(frame->mutex);
	if (frame->done)
		return;
	frame->done = true;
	frame->ready.notify_one();
	struct Tidy {
		Frame *frame;
		~Tidy()
		{
			if (frame->littered)
				tidyState();
		}
	} tidy{frame};
#ifndef _WIN32
	auto read = reinterpret_cast<ReadPixels>(dlsym(RTLD_DEFAULT, "glReadPixels"));
	if (gs_get_device_type() != GS_DEVICE_OPENGL || !read || !cx || !cy) {
		frame->answer = error("unsupported", "this graphics backend's pixels can't be read");
		return;
	}
	const unsigned int GL_RGBA_ = 0x1908, GL_UNSIGNED_BYTE_ = 0x1401;
	std::vector<uint8_t> pixels(size_t(cx) * cy * 4);
	read(0, 0, int(cx), int(cy), GL_RGBA_, GL_UNSIGNED_BYTE_, pixels.data());
	size_t white = 0, dark = 0;
	for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
		white += pixels[i] > 240 && pixels[i + 1] > 240 && pixels[i + 2] > 240;
		dark += pixels[i] < 120 && pixels[i + 1] < 120 && pixels[i + 2] < 120;
	}
	const double total = double(cx) * cy;
	frame->answer = {{"width", int(cx)}, {"height", int(cy)}, {"white", white / total}, {"dark", dark / total}};
#else
	frame->answer = error("unsupported", "this graphics backend's pixels can't be read");
#endif
}

OBSQTDisplay *displayAt(const QJsonValue &path, QJsonObject &failed)
{
	Found found = resolve(path);
	if (!found.error.isEmpty()) {
		failed = found.error;
		return nullptr;
	}
	QObject *object = found.objects.first();
	if (!object->inherits("OBSQTDisplay")) {
		failed = error("bad_request", "not an OBSQTDisplay");
		return nullptr;
	}
	return static_cast<OBSQTDisplay *>(object);
}

QJsonObject frameOf(obs_display_t *display, bool litter)
{
	Frame frame;
	frame.littered = litter;
	if (litter)
		obs_queue_task(OBS_TASK_GRAPHICS, litterState, nullptr, true);
	obs_display_add_draw_callback(display, readFrame, &frame);
	{
		std::unique_lock<std::mutex> lock(frame.mutex);
		frame.ready.wait_for(lock, std::chrono::seconds(2), [&frame]() { return frame.done; });
	}
	/* Waits for a draw in progress, so `frame` outlives the callback */
	obs_display_remove_draw_callback(display, readFrame, &frame);
	if (litter && !frame.done)
		obs_queue_task(
			OBS_TASK_GRAPHICS,
			[](void *) {
				obs_enter_graphics();
				tidyState();
				obs_leave_graphics();
			},
			nullptr, true);
	return frame.done ? frame.answer : error("no_frame", "the display drew nothing");
}

} // namespace

void addDisplayRequests()
{
	addRequest("display_frame", [](const QJsonObject &request) -> QJsonObject {
		QJsonObject failed;
		OBSQTDisplay *widget = displayAt(request["path"], failed);
		if (!widget)
			return failed;
		obs_display_t *display = widget->GetDisplay();
		if (!display)
			return error("no_frame", "it has no display");
		return frameOf(display, request["litter"].toBool());
	});
}
