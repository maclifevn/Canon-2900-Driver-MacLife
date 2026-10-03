/*
 * Copyright (C) 2013 Alexey Galakhov <agalakhov@gmail.com>
 * Copyright (C) 2016 Alexei Gordeev <KP1533TM2@gmail.com>
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

#include "std.h"
#include "word.h"
#include "capt-command.h"
#include "capt-status.h"
#include "generic-ops.h"
#include "hiscoa-common.h"
#include "hiscoa-compress.h"
#include "paper.h"
#include "printer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

uint16_t job;

struct printer_gpio_s {
	const uint8_t (*init);
	const uint8_t (*blink);
};

struct lbp2900_ops_s {
	struct printer_ops_s ops;
	struct printer_gpio_s gpio;

	const struct capt_status_s * (*get_status) (void);
	void (*wait_ready) (void);
};

static const uint8_t magicbuf_0[] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const uint8_t magicbuf_2[] = {
	0xEE, 0xDB, 0xEA, 0xAD, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* Linux GoOnline payload: 8 bytes only (Windows uses 16) — protocol §2.12 */
static const uint8_t magicbuf_linux_online[] = {
	0xEE, 0xDB, 0xEA, 0xAD, 0x00, 0x00, 0x00, 0x00
};

static const uint8_t lbp2900_gpio_blink[] = {
	0x00, 0x00, 0x01, 0x02, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x01, 0x00,
};

static const uint8_t lbp2900_gpio_init[] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
};

static const uint8_t lbp3000_job_init[] = {
	0x00, 0x00,
};

