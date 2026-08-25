/* Pan Tilt Zoom UART wrapper class
 *
 * Copyright 2021 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include "ptz.h"
#include "uart-wrapper.hpp"

// Q_OS_MACOS/Q_OS_WIN are only defined once a Qt header has been pulled
// in (by uart-wrapper.hpp above), so this check has to come after it.
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
#include <qserialportinfo.h>
#else
#include <QSerialPortInfo>
#endif
#include <QMetaEnum>

#include <obs-module.h>

PTZUARTWrapper::PTZUARTWrapper(QString &port_name) : port_name(port_name)
{
	connect(&uart, &QSerialPort::readyRead, this, &PTZUARTWrapper::poll);
	connect(&uart, &QSerialPort::errorOccurred, this, &PTZUARTWrapper::handleError);
	uart.setPortName(port_name);

	reconnect_timer.setInterval(reconnect_poll_interval_ms);
	connect(&reconnect_timer, &QTimer::timeout, this, &PTZUARTWrapper::open);
}

bool PTZUARTWrapper::open()
{
	if (uart.isOpen()) {
		reconnect_timer.stop();
		return true;
	}

	bool rc = uart.open(QIODevice::ReadWrite);
	if (!rc) {
		/* Kick off the reconnect timer to try again later */
		if (!reconnect_timer.isActive()) {
			blog(LOG_INFO, "Unable to open UART %s", qPrintable(port_name));
			reconnect_timer.start();
		}
		return false;
	}

	if (reconnect_timer.isActive()) {
		blog(LOG_INFO, "UART %s reconnected", qPrintable(port_name));
		reconnect_timer.stop();
	}
	return true;
}

void PTZUARTWrapper::close()
{
	if (uart.isOpen())
		uart.close();
}

void PTZUARTWrapper::setBaudRate(int baudRate)
{
	if (!baudRate || baudRate == uart.baudRate())
		return;

	close();
	uart.setBaudRate(baudRate);
	open();
}

int PTZUARTWrapper::baudRate() const
{
	return uart.baudRate();
}

void PTZUARTWrapper::setConfig(OBSData config)
{
	setBaudRate((int)obs_data_get_int(config, "baud_rate"));
}

void PTZUARTWrapper::save(OBSData config) const
{
	obs_data_set_string(config, "port", qPrintable(portName()));
	obs_data_set_int(config, "baud_rate", baudRate());
}

void PTZUARTWrapper::addOBSProperties(obs_properties_t *props)
{
	obs_property_t *p;

	p = obs_properties_add_list(props, "port", obs_module_text("PTZ.Device.SerialPort"), OBS_COMBO_TYPE_EDITABLE,
				    OBS_COMBO_FORMAT_STRING);
	Q_FOREACH(auto port, QSerialPortInfo::availablePorts())
	{
		std::string name = port.portName().toStdString();
		obs_property_list_add_string(p, name.c_str(), name.c_str());
	}

	p = obs_properties_add_list(props, "baud_rate", obs_module_text("PTZ.Device.SerialBaud"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	QMetaEnum e = QMetaEnum::fromType<QSerialPort::BaudRate>();
	for (int i = 0; i < e.keyCount(); i++) {
		auto baud_rate = (QSerialPort::BaudRate)e.value(i);
		auto baud_rate_string = std::to_string(baud_rate);
		if (baud_rate < 0)
			continue;
		obs_property_list_add_int(p, baud_rate_string.c_str(), baud_rate);
	}
}

void PTZUARTWrapper::send(const QByteArray &packet)
{
	if (!uart.isOpen())
		return;
	uart.write(packet);
}

void PTZUARTWrapper::poll()
{
	receiveBytes(uart.readAll());
};

void PTZUARTWrapper::handleError(QSerialPort::SerialPortError error)
{
	switch (error) {
	case QSerialPort::ResourceError:
	case QSerialPort::ReadError:
	case QSerialPort::WriteError:
		blog(LOG_INFO, "UART %s error: code=%d, \"%s\"", qPrintable(port_name), (int)error,
		     qPrintable(uart.errorString()));
		close();
		/* Defer the reopen to reconnect_timer */
		if (!reconnect_timer.isActive())
			reconnect_timer.start();
		break;
	default:
		return;
	}
}
