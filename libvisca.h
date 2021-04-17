/*
 * VISCA(tm) Camera Control Library
 * Copyright (C) 2002 Damien Douxchamps 
 *
 * Written by Damien Douxchamps <ddouxchamps@users.sf.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef __LIBVISCA_H__
#define __LIBVISCA_H__

#if defined(_WIN32)||defined(WIN32)||defined(__WIN32__)||defined(_MSC_VER)
#  ifdef STATIC_LIB
#    define VISCA_API
#  else
#    ifdef DLL_EXPORTS
#      define VISCA_API __declspec(dllexport)
#    else
#      define VISCA_API  __declspec(dllimport)
#    endif
#  endif
#  define VISCA_WIN
#else
#  define VISCA_API
#  define VISCA_POSIX
#endif

#if DEBUG
#  if defined(VISCA_WIN)
#    define _VISCA_debug(msg, ...) _CrtDbgReport(_CRT_WARN, __FILE__, __LINE__, "libvisca", msg, __VA_ARGS__)
#  elif defined (__AVR__)
#    define _VISCA_debug(msg, ...) dbg_ReportStrP(PSTR(msg))
#  else
#    include <stdio.h>
#    define _VISCA_debug(msg, ...) fprintf(stderr, msg, __VA_ARGS__);
#  endif
#else
#  define _VISCA_debug(msg, ...) ;
#endif

/**********************/
/* Message formatting */
/**********************/

#define VISCA_COMMAND                    0x01
#define VISCA_INQUIRY                    0x09
#define VISCA_TERMINATOR                 0xFF

#define VISCA_CATEGORY_INTERFACE         0x00
#define VISCA_CATEGORY_CAMERA1           0x04
#define VISCA_CATEGORY_PAN_TILTER        0x06
#define VISCA_CATEGORY_CAMERA2           0x07

/* Known Vendor IDs */
#define VISCA_VENDOR_SONY    0x0020

/* Known Model IDs. The manual can be taken from 
 * http://www.sony.net/Products/ISP/docu_soft/index.html
 */
#define VISCA_MODEL_IX47x    0x0401          /* from FCB-IX47, FCB-IX470 instruction list */
#define VISCA_MODEL_EX47xL   0x0402          /* from FCB-EX47L, FCB-EX470L instruction list */
#define VISCA_MODEL_IX10     0x0404          /* FCB-IX10, FCB-IX10P instruction list */

#define VISCA_MODEL_EX780    0x0411          /* from EX780S(P) tech-manual */

#define VISCA_MODEL_EX480A   0x0412          /* from EX48A/EX480A tech-manual */
#define VISCA_MODEL_EX480AP  0x0413
#define VISCA_MODEL_EX48A    0x0414
#define VISCA_MODEL_EX48AP   0x0414
#define VISCA_MODEL_EX45M    0x041E
#define VISCA_MODEL_EX45MCE  0x041F

#define VISCA_MODEL_IX47A    0x0418          /* from IX47A tech-manual */
#define VISCA_MODEL_IX47AP   0x0419
#define VISCA_MODEL_IX45A    0x041A
#define VISCA_MODEL_IX45AP   0x041B

#define VISCA_MODEL_IX10A    0x041C          /* from IX10A tech-manual */
#define VISCA_MODEL_IX10AP   0x041D

#define VISCA_MODEL_EX780B   0x0420          /* from EX78/EX780 tech-manual */
#define VISCA_MODEL_EX780BP  0x0421
#define VISCA_MODEL_EX78B    0x0422
#define VISCA_MODEL_EX78BP   0x0423

#define VISCA_MODEL_EX480B   0x0424          /* from EX48/EX480 tech-manual */
#define VISCA_MODEL_EX480BP  0x0425
#define VISCA_MODEL_EX48B    0x0426
#define VISCA_MODEL_EX48BP   0x0427

#define VISCA_MODEL_EX980S   0x042E          /* from EX98/EX980 tech-manual */
#define VISCA_MODEL_EX980SP  0x042F
#define VISCA_MODEL_EX980    0x0430
#define VISCA_MODEL_EX980P   0x0431

#define VISCA_MODEL_H10      0x044A	     /* from H10 tech-manual */


