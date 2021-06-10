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
const ViscaInq VISCA_CAM_PowerInq("81090400ff", {new visca_flag("power_on", 2)});


const ViscaCmd VISCA_CAM_Zoom_Stop("8101040700ff");
const ViscaCmd VISCA_CAM_Zoom_Tele("8101040702ff");
const ViscaCmd VISCA_CAM_Zoom_Wide("8101040703ff");
const ViscaCmd VISCA_CAM_Zoom_TeleVar("8101040720ff", {new visca_u4("p", 4),});
const ViscaCmd VISCA_CAM_Zoom_WideVar("8101040730ff", {new visca_u4("p", 4),});
const ViscaCmd VISCA_CAM_Zoom_Direct ("8101044700000000ff", {new visca_s16("zoompos", 4),});
const ViscaInq VISCA_CAM_ZoomPosInq("81090447ff", {new visca_s16("zoom_pos", 2)});

const ViscaCmd VISCA_CAM_DZoom_On("8101040602ff");
const ViscaCmd VISCA_CAM_DZoom_Off("8101040603ff");
const ViscaInq VISCA_CAM_DZoomModeInq("81090406ff", {new visca_flag("dzoom_on", 2)});

const ViscaCmd VISCA_CAM_Focus_Stop(   "8101040800ff");
const ViscaCmd VISCA_CAM_Focus_Far(    "8101040802ff");
const ViscaCmd VISCA_CAM_Focus_Near(   "8101040803ff");
const ViscaCmd VISCA_CAM_Focus_FarVar( "8101040820ff", {new visca_u4("p", 4),});
const ViscaCmd VISCA_CAM_Focus_NearVar("8101040830ff", {new visca_u4("p", 4),});

const ViscaCmd VISCA_CAM_Focus_Auto(      "8101043802ff");
const ViscaCmd VISCA_CAM_Focus_Manual(    "8101043803ff");
const ViscaCmd VISCA_CAM_Focus_AutoManual("8101043810ff");
const ViscaInq VISCA_CAM_FocusModeInq(    "81090438ff", {new visca_flag("autofocus_on", 2)});

const ViscaCmd VISCA_CAM_Focus_OneTouch("8101041801ff");
const ViscaCmd VISCA_CAM_Focus_Infinity("8101041802ff");

const ViscaCmd VISCA_CAM_FocusPos(   "8101044800000000ff", {new visca_s16("focuspos", 4),});
const ViscaInq VISCA_CAM_FocusPosInq("81090448ff", {new visca_s16("focuspos", 2)});

const ViscaCmd VISCA_CAM_Focus_NearLimit(  "8101042800000000ff", {new visca_s16("focus_nearlimit", 4)});
const ViscaInq VISCA_CAM_FocusNearLimitInq("81090428ff", {new visca_s16("focus_near_limit", 2)});

const ViscaCmd VISCA_CAM_ZoomFocus_Direct("810104470000000000000000ff",
		{new visca_s16("zoompos", 4), new visca_s16("focuspos", 8)});

const ViscaCmd VISCA_CAM_AF_SensitivityNormal("8101045802ff");
const ViscaCmd VISCA_CAM_AF_SensitivityLow(   "8101045803ff");
const ViscaInq VISCA_CAM_AFSensitivityInq(    "81090458ff", {new visca_flag("af_sensitivity", 2)});

const ViscaCmd VISCA_CAM_AFMode_Normal(     "8101045700ff");
const ViscaCmd VISCA_CAM_AFMode_Interval(   "8101045701ff");
const ViscaCmd VISCA_CAM_AFMode_ZoomTrigger("8101045702ff");
const ViscaInq VISCA_CAM_AFModeInq(         "81090457ff", {new visca_flag("af_mode", 2)});

const ViscaCmd VISCA_CAM_AFMode_ActiveIntervalTime("8101042700000000ff",
						{new visca_u8("af_movement_time", 4),
						 new visca_u8("af_interval", 6)});

const ViscaInq VISCA_CAM_AFTimeSettingInq("81090427ff", {new visca_u8("move_time", 2), new visca_u8("move_interval", 4)});

const ViscaCmd VISCA_CAM_IRCorrection_Standard("8101041100ff");
const ViscaCmd VISCA_CAM_IRCorrection_IRLight( "8101041101ff");
const ViscaInq VISCA_CAM_IRCorrectionInq(      "81090411ff", {new visca_flag("ircorrection", 2)});

