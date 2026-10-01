/* VISCA commands and inquiries
 *
 * Copyright 2020 Grant Likely <grant.likely@secretlab.ca>
 *
 * SPDX-License-Identifier: GPLv2
 */

#include "ptz-visca.hpp"
#include "ptz-visca-commands.hpp"

/* Visca specific datagram field classes */
class visca_u4 : public int_field {
public:
	visca_u4(const char *name, int offset) : int_field(name, offset, 0x0f) {}
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
		msg[offset] = val ? std::clamp(abs(val) - 1, 0, 0x7) | (val > 0 ? 0x20 : 0x30) : 0;
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
	visca_flag(const char *name, int offset) : datagram_field(name, offset) {}
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
	visca_u7(const char *name, int offset) : int_field(name, offset, 0x7f) {}
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
	visca_u8(const char *name, int offset) : int_field(name, offset, 0x0f0f) {}
};

/* 15 bit value encoded into two bytes. Protocol encoding forces bit 15 & 7 to zero */
class visca_u15 : public datagram_field {
public:
	visca_u15(const char *name, int offset) : datagram_field(name, offset) {}
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
		uint16_t val = (msg[offset] & 0x7f) << 8 | (msg[offset + 1] & 0x7f);
		obs_data_set_int(data, name, val);
		return true;
	}
};

class visca_s16 : public int_field {
public:
	visca_s16(const char *name, int offset) : int_field(name, offset, 0x0f0f0f0f, true) {}
};

class visca_u16 : public int_field {
public:
	visca_u16(const char *name, int offset) : int_field(name, offset, 0x0f0f0f0f) {}
};

/* The top two nibbles of a visca_u16, where a reply leaves out the rest: the
 * lens control block inquiry's focus near limit, which is 0xpq00 */
class visca_u16_high : public int_field {
public:
	visca_u16_high(const char *name, int offset) : int_field(name, offset, 0x0f0f) {}
	bool decode(OBSData data, QByteArray &msg) override
	{
		int val;
		if (!decode_int(&val, msg))
			return false;
		obs_data_set_int(data, name, val << 8);
		return true;
	}
};

const PTZCmd VISCA_ENUMERATE("883001ff");

const PTZInq VISCA_CAM_VersionInq("81090002ff",
				  {new int_field("vendor_id", 2, 0x7fff), new int_field("model_id", 4, 0x7fff),
				   new string_lookup_field("vendor_name", PTZVisca::viscaVendors, 2, 0x7fff),
				   new string_lookup_field("model_name", PTZVisca::viscaModels, 2, 0x7fffffff),
				   new int_field("rom_version", 6, 0xffff), new int_field("socket_number", 8, 0xff)});

/* The block inquiries: what each reply byte holds is in the Sony SRG-120DH
 * manual's "Block Inquiry Command List", where a reply's bytes are numbered
 * from its address byte, as the offsets here are. Every value they read has
 * the same key and scale as the single-value inquiry for it further down, so
 * that either can be used to read it (see PTZVisca::inquiresFallback). */
const PTZInq VISCA_LensControlInq(
	"81097e7e00ff",
	{new int_field("zoom_pos", 2, 0x0f0f0f0f), new visca_u16_high("focus_near_limit", 6),
	 new int_field("focus_pos", 8, 0x0f0f0f0f), new int_field("focus_af_mode", 13, 0b00011000),
	 new bool_field("focus_af_sensitivity", 13, 0b0100), new bool_field("dzoom_on", 13, 0b0010),
	 new bool_field("focus_af_enabled", 13, 0b0001), new bool_field("low_contrast", 14, 0b1000),
	 new bool_field("memory_recall_running", 14, 0b0100), new bool_field("focus_command_running", 14, 0b0010),
	 new bool_field("zoom_command_running", 14, 0b0001)});

/* Byte 9 also has a bit for wide dynamic range, set for any mode but off;
 * "wd_mode", which says which mode, is read with VISCA_CAM_WDInq instead. */
