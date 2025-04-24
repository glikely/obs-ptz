/* Pan Tilt Zoom camera pelco-p and pelco-d instance
 *
 * Copyright 2021 Luuk Verhagen <developer@verhagenluuk.nl>
 * Copyright 2020-2021 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <QSerialPortInfo>
#include "ptz-pelco.hpp"

const QByteArray HOME = QByteArray::fromHex("0007002B");

std::map<QString, PelcoUART *> PelcoUART::interfaces;

/*
 * Pelco UART wrapper implementation
 */

void PelcoUART::receive_datagram(const QByteArray &packet)
{
	blog(ptz_debug_level, "%s <-- %s", qPrintable(port_name), packet.toHex(':').data());

	emit receive(packet);
}

void PelcoUART::receiveBytes(const QByteArray &data)
{
	for (auto b : data) {
		rxbuffer += b;
		if (rxbuffer.size() >= messageLength) {
			receive_datagram(rxbuffer);
			rxbuffer.clear();
		}
	}
}

PelcoUART *PelcoUART::get_interface(QString port_name)
{
	PelcoUART *iface;
	blog(ptz_debug_level, "Looking for UART object %s", qPrintable(port_name));

	iface = interfaces[port_name];
	if (!iface) {
		blog(ptz_debug_level, "Creating new Pelco UART object %s", qPrintable(port_name));
		iface = new PelcoUART(port_name);
		iface->open();
		interfaces[port_name] = iface;
	}
	return iface;
}

/*
 * PTZPelco class implementation with -P and -D variants
 */

QString PTZPelco::description()
{
	return QString("PELCO/%1 %2 id:%3").arg(use_pelco_d ? "D" : "P", iface->portName(), QString::number(address));
}

void PTZPelco::attach_interface(PelcoUART *new_iface)
{
	if (iface)
		iface->disconnect(this);
	iface = new_iface;
	if (iface)
		connect(iface, &PelcoUART::receive, this, &PTZPelco::receive);
}

char PTZPelco::checkSum(QByteArray &data)
{
	int sum = 0x00;
	if (use_pelco_d) {
		/* Pelco-D checksum */
		for (char c : data)
			sum += c;
		sum = sum % 100;
	} else {
		/* Pelco-P checksum */
		for (char c : data)
			sum ^= c;
	}

	return sum & 0xff;
}

void PTZPelco::receive(const QByteArray &msg)
{
	unsigned int addr = msg[1];
	if (!use_pelco_d)
		addr++;
	if (addr == this->address)
		ptz_debug("Pelco received: %s", qPrintable(msg.toHex()));
}

void PTZPelco::send(const QByteArray &msg)
{
	QByteArray result = msg;
	if (use_pelco_d) {
		/* Pelco-D datagram */
		result.prepend(address);
		result.append(checkSum(result));
		result.prepend(QByteArray::fromHex("ff"));
	} else {
		/* Pelco-P datagram */
		result.prepend(address - 1);
		result.prepend(QByteArray::fromHex("a0"));
		result.append(QByteArray::fromHex("af"));
		result.append(checkSum(result));
	}

	iface->send(result);

	ptz_debug("Pelco %c command send: %s", use_pelco_d ? 'D' : 'P', qPrintable(result.toHex(':')));
}

void PTZPelco::send(const unsigned char data_1, const unsigned char data_2, const unsigned char data_3,
		    const unsigned char data_4)
{
	QByteArray message;
	message.resize(4);
	message[0] = data_1;
	message[1] = data_2;
	message[2] = data_3;
	message[3] = data_4;

	send(message);
}

void PTZPelco::focus_speed_set(double speed)
{
	send(0x00, 0x27, 0x00, abs(speed) * 0x33);
}

void PTZPelco::zoom_speed_set(double speed)
{
	send(0x00, 0x25, 0x00, abs(speed) * 0x33);
}