const ViscaCmd VISCA_CAM_WB_Auto(          "8101043500ff");
const ViscaCmd VISCA_CAM_WB_Indoor(        "8101043501ff");
const ViscaCmd VISCA_CAM_WB_Outdoor(       "8101043502ff");
const ViscaCmd VISCA_CAM_WB_OnePush(       "8101043503ff");
const ViscaCmd VISCA_CAM_WB_AutoTracing(   "8101043504ff");
const ViscaCmd VISCA_CAM_WB_Manual(        "8101043505ff");
const ViscaInq VISCA_CAM_WBModeInq(        "81090435ff", {new visca_u4("wbmode", 2)});

const ViscaCmd VISCA_CAM_WB_OnePushTrigger("8101041005ff");

const ViscaCmd VISCA_CAM_RGain_Reset( "8101040300ff");
const ViscaCmd VISCA_CAM_RGain_Up(    "8101040302ff");
const ViscaCmd VISCA_CAM_RGain_Down(  "8101040303ff");
const ViscaCmd VISCA_CAM_RGain_Direct("8101044300000000ff", {new visca_u8("rgain", 6)});
const ViscaInq VISCA_CAM_RGainInq(    "81090443ff", {new visca_u8("rgain", 4)});

const ViscaCmd VISCA_CAM_BGain_Reset( "8101040400ff");
const ViscaCmd VISCA_CAM_BGain_Up(    "8101040402ff");
const ViscaCmd VISCA_CAM_BGain_Down(  "8101040403ff");
const ViscaCmd VISCA_CAM_BGain_Direct("8101044400000000ff", {new visca_u8("bgain", 6)});
const ViscaInq VISCA_CAM_BGainInq(    "81090444ff", {new visca_u8("bgain", 4)});

const ViscaCmd VISCA_CAM_AutoExposure_Auto(           "8101043900ff");
const ViscaCmd VISCA_CAM_AutoExposure_Manual(         "8101043903ff");
const ViscaCmd VISCA_CAM_AutoExposure_ShutterPriority("810104390aff");
const ViscaCmd VISCA_CAM_AutoExposure_IrisPriority(   "810104390bff");
const ViscaCmd VISCA_CAM_AutoExposure_Bright(         "810104390dff");
const ViscaInq VISCA_CAM_AutoExposureModeInq(         "81090439ff", {new visca_u4("aemode", 2)});

const ViscaCmd VISCA_CAM_SlowShutter_Auto(  "8101045a02ff");
const ViscaCmd VISCA_CAM_SlowShutter_Manual("8101045a03ff");
const ViscaInq VISCA_CAM_SlowShutterModeInq("8109045aff", {new visca_u4("slowshuttermode", 2)});

const ViscaCmd VISCA_CAM_Shutter_Reset( "8101040a00ff");
const ViscaCmd VISCA_CAM_Shutter_Up(    "8101040a02ff");
const ViscaCmd VISCA_CAM_Shutter_Down(  "8101040a03ff");
const ViscaCmd VISCA_CAM_Shutter_Direct("8101044a00000000ff", {new visca_u8("shutter", 6)});
const ViscaInq VISCA_CAM_ShutterPosInq( "8109044aff", {new visca_u8("shutterpos", 4)});

const ViscaCmd VISCA_CAM_Iris_Reset( "8101040b00ff");
const ViscaCmd VISCA_CAM_Iris_Up(    "8101040b02ff");
const ViscaCmd VISCA_CAM_Iris_Down(  "8101040b03ff");
const ViscaCmd VISCA_CAM_Iris_Direct("8101044b00000000ff", {new visca_u8("iris", 6)});
const ViscaInq VISCA_CAM_IrisPosInq( "8109044bff", {new visca_u8("irispos", 4)});

const ViscaCmd VISCA_CAM_Gain_Reset( "8101040c00ff");
const ViscaCmd VISCA_CAM_Gain_Up(    "8101040c02ff");
const ViscaCmd VISCA_CAM_Gain_Down(  "8101040c03ff");
const ViscaCmd VISCA_CAM_Gain_Direct("8101044c00000000ff", {new visca_u8("gain", 6)});
const ViscaInq VISCA_CAM_GainPosInq( "8109044cff", {new visca_u8("gainpos", 4)});

