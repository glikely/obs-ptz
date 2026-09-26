/* USB Video Class (UVC) protocol helpers
 *
 * Platform-independent pieces of talking UVC to a camera's VideoControl
 * interface: request constants, control payload encoding, config descriptor
 * parsing and the AVFoundation device id format. No I/O happens here, so this
 * can be unit tested without a camera.
 *
 * SPDX-License-Identifier: GPLv2
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace uvc {

/* bRequest values (UVC 1.5 table A-8) */
constexpr uint8_t SET_CUR = 0x01;
constexpr uint8_t GET_CUR = 0x81;
constexpr uint8_t GET_MIN = 0x82;
constexpr uint8_t GET_MAX = 0x83;
constexpr uint8_t GET_RES = 0x84;

/* Camera Terminal control selectors (UVC 1.5 table A-12) */
constexpr uint8_t CT_FOCUS_ABSOLUTE = 0x06;
constexpr uint8_t CT_FOCUS_AUTO = 0x08;
constexpr uint8_t CT_ZOOM_ABSOLUTE = 0x0B;
constexpr uint8_t CT_PANTILT_ABSOLUTE = 0x0D;

/* Payload sizes. Pan and tilt share one 8 byte control; there is no way to set
 * just one axis. */
constexpr uint16_t PANTILT_LEN = 8;
constexpr uint16_t ZOOM_LEN = 2;
constexpr uint16_t FOCUS_LEN = 2;
constexpr uint16_t FOCUS_AUTO_LEN = 1;

inline void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = v & 0xff;
	p[1] = v >> 8;
}
inline void put_le32(uint8_t *p, int32_t v)
{
	uint32_t u = static_cast<uint32_t>(v);
	for (int i = 0; i < 4; i++)
		p[i] = (u >> (8 * i)) & 0xff;
}
inline uint16_t get_le16(const uint8_t *p)
{
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
inline int32_t get_le32(const uint8_t *p)
{
	uint32_t u = 0;
	for (int i = 0; i < 4; i++)
		u |= static_cast<uint32_t>(p[i]) << (8 * i);
	return static_cast<int32_t>(u);
}

/* wIndex for a control request: the entity id in the high byte and the
 * VideoControl interface number in the low byte. */
inline uint16_t control_index(uint8_t entity, uint8_t interface_number)
{
	return static_cast<uint16_t>((entity << 8) | interface_number);
}
inline uint16_t control_value(uint8_t selector)
{
	return static_cast<uint16_t>(selector << 8);
}

/* Where the camera's Camera Terminal lives: the VideoControl interface it is
 * described in, and its entity id. */
struct CameraTerminal {
	uint8_t interface_number;
	uint8_t terminal_id;
};

/* Walk a full configuration descriptor (config header included) and return the
 * first Camera Terminal (ITT_CAMERA) of the first VideoControl interface. */
inline std::optional<CameraTerminal> find_camera_terminal(const uint8_t *desc, size_t len)
{
	constexpr uint8_t DT_INTERFACE = 0x04;
	constexpr uint8_t DT_CS_INTERFACE = 0x24;
	constexpr uint8_t CC_VIDEO = 0x0E;
	constexpr uint8_t SC_VIDEOCONTROL = 0x01;
	constexpr uint8_t VC_INPUT_TERMINAL = 0x02;
	constexpr uint16_t ITT_CAMERA = 0x0201;

	bool in_video_control = false;
	uint8_t interface_number = 0;
	size_t off = 0;
	while (off + 2 <= len) {
		uint8_t length = desc[off];
		if (length < 2 || off + length > len)
			break;
		uint8_t type = desc[off + 1];
		if (type == DT_INTERFACE && length >= 9) {
			interface_number = desc[off + 2];
			in_video_control = desc[off + 5] == CC_VIDEO && desc[off + 6] == SC_VIDEOCONTROL;
		} else if (type == DT_CS_INTERFACE && in_video_control && length >= 8 &&
			   desc[off + 2] == VC_INPUT_TERMINAL && get_le16(desc + off + 4) == ITT_CAMERA) {
			return CameraTerminal{interface_number, desc[off + 3]};
		}
		off += length;
	}
	return std::nullopt;
}

/* What AVFoundation's uniqueID encodes for a USB camera. */
struct UsbDeviceId {
	uint32_t location_id;
	uint16_t vendor_id;
	uint16_t product_id;
};

/* AVCaptureDevice.uniqueID for a UVC camera is "0x" followed by the USB
 * locationID (unpadded hex), then the vendor and product ids as 4 hex digits
 * each, e.g. "0x1420000046d0825". Anything else (the built-in camera, camera
 * extensions and other virtual cameras have UUIDs) is not a USB id. */
inline std::optional<UsbDeviceId> parse_avfoundation_unique_id(const std::string &id)
{
	if (id.size() < 2 + 9 || id.size() > 2 + 16 || id[0] != '0' || (id[1] != 'x' && id[1] != 'X'))
		return std::nullopt;
	uint64_t v = 0;
	for (size_t i = 2; i < id.size(); i++) {
		char c = id[i];
		int digit;
		if (c >= '0' && c <= '9')
			digit = c - '0';
		else if (c >= 'a' && c <= 'f')
			digit = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			digit = c - 'A' + 10;
		else
			return std::nullopt;
		v = (v << 4) | static_cast<uint64_t>(digit);
	}
	return UsbDeviceId{static_cast<uint32_t>(v >> 32), static_cast<uint16_t>((v >> 16) & 0xffff),
			   static_cast<uint16_t>(v & 0xffff)};
}

} // namespace uvc
