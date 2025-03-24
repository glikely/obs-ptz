/* Pan Tilt Zoom VISCA over UVC USB
	*
	* Copyright 2025 Fabio Ferrari <fabioferrari@gmail.com>
	*
	* SPDX-License-Identifier: GPLv2
	*/
#pragma once

#include <QObject>
#include <QTcpSocket>
#include "ptz-device.hpp"

struct PtzUsbCamLimits {
	long pan = 0;
	long tilt = 0;
	long zoom = 0;
	long focus = 0;
	// long temperature = 0;
};

struct PtzUsbCamPos {
	double pan = 0;
	double tilt = 0;
	double zoom = 0;
	bool focusAuto = true;
	double focus = 0;
	// bool whitebalAuto = true;
	// double temperature = 0;
};

class PTZControl {
protected:
	std::string device_path;
	PtzUsbCamLimits min, max;
	PtzUsbCamPos now_pos;

public:
	virtual ~PTZControl() {}
	virtual bool internal_pan(long value) = 0;
	bool pan(double value)
	{
		now_pos.pan = std::clamp(value, -1.0, 1.0);
		long pan = std::clamp(static_cast<long>(now_pos.pan * max.pan),
				      min.pan, max.pan);
		return internal_pan(pan);
	}
	double getPan() const { return now_pos.pan; }
	virtual bool internal_tilt(long value) = 0;
	bool tilt(double value)
	{
		now_pos.tilt = std::clamp(value, -1.0, 1.0);
		long tilt =
			std::clamp(static_cast<long>(now_pos.tilt * max.tilt),
				   min.tilt, max.tilt);
		return internal_tilt(tilt);
	}
	double getTilt() const { return now_pos.tilt; }
	virtual bool internal_zoom(long value) = 0;
	bool zoom(double value)
	{
		now_pos.zoom = std::clamp(value, 0.0, 1.0);
		long zoom =
			std::clamp(static_cast<long>(now_pos.zoom * max.zoom),
				   min.zoom, max.zoom);
		return internal_zoom(zoom);
	}
	double getZoom() const { return now_pos.zoom; }
	virtual bool internal_focus(bool auto_focus, long value) = 0;
	bool focus(double value)
	{
		now_pos.focus = std::clamp(value, 0.0, 1.0);
		long focus =
			std::clamp(static_cast<long>(now_pos.focus * max.focus),
				   min.focus, max.focus);
		return internal_focus(false, focus);
	}
	double getFocus() const { return now_pos.focus; }
	bool setAutoFocus(bool enabled)
	{
		long focus =
			std::clamp(static_cast<long>(now_pos.focus * max.focus),
				   min.focus, max.focus);
		return internal_focus(enabled, focus);
	}
	struct PtzUsbCamPos getPosition() const { return now_pos; }
	std::string getDevicePath() { return device_path; }
	virtual bool isValid() const = 0;
};

class PTZUSBCam : public PTZDevice {
	Q_OBJECT

private:
	QString m_PTZAddress{""};
	QMap<int, PtzUsbCamPos> presets;
	double tick_elapsed = 0.0f;
	PTZControl *ptz_control_ = nullptr;
	PTZControl *get_ptz_control();

protected:
	static void ptz_tick_callback(void *param, float seconds);
	void ptz_tick(float seconds);

public:
	PTZUSBCam(OBSData config);
	~PTZUSBCam();
	void save(obs_data_t *settings) const;
	virtual QString description();

	void set_config(OBSData ptz_data);
	OBSData get_config();
	obs_properties_t *get_obs_properties();

	void do_update();
	void pantilt_rel(double pan, double tilt);
	void pantilt_abs(double pan, double tilt);
	void pantilt_home();
	void zoom_abs(double pos);
	void focus_abs(double pos);
	void set_autofocus(bool enabled);
	void memory_reset(int i);
	void memory_set(int i);
	void memory_recall(int i);
};
