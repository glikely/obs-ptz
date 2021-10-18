/* Pan Tilt Zoom visca instance
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include <QNetworkDatagram>
#include <QSerialPortInfo>
#include "ptz-visca.hpp"
#include <util/base.h>

std::map<QString, ViscaUART*> ViscaUART::interfaces;
std::map<int, ViscaUDPSocket*> ViscaUDPSocket::interfaces;

/* Visca specific datagram field classes */
class visca_u4 : public int_field {
public:
	visca_u4(const char *name, int offset) : int_field(name, offset, 0x0f) { }
};

class visca_flag : public datagram_field {
public:
	visca_flag(const char *name, int offset) : datagram_field(name, offset) { }
	void encode(QByteArray &msg, int val) {
		if (msg.size() < offset + 1)
			return;
		msg[offset] = val ? 0x2 : 0x3;
	}
	bool decode(OBSData data, QByteArray &msg) {
		if (msg.size() < offset + 1)
			return false;
		switch (msg[offset]) {
		case 0x02:
			obs_data_set_bool(data, name, true); break;
		case 0x03:
			obs_data_set_bool(data, name, false); break;
		default:
			return false;
		}
		return true;
	}
};

class visca_u7 : public int_field {
public:
	visca_u7(const char *name, int offset) : int_field(name, offset, 0x7f) { }
};

class visca_s7 : public datagram_field {
public:
	visca_s7(const char *name, int offset) : datagram_field(name, offset) { }
	void encode(QByteArray &msg, int val) {
		if (msg.size() < offset + 3)
			return;
		msg[offset] = qMax(abs(val) & 0x7f, 1);
		msg[offset+2] = 3;
		if (val)
			msg[offset+2] = val < 0 ? 1 : 2;
	}
	bool decode(OBSData data, QByteArray &msg) {
		if (msg.size() < offset + 3)
			return false;
		int val = msg[offset] & 0x7f;
		switch (msg[offset+2]) {
		case 0x01:
			obs_data_set_int(data, name, -val); break;
		case 0x02:
			obs_data_set_int(data, name, val); break;
		default:
			return false;
		}
		return true;
	}
};

class visca_u8 : public int_field {
public:
	visca_u8(const char *name, int offset) : int_field(name, offset, 0x0f0f) { }
};

/* 15 bit value encoded into two bytes. Protocol encoding forces bit 15 & 7 to zero */
class visca_u15 : public datagram_field {
public:
	visca_u15(const char *name, int offset) : datagram_field(name, offset) { }
	void encode(QByteArray &msg, int val) {
		if (msg.size() < offset + 2)
			return;
		msg[offset] = (val >> 8) & 0x7f;
		msg[offset+1] = val & 0x7f;
	}
	bool decode(OBSData data, QByteArray &msg) {
		if (msg.size() < offset + 2)
			return false;
		uint16_t val = (msg[offset] & 0x7f) << 8 |
			(msg[offset+1] & 0x7f);
		obs_data_set_int(data, name, val);
		return true;
	}
};

class visca_s16 : public int_field {
public:
	visca_s16(const char *name, int offset) : int_field(name, offset, 0x0f0f0f0f, true) { }
};

class visca_u16 : public int_field {
public:
	visca_u16(const char *name, int offset) : int_field(name, offset, 0x0f0f0f0f) { }
};

const PTZCmd VISCA_ENUMERATE("883001ff");
const PTZCmd VISCA_IF_CLEAR("88010001ff");

const PTZInq VISCA_CAM_VersionInq("81090002ff", {
		new int_field("vendor_id", 2, 0x7fff),
		new int_field("model_id", 4, 0x7fff),
		new string_lookup_field("vendor_name", PTZVisca::viscaVendors, 2, 0x7fff),
		new string_lookup_field("model_name", PTZVisca::viscaModels, 2, 0x7fffffff),
		new int_field("rom_version", 6, 0xffff),
		new int_field("socket_number", 8, 0xff)
	});

const PTZInq VISCA_LensControlInq("81097e7e00ff", {
		new int_field("zoom_pos", 2, 0x0f0f0f0f),
		new int_field("focus_near_limit", 6, 0x0f0f0f0f),
		new int_field("focus_pos", 8, 0x0f0f0f0f),
		new int_field("focus_af_mode", 13, 0b00011000),
		new bool_field("focus_af_sensitivity", 13, 0b0100),
		new bool_field("dzoom", 13, 0b0010),
		new bool_field("focus_af_enabled", 13, 0b0001),
		new bool_field("low_contrast_mode", 14, 0b1000)
	});

