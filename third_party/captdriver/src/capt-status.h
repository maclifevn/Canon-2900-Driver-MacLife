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

#pragma once

#include "std.h"

struct capt_status_s {
	uint16_t status[7];

	uint16_t page_decoding;
	uint16_t page_printing;
	uint16_t page_out;
	uint16_t page_completed;
	uint16_t page_received;
	uint8_t  buf_level;        /* BufLevel low byte from GetBasicStatus bytes 5-6 */
	uint8_t  xstat_cnt;        /* GetExtendedStatus byte 10 (Cnt) engine comm flags */
	uint8_t  xstat_pap;        /* GetExtendedStatus byte 11 (Pap) paper availability */
	uint8_t  aux;              /* GetExtendedStatus byte 9 (Aux) auxiliary engine status */
	uint16_t xstat_eng;        /* GetExtendedStatus bytes 13-14 (Eng) engine error flags */
};

/* Local macOS port: fail a stuck hardware handshake instead of treating an
 * unconfirmed page as printed. Waiting for the user's paper/button is separate. */
#ifndef CAPT_MAX_POLLS
#define CAPT_MAX_POLLS 1200
#endif
void capt_poll_wait(unsigned *attempts, const char *phase);

#define _FL(s, b) ((s << 16) | (1 << b))
enum capt_flags
{
	/* status[0] bBasicStatus — byte 1 of GetBasicStatus / GetExtendedStatus */
	CAPT_FL_READY1              = _FL(0, 15), /* ? */
	CAPT_FL_READY2              = _FL(0, 12), /* ? */
	/* byte 2 secondary flags (packed into high byte of status[0]) */
	CAPT_FL_NEED_INPUT_STATUS   = _FL(0, 9),  /* paper tray state changed — call GetInputStatus */
	CAPT_FL_XSTATUS_CHANGED     = _FL(0, 8),  /* extended status changed — call GetExtendedStatus */
	/* byte 1 flags */
	CAPT_FL_ERROR               = _FL(0, 7),  /* ERROR_BIT — 0x80 */
	CAPT_FL_UNINIT1             = _FL(0, 5),  /* bit 5 = 0x20 — undefined in protocol (kept for compat) */
	CAPT_FL_OFFLINE             = _FL(0, 4),  /* RCF_OFFLINE — 0x10 */
	CAPT_FL_CMD_BUSY            = _FL(0, 2),  /* CMD_BUSY — 0x04 — processing command / printing */
	CAPT_FL_IM_DATA_BUSY        = _FL(0, 3),  /* IM_DATA_BUSY — 0x08 — data buffer full */
	CAPT_FL_NOTREADY            = _FL(0, 1),  /* RCF_NOTREADY — 0x02 */
	CAPT_FL_PRINTER_FREE        = _FL(0, 0),  /* RCF_PRINTER_FREE — 0x01 — printer idle/free */
	CAPT_FL_UNIT_FREE           = _FL(0, 6),  /* UNIT_FREE — 0x40 */
	/* status[1] */
	CAPT_FL_NOPAPER2            = _FL(1, 14),
	CAPT_FL_PROCESSING1         = _FL(1, 7),
	CAPT_FL_BUTTON              = _FL(1, 5),
	CAPT_FL_PRINTING            = _FL(1, 2),
	CAPT_FL_POWERUP             = _FL(1, 0),
	/* status[2] */
	CAPT_FL_nERROR              = _FL(2, 7),
	CAPT_FL_BUTTON1             = _FL(2, 8),
	/* status[3] */
	CAPT_FL_POWERUP1            = _FL(3, 12),
	/* status[4] */
	CAPT_FL_BUTTON_ON           = _FL(4, 0),
	/* status[5] */
	/* status[6] */
};
#undef _FL

static inline bool FLAG(const struct capt_status_s *status, enum capt_flags flag)
{
	return !! (status->status[flag >> 16] & (flag & 0xFFFF));
}

/* ---------------------------------------------------------------------------
 * GetBasicStatus byte offsets (0-indexed in C buffer)
 * ---------------------------------------------------------------------------*/
#define CAPT_BSTAT_STATUS   0   /* byte 1: status flags (CAPT_FL_*) */
#define CAPT_BSTAT_STATUS2  1   /* byte 2: secondary flags */
#define CAPT_BSTAT_BUF_LO   4   /* byte 5: BufLevel low byte */
#define CAPT_BSTAT_BUF_HI   5   /* byte 6: BufLevel high byte */

