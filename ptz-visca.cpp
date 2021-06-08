/* Pan Tilt Zoom visca instance
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <QNetworkDatagram>
#include "ptz-visca.hpp"
#include <util/base.h>

std::map<QString, ViscaUART*> ViscaUART::interfaces;
std::map<int, ViscaUDPSocket*> ViscaUDPSocket::interfaces;

const ViscaCmd VISCA_ENUMERATE("883001ff");

const ViscaCmd VISCA_Clear("81010001ff");
const ViscaCmd VISCA_CommandCancel("8120ff", {new visca_u4("socket", 1)});
const ViscaCmd VISCA_CAM_Power_On("8101040002ff");
const ViscaCmd VISCA_CAM_Power_Off("8101040003ff");

const ViscaCmd VISCA_CAM_Zoom_Stop("8101040700ff");
const ViscaCmd VISCA_CAM_Zoom_Tele("8101040702ff");
const ViscaCmd VISCA_CAM_Zoom_Wide("8101040703ff");
const ViscaCmd VISCA_CAM_Zoom_TeleVar("8101040720ff", {new visca_u4("p", 4),});
const ViscaCmd VISCA_CAM_Zoom_WideVar("8101040730ff", {new visca_u4("p", 4),});
const ViscaCmd VISCA_CAM_Zoom_Direct ("8101044700000000ff", {new visca_s16("pos", 4),});
const ViscaCmd VISCA_CAM_DZoom_On("8101040602ff");
const ViscaCmd VISCA_CAM_DZoom_Off("8101040603ff");

const ViscaCmd VISCA_CAM_Focus_Stop("8101040800ff");
const ViscaCmd VISCA_CAM_Focus_Far("8101040802ff");
const ViscaCmd VISCA_CAM_Focus_Near("8101040803ff");
const ViscaCmd VISCA_CAM_Focus_FarVar("8101040820ff", {new visca_u4("p", 4),});
const ViscaCmd VISCA_CAM_Focus_NearVar("8101040830ff", {new visca_u4("p", 4),});
const ViscaCmd VISCA_CAM_Focus_Direct("8101044800000000ff", {new visca_s16("pos", 4),});
const ViscaCmd VISCA_CAM_Focus_Auto("8101043802ff");
const ViscaCmd VISCA_CAM_Focus_Manual("8101043803ff");
const ViscaCmd VISCA_CAM_Focus_AutoManual("8101043810ff");
const ViscaCmd VISCA_CAM_Focus_OneTouch("8101041801ff");
const ViscaCmd VISCA_CAM_Focus_Infinity("8101041802ff");
const ViscaCmd VISCA_CAM_NearLimit("8101042800000000ff", {new visca_s16("limit", 4)});
const ViscaCmd VISCA_CAM_Memory_Reset ("8101043f0000ff", {new visca_u4("preset_num", 5)});
const ViscaCmd VISCA_CAM_Memory_Set   ("8101043f0100ff", {new visca_u4("preset_num", 5)});
const ViscaCmd VISCA_CAM_Memory_Recall("8101043f0200ff", {new visca_u4("preset_num", 5)});

const ViscaCmd VISCA_PanTilt_drive("8101060100000303ff", {new visca_s7("pan", 4), new visca_s7("tilt", 5)});
const ViscaCmd VISCA_PanTilt_drive_abs("8101060200000000000000000000ff",
	                                  {new visca_u7("panspeed", 4), new visca_u7("tiltspeed", 5),
					   new visca_s16("panpos", 6), new visca_s16("tiltpos", 10)});
const ViscaCmd VISCA_PanTilt_drive_rel("8101060300000000000000000000ff",
	                                  {new visca_u7("panspeed", 4), new visca_u7("tiltspeed", 5),
					   new visca_s16("panpos", 6), new visca_s16("tiltpos", 10)});
const ViscaCmd VISCA_PanTilt_Home("81010604ff");
const ViscaCmd VISCA_PanTilt_Reset("81010605ff");

const ViscaInq VISCA_PowerInq("81090400ff", {new visca_flag("power_on", 2)});
const ViscaInq VISCA_ZoomPosInq("81090447ff", {new visca_s16("zoom_pos", 2)});
const ViscaInq VISCA_DZoomModeInq("81090406ff", {new visca_flag("dzoom_on", 2)});
const ViscaInq VISCA_FocusModeInq("81090438ff", {new visca_flag("autofocus_on", 2)});
const ViscaInq VISCA_FocusPosInq("81090448ff", {new visca_s16("focus_pos", 2)});
const ViscaInq VISCA_FocusNearLimitInq("81090428ff", {new visca_s16("focus_near_limit", 2)});

const ViscaInq VISCA_PanTiltPosInq("81090612ff", {new visca_s16("pan_pos", 2), new visca_s16("tilt_pos", 6)});

#define VISCA_RESPONSE_ADDRESS   0x30
#define VISCA_RESPONSE_ACK       0x40
#define VISCA_RESPONSE_COMPLETED 0x50
#define VISCA_RESPONSE_ERROR     0x60
#define VISCA_PACKET_SENDER(pkt) ((unsigned)((pkt)[0] & 0x70) >> 4)

/*
 * PTZVisca Methods
 */