const ViscaCmd VISCA_CAM_Gain_Limit( "8101042c00ff", {new visca_u4("ae_gain_limit", 4)});
const ViscaInq VISCA_CAM_GainLimitInq("8109042cff", {new visca_u4("gain_limit", 2)});

const ViscaCmd VISCA_CAM_Bright_Up(    "8101040d02ff");
const ViscaCmd VISCA_CAM_Bright_Down(  "8101040d03ff");
const ViscaCmd VISCA_CAM_Bright_Direct("8101044d00000000ff", {new visca_u8("bright", 6)});
const ViscaInq VISCA_CAM_BrightPosInq("8109044dff", {new visca_u8("brightpos", 4)});

const ViscaCmd VISCA_CAM_ExpComp_On(    "8101043e02ff");
const ViscaCmd VISCA_CAM_ExpComp_Off(   "8101043e03ff");
const ViscaInq VISCA_CAM_ExpCompModeInq("8109043eff", {new visca_u4("expcomp_mode", 2)});

const ViscaCmd VISCA_CAM_ExpComp_Reset( "8101040e00ff");
const ViscaCmd VISCA_CAM_ExpComp_Up(    "8101040e02ff");
const ViscaCmd VISCA_CAM_ExpComp_Down(  "8101040e03ff");
const ViscaCmd VISCA_CAM_ExpComp_Direct("8101044e00000000ff", {new visca_u8("expcomp_pos", 6)});
const ViscaInq VISCA_CAM_ExpCompPosInq( "8109044eff", {new visca_u8("expcomp_pos", 4)});

const ViscaCmd VISCA_CAM_Backlight_On( "8101043302ff");
const ViscaCmd VISCA_CAM_Backlight_Off("8101043303ff");
const ViscaInq VISCA_CAM_BacklightInq( "81090433ff", {new visca_u4("backlight", 2)});

const ViscaCmd VISCA_CAM_WD_Off( "81017e040000ff");
const ViscaCmd VISCA_CAM_WD_Low( "81017e040001ff");
const ViscaCmd VISCA_CAM_WD_Mid( "81017e040002ff");
const ViscaCmd VISCA_CAM_WD_High("81017e040003ff");
const ViscaInq VISCA_CAM_WDInq(  "81097e0400ff", {new visca_u4("wd", 2)});

const ViscaCmd VISCA_CAM_Defog_On( "810104370200ff");
const ViscaCmd VISCA_CAM_Defog_Off("810104370300ff");
const ViscaInq VISCA_CAM_DefogInq( "81090437ff", {new visca_u4("defog", 2)});

const ViscaCmd VISCA_CAM_Apature_Reset( "8101040200ff");
const ViscaCmd VISCA_CAM_Apature_Up(    "8101040202ff");
const ViscaCmd VISCA_CAM_Apature_Down(  "8101040203ff");
const ViscaCmd VISCA_CAM_Apature_Direct("8101044200000000ff", {new visca_u8("apature_gain", 6)});
const ViscaInq VISCA_CAM_ApatureInq(    "81090442ff", {new visca_u8("apature_gain", 4)});

const ViscaCmd VISCA_CAM_HR_On( "8101045202ff");
const ViscaCmd VISCA_CAM_HR_Off("8101045203ff");
const ViscaInq VISCA_CAM_HRInq( "81090452ff", {new visca_u4("hr", 2)});

const ViscaCmd VISCA_CAM_NR(   "8101045300ff", {new visca_u4("nr_level", 4)});
const ViscaInq VISCA_CAM_NRInq("81090453ff", {new visca_u4("nr_level", 2)});

const ViscaCmd VISCA_CAM_Gamma(   "8101045b00ff", {new visca_u4("gamma", 4)});
const ViscaInq VISCA_CAM_GammaInq("8109045bff", {new visca_u4("gamma", 2)});

const ViscaCmd VISCA_CAM_HighSensitivity_On( "8101045e02ff");
const ViscaCmd VISCA_CAM_HighSensitivity_Off("8101045e03ff");
const ViscaInq VISCA_CAM_HighSensitivityInq( "8109045eff", {new visca_u4("high_sensitivity", 2)});

const ViscaCmd VISCA_CAM_PictureEffect_Off(   "8101046300ff");
const ViscaCmd VISCA_CAM_PictureEffect_NegArt("8101046302ff");
const ViscaCmd VISCA_CAM_PictureEffect_BW(    "8101046304ff");
const ViscaInq VISCA_CAM_PictureEffectInq(    "81090463ff", {new visca_u4("picture_effect", 2)});

