/*
 * Copyright (C) 2013 Alexey Galakhov <agalakhov@gmail.com>
 *
 * Licensed under the GNU General Public License Version 3
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _XOPEN_SOURCE   600
#define _POSIX_C_SOURCE 200112L

#include "capt-command.h"

#include "std.h"
#include "word.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <unistd.h>

#include <cups/cups.h>
#include <cups/sidechannel.h>

static uint8_t capt_iobuf[0x10000];
static size_t  capt_iosize;

/* CUPS connects the filter's backchannel on descriptor 3. */
#define CAPT_BACKCHANNEL_FD 3

static void capt_debug_buf(const char *level, size_t size)
{
	size_t i;
	if (size > capt_iosize)
		size = capt_iosize;
	for (i = 0; i < size; ++i) {
		if (i != 0 && (i % 16) == 0)
			fprintf(stderr, "\n%s: CAPT:", level);
		fprintf(stderr, " %02X", capt_iobuf[i]);
	}
	if (size < capt_iosize)
		fprintf(stderr, "... (%u more)", (unsigned) (capt_iosize - size));
	fprintf(stderr, "\n");
}

static void capt_send_buf(void)
{
	const uint8_t *iopos = capt_iobuf;
	size_t iosize = capt_iosize;

	if (debug) {
		fprintf(stderr, "DEBUG: CAPT: send ");
		capt_debug_buf("DEBUG", 128);
	}

	while (iosize) {
		cups_sc_status_t status;
		uint8_t tmpbuf[128];
		int tmpsize = sizeof(tmpbuf);
		size_t sendsize = iosize;
		if (sendsize > 4096)
			sendsize = 4096;

		if (fwrite(iopos, 1, sendsize, stdout) != sendsize || fflush(stdout)) {
			fprintf(stderr, "ERROR: CAPT: USB backend closed the print stream\n");
			exit(1);
		}
		iopos += sendsize;
		iosize -= sendsize;

		status = cupsSideChannelDoRequest(CUPS_SC_CMD_DRAIN_OUTPUT,
				(char *) tmpbuf, &tmpsize,
#ifdef __APPLE__
				10.0);
#else
				1.0);
		if (status == CUPS_SC_STATUS_TIMEOUT) {
			/* Older USB backends can miss a drain request on an empty pipe. */
			fprintf(stderr, "DEBUG: CAPT: output already empty, not drained\n");
			continue;
		}
#endif
		if (status != CUPS_SC_STATUS_OK) {
			fprintf(stderr, "ERROR: CAPT: no reply from backend, err=%i\n", (int) status);
			exit(1);
		}
	}
}

static double monotonic_seconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec + now.tv_nsec / 1e9;
}

static bool capt_read_to(size_t end, double deadline)
{
	while (capt_iosize < end) {
		double remaining = deadline - monotonic_seconds();
		if (remaining <= 0)
			return false;
		/* macOS libcups can block indefinitely on sub-second backchannel
		 * timeouts. Poll the descriptor with the remaining deadline. */
		struct pollfd backchannel = { CAPT_BACKCHANNEL_FD, POLLIN, 0 };
		int ready = poll(&backchannel, 1, (int)(remaining * 1000) + 1);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready <= 0)
			return false;
		ssize_t size = read(CAPT_BACKCHANNEL_FD, capt_iobuf + capt_iosize, end - capt_iosize);
		if (size < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (size <= 0)
			return false;
		capt_iosize += (size_t)size;
	}
	return true;
}

const char *capt_identify(void)
{
	while (1) {
		cups_sc_status_t status;
		int size = sizeof(capt_iobuf) - 1;
		status = cupsSideChannelDoRequest(CUPS_SC_CMD_GET_DEVICE_ID,
				(char *) capt_iobuf, &size, 60.0);
		if (status != CUPS_SC_STATUS_OK) {
			fprintf(stderr, "ERROR: CAPT: unable to communicate with printer\n");
			exit(1);
		}
		capt_iobuf[size] = '\0';
		fprintf(stderr, "DEBUG: CAPT: printer ID string %s\n", capt_iobuf);
		if (size)
			return (const char*) capt_iobuf;
		usleep(50000);
	}
}

