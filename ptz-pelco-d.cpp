/* Pan Tilt Zoom camera pelco-d instance
 *
 * Copyright 2021 Luuk Verhagen <developer@verhagenluuk.nl>
 * Copyright 2020-2021 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <QSerialPortInfo>
#include "ptz-pelco-d.hpp"

const QByteArray STOP = QByteArray::fromHex("00000000");
const QByteArray HOME = QByteArray::fromHex("0007002B");
const QByteArray ZOOM_IN = QByteArray::fromHex("00200000");
const QByteArray ZOOM_OUT = QByteArray::fromHex("00400000");

std::map<QString, PelcoDUART*> PelcoDUART::interfaces;

void PelcoDUART::receive_datagram(const QByteArray& packet)
{
	ptz_debug("%s <-- %s", qPrintable(port_name), packet.toHex(':').data());

	emit receive(packet);
}

void PelcoDUART::receiveBytes(const QByteArray &data)
{
	for (auto b : data) {
		rxbuffer += b;
		if (rxbuffer.size() >= messageLength) {
			receive_datagram(rxbuffer);
			rxbuffer.clear();
		}
	}
}

PelcoDUART* PelcoDUART::get_interface(QString port_name)
{
	PelcoDUART* iface;
	ptz_debug("Looking for UART object %s", qPrintable(port_name));

	iface = interfaces[port_name];
	if (!iface) {
		ptz_debug("Creating new Pelco-D UART object %s", qPrintable(port_name));
		iface = new PelcoDUART(port_name);
		interfaces[port_name] = iface;
	}
	return iface;
}

void PTZPelcoD::attach_interface(PelcoDUART* new_iface)
{
	if (iface)
		iface->disconnect(this);
	iface = new_iface;
	if (iface)
		connect(iface, &PelcoDUART::receive, this, &PTZPelcoD::receive);
}

char PTZPelcoD::checkSum(QByteArray& data)
{
	int sum = 0x00;
	for (char c : data)
		sum += c;

	sum = sum % 100;

	return sum;
}

void PTZPelcoD::receive(const QByteArray &msg)
{
	int address = msg[1];

	if (address == this->address)
		ptz_debug("Pelco D received: %s", qPrintable(msg.toHex()));
}

void PTZPelcoD::send(const QByteArray& msg)
{
	QByteArray result = QByteArray::fromHex("ff00") + msg + QByteArray::fromHex("00");
	result[1] = address;
	result[6] = checkSum(result.mid(1, 5));

	iface->send(result);

	ptz_debug("Pelco D command send: %s", qPrintable(result.toHex(':')));
}

void PTZPelcoD::send(const unsigned char data_1, const unsigned char data_2,
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

void PTZPelcoD::zoom_speed_set(double speed)
{
	send(0x00, 0x25, 0x00, abs(speed) * 0x33);
}

PTZPelcoD::PTZPelcoD(OBSData data)
	: PTZDevice("pelco-d"), iface(NULL)
{
	set_config(data);
	ptz_debug("pelco-d device created");
	auto_settings_filter += {"port", "address", "baud_rate"};
}

PTZPelcoD::~PTZPelcoD()
{
	attach_interface(nullptr);
}

void PTZPelcoD::set_config(OBSData config)
{
	PTZDevice::set_config(config);
	const char* uartt = obs_data_get_string(config, "port");
	address = obs_data_get_int(config, "address");
	if (!uartt)
		return;

	PelcoDUART* iface = PelcoDUART::get_interface(uartt);
	iface->setConfig(config);

	attach_interface(iface);
}

OBSData PTZPelcoD::get_config()
{
	OBSData config = PTZDevice::get_config();
	obs_data_apply(config, iface->getConfig());
	obs_data_set_int(config, "address", address);
	return config;
}

obs_properties_t * PTZPelcoD::get_obs_properties()
{
	obs_properties_t *props = PTZDevice::get_obs_properties();
	obs_property_t *p = obs_properties_get(props, "interface");
	obs_properties_t *config = obs_property_group_content(p);
	obs_property_set_description(p, "Pelco-D Connection");

	iface->addOBSProperties(config);
	obs_properties_add_int(config, "address", "Pelco-D ID", 0, 15, 1);

	return props;
}

void PTZPelcoD::pantilt(double pan, double tilt)
{
	unsigned char panTiltData = 0x00;
	if (tilt < -0.005) panTiltData |= (1 << 4);
	if (tilt > 0.005) panTiltData |= (1 << 3);
	if (pan < -0.005) panTiltData |= (1 << 2);
	if (pan > 0.005) panTiltData |= (1 << 1);

	send(0x00, panTiltData, abs(pan) * 0x3F, abs(tilt) * 0x3F);

	ptz_debug("pantilt: pan %f tilt %f", pan, tilt);
}

void PTZPelcoD::pantilt_rel(int pan, int tilt)
{
	ptz_debug("pantilt_rel");
}

void PTZPelcoD::pantilt_stop()
{
	send(STOP);
	ptz_debug("pantilt_stop");
}

void PTZPelcoD::pantilt_home()
{
	send(HOME);
	ptz_debug("pantilt_home");
}

void PTZPelcoD::zoom_stop()
{
	send(STOP);
	ptz_debug("zoom_stop");
}

void PTZPelcoD::zoom_tele(double speed)
{
	zoom_speed_set(speed);
	send(ZOOM_IN);
	ptz_debug("zoom_tele");
}

void PTZPelcoD::zoom_wide(double speed)
{
	zoom_speed_set(speed);
	send(ZOOM_OUT);
	ptz_debug("zoom_wide");
}

void PTZPelcoD::memory_reset(int i)
{
	if (i < 0x01 || i > 0xFF)
		return;

	send(0x00, 0x05, 0x00, i + 1);
	ptz_debug("memory_reset");
}

void PTZPelcoD::memory_set(int i)
{
	if (i < 0x01 || i > 0xFF)
		return;

	send(0x00, 0x03, 0x00, i + 1);
	ptz_debug("memory_set");
}

void PTZPelcoD::memory_recall(int i)
{
	if (i < 0x00 || i > 0xFF)
		return;

	send(0x00, 0x07, 0x00, i + 1);
	ptz_debug("memory_recall");
}
