/* Pan Tilt Zoom visca instance
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include "imported/qt-wrappers.hpp"
#include <QNetworkDatagram>
#include "ptz-visca.hpp"
#include <util/base.h>

/* Visca specific datagram field classes */
class visca_u4 : public int_field {
public:
	visca_u4(const char *name, int offset) : int_field(name, offset, 0x0f)
	{
	}
};

/*
 * VISCA Signed 4-bit integer
 * The VISCA signed 4-bit encoding separates the direction and speed into
 * separate values. The speed value is encoded in the range 0x0-0x7, where '0'
 * means the slowest speed. It does not mean stop. Direction is encoded in the
 * same byte as '0x30' for negative movement, '0x20' for positive movement, and
 * '0x00' for stop. This helper encodes the speed value with 'abs(val)-1' so
 * that the slowest valid speed can be encoded. val==0 is encoded as 'stop'.
 */
class visca_s4 : public datagram_field {
public:
	visca_s4(const char *name, int offset) : datagram_field(name, offset) {}
	void encode(QByteArray &msg, int val)
	{
		if (msg.size() < offset)
			return;
		msg[offset] = val ? std::clamp(abs(val) - 1, 0, 0x7) |
					      (val > 0 ? 0x20 : 0x30)
				  : 0;
	}
	bool decode(OBSData data, QByteArray &msg)
	{
		if (msg.size() < offset)
			return false;
		int val = (msg[offset] & 0x07) + 1;
		switch (msg[offset] & 0xf0) {
		case 0x30:
			obs_data_set_int(data, name, -val);
			break;
		case 0x20:
			obs_data_set_int(data, name, val);
			break;
		case 0x00:
			obs_data_set_int(data, name, 0);
			break;
		default:
			return false;
		}
		return true;
	}
};

class visca_flag : public datagram_field {
public:
	visca_flag(const char *name, int offset) : datagram_field(name, offset)
	{
	}
	void encode(QByteArray &msg, int val)
	{
		if (msg.size() < offset + 1)
			return;
		msg[offset] = val ? 0x2 : 0x3;
	}
	bool decode(OBSData data, QByteArray &msg)
	{
		if (msg.size() < offset + 1)
			return false;
		switch (msg[offset]) {
		case 0x02:
			obs_data_set_bool(data, name, true);
			break;
		case 0x03:
			obs_data_set_bool(data, name, false);
			break;
		default:
			return false;
		}
		return true;
	}
};

class visca_u7 : public int_field {
public:
	visca_u7(const char *name, int offset) : int_field(name, offset, 0x7f)
	{
	}
};

/*
 * VISCA Signed 7-bit integer
 * The VISCA signed 7-bit encoding separates the direction and speed into
 * separate values. The speed value is encoded in the range 0x01-0x7f, where
 * '1' means the slowest speed. '0' isn't a valid speed. Direction is encoded in
 * a separate byte as '1' for negative movement, '2' for positive movement, and
 * '3' for stop.
 */
class visca_s7 : public datagram_field {
public:
	visca_s7(const char *name, int offset) : datagram_field(name, offset) {}
	void encode(QByteArray &msg, int val)
	{
		if (msg.size() < offset + 3)
			return;
		msg[offset] = std::clamp(abs(val), 0, 0x7f);
		msg[offset + 2] = val ? (val < 0 ? 1 : 2) : 3;
	}
	bool decode(OBSData data, QByteArray &msg)
	{
		if (msg.size() < offset + 3)
			return false;
		int val = (msg[offset] & 0x7f);
		switch (msg[offset + 2]) {
		case 0x01:
			obs_data_set_int(data, name, -val);
			break;
		case 0x02:
			obs_data_set_int(data, name, val);
			break;
		case 0x03:
			obs_data_set_int(data, name, 0);
			break;
		default:
			return false;
		}
		return true;
	}
};

class visca_u8 : public int_field {
public:
	visca_u8(const char *name, int offset) : int_field(name, offset, 0x0f0f)
	{
	}
};

/* 15 bit value encoded into two bytes. Protocol encoding forces bit 15 & 7 to zero */
class visca_u15 : public datagram_field {
public:
	visca_u15(const char *name, int offset) : datagram_field(name, offset)
	{
	}
	void encode(QByteArray &msg, int val)
	{
		if (msg.size() < offset + 2)
			return;
		msg[offset] = (val >> 8) & 0x7f;
		msg[offset + 1] = val & 0x7f;
	}
	bool decode(OBSData data, QByteArray &msg)
	{
		if (msg.size() < offset + 2)
			return false;
		uint16_t val = (msg[offset] & 0x7f) << 8 |
			       (msg[offset + 1] & 0x7f);
		obs_data_set_int(data, name, val);
		return true;
	}
};

class visca_s16 : public int_field {
public:
	visca_s16(const char *name, int offset)
		: int_field(name, offset, 0x0f0f0f0f, true)
	{
	}
};

class visca_u16 : public int_field {
public:
	visca_u16(const char *name, int offset)
		: int_field(name, offset, 0x0f0f0f0f)
	{
	}
};

const PTZCmd VISCA_ENUMERATE("883001ff");