const PTZInq VISCA_CameraControlInq("81097e7e01ff", {
		new visca_u8("r_gain", 2),
		new visca_u8("b_gain", 4),
		new visca_u4("wb_mode", 6),
		new visca_u4("aperature_gain", 7),
		new visca_u4("exposure_mode", 8),
		new bool_field("high_resolution", 9, 0b00100000),
		new bool_field("wide_d", 9, 0b00010000),
		new bool_field("back_light", 9, 0b1000),
		new bool_field("exposure_comp", 9, 0b1000),
		new bool_field("slow_shutter", 9, 0b0001),
		new int_field("shutter_pos", 10, 0x1f),
		new int_field("iris_pos", 11, 0x1f),
		new int_field("gain_pos", 12, 0x1f),
		new int_field("bright_pos", 13, 0x1f),
		new int_field("exposure_comp_pos", 14, 0x0f)
	});

const PTZInq VISCA_OtherInq("81097e7e02ff", {
		new int_field("power", 2, 0b0001),
		new int_field("picture_effect_mode", 5, 0x0f),
		new int_field("camera_id", 8, 0x0f0f0f0f),
		new int_field("framerate", 12, 0b0001)
	});

const PTZInq VISCA_EnlargementFunction1Inq("81097e7e03ff", {
		new int_field("dzoom_pos", 2, 0x0f0f),
		new int_field("focus_af_move_time", 4, 0x0f0f),
		new int_field("focus_af_interval_time", 6, 0x0f0f),
		new int_field("color_gain", 11, 0b01111000),
		new int_field("gamma", 13, 0b01110000),
		new bool_field("high_sensitivity", 13, 0b00001000),
		new int_field("nr_level", 13, 0b00000111),
		new int_field("chroma_suppress", 14, 0b01110000),
		new int_field("gain_limit", 14, 0b00001111),
	});

const PTZInq VISCA_EnlargementFunction2Inq("81097e7e04ff", {
		new bool_field("defog_mode", 7, 0b0001)
	});

const PTZInq VISCA_EnlargementFunction3Inq("81097e7e05ff", {
		new int_field("color_hue", 2, 0b1111)
	});

const PTZCmd VISCA_Clear("81010001ff");
const PTZCmd VISCA_CommandCancel("8120ff", {new visca_u4("socket", 1)});
const PTZCmd VISCA_CAM_Power_On("8101040002ff");
const PTZCmd VISCA_CAM_Power_Off("8101040003ff");
const PTZInq VISCA_CAM_PowerInq("81090400ff", {new visca_flag("power_on", 2)});


const PTZCmd VISCA_CAM_Zoom_Stop("8101040700ff");
const PTZCmd VISCA_CAM_Zoom_Tele("8101040702ff");
const PTZCmd VISCA_CAM_Zoom_Wide("8101040703ff");
const PTZCmd VISCA_CAM_Zoom_TeleVar("8101040720ff", {new visca_u4("p", 4),});
const PTZCmd VISCA_CAM_Zoom_WideVar("8101040730ff", {new visca_u4("p", 4),});
const PTZCmd VISCA_CAM_Zoom_Direct ("8101044700000000ff", {new visca_s16("zoom_pos", 4),});
const PTZInq VISCA_CAM_ZoomPosInq("81090447ff", {new visca_s16("zoom_pos", 2)});

const PTZCmd VISCA_CAM_DZoom_On("8101040602ff");
const PTZCmd VISCA_CAM_DZoom_Off("8101040603ff");
const PTZInq VISCA_CAM_DZoomModeInq("81090406ff", {new visca_flag("dzoom_on", 2)});

const PTZCmd VISCA_CAM_Focus_Stop(   "8101040800ff");
const PTZCmd VISCA_CAM_Focus_Far(    "8101040802ff");
const PTZCmd VISCA_CAM_Focus_Near(   "8101040803ff");
const PTZCmd VISCA_CAM_Focus_FarVar( "8101040820ff", {new visca_u4("p", 4),});
const PTZCmd VISCA_CAM_Focus_NearVar("8101040830ff", {new visca_u4("p", 4),});

const PTZCmd VISCA_CAM_Focus_Auto(      "8101043802ff");
const PTZCmd VISCA_CAM_Focus_Manual(    "8101043803ff");
const PTZCmd VISCA_CAM_Focus_AutoManual("8101043810ff");
const PTZInq VISCA_CAM_Focus_AFEnabledInq(    "81090438ff", {new visca_flag("focus_af_enabled", 2)});

const PTZCmd VISCA_CAM_Focus_OneTouch("8101041801ff");
const PTZCmd VISCA_CAM_Focus_Infinity("8101041802ff");

const PTZCmd VISCA_CAM_FocusPos(   "8101044800000000ff", {new visca_s16("focus_pos", 4),});
const PTZInq VISCA_CAM_FocusPosInq("81090448ff", {new visca_s16("focus_pos", 2)});

const PTZCmd VISCA_CAM_Focus_NearLimit(  "8101042800000000ff", {new visca_s16("focus_nearlimit", 4)});
const PTZInq VISCA_CAM_FocusNearLimitInq("81090428ff", {new visca_s16("focus_near_limit", 2)});