/* Commands/inquiries codes */
#define VISCA_POWER                      0x00
#define VISCA_DEVICE_INFO                0x02
#define VISCA_KEYLOCK                    0x17
#define VISCA_ID                         0x22
#define VISCA_ZOOM                       0x07
#define   VISCA_ZOOM_STOP                  0x00
#define   VISCA_ZOOM_TELE                  0x02
#define   VISCA_ZOOM_WIDE                  0x03
#define   VISCA_ZOOM_TELE_SPEED            0x20
#define   VISCA_ZOOM_WIDE_SPEED            0x30
#define VISCA_ZOOM_VALUE                 0x47
#define VISCA_ZOOM_FOCUS_VALUE           0x47
#define VISCA_DZOOM                      0x06
#define   VISCA_DZOOM_OFF                  0x03
#define   VISCA_DZOOM_ON                   0x02
#define VISCA_DZOOM_LIMIT                0x26 /* implemented for H10 */
#define   VISCA_DZOOM_1X                   0x00
#define   VISCA_DZOOM_1_5X                 0x01
#define   VISCA_DZOOM_2X                   0x02
#define   VISCA_DZOOM_4X                   0x03
#define   VISCA_DZOOM_8X                   0x04
#define   VISCA_DZOOM_12X                  0x05
#define VISCA_DZOOM_MODE                 0x36
#define   VISCA_DZOOM_COMBINE              0x00
#define   VISCA_DZOOM_SEPARATE             0x01
#define VISCA_FOCUS                      0x08
#define   VISCA_FOCUS_STOP                 0x00
#define   VISCA_FOCUS_FAR                  0x02
#define   VISCA_FOCUS_NEAR                 0x03
#define   VISCA_FOCUS_FAR_SPEED            0x20
#define   VISCA_FOCUS_NEAR_SPEED           0x30
#define VISCA_FOCUS_VALUE                0x48
#define VISCA_FOCUS_AUTO                 0x38
#define   VISCA_FOCUS_AUTO_ON              0x02
#define   VISCA_FOCUS_AUTO_OFF             0x03
#define   VISCA_FOCUS_AUTO_MAN             0x10
#define VISCA_FOCUS_ONE_PUSH             0x18
#define   VISCA_FOCUS_ONE_PUSH_TRIG        0x01
#define   VISCA_FOCUS_ONE_PUSH_INF         0x02
#define VISCA_FOCUS_AUTO_SENSE           0x58
#define   VISCA_FOCUS_AUTO_SENSE_HIGH      0x02
#define   VISCA_FOCUS_AUTO_SENSE_LOW       0x03
#define VISCA_FOCUS_NEAR_LIMIT           0x28
#define VISCA_WB                         0x35
#define   VISCA_WB_AUTO                    0x00
#define   VISCA_WB_INDOOR                  0x01
#define   VISCA_WB_OUTDOOR                 0x02
#define   VISCA_WB_ONE_PUSH                0x03
#define   VISCA_WB_ATW                     0x04
#define   VISCA_WB_MANUAL                  0x05
#define VISCA_WB_TRIGGER                 0x10
#define   VISCA_WB_ONE_PUSH_TRIG           0x05
#define VISCA_RGAIN                      0x03
#define VISCA_RGAIN_VALUE                0x43
#define VISCA_BGAIN                      0x04
#define VISCA_BGAIN_VALUE                0x44
#define VISCA_AUTO_EXP                   0x39
#define   VISCA_AUTO_EXP_FULL_AUTO         0x00
#define   VISCA_AUTO_EXP_MANUAL            0x03
#define   VISCA_AUTO_EXP_SHUTTER_PRIORITY  0x0A
#define   VISCA_AUTO_EXP_IRIS_PRIORITY     0x0B
#define   VISCA_AUTO_EXP_GAIN_PRIORITY     0x0C
#define   VISCA_AUTO_EXP_BRIGHT            0x0D
#define   VISCA_AUTO_EXP_SHUTTER_AUTO      0x1A
#define   VISCA_AUTO_EXP_IRIS_AUTO         0x1B
#define   VISCA_AUTO_EXP_GAIN_AUTO         0x1C
#define VISCA_SLOW_SHUTTER               0x5A
#define   VISCA_SLOW_SHUTTER_AUTO          0x02
#define   VISCA_SLOW_SHUTTER_MANUAL        0x03
#define VISCA_SHUTTER                    0x0A
#define VISCA_SHUTTER_VALUE              0x4A
#define VISCA_IRIS                       0x0B
#define VISCA_IRIS_VALUE                 0x4B
#define VISCA_GAIN                       0x0C
#define VISCA_GAIN_VALUE                 0x4C
#define VISCA_BRIGHT                     0x0D
#define VISCA_BRIGHT_VALUE               0x4D
#define VISCA_EXP_COMP                   0x0E
#define VISCA_EXP_COMP_POWER             0x3E
#define VISCA_EXP_COMP_VALUE             0x4E
#define VISCA_BACKLIGHT_COMP             0x33
#define VISCA_SPOT_AE                    0x59
#define   VISCA_SPOT_AE_ON               0x02
#define   VISCA_SPOT_AE_OFF              0x03
#define VISCA_SPOT_AE_POSITION           0x29
#define VISCA_APERTURE                   0x02
#define VISCA_APERTURE_VALUE             0x42
#define VISCA_ZERO_LUX                   0x01
#define VISCA_IR_LED                     0x31
#define VISCA_WIDE_MODE                  0x60
#define   VISCA_WIDE_MODE_OFF              0x00
#define   VISCA_WIDE_MODE_CINEMA           0x01
#define   VISCA_WIDE_MODE_16_9             0x02
#define VISCA_MIRROR                     0x61
#define VISCA_FREEZE                     0x62
#define   VISCA_FREEZE_ON                  0x02
#define   VISCA_FREEZE_OFF                 0x03
#define VISCA_PICTURE_EFFECT             0x63
#define   VISCA_PICTURE_EFFECT_OFF         0x00
#define   VISCA_PICTURE_EFFECT_PASTEL      0x01
#define   VISCA_PICTURE_EFFECT_NEGATIVE    0x02
#define   VISCA_PICTURE_EFFECT_SEPIA       0x03
#define   VISCA_PICTURE_EFFECT_BW          0x04
#define   VISCA_PICTURE_EFFECT_SOLARIZE    0x05
#define   VISCA_PICTURE_EFFECT_MOSAIC      0x06
#define   VISCA_PICTURE_EFFECT_SLIM        0x07
#define   VISCA_PICTURE_EFFECT_STRETCH     0x08
#define VISCA_DIGITAL_EFFECT             0x64
#define   VISCA_DIGITAL_EFFECT_OFF         0x00
#define   VISCA_DIGITAL_EFFECT_STILL       0x01
#define   VISCA_DIGITAL_EFFECT_FLASH       0x02
#define   VISCA_DIGITAL_EFFECT_LUMI        0x03
#define   VISCA_DIGITAL_EFFECT_TRAIL       0x04
#define VISCA_DIGITAL_EFFECT_LEVEL       0x65
#define VISCA_CAM_STABILIZER             0x34
#define   VISCA_CAM_STABILIZER_ON         0x02
#define   VISCA_CAM_STABILIZER_OFF        0x03
#define VISCA_MEMORY                     0x3F
#define   VISCA_MEMORY_RESET               0x00
#define   VISCA_MEMORY_SET                 0x01
#define   VISCA_MEMORY_RECALL              0x02
#define     VISCA_MEMORY_0                   0x00
#define     VISCA_MEMORY_1                   0x01
#define     VISCA_MEMORY_2                   0x02
#define     VISCA_MEMORY_3                   0x03
#define     VISCA_MEMORY_4                   0x04
#define     VISCA_MEMORY_5                   0x05
#define     VISCA_MEMORY_CUSTOM              0x7F
#define VISCA_DISPLAY                    0x15
#define   VISCA_DISPLAY_ON                  0x02
#define   VISCA_DISPLAY_OFF                 0x03
#define   VISCA_DISPLAY_TOGGLE              0x10
#define VISCA_DATE_TIME_SET              0x70
#define VISCA_DATE_DISPLAY               0x71
#define VISCA_TIME_DISPLAY               0x72
#define VISCA_TITLE_DISPLAY              0x74
#define   VISCA_TITLE_DISPLAY_CLEAR        0x00
#define   VISCA_TITLE_DISPLAY_ON           0x02
#define   VISCA_TITLE_DISPLAY_OFF          0x03
#define VISCA_TITLE_SET                  0x73
#define   VISCA_TITLE_SET_PARAMS           0x00
#define   VISCA_TITLE_SET_PART1            0x01
#define   VISCA_TITLE_SET_PART2            0x02
#define VISCA_IRRECEIVE                   0x08
#define   VISCA_IRRECEIVE_ON              0x02
#define   VISCA_IRRECEIVE_OFF             0x03
#define   VISCA_IRRECEIVE_ONOFF           0x10
#define VISCA_PT_DRIVE                     0x01
#define   VISCA_PT_DRIVE_HORIZ_LEFT        0x01
#define   VISCA_PT_DRIVE_HORIZ_RIGHT       0x02
#define   VISCA_PT_DRIVE_HORIZ_STOP        0x03
#define   VISCA_PT_DRIVE_VERT_UP           0x01
#define   VISCA_PT_DRIVE_VERT_DOWN         0x02
#define   VISCA_PT_DRIVE_VERT_STOP         0x03
#define VISCA_PT_ABSOLUTE_POSITION         0x02
#define VISCA_PT_RELATIVE_POSITION         0x03
#define VISCA_PT_HOME                      0x04
#define VISCA_PT_RESET                     0x05
#define VISCA_PT_LIMITSET                  0x07
#define   VISCA_PT_LIMITSET_SET            0x00
#define   VISCA_PT_LIMITSET_CLEAR          0x01
#define     VISCA_PT_LIMITSET_SET_UR       0x01
#define     VISCA_PT_LIMITSET_SET_DL       0x00
#define VISCA_PT_DATASCREEN                0x06
#define   VISCA_PT_DATASCREEN_ON           0x02
#define   VISCA_PT_DATASCREEN_OFF          0x03
#define   VISCA_PT_DATASCREEN_ONOFF        0x10