PTZVisca::PTZVisca(std::string type)
	: PTZDevice(type), active_cmd(false)
{
	connect(&timeout_timer, &QTimer::timeout, this, &PTZVisca::timeout);
}

void PTZVisca::send(const ViscaCmd &cmd)
{
	pending_cmds.append(cmd);
	send_pending();
}

void PTZVisca::send(const ViscaCmd &cmd, QList<int> args)
{
	pending_cmds.append(cmd);
	pending_cmds.last().encode(args);
	send_pending();
}

void PTZVisca::timeout()
{
	blog(LOG_INFO, "PTZVisca::timeout() %p", this);
	active_cmd = false;
	pending_cmds.clear();
}

void PTZVisca::cmd_get_camera_info()
{
	send(VISCA_ZoomPosInq);
	send(VISCA_PanTiltPosInq);
}

void PTZVisca::receive(const QByteArray &msg)
{
	if (VISCA_PACKET_SENDER(msg) != address)
		return;

	switch (msg[1] & 0xf0) {
	case VISCA_RESPONSE_ACK:
		receive_ack(msg);
		break;
	case VISCA_RESPONSE_COMPLETED:
		receive_complete(msg);
		break;
	case VISCA_RESPONSE_ERROR:
		blog(LOG_DEBUG, "VISCA received error");
		break;
	default:
		blog(LOG_INFO, "VISCA received unknown");
		break;
	}
}

void PTZVisca::receive_ack(const QByteArray &msg)
{
	Q_UNUSED(msg);
	timeout_timer.stop();
	if (active_cmd) {
		active_cmd = false;
		pending_cmds.removeFirst();
	}
	send_pending();
}

void PTZVisca::receive_complete(const QByteArray &msg)
{
	if (!active_cmd)
		return;
	timeout_timer.stop();
	blog(LOG_INFO, "VISCA %p receive_complete slot: %s", this, msg.toHex(':').data());

	pending_cmds.first().decode(this, msg);

	QByteArrayList propnames = dynamicPropertyNames();
	QString logmsg(objectName() + ":");
	for (QByteArrayList::iterator i = propnames.begin(); i != propnames.end(); i++) 
		logmsg = logmsg + " " + QString(i->data()) + "=" + property(i->data()).toString();
	blog(LOG_INFO, qPrintable(logmsg));

	if (active_cmd) {
		active_cmd = false;
		pending_cmds.removeFirst();
	}
	send_pending();
}

void PTZVisca::pantilt(double pan, double tilt)
{
	send(VISCA_PanTilt_drive, {(int)pan, (int)-tilt});
}

void PTZVisca::pantilt_rel(int pan, int tilt)
{
	send(VISCA_PanTilt_drive_rel, {0x14, 0x14, (int)pan, (int)-tilt});
}

void PTZVisca::pantilt_stop()
{
	send(VISCA_PanTilt_drive, {0, 0});
}

void PTZVisca::pantilt_home()
{
	send(VISCA_PanTilt_Home);
}

void PTZVisca::zoom_tele()
{
	send(VISCA_CAM_Zoom_Tele);
}