/* ---------------------------------------------------------------------------
 * GetExtendedStatus reply byte offsets (0-indexed in C buffer, protocol bytes are 1-indexed)
 * ---------------------------------------------------------------------------*/
#define CAPT_XSTAT_BAS      0   /* byte 1: basic status flags */
#define CAPT_XSTAT_BAS1     1   /* byte 2: secondary flags */
#define CAPT_XSTAT_BAS2     2   /* byte 3: unknown */
#define CAPT_XSTAT_BUF_LO   4   /* byte 5: BufLevel low byte */
#define CAPT_XSTAT_BUF_HI   5   /* byte 6: BufLevel high byte */
#define CAPT_XSTAT_AUX      8   /* byte 9: auxiliary engine status */
#define CAPT_XSTAT_CNT      9   /* byte 10: engine communication flags */
#define CAPT_XSTAT_PAP      10  /* byte 11: paper availability */
#define CAPT_XSTAT_CNT2     11  /* byte 12: controller status 2 */
#define CAPT_XSTAT_ENG_LO   12  /* byte 13: engine ready flags low */
#define CAPT_XSTAT_ENG_HI   13  /* byte 14: engine ready flags high */
#define CAPT_XSTAT_START_LO 14  /* byte 15: Start page counter low */
#define CAPT_XSTAT_START_HI 15  /* byte 16: Start page counter high */
#define CAPT_XSTAT_PRINTING_LO 16 /* byte 17: Printing counter low */
#define CAPT_XSTAT_PRINTING_HI 17 /* byte 18: Printing counter high */
#define CAPT_XSTAT_SHIPPED_LO 18  /* byte 19: Shipped counter low */
#define CAPT_XSTAT_SHIPPED_HI 19  /* byte 20: Shipped counter high */
#define CAPT_XSTAT_PRINTED_LO 20  /* byte 21: Printed counter low */
#define CAPT_XSTAT_PRINTED_HI 21  /* byte 22: Printed counter high */
#define CAPT_XSTAT_LED      24  /* byte 25: LED/internal status code */

/* ---------------------------------------------------------------------------
 * GetBasicStatus / GetExtendedStatus byte 1 flag bits (for raw byte access)
 * ---------------------------------------------------------------------------*/
#define CAPT_FL_PRINTER_FREE_BIT    0x01  /* RCF_PRINTER_FREE — printer idle/free */
#define CAPT_FL_NOTREADY_BIT        0x02  /* RCF_NOTREADY */
#define CAPT_FL_CMD_BUSY_BIT        0x04  /* CMD_BUSY — processing command / printing */
#define CAPT_FL_IM_DATA_BUSY_BIT    0x08  /* IM_DATA_BUSY — data buffer full (NOT observed on LBP3000) */
#define CAPT_FL_OFFLINE_BIT         0x10  /* RCF_OFFLINE */
#define CAPT_FL_UNIT_FREE_BIT       0x40  /* UNIT_FREE */
#define CAPT_FL_ERROR_BIT           0x80  /* ERROR_BIT */

/* GetBasicStatus / GetExtendedStatus byte 2 flag bits */
#define CAPT_FL2_XSTATUS_CHANGED    0x01  /* Extended status changed — call GetExtendedStatus */
#define CAPT_FL2_NEED_INPUT_STATUS  0x02  /* Paper tray state changed — call GetInputStatus */

/* ---------------------------------------------------------------------------
 * GetExtendedStatus byte 9 (Aux) flags
 * ---------------------------------------------------------------------------*/
#define CAPT_AUX_PRINTER_BUSY   0x02  /* RCF_PRINTER_BUSY */
#define CAPT_AUX_PAPER_DELIVERY 0x04  /* RCF_PAPER_DELIVERY — paper being fed */
#define CAPT_AUX_SAFE_TIMER     0x80  /* RCF_SAFE_TIMER — motor running */

/* ---------------------------------------------------------------------------
 * GetExtendedStatus byte 10 (Cnt) flags
 * ---------------------------------------------------------------------------*/