#define VISCA_PT_VIDEOSYSTEM_INQ           0x23
#define VISCA_PT_MODE_INQ                  0x10
#define VISCA_PT_MAXSPEED_INQ              0x11
#define VISCA_PT_POSITION_INQ              0x12
#define VISCA_PT_DATASCREEN_INQ            0x06


/**************************/
/* DIRECT REGISTER ACCESS */
/**************************/

#define VISCA_REGISTER_VALUE              0x24

#define VISCA_REGISTER_VISCA_BAUD          0x00
#define VISCA_REGISTER_BD9600               0x00
#define VISCA_REGISTER_BD19200              0x01
#define VISCA_REGISTER_BD38400              0x02

/* FCB-H10: Video Standard */
#define VISCA_REGISTER_VIDEO_SIGNAL        0x70

#define VISCA_REGISTER_VIDEO_1080I_60       0x01
#define VISCA_REGISTER_VIDEO_720P_60        0x02
#define VISCA_REGISTER_VIDEO_D1_CROP_60     0x03
#define VISCA_REGISTER_VIDEO_D1_SQ_60       0x04

#define VISCA_REGISTER_VIDEO_1080I_50       0x11
#define VISCA_REGISTER_VIDEO_720P_50        0x12
#define VISCA_REGISTER_VIDEO_D1_CROP_50     0x13
#define VISCA_REGISTER_VIDEO_D1_SQ_50       0x14


