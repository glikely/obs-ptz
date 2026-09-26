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
#include "ptz-usb-backend.hpp"

class PTZUSBCam : public PTZDevice {
	Q_OBJECT

private:
	QString m_PTZAddress{""};
	QMap<int, PtzUsbCamPos> presets;
	double tick_elapsed = 0.0f;
	PTZUsbBackend *ptz_control_ = nullptr;
	PTZUsbBackend *getBackend();

protected:
	static void ptz_tick_callback(void *param, float seconds);
	void ptz_tick(float seconds);

public:
	PTZUSBCam(OBSData config);
	~PTZUSBCam();
	void save(obs_data_t *settings) const;
	QString description() override;

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