void PTZVisca::zoom_wide()
{
	send(VISCA_CAM_Zoom_Wide);
}

void PTZVisca::zoom_stop()
{
	send(VISCA_CAM_Zoom_Stop);
}

void PTZVisca::memory_reset(int i)
{
	send(VISCA_CAM_Memory_Reset, {i});
}

void PTZVisca::memory_set(int i)
{
	send(VISCA_CAM_Memory_Set, {i});
}

void PTZVisca::memory_recall(int i)
{
	send(VISCA_CAM_Memory_Recall, {i});
}

/*
 * VISCA over serial UART implementation
 */
ViscaUART::ViscaUART(QString &port_name) : port_name(port_name)
{
	connect(&uart, &QSerialPort::readyRead, this, &ViscaUART::poll);
	open();
}

void ViscaUART::open()
{
	camera_count = 0;

	uart.setPortName(port_name);
	uart.setBaudRate(9600);
	if (!uart.open(QIODevice::ReadWrite)) {
		blog(LOG_INFO, "VISCA Unable to open UART %s", qPrintable(port_name));
		return;
	}

	send(VISCA_ENUMERATE.cmd);
}

void ViscaUART::close()
{
	if (uart.isOpen())
		uart.close();
	camera_count = 0;
}

void ViscaUART::send(const QByteArray &packet)
{
	if (!uart.isOpen())
		return;
	blog(LOG_INFO, "VISCA --> %s", packet.toHex(':').data());
	uart.write(packet);
}

void ViscaUART::receive_datagram(const QByteArray &packet)
{
	blog(LOG_INFO, "VISCA <-- %s", packet.toHex(':').data());
	if (packet.size() < 3)
		return;
	if ((packet[1] & 0xf0) == VISCA_RESPONSE_ADDRESS) {
		switch (packet[1] & 0x0f) { /* Decode Packet Socket Field */
		case 0:
			camera_count = (packet[2] & 0x7) - 1;
			blog(LOG_INFO, "VISCA Interface %s: %i camera%s found", qPrintable(uart.portName()),
				camera_count, camera_count == 1 ? "" : "s");
			emit reset();
			break;
		case 8:
			/* network change, trigger a change */
			send(VISCA_ENUMERATE.cmd);
			break;
		default:
			break;
		}
		return;
	}

	emit receive(packet);
}

void ViscaUART::poll()
{
	const QByteArray data = uart.readAll();
	for (auto b : data) {
		rxbuffer += b;
		if ((b & 0xff) == 0xff) {
			if (rxbuffer.size())
				receive_datagram(rxbuffer);
			rxbuffer.clear();
		}
	}
}

ViscaUART * ViscaUART::get_interface(QString port_name)
{
	ViscaUART *iface;
	qDebug() << "Looking for UART object" << port_name;
	iface = interfaces[port_name];
	if (!iface) {
		qDebug() << "Creating new VISCA object" << port_name;
		iface = new ViscaUART(port_name);
		interfaces[port_name] = iface;
	}
	return iface;
}

PTZViscaSerial::PTZViscaSerial(OBSData config)
	: PTZVisca("visca"), iface(NULL)
{
	set_config(config);
}

PTZViscaSerial::~PTZViscaSerial()
{
	attach_interface(nullptr);
}

void PTZViscaSerial::attach_interface(ViscaUART *new_iface)
{
	if (iface)
		iface->disconnect(this);
	iface = new_iface;
	if (iface) {
		connect(iface, &ViscaUART::receive, this, &PTZViscaSerial::receive);
		connect(iface, &ViscaUART::reset, this, &PTZViscaSerial::reset);
	}
}

void PTZViscaSerial::reset()
{
	send(VISCA_Clear);
	cmd_get_camera_info();
}

void PTZViscaSerial::send_pending()
{
	if (active_cmd || pending_cmds.isEmpty())
		return;
	active_cmd = true;
	pending_cmds.first().setAddress(address);
	iface->send(pending_cmds.first().cmd);
	timeout_timer.setSingleShot(true);
	timeout_timer.start(1000);
}

void PTZViscaSerial::set_config(OBSData config)
{
	PTZDevice::set_config(config);
	const char *uart = obs_data_get_string(config, "port");
	address = obs_data_get_int(config, "address");
	if (!uart)
		return;
	attach_interface(ViscaUART::get_interface(uart));
}