/*****************/
/* D30/D31 CODES */
/*****************/
#define VISCA_WIDE_CON_LENS		   0x26
#define   VISCA_WIDE_CON_LENS_SET          0x00

#define VISCA_AT_MODE                      0x01
#define   VISCA_AT_ONOFF                   0x10
#define VISCA_AT_AE                        0x02
#define VISCA_AT_AUTOZOOM                  0x03
#define VISCA_ATMD_FRAMEDISPLAY            0x04
#define VISCA_AT_FRAMEOFFSET               0x05
#define VISCA_ATMD_STARTSTOP               0x06
#define VISCA_AT_CHASE                     0x07
#define   VISCA_AT_CHASE_NEXT              0x10

#define VISCA_MD_MODE                      0x08
#define   VISCA_MD_ONOFF                   0x10
#define VISCA_MD_FRAME                     0x09
#define VISCA_MD_DETECT                    0x0A

#define VISCA_MD_ADJUST                    0x00
#define   VISCA_MD_ADJUST_YLEVEL           0x0B
#define   VISCA_MD_ADJUST_HUELEVEL         0x0C
#define   VISCA_MD_ADJUST_SIZE             0x0D
#define   VISCA_MD_ADJUST_DISPTIME         0x0F
#define   VISCA_MD_ADJUST_REFTIME          0x0B
#define   VISCA_MD_ADJUST_REFMODE          0x10

#define VISCA_AT_ENTRY                     0x15
#define VISCA_AT_LOSTINFO                  0x20
#define VISCA_MD_LOSTINFO                  0x21
#define VISCA_ATMD_LOSTINFO1               0x20
#define VISCA_ATMD_LOSTINFO2               0x07

#define VISCA_MD_MEASURE_MODE_1            0x27
#define VISCA_MD_MEASURE_MODE_2            0x28

#define VISCA_ATMD_MODE                    0x22
#define VISCA_AT_MODE_QUERY                0x23
#define VISCA_MD_MODE_QUERY                0x24
#define VISCA_MD_REFTIME_QUERY             0x11
#define VISCA_AT_POSITION                  0x20
#define VISCA_MD_POSITION                  0x21

/***************/
/* ERROR CODES */
/***************/

/* these two are defined by me, not by the specs. */
#define VISCA_SUCCESS                    0x00
#define VISCA_FAILURE                    0xFF

/* specs errors: */
#define VISCA_ERROR_MESSAGE_LENGTH       0x01
#define VISCA_ERROR_SYNTAX               0x02
#define VISCA_ERROR_CMD_BUFFER_FULL      0x03
#define VISCA_ERROR_CMD_CANCELLED        0x04
#define VISCA_ERROR_NO_SOCKET            0x05
#define VISCA_ERROR_CMD_NOT_EXECUTABLE   0x41