const PTZCmd VISCA_CAM_ZoomFocus_Direct("810104470000000000000000ff",
		{new visca_s16("zoom_pos", 4), new visca_s16("focus_pos", 8)});

const PTZCmd VISCA_CAM_AF_SensitivityNormal("8101045802ff");
const PTZCmd VISCA_CAM_AF_SensitivityLow(   "8101045803ff");
const PTZInq VISCA_CAM_AFSensitivityInq(    "81090458ff", {new visca_flag("focus_af_sensitivity", 2)});

const PTZCmd VISCA_CAM_AFMode_Normal(     "8101045700ff");
const PTZCmd VISCA_CAM_AFMode_Interval(   "8101045701ff");
const PTZCmd VISCA_CAM_AFMode_ZoomTrigger("8101045702ff");
const PTZInq VISCA_CAM_AFModeInq( "81090457ff", {new visca_flag("focus_af_mode", 2)});

const PTZCmd VISCA_CAM_AFMode_ActiveIntervalTime("8101042700000000ff", {
		new visca_u8("focus_af_move_time", 4),
		new visca_u8("focus_af_move_interval", 6)
	});
const PTZInq VISCA_CAM_AFTimeSettingInq("81090427ff", {
		new visca_u8("focus_af_move_time", 2),
		new visca_u8("focus_af_move_interval", 4)
	});

const PTZCmd VISCA_CAM_IRCorrection_Standard("8101041100ff");
const PTZCmd VISCA_CAM_IRCorrection_IRLight( "8101041101ff");
const PTZInq VISCA_CAM_IRCorrectionInq(      "81090411ff", {
		new visca_flag("ircorrection", 2)
	});

const PTZCmd VISCA_CAM_WB_Auto(  "8101043500ff");
const PTZCmd VISCA_CAM_WB_Indoor("8101043501ff");
const PTZCmd VISCA_CAM_WB_Outdoor("8101043502ff");
const PTZCmd VISCA_CAM_WB_OnePush("8101043503ff");
const PTZCmd VISCA_CAM_WB_AutoTracing(   "8101043504ff");
const PTZCmd VISCA_CAM_WB_Manual("8101043505ff");
const PTZInq VISCA_CAM_WBModeInq("81090435ff", {new visca_u4("wbmode", 2)});

const PTZCmd VISCA_CAM_WB_OnePushTrigger("8101041005ff");

const PTZCmd VISCA_CAM_RGain_Reset( "8101040300ff");
const PTZCmd VISCA_CAM_RGain_Up(    "8101040302ff");
const PTZCmd VISCA_CAM_RGain_Down(  "8101040303ff");
const PTZCmd VISCA_CAM_RGain_Direct("8101044300000000ff", {new visca_u8("rgain", 6)});
const PTZInq VISCA_CAM_RGainInq(    "81090443ff", {new visca_u8("rgain", 4)});

const PTZCmd VISCA_CAM_BGain_Reset( "8101040400ff");
const PTZCmd VISCA_CAM_BGain_Up(    "8101040402ff");
const PTZCmd VISCA_CAM_BGain_Down(  "8101040403ff");
const PTZCmd VISCA_CAM_BGain_Direct("8101044400000000ff", {new visca_u8("bgain", 6)});
const PTZInq VISCA_CAM_BGainInq(    "81090444ff", {new visca_u8("bgain", 4)});

const PTZCmd VISCA_CAM_AutoExposure_Auto(   "8101043900ff");
const PTZCmd VISCA_CAM_AutoExposure_Manual( "8101043903ff");
const PTZCmd VISCA_CAM_AutoExposure_ShutterPriority("810104390aff");
const PTZCmd VISCA_CAM_AutoExposure_IrisPriority(   "810104390bff");
const PTZCmd VISCA_CAM_AutoExposure_Bright( "810104390dff");
const PTZInq VISCA_CAM_AutoExposureModeInq( "81090439ff", {new visca_u4("aemode", 2)});

const PTZCmd VISCA_CAM_SlowShutter_Auto(  "8101045a02ff");
const PTZCmd VISCA_CAM_SlowShutter_Manual("8101045a03ff");
const PTZInq VISCA_CAM_SlowShutterModeInq("8109045aff", {new visca_u4("slowshuttermode", 2)});

const PTZCmd VISCA_CAM_Shutter_Reset( "8101040a00ff");
const PTZCmd VISCA_CAM_Shutter_Up(    "8101040a02ff");
const PTZCmd VISCA_CAM_Shutter_Down(  "8101040a03ff");
const PTZCmd VISCA_CAM_Shutter_Direct("8101044a00000000ff", {new visca_u8("shutter", 6)});
const PTZInq VISCA_CAM_ShutterPosInq( "8109044aff", {new visca_u8("shutter_pos", 4)});