const ViscaCmd VISCA_CAM_Memory_Reset ("8101043f0000ff", {new visca_u4("preset_num", 5)});
const ViscaCmd VISCA_CAM_Memory_Set   ("8101043f0100ff", {new visca_u4("preset_num", 5)});
const ViscaCmd VISCA_CAM_Memory_Recall("8101043f0200ff", {new visca_u4("preset_num", 5)});

const ViscaCmd VISCA_CAM_IDWrite ("8101042200000000ff", {new visca_u16("camera_id", 4),});
const ViscaInq VISCA_CAM_IDInq(   "81090422ff", {new visca_u16("camera_id", 2)});

const ViscaCmd VISCA_CAM_ChromaSuppress(   "8101045f00ff", {new visca_u4("chroma_suppress", 4)});
const ViscaInq VISCA_CAM_ChromaSuppressInq("8109045fff", {new visca_u4("chroma_suppress", 2)});

const ViscaCmd VISCA_CAM_ColorGain(   "8101044900000000ff", {new visca_u4("color_spec", 6), new visca_u4("color_gain", 7)});
const ViscaInq VISCA_CAM_ColorGainInq("81090449ff", {new visca_u4("color_gain", 4)});

const ViscaCmd VISCA_CAM_ColorHue(   "8101044f00000000ff", {new visca_u4("hue_spec", 6), new visca_u4("hue_phase", 7)});
const ViscaInq VISCA_CAM_ColorHueInq("8109044fff", {new visca_u4("hue_phase", 4)});

const ViscaCmd VISCA_CAM_LowLatency_On( "81017e015a02ff");
const ViscaCmd VISCA_CAM_LowLatency_Off("81017e015a03ff");
const ViscaInq VISCA_CAM_LowLatencyInq( "81097e015aff", {new visca_flag("lowlatency", 2)});

const ViscaCmd VISCA_SYSMenu_Off("8101060603ff");
const ViscaInq VISCA_SYSMenuInq( "81010606ff", {new visca_flag("menumode", 2)});

const ViscaCmd VISCA_CAM_InfoDisplay_On( "81017e011802ff");
const ViscaCmd VISCA_CAM_InfoDisplay_Off("81017e011803ff");
const ViscaInq VISCA_CAM_InfoDisplayInq( "81097e0118ff", {new visca_flag("info_display", 2)});

const ViscaCmd VISCA_VideoFormat_set("81017e011e0000ff", {new visca_u8("video_format", 5)});
const ViscaInq VISCA_VideoFormatInq( "81090623ff", {new visca_u4("video_format", 2)});

const ViscaCmd VISCA_ColorSystem_set("81017e01030000ff", {new visca_u4("color_format", 6)});
const ViscaInq VISCA_ColorSystemInq( "81097e0103ff", {new visca_u4("color_format", 2)});

const ViscaCmd VISCA_IRReceive_On(    "8101060802ff");
const ViscaCmd VISCA_IRReceive_Off(   "8101060803ff");
const ViscaCmd VISCA_IRReceive_Toggle("8101060810ff");
const ViscaInq VISCA_IRReceiveInq(    "81090608ff", {new visca_flag("irreceive", 2)});

const ViscaCmd VISCA_IRReceiveReturn_On( "81017d01030000ff");
const ViscaCmd VISCA_IRReceiveReturn_Off("81017d01130000ff");

const ViscaInq VISCA_IRConditionInq("81090634ff", {new visca_u4("ircondition", 2)});

const ViscaInq VISCA_PanTilt_MaxSpeedInq("81090611ff", {new visca_u7("panmaxspeed", 2), new visca_u7("tiltmaxspeed", 3)});

const ViscaCmd VISCA_PanTilt_drive(    "8101060100000303ff", {new visca_s7("pan", 4), new visca_s7("tilt", 5)});
const ViscaCmd VISCA_PanTilt_drive_abs("8101060200000000000000000000ff",
	                                  {new visca_u7("panspeed", 4), new visca_u7("tiltspeed", 5),
					   new visca_s16("panpos", 6), new visca_s16("tiltpos", 10)});