const PTZInq VISCA_CameraControlInq(
	"81097e7e01ff",
	{new visca_u8("r_gain", 2), new visca_u8("b_gain", 4), new visca_u4("wb_mode", 6),
	 new visca_u4("aperture_gain", 7), new visca_u4("ae_mode", 8), new bool_field("high_resolution", 9, 0b00100000),
	 new bool_field("back_light", 9, 0b0100), new bool_field("exposure_comp", 9, 0b0010),
	 new bool_field("slow_shutter", 9, 0b0001), new int_field("shutter_pos", 10, 0x1f),
	 new int_field("iris_pos", 11, 0x1f), new int_field("gain_pos", 12, 0x1f),
	 new int_field("bright_pos", 13, 0x1f), new int_field("exposure_comp_pos", 14, 0x0f)});

/* Byte 2 also has the power, which VISCA_CAM_PowerInq reads for every camera */
const PTZInq VISCA_OtherInq("81097e7e02ff",
			    {new int_field("picture_effect", 5, 0x0f), new int_field("camera_id", 8, 0x0f0f0f0f),
			     new bool_field("video_50hz", 12, 0b0001)});

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

const PTZInq VISCA_EnlargementFunction2Inq("81097e7e04ff", {new bool_field("defog_mode", 7, 0b0001)});

const PTZInq VISCA_EnlargementFunction3Inq("81097e7e05ff", {new int_field("color_hue", 2, 0b1111)});

const PTZCmd VISCA_CommandCancel("8120ff", {new visca_u4("socket", 1)});
const PTZCmd VISCA_CAM_Power("8101040000ff", {new visca_flag("power_on", 4)}, "power_on");
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
				   },
				   "zoom_pos");
const PTZInq VISCA_CAM_ZoomPosInq("81090447ff", {new visca_u16("zoom_pos", 2)});

const PTZCmd VISCA_CAM_DZoom("8101040600ff", {new visca_flag("dzoom_on", 4)}, "dzoom_on");
const PTZInq VISCA_CAM_DZoomModeInq("81090406ff", {new visca_flag("dzoom_on", 2)});

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
const PTZInq VISCA_CAM_Focus_AFEnabledInq("81090438ff", {new visca_flag("focus_af_enabled", 2)});

const PTZCmd VISCA_CAM_Focus_OneTouch("8101041801ff");
const PTZCmd VISCA_CAM_Focus_Infinity("8101041802ff");

const PTZCmd VISCA_CAM_FocusPos("8101044800000000ff",
				{
					new visca_s16("focus_pos", 4),
				},
				"focus_pos");
const PTZInq VISCA_CAM_FocusPosInq("81090448ff", {new visca_u16("focus_pos", 2)});

const PTZCmd VISCA_CAM_Focus_NearLimit("8101042800000000ff", {new visca_u16("focus_near_limit", 4)},
				       "focus_near_limit");
const PTZInq VISCA_CAM_FocusNearLimitInq("81090428ff", {new visca_u16("focus_near_limit", 2)});

const PTZCmd VISCA_CAM_ZoomFocus_Direct("810104470000000000000000ff",
					{new visca_s16("zoom_pos", 4), new visca_s16("focus_pos", 8)},
					affected_keys{"zoom_pos", "focus_pos"});

/* true is Normal, false is Low */
const PTZCmd VISCA_CAM_AF_Sensitivity("8101045800ff", {new visca_flag("focus_af_sensitivity", 4)},
				      "focus_af_sensitivity");
const PTZInq VISCA_CAM_AFSensitivityInq("81090458ff", {new visca_flag("focus_af_sensitivity", 2)});

/* 0: Normal, 1: Interval, 2: Zoom trigger */
const PTZCmd VISCA_CAM_AFMode("8101045700ff", {new visca_u4("focus_af_mode", 4)}, "focus_af_mode");
const PTZInq VISCA_CAM_AFModeInq("81090457ff", {new visca_u4("focus_af_mode", 2)});

/* In seconds, for the Interval AF mode */
const PTZCmd VISCA_CAM_AFMode_ActiveIntervalTime("8101042700000000ff",
						 {new visca_u8("focus_af_move_time", 4),
						  new visca_u8("focus_af_interval_time", 6)},
						 affected_keys{"focus_af_move_time", "focus_af_interval_time"});
const PTZInq VISCA_CAM_AFTimeSettingInq("81090427ff", {new visca_u8("focus_af_move_time", 2),
						       new visca_u8("focus_af_interval_time", 4)});

