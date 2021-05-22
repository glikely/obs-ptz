/* Pan Tilt Zoom visca instance
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include "ptz-visca.hpp"
#include <util/base.h>

std::map<std::string, ViscaInterface*> ViscaInterface::interfaces;

const ViscaCmd VISCA_ENUMERATE("883001ff");

const ViscaCmd VISCA_Clear("80010001ff");
const ViscaCmd VISCA_CommandCancel("8020ff", {new visca_u4("socket", 1)});
const ViscaCmd VISCA_CAM_Power_On("8001040002ff");
const ViscaCmd VISCA_CAM_Power_Off("8001040003ff");

const ViscaCmd VISCA_CAM_Zoom_Stop("8001040700ff");
const ViscaCmd VISCA_CAM_Zoom_Tele("8001040702ff");
const ViscaCmd VISCA_CAM_Zoom_Wide("8001040703ff");
const ViscaCmd VISCA_CAM_Zoom_TeleVar("8001040720ff", {new visca_u4("p", 4),});
const ViscaCmd VISCA_CAM_Zoom_WideVar("8001040730ff", {new visca_u4("p", 4),});
const ViscaCmd VISCA_CAM_Zoom_Direct ("8001044700000000ff", {new visca_u16("pos", 4),});
const ViscaCmd VISCA_CAM_DZoom_On("8001040602ff");
const ViscaCmd VISCA_CAM_DZoom_Off("8001040603ff");

const ViscaCmd VISCA_CAM_Focus_Stop("8001040800ff");
const ViscaCmd VISCA_CAM_Focus_Far("8001040802ff");
const ViscaCmd VISCA_CAM_Focus_Near("8001040803ff");
const ViscaCmd VISCA_CAM_Focus_FarVar("8001040820ff", {new visca_u4("p", 4),});
const ViscaCmd VISCA_CAM_Focus_NearVar("8001040830ff", {new visca_u4("p", 4),});
const ViscaCmd VISCA_CAM_Focus_Direct("8001044800000000ff", {new visca_u16("pos", 4),});
const ViscaCmd VISCA_CAM_Focus_Auto("8001043802ff");
const ViscaCmd VISCA_CAM_Focus_Manual("8001043803ff");
const ViscaCmd VISCA_CAM_Focus_AutoManual("8001043810ff");
const ViscaCmd VISCA_CAM_Focus_OneTouch("8001041801ff");
const ViscaCmd VISCA_CAM_Focus_Infinity("8001041802ff");
const ViscaCmd VISCA_CAM_NearLimit("8001042800000000ff", {new visca_u16("limit", 4)});
const ViscaCmd VISCA_PanTilt_drive("8001060100000303ff", {new visca_s7("pan", 4), new visca_s7("tilt", 5)});
const ViscaCmd VISCA_PanTilt_drive_abs("8001060200000000000000000000ff",
	                                  {new visca_u7("panspeed", 4), new visca_u7("tiltspeed", 5),
					   new visca_u16("panpos", 6), new visca_u16("tiltpos", 10)});
const ViscaCmd VISCA_PanTilt_drive_rel("8001060300000000000000000000ff",
	                                  {new visca_u7("panspeed", 4), new visca_u7("tiltspeed", 5),
					   new visca_u16("panpos", 6), new visca_u16("tiltpos", 10)});
const ViscaCmd VISCA_PanTilt_Home("80010604ff");
const ViscaCmd VISCA_PanTilt_Reset("80010605ff");

const ViscaInq VISCA_PowerInq("80090400ff", {new visca_flag("power_on", 2)});
const ViscaInq VISCA_ZoomPosInq("80090447ff", {new visca_u16("zoom_pos", 2)});
const ViscaInq VISCA_DZoomModeInq("80090406ff", {new visca_flag("dzoom_on", 2)});
const ViscaInq VISCA_FocusModeInq("80090438ff", {new visca_flag("autofocus_on", 2)});
const ViscaInq VISCA_FocusPosInq("80090448ff", {new visca_u16("focus_pos", 2)});
const ViscaInq VISCA_FocusNearLimitInq("80090428ff", {new visca_u16("focus_near_limit", 2)});

const ViscaInq VISCA_PanTiltPosInq("80090612ff", {new visca_u16("pan_pos", 2), new visca_u16("tilt_pos", 6)});

void ViscaInterface::open()
{
	camera_count = 0;

	uart.setPortName(QString::fromStdString(uart_name));
	uart.setBaudRate(9600);
	if (!uart.open(QIODevice::ReadWrite)) {
		blog(LOG_INFO, "VISCA Unable to open UART %s", uart_name.c_str());
		return;
	}

	connect(&uart, &QSerialPort::readyRead, this, &ViscaInterface::poll);

	send(VISCA_ENUMERATE.cmd);
}

void ViscaInterface::close()
{
	if (uart.isOpen())
		uart.close();
	camera_count = 0;
}

void ViscaInterface::send(const QByteArray &packet)
{
	if (!uart.isOpen())
		return;
	blog(LOG_INFO, "VISCA --> %s", packet.toHex(':').data());
	uart.write(packet);
}

#define VISCA_RESPONSE_ADDRESS   0x30
#define VISCA_RESPONSE_ACK       0x40
#define VISCA_RESPONSE_COMPLETED 0x50
#define VISCA_RESPONSE_ERROR     0x60
#define VISCA_PACKET_SENDER(pkt) (((pkt)[0] & 0x70) >> 4)

void ViscaInterface::receive(const QByteArray &packet)
{
	blog(LOG_INFO, "VISCA <-- %s", packet.toHex(':').data());
	if (packet.size() < 3)
		return;
	switch (packet[1] & 0xf0) { /* Decode response field */
	case VISCA_RESPONSE_ADDRESS:
		switch (packet[1] & 0x0f) { /* Decode Packet Socket Field */
		case 0:
			camera_count = (packet[2] & 0x7) - 1;
			blog(LOG_INFO, "VISCA Interface %s: %i camera%s found", uart_name.c_str(),
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
		break;
	case VISCA_RESPONSE_ACK:
		/* Don't do anything with the ack yet */
		blog(LOG_DEBUG, "VISCA received ACK");
		emit receive_ack(packet);
		break;
	case VISCA_RESPONSE_COMPLETED:
		blog(LOG_DEBUG, "VISCA received complete");
		emit receive_complete(packet);
		break;
	case VISCA_RESPONSE_ERROR:
		blog(LOG_DEBUG, "VISCA received error");
		emit receive_error(packet);
		break;
	default:
		blog(LOG_INFO, "VISCA received unknown");
		/* Unknown */
		break;
	}
}

