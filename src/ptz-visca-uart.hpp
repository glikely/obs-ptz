/* Pan Tilt Zoom VISCA over Serial UART implementation
 *
 * Copyright 2021 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QObject>
#include "uart-wrapper.hpp"
#include "ptz-visca.hpp"

class ViscaUART : public PTZUARTWrapper {
	Q_OBJECT

private:
	/* Global lookup table of UART instances, used to eliminate duplicates */
	static std::map<QString, ViscaUART*> interfaces;

	int camera_count;

public:
	ViscaUART(QString &port_name);
	bool open();
	void close();
	void receive_datagram(const QByteArray &packet);
	void receiveBytes(const QByteArray &packet);

	static ViscaUART *get_interface(QString port_name);
};

class PTZViscaSerial : public PTZVisca {
	Q_OBJECT

private:
	ViscaUART *iface;
	void attach_interface(ViscaUART *iface);

protected:
	void send_immediate(const QByteArray &msg);
	void reset();

public:
	PTZViscaSerial(OBSData config);
	~PTZViscaSerial();

	void set_config(OBSData ptz_data);
	OBSData get_config();
	obs_properties_t *get_obs_properties();
};