const PTZInq VISCA_CAM_VersionInq(
	"81090002ff",
	{new int_field("vendor_id", 2, 0x7fff),
	 new int_field("model_id", 4, 0x7fff),
	 new string_lookup_field("vendor_name", PTZVisca::viscaVendors, 2,
				 0x7fff),
	 new string_lookup_field("model_name", PTZVisca::viscaModels, 2,
				 0x7fffffff),
	 new int_field("rom_version", 6, 0xffff),
	 new int_field("socket_number", 8, 0xff)});

const PTZInq VISCA_LensControlInq(
	"81097e7e00ff", {new int_field("zoom_pos", 2, 0x0f0f0f0f),
			 new int_field("focus_near_limit", 6, 0x0f0f0f0f),
			 new int_field("focus_pos", 8, 0x0f0f0f0f),
			 new int_field("focus_af_mode", 13, 0b00011000),
			 new bool_field("focus_af_sensitivity", 13, 0b0100),
			 new bool_field("dzoom", 13, 0b0010),
			 new bool_field("focus_af_enabled", 13, 0b0001),
			 new bool_field("low_contrast_mode", 14, 0b1000)});

const PTZInq VISCA_CameraControlInq(
	"81097e7e01ff",
	{new visca_u8("r_gain", 2), new visca_u8("b_gain", 4),
	 new visca_u4("wb_mode", 6), new visca_u4("aperature_gain", 7),
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
	 new int_field("exposure_comp_pos", 14, 0x0f)});

const PTZInq VISCA_OtherInq("81097e7e02ff",
			    {/*new bool_field("power_on", 2, 0b0001),*/
			     new int_field("picture_effect_mode", 5, 0x0f),
			     new int_field("camera_id", 8, 0x0f0f0f0f),
			     new int_field("framerate", 12, 0b0001)});