/* 0: Standard, 1: IR light */
const PTZCmd VISCA_CAM_IRCorrection("8101041100ff", {new visca_u4("ir_correction", 4)}, "ir_correction");
const PTZInq VISCA_CAM_IRCorrectionInq("81090411ff", {new visca_u4("ir_correction", 2)});

const PTZCmd VISCA_CAM_WB_Mode("8101043500ff", {new visca_u4("wb_mode", 4)}, "wb_mode");
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
const PTZCmd VISCA_CAM_RGain_Direct("8101044300000000ff", {new visca_u8("r_gain", 6)}, "r_gain");
const PTZInq VISCA_CAM_RGainInq("81090443ff", {new visca_u8("r_gain", 4)});

const PTZCmd VISCA_CAM_BGain_Reset("8101040400ff");
const PTZCmd VISCA_CAM_BGain_Up("8101040402ff");
const PTZCmd VISCA_CAM_BGain_Down("8101040403ff");
const PTZCmd VISCA_CAM_BGain_Direct("8101044400000000ff", {new visca_u8("b_gain", 6)}, "b_gain");
const PTZInq VISCA_CAM_BGainInq("81090444ff", {new visca_u8("b_gain", 4)});

/* 0: Full auto, 3: Manual, 0xa: Shutter priority, 0xb: Iris priority, 0xd: Bright */
const PTZCmd VISCA_CAM_AE("8101043900ff", {new visca_u4("ae_mode", 4)}, "ae_mode");
const PTZInq VISCA_CAM_AEModeInq("81090439ff", {new visca_u4("ae_mode", 2)});

/* true is Auto */
const PTZCmd VISCA_CAM_SlowShutter("8101045a00ff", {new visca_flag("slow_shutter", 4)}, "slow_shutter");
const PTZInq VISCA_CAM_SlowShutterModeInq("8109045aff", {new visca_flag("slow_shutter", 2)});

const PTZCmd VISCA_CAM_Shutter_Reset("8101040a00ff");
const PTZCmd VISCA_CAM_Shutter_Up("8101040a02ff");
const PTZCmd VISCA_CAM_Shutter_Down("8101040a03ff");
const PTZCmd VISCA_CAM_Shutter_Direct("8101044a00000000ff", {new visca_u8("shutter_pos", 6)}, "shutter_pos");
const PTZInq VISCA_CAM_ShutterPosInq("8109044aff", {new visca_u8("shutter_pos", 4)});

const PTZCmd VISCA_CAM_Iris_Reset("8101040b00ff");
const PTZCmd VISCA_CAM_Iris_Up("8101040b02ff");
const PTZCmd VISCA_CAM_Iris_Down("8101040b03ff");
const PTZCmd VISCA_CAM_Iris_Direct("8101044b00000000ff", {new visca_u8("iris_pos", 6)}, "iris_pos");
const PTZInq VISCA_CAM_IrisPosInq("8109044bff", {new visca_u8("iris_pos", 4)});

const PTZCmd VISCA_CAM_Gain_Reset("8101040c00ff");
const PTZCmd VISCA_CAM_Gain_Up("8101040c02ff");
const PTZCmd VISCA_CAM_Gain_Down("8101040c03ff");
const PTZCmd VISCA_CAM_Gain_Direct("8101044c00000000ff", {new visca_u8("gain_pos", 6)}, "gain_pos");
const PTZInq VISCA_CAM_GainPosInq("8109044cff", {new visca_u8("gain_pos", 4)});

const PTZCmd VISCA_CAM_Gain_Limit("8101042c00ff", {new visca_u4("gain_limit", 4)}, "gain_limit");
const PTZInq VISCA_CAM_GainLimitInq("8109042cff", {new visca_u4("gain_limit", 2)});

const PTZCmd VISCA_CAM_Bright_Up("8101040d02ff");
const PTZCmd VISCA_CAM_Bright_Down("8101040d03ff");
const PTZCmd VISCA_CAM_Bright_Direct("8101044d00000000ff", {new visca_u8("bright_pos", 6)}, "bright_pos");
const PTZInq VISCA_CAM_BrightPosInq("8109044dff", {new visca_u8("bright_pos", 4)});

const PTZCmd VISCA_CAM_ExpComp("8101043e00ff", {new visca_flag("exposure_comp", 4)}, "exposure_comp");
const PTZInq VISCA_CAM_ExpCompModeInq("8109043eff", {new visca_flag("exposure_comp", 2)});