const ViscaCmd VISCA_PanTilt_drive_rel("8101060300000000000000000000ff",
	                                  {new visca_u7("panspeed", 4), new visca_u7("tiltspeed", 5),
					   new visca_s16("panpos", 6), new visca_s16("tiltpos", 10)});
const ViscaCmd VISCA_PanTilt_Home(     "81010604ff");
const ViscaCmd VISCA_PanTilt_Reset(    "81010605ff");
const ViscaInq VISCA_PanTilt_PosInq(   "81090612ff", {new visca_s16("pan_pos", 2), new visca_s16("tilt_pos", 6)});

const ViscaCmd VISCA_PanTilt_LimitSetUpRight( "8101060700010000000000000000ff", {new visca_u16("pan_limit_right", 6), new visca_u16("tilt_limit_up", 10)});
const ViscaCmd VISCA_PanTilt_LimitSetDownLeft("8101060700000000000000000000ff", {new visca_u16("pan_limit_left", 6), new visca_u16("tilt_limit_down", 10)});
const ViscaCmd VISCA_PanTilt_LimitClearUpRight( "810106070101070f0f0f070f0f0fff", {new visca_u16("pan_limit_right", 6), new visca_u16("tilt_limit_up", 10)});
const ViscaCmd VISCA_PanTilt_LimitClearDownLeft("810106070100070f0f0f070f0f0fff", {new visca_u16("pan_limit_left", 6), new visca_u16("tilt_limit_down", 10)});

#define VISCA_RESPONSE_ADDRESS   0x30
#define VISCA_RESPONSE_ACK       0x40
#define VISCA_RESPONSE_COMPLETED 0x50
#define VISCA_RESPONSE_ERROR     0x60
#define VISCA_PACKET_SENDER(pkt) ((unsigned)((pkt)[0] & 0x70) >> 4)

/*
 * PTZVisca Methods
 */
PTZVisca::PTZVisca(std::string type)
	: PTZDevice(type)
{
	for (int i = 0; i < 8; i++)
		active_cmd[i] = false;
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
	active_cmd[0] = false;
	pending_cmds.clear();
}

void PTZVisca::cmd_get_camera_info()
{
	send(VISCA_CAM_ZoomPosInq);
	send(VISCA_PanTilt_PosInq);
}

void PTZVisca::receive(const QByteArray &msg)
{
	if (VISCA_PACKET_SENDER(msg) != address || (msg.size() < 3))
		return;
	int slot = msg[1] & 0x7;

	switch (msg[1] & 0xf0) {
	case VISCA_RESPONSE_ACK:
		timeout_timer.stop();
		if (active_cmd[0]) {
			pending_cmds.removeFirst();
			active_cmd[0] = false;
			active_cmd[slot] = true;
		}
		break;
	case VISCA_RESPONSE_COMPLETED:
		if (!active_cmd[slot]) {
			blog(LOG_INFO, "VISCA %s spurious reply: %s", qPrintable(objectName()), msg.toHex(':').data());
			break;
		}
		active_cmd[slot] = false;

		/* Slot 0 responses are inquiries that need to be parsed */
		if (slot == 0) {
			timeout_timer.stop();
			pending_cmds.first().decode(this, msg);
			pending_cmds.removeFirst();

			QByteArrayList propnames = dynamicPropertyNames();
			QString logmsg(objectName() + ":");
			for (QByteArrayList::iterator i = propnames.begin(); i != propnames.end(); i++) 
				logmsg = logmsg + " " + QString(i->data()) + "=" + property(i->data()).toString();
			blog(LOG_INFO, qPrintable(logmsg));
		}

		break;
	case VISCA_RESPONSE_ERROR:
		active_cmd[slot] = false;
		if (slot == 0)
			timeout_timer.stop();
		blog(LOG_INFO, "VISCA %s received error: %s", qPrintable(objectName()), msg.toHex(':').data());
		break;
	default:
		blog(LOG_INFO, "VISCA %s received unknown: %s", qPrintable(objectName()), msg.toHex(':').data());
		break;
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
	if (active_cmd[0] || pending_cmds.isEmpty())
		return;
	active_cmd[0] = true;
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
	if (active_cmd[0] || pending_cmds.isEmpty())
		return;
	active_cmd[0] = true;

	QByteArray packet = pending_cmds.first().cmd;
	QByteArray p = QByteArray::fromHex("0100000000000000") + packet;
	p[1] = (0x9 == packet[1]) ? 0x10 : 0x00;
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
