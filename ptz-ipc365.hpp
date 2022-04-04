/* Pan Tilt Zoom IPC365 implementation
 *
 * Copyright 2022 Jonatã Bolzan Loss <jonata@jonata.org>
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <QObject>
#include <QTcpSocket>
#include "ptz-device.hpp"

class PTZIPC365 : public PTZDevice {
	Q_OBJECT

private:
	QTcpSocket tcp_socket;
	QByteArray rxbuffer;
	QString host;
	int port;

protected:
	void send(const QByteArray &msg);

private slots:
	void connectSocket();
	void on_socket_stateChanged(QAbstractSocket::SocketState);

public:
	PTZIPC365(OBSData config);

	void set_config(OBSData ptz_data);
	OBSData get_config();

    obs_properties_t *get_obs_properties();

    void pantilt(double pan, double tilt);
	void pantilt_abs(int pan, int tilt);
    void pantilt_home();
};