OBSData PTZViscaSerial::get_config()
{
	OBSData config = PTZDevice::get_config();
	obs_data_set_string(config, "port", qPrintable(iface->portName()));
	obs_data_set_int(config, "address", address);
	return config;
}

/*
 * VISCA over IP implementation
 */
ViscaUDPSocket::ViscaUDPSocket(int port) :
	visca_port(port)
{
	visca_socket.bind(QHostAddress::Any, visca_port);
	connect(&visca_socket, &QUdpSocket::readyRead, this, &ViscaUDPSocket::poll);
}

void ViscaUDPSocket::receive_datagram(QNetworkDatagram &dg)
{
	QByteArray data = dg.data();
	int size = data[2] << 8 | data[3];
	blog(LOG_INFO, "VISCA UDP <-- %s", qPrintable(data.toHex(':')));
	if (data.size() == size + 8)
		emit receive(data.mid(8, size));
	else
		emit reset();
}

void ViscaUDPSocket::send(QHostAddress ip_address, const QByteArray &packet)
{
	blog(LOG_INFO, "VISCA UDP --> %s", qPrintable(packet.toHex(':')));
	visca_socket.writeDatagram(packet, ip_address, visca_port);
}

void ViscaUDPSocket::poll()
{
	while (visca_socket.hasPendingDatagrams()) {
		QNetworkDatagram dg = visca_socket.receiveDatagram();
		receive_datagram(dg);
	}
}

ViscaUDPSocket * ViscaUDPSocket::get_interface(int port)
{
	ViscaUDPSocket *iface;
	qDebug() << "Looking for Visca UDP Socket object" << port;
	iface = interfaces[port];
	if (!iface) {
		qDebug() << "Creating new VISCA object" << port;
		iface = new ViscaUDPSocket(port);
		interfaces[port] = iface;
	}
	return iface;
}

PTZViscaOverIP::PTZViscaOverIP(OBSData config)
	: PTZVisca("visca-over-ip"), iface(NULL)
{
	address = 1;
	set_config(config);
}

PTZViscaOverIP::~PTZViscaOverIP()
{
	attach_interface(nullptr);
}

void PTZViscaOverIP::attach_interface(ViscaUDPSocket *new_iface)
{
	if (iface)
		iface->disconnect(this);
	iface = new_iface;
	if (iface) {
		connect(iface, &ViscaUDPSocket::receive, this, &PTZViscaOverIP::receive);
		connect(iface, &ViscaUDPSocket::reset, this, &PTZViscaOverIP::reset);
		reset();
	}
}

void PTZViscaOverIP::reset()
{
	sequence = 1;
	iface->send(ip_address, QByteArray::fromHex("020000010000000001"));
	send(VISCA_Clear);
	cmd_get_camera_info();
}

void PTZViscaOverIP::send_pending()
{
	if (active_cmd || pending_cmds.isEmpty())
		return;
	active_cmd = true;

	QByteArray packet = pending_cmds.first().cmd;
	QByteArray p = QByteArray::fromHex("0100000000000000") + packet;
	p[3] = packet.size();
	p[4] = (sequence >> 24) & 0xff;
	p[5] = (sequence >> 16) & 0xff;
	p[6] = (sequence >> 8) & 0xff;
	p[7] = sequence & 0xff;
	p[8] = '\x81';
	sequence++;

	iface->send(ip_address, p);
	timeout_timer.setSingleShot(true);
	timeout_timer.start(1000);
}

void PTZViscaOverIP::set_config(OBSData config)
{
	PTZDevice::set_config(config);
	const char *ip = obs_data_get_string(config, "address");
	if (ip)
		ip_address = QHostAddress(ip);
	int port = obs_data_get_int(config, "port");
	if (!port)
		port = 52381;
	attach_interface(ViscaUDPSocket::get_interface(port));
}

OBSData PTZViscaOverIP::get_config()
{
	OBSData config = PTZDevice::get_config();
	obs_data_set_string(config, "address", qPrintable(ip_address.toString()));
	obs_data_set_int(config, "port", iface->port());
	return config;
}
