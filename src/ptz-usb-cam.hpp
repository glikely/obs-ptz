/* Pan Tilt Zoom VISCA over UVC USB
	*
	* Copyright 2025 Fabio Ferrari <fabioferrari@gmail.com>
	*
	* SPDX-License-Identifier: GPLv2
	*/
#pragma once

#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <memory>
#include "ptz-device.hpp"
#include "ptz-usb-backend.hpp"
#include "ptz-usb-worker.hpp"

class PTZUSBCam : public PTZDevice {
	Q_OBJECT

private:
	QString m_PTZAddress{""};
	QMap<int, PtzUsbCamPos> presets;
	/* All the talking to the camera happens on the worker's own thread. The
	 * device only tells it what to do, and hears back through signals. */
	std::unique_ptr<PTZUsbWorker> worker_;
	/* Which camera the worker was told to use, and a timer to notice when the
	 * source is pointed at another one (the source's settings can change at
	 * any time). */
	std::string device_id_;
	QTimer device_id_timer_;
	void refreshDeviceId();

public:
	PTZUSBCam(OBSData config, obs_source_t *source = nullptr);
	~PTZUSBCam();
	void save(obs_data_t *settings) const;
	QString description() const override;

	void update(OBSData ptz_data) override;
	void save(OBSData ptz_data) const override;
	obs_properties_t *get_obs_properties() override;

	void do_update() override;
	void pantilt_rel(double pan, double tilt) override;
	void pantilt_abs(double pan, double tilt) override;
	void pantilt_home() override;
	void zoom_abs(double pos) override;
	void focus_abs(double pos) override;
	void set_autofocus(bool enabled) override;
	void memory_reset(int i) override;
	void memory_set(int i) override;
	void memory_recall(int i) override;
};

void ptz_usb_cam_register_filter();
