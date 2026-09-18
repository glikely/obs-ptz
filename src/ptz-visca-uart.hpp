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
	static std::map<QString, ViscaUART *> interfaces;

	int camera_count;

public:
	ViscaUART(QString &port_name);
	bool open();
	void receive_datagram(const QByteArray &packet);
	void receiveBytes(const QByteArray &packet);

	static ViscaUART *get_interface(QString port_name);
};

/*
 * VISCA-over-serial transport. Multiple PTZVisca objects on the same
 * RS-422 bus share a single ViscaUART, distinguished by their bus address.
 */
class ViscaSerialTransport : public ViscaTransport {
	Q_OBJECT

private:
	ViscaUART *iface = nullptr;
	void attach_interface(ViscaUART *iface);

public:
	ViscaSerialTransport() = default;
	~ViscaSerialTransport() override;

	QString description(unsigned int address) const override;
	void update(OBSData config) override;
	void save(OBSData config) const override;
	void send(const QByteArray &msg, unsigned int address) override;

	static void add_obs_properties(obs_properties_t *props);
};