void ViscaInterface::poll()
{
	const QByteArray data = uart.readAll();
	for (int i = 0; i < data.size(); i++) {
		rxbuffer += data[i];
		if ((data[i] & 0xff) == 0xff) {
			if (rxbuffer.size())
				receive(rxbuffer);
			rxbuffer.clear();
		}
	}
}

ViscaInterface * ViscaInterface::get_interface(std::string uart)
{
	ViscaInterface *iface;
	qDebug() << "Looking for UART object" << uart.c_str();
	iface = interfaces[uart];
	if (!iface) {
		qDebug() << "Creating new VISCA object" << uart.c_str();
		iface = new ViscaInterface(uart);
		interfaces[uart] = iface;
	}
	return iface;
}

/*
 * PTZVisca Methods
 */
PTZVisca::PTZVisca(const char *uart_name, int address)
	: PTZDevice("visca"), active_cmd(false)
{
	attach_interface(ViscaInterface::get_interface(uart_name));
}

PTZVisca::PTZVisca(obs_data_t *config)
	: PTZDevice("visca"), active_cmd(false), iface(NULL)
{
	set_config(config);
	connect(&timeout_timer, &QTimer::timeout, this, &PTZVisca::timeout);
}

PTZVisca::~PTZVisca()
{
	pantilt(0, 0);
	zoom_stop();
}

void PTZVisca::send_pending()
{
	if (active_cmd || pending_cmds.isEmpty())
		return;
	active_cmd = true;
	iface->send(pending_cmds.first().cmd);
	timeout_timer.setSingleShot(true);
	timeout_timer.start(1000);
}

void PTZVisca::send(const ViscaCmd &cmd)
{
	pending_cmds.append(cmd);
	pending_cmds.last().encode(address);
	send_pending();
}

void PTZVisca::send(const ViscaCmd &cmd, QList<int> args)
{
	pending_cmds.append(cmd);
	pending_cmds.last().encode(address, args);
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

void PTZVisca::reset()
{
	send(VISCA_Clear);
	cmd_get_camera_info();
}

void PTZVisca::attach_interface(ViscaInterface *new_iface)
{
	if (iface)
		iface->disconnect(this);
	iface = new_iface;
	if (iface) {
		connect(iface, &ViscaInterface::receive_ack, this, &PTZVisca::receive_ack);
		connect(iface, &ViscaInterface::receive_complete, this, &PTZVisca::receive_complete);
		connect(iface, &ViscaInterface::reset, this, &PTZVisca::reset);
	}
}

void PTZVisca::set_config(obs_data_t *config)
{
	PTZDevice::set_config(config);
	const char *uart = obs_data_get_string(config, "port");
	address = obs_data_get_int(config, "address");
	if (!uart)
		return;

	blog(LOG_INFO, "PTZVisca::set_config() %p iface=%p", this, iface);
	attach_interface(ViscaInterface::get_interface(uart));
}

obs_data_t * PTZVisca::get_config()
{
	obs_data_set_int(config, "address", address);
	return PTZDevice::get_config();
}

void PTZVisca::receive_ack(const QByteArray &msg)
{
	if (VISCA_PACKET_SENDER(msg) != address)
		return;
	timeout_timer.stop();
	if (active_cmd) {
		active_cmd = false;
		pending_cmds.removeFirst();
	}
	send_pending();
}

void PTZVisca::receive_complete(const QByteArray &msg)
{
	if (VISCA_PACKET_SENDER(msg) != address || !active_cmd)
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