#define CAPT_CNT_OVERRUN            0x01
#define CAPT_CNT_UNDERRUN           0x02
#define CAPT_CNT_MISSING_EOP        0x04
#define CAPT_CNT_INVALID_DATA       0x08
#define CAPT_CNT_ENGINE_COMM_ERROR  0x10
#define CAPT_CNT_ENGINE_RESET       0x20
#define CAPT_CNT_PRINT_REJECTED     0x40  /* RCF_PRINT_REJECTED — key flag for out-of-paper */

/* ---------------------------------------------------------------------------
 * GetExtendedStatus bytes 13-14 (Eng) flags
 * ---------------------------------------------------------------------------*/
#define CAPT_ENG_SERVICE_CALL   0x0002
#define CAPT_ENG_CLEANING       0x0004
#define CAPT_ENG_ILLEGAL_CONN   0x0008
#define CAPT_ENG_JAM            0x0100
#define CAPT_ENG_NO_CARTRIDGE   0x2000
#define CAPT_ENG_DOOR_OPEN      0x4000

/* ---------------------------------------------------------------------------
 * BufLevel macros
 * BufLevel from GetBasicStatus or GetExtendedStatus bytes 5-6.
 * Formula: (raw & 0xF) * (GetPrinterInfo_Buf << 15) / 15
 * The raw 4-bit count (0-15 slots) is in the low nibble.
 * ---------------------------------------------------------------------------*/
#define CAPT_BUFLEVEL_SLOTS(raw)  ((raw) & 0x0F)
#define CAPT_BUFLEVEL_MAX   15

/* ---------------------------------------------------------------------------
 * Paper availability (GetExtendedStatus byte 11)
 * ---------------------------------------------------------------------------*/
#define CAPT_PAP_READY      0x80  /* engine ready / paper available */
/* 0x00 = engine in paper-out error state */

/* ---------------------------------------------------------------------------
 * SetLEDStatus int_status codes
 * ---------------------------------------------------------------------------*/
#define CAPT_INTSTAT_READY      1   /* PrinterReadyToPrint */
#define CAPT_INTSTAT_PAUSED     4   /* CNPaused */
#define CAPT_INTSTAT_NO_PAPER   9   /* InputMediaSupplyEmpty */
#define CAPT_INTSTAT_XFER_ERR   11  /* CNDataXferError */
#define CAPT_INTSTAT_WRONG_SIZE 12  /* CNChangePaperSize */

/* ---------------------------------------------------------------------------
 * SetLEDStatus hs_code values
 * ---------------------------------------------------------------------------*/
#define CAPT_HSCODE_READY       0
#define CAPT_HSCODE_NO_PAPER    1
#define CAPT_HSCODE_WRONG_SIZE  2
#define CAPT_HSCODE_XFER_ERR    4
#define CAPT_HSCODE_PAUSED      5

/* ---------------------------------------------------------------------------
 * SetLEDStatus HostErr flags
 * ---------------------------------------------------------------------------*/
#define CAPT_HOSTERR_NONE       0x00000000
#define CAPT_HOSTERR_NO_PAPER   0x00010000
#define CAPT_HOSTERR_XFER_ERR   0x00008000
#define CAPT_HOSTERR_WRONG_SIZE 0x00080000
#define CAPT_HOSTERR_PAUSED     0x01000000
#define CAPT_HOSTERR_CLEANING   0x00000004

/* ---------------------------------------------------------------------------
 * SetJobInfo2 JobFlag values
 * ---------------------------------------------------------------------------*/
#define CAPT_JOBFLAG_START  1   /* job start */
#define CAPT_JOBFLAG_CONT   2   /* job continuation */
#define CAPT_JOBFLAG_END    6   /* job end (Windows canonical; use this) */
#define CAPT_JOBFLAG_END_LIN 3  /* job end (Linux driver) */
#define CAPT_JOBFLAG_ABORT  4   /* job abort (unconfirmed) */

/* ---------------------------------------------------------------------------
 * GoOnline magic payload (4 bytes, rest zeros)
 * ---------------------------------------------------------------------------*/
#define CAPT_GO_ONLINE_MAGIC "\xee\xdb\xea\xad"

void capt_init_status(void);
const struct capt_status_s *capt_get_status(void);
const struct capt_status_s *capt_get_xstatus_only(void);
const struct capt_status_s *capt_get_xstatus(void);
void capt_wait_ready(void);
void capt_wait_xready(void);
void capt_wait_xready_only(void);