const PTZCmd VISCA_CAM_Iris_Reset( "8101040b00ff");
const PTZCmd VISCA_CAM_Iris_Up(    "8101040b02ff");
const PTZCmd VISCA_CAM_Iris_Down(  "8101040b03ff");
const PTZCmd VISCA_CAM_Iris_Direct("8101044b00000000ff", {new visca_u8("iris", 6)});
const PTZInq VISCA_CAM_IrisPosInq( "8109044bff", {new visca_u8("iris_pos", 4)});

const PTZCmd VISCA_CAM_Gain_Reset( "8101040c00ff");
const PTZCmd VISCA_CAM_Gain_Up(    "8101040c02ff");
const PTZCmd VISCA_CAM_Gain_Down(  "8101040c03ff");
const PTZCmd VISCA_CAM_Gain_Direct("8101044c00000000ff", {new visca_u8("gain", 6)});
const PTZInq VISCA_CAM_GainPosInq( "8109044cff", {new visca_u8("gain_pos", 4)});

const PTZCmd VISCA_CAM_Gain_Limit( "8101042c00ff", {new visca_u4("ae_gain_limit", 4)});
const PTZInq VISCA_CAM_GainLimitInq("8109042cff", {new visca_u4("gain_limit", 2)});

const PTZCmd VISCA_CAM_Bright_Up(    "8101040d02ff");
const PTZCmd VISCA_CAM_Bright_Down(  "8101040d03ff");
const PTZCmd VISCA_CAM_Bright_Direct("8101044d00000000ff", {new visca_u8("bright", 6)});
const PTZInq VISCA_CAM_BrightPosInq("8109044dff", {new visca_u8("bright_pos", 4)});

const PTZCmd VISCA_CAM_ExpComp_On(    "8101043e02ff");
const PTZCmd VISCA_CAM_ExpComp_Off(   "8101043e03ff");
const PTZInq VISCA_CAM_ExpCompModeInq("8109043eff", {new visca_u4("expcomp_mode", 2)});

const PTZCmd VISCA_CAM_ExpComp_Reset( "8101040e00ff");
const PTZCmd VISCA_CAM_ExpComp_Up(    "8101040e02ff");
const PTZCmd VISCA_CAM_ExpComp_Down(  "8101040e03ff");
const PTZCmd VISCA_CAM_ExpComp_Direct("8101044e00000000ff", {new visca_u8("expcomp_pos", 6)});
const PTZInq VISCA_CAM_ExpCompPosInq( "8109044eff", {new visca_u8("expcomp_pos", 4)});

const PTZCmd VISCA_CAM_Backlight_On( "8101043302ff");
const PTZCmd VISCA_CAM_Backlight_Off("8101043303ff");
const PTZInq VISCA_CAM_BacklightInq( "81090433ff", {new visca_u4("backlight", 2)});

const PTZCmd VISCA_CAM_WD_Off( "81017e040000ff");
const PTZCmd VISCA_CAM_WD_Low( "81017e040001ff");
const PTZCmd VISCA_CAM_WD_Mid( "81017e040002ff");
const PTZCmd VISCA_CAM_WD_High("81017e040003ff");
const PTZInq VISCA_CAM_WDInq(  "81097e0400ff", {new visca_u4("wd", 2)});

const PTZCmd VISCA_CAM_Defog_On( "810104370200ff");
const PTZCmd VISCA_CAM_Defog_Off("810104370300ff");
const PTZInq VISCA_CAM_DefogInq( "81090437ff", {new visca_u4("defog", 2)});

const PTZCmd VISCA_CAM_Apature_Reset( "8101040200ff");
const PTZCmd VISCA_CAM_Apature_Up(    "8101040202ff");
const PTZCmd VISCA_CAM_Apature_Down(  "8101040203ff");
const PTZCmd VISCA_CAM_Apature_Direct("8101044200000000ff", {new visca_u8("apature_gain", 6)});
const PTZInq VISCA_CAM_ApatureInq(    "81090442ff", {new visca_u8("apature_gain", 4)});

const PTZCmd VISCA_CAM_HR_On( "8101045202ff");
const PTZCmd VISCA_CAM_HR_Off("8101045203ff");
const PTZInq VISCA_CAM_HRInq( "81090452ff", {new visca_u4("hr", 2)});

const PTZCmd VISCA_CAM_NR(   "8101045300ff", {new visca_u4("nr_level", 4)});
const PTZInq VISCA_CAM_NRInq("81090453ff", {new visca_u4("nr_level", 2)});

const PTZCmd VISCA_CAM_Gamma(   "8101045b00ff", {new visca_u4("gamma", 4)});
const PTZInq VISCA_CAM_GammaInq("8109045bff", {new visca_u4("gamma", 2)});