const PTZCmd VISCA_CAM_ExpComp_Reset("8101040e00ff");
const PTZCmd VISCA_CAM_ExpComp_Up("8101040e02ff");
const PTZCmd VISCA_CAM_ExpComp_Down("8101040e03ff");
const PTZCmd VISCA_CAM_ExpComp_Direct("8101044e00000000ff", {new visca_u8("exposure_comp_pos", 6)},
				      "exposure_comp_pos");
const PTZInq VISCA_CAM_ExpCompPosInq("8109044eff", {new visca_u8("exposure_comp_pos", 4)});

const PTZCmd VISCA_CAM_Backlight("8101043300ff", {new visca_flag("back_light", 4)}, "back_light");
const PTZInq VISCA_CAM_BacklightInq("81090433ff", {new visca_flag("back_light", 2)});

/* 0: Off, 1: Low, 2: Mid, 3: High */
const PTZCmd VISCA_CAM_WD("81017e040000ff", {new visca_u4("wd_mode", 5)}, "wd_mode");
const PTZInq VISCA_CAM_WDInq("81097e0400ff", {new visca_u4("wd_mode", 2)});

const PTZCmd VISCA_CAM_Defog("810104370000ff", {new visca_flag("defog_mode", 4)}, "defog_mode");
const PTZInq VISCA_CAM_DefogInq("81090437ff", {new visca_flag("defog_mode", 2)});

const PTZCmd VISCA_CAM_Aperture_Reset("8101040200ff");
const PTZCmd VISCA_CAM_Aperture_Up("8101040202ff");
const PTZCmd VISCA_CAM_Aperture_Down("8101040203ff");
const PTZCmd VISCA_CAM_Aperture_Direct("8101044200000000ff", {new visca_u8("aperture_gain", 6)}, "aperture_gain");
const PTZInq VISCA_CAM_ApertureInq("81090442ff", {new visca_u8("aperture_gain", 4)});

const PTZCmd VISCA_CAM_HR("8101045200ff", {new visca_flag("high_resolution", 4)}, "high_resolution");
const PTZInq VISCA_CAM_HRInq("81090452ff", {new visca_flag("high_resolution", 2)});

/* 0: Off, 1 to 5 */
const PTZCmd VISCA_CAM_NR("8101045300ff", {new visca_u4("nr_level", 4)}, "nr_level");
const PTZInq VISCA_CAM_NRInq("81090453ff", {new visca_u4("nr_level", 2)});

/* 0: Standard, 1: Off */
const PTZCmd VISCA_CAM_Gamma("8101045b00ff", {new visca_u4("gamma", 4)}, "gamma");
const PTZInq VISCA_CAM_GammaInq("8109045bff", {new visca_u4("gamma", 2)});

const PTZCmd VISCA_CAM_HighSensitivity("8101045e00ff", {new visca_flag("high_sensitivity", 4)}, "high_sensitivity");
const PTZInq VISCA_CAM_HighSensitivityInq("8109045eff", {new visca_flag("high_sensitivity", 2)});

/* 0: Off, 2: Negative art, 4: Black and white */
const PTZCmd VISCA_CAM_PictureEffect("8101046300ff", {new visca_u4("picture_effect", 4)}, "picture_effect");
const PTZInq VISCA_CAM_PictureEffectInq("81090463ff", {new visca_u4("picture_effect", 2)});

const PTZCmd VISCA_CAM_Memory_Reset("8101043f0000ff", {new visca_u7("preset_num", 5)});
const PTZCmd VISCA_CAM_Memory_Set("8101043f0100ff", {new visca_u7("preset_num", 5)});
const PTZCmd VISCA_CAM_Memory_Recall("8101043f0200ff", {new visca_u7("preset_num", 5)});

const PTZCmd VISCA_CAM_IDWrite("8101042200000000ff", {new visca_u16("camera_id", 4)}, "camera_id");
const PTZInq VISCA_CAM_IDInq("81090422ff", {new visca_u16("camera_id", 2)});

/* 0: Off, 1 to 3 */
const PTZCmd VISCA_CAM_ChromaSuppress("8101045f00ff", {new int_field("chroma_suppress", 4, 0xff)}, "chroma_suppress");
const PTZInq VISCA_CAM_ChromaSuppressInq("8109045fff", {new visca_u4("chroma_suppress", 2)});