static void capt_copy_cmd(uint16_t cmd, const void *buf, size_t size)
{
	if (capt_iosize + 4 + size > sizeof(capt_iobuf)) {
		fprintf(stderr, "ALERT: bug in CAPT driver, output buffer overflow\n");
		exit(1);
	}
	if (buf)
		memcpy(capt_iobuf + capt_iosize + 4, buf, size);
	else
		size = 0;
	capt_iobuf[capt_iosize + 0] = LO(cmd);
	capt_iobuf[capt_iosize + 1] = HI(cmd);
	capt_iobuf[capt_iosize + 2] = LO(size + 4);
	capt_iobuf[capt_iosize + 3] = HI(size + 4);
	capt_iosize += size + 4;
}

void capt_send(uint16_t cmd, const void *buf, size_t size)
{
	capt_iosize = 0;
	capt_copy_cmd(cmd, buf, size);
	capt_send_buf();
}

void capt_sendrecv(uint16_t cmd, const void *buf, size_t size, void *reply, size_t *reply_size)
{
	capt_send(cmd, buf, size);
	double deadline = monotonic_seconds() + 15.0;
	capt_iosize = 0;
	if (!capt_read_to(6, deadline))
		goto incomplete;

	/* The first command in a job can encounter a tail from the prior job.
	 * Only search for a new header here, before sending any print data. */
	unsigned discarded = 0;
	while (cmd == CAPT_GET_PRINTER_INFO && discarded < 256 &&
	       (WORD(capt_iobuf[0], capt_iobuf[1]) != cmd || WORD(capt_iobuf[2], capt_iobuf[3]) < 6)) {
		memmove(capt_iobuf, capt_iobuf + 1, 5);
		capt_iosize = 5;
		if (!capt_read_to(6, deadline))
			goto incomplete;
		++discarded;
	}
	if (WORD(capt_iobuf[0], capt_iobuf[1]) != cmd || WORD(capt_iobuf[2], capt_iobuf[3]) < 6) {
		fprintf(stderr, "ERROR: CAPT: bad reply from printer, "
				"expected %02X %02X xx xx xx xx, got", LO(cmd), HI(cmd));
		capt_debug_buf("ERROR", 6);
		exit(1);
	}
	if (discarded)
		fprintf(stderr, "DEBUG: CAPT: discarded %u stale USB reply bytes\n", discarded);

	size_t binary = WORD(capt_iobuf[2], capt_iobuf[3]);
	size_t bcd = BCD(capt_iobuf[2], capt_iobuf[3]);
	if (bcd >= 6 && bcd < binary) {
		/* Some firmware encodes the length in BCD. Look briefly for another
		 * byte at that boundary before accepting the shorter reply. */
		if (!capt_read_to(bcd, deadline))
			goto incomplete;
		double probe = monotonic_seconds() + 0.35;
		if (!capt_read_to(bcd + 1, probe < deadline ? probe : deadline))
			binary = bcd;
	}
	if (!capt_read_to(binary, deadline))
		goto incomplete;
	if (debug) {
		fprintf(stderr, "DEBUG: CAPT: recv ");
		capt_debug_buf("DEBUG", capt_iosize);
	}
	size_t payload_size = capt_iosize - 4;
	if (reply) {
		size_t copysize = reply_size && *reply_size < payload_size ? *reply_size : payload_size;
		memcpy(reply, capt_iobuf + 4, copysize);
	}
	if (reply_size)
		*reply_size = payload_size;
	return;

incomplete:
	fprintf(stderr, "ERROR: CAPT: incomplete reply for command 0x%04x (%zu bytes read)\n", cmd, capt_iosize);
	exit(1);
}

void capt_multi_begin(uint16_t cmd)
{
	capt_iobuf[0] = LO(cmd);
	capt_iobuf[1] = HI(cmd);
	capt_iosize = 4;
}

void capt_multi_add(uint16_t cmd, const void *buf, size_t size)
{
	capt_copy_cmd(cmd, buf, size);
}

void capt_multi_send(void)
{
	capt_iobuf[2] = LO(capt_iosize);
	capt_iobuf[3] = HI(capt_iosize);
	capt_send_buf();
}