const PTZCmd VISCA_CAM_HighSensitivity_On( "8101045e02ff");
const PTZCmd VISCA_CAM_HighSensitivity_Off("8101045e03ff");
const PTZInq VISCA_CAM_HighSensitivityInq( "8109045eff", {new visca_u4("high_sensitivity", 2)});

const PTZCmd VISCA_CAM_PictureEffect_Off(   "8101046300ff");
const PTZCmd VISCA_CAM_PictureEffect_NegArt("8101046302ff");
const PTZCmd VISCA_CAM_PictureEffect_BW(    "8101046304ff");
const PTZInq VISCA_CAM_PictureEffectInq(    "81090463ff", {new visca_u4("picture_effect", 2)});

const PTZCmd VISCA_CAM_Memory_Reset ("8101043f0000ff", {new visca_u4("preset_num", 5)});
const PTZCmd VISCA_CAM_Memory_Set   ("8101043f0100ff", {new visca_u4("preset_num", 5)});
const PTZCmd VISCA_CAM_Memory_Recall("8101043f0200ff", {new visca_u4("preset_num", 5)});

const PTZCmd VISCA_CAM_IDWrite ("8101042200000000ff", {new visca_u16("camera_id", 4),});
const PTZInq VISCA_CAM_IDInq(   "81090422ff", {new visca_u16("camera_id", 2)});

const PTZCmd VISCA_CAM_ChromaSuppress(   "8101045f00ff", {new visca_u4("chroma_suppress", 4)});
const PTZInq VISCA_CAM_ChromaSuppressInq("8109045fff", {new visca_u4("chroma_suppress", 2)});

const PTZCmd VISCA_CAM_ColorGain(   "8101044900000000ff", {
		new visca_u4("color_spec", 6),
		new visca_u4("color_gain", 7)
	});
const PTZInq VISCA_CAM_ColorGainInq("81090449ff", {new visca_u4("color_gain", 4)});

const PTZCmd VISCA_CAM_ColorHue(   "8101044f00000000ff", {
		new visca_u4("hue_spec", 6),
		new visca_u4("hue_phase", 7)
	});
const PTZInq VISCA_CAM_ColorHueInq("8109044fff", {new visca_u4("hue_phase", 4)});

const PTZCmd VISCA_CAM_LowLatency_On( "81017e015a02ff");
const PTZCmd VISCA_CAM_LowLatency_Off("81017e015a03ff");
const PTZInq VISCA_CAM_LowLatencyInq( "81097e015aff", {new visca_flag("lowlatency", 2)});

const PTZCmd VISCA_SYSMenu_Off("8101060603ff");
const PTZInq VISCA_SYSMenuInq( "81010606ff", {new visca_flag("menumode", 2)});

const PTZCmd VISCA_CAM_InfoDisplay_On( "81017e011802ff");
const PTZCmd VISCA_CAM_InfoDisplay_Off("81017e011803ff");
const PTZInq VISCA_CAM_InfoDisplayInq( "81097e0118ff", {new visca_flag("info_display", 2)});

const PTZCmd VISCA_VideoFormat_set("81017e011e0000ff", {new visca_u8("video_format", 5)});
const PTZInq VISCA_VideoFormatInq( "81090623ff", {new visca_u4("video_format", 2)});

const PTZCmd VISCA_ColorSystem_set("81017e01030000ff", {new visca_u4("color_format", 6)});
const PTZInq VISCA_ColorSystemInq( "81097e0103ff", {new visca_u4("color_format", 2)});

const PTZCmd VISCA_IRReceive_On(    "8101060802ff");
const PTZCmd VISCA_IRReceive_Off(   "8101060803ff");
const PTZCmd VISCA_IRReceive_Toggle("8101060810ff");
const PTZInq VISCA_IRReceiveInq(    "81090608ff", {new visca_flag("irreceive", 2)});

const PTZCmd VISCA_IRReceiveReturn_On( "81017d01030000ff");
const PTZCmd VISCA_IRReceiveReturn_Off("81017d01130000ff");

const PTZInq VISCA_IRConditionInq("81090634ff", {new visca_u4("ircondition", 2)});

const PTZInq VISCA_PanTilt_MaxSpeedInq("81090611ff", {
		new visca_u7("panmaxspeed", 2), new visca_u7("tiltmaxspeed", 3)});

const PTZCmd VISCA_PanTilt_drive(    "8101060100000303ff", {
		new visca_s7("pan", 4),
		new visca_s7("tilt", 5)
	});
const PTZCmd VISCA_PanTilt_drive_abs("8101060200000000000000000000ff", {
		new visca_u7("panspeed", 4), new visca_u7("tiltspeed", 5),
		new visca_s16("pan_pos", 6), new visca_s16("tilt_pos", 10)
	});
