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

#include "libvisca.h"

#include <windows.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

/* implemented in libvisca.c
 */
void _VISCA_append_byte(VISCAPacket_t *packet, unsigned char byte);
void _VISCA_init_packet(VISCAPacket_t *packet);
unsigned int _VISCA_get_reply(VISCAInterface_t *iface, VISCACamera_t *camera);
unsigned int _VISCA_send_packet_with_reply(VISCAInterface_t *iface, VISCACamera_t *camera, VISCAPacket_t *packet);

/* Implementation of the platform specific code. The following functions must
 * be implemented here:
 *
 * unsigned int _VISCA_write_packet_data(VISCAInterface_t *iface, VISCAPacket_t *packet);
 * unsigned int _VISCA_get_byte(VISCAInterface_t *iface, unsigned char *buffer);
 * unsigned int VISCA_open_serial(VISCAInterface_t *iface, const char *device_name);
 * unsigned int VISCA_close_serial(VISCAInterface_t *iface);
 *
 */

uint32_t
_VISCA_write_packet_data(VISCAInterface_t *iface, VISCAPacket_t *packet)
{
	DWORD iBytesWritten;
	BOOL rVal = 0;
	DWORD errors;
	COMSTAT stat;
	int nTrials;

	for (nTrials = 0; nTrials < 3 && rVal == 0; nTrials++) {
		if (nTrials > 0)
			ClearCommError(iface->port_fd, &errors, &stat);
		rVal = WriteFile(iface->port_fd, &packet->bytes, packet->length, &iBytesWritten, NULL);
	}

	if (iBytesWritten < packet->length) {
		return VISCA_FAILURE;
	}
	return VISCA_SUCCESS;
}


ssize_t
_VISCA_read_bytes(VISCAInterface_t *iface, unsigned char *buffer, size_t size)
{
	BOOL rc;
	DWORD iBytesRead;

	rc = ReadFile(iface->port_fd, buffer, 1, &iBytesRead, NULL);
	if (!rc) {
		_RPTF0(_CRT_WARN,"ReadFile failed.\n");
		return -1;
	}
	return iBytesRead;
}


/***********************************/
/*       SYSTEM  FUNCTIONS         */
/***********************************/

uint32_t
VISCA_open_serial(VISCAInterface_t *iface, const char *device_name)
{
	DCB      m_dcb;
	COMMTIMEOUTS cto;

	iface->reply_packet = NULL;
	iface->ipacket.length = 0;
	iface->busy = 0;

	iface->port_fd = CreateFile(device_name,
			    GENERIC_READ | GENERIC_WRITE,
			    0, // exclusive access
			    NULL, // no security
			    OPEN_EXISTING,
			    0, // no overlapped I/O
			    NULL); // null template

	// Check the returned handle for INVALID_HANDLE_VALUE and then set the buffer sizes.
	if (iface->port_fd == INVALID_HANDLE_VALUE) {
		_RPTF1(_CRT_WARN,"cannot open serial device %s\n", device_name);
		goto err_setup;
	}

	if (!SetupComm(iface->port_fd, 4, 4)) { // set buffer sizes
		_RPTF1(_CRT_WARN,"SetupComm() failed on device %s\n", device_name);
		goto err_setup;
	}

	// Port settings are specified in a Data Communication Block (DCB). The
	// easiest way to initialize a DCB is to call GetCommState to fill in
	// its default values, override the values that you want to change and
	// then call SetCommState to set the values.
	if (!GetCommState(iface->port_fd, &m_dcb)) {
		_RPTF1(_CRT_WARN,"GetCommState() failed on device %s\n", device_name);
		goto err_setup;
	}

	m_dcb.BaudRate = 9600;
	m_dcb.ByteSize = 8;
	m_dcb.Parity = NOPARITY;
	m_dcb.StopBits = ONESTOPBIT;
	m_dcb.fAbortOnError = TRUE;

	// =========================================
	// (jd) added to get a complete setup...
	m_dcb.fOutxCtsFlow = FALSE;                // Disable CTS monitoring
	m_dcb.fOutxDsrFlow = FALSE;                // Disable DSR monitoring
	m_dcb.fDtrControl = DTR_CONTROL_DISABLE;   // Disable DTR monitoring
	m_dcb.fOutX = FALSE;                       // Disable XON/XOFF for transmission
	m_dcb.fInX = FALSE;                        // Disable XON/XOFF for receiving
	m_dcb.fRtsControl = RTS_CONTROL_DISABLE;   // Disable RTS (Ready To Send)
	m_dcb.fBinary = TRUE;                      // muss immer "TRUE" sein!
	m_dcb.fErrorChar = FALSE;
	m_dcb.fNull = FALSE;

	if (!SetCommState(iface->port_fd, &m_dcb)) {
		_RPTF1(_CRT_WARN,"GetCommState() failed on device %s\n", device_name);
		goto err_setup;
	}

	// =========================================
	// (jd) set timeout
	if (!GetCommTimeouts(iface->port_fd,&cto)) {
		// Obtain the error code
		//m_lLastError = ::GetLastError();
		_RPTF0(_CRT_WARN,"unable to obtain timeout information\n");
		goto err_setup;
	}

	/* Setting interval and multiplier to 0 ensures received bytes are
	 * returned immediately, but ReadFile() call will wait up to
	 * 2 seconds for bytes to arrive if the buffer is empty */
	cto.ReadIntervalTimeout = 0;
	cto.ReadTotalTimeoutConstant = 2000;
	cto.ReadTotalTimeoutMultiplier = 0;
	cto.WriteTotalTimeoutMultiplier = 500;
	cto.WriteTotalTimeoutConstant = 1000;
	if (!SetCommTimeouts(iface->port_fd,&cto)) {
		_RPTF0(_CRT_WARN,"unable to setup timeout information\n");
		goto err_setup;
	}

	// =========================================
	// If all of these API's were successful then the port is ready for use.
	return VISCA_SUCCESS;

 err_setup:
	VISCA_close_serial(iface);
	return VISCA_FAILURE;
}

uint32_t
VISCA_unread_bytes(VISCAInterface_t *iface, unsigned char *buffer, uint32_t *buffer_size)
{
	// TODO
	*buffer_size = 0;
	return VISCA_SUCCESS;
}

uint32_t
VISCA_close_serial(VISCAInterface_t *iface)
{
	if (iface->port_fd != NULL) {
		CloseHandle(iface->port_fd);
		iface->port_fd = NULL;
		return VISCA_SUCCESS;
	}
	return VISCA_FAILURE;
}

uint32_t
VISCA_usleep(uint32_t useconds)
{
	uint32_t microsecs = useconds / 1000;
	Sleep (microsecs);
	return 0;
}

