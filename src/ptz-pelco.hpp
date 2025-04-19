/* Pan Tilt Zoom camera pelco-p and pelco-d protocol support
 *
 * Copyright 2021 Luuk Verhagen <developer@verhagenluuk.nl>
 * Copyright 2020-2021 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#pragma once
#include "ptz-device.hpp"
#include <util/base.h>
#include <QObject>
#include <QDebug>
#include <QStringListModel>
#include <QtGlobal>
#include "protocol-helpers.hpp"
#include "uart-wrapper.hpp"

/*
 * General Serial UART class
 */
class PelcoUART : public PTZUARTWrapper {
	Q_OBJECT

private:
	static std::map<QString, PelcoUART *> interfaces;
	const int messageLength = 8;

public:
	PelcoUART(QString &port_name) : PTZUARTWrapper(port_name) {}
	void receive_datagram(const QByteArray &packet);
	void receiveBytes(const QByteArray &packet);

	static PelcoUART *get_interface(QString port_name);
};

class PTZPelco : public PTZDevice {
private:
	bool use_pelco_d =
		false; // Flag that Pelco-D is used instead of Pelco-P
	PelcoUART *iface;
	void attach_interface(PelcoUART *new_iface);
	char checkSum(QByteArray &data);

protected:
	unsigned int address;

	void send(const QByteArray &msg);
	void send(const unsigned char data_1, const unsigned char data_2,
		  const unsigned char data_3, const unsigned char data_4);
	void focus_speed_set(double speed);
	void zoom_speed_set(double speed);
	void receive(const QByteArray &msg);

public:
	PTZPelco(OBSData data);
	~PTZPelco();
	virtual QString description();

	void set_config(OBSData ptz_data);
	OBSData get_config();
	obs_properties_t *get_obs_properties();

	void do_update();
	void pantilt_home();
	void memory_reset(int i);
	void memory_set(int i);
	void memory_recall(int i);
};
