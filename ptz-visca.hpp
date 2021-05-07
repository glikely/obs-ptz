/* Pan Tilt Zoom camera instance
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QObject>
#include "ptz-device.hpp"

class ViscaInterface;
class PTZVisca : public PTZDevice {
	Q_OBJECT

private:
	ViscaInterface *iface;
	unsigned int address;

	void init();
	void send(QByteArray &msg);

public:
	PTZVisca(const char *uart_name, int address);
	PTZVisca(obs_data_t *config);
	~PTZVisca();

	void set_config(obs_data_t *ptz_data);
	obs_data_t * get_config();

	void cmd_clear();
	void cmd_get_camera_info();
	void pantilt(double pan, double tilt);
	void pantilt_stop();
	void pantilt_home();
	void zoom_stop();
	void zoom_tele();
	void zoom_wide();
};