/* Color gain and hue can be set for each of six colours as well as for them
 * all together ("master", specification 0), but the inquiries only say what
 * the master one is, so that is the one set. Both are 0 to 0xe: a gain of
 * 60% to 200%, and a hue of -14 to +14 degrees. */
const PTZCmd VISCA_CAM_ColorGain("8101044900000000ff", {new visca_u4("color_gain", 7)}, "color_gain");
const PTZInq VISCA_CAM_ColorGainInq("81090449ff", {new visca_u4("color_gain", 5)});

const PTZCmd VISCA_CAM_ColorHue("8101044f00000000ff", {new visca_u4("color_hue", 7)}, "color_hue");
const PTZInq VISCA_CAM_ColorHueInq("8109044fff", {new visca_u4("color_hue", 5)});

/* true is Low latency, false Normal */
const PTZCmd VISCA_CAM_LowLatency("81017e015a00ff", {new visca_flag("low_latency", 5)}, "low_latency");
const PTZInq VISCA_CAM_LowLatencyInq("81097e015aff", {new visca_flag("low_latency", 2)});

/* Tally lamp on/off, an extension outside Sony's own manual for this
 * camera that Sony's broadcast/interchangeable-lens VISCA cameras and most
 * third-party VISCA PTZ cameras (PTZOptics, BirdDog, Avonic, ...) share */
const PTZCmd VISCA_CAM_Tally_On("81017e010a0002ff", "tally_on");
const PTZCmd VISCA_CAM_Tally_Off("81017e010a0003ff", "tally_on");
const PTZInq VISCA_CAM_TallyInq("81097e010aff", {new visca_flag("tally_on", 2)});

/* The green tally lamp, which BirdDog documents for its X4 series (a P100
 * answers it with a syntax error), so it is not on every camera that has
 * the red one above. Neither has an inquiry on a BirdDog, so nothing reads
 * a lamp back: see PTZVisca::receive() for how the state follows them. */
const PTZCmd VISCA_CAM_TallyGreen_On("81017e041a0002ff");
const PTZCmd VISCA_CAM_TallyGreen_Off("81017e041a0003ff");

/* The state a tally lamp command sets, if it is one: red is "tally_on", the
 * program lamp; green is "tally_preview". */
const char *visca_tally_key(const QByteArray &cmd)
{
	if (cmd == VISCA_CAM_Tally_On.cmd || cmd == VISCA_CAM_Tally_Off.cmd)
		return "tally_on";
	if (cmd == VISCA_CAM_TallyGreen_On.cmd || cmd == VISCA_CAM_TallyGreen_Off.cmd)
		return "tally_preview";
	return nullptr;
}

/* The on-screen menu can only be closed, not opened, over VISCA */
const PTZCmd VISCA_SYSMenu_Off("8101060603ff", "menu_on");
const PTZInq VISCA_SYSMenuInq("81090606ff", {new visca_flag("menu_on", 2)});

const PTZCmd VISCA_CAM_InfoDisplay("81017e011800ff", {new visca_flag("info_display", 5)}, "info_display");
const PTZInq VISCA_CAM_InfoDisplayInq("81097e0118ff", {new visca_flag("info_display", 2)});

/* See the manual's "Video Format Change" for the values. Only taken with the
 * SYSTEM SELECT switch at 7, which leaves it to VISCA. */
const PTZCmd VISCA_VideoFormat_set("81017e011e0000ff", {new visca_u8("video_format", 5)}, "video_format");
const PTZInq VISCA_VideoFormatInq("81090623ff", {new visca_u4("video_format", 2)});

/* 0: HDMI YUV, 1: HDMI GBR, 2: DVI GBR, 3: DVI YUV */
const PTZCmd VISCA_ColorSystem_set("81017e01030000ff", {new visca_u4("color_system", 6)}, "color_system");
const PTZInq VISCA_ColorSystemInq("81097e0103ff", {new visca_u4("color_system", 2)});

const PTZCmd VISCA_IRReceive("8101060800ff", {new visca_flag("ir_receive", 4)}, "ir_receive");
const PTZCmd VISCA_IRReceive_Toggle("8101060810ff");
const PTZInq VISCA_IRReceiveInq("81090608ff", {new visca_flag("ir_receive", 2)});