const PTZCmd VISCA_PanTilt_drive_rel("8101060300000000000000000000ff", {
		new visca_u7("panspeed", 4), new visca_u7("tiltspeed", 5),
		new visca_s16("pan_pos", 6), new visca_s16("tilt_pos", 10)
	});
const PTZCmd VISCA_PanTilt_Home(     "81010604ff");
const PTZCmd VISCA_PanTilt_Reset(    "81010605ff");
const PTZInq VISCA_PanTilt_PosInq(   "81090612ff", {
		new visca_s16("pan_pos", 2),
		new visca_s16("tilt_pos", 6)
	});

const PTZCmd VISCA_PanTilt_LimitSetUpRight( "8101060700010000000000000000ff", {
		new visca_u16("pan_limit_right", 6),
		new visca_u16("tilt_limit_up", 10)
	});
const PTZCmd VISCA_PanTilt_LimitSetDownLeft("8101060700000000000000000000ff", {
		new visca_u16("pan_limit_left", 6),
		new visca_u16("tilt_limit_down", 10)
	});
const PTZCmd VISCA_PanTilt_LimitClearUpRight( "810106070101070f0f0f070f0f0fff", {
		new visca_u16("pan_limit_right", 6),
		new visca_u16("tilt_limit_up", 10)
	});
const PTZCmd VISCA_PanTilt_LimitClearDownLeft("810106070100070f0f0f070f0f0fff", {
		new visca_u16("pan_limit_left", 6),
		new visca_u16("tilt_limit_down", 10)
	});

#define VISCA_RESPONSE_ADDRESS   0x30
#define VISCA_RESPONSE_ACK	 0x40
#define VISCA_RESPONSE_COMPLETED 0x50
#define VISCA_RESPONSE_ERROR     0x60
#define VISCA_PACKET_SENDER(pkt) ((unsigned)((pkt)[0] & 0x70) >> 4)

const QMap<int, std::string> PTZVisca::viscaVendors = {
	{ 0x0001, "Sony" },
	{ 0x0109, "Birddog" },
};

/* lookup in this table is: (Vendor ID << 16) | Model ID */
const QMap<int, std::string> PTZVisca::viscaModels = {
	/* Sony Cameras */
	{ 0x0001040f, "BRC-300" },
	{ 0x00010511, "SRG-120DH" },
	/* Birddog */
	{ 0x01092020, "P100" },
};

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

void PTZVisca::send(PTZCmd cmd)
{
	if (cmd.cmd[1] == (char)0x01) { // command packets get sent immediately
		send_immediate(cmd.cmd);
	} else {
		pending_cmds.append(cmd);
		send_pending();
	}
}

void PTZVisca::send(PTZCmd cmd, QList<int> args)
{
	cmd.encode(args);
	send(cmd);
}

void PTZVisca::timeout()
{
	ptz_debug("VISCA %s timeout", qPrintable(objectName()));
	active_cmd[0] = false;
	if (!pending_cmds.isEmpty())
		pending_cmds.removeFirst();
	send_pending();
}

void PTZVisca::cmd_get_camera_info()
{
	send(VISCA_CAM_VersionInq);
	send(VISCA_CAM_VersionInq); // hack: first inquiry doesn't always work, send twice
	send(VISCA_PanTilt_PosInq);
	send(VISCA_LensControlInq);
	send(VISCA_CameraControlInq);
	send(VISCA_OtherInq);
	send(VISCA_EnlargementFunction1Inq);
	send(VISCA_EnlargementFunction2Inq);
	send(VISCA_EnlargementFunction3Inq);
}

void PTZVisca::receive(const QByteArray &msg)
{
	if (VISCA_PACKET_SENDER(msg) != address || (msg.size() < 3))
		return;
	int slot = msg[1] & 0x7;

	switch (msg[1] & 0xf0) {
	case VISCA_RESPONSE_ACK:
		active_cmd[slot] = true;
		break;
	case VISCA_RESPONSE_COMPLETED:
		if (msg.size() == 3 && slot == 0) {
			/* Some devices *cough*cicso*cough* don't use slots and commands
			 * complete immediately. If the response is empty, then assume
			 * it was the result of a command not an enquiry */
			break;
		}
		if (!active_cmd[slot]) {
			ptz_debug("VISCA %s spurious reply: %s", qPrintable(objectName()), msg.toHex(':').data());
			break;
		}
		active_cmd[slot] = false;

		/* Slot 0 responses are inquiries that need to be parsed */
		if (slot == 0) {
			timeout_timer.stop();
			obs_data_t *props = pending_cmds.first().decode(msg);
			obs_data_apply(settings, props);
			emit settingsChanged(props);
			obs_data_release(props);
			pending_cmds.removeFirst();
		}

		break;
	case VISCA_RESPONSE_ERROR:
		active_cmd[slot] = false;
		if ((slot == 0) && (msg[2] != 3) && (msg[2] != 4) && (msg[2] != 5)) {
			timeout_timer.stop();
			if (!pending_cmds.isEmpty())
				pending_cmds.removeFirst();
		}
		ptz_debug("VISCA %s received error: %s", qPrintable(objectName()), msg.toHex(':').data());
		break;
	default:
		ptz_debug("VISCA %s received unknown: %s", qPrintable(objectName()), msg.toHex(':').data());
		break;
	}
	send_pending();
}

