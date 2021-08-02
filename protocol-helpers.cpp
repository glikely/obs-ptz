/* Pan Tilt Zoom UART wrapper class
 *
 * Copyright 2021 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <QSerialPortInfo>
#include <QSerialPort>
#include "protocol-helpers.hpp"
#include "ptz-device.hpp"

PTZUARTWrapper::PTZUARTWrapper(QString &port_name) :
	port_name(port_name)
{
	connect(&uart, &QSerialPort::readyRead, this, &PTZUARTWrapper::poll);
	uart.setPortName(port_name);
	open();
}

bool PTZUARTWrapper::open()
{
	bool rc = uart.open(QIODevice::ReadWrite);
	if (!rc)
		blog(LOG_INFO, "VISCA Unable to open UART %s", qPrintable(port_name));
	return rc;
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

int PTZUARTWrapper::baudRate()
{
	return uart.baudRate();
}

void PTZUARTWrapper::setConfig(OBSData config)
{
	setBaudRate(obs_data_get_int(config, "baud_rate"));
}

OBSData PTZUARTWrapper::getConfig()
{
	OBSData config = obs_data_create();
	obs_data_release(config);
	obs_data_set_string(config, "port", qPrintable(portName()));
	obs_data_set_int(config, "baud_rate", baudRate());
	return config;
}

const QList<QSerialPort::BaudRate> common_baud_rates({
	QSerialPort::Baud2400,
	QSerialPort::Baud4800,
	QSerialPort::Baud9600,
	QSerialPort::Baud38400,
	QSerialPort::Baud115200 });

void PTZUARTWrapper::addOBSProperties(obs_properties_t *props)
{
	obs_property_t *p;

	p = obs_properties_add_list(props, "port", "UART Port", OBS_COMBO_TYPE_EDITABLE,
				OBS_COMBO_FORMAT_STRING);
	Q_FOREACH(auto port, QSerialPortInfo::availablePorts()) {
		std::string name = port.portName().toStdString();
		obs_property_list_add_string(p, name.c_str(), name.c_str());
	}

	p = obs_properties_add_list(props, "baud_rate", "Baud Rate", OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	for(QSerialPort::BaudRate baud_rate : common_baud_rates) {
		std::string baud_rate_string = std::to_string(baud_rate);
		obs_property_list_add_int(p, baud_rate_string.c_str(), baud_rate);
	}
}

void PTZUARTWrapper::send(const QByteArray &packet)
{
	if (!uart.isOpen())
		return;
	ptz_debug("%s --> %s", qPrintable(port_name), packet.toHex(':').data());
	uart.write(packet);
}

void PTZUARTWrapper::poll()
{
	receiveBytes(uart.readAll());
};