const PTZInq VISCA_EnlargementFunction1Inq(
	"81097e7e03ff",
	{
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

const PTZInq VISCA_EnlargementFunction2Inq("81097e7e04ff",
					   {new bool_field("defog_mode", 7,
							   0b0001)});

const PTZInq
	VISCA_EnlargementFunction3Inq("81097e7e05ff",
				      {new int_field("color_hue", 2, 0b1111)});

const PTZCmd VISCA_CommandCancel("8120ff", {new visca_u4("socket", 1)});
const PTZCmd VISCA_CAM_Power("8101040000ff", {new visca_flag("power_on", 4)},
			     "power_on");
const PTZInq VISCA_CAM_PowerInq("81090400ff", {new visca_flag("power_on", 2)});

const PTZCmd VISCA_CAM_Zoom_Stop("8101040700ff", "zoom_pos");
const PTZCmd VISCA_CAM_Zoom_Tele("8101040702ff", "zoom_pos");
const PTZCmd VISCA_CAM_Zoom_Wide("8101040703ff", "zoom_pos");
const PTZCmd VISCA_CAM_Zoom_drive("8101040700ff",
				  {
					  new visca_s4("zoom_speed", 4),
				  },
				  "zoom_pos");
const PTZCmd VISCA_CAM_Zoom_TeleVar("8101040720ff",
				    {
					    new visca_u4("zoom_speed", 4),
				    },
				    "zoom_pos");
const PTZCmd VISCA_CAM_Zoom_WideVar("8101040730ff",
				    {
					    new visca_u4("zoom_speed", 4),
				    },
				    "zoom_pos");
const PTZCmd VISCA_CAM_Zoom_Direct("8101044700000000ff",
				   {
					   new visca_s16("zoom_pos", 4),
				   });
const PTZInq VISCA_CAM_ZoomPosInq("81090447ff", {new visca_s16("zoom_pos", 2)});

const PTZCmd VISCA_CAM_DZoom_On("8101040602ff", "dzoom_on");
const PTZCmd VISCA_CAM_DZoom_Off("8101040603ff", "dzoom_on");
const PTZInq VISCA_CAM_DZoomModeInq("81090406ff",
				    {new visca_flag("dzoom_on", 2)});

const PTZCmd VISCA_CAM_Focus_Stop("8101040800ff", "focus_pos");
const PTZCmd VISCA_CAM_Focus_Far("8101040802ff", "focus_pos");
const PTZCmd VISCA_CAM_Focus_Near("8101040803ff", "focus_pos");
const PTZCmd VISCA_CAM_Focus_drive("8101040800ff",
				   {
					   new visca_s4("focus_speed", 4),
				   },
				   "focus_pos");
const PTZCmd VISCA_CAM_Focus_FarVar("8101040820ff",
				    {
					    new visca_u4("focus_speed", 4),
				    },
				    "focus_pos");
const PTZCmd VISCA_CAM_Focus_NearVar("8101040830ff",
				     {
					     new visca_u4("focus_speed", 4),
				     },
				     "focus_pos");

const PTZCmd VISCA_CAM_Focus_Auto("8101043802ff");
const PTZCmd VISCA_CAM_Focus_Manual("8101043803ff");
const PTZCmd VISCA_CAM_Focus_AutoManual("8101043810ff");
const PTZInq
	VISCA_CAM_Focus_AFEnabledInq("81090438ff",
				     {new visca_flag("focus_af_enabled", 2)});

const PTZCmd VISCA_CAM_Focus_OneTouch("8101041801ff");
const PTZCmd VISCA_CAM_Focus_Infinity("8101041802ff");

const PTZCmd VISCA_CAM_FocusPos("8101044800000000ff",
				{
					new visca_s16("focus_pos", 4),
				},
				"focus_pos");
const PTZInq VISCA_CAM_FocusPosInq("81090448ff",
				   {new visca_s16("focus_pos", 2)});

const PTZCmd VISCA_CAM_Focus_NearLimit("8101042800000000ff",
				       {new visca_s16("focus_nearlimit", 4)});
const PTZInq
	VISCA_CAM_FocusNearLimitInq("81090428ff",
				    {new visca_s16("focus_near_limit", 2)});

const PTZCmd VISCA_CAM_ZoomFocus_Direct("810104470000000000000000ff",
					{new visca_s16("zoom_pos", 4),
					 new visca_s16("focus_pos", 8)});

const PTZCmd VISCA_CAM_AF_SensitivityNormal("8101045802ff");
const PTZCmd VISCA_CAM_AF_SensitivityLow("8101045803ff");
const PTZInq
	VISCA_CAM_AFSensitivityInq("81090458ff",
				   {new visca_flag("focus_af_sensitivity", 2)});

const PTZCmd VISCA_CAM_AFMode_Normal("8101045700ff");
const PTZCmd VISCA_CAM_AFMode_Interval("8101045701ff");
const PTZCmd VISCA_CAM_AFMode_ZoomTrigger("8101045702ff");
const PTZInq VISCA_CAM_AFModeInq("81090457ff",
				 {new visca_flag("focus_af_mode", 2)});

const PTZCmd VISCA_CAM_AFMode_ActiveIntervalTime(
	"8101042700000000ff", {new visca_u8("focus_af_move_time", 4),
			       new visca_u8("focus_af_move_interval", 6)});
const PTZInq
	VISCA_CAM_AFTimeSettingInq("81090427ff",
				   {new visca_u8("focus_af_move_time", 2),
				    new visca_u8("focus_af_move_interval", 4)});

const PTZCmd VISCA_CAM_IRCorrection_Standard("8101041100ff");
const PTZCmd VISCA_CAM_IRCorrection_IRLight("8101041101ff");
const PTZInq VISCA_CAM_IRCorrectionInq("81090411ff",
				       {new visca_flag("ircorrection", 2)});

const PTZCmd VISCA_CAM_WB_Mode("8101043500ff", {new visca_u4("wb_mode", 4)},
			       "wb_mode");
const PTZCmd VISCA_CAM_WB_Auto("8101043500ff");
const PTZCmd VISCA_CAM_WB_Indoor("8101043501ff");
const PTZCmd VISCA_CAM_WB_Outdoor("8101043502ff");
const PTZCmd VISCA_CAM_WB_OnePush("8101043503ff");
const PTZCmd VISCA_CAM_WB_AutoTracing("8101043504ff");
const PTZCmd VISCA_CAM_WB_Manual("8101043505ff");
const PTZInq VISCA_CAM_WBModeInq("81090435ff", {new visca_u4("wb_mode", 2)});

const PTZCmd VISCA_CAM_WB_OnePushTrigger("8101041005ff");

const PTZCmd VISCA_CAM_RGain_Reset("8101040300ff");
const PTZCmd VISCA_CAM_RGain_Up("8101040302ff");
const PTZCmd VISCA_CAM_RGain_Down("8101040303ff");
const PTZCmd VISCA_CAM_RGain_Direct("8101044300000000ff",
				    {new visca_u8("rgain", 6)});
const PTZInq VISCA_CAM_RGainInq("81090443ff", {new visca_u8("rgain", 4)});

const PTZCmd VISCA_CAM_BGain_Reset("8101040400ff");
const PTZCmd VISCA_CAM_BGain_Up("8101040402ff");
const PTZCmd VISCA_CAM_BGain_Down("8101040403ff");
const PTZCmd VISCA_CAM_BGain_Direct("8101044400000000ff",
				    {new visca_u8("bgain", 6)});
const PTZInq VISCA_CAM_BGainInq("81090444ff", {new visca_u8("bgain", 4)});

const PTZCmd VISCA_CAM_AutoExposure_Auto("8101043900ff");
const PTZCmd VISCA_CAM_AutoExposure_Manual("8101043903ff");
const PTZCmd VISCA_CAM_AutoExposure_ShutterPriority("810104390aff");
const PTZCmd VISCA_CAM_AutoExposure_IrisPriority("810104390bff");
const PTZCmd VISCA_CAM_AutoExposure_Bright("810104390dff");
const PTZInq VISCA_CAM_AutoExposureModeInq("81090439ff",
					   {new visca_u4("aemode", 2)});

const PTZCmd VISCA_CAM_SlowShutter_Auto("8101045a02ff");
const PTZCmd VISCA_CAM_SlowShutter_Manual("8101045a03ff");
const PTZInq VISCA_CAM_SlowShutterModeInq("8109045aff",
					  {new visca_u4("slowshuttermode", 2)});

const PTZCmd VISCA_CAM_Shutter_Reset("8101040a00ff");
const PTZCmd VISCA_CAM_Shutter_Up("8101040a02ff");
const PTZCmd VISCA_CAM_Shutter_Down("8101040a03ff");
const PTZCmd VISCA_CAM_Shutter_Direct("8101044a00000000ff",
				      {new visca_u8("shutter", 6)});
const PTZInq VISCA_CAM_ShutterPosInq("8109044aff",
				     {new visca_u8("shutter_pos", 4)});

const PTZCmd VISCA_CAM_Iris_Reset("8101040b00ff");
const PTZCmd VISCA_CAM_Iris_Up("8101040b02ff");
const PTZCmd VISCA_CAM_Iris_Down("8101040b03ff");
const PTZCmd VISCA_CAM_Iris_Direct("8101044b00000000ff",
				   {new visca_u8("iris", 6)});
const PTZInq VISCA_CAM_IrisPosInq("8109044bff", {new visca_u8("iris_pos", 4)});

const PTZCmd VISCA_CAM_Gain_Reset("8101040c00ff");
const PTZCmd VISCA_CAM_Gain_Up("8101040c02ff");
const PTZCmd VISCA_CAM_Gain_Down("8101040c03ff");
const PTZCmd VISCA_CAM_Gain_Direct("8101044c00000000ff",
				   {new visca_u8("gain", 6)});
const PTZInq VISCA_CAM_GainPosInq("8109044cff", {new visca_u8("gain_pos", 4)});

const PTZCmd VISCA_CAM_Gain_Limit("8101042c00ff",
				  {new visca_u4("ae_gain_limit", 4)});
const PTZInq VISCA_CAM_GainLimitInq("8109042cff",
				    {new visca_u4("gain_limit", 2)});

const PTZCmd VISCA_CAM_Bright_Up("8101040d02ff");
const PTZCmd VISCA_CAM_Bright_Down("8101040d03ff");
const PTZCmd VISCA_CAM_Bright_Direct("8101044d00000000ff",
				     {new visca_u8("bright", 6)});
const PTZInq VISCA_CAM_BrightPosInq("8109044dff",
				    {new visca_u8("bright_pos", 4)});

const PTZCmd VISCA_CAM_ExpComp_On("8101043e02ff");
const PTZCmd VISCA_CAM_ExpComp_Off("8101043e03ff");
const PTZInq VISCA_CAM_ExpCompModeInq("8109043eff",
				      {new visca_u4("expcomp_mode", 2)});

const PTZCmd VISCA_CAM_ExpComp_Reset("8101040e00ff");
const PTZCmd VISCA_CAM_ExpComp_Up("8101040e02ff");
const PTZCmd VISCA_CAM_ExpComp_Down("8101040e03ff");
const PTZCmd VISCA_CAM_ExpComp_Direct("8101044e00000000ff",
				      {new visca_u8("expcomp_pos", 6)});
const PTZInq VISCA_CAM_ExpCompPosInq("8109044eff",
				     {new visca_u8("expcomp_pos", 4)});

const PTZCmd VISCA_CAM_Backlight_On("8101043302ff");
const PTZCmd VISCA_CAM_Backlight_Off("8101043303ff");
const PTZInq VISCA_CAM_BacklightInq("81090433ff",
				    {new visca_u4("backlight", 2)});

const PTZCmd VISCA_CAM_WD_Off("81017e040000ff");
const PTZCmd VISCA_CAM_WD_Low("81017e040001ff");
const PTZCmd VISCA_CAM_WD_Mid("81017e040002ff");
const PTZCmd VISCA_CAM_WD_High("81017e040003ff");
const PTZInq VISCA_CAM_WDInq("81097e0400ff", {new visca_u4("wd", 2)});

const PTZCmd VISCA_CAM_Defog_On("810104370200ff");
const PTZCmd VISCA_CAM_Defog_Off("810104370300ff");
const PTZInq VISCA_CAM_DefogInq("81090437ff", {new visca_u4("defog", 2)});

const PTZCmd VISCA_CAM_Apature_Reset("8101040200ff");
const PTZCmd VISCA_CAM_Apature_Up("8101040202ff");
const PTZCmd VISCA_CAM_Apature_Down("8101040203ff");
const PTZCmd VISCA_CAM_Apature_Direct("8101044200000000ff",
				      {new visca_u8("apature_gain", 6)});
const PTZInq VISCA_CAM_ApatureInq("81090442ff",
				  {new visca_u8("apature_gain", 4)});

const PTZCmd VISCA_CAM_HR_On("8101045202ff");
const PTZCmd VISCA_CAM_HR_Off("8101045203ff");
const PTZInq VISCA_CAM_HRInq("81090452ff", {new visca_u4("hr", 2)});

const PTZCmd VISCA_CAM_NR("8101045300ff", {new visca_u4("nr_level", 4)});
const PTZInq VISCA_CAM_NRInq("81090453ff", {new visca_u4("nr_level", 2)});

const PTZCmd VISCA_CAM_Gamma("8101045b00ff", {new visca_u4("gamma", 4)});
const PTZInq VISCA_CAM_GammaInq("8109045bff", {new visca_u4("gamma", 2)});

const PTZCmd VISCA_CAM_HighSensitivity_On("8101045e02ff");
const PTZCmd VISCA_CAM_HighSensitivity_Off("8101045e03ff");
const PTZInq
	VISCA_CAM_HighSensitivityInq("8109045eff",
				     {new visca_u4("high_sensitivity", 2)});

const PTZCmd VISCA_CAM_PictureEffect_Off("8101046300ff");
const PTZCmd VISCA_CAM_PictureEffect_NegArt("8101046302ff");
const PTZCmd VISCA_CAM_PictureEffect_BW("8101046304ff");
const PTZInq VISCA_CAM_PictureEffectInq("81090463ff",
					{new visca_u4("picture_effect", 2)});

const PTZCmd VISCA_CAM_Memory_Reset("8101043f0000ff",
				    {new visca_u7("preset_num", 5)});
const PTZCmd VISCA_CAM_Memory_Set("8101043f0100ff",
				  {new visca_u7("preset_num", 5)});
const PTZCmd VISCA_CAM_Memory_Recall("8101043f0200ff",
				     {new visca_u7("preset_num", 5)});

const PTZCmd VISCA_CAM_IDWrite("8101042200000000ff",
			       {
				       new visca_u16("camera_id", 4),
			       });
const PTZInq VISCA_CAM_IDInq("81090422ff", {new visca_u16("camera_id", 2)});

const PTZCmd VISCA_CAM_ChromaSuppress("8101045f00ff",
				      {new visca_u4("chroma_suppress", 4)});
const PTZInq VISCA_CAM_ChromaSuppressInq("8109045fff",
					 {new visca_u4("chroma_suppress", 2)});

const PTZCmd VISCA_CAM_ColorGain("8101044900000000ff",
				 {new visca_u4("color_spec", 6),
				  new visca_u4("color_gain", 7)});
const PTZInq VISCA_CAM_ColorGainInq("81090449ff",
				    {new visca_u4("color_gain", 4)});

const PTZCmd VISCA_CAM_ColorHue("8101044f00000000ff",
				{new visca_u4("hue_spec", 6),
				 new visca_u4("hue_phase", 7)});
const PTZInq VISCA_CAM_ColorHueInq("8109044fff",
				   {new visca_u4("hue_phase", 4)});

const PTZCmd VISCA_CAM_LowLatency_On("81017e015a02ff");
const PTZCmd VISCA_CAM_LowLatency_Off("81017e015a03ff");
const PTZInq VISCA_CAM_LowLatencyInq("81097e015aff",
				     {new visca_flag("lowlatency", 2)});

const PTZCmd VISCA_SYSMenu_Off("8101060603ff");
const PTZInq VISCA_SYSMenuInq("81010606ff", {new visca_flag("menumode", 2)});

const PTZCmd VISCA_CAM_InfoDisplay_On("81017e011802ff");
const PTZCmd VISCA_CAM_InfoDisplay_Off("81017e011803ff");
const PTZInq VISCA_CAM_InfoDisplayInq("81097e0118ff",
				      {new visca_flag("info_display", 2)});

const PTZCmd VISCA_VideoFormat_set("81017e011e0000ff",
				   {new visca_u8("video_format", 5)});
const PTZInq VISCA_VideoFormatInq("81090623ff",
				  {new visca_u4("video_format", 2)});

const PTZCmd VISCA_ColorSystem_set("81017e01030000ff",
				   {new visca_u4("color_format", 6)});
const PTZInq VISCA_ColorSystemInq("81097e0103ff",
				  {new visca_u4("color_format", 2)});

const PTZCmd VISCA_IRReceive_On("8101060802ff");
const PTZCmd VISCA_IRReceive_Off("8101060803ff");
const PTZCmd VISCA_IRReceive_Toggle("8101060810ff");
const PTZInq VISCA_IRReceiveInq("81090608ff", {new visca_flag("irreceive", 2)});

const PTZCmd VISCA_IRReceiveReturn_On("81017d01030000ff");
const PTZCmd VISCA_IRReceiveReturn_Off("81017d01130000ff");

const PTZInq VISCA_IRConditionInq("81090634ff",
				  {new visca_u4("ircondition", 2)});

const PTZInq VISCA_PanTilt_MaxSpeedInq("81090611ff",
				       {new visca_u7("panmaxspeed", 2),
					new visca_u7("tiltmaxspeed", 3)});

const PTZCmd VISCA_PanTilt_drive("8101060100000303ff",
				 {new visca_s7("pan", 4),
				  new visca_s7("tilt", 5)},
				 "pan_pos");
const PTZCmd VISCA_PanTilt_drive_abs("8101060200000000000000000000ff",
				     {new visca_u7("panspeed", 4),
				      new visca_u7("tiltspeed", 5),
				      new visca_s16("pan_pos", 6),
				      new visca_s16("tilt_pos", 10)},
				     "pan_pos");
const PTZCmd VISCA_PanTilt_drive_rel("8101060300000000000000000000ff",
				     {new visca_u7("panspeed", 4),
				      new visca_u7("tiltspeed", 5),
				      new visca_s16("pan_pos", 6),
				      new visca_s16("tilt_pos", 10)},
				     "pan_pos");
const PTZCmd VISCA_PanTilt_Home("81010604ff", "pan_pos");
const PTZCmd VISCA_PanTilt_Reset("81010605ff", "pan_pos");
const PTZInq VISCA_PanTilt_PosInq("81090612ff", {new visca_s16("pan_pos", 2),
						 new visca_s16("tilt_pos", 6)});

const PTZCmd
	VISCA_PanTilt_LimitSetUpRight("8101060700010000000000000000ff",
				      {new visca_u16("pan_limit_right", 6),
				       new visca_u16("tilt_limit_up", 10)});
const PTZCmd
	VISCA_PanTilt_LimitSetDownLeft("8101060700000000000000000000ff",
				       {new visca_u16("pan_limit_left", 6),
					new visca_u16("tilt_limit_down", 10)});
const PTZCmd
	VISCA_PanTilt_LimitClearUpRight("810106070101070f0f0f070f0f0fff",
					{new visca_u16("pan_limit_right", 6),
					 new visca_u16("tilt_limit_up", 10)});
const PTZCmd VISCA_PanTilt_LimitClearDownLeft(
	"810106070100070f0f0f070f0f0fff",
	{new visca_u16("pan_limit_left", 6),
	 new visca_u16("tilt_limit_down", 10)});

const QMap<int, std::string> PTZVisca::viscaVendors = {
	{0x0001, "Sony"},
	{0x0109, "Birddog"},
};

/* lookup in this table is: (Vendor ID << 16) | Model ID */
const QMap<int, std::string> PTZVisca::viscaModels = {
	/* Sony Cameras */
	{0x0001040f, "BRC-300"},
	{0x00010511, "SRG-120DH"},
	/* Birddog */
	{0x01092020, "P100"},
};

/* Mapping properties to enquires */
const QMap<QString, PTZInq> PTZVisca::inquires = {
	{"vendor_id", VISCA_CAM_VersionInq},
	{"power_on", VISCA_CAM_PowerInq},
	{"pan_pos", VISCA_PanTilt_PosInq},
	{"tilt_pos", VISCA_PanTilt_PosInq},
	{"focus_pos", VISCA_LensControlInq},
	{"zoom_pos", VISCA_LensControlInq},
	{"wb_mode", VISCA_CameraControlInq},
	{"iris_pos", VISCA_CameraControlInq},
	{"gain_pos", VISCA_CameraControlInq},
	{"camera_id", VISCA_OtherInq},
	{"dzoom_pos", VISCA_EnlargementFunction1Inq},
	{"defog_mode", VISCA_EnlargementFunction2Inq},
	{"color_hue", VISCA_EnlargementFunction3Inq},
};

/*
 * PTZVisca Methods
 */
PTZVisca::PTZVisca(OBSData config) : PTZDevice(config)
{
	for (int i = 0; i < 8; i++)
		active_cmd[i] = std::nullopt;
	connect(&timeout_timer, &QTimer::timeout, this, &PTZVisca::timeout);
}

void PTZVisca::set_settings(OBSData new_settings)
{
	/* `updates` is the property values that should be cached */
	OBSData updates = obs_data_create();
	obs_data_release(updates);

	PTZDevice::set_settings(new_settings);

	if (obs_data_has_user_value(new_settings, "visca_pan_speed_max"))
		visca_pan_speed_max = (int)obs_data_get_int(
			new_settings, "visca_pan_speed_max");
	if (obs_data_has_user_value(new_settings, "visca_tilt_speed_max"))
		visca_tilt_speed_max = (int)obs_data_get_int(
			new_settings, "visca_tilt_speed_max");
	if (obs_data_has_user_value(new_settings, "visca_zoom_speed_max"))
		visca_zoom_speed_max = (int)obs_data_get_int(
			new_settings, "visca_zoom_speed_max");
	if (obs_data_has_user_value(new_settings, "visca_focus_speed_max"))
		visca_focus_speed_max = (int)obs_data_get_int(
			new_settings, "visca_focus_speed_max");

	if (obs_data_has_user_value(new_settings, "power_on")) {
		bool power_on = obs_data_get_bool(new_settings, "power_on");
		if (power_on != obs_data_get_bool(settings, "power_on"))
			send(VISCA_CAM_Power, {power_on});
	}

	auto wb_mode = (int)obs_data_get_int(new_settings, "wb_mode");
	if (wb_mode != obs_data_get_int(settings, "wb_mode")) {
		send(VISCA_CAM_WB_Mode, {wb_mode});
	}

	if (obs_data_has_user_value(new_settings, "wb_onepush_trigger")) {
		/* Just send command, don't change state */
		send(VISCA_CAM_WB_OnePushTrigger);
	}
}

void PTZVisca::set_config(OBSData cfg)
{
	PTZDevice::set_config(cfg);
	obs_data_set_default_int(cfg, "visca_pan_speed_max", 0x18);
	obs_data_set_default_int(cfg, "visca_tilt_speed_max", 0x14);
	obs_data_set_default_int(cfg, "visca_zoom_speed_max", 0x7);
	obs_data_set_default_int(cfg, "visca_focus_speed_max", 0x7);
	visca_pan_speed_max = (int)obs_data_get_int(cfg, "visca_pan_speed_max");
	visca_tilt_speed_max =
		(int)obs_data_get_int(cfg, "visca_tilt_speed_max");
	visca_zoom_speed_max =
		(int)obs_data_get_int(cfg, "visca_zoom_speed_max");
	visca_focus_speed_max =
		(int)obs_data_get_int(cfg, "visca_focus_speed_max");
}

OBSData PTZVisca::get_config()
{
	OBSData cfg = PTZDevice::get_config();
	obs_data_set_int(cfg, "visca_pan_speed_max", visca_pan_speed_max);
	obs_data_set_int(cfg, "visca_tilt_speed_max", visca_tilt_speed_max);
	obs_data_set_int(cfg, "visca_zoom_speed_max", visca_zoom_speed_max);
	obs_data_set_int(cfg, "visca_focus_speed_max", visca_focus_speed_max);
	return cfg;
}

obs_properties_t *PTZVisca::get_obs_properties()
{
	auto *ptz_props = PTZDevice::get_obs_properties();

	auto *wbGroup = obs_properties_create();
	obs_properties_add_group(ptz_props, "whitebalance", "White Balance",
				 OBS_GROUP_NORMAL, wbGroup);

	auto *list = obs_properties_add_list(wbGroup, "wb_mode", "Mode",
					     OBS_COMBO_TYPE_LIST,
					     OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(list, "Auto", 0);
	obs_property_list_add_int(list, "Indoor", 1);
	obs_property_list_add_int(list, "Outdoor", 2);
	obs_property_list_add_int(list, "One Push", 3);
	obs_property_list_add_int(list, "Auto Tracing", 4);
	obs_property_list_add_int(list, "Manual", 5);

	auto clicked_cb = [](obs_properties_t *pps, obs_property_t *property,
			     void *data) {
		Q_UNUSED(pps);
		Q_UNUSED(property);
		PTZVisca *ptz = static_cast<PTZVisca *>(data);
		ptz->send(VISCA_CAM_WB_OnePushTrigger);
		return false;
	};
	obs_properties_add_button2(wbGroup, "one-push", "One Push Whitebalance",
				   clicked_cb, this);

	auto visca_grp = obs_properties_create();
	obs_properties_add_group(ptz_props, "visca_advanced",
				 "Modify Expert VISCA Settings",
				 OBS_GROUP_CHECKABLE, visca_grp);
	obs_properties_add_int_slider(visca_grp, "visca_pan_speed_max",
				      "Pan Maximum Speed (default 24)", 1, 0x7f,
				      1);
	obs_properties_add_int_slider(visca_grp, "visca_tilt_speed_max",
				      "Tilt Maximum Speed (default 20)", 1,
				      0x7f, 1);
	obs_properties_add_int_slider(visca_grp, "visca_zoom_speed_max",
				      "Zoom Maximum Speed (default 7)", 0, 7,
				      1);
	obs_properties_add_int_slider(visca_grp, "visca_focus_speed_max",
				      "Focus Maximum Speed (default 7)", 0, 7,
				      1);
	return ptz_props;
}

void PTZVisca::send(PTZCmd cmd)
{
	pending_cmds.append(cmd);
	ptz_debug("sending cmd, queuelength=%i", (int)pending_cmds.count());
	send_pending();
}

void PTZVisca::send(PTZCmd cmd, QList<int> args)
{
	cmd.encode(args);
	send(cmd);
}

void PTZVisca::send_packet(const QByteArray &packet)
{
	ptz_debug("--> %s", packet.toHex(':').data());
	send_immediate(packet);
	timeout_timer.setSingleShot(true);
	timeout_timer.start(1000 / 30);
}

void PTZVisca::timeout()
{
	if ((status & STATUS_CONNECTED) && active_cmd[0].has_value() &&
	    (timeout_retry < 3)) {
		ptz_debug("timeout");
		send_packet(active_cmd[0].value().cmd);
		timeout_retry++;
	} else {
		status &= ~STATUS_CONNECTED;
		active_cmd[0] = std::nullopt;
		send_pending();
	}
}

void PTZVisca::cmd_get_camera_info()
{
	status |= STATUS_CONNECTED;
	for (auto key : inquires.keys())
		stale_settings += key;
	send_pending();
}

void PTZVisca::receive(const QByteArray &msg)
{
	if (VISCA_PACKET_SENDER(msg) != address || (msg.size() < 3))
		return;
	ptz_debug("<-- %s", msg.toHex(':').data());
	int slot = msg[1] & 0x7;

	switch (msg[1] & 0xf0) {
	case VISCA_RESPONSE_ACK:
		status |= STATUS_CONNECTED;
		if (slot != 0) {
			active_cmd[slot] = active_cmd[0];
			active_cmd[0] = std::nullopt;
		}
		break;
	case VISCA_RESPONSE_COMPLETED:
		status |= STATUS_CONNECTED;
		if (slot == 0)
			timeout_timer.stop(); /* timer is only for slot 0 */
		if (!active_cmd[slot].has_value()) {
			ptz_debug("spurious reply: %s", msg.toHex(':').data());
			break;
		}

		/* Slot 0 responses are inquiries that need to be parsed */
		if (slot == 0 && msg.size() > 3) {
			/* Some devices (e.g. cicso) don't use slots and
			 * commands complete immediately. Only decode
			 * response if the payload size is non-zero */
			obs_data_t *rslt_props =
				active_cmd[0].value().decode(msg);
			obs_data_apply(settings, rslt_props);

			/* Mark returned properties as clean */
			for (auto item = obs_data_first(rslt_props); item;
			     obs_data_item_next(&item))
				stale_settings -= obs_data_item_get_name(item);

			/* Data has been updated */
			emit settingsChanged(rslt_props);
			obs_data_release(rslt_props);
		}

		active_cmd[slot] = std::nullopt;
		break;
	case VISCA_RESPONSE_ERROR:
		timeout_timer.stop();
		/* This command failed, don't generate it again */
		if (active_cmd[0].has_value()) {
			for (auto rslt : active_cmd[0].value().results)
				stale_settings -= rslt->name;
		}
		ptz_debug("rx error: %s", msg.toHex(':').data());
		active_cmd[0] = std::nullopt;
		active_cmd[slot] = std::nullopt;
		break;
	default:
		ptz_debug("rx unknown: %s", msg.toHex(':').data());
		break;
	}
	send_pending();
}

void PTZVisca::send_pending()
{
	if (active_cmd[0].has_value())
		return;

	if (pending_cmds.isEmpty()) {
		if (status & STATUS_PANTILT_SPEED_CHANGED) {
			status &= ~STATUS_PANTILT_SPEED_CHANGED;
			int p = scale_speed(pan_speed, visca_pan_speed_max);
			int t = -scale_speed(tilt_speed, visca_tilt_speed_max);
			PTZCmd cmd = VISCA_PanTilt_drive;
			cmd.encode({p, t});
			pending_cmds += cmd;
		} else if (status & STATUS_ZOOM_SPEED_CHANGED) {
			status &= ~STATUS_ZOOM_SPEED_CHANGED;
			PTZCmd cmd = VISCA_CAM_Zoom_drive;
			cmd.encode({scale_speed(zoom_speed,
						visca_zoom_speed_max + 1)});
			pending_cmds += cmd;
		} else if (status & STATUS_FOCUS_SPEED_CHANGED) {
			status &= ~STATUS_FOCUS_SPEED_CHANGED;
			PTZCmd cmd = VISCA_CAM_Focus_drive;
			cmd.encode({scale_speed(focus_speed,
						visca_focus_speed_max + 1)});
			pending_cmds += cmd;
		} else if (status & STATUS_CONNECTED) {
			if (pan_speed || tilt_speed)
				stale_settings += "pan_pos";
			if (zoom_speed)
				stale_settings += "zoom_pos";
			if (focus_speed)
				stale_settings += "focus_pos";
			ptz_debug(
				"Stale prop list: {%s}",
				QT_TO_UTF8(stale_settings.values().join(',')));
			QSetIterator<QString> i(stale_settings);
			while (i.hasNext()) {
				QString prop = i.next();
				if (inquires.contains(prop)) {
					pending_cmds += inquires[prop];
					break;
				}
			}
		}
	}

	if (pending_cmds.isEmpty())
		return;

	active_cmd[0] = pending_cmds.takeFirst();
	auto affects = active_cmd[0].value().affects;
	if (affects != "")
		stale_settings += affects;
	send_packet(active_cmd[0].value().cmd);
	timeout_retry = 0;
}

void PTZVisca::do_update(void)
{
	send_pending();
}

void PTZVisca::pantilt_rel(double pan_, double tilt_)
{
	int pan = std::clamp(pan_, -1.0, 1.0) * 0x1400 * 2;
	int tilt = std::clamp(tilt_, -1.0, 1.0) * 0x500 * 2;
	send(VISCA_PanTilt_drive_rel, {0x14, 0x14, pan, tilt});
}

void PTZVisca::pantilt_abs(double pan_, double tilt_)
{
	int pan = std::clamp(pan_, -1.0, 1.0) * 0x1400;
	int tilt = std::clamp(tilt_, -1.0, 1.0) * 0x500;
	send(VISCA_PanTilt_drive_abs, {0x0f, 0x0f, pan, tilt});
}

void PTZVisca::pantilt_home()
{
	send(VISCA_PanTilt_Home);
}

void PTZVisca::zoom_abs(double pos_)
{
	int pos = std::clamp(pos_, 0.0, 1.0) * 0x7ac0;
	send(VISCA_CAM_Zoom_Direct, {pos});
}

void PTZVisca::set_autofocus(bool enabled)
{
	send(enabled ? VISCA_CAM_Focus_Auto : VISCA_CAM_Focus_Manual);
	obs_data_set_bool(settings, "focus_af_enabled", enabled);
}

void PTZVisca::focus_onetouch()
{
	send(VISCA_CAM_Focus_OneTouch);
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