static const uint8_t lbp3010_gpio_blink[] = {
        /* led */ 0x31, 0x00, 0x00, /* S6 */ 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, /* S7 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t lbp3010_gpio_init[] = {
        /* led */ 0x13, 0x00, 0x00, /* S6 */ 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, /* S7 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t lbp6000_job_init[] = {
        0x01, 0x00,
};

static const struct capt_status_s *lbp2900_get_status(const struct printer_ops_s *ops)
{
	const struct lbp2900_ops_s *lops = container_of(ops, struct lbp2900_ops_s, ops);
	return lops->get_status();
}

static void lbp2900_wait_ready(const struct printer_ops_s *ops)
{
	const struct lbp2900_ops_s *lops = container_of(ops, struct lbp2900_ops_s, ops);
	lops->wait_ready();
}

/*
 * Parse GetPrinterInfo reply and store Blk/Buf in printer state.
 * Per protocol §2.1 and §9 note 1:
 *   Blk = bytes 7-8 of the reply body (0-indexed: bytes 6-7), uint16 LE.
 *   Buf = bytes 9-10 of the reply body (0-indexed: bytes 8-9), uint16 LE.
 * LBP3000 defaults: Blk=0xfff0 (65520), Buf=0x0040 (64).
 */
static void capt_get_printer_info(struct printer_state_s *state)
{
	uint8_t buf[32];
	size_t size = sizeof(buf);
	capt_sendrecv(CAPT_GET_PRINTER_INFO, NULL, 0, buf, &size);
	if (size >= 10) {
		state->printer_blk = WORD(buf[6], buf[7]);
		state->printer_buf = WORD(buf[8], buf[9]);
		fprintf(stderr, "DEBUG: CAPT: GetPrinterInfo Blk=0x%04x (%u) Buf=0x%04x (%u)\n",
			state->printer_blk, state->printer_blk,
			state->printer_buf, state->printer_buf);
	} else {
		/* Default values for LBP3000 if reply is too short */
		state->printer_blk = 65520;
		state->printer_buf = 64;
		fprintf(stderr, "DEBUG: CAPT: GetPrinterInfo reply too short (%u), using defaults\n",
			(unsigned)size);
	}
}

static void send_job_start(uint8_t fg)
{
	/* host/user/document name lengths: all zero (no strings appended) */
	uint8_t ml = 0x00;
	uint8_t ul = 0x00;
	uint8_t nl = 0x00;

	time_t rawtime = time(NULL);
	const struct tm *tm = localtime(&rawtime);

	/* TZOffset: protocol uses seconds-west-of-UTC / 60.
	 * `timezone` (from <time.h>) is seconds WEST of UTC (set by localtime()).
	 * For Moscow UTC+3: timezone = -10800 → TZOffset = -180 = 0xFF4C LE.
	 * Both TZOffset (bytes 20-21) and TZOffset2 (bytes 22-23) must be equal. */
	int16_t tz = (int16_t)(timezone / 60);

	/* Year must be 1900 + tm_year (e.g. 2026).
	 * Month must be 1-indexed (1=Jan … 12=Dec). */
	uint16_t year = (uint16_t)(tm->tm_year + 1900);
	uint8_t month = (uint8_t)(tm->tm_mon + 1);

	/* Fixed 72-byte payload per protocol §2.7:
	 * bytes  0- 7: hard-coded zeros (bytes 1-8 in 1-indexed protocol)
	 * bytes  8- 9: HostLen (uint16 LE)
	 * bytes 10-11: UserLen (uint16 LE)
	 * bytes 12-13: JobNameLen (uint16 LE)
	 * bytes 14-15: hard-coded zeros
	 * byte  16: JobFlag
	 * byte  17: NumberUp = 1
	 * bytes 18-19: JobID (uint16 LE)
	 * bytes 20-21: TZOffset (int16 LE)
	 * bytes 22-23: TZOffset2 (same value, int16 LE)
	 * bytes 24-25: Year (uint16 LE)
	 * byte  26: Month (1-indexed)
	 * byte  27: Day
	 * byte  28: Hour
	 * byte  29: Minute
	 * byte  30: Second
	 * byte  31: hard-coded zero (NOT 0x01)
	 * bytes 32-71: hard-coded zeros
	 * Total payload: 72 bytes */
	uint8_t buf[72];
	memset(buf, 0, sizeof(buf));
	buf[8]  = ml;
	buf[10] = ul;
	buf[12] = nl;
	buf[16] = fg;
	buf[17] = 0x01; /* NumberUp */
	buf[18] = LO(job);
	buf[19] = HI(job);
	buf[20] = (uint8_t)(tz & 0xFF);
	buf[21] = (uint8_t)((tz >> 8) & 0xFF);
	buf[22] = (uint8_t)(tz & 0xFF);
	buf[23] = (uint8_t)((tz >> 8) & 0xFF);
	buf[24] = LO(year);
	buf[25] = HI(year);
	buf[26] = month;
	buf[27] = (uint8_t)tm->tm_mday;
	buf[28] = (uint8_t)tm->tm_hour;
	buf[29] = (uint8_t)tm->tm_min;
	buf[30] = (uint8_t)tm->tm_sec;
	/* buf[31] = 0x00 (already zeroed) */

	capt_sendrecv(CAPT_SET_JOB_INFO2, buf, sizeof(buf), NULL, 0);
}

/*
 * ReserveUnit with error recovery (all job prologues).
 *
 * ReserveUnit reply byte 0: if bit 0x80 is set the printer is in an error
 * state (e.g. a stale job from a previous run).  Perform the canonical clear
 * sequence (GoOffline → DiscardData → ClearMisPrint → ClearError → GoOnline)
 * and retry once.  Returns the job ID extracted from buf[2:3].
 */
static uint16_t capt_reserve_unit(struct printer_state_s *state)
{
	uint8_t buf[8];
	size_t size = sizeof(buf);

	capt_sendrecv(CAPT_RESERVE_UNIT, magicbuf_0, ARRAY_SIZE(magicbuf_0), buf, &size);

	if (size >= 1 && (buf[0] & 0x80)) {
		fprintf(stderr, "DEBUG: CAPT: ReserveUnit error flag 0x%02x — clearing and retrying\n",
			buf[0]);

		/* Go offline first if not already offline */
		const struct capt_status_s *status = lbp2900_get_status(state->ops);
		if (!FLAG(status, CAPT_FL_OFFLINE)) {
			capt_sendrecv(CAPT_GO_OFFLINE, lbp3000_job_init, ARRAY_SIZE(lbp3000_job_init), NULL, 0);
			for (int i = 0; i < 50; i++) {
				status = lbp2900_get_status(state->ops);
				if (FLAG(status, CAPT_FL_OFFLINE))
					break;
				usleep(100000);
			}
		}
		capt_sendrecv(CAPT_DISCARD_DATA, NULL, 0, NULL, 0);
		capt_sendrecv(CAPT_CLEAR_MIS_PRINT, NULL, 0, NULL, 0);
		capt_sendrecv(CAPT_CLEAR_ERROR, NULL, 0, NULL, 0);
		capt_sendrecv(CAPT_GO_ONLINE, magicbuf_linux_online, ARRAY_SIZE(magicbuf_linux_online), NULL, 0);
		lbp2900_wait_ready(state->ops);

		/* Retry */
		size = sizeof(buf);
		capt_sendrecv(CAPT_RESERVE_UNIT, magicbuf_0, ARRAY_SIZE(magicbuf_0), buf, &size);
		if (size >= 1 && (buf[0] & 0x80)) {
			fprintf(stderr, "ERROR: CAPT: ReserveUnit failed (0x%02x) after error recovery — aborting\n",
				buf[0]);
			exit(1);
		}
	}

	return (size >= 4) ? WORD(buf[2], buf[3]) : 0;
}

static void lbp2900_job_prologue(struct printer_state_s *state)
{
	capt_get_printer_info(state);
	//sleep(1);
	capt_init_status();
	lbp2900_get_status(state->ops);

	capt_sendrecv(CAPT_START_0, NULL, 0, NULL, 0);
	job = capt_reserve_unit(state);

	capt_sendrecv(CAPT_SET_LED_STATUS, lbp3010_gpio_init, ARRAY_SIZE(lbp3010_gpio_init), NULL, 0);
	lbp2900_wait_ready(state->ops);

	send_job_start(1);
	lbp2900_wait_ready(state->ops);
}

static void lbp3000_job_prologue(struct printer_state_s *state)
{
	const struct capt_status_s *status;

	state->sent_job_cont = false;
	state->printer_page_offset = 0;

	capt_get_printer_info(state);
	//sleep(1);
	capt_init_status();
	lbp2900_get_status(state->ops);

	/* Wait for printer to settle at startup: loop GetExtendedStatus + GetInputStatus
	 * until NEED_INPUT_STATUS (Bas1 & 0x02) clears (protocol §3.1 startup §9.1) */
	for (int i = 0; i < 50; i++) {
		status = lbp2900_get_status(state->ops);
		if (!FLAG(status, CAPT_FL_NEED_INPUT_STATUS))
			break;
		capt_sendrecv(CAPT_GET_INPUT_STATUS, NULL, 0, NULL, 0);
		usleep(200000);
	}

	job = capt_reserve_unit(state);

	capt_sendrecv(CAPT_SET_POWER_DOWN, lbp6000_job_init, ARRAY_SIZE(lbp6000_job_init), NULL, 0);
	status = lbp2900_get_status(state->ops);
	if (FLAG(status, CAPT_FL_NOTREADY)) {
		for (int i = 0; i < 50; i++) {
			status = lbp2900_get_status(state->ops);
			if (!FLAG(status, CAPT_FL_NOTREADY))
				break;
			usleep(100000);
		}
		/* GetExtendedStatus to confirm offline (protocol §2 §7: Bas=0x10) */
		capt_get_xstatus_only();
	}

	send_job_start(CAPT_JOBFLAG_START);

	/* Only go offline if printer is currently online (protocol §3.1 §9.4).
	 * Read GetBasicStatus and check the OFFLINE bit; skip GoOffline if
	 * the printer is already offline and go directly to the clear sequence. */
	status = lbp2900_get_status(state->ops);
	if (!(FLAG(status, CAPT_FL_OFFLINE))) {
		capt_sendrecv(CAPT_GO_OFFLINE, lbp3000_job_init, ARRAY_SIZE(lbp3000_job_init), NULL, 0);
		/* Poll GetBasicStatus until OFFLINE bit is set (protocol §3.1 §9.4) */
		for (int i = 0; i < 50; i++) {
			status = lbp2900_get_status(state->ops);
			if (FLAG(status, CAPT_FL_OFFLINE))
				break;
			usleep(100000);
		}
		/* GetExtendedStatus to confirm offline (protocol §2 §7: Bas=0x10) */
		capt_get_xstatus_only();
	}

	/* Linux canonical clear order: DiscardData → ClearMisPrint → ClearError → GoOnline
	 * (protocol §3.1.1 Linux order, §9 note 18) */
	capt_sendrecv(CAPT_DISCARD_DATA, NULL, 0, NULL, 0);
	capt_sendrecv(CAPT_CLEAR_MIS_PRINT, NULL, 0, NULL, 0);
	capt_sendrecv(CAPT_CLEAR_ERROR, NULL, 0, NULL, 0);
	/* GoOnline: 8-byte Linux payload with ee db ea ad magic (protocol §2.12) */
	capt_sendrecv(CAPT_GO_ONLINE, magicbuf_linux_online, ARRAY_SIZE(magicbuf_linux_online), NULL, 0);

	lbp2900_wait_ready(state->ops);
	/* GetExtendedStatus: confirm Start=0,Printing=0,Shipped=0,Printed=0 (protocol §2) */
	capt_get_xstatus_only();
	/* GetBasicStatus: final check after GoOnline (protocol §2) */
	lbp2900_get_status(state->ops);
}

static void lbp3010_job_prologue(struct printer_state_s *state)
{
	capt_get_printer_info(state);
	//sleep(1);
	capt_init_status();
	lbp2900_get_status(state->ops);

	capt_sendrecv(CAPT_START_0, NULL, 0, NULL, 0);
	job = capt_reserve_unit(state);

	capt_sendrecv(CAPT_SET_LED_STATUS, lbp3010_gpio_init, ARRAY_SIZE(lbp3010_gpio_init), NULL, 0);
	lbp2900_wait_ready(state->ops);

	send_job_start(1);
	lbp2900_wait_ready(state->ops);
}

static void lbp6000_job_prologue(struct printer_state_s *state)
{
	capt_get_printer_info(state);
	sleep(1);
	capt_init_status();
	lbp2900_get_status(state->ops);

	capt_sendrecv(CAPT_START_0, NULL, 0, NULL, 0);
	job = capt_reserve_unit(state);

	capt_sendrecv(CAPT_SET_LED_STATUS, lbp3010_gpio_init, ARRAY_SIZE(lbp3010_gpio_init), NULL, 0);
	lbp2900_wait_ready(state->ops);

	capt_sendrecv(CAPT_SET_POWER_DOWN, lbp6000_job_init, ARRAY_SIZE(lbp6000_job_init), NULL, 0);
	lbp2900_wait_ready(state->ops);

	send_job_start(1);
	lbp2900_wait_ready(state->ops);
}

static bool lbp2900_page_prologue(struct printer_state_s *state, const struct page_dims_s *dims)
{
	const struct capt_status_s *status;
	size_t s;
	uint8_t buf[16];

	uint8_t sz = 0x00; /* paper size byte from tPaperSizeTbl */
	uint8_t save = dims->toner_save; /* TonerSave */
	uint8_t fm = 0x00; /* MediaType (fuser/media mode) */

	/* TonerDensity: all 4 bytes equal per protocol §2.14.
	 * Default 0x1f (density=7 in bits 5-2). If ink_k is set, shift it.
	 * Fix: was ink_k, 0x1C, 0x1C, 0x1C (non-uniform); now all 4 bytes equal. */
	uint8_t td = (dims->ink_k > 0) ? (uint8_t)(dims->ink_k << 2) : 0x1f;

	/* PaperType: special_mode_for_papertype() result per protocol §2.14.
	 * Fix: was raw dims->media_type (wrong enum values); now converted correctly.
	 * 0x00=Plain/Thick, 0x20=Envelope, 0x24=Transparency */
	uint8_t paper_type = 0x00;

	switch (dims->media_type) {
		case 0x80:
			/* Plain Paper, special case for LBP2900/3000 */
			fm = 0x01;
			paper_type = 0x00;
			break;
		case 0x00:
			/* Plain Paper */
			fm = 0x01;
			paper_type = 0x01;
			break;
		case 0x02:
			/* Plain Paper L */
			fm = 0x02;
			paper_type = 0x04;
			break;
		case 0x01:
			/* Thick Paper */
			fm = 0x01;
			paper_type = 0x03;
			break;
		case 0x03:
			/* Thick Paper H */
			fm = 0x03;
			paper_type = 0x05;
			break;
		case 0x04:
			/* Transparency */
			fm = 0x13;
			paper_type = 0x24; // ???
			break;
		case 0x05:
			/* Envelope */
			fm = 0x05;
			paper_type = 0x07;
			break;
		default:
			fm = 0x01;
			paper_type = 0x01;
	}
	fprintf(stderr, "DEBUG: CAPT: media_type=%u, fm=%u, paper_type=0x%02x\n",
		dims->media_type, fm, paper_type);

	if ( strncmp(dims->media_size, "A4", 2) == 0 ) sz = 0x02;
	else if ( strncmp(dims->media_size, "A5", 2) == 0 ) sz = 0x03;
	else if ( strncmp(dims->media_size, "B5", 2) == 0 ) sz = 0x07;
	else if ( strncmp(dims->media_size, "Executive", 9) == 0 ) sz = 0x0A;
	else if ( strncmp(dims->media_size, "Legal", 5) == 0 ) sz = 0x0C;
	else if ( strncmp(dims->media_size, "Letter", 6) ==0 ) sz = 0x0D;
	else if ( strncmp(dims->media_size, "EnvC5", 5) ==0 ) sz = 0x15;
	else if ( strncmp(dims->media_size, "Env10", 5) == 0 ) sz = 0x16;
	else if ( strncmp(dims->media_size, "EnvMonarch", 10) == 0 ) sz = 0x17;
	else if ( strncmp(dims->media_size, "EnvDL", 5) == 0 ) sz = 0x18;
	else if ( strncmp(dims->media_size, "3x5", 3) ==0 ) sz = 0x40;
	else if ( strncmp(dims->media_size, "PRC16K", 6) ==0 ) sz = 0xD4;
	else sz = 0x02;
	fprintf(stderr, "DEBUG: CAPT: media_size=%s, sz=0x%02x\n", dims->media_size, sz);

	/* Save paper size byte for use in out-of-paper SetLEDStatus payload. */
	state->paper_sz = sz;

	/* Reset streaming-mode flag for each new page. */
	state->startprint_sent = false;

	/* IC_BEGIN_PAGE (D0A0) 40-byte payload per protocol §2.14:
	 * idx  0- 1: PageSeq (set to 0x0000 here; sequence filled per page)
	 * idx  2- 3: ModelConst = 0x2a30 for LBP3000 (LE: 0x30, 0x2a)
	 * idx  4   : PaperSzByte
	 * idx  5   : Unk6 = 0x00 (Windows value)
	 * idx  6   : PaperSrc = 0x00 (auto-feed)
	 * idx  7   : 0x00
	 * idx  8-11: TonerDensity — all 4 bytes equal (default 0x1f)
	 * idx 12   : PaperType (special_mode_for_papertype: 0x00=Plain, 0x20=Env, 0x24=Trans)
	 * idx 13   : ResFlag = 0x11 (600 dpi constant for LBP3000, NOT media_adapt)
	 * idx 14   : Fixed_04 = 0x04 (CNTblModel=1)
	 * idx 15   : Fixed_00 = 0x00
	 * idx 16   : Fixed_01 = 0x01
	 * idx 17   : Fixed_01b = 0x01
	 * idx 18   : SuperSmooth = 0x02 (CNSuperSmooth for LBP3000)
	 * idx 19   : TonerSave = 0x00 off by default
	 * idx 20   : Unk21 = 0x00 (Windows value)
	 * idx 21   : 0x00 for LBP3000
	 * idx 22-23: MarginW uint16 LE
	 * idx 24-25: MarginH uint16 LE
	 * idx 26-27: LineSize uint16 LE
	 * idx 28-29: ImgHeight uint16 LE
	 * idx 30-31: PaperW uint16 LE
	 * idx 32-33: PaperH uint16 LE
	 * idx 34-35: FixFlags uint16 LE = 0x0000 for plain paper, no duplex
	 * idx 36   : MediaType = 0x01 for Plain/Plain L
	 * idx 37-39: 0x00 padding
	 * Total: 40 bytes for LBP3000 (CNTblModel=1)
	 */
	uint8_t pageparms[] = {
		/* idx  0- 7 */
		0x00, 0x00, 0x30, 0x2A, sz, 0x00, 0x00, 0x00,
		/* idx  8-15: TonerDensity, PaperType, ResFlag=0x11, Fixed_04, Fixed_00 */
		td, 0x1C, 0x1C, 0x1C, paper_type, dims->media_adapt, 0x04, 0x00,
		/* idx 16-23: Fixed_01, Fixed_01b, SuperSmooth=0x02, TonerSave, Unk21=0x00, 0x00, MarginW LE */
		0x01, 0x01, 0x02, save, 0x00, 0x00,
		LO(dims->margin_width), HI(dims->margin_width),
		/* idx 24-31: MarginH LE, LineSize LE, ImgHeight LE */
		LO(dims->margin_height), HI(dims->margin_height),
		LO(dims->line_size), HI(dims->line_size),
		LO(dims->num_lines), HI(dims->num_lines),
		/* idx 32-39: PaperW LE, PaperH LE, FixFlags=0x0000, MediaType, padding */
		LO(dims->paper_width), HI(dims->paper_width),
		LO(dims->paper_height), HI(dims->paper_height),
		0x00, 0x00, fm, 0x00, 0x00, 0x00,
	};

	status = lbp2900_get_status(state->ops);
	if (FLAG(status, CAPT_FL_UNINIT1) || FLAG(status, CAPT_FL_OFFLINE)) {
		/* Windows canonical clear order: ClearMisPrint → ClearError → DiscardData → GoOnline
		 * (protocol §3.1.1 Windows order, §9 note 18) */
		capt_sendrecv(CAPT_CLEAR_MIS_PRINT, NULL, 0, NULL, 0);
		capt_sendrecv(CAPT_CLEAR_ERROR, NULL, 0, NULL, 0);
		capt_sendrecv(CAPT_DISCARD_DATA, NULL, 0, NULL, 0);
		lbp2900_wait_ready(state->ops);

		/* GoOnline: 16-byte Windows payload with ee db ea ad magic (protocol §2.12) */
		capt_sendrecv(CAPT_GO_ONLINE, magicbuf_2, ARRAY_SIZE(magicbuf_2), NULL, 0);
		lbp2900_wait_ready(state->ops);
	}

	unsigned ready_attempts = 0;
	while (1) {
		if (! FLAG(lbp2900_get_status(state->ops), CAPT_FL_CMD_BUSY))
			break;
		capt_poll_wait(&ready_attempts, "page command readiness");
	}

	capt_multi_begin(CAPT_MULTI_COMMAND);
	capt_multi_add(CAPT_IC_BEGIN_PAGE, pageparms, sizeof(pageparms));
	s = hiscoa_format_params(buf, sizeof(buf), &hiscoa_default_params);
	capt_multi_add(CAPT_IC_BLACK_PLANE, buf, s);
	capt_multi_add(CAPT_IC_BEGIN_DATA, NULL, 0);
	capt_multi_add(CAPT_IC_END_PAGE, NULL, 0);
	capt_multi_send();

	return true;
}

/*
 * Out-of-paper recovery sequence (protocol §3.2, §9 notes 11-12).
 *
 * Called when GetBasicStatus shows NOTREADY|OFFLINE and GetExtendedStatus
 * confirms PRINT_REJECTED + Pap==0x00.
 *
 * Returns after the printer is back online and ready to reprint.
 * Callers should track pages_printed_before_error for reprint logic.
 */
static void lbp3000_oop_recovery(struct printer_state_s *state)
{
	const struct capt_status_s *status;

	fprintf(stderr, "DEBUG: CAPT: out-of-paper recovery starting\n");
	fprintf(stderr, "STATE: +media-empty\nINFO: Load paper and press the printer's button\n");

	/* 1. GetInputStatus — triggered by Bas1 & NEED_INPUT_STATUS; byte3=0x80 confirms paper-out */
	capt_sendrecv(CAPT_GET_INPUT_STATUS, NULL, 0, NULL, 0);
	/* 2. GetBasicStatus */
	lbp2900_get_status(state->ops);
	/* 3. GetExtendedStatus — Cnt=0x40 (PRINT_REJECTED), Pap=0x00 */
	capt_get_xstatus_only();

	/* 4. ClearMisPrint */
	capt_sendrecv(CAPT_CLEAR_MIS_PRINT, NULL, 0, NULL, 0);
	/* 5. ClearError */
	capt_sendrecv(CAPT_CLEAR_ERROR, NULL, 0, NULL, 0);
	/* 6. DiscardData */
	capt_sendrecv(CAPT_DISCARD_DATA, NULL, 0, NULL, 0);

	/* 7. GetBasicStatus */
	lbp2900_get_status(state->ops);
	/* 8. GetExtendedStatus — Bas=0x10 (offline), Cnt=0x00 (cleared) */
	capt_get_xstatus_only();

	/* 9. GoOnline — pulse online to RESET page counters to 0 */
	capt_sendrecv(CAPT_GO_ONLINE, magicbuf_linux_online, ARRAY_SIZE(magicbuf_linux_online), NULL, 0);
	/* 10. GetBasicStatus — byte1 → 0x00 */
	lbp2900_get_status(state->ops);
	/* 11. GetExtendedStatus — Start=0, Printing=0, Shipped=0, Printed=0 (ALL RESET) */
	capt_get_xstatus_only();

	/* 12. GetBasicStatus */
	lbp2900_get_status(state->ops);

	/* 13. SetLEDStatus(NoPaper):
	 *   byte1 = CAPT_INTSTAT_NO_PAPER (9)
	 *   byte2 = 0x00
	 *   byte3 = CAPT_HSCODE_NO_PAPER (1)
	 *   byte4 = paper_size_id
	 *   byte5 = 0x01
	 *   bytes 6-8 = 0x00
	 *   bytes 9-12 = CAPT_HOSTERR_NO_PAPER (0x00010000 LE)
	 *
	 * Using 12-byte payload matching lbp2900_gpio_init format. */
	capt_sendrecv(CAPT_SET_LED_STATUS, state->ops->gpio_blink, state->ops->gpio_blink_size, NULL, 0);

	/* 14. GetBasicStatus */
	lbp2900_get_status(state->ops);
	/* 15. GetExtendedStatus */
	capt_get_xstatus_only();

	/* 16. GoOffline — go offline to wait */
	capt_sendrecv(CAPT_GO_OFFLINE, lbp3000_job_init, ARRAY_SIZE(lbp3000_job_init), NULL, 0);
	/* 17. GetBasicStatus — byte1 = 0x10 (offline) */
	lbp2900_get_status(state->ops);

	fprintf(stderr, "DEBUG: CAPT: out-of-paper: waiting for user to insert paper\n");

	/* [Wait for user — poll loop:]
	 * 18. Poll GetBasicStatus repeatedly until NEED_INPUT_STATUS bit set
	 *     (user inserts paper and presses physical go button). */
	while (1) {
		status = lbp2900_get_status(state->ops);
		/* CAPT_FL2_NEED_INPUT_STATUS (byte 2 bit 0x02) signals button press */
		if (FLAG(status, CAPT_FL_NEED_INPUT_STATUS))
			break;
		usleep(100000);
	}

	/* 19. NEED_INPUT_STATUS is set: paper inserted and button pressed. */
	/* GetInputStatus — byte3 changes from 0x80 → 0xc0 (engine ready) */
	capt_sendrecv(CAPT_GET_INPUT_STATUS, NULL, 0, NULL, 0);

	/* SetLEDStatus(0) — clear NoPaper LED (all 12 payload bytes = 0x00) */
	capt_sendrecv(CAPT_SET_LED_STATUS, state->ops->gpio_init, state->ops->gpio_init_size, NULL, 0);

	/* ClearMisPrint */
	capt_sendrecv(CAPT_CLEAR_MIS_PRINT, NULL, 0, NULL, 0);
	/* ClearError */
	capt_sendrecv(CAPT_CLEAR_ERROR, NULL, 0, NULL, 0);
	/* DiscardData */
	capt_sendrecv(CAPT_DISCARD_DATA, NULL, 0, NULL, 0);

	/* GetBasicStatus */
	lbp2900_get_status(state->ops);
	/* GetExtendedStatus */
	capt_get_xstatus_only();

	/* GoOnline — bring back online, resets counters again */
	capt_sendrecv(CAPT_GO_ONLINE, magicbuf_linux_online, ARRAY_SIZE(magicbuf_linux_online), NULL, 0);
	/* GetBasicStatus — byte1 → 0x00 */
	lbp2900_get_status(state->ops);
	/* GetExtendedStatus — all counters 0 again */
	capt_get_xstatus_only();

	fprintf(stderr, "DEBUG: CAPT: out-of-paper recovery complete, counters reset to 0\n");
	fprintf(stderr, "STATE: -media-empty\n");
	/* After recovery, page counters are reset to 0.
	 * Caller must track pages_printed_before_error to know where to resume. */
}

static bool lbp2900_page_epilogue(struct printer_state_s *state, const struct page_dims_s *dims)
{
	(void) dims;
	const struct capt_status_s *status;

	/*
	 * Printer-relative page number: after an OOP recovery the printer's
	 * internal counters (Start / Printing / Printed) are reset to 0 by
	 * GoOnline.  printer_page_offset records how many host pages were
	 * already processed before the reset, so pipage is always the value
	 * the printer itself will report for the current page.
	 */
	unsigned pipage = state->ipage - state->printer_page_offset;

	/*
	 * IC_BLACK_END / StartPrint sequencing (protocol §2.18a):
	 *
	 * Normal mode (startprint_sent == false):
	 *   all chunks fit in buffer → send IC_BLACK_END → StartPrint(N)
	 *
	 * Streaming mode (startprint_sent == true):
	 *   StartPrint(N) was already sent mid-stream → send IC_BLACK_END only
	 *   (ops_send_band_hiscoa already drained BufLevel to >= 1 before returning)
	 */
	capt_send(CAPT_IC_BLACK_END, NULL, 0);

	if (!state->startprint_sent) {
		/* Normal mode: poll until the printer's decoder has accepted this page's
		 * descriptor (Start == pipage), then send StartPrint immediately.
		 * Do NOT call lbp2900_wait_ready() here — CMD_BUSY (0x04) is set
		 * throughout physical printing of the previous page and will not clear
		 * until it finishes, which defeats pipeline parallelism.
		 *
		 * Fast-path: if Start==pipage was already confirmed during IC_VIDEO_DATA
		 * upload, skip the poll loop entirely (spec §3.3, §14 Rule 10). */
		status = lbp2900_get_status(state->ops);
		if (status->page_decoding != pipage) {
			unsigned decode_attempts = 0;
			/* Start not yet confirmed — poll until Start==pipage exact match (spec §3.3) */
			do {
				capt_poll_wait(&decode_attempts, "page decoder");
				status = lbp2900_get_status(state->ops);
			} while (status->page_decoding != pipage);
		}

		/* Send StartPrint immediately — no CMD_BUSY wait needed. */
		uint8_t buf[2] = { LO(pipage), HI(pipage) };
		capt_sendrecv(CAPT_START_PRINT, buf, 2, NULL, 0);
	}

	/* SetJobInfo2(flag=6) at job end is now sent once in lbp2900_job_epilogue,
	 * not per-page here, per protocol §2.7 §3.1.1. */

	unsigned page_attempts = 0;
	while (1) {
		status = lbp2900_get_status(state->ops);

		/* SetJobInfo2(flag=2): send ONCE at first inter-page idle when Printed >= 1.
		 * Spec §5, §11, §14 Rule 13: fire after StartPrint, during between-pages wait. */
		if (!state->sent_job_cont && status->page_completed >= 1) {
			send_job_start(CAPT_JOBFLAG_CONT);
			state->sent_job_cont = true;
		}

		/* Return as soon as the Printing counter reaches this page — the printer
		 * has accepted the page into its engine and the next page's D0A9 multi-command
		 * can be announced immediately (protocol §2.13, §2.18a, §4).
		 * Waiting for page_out (Shipped) == page_decoding (Start) instead adds
		 * unnecessary latency equal to the physical paper-delivery time per page. */
		if (status->page_printing >= pipage)
			return true;

		/*
		 * Out-of-paper detection (protocol §3.2, §9 notes 11-12):
		 * Byte 1: NOTREADY (0x02) | OFFLINE (0x10) set during printing.
		 * Confirm via GetExtendedStatus: PRINT_REJECTED (Cnt & 0x40) AND Pap == 0x00.
		 */
		if ((FLAG(status, CAPT_FL_NOTREADY) || FLAG(status, CAPT_FL_OFFLINE))
		    && !FLAG(status, CAPT_FL_PRINTING)
		    && !FLAG(status, CAPT_FL_PROCESSING1)) {
			/* Confirm with GetExtendedStatus — this populates xstat_cnt and xstat_pap */
			status = capt_get_xstatus_only();
			/* Do NOT call lbp2900_get_status() here — it would overwrite xstat fields */
			if ((status->xstat_cnt & CAPT_CNT_PRINT_REJECTED)
			    && status->xstat_pap == 0x00) {
				fprintf(stderr, "DEBUG: CAPT: out-of-paper confirmed (Cnt=0x%02x, Pap=0x%02x)\n",
					status->xstat_cnt, status->xstat_pap);
				/*
				 * Track pages printed before the error for reprint logic.
				 * Page counters will reset to 0 after GoOnline in recovery.
				 */
				state->pages_printed_before_error = (int)status->page_completed;
				lbp3000_oop_recovery(state);
				/*
				 * After recovery the printer's Start/Printing/Printed counters
				 * have been reset to 0 by GoOnline.  Record the offset so that
				 * pipage = ipage - printer_page_offset is correct for the reprint.
				 * The page that triggered OOP was never physically printed, so the
				 * offset is (ipage - 1): the next attempt at ipage maps to pipage=1.
				 */
				state->printer_page_offset = state->ipage - 1;
				fprintf(stderr, "DEBUG: CAPT: printer_page_offset set to %u (ipage=%u)\n",
					state->printer_page_offset, state->ipage);
				/*
				 * Return false so the caller can reprint the affected page.
				 */
				return false;
			}
		}

		/*
		 * Fatal hardware errors: paper jam (Eng bit 0x0100) or door open
		 * (Eng bits 0x4000).  These conditions cannot be recovered by the
		 * driver.  Release the reserved unit so the printer is not left
		 * in a locked state, then abort.
		 */
		if (status->xstat_eng & (CAPT_ENG_JAM | CAPT_ENG_DOOR_OPEN)) {
			const char *reason = (status->xstat_eng & CAPT_ENG_JAM)
				? "paper jam" : "door open";
			fprintf(stderr, "ERROR: CAPT: fatal printer error: %s (Eng=0x%04x) — aborting\n",
				reason, status->xstat_eng);
			fprintf(stderr, "STATE: +%s\n",
				(status->xstat_eng & CAPT_ENG_JAM) ? "media-jam" : "door-open");
			uint8_t jbuf[2] = { LO(job), HI(job) };
			capt_sendrecv(CAPT_RELEASE_UNIT, jbuf, 2, NULL, 0);
			exit(1);
		}

		/* Legacy no-paper flags from extended status */
		if (FLAG(status, CAPT_FL_NOPAPER2) || FLAG(status, CAPT_FL_NOTREADY)) {
			fprintf(stderr, "DEBUG: CAPT: no paper\n");
			fprintf(stderr, "STATE: +media-empty\n");
			if (FLAG(status, CAPT_FL_PRINTING) || FLAG(status, CAPT_FL_PROCESSING1))
				capt_poll_wait(&page_attempts, "page acceptance");
			else
				return false;
		}
		capt_poll_wait(&page_attempts, "page acceptance");
	}
}

static void lbp2900_job_epilogue(struct printer_state_s *state)
{
	uint8_t jbuf[2] = { LO(job), HI(job) };

	/* SetJobInfo2(flag=3): send immediately after last page's IC_BLACK_END.
	 * Spec §9, §11, §14 Rule 14: do NOT wait for Printed==totalPages first.
	 * GetExtendedStatus first to confirm final state, then send flag=3. */
	capt_get_xstatus_only();
	send_job_start(CAPT_JOBFLAG_END);

	capt_sendrecv(CAPT_RELEASE_UNIT, jbuf, 2, NULL, 0);

	/* Wait for RCF_PAPER_DELIVERY (Aux byte 9 bit 0x04) to clear before
	 * issuing the teardown sequence.  The last sheet is still being
	 * physically delivered while this bit is set; sending ClearError /
	 * DiscardData / GoOffline / ReleaseUnit before it clears aborts the
	 * delivery prematurely.  Poll GetExtendedStatus (with GetInputStatus
	 * interleaved as the Windows driver does) until the bit is gone. */
	{
		const struct capt_status_s *s;
		unsigned delivery_attempts = 0;
		while (1) {
			capt_sendrecv(CAPT_GET_INPUT_STATUS, NULL, 0, NULL, 0);
			s = capt_get_xstatus_only();
			if (!(s->aux & CAPT_AUX_PAPER_DELIVERY))
				break;
			capt_poll_wait(&delivery_attempts, "final paper delivery");
		}
	}
	return;

	/* Linux epilogue: ClearError → DiscardData → GetExtendedStatus
	 * → GoOffline(JobID) → ReleaseUnit (protocol §3.1.1 Linux order) */
	capt_get_xstatus_only();
	capt_sendrecv(CAPT_CLEAR_ERROR, NULL, 0, NULL, 0);
	capt_sendrecv(CAPT_DISCARD_DATA, NULL, 0, NULL, 0);
	capt_get_xstatus_only();
	capt_sendrecv(CAPT_GO_OFFLINE, jbuf, 2, NULL, 0);
	capt_sendrecv(CAPT_RELEASE_UNIT, jbuf, 2, NULL, 0);
}

static void lbp2900_page_setup(struct printer_state_s *state,
		struct page_dims_s *dims,
		unsigned width, unsigned height)
{
	/* FIXME: Do we still need this function? */
	(void) state;
	(void) width;
	(void) height;
	(void) dims;
	/* Get raster dimensions straight from CUPS in paper.c */
	//dims->num_lines = dims->paper_height;
	//dims->line_size = dims->paper_width / 8;
	//dims->band_size = 70;
}

static void lbp2900_cancel_cleanup(struct printer_state_s *state)
{
	(void) state;
	uint8_t jbuf[2] = { LO(job), HI(job) };

	capt_sendrecv(CAPT_SET_LED_STATUS, lbp2900_gpio_init, ARRAY_SIZE(lbp2900_gpio_init), NULL, 0);
	send_job_start(CAPT_JOBFLAG_ABORT);
	capt_sendrecv(CAPT_RELEASE_UNIT, jbuf, 2, NULL, 0);
}

static void lbp3010_cancel_cleanup(struct printer_state_s *state)
{
	(void) state;
	uint8_t jbuf[2] = { LO(job), HI(job) };

	capt_sendrecv(CAPT_SET_LED_STATUS, lbp3010_gpio_init, ARRAY_SIZE(lbp3010_gpio_init), NULL, 0);
	send_job_start(CAPT_JOBFLAG_ABORT);
	capt_sendrecv(CAPT_RELEASE_UNIT, jbuf, 2, NULL, 0);
}

static void lbp2900_wait_user(struct printer_state_s *state)
{
	(void) state;

	capt_sendrecv(CAPT_SET_LED_STATUS, lbp2900_gpio_blink, ARRAY_SIZE(lbp2900_gpio_blink), NULL, 0);
	lbp2900_wait_ready(state->ops);

	while (1) {
		const struct capt_status_s *status = lbp2900_get_status(state->ops);
		if (FLAG(status, CAPT_FL_BUTTON_ON)) {
			fprintf(stderr, "DEBUG: CAPT: button activated\n");
		}
		if (FLAG(status, CAPT_FL_BUTTON)) {
			fprintf(stderr, "DEBUG: CAPT: button pressed\n");
			break;
		}
		usleep(100000);
	}

	capt_sendrecv(CAPT_SET_LED_STATUS, lbp2900_gpio_init, ARRAY_SIZE(lbp2900_gpio_init), NULL, 0);
	lbp2900_wait_ready(state->ops);
}

static void lbp3010_wait_user(struct printer_state_s *state)
{
	(void) state;

	capt_sendrecv(CAPT_SET_LED_STATUS, lbp3010_gpio_blink, ARRAY_SIZE(lbp3010_gpio_blink), NULL, 0);
	lbp2900_wait_ready(state->ops);

	while (1) {
		const struct capt_status_s *status = lbp2900_get_status(state->ops);
		if (FLAG(status, CAPT_FL_BUTTON_ON)) {
			fprintf(stderr, "DEBUG: CAPT: button activated\n");
		}
		if (FLAG(status, CAPT_FL_nERROR)) {
			fprintf(stderr, "DEBUG: CAPT: (virtual) button pressed\n");
			break;
		}
		sleep(1);
	}

	capt_sendrecv(CAPT_SET_LED_STATUS, lbp3010_gpio_init, ARRAY_SIZE(lbp3010_gpio_init), NULL, 0);
	lbp2900_wait_ready(state->ops);
}

static struct lbp2900_ops_s lbp2900_ops = {
	.ops = {
		.job_prologue = lbp3000_job_prologue,
		.job_epilogue = lbp2900_job_epilogue,
		.page_setup = lbp2900_page_setup,
		.page_prologue = lbp2900_page_prologue,
		.page_epilogue = lbp2900_page_epilogue,
		.compress_band = ops_compress_band_hiscoa,
		.send_band = ops_send_band_hiscoa,
		.cancel_cleanup = lbp2900_cancel_cleanup,
		.wait_user = lbp2900_wait_user,
		.gpio_init = lbp2900_gpio_init,
		.gpio_init_size = ARRAY_SIZE(lbp2900_gpio_init),
		.gpio_blink = lbp2900_gpio_blink,
		.gpio_blink_size = ARRAY_SIZE(lbp2900_gpio_blink)
	},
	/* Always refresh extended page counters on the LBP2900. */
	.get_status = capt_get_xstatus_only,
	.wait_ready = capt_wait_xready_only,
};
register_printer("LBP2900", lbp2900_ops.ops, WORKS);

static struct lbp2900_ops_s lbp3000_ops = {
	.ops = {
		.job_prologue = lbp3000_job_prologue,	/* different job prologue */
		.job_epilogue = lbp2900_job_epilogue,
		.page_setup = lbp2900_page_setup,
		.page_prologue = lbp2900_page_prologue,
		.page_epilogue = lbp2900_page_epilogue,
		.compress_band = ops_compress_band_hiscoa,
		.send_band = ops_send_band_hiscoa,
		.cancel_cleanup = lbp2900_cancel_cleanup,
		.wait_user = lbp2900_wait_user,
		.gpio_init = lbp2900_gpio_init,
		.gpio_init_size = ARRAY_SIZE(lbp2900_gpio_init),
		.gpio_blink = lbp2900_gpio_blink,
		.gpio_blink_size = ARRAY_SIZE(lbp2900_gpio_blink)
	},
	.get_status = capt_get_xstatus,
	.wait_ready = capt_wait_ready,
};
register_printer("LBP3000", lbp3000_ops.ops, EXPERIMENTAL);

static struct lbp2900_ops_s lbp3010_ops = {
	.ops = {
		.job_prologue = lbp3010_job_prologue,
		.job_epilogue = lbp2900_job_epilogue,
		.page_setup = lbp2900_page_setup,
		.page_prologue = lbp2900_page_prologue,
		.page_epilogue = lbp2900_page_epilogue,
		.compress_band = ops_compress_band_hiscoa,
		.send_band = ops_send_band_hiscoa,
		.cancel_cleanup = lbp3010_cancel_cleanup,
		.wait_user = lbp3010_wait_user,
		.gpio_init = lbp3010_gpio_init,
		.gpio_init_size = ARRAY_SIZE(lbp3010_gpio_init),
		.gpio_blink = lbp3010_gpio_blink,
		.gpio_blink_size = ARRAY_SIZE(lbp3010_gpio_blink)
	},
	.get_status = capt_get_xstatus_only,
	.wait_ready = capt_wait_xready_only,
};

static struct lbp2900_ops_s lbp6000_ops = {
	.ops = {
		.job_prologue = lbp3000_job_prologue,
		.job_epilogue = lbp2900_job_epilogue,
		.page_setup = lbp2900_page_setup,
		.page_prologue = lbp2900_page_prologue,
		.page_epilogue = lbp2900_page_epilogue,
		.compress_band = ops_compress_band_hiscoa,
		.send_band = ops_send_band_hiscoa,
		.cancel_cleanup = lbp3010_cancel_cleanup,
		.wait_user = lbp3010_wait_user,
		.gpio_init = lbp3010_gpio_init,
		.gpio_init_size = ARRAY_SIZE(lbp3010_gpio_init),
		.gpio_blink = lbp3010_gpio_blink,
		.gpio_blink_size = ARRAY_SIZE(lbp3010_gpio_blink)
	},
	.get_status = capt_get_xstatus_only,
	.wait_ready = capt_wait_xready_only,
};

register_printer("LBP3010/LBP3018/LBP3050", lbp3010_ops.ops, WORKS);
register_printer("LBP3100/LBP3108/LBP3150", lbp3010_ops.ops, EXPERIMENTAL);
register_printer("LBP6000/LBP6018", lbp6000_ops.ops, EXPERIMENTAL);