/* Generic definitions */
#define VISCA_ON                         0x02
#define VISCA_OFF                        0x03
#define VISCA_RESET                      0x00
#define VISCA_UP                         0x02
#define VISCA_DOWN                       0x03

/* response types */
#define VISCA_RESPONSE_ADDRESS           0x30
#define VISCA_RESPONSE_ACK               0x40
#define VISCA_RESPONSE_COMPLETED         0x50
#define VISCA_RESPONSE_ERROR             0x60

#ifdef __cplusplus
extern "C" {
#endif

/* timeout in us */
#define VISCA_SERIAL_WAIT              100000

/* size of the local packet buffer */
#define VISCA_BUFFER_SIZE                  32

#ifdef VISCA_WIN

#include <windows.h>

#ifdef _MSC_VER
typedef unsigned __int8 uint8_t;
typedef unsigned __int16 uint16_t;
typedef unsigned __int32 uint32_t;
#  include <crtdbg.h>
#else
#  include <stdint.h>
#  ifndef _RPTF0
#    define _RPTF0(rptno,msg)
#  endif
#  ifndef _RPTF1
#    define _RPTF1(rptno,msg,arg1)
#  endif
#  ifndef _RPTF3
#    define _RPTF3(rptno,msg,arg1,arg2,arg3)
#  endif
#  ifndef _CRT_WARN
#    define _CRT_WARN
#  endif
#endif /* _MSC_VER */

#elif __AVR__

#include "v24.h"

#else /* POSIX */

#include <termios.h>
#include <stdint.h>
#include <unistd.h>

#endif

typedef struct _VISCA_packet
{
  unsigned char bytes[VISCA_BUFFER_SIZE];
  uint32_t length;
} VISCAPacket_t;

#define VISCA_PACKET_SENDER(pkt) (((pkt)->bytes[0] & 0x70) >> 4)
#define VISCA_PACKET_RECEIVER(pkt) ((pkt)->bytes[0] & 0x0F)
#define VISCA_PACKET_TYPE(pkt) ((pkt)->bytes[1] & 0xf0)
#define VISCA_PACKET_SOCKET(pkt) ((pkt)->bytes[1] & 0xf)

typedef struct _VISCA_interface
{
  // RS232 data:
#ifdef VISCA_WIN
  HANDLE port_fd;
#elif __AVR__
  v24_port_t port_fd;
#else /* POSIX */
  int port_fd;
#endif
  uint32_t baud;

  // VISCA data:
  uint8_t num_cameras;

  // RS232 input buffer and protocol state
  VISCAPacket_t ipacket;
  VISCAPacket_t *reply_packet;
  int busy; /* bool */
} VISCAInterface_t;

/* INTERFACE STRUCTURE -- this is only a forward declaration to the
 * structure. We declare a pointer to hide the platform specific code.
 */
typedef struct _VISCA_interface *pVISCAInterface_t;

/* CAMERA STRUCTURE */
typedef struct _VISCA_camera
{
  // VISCA data:
  int address;
  
  // camera info:
  uint32_t vendor;
  uint32_t model;
  uint32_t rom_version;
  uint32_t socket_num;
} VISCACamera_t;


/* TITLE STRUCTURE */
typedef struct _VISCA_title
{
  uint32_t vposition;
  uint32_t hposition;
  uint32_t color;
  uint32_t blink;
  unsigned char title[20];

} VISCATitleData_t;

/* GENERAL FUNCTIONS */

VISCA_API uint32_t
VISCA_set_address(VISCAInterface_t *iface, int *camera_num);

VISCA_API uint32_t
VISCA_clear(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_get_camera_info(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
_VISCA_write_packet_data(VISCAInterface_t *iface, VISCAPacket_t *packet);

VISCA_API uint32_t
_VISCA_send_packet(VISCAInterface_t *iface, VISCACamera_t *camera, VISCAPacket_t *packet);

VISCA_API uint32_t
_VISCA_get_packet(VISCAInterface_t *iface);

VISCA_API ssize_t
_VISCA_read_bytes(VISCAInterface_t *iface, unsigned char *read_buffer, size_t size);

VISCA_API uint32_t
VISCA_open_serial(VISCAInterface_t *iface, const char *device_name);

VISCA_API uint32_t
VISCA_unread_bytes(VISCAInterface_t *iface, unsigned char *buffer, uint32_t *buffer_size);

VISCA_API uint32_t
VISCA_close_serial(VISCAInterface_t *iface);

/* COMMANDS */

VISCA_API uint32_t
VISCA_set_power(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_get_info(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_keylock(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_camera_id(VISCAInterface_t *iface, VISCACamera_t *camera, uint16_t id);

VISCA_API uint32_t
VISCA_set_zoom_tele(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_zoom_wide(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_zoom_stop(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_zoom_tele_speed(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t speed);

VISCA_API uint32_t
VISCA_set_zoom_wide_speed(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t speed);

VISCA_API uint32_t
VISCA_set_zoom_value(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t zoom);

VISCA_API uint32_t
VISCA_set_zoom_and_focus_value(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t zoom, uint32_t focus);

VISCA_API uint32_t
VISCA_set_dzoom(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t power);

VISCA_API uint32_t
VISCA_set_dzoom_limit(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t limit);

VISCA_API uint32_t
VISCA_set_dzoom_mode(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t power);

VISCA_API uint32_t
VISCA_set_focus_far(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_focus_near(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_focus_stop(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_focus_far_speed(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t speed);

VISCA_API uint32_t
VISCA_set_focus_near_speed(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t speed);

VISCA_API uint32_t
VISCA_set_focus_value(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t focus);

VISCA_API uint32_t
VISCA_set_focus_auto(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_focus_one_push(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_focus_infinity(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_focus_autosense_high(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_focus_autosense_low(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_focus_near_limit(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t limit);

VISCA_API uint32_t
VISCA_set_whitebal_mode(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t mode);

VISCA_API uint32_t
VISCA_set_whitebal_one_push(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_rgain_up(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_rgain_down(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_rgain_reset(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_rgain_value(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t value);

VISCA_API uint32_t
VISCA_set_bgain_up(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_bgain_down(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_bgain_reset(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_bgain_value(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t value);

VISCA_API uint32_t
VISCA_set_shutter_up(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_shutter_down(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_shutter_reset(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_shutter_value(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t value);

VISCA_API uint32_t
VISCA_set_iris_up(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_iris_down(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_iris_reset(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_iris_value(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t value);

VISCA_API uint32_t
VISCA_set_gain_up(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_gain_down(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_gain_reset(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_gain_value(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t value);

VISCA_API uint32_t
VISCA_set_bright_up(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_bright_down(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_bright_reset(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_bright_value(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t value);

VISCA_API uint32_t
VISCA_set_aperture_up(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_aperture_down(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_aperture_reset(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_aperture_value(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t value);

VISCA_API uint32_t
VISCA_set_exp_comp_up(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_exp_comp_down(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_exp_comp_reset(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_exp_comp_value(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t value);

VISCA_API uint32_t
VISCA_set_exp_comp_power(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_auto_exp_mode(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t mode);

VISCA_API uint32_t
VISCA_set_slow_shutter_auto(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_backlight_comp(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_zero_lux_shot(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_ir_led(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_wide_mode(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t mode);

VISCA_API uint32_t
VISCA_set_mirror(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_freeze(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_picture_effect(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t mode);

VISCA_API uint32_t
VISCA_set_digital_effect(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t mode);

VISCA_API uint32_t
VISCA_set_digital_effect_level(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t level);

VISCA_API uint32_t
VISCA_memory_set(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t channel);

VISCA_API uint32_t
VISCA_memory_recall(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t channel);

VISCA_API uint32_t
VISCA_memory_reset(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t channel);

VISCA_API uint32_t
VISCA_set_display(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_date_time(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t year, uint32_t month, uint32_t day, uint32_t hour, uint32_t minute);

VISCA_API uint32_t
VISCA_set_date_display(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_time_display(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_title_display(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_title_clear(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_title_params(VISCAInterface_t *iface, VISCACamera_t *camera, VISCATitleData_t *title);

VISCA_API uint32_t
VISCA_set_title(VISCAInterface_t *iface, VISCACamera_t *camera, VISCATitleData_t *title);

VISCA_API uint32_t
VISCA_set_irreceive_on(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_irreceive_off(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_irreceive_onoff(VISCAInterface_t *iface, VISCACamera_t *camera);

/*  pan_speed should be in the range 01 - 18.
    tilt_speed should be in the range 01 - 14
    Negative speeds are LEFT for pan, and DOWN for tilt */

VISCA_API uint32_t
VISCA_set_pantilt(VISCAInterface_t *iface, VISCACamera_t *camera, int pan_speed, int tilt_speed);

VISCA_API uint32_t
VISCA_set_pantilt_up(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t pan_speed, uint32_t tilt_speed);

VISCA_API uint32_t
VISCA_set_pantilt_down(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t pan_speed, uint32_t tilt_speed);

VISCA_API uint32_t
VISCA_set_pantilt_left(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t pan_speed, uint32_t tilt_speed);

VISCA_API uint32_t
VISCA_set_pantilt_right(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t pan_speed, uint32_t tilt_speed);

VISCA_API uint32_t
VISCA_set_pantilt_upleft(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t pan_speed, uint32_t tilt_speed);

VISCA_API uint32_t
VISCA_set_pantilt_upright(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t pan_speed, uint32_t tilt_speed);

VISCA_API uint32_t
VISCA_set_pantilt_downleft(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t pan_speed, uint32_t tilt_speed);

VISCA_API uint32_t
VISCA_set_pantilt_downright(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t pan_speed, uint32_t tilt_speed);

VISCA_API uint32_t
VISCA_set_pantilt_stop(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t pan_speed, uint32_t tilt_speed);

/*  pan_speed should be in the range 01 - 18.
    tilt_speed should be in the range 01 - 14
    pan_position should be in the range -880 - 880 (0xFC90 - 0x370) 
    tilt_position should be in range -300 - 300 (0xFED4 - 0x12C)  */
VISCA_API uint32_t
VISCA_set_pantilt_absolute_position(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t pan_speed, uint32_t tilt_speed, int pan_position, int tilt_position);

VISCA_API uint32_t
VISCA_set_pantilt_relative_position(VISCAInterface_t *iface, VISCACamera_t *camera, uint32_t pan_speed, uint32_t tilt_speed, int pan_position, int tilt_position);

VISCA_API uint32_t
VISCA_set_pantilt_home(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_pantilt_reset(VISCAInterface_t *iface, VISCACamera_t *camera);

/*  pan_limit should be in the range -880 - 880 (0xFC90 - 0x370) 
    tilt_limit should be in range -300 - 300 (0xFED4 - 0x12C)  */
VISCA_API uint32_t
VISCA_set_pantilt_limit_upright(VISCAInterface_t *iface, VISCACamera_t *camera, int pan_limit, int tilt_limit);

VISCA_API uint32_t
VISCA_set_pantilt_limit_downleft(VISCAInterface_t *iface, VISCACamera_t *camera, int pan_limit, int tilt_limit);

VISCA_API uint32_t
VISCA_set_pantilt_limit_downleft_clear(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_pantilt_limit_upright_clear(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_datascreen_on(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_datascreen_off(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_datascreen_onoff(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_spot_ae_on(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_spot_ae_off(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_spot_ae_position(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t x_position, uint8_t y_position);

/* INQUIRIES */

VISCA_API uint32_t
VISCA_get_power(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *power);

VISCA_API uint32_t
VISCA_get_dzoom(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *power);

VISCA_API uint32_t
VISCA_get_dzoom_limit(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *value);

VISCA_API uint32_t
VISCA_get_zoom_value(VISCAInterface_t *iface, VISCACamera_t *camera, uint16_t *value);

VISCA_API uint32_t
VISCA_get_focus_auto(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *power);

VISCA_API uint32_t
VISCA_get_focus_value(VISCAInterface_t *iface, VISCACamera_t *camera, uint16_t *value);

VISCA_API uint32_t
VISCA_get_focus_auto_sense(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *mode);

VISCA_API uint32_t
VISCA_get_focus_near_limit(VISCAInterface_t *iface, VISCACamera_t *camera, uint16_t *value);

VISCA_API uint32_t
VISCA_get_whitebal_mode(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *mode);

VISCA_API uint32_t
VISCA_get_rgain_value(VISCAInterface_t *iface, VISCACamera_t *camera, uint16_t *value);

VISCA_API uint32_t
VISCA_get_bgain_value(VISCAInterface_t *iface, VISCACamera_t *camera, uint16_t *value);

VISCA_API uint32_t
VISCA_get_auto_exp_mode(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *mode);

VISCA_API uint32_t
VISCA_get_slow_shutter_auto(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *mode);

VISCA_API uint32_t
VISCA_get_shutter_value(VISCAInterface_t *iface, VISCACamera_t *camera, uint16_t *value);

VISCA_API uint32_t
VISCA_get_iris_value(VISCAInterface_t *iface, VISCACamera_t *camera, uint16_t *value);

VISCA_API uint32_t
VISCA_get_gain_value(VISCAInterface_t *iface, VISCACamera_t *camera, uint16_t *value);

VISCA_API uint32_t
VISCA_get_bright_value(VISCAInterface_t *iface, VISCACamera_t *camera, uint16_t *value);

VISCA_API uint32_t
VISCA_get_exp_comp_power(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *power);

VISCA_API uint32_t
VISCA_get_exp_comp_value(VISCAInterface_t *iface, VISCACamera_t *camera, uint16_t *value);

VISCA_API uint32_t
VISCA_get_backlight_comp(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *power);

VISCA_API uint32_t
VISCA_get_aperture_value(VISCAInterface_t *iface, VISCACamera_t *camera, uint16_t *value);

VISCA_API uint32_t
VISCA_get_zero_lux_shot(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *power);

VISCA_API uint32_t
VISCA_get_ir_led(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *power);

VISCA_API uint32_t
VISCA_get_wide_mode(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *mode);

VISCA_API uint32_t
VISCA_get_mirror(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *power);

VISCA_API uint32_t
VISCA_get_freeze(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *power);

VISCA_API uint32_t
VISCA_get_picture_effect(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *mode);

VISCA_API uint32_t
VISCA_get_digital_effect(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *mode);

VISCA_API uint32_t
VISCA_get_digital_effect_level(VISCAInterface_t *iface, VISCACamera_t *camera, uint16_t *value);

VISCA_API uint32_t
VISCA_get_memory(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *channel);

VISCA_API uint32_t
VISCA_get_display(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *power);

VISCA_API uint32_t
VISCA_get_id(VISCAInterface_t *iface, VISCACamera_t *camera, uint16_t *id);

VISCA_API uint32_t
VISCA_get_videosystem(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *system);

VISCA_API uint32_t
VISCA_get_pantilt_mode(VISCAInterface_t *iface, VISCACamera_t *camera, uint16_t *status);

VISCA_API uint32_t
VISCA_get_pantilt_maxspeed(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *max_pan_speed, uint8_t *max_tilt_speed);

VISCA_API uint32_t
VISCA_get_pantilt_position(VISCAInterface_t *iface, VISCACamera_t *camera, int *pan_position, int *tilt_position);

VISCA_API uint32_t
VISCA_get_datascreen(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *status);

/* SPECIAL FUNCTIONS FOR D30/31 */
VISCA_API uint32_t
VISCA_set_wide_con_lens(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_at_mode_onoff(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_at_mode(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_at_ae_onoff(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_at_ae(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_at_autozoom_onoff(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_at_autozoom(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_atmd_framedisplay_onoff(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_atmd_framedisplay(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_at_frameoffset_onoff(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_at_frameoffset(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_atmd_startstop(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_at_chase(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_at_chase_next(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_md_mode_onoff(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_md_mode(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_md_frame(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_md_detect(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_at_entry(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_at_lostinfo(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_md_lostinfo(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_md_adjust_ylevel(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_md_adjust_huelevel(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_md_adjust_size(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_md_adjust_disptime(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_md_adjust_refmode(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_md_adjust_reftime(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_md_measure_mode1_onoff(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_md_measure_mode1(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_set_md_measure_mode2_onoff(VISCAInterface_t *iface, VISCACamera_t *camera);

VISCA_API uint32_t
VISCA_set_md_measure_mode2(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t power);

VISCA_API uint32_t
VISCA_get_keylock(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *power);

VISCA_API uint32_t
VISCA_get_wide_con_lens(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *power);

VISCA_API uint32_t
VISCA_get_atmd_mode(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *power);

VISCA_API uint32_t
VISCA_get_at_mode(VISCAInterface_t *iface, VISCACamera_t *camera, uint16_t *value);

VISCA_API uint32_t
VISCA_get_at_entry(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *power);

VISCA_API uint32_t
VISCA_get_md_mode(VISCAInterface_t *iface, VISCACamera_t *camera, uint16_t *value);

VISCA_API uint32_t
VISCA_get_md_ylevel(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *power);

VISCA_API uint32_t
VISCA_get_md_huelevel(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *power);

VISCA_API uint32_t
VISCA_get_md_size(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *power);

VISCA_API uint32_t
VISCA_get_md_disptime(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *power);

VISCA_API uint32_t
VISCA_get_md_refmode(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *power);

VISCA_API uint32_t
VISCA_get_md_reftime(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *power);

VISCA_API uint32_t
VISCA_get_at_obj_pos(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *xpos, uint8_t *ypos, uint8_t *status);

VISCA_API uint32_t
VISCA_get_md_obj_pos(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t *xpos, uint8_t *ypos, uint8_t *status);


VISCA_API uint32_t
VISCA_set_register(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t reg_num, uint8_t reg_val);

VISCA_API uint32_t
VISCA_get_register(VISCAInterface_t *iface, VISCACamera_t *camera, uint8_t reg_num, uint8_t* reg_val);

/* Utility */
VISCA_API uint32_t
VISCA_usleep(uint32_t useconds);

#ifdef __cplusplus
} /* closing brace for extern "C" */
#endif

#endif /* __LIBVISCA_H__ */
