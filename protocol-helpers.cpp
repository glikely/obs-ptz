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
