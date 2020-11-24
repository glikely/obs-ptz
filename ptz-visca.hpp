/* Pan Tilt Zoom camera instance
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#if LIBVISCA_FOUND

#include <QObject>
#include <visca/libvisca.h>
#include "ptz-device.hpp"

class ViscaInterface;
class PTZVisca : public PTZDevice {
	Q_OBJECT

private:
	ViscaInterface *interface;
	VISCACamera_t camera;

	void init();

public:
	PTZVisca(const char *uart_name, int address);
	PTZVisca(obs_data_t *config);
	~PTZVisca();

	void set_config(obs_data_t *ptz_data);
	void get_config(obs_data_t *ptz_data);

	void pantilt(double pan, double tilt);
	void pantilt_stop();
	void pantilt_up(uint32_t pan_speed, uint32_t tilt_speed);
	void pantilt_upleft(uint32_t pan_speed, uint32_t tilt_speed);
	void pantilt_upright(uint32_t pan_speed, uint32_t tilt_speed);
	void pantilt_left(uint32_t pan_speed, uint32_t tilt_speed);
	void pantilt_right(uint32_t pan_speed, uint32_t tilt_speed);
	void pantilt_down(uint32_t pan_speed, uint32_t tilt_speed);
	void pantilt_downleft(uint32_t pan_speed, uint32_t tilt_speed);
	void pantilt_downright(uint32_t pan_speed, uint32_t tilt_speed);
	void pantilt_home();
	void zoom_stop();
	void zoom_tele();
	void zoom_wide();
};
#endif