PTZPelco::PTZPelco(OBSData data) : PTZDevice(data), iface(NULL)
{
	set_config(data);
	ptz_debug("pelco device created");
}

PTZPelco::~PTZPelco()
{
	attach_interface(nullptr);
}

void PTZPelco::set_config(OBSData config)
{
	PTZDevice::set_config(config);
	const char *uartt = obs_data_get_string(config, "port");
	use_pelco_d = obs_data_get_bool(config, "use_pelco_d");
	address = (unsigned int)obs_data_get_int(config, "address");
	if (!uartt)
		return;

	PelcoUART *ifc = PelcoUART::get_interface(uartt);
	ifc->setConfig(config);
	attach_interface(ifc);
}

OBSData PTZPelco::get_config()
{
	OBSData config = PTZDevice::get_config();
	obs_data_apply(config, iface->getConfig());
	obs_data_set_int(config, "address", address);
	obs_data_set_bool(config, "use_pelco_d", use_pelco_d);
	return config;
}

obs_properties_t *PTZPelco::get_obs_properties()
{
	obs_properties_t *ptz_props = PTZDevice::get_obs_properties();
	obs_property_t *p = obs_properties_get(ptz_props, "interface");
	obs_properties_t *config = obs_property_group_content(p);
	obs_property_set_description(p, "Serial Port");

	iface->addOBSProperties(config);
	obs_properties_add_int(config, "address", "Device ID", 0, 15, 1);
	obs_properties_add_bool(config, "use_pelco_d", "Use Pelco-D");

	return ptz_props;
}

void PTZPelco::do_update()
{
	QByteArray msg = QByteArray::fromHex("00000000");
	bool send_update = false;

	if (status & STATUS_PANTILT_SPEED_CHANGED) {
		status &= ~STATUS_PANTILT_SPEED_CHANGED;
		if (tilt_speed) {
			msg[1] = msg[1] | (tilt_speed > 0.0 ? (1 << 3) : (1 << 4));
			msg[3] = abs(tilt_speed) * 0x3f;
		}
		if (pan_speed) {
			msg[1] = msg[1] | (pan_speed > 0.0 ? (1 << 1) : (1 << 2));
			msg[2] = abs(pan_speed) * 0x3f;
		}
		send_update = true;
	}

	if (status & STATUS_ZOOM_SPEED_CHANGED) {
		status &= ~STATUS_ZOOM_SPEED_CHANGED;
		if (zoom_speed) {
			zoom_speed_set(std::abs(zoom_speed));
			msg[1] = msg[1] | (zoom_speed > 0.0 ? (1 << 5) : (1 << 6));
		}
		send_update = true;
	}

	if (status & STATUS_FOCUS_SPEED_CHANGED) {
		status &= ~STATUS_FOCUS_SPEED_CHANGED;
		if (focus_speed) {
			focus_speed_set(std::abs(focus_speed));
			if (focus_speed > 0.0)
				msg[0] = msg[0] | (1 << 0);
			else
				msg[1] = msg[1] | (1 << 7);
		}
		send_update = true;
	}

	if (send_update) {
		ptz_debug("pan %f, tilt %f, zoom %f, focus %f", pan_speed, tilt_speed, zoom_speed, focus_speed);
		send(msg);
	}
}

void PTZPelco::pantilt_home()
{
	send(HOME);
	ptz_debug("pantilt_home");
}

void PTZPelco::memory_reset(int i)
{
	if (i < 0x01 || i > 0xFF)
		return;

	send(0x00, 0x05, 0x00, i + 1);
	ptz_debug("memory_reset(%i)", i);
}

void PTZPelco::memory_set(int i)
{
	if (i < 0x01 || i > 0xFF)
		return;

	send(0x00, 0x03, 0x00, i + 1);
	ptz_debug("memory_set");
}

void PTZPelco::memory_recall(int i)
{
	if (i < 0x00 || i > 0xFF)
		return;

	send(0x00, 0x07, 0x00, i + 1);
	ptz_debug("memory_recall");
}