void PTZVisca::send_pending()
{
	if (active_cmd[0] || pending_cmds.isEmpty())
		return;
	active_cmd[0] = true;
	send_immediate(pending_cmds.first().cmd);
	timeout_timer.setSingleShot(true);
	timeout_timer.start(2000);
}

void PTZVisca::pantilt(double pan_, double tilt_)
{
	int pan = (pan_ > 1 ? 1 : (pan_ < -1 ? -1 : pan_)) * 0x18;
	int tilt = (tilt_ > 1 ? 1 : (tilt_ < -1 ? -1 : tilt_)) * 0x14;
	send(VISCA_PanTilt_drive, {pan, -tilt});
}

void PTZVisca::pantilt_rel(int pan, int tilt)
{
	send(VISCA_PanTilt_drive_rel, {0x14, 0x14, pan, -tilt});
}

void PTZVisca::pantilt_abs(int pan, int tilt)
{
	send(VISCA_PanTilt_drive_abs, {0x0f, 0x0f, pan, tilt});
}

void PTZVisca::pantilt_stop()
{
	send(VISCA_PanTilt_drive, {0, 0});
}

void PTZVisca::pantilt_home()
{
	send(VISCA_PanTilt_Home);
}

void PTZVisca::zoom_tele(double speed_)
{
	int speed = (speed_ > 1 ? 1 : (speed_ < 0 ? 0 : speed_)) * 0x7;
	send(VISCA_CAM_Zoom_TeleVar, { speed });
}

void PTZVisca::zoom_wide(double speed_)
{
	int speed = (speed_ > 1 ? 1 : (speed_ < 0 ? 0 : speed_)) * 0x7;
	send(VISCA_CAM_Zoom_WideVar, { speed });
}

void PTZVisca::zoom_abs(int pos)
{
	send(VISCA_CAM_Zoom_Direct, { pos });
}

void PTZVisca::set_autofocus(bool enabled)
{
	send(enabled ? VISCA_CAM_Focus_Auto : VISCA_CAM_Focus_Manual);
	obs_data_set_bool(settings, "focus_af_enabled", enabled);
}

void PTZVisca::focus_stop()
{
	send(VISCA_CAM_Focus_Stop);
}

void PTZVisca::focus_near(double speed_)
{
	// The following two lines allows the focus speed to be adjusted using
	// the speed slide, but in practical terms this makes the focus change
	// far too quickly. Just use the slowest speed instead.
	//int speed = (speed_ > 1 ? 1 : (speed_ < 0 ? 0 : speed_)) * 0x7;
	//send(VISCA_CAM_Focus_NearVar, { speed });

	send(VISCA_CAM_Focus_NearVar, { 1 });
}

void PTZVisca::focus_far(double speed_)
{
	//int speed = (speed_ > 1 ? 1 : (speed_ < 0 ? 0 : speed_)) * 0x7;
	//send(VISCA_CAM_Focus_FarVar, { speed });

	send(VISCA_CAM_Focus_FarVar, { 1 });
}