const PTZCmd VISCA_IRReceiveReturn_On("81017d01030000ff");
const PTZCmd VISCA_IRReceiveReturn_Off("81017d01130000ff");

/* 0: the remote commander can be received reliably, 1: it can't, 2: the
 * camera was turned on with it, so this couldn't be checked */
const PTZInq VISCA_IRConditionInq("81090634ff", {new visca_u4("ir_condition", 2)});

const PTZInq VISCA_PanTilt_MaxSpeedInq("81090611ff",
				       {new visca_u7("pan_max_speed", 2), new visca_u7("tilt_max_speed", 3)});

const PTZCmd VISCA_PanTilt_drive("8101060100000303ff", {new visca_s7("pan", 4), new visca_s7("tilt", 5)}, "pan_pos");
const PTZCmd VISCA_PanTilt_drive_abs("8101060200000000000000000000ff",
				     {new visca_u7("panspeed", 4), new visca_u7("tiltspeed", 5),
				      new visca_s16("pan_pos", 6), new visca_s16("tilt_pos", 10)},
				     "pan_pos");
const PTZCmd VISCA_PanTilt_drive_rel("8101060300000000000000000000ff",
				     {new visca_u7("panspeed", 4), new visca_u7("tiltspeed", 5),
				      new visca_s16("pan_pos", 6), new visca_s16("tilt_pos", 10)},
				     "pan_pos");
const PTZCmd VISCA_PanTilt_Home("81010604ff", "pan_pos");
const PTZCmd VISCA_PanTilt_Reset("81010605ff", "pan_pos");
const PTZInq VISCA_PanTilt_PosInq("81090612ff", {new visca_s16("pan_pos", 2), new visca_s16("tilt_pos", 6)});

/* The manual's "Pan/Tilt Status Code List". Init status is 0: not
 * initialized, 1: initializing, 2: done, 3: failed; move status is 0: no move
 * asked for, 1: moving, 2: done, 3: failed. An error is an abnormal position
 * found for that axis. */
const PTZInq VISCA_PanTilt_ModeInq(
	"81090610ff",
	{new int_field("pantilt_init_status", 2, 0b00110000), new int_field("pantilt_move_status", 2, 0b00001100),
	 new bool_field("pantilt_tilt_error", 2, 0b00000011), new bool_field("pantilt_pan_error", 3, 0b00110000),
	 new bool_field("pantilt_at_left_limit", 3, 0b0001), new bool_field("pantilt_at_right_limit", 3, 0b0010),
	 new bool_field("pantilt_at_upper_limit", 3, 0b0100), new bool_field("pantilt_at_lower_limit", 3, 0b1000)});

const PTZCmd VISCA_PanTilt_LimitSetUpRight("8101060700010000000000000000ff",
					   {new visca_u16("pan_limit_right", 6), new visca_u16("tilt_limit_up", 10)});
const PTZCmd VISCA_PanTilt_LimitSetDownLeft("8101060700000000000000000000ff",
					    {new visca_u16("pan_limit_left", 6), new visca_u16("tilt_limit_down", 10)});
const PTZCmd VISCA_PanTilt_LimitClearUpRight("810106070101070f0f0f070f0f0fff",
					     {new visca_u16("pan_limit_right", 6), new visca_u16("tilt_limit_up", 10)});
const PTZCmd VISCA_PanTilt_LimitClearDownLeft("810106070100070f0f0f070f0f0fff", {new visca_u16("pan_limit_left", 6),
										 new visca_u16("tilt_limit_down", 10)});

const QMap<int, std::string> PTZVisca::viscaVendors = {
	{0x0001, "Sony"},
	{0x0109, "Birddog"},
	{0x2574, "AVer"},
};

/* lookup in this table is: (Vendor ID << 16) | Model ID */
const QMap<int, std::string> PTZVisca::viscaModels = {
	/* Sony Cameras */
	{0x0001040f, "BRC-300"},
	{0x00010511, "SRG-120DH"},
	/* Birddog */
	{0x01092020, "P100"},
	/* AVer */
	{0x25740a30, "CAM520 Pro2"},
};

/* Mapping properties to enquires. Every property the camera is asked for when
 * the link comes up, and asked for again after a command that changes it (the
 * command's "affects"). Where a block inquiry has it, that is used: it reads
 * many at once. */
