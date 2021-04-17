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
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* implemented in libvisca.c */
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
	int err;

#ifdef DEBUG
	int i;
	fprintf(stderr, "Sending packet length:%i data:", packet->length);
	for (i = 0; i < packet->length; i++)
		fprintf(stderr, " %.2x", packet->bytes[i]);
	fprintf(stderr, "\n");
#endif

	err = write(iface->port_fd, packet->bytes, packet->length);
	if ( err < (int) packet->length )
		return VISCA_FAILURE;
	return VISCA_SUCCESS;
}

ssize_t _VISCA_read_bytes(VISCAInterface_t *iface, unsigned char *read_buffer, size_t size)
{
	return read(iface->port_fd, read_buffer, size);
}

/***********************************/
/*       SYSTEM  FUNCTIONS         */
/***********************************/

uint32_t
VISCA_open_serial(VISCAInterface_t *iface, const char *device_name)
{
	struct termios options = {};
	int fd;

	iface->reply_packet = NULL;
	iface->ipacket.length = 0;
	iface->busy = 0;

	fd = open(device_name, O_RDWR | O_NDELAY | O_NOCTTY);

	if (fd == -1) {
#if DEBUG
		fprintf(stderr,"(%s): cannot open serial device %s\n",__FILE__,device_name);
#endif
		iface->port_fd=-1;
		return VISCA_FAILURE;
	}

	fcntl(fd, F_SETFL,0);
	/* Setting port parameters */
	tcgetattr(fd, &options);

	/* control flags */
	cfsetispeed(&options,B9600);    /* 9600 Bds   */
	options.c_cflag &= ~PARENB;     /* No parity  */
	options.c_cflag &= ~CSTOPB;     /*            */
	options.c_cflag &= ~CSIZE;      /* 8bit       */
	options.c_cflag |= CS8;         /*            */
	options.c_cflag &= ~CRTSCTS;    /* No hdw ctl */

	/* local flags */
	options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); /* raw input */

	/* input flags
	 *
	 * options.c_iflag &= ~(INPCK | ISTRIP); // no parity
	 * options.c_iflag &= ~(IXON | IXOFF | IXANY); // no soft ctl
	 */
	/* patch: bpflegin: set to 0 in order to avoid invalid pan/tilt return values */
	options.c_iflag = 0;

	/* output flags */
	options.c_oflag &= ~OPOST; /* raw output */

	/* Timeout parameters */
	options.c_cc[VMIN] = 0; /* Return immediately if any data available */
	options.c_cc[VTIME] = (2000 + 99) / 100; // VTIME is 1/10s

	tcsetattr(fd, TCSANOW, &options);

	iface->port_fd = fd;

	return VISCA_SUCCESS;
}

uint32_t
VISCA_unread_bytes(VISCAInterface_t *iface, unsigned char *buffer, uint32_t *buffer_size)
{
	uint32_t bytes = 0;
	*buffer_size = 0;

	ioctl(iface->port_fd, FIONREAD, &bytes);
	if (bytes > 0) {
		bytes = (bytes>*buffer_size) ? *buffer_size : bytes;
		read(iface->port_fd, &buffer, bytes);
		*buffer_size = bytes;
		return VISCA_FAILURE;
	}
	return VISCA_SUCCESS;
}

uint32_t
VISCA_close_serial(VISCAInterface_t *iface)
{
	if (iface->port_fd != -1) {
		close(iface->port_fd);
		iface->port_fd = -1;
		return VISCA_SUCCESS;
	}
	return VISCA_FAILURE;
}

uint32_t
VISCA_usleep(uint32_t useconds)
{
	return (uint32_t) usleep(useconds);
}
