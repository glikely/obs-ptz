/* Pan Tilt Zoom camera pelco-p instance
 *
 * Copyright 2021 Luuk Verhagen <developer@verhagenluuk.nl>
 * Copyright 2020-2021 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include "ptz-pelco-p.hpp"

const QByteArray STOP = QByteArray::fromHex("00000000");
const QByteArray HOME = QByteArray::fromHex("0007002B");
const QByteArray ZOOM_IN = QByteArray::fromHex("00200000");
const QByteArray ZOOM_OUT = QByteArray::fromHex("00400000");

std::map<QString, PelcoPUART*> PelcoPUART::interfaces;

PelcoPUART::PelcoPUART(QString& port_name, qint32 baudrate)
	: port_name(port_name), baud_rate(baudrate)
{
	connect(&uart, &QSerialPort::readyRead, this, &PelcoPUART::poll);
	open();
}

void PelcoPUART::open()
{
	uart.setPortName(port_name);
	uart.setBaudRate(baud_rate);

	if (!uart.open(QIODevice::ReadWrite)) {
		blog(LOG_INFO, "Unable to open UART %s", qPrintable(port_name));
		return;
	}
	blog(LOG_INFO, "Serial connection with %s is opened", qPrintable(port_name));
}

void PelcoPUART::close()
{
	if (uart.isOpen()) {
		uart.close();
		blog(LOG_INFO, "Serial connection %s is closed", qPrintable(port_name));
	}
}

void PelcoPUART::send(const QByteArray& packet)
{
	if (!uart.isOpen())
		return;
	ptz_debug("%s --> %s", packet.toHex(':').data(), qPrintable(port_name));
	uart.write(packet);
}

void PelcoPUART::receive_datagram(const QByteArray& packet)
{
	ptz_debug("%s <-- %s", qPrintable(port_name), packet.toHex(':').data());

	emit receive(packet);
}

void PelcoPUART::poll()
{
	const QByteArray data = uart.readAll();
	for (auto b : data) {
		rxbuffer += b;
		if (rxbuffer.size() >= messageLength) {
			receive_datagram(rxbuffer);
			rxbuffer.clear();
		}
	}
}

PelcoPUART* PelcoPUART::get_interface(QString port_name)
{
	PelcoPUART* iface;
	ptz_debug("Looking for UART object %s", port_name);

	iface = interfaces[port_name];
	return iface;
}

PelcoPUART* PelcoPUART::add_interface(QString port_name, qint32 baudrate)
{
	PelcoPUART* iface = get_interface(port_name);
	if (!iface) {
		ptz_debug("Creating new UART object %s", port_name);
		iface = new PelcoPUART(port_name, baudrate);
		interfaces[port_name] = iface;
		return iface;
	}
	return iface;
}

void PTZPelcoP::attach_interface(PelcoPUART* new_iface)
{
	if (iface)
		iface->disconnect(this);
	iface = new_iface;
	if (iface)
		connect(iface, &PelcoPUART::receive, this, &PTZPelcoP::receive);
}

char PTZPelcoP::checkSum(QByteArray& data)
{
	char sum = 0x00;
	for (char c : data)
		sum ^= c;
	return sum;
}

void PTZPelcoP::receive(const QByteArray &msg)
{
	int address = msg[1] + 1;

	if (address == this->address)
		ptz_debug("Pelco P received: %s", qPrintable(msg.toHex()));
}

void PTZPelcoP::send(const QByteArray& msg)
{
	QByteArray result = QByteArray::fromHex("a000") + msg + QByteArray::fromHex("af00");
	result[1] = address - 1;
	result[7] = checkSum(result);

	iface->send(result);

	ptz_debug("Pelco P command send: %s", qPrintable(result.toHex(':')));
}

void PTZPelcoP::send(const unsigned char data_1, const unsigned char data_2,
		     const unsigned char data_3, const unsigned char data_4)
{
	QByteArray message;
	message.resize(4);
	message[0] = data_1;
	message[1] = data_2;
	message[2] = data_3;
	message[3] = data_4;

	send(message);
}

void PTZPelcoP::zoom_speed_set(double speed)
{
	send(0x00, 0x25, 0x00, abs(speed) * 0x33);
}

PTZPelcoP::PTZPelcoP(OBSData data)
	: PTZDevice("pelco-p"), iface(NULL)
{
	set_config(data);
	ptz_debug("pelco-p device created");
}

PTZPelcoP::~PTZPelcoP()
{
	attach_interface(nullptr);
}

void PTZPelcoP::set_config(OBSData config)
{
	PTZDevice::set_config(config);
	const char* uartt = obs_data_get_string(config, "port");
	address = obs_data_get_int(config, "address");
	if (!uartt)
		return;

	PelcoPUART* iface = PelcoPUART::get_interface(uartt);
	if (!iface)
		iface = PelcoPUART::add_interface(uartt);

	attach_interface(iface);
}

OBSData PTZPelcoP::get_config()
{
	OBSData config = PTZDevice::get_config();
	obs_data_set_int(config, "address", address);
	obs_data_set_string(config, "port", qPrintable(iface->portName()));
	return config;
}

obs_properties_t *PTZPelcoP::get_obs_properties()
{
	obs_properties_t *props = PTZDevice::get_obs_properties();
	obs_property_t *config_prop = obs_properties_get(props, "interface");
	obs_properties_t *config = obs_property_group_content(config_prop);

	obs_property_set_description(config_prop, "Pelco-P Connection");
	obs_properties_add_text(config, "port", "UART Port", OBS_TEXT_DEFAULT);
	obs_properties_add_int(config, "address", "PelcoP ID", 0, 15, 1);
	return props;
}

void PTZPelcoP::pantilt(double pan, double tilt)
{
	unsigned char panTiltData = 0x00;
	if (tilt < -0.005) panTiltData |= (1 << 4);
	if (tilt > 0.005) panTiltData |= (1 << 3);
	if (pan < -0.005) panTiltData |= (1 << 2);
	if (pan > 0.005) panTiltData |= (1 << 1);

	send(0x00, panTiltData, abs(pan) * 0x3F, abs(tilt) * 0x3F);

	ptz_debug("pantilt: pan %f tilt %f", pan, tilt);
}

void PTZPelcoP::pantilt_rel(int pan, int tilt) //What is this?
{
	ptz_debug("pantilt_rel");
}

void PTZPelcoP::pantilt_stop()
{
	send(STOP);
	ptz_debug("pantilt_stop");
}

void PTZPelcoP::pantilt_home()
{
	send(HOME);
	ptz_debug("pantilt_home");
}

void PTZPelcoP::zoom_stop()
{
	send(STOP);
	ptz_debug("zoom_stop");
}

void PTZPelcoP::zoom_tele(double speed)
{
	zoom_speed_set(speed);
	send(ZOOM_IN);
	ptz_debug("zoom_tele");
}

void PTZPelcoP::zoom_wide(double speed)
{
	zoom_speed_set(speed);
	send(ZOOM_OUT);
	ptz_debug("zoom_wide");
}

void PTZPelcoP::memory_reset(int i)
{
	if (i < 0x01 || i > 0xFF)
		return;

	send(0x00, 0x05, 0x00, i - 1);
	ptz_debug("memory_reset");
}

void PTZPelcoP::memory_set(int i)
{
	if (i < 0x01 || i > 0xFF)
		return;

	send(0x00, 0x03, 0x00, i - 1);
	ptz_debug("memory_set");
}

void PTZPelcoP::memory_recall(int i)
{
	if (i < 0x01 || i > 0xFF)
		return;

	send(0x00, 0x07, 0x00, i - 1);
	ptz_debug("memory_recall");
}