const QMap<QString, PTZInq> PTZVisca::inquires = {
	{"vendor_id", VISCA_CAM_VersionInq},
	{"power_on", VISCA_CAM_PowerInq},
	{"pan_pos", VISCA_PanTilt_PosInq},
	{"tilt_pos", VISCA_PanTilt_PosInq},
	{"pan_max_speed", VISCA_PanTilt_MaxSpeedInq},
	{"focus_pos", VISCA_LensControlInq},
	{"zoom_pos", VISCA_LensControlInq},
	{"focus_af_enabled", VISCA_LensControlInq},
	{"focus_af_mode", VISCA_LensControlInq},
	{"focus_af_sensitivity", VISCA_LensControlInq},
	{"focus_near_limit", VISCA_LensControlInq},
	{"dzoom_on", VISCA_LensControlInq},
	{"wb_mode", VISCA_CameraControlInq},
	{"r_gain", VISCA_CameraControlInq},
	{"b_gain", VISCA_CameraControlInq},
	{"aperture_gain", VISCA_CameraControlInq},
	{"ae_mode", VISCA_CameraControlInq},
	{"high_resolution", VISCA_CameraControlInq},
	{"back_light", VISCA_CameraControlInq},
	{"exposure_comp", VISCA_CameraControlInq},
	{"slow_shutter", VISCA_CameraControlInq},
	{"shutter_pos", VISCA_CameraControlInq},
	{"iris_pos", VISCA_CameraControlInq},
	{"gain_pos", VISCA_CameraControlInq},
	{"bright_pos", VISCA_CameraControlInq},
	{"exposure_comp_pos", VISCA_CameraControlInq},
	{"camera_id", VISCA_OtherInq},
	{"picture_effect", VISCA_OtherInq},
	{"dzoom_pos", VISCA_EnlargementFunction1Inq},
	{"focus_af_move_time", VISCA_EnlargementFunction1Inq},
	{"focus_af_interval_time", VISCA_EnlargementFunction1Inq},
	{"color_gain", VISCA_EnlargementFunction1Inq},
	{"gamma", VISCA_EnlargementFunction1Inq},
	{"high_sensitivity", VISCA_EnlargementFunction1Inq},
	{"nr_level", VISCA_EnlargementFunction1Inq},
	{"chroma_suppress", VISCA_EnlargementFunction1Inq},
	{"gain_limit", VISCA_EnlargementFunction1Inq},
	{"defog_mode", VISCA_EnlargementFunction2Inq},
	{"color_hue", VISCA_EnlargementFunction3Inq},
	{"ir_correction", VISCA_CAM_IRCorrectionInq},
	{"wd_mode", VISCA_CAM_WDInq},
	{"low_latency", VISCA_CAM_LowLatencyInq},
	{"menu_on", VISCA_SYSMenuInq},
	{"info_display", VISCA_CAM_InfoDisplayInq},
	{"video_format", VISCA_VideoFormatInq},
	{"color_system", VISCA_ColorSystemInq},
	{"ir_receive", VISCA_IRReceiveInq},
	{"ir_condition", VISCA_IRConditionInq},
	{"pantilt_move_status", VISCA_PanTilt_ModeInq},
	{"tally_on", VISCA_CAM_TallyInq},
};

/* What to ask for, one value at a time, when a camera answers the block
 * inquiry a property is normally read with (above) with a syntax error. A
 * BirdDog has none of the "7e 7e xx" block inquiries. Only properties whose
 * single-value inquiry reads the same value are here. */