void PTZVisca::focus_onetouch()
{
	send(VISCA_CAM_Focus_OneTouch);
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
ViscaUART::ViscaUART(QString &port_name) :
	PTZUARTWrapper(port_name)
{
	camera_count = 0;
}

bool ViscaUART::open()
{
	camera_count = 0;
	bool rc = PTZUARTWrapper::open();
	if (rc)
		send(VISCA_ENUMERATE.cmd);
	return rc;
}

void ViscaUART::close()
{
	PTZUARTWrapper::close();
	camera_count = 0;
}

void ViscaUART::receive_datagram(const QByteArray &packet)
{
	ptz_debug("VISCA <-- %s", packet.toHex(':').data());
	if (packet.size() < 3)
		return;
	if ((packet[1] & 0xf0) == VISCA_RESPONSE_ADDRESS) {
		switch (packet[1] & 0x0f) { /* Decode Packet Socket Field */
		case 0:
			camera_count = (packet[2] & 0x7) - 1;
			blog(LOG_INFO, "VISCA Interface %s: %i camera%s found",
				qPrintable(uart.portName()),
				camera_count, camera_count == 1 ? "" : "s");
			send(VISCA_IF_CLEAR.cmd);
			emit reset();
			break;
		case 1:
			// Response from IF_CLEAR message; ignore
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

void ViscaUART::receiveBytes(const QByteArray &msg)
{
	for (auto b : msg) {
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
	ptz_debug("Looking for UART object %s", qPrintable(port_name));
	iface = interfaces[port_name];
	if (!iface) {
		ptz_debug("Creating new VISCA object %s", qPrintable(port_name));
		iface = new ViscaUART(port_name);
		iface->open();
		interfaces[port_name] = iface;
	}
	return iface;
}

PTZViscaSerial::PTZViscaSerial(OBSData config)
	: PTZVisca("visca"), iface(NULL)
{
	set_config(config);
	auto_settings_filter += {"port", "address", "baud_rate"};
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
	cmd_get_camera_info();
}

void PTZViscaSerial::send_immediate(QByteArray &msg)
{
	msg[0] = (char)(0x80 | address & 0x7); // Set the camera address
	iface->send(msg);
}

void PTZViscaSerial::set_config(OBSData config)
{
	PTZDevice::set_config(config);
	const char *uart = obs_data_get_string(config, "port");
	address = obs_data_get_int(config, "address");
	if (!uart)
		return;

	iface = ViscaUART::get_interface(uart);
	iface->setConfig(config);
	attach_interface(iface);
}

OBSData PTZViscaSerial::get_config()
{
	OBSData config = PTZDevice::get_config();
	obs_data_apply(config, iface->getConfig());
	obs_data_set_int(config, "address", address);
	return config;
}

obs_properties_t *PTZViscaSerial::get_obs_properties()
{
	obs_properties_t *props = PTZDevice::get_obs_properties();
	obs_property_t *p = obs_properties_get(props, "interface");
	obs_properties_t *config = obs_property_group_content(p);
	obs_property_set_description(p, "VISCA Connection");

	iface->addOBSProperties(config);
	obs_properties_add_int(config, "address", "VISCA ID", 1, 7, 1);

	return props;
}

/*
 * VISCA over IP implementation
 */
ViscaUDPSocket::ViscaUDPSocket(int port) :
	visca_port(port)
{
	if (!visca_socket.bind(QHostAddress::Any, visca_port)) {
		blog(LOG_INFO, "VISCA-over-IP bind to port %i failed", visca_port);
		return;
	}
	connect(&visca_socket, &QUdpSocket::readyRead, this, &ViscaUDPSocket::poll);
}

void ViscaUDPSocket::receive_datagram(QNetworkDatagram &dg)
{
	QByteArray data = dg.data();
	int type = ((data[0] & 0xff) << 8) | (data[1] & 0xff);
	int size = data[2] << 8 | data[3];
	int seq = data[4] << 24 | data[5] << 16 | data[6] << 8 | data[7];

	if ((data.size() != size + 8) || size < 1) {
		ptz_debug("VISCA UDP (malformed) <-- %s", qPrintable(data.toHex(':')));
		return;
	}
	ptz_debug("VISCA UDP type=%.4x seq=%i size=%i <-- %s",
		type, seq, size, qPrintable(data.toHex(':')));

	switch (type) {
	case 0x0111:
		emit receive(data.mid(8, size));
		break;
	case 0x0200:
	case 0x0201: /* Check for sequence number out of sync */
		if (data[8] == (char)0x0f && data[8+1] == (char)1)
			emit reset();
		break;
	default:
		ptz_debug("VISCA UDP unrecognized type: %x", type);
	}

}

void ViscaUDPSocket::send(QHostAddress ip_address, const QByteArray &packet)
{
	ptz_debug("VISCA UDP --> %s", qPrintable(packet.toHex(':')));
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
	ptz_debug("Looking for Visca UDP Socket object %i", port);
	iface = interfaces[port];
	if (!iface) {
		ptz_debug("Creating new VISCA object %i", port);
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
	auto_settings_filter += {"port", "address"};
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

void PTZViscaOverIP::send_immediate(QByteArray &msg)
{
	QByteArray p = QByteArray::fromHex("0100000000000000") + msg;
	p[1] = (0x9 == msg[1]) ? 0x10 : 0x00;
	p[3] = msg.size();
	p[4] = (sequence >> 24) & 0xff;
	p[5] = (sequence >> 16) & 0xff;
	p[6] = (sequence >> 8) & 0xff;
	p[7] = sequence & 0xff;
	p[8] = '\x81';
	sequence++;
	iface->send(ip_address, p);
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

obs_properties_t *PTZViscaOverIP::get_obs_properties()
{
	obs_properties_t *props = PTZDevice::get_obs_properties();
	obs_property_t *p = obs_properties_get(props, "interface");
	obs_properties_t *config = obs_property_group_content(p);
	obs_property_set_description(p, "VISCA-over-IP Connection");
	obs_properties_add_text(config, "address", "IP Address", OBS_TEXT_DEFAULT);
	obs_properties_add_int(config, "port", "UDP port", 1, 65535, 1);
	return props;
}