const QMap<QString, PTZInq> PTZVisca::inquiresFallback = {
	{"zoom_pos", VISCA_CAM_ZoomPosInq},
	{"focus_pos", VISCA_CAM_FocusPosInq},
	{"focus_af_enabled", VISCA_CAM_Focus_AFEnabledInq},
	{"focus_af_mode", VISCA_CAM_AFModeInq},
	{"focus_af_sensitivity", VISCA_CAM_AFSensitivityInq},
	{"focus_near_limit", VISCA_CAM_FocusNearLimitInq},
	{"dzoom_on", VISCA_CAM_DZoomModeInq},
	{"wb_mode", VISCA_CAM_WBModeInq},
	{"r_gain", VISCA_CAM_RGainInq},
	{"b_gain", VISCA_CAM_BGainInq},
	{"aperture_gain", VISCA_CAM_ApertureInq},
	{"ae_mode", VISCA_CAM_AEModeInq},
	{"high_resolution", VISCA_CAM_HRInq},
	{"back_light", VISCA_CAM_BacklightInq},
	{"exposure_comp", VISCA_CAM_ExpCompModeInq},
	{"slow_shutter", VISCA_CAM_SlowShutterModeInq},
	{"shutter_pos", VISCA_CAM_ShutterPosInq},
	{"iris_pos", VISCA_CAM_IrisPosInq},
	{"gain_pos", VISCA_CAM_GainPosInq},
	{"bright_pos", VISCA_CAM_BrightPosInq},
	{"exposure_comp_pos", VISCA_CAM_ExpCompPosInq},
	{"camera_id", VISCA_CAM_IDInq},
	{"picture_effect", VISCA_CAM_PictureEffectInq},
	{"focus_af_move_time", VISCA_CAM_AFTimeSettingInq},
	{"focus_af_interval_time", VISCA_CAM_AFTimeSettingInq},
	{"color_gain", VISCA_CAM_ColorGainInq},
	{"gamma", VISCA_CAM_GammaInq},
	{"high_sensitivity", VISCA_CAM_HighSensitivityInq},
	{"nr_level", VISCA_CAM_NRInq},
	{"chroma_suppress", VISCA_CAM_ChromaSuppressInq},
	{"gain_limit", VISCA_CAM_GainLimitInq},
	{"defog_mode", VISCA_CAM_DefogInq},
	{"color_hue", VISCA_CAM_ColorHueInq},
};

/* Every state value the camera has, and how it is set. A value is set by
 * the command's one argument, a bool for an on/off one. In the order they are
 * sent when asked for at once: a mode before what can only be set in it (R and
 * B gain in the manual white balance mode, the shutter speed in the manual or
 * shutter priority exposure mode, and so on). */
const QList<ViscaControl> visca_controls = {
	{"power_on", VISCA_CAM_Power},
	{"wb_mode", VISCA_CAM_WB_Mode},
	{"ae_mode", VISCA_CAM_AE},
	{"exposure_comp", VISCA_CAM_ExpComp},
	{"slow_shutter", VISCA_CAM_SlowShutter},
	{"focus_af_mode", VISCA_CAM_AFMode},
	{"low_latency", VISCA_CAM_LowLatency},
	{"dzoom_on", VISCA_CAM_DZoom},
	{"focus_af_sensitivity", VISCA_CAM_AF_Sensitivity},
	{"focus_near_limit", VISCA_CAM_Focus_NearLimit},
	{"ir_correction", VISCA_CAM_IRCorrection},
	{"r_gain", VISCA_CAM_RGain_Direct},
	{"b_gain", VISCA_CAM_BGain_Direct},
	{"shutter_pos", VISCA_CAM_Shutter_Direct},
	{"iris_pos", VISCA_CAM_Iris_Direct},
	{"gain_pos", VISCA_CAM_Gain_Direct},
	{"gain_limit", VISCA_CAM_Gain_Limit},
	{"bright_pos", VISCA_CAM_Bright_Direct},
	{"exposure_comp_pos", VISCA_CAM_ExpComp_Direct},
	{"back_light", VISCA_CAM_Backlight},
	{"wd_mode", VISCA_CAM_WD},
	{"defog_mode", VISCA_CAM_Defog},
	{"high_sensitivity", VISCA_CAM_HighSensitivity},
	{"aperture_gain", VISCA_CAM_Aperture_Direct},
	{"high_resolution", VISCA_CAM_HR},
	{"nr_level", VISCA_CAM_NR},
	{"gamma", VISCA_CAM_Gamma},
	{"chroma_suppress", VISCA_CAM_ChromaSuppress},
	{"color_gain", VISCA_CAM_ColorGain},
	{"color_hue", VISCA_CAM_ColorHue},
	{"picture_effect", VISCA_CAM_PictureEffect},
	{"camera_id", VISCA_CAM_IDWrite},
	{"video_format", VISCA_VideoFormat_set},
	{"color_system", VISCA_ColorSystem_set},
	{"info_display", VISCA_CAM_InfoDisplay},
	{"ir_receive", VISCA_IRReceive},
};
