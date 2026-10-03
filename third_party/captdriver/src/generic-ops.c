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

#include "generic-ops.h"

#include "std.h"
#include "word.h"
#include "hiscoa-common.h"
#include "hiscoa-compress.h"
#include "capt-command.h"
#include "capt-status.h"
#include "printer.h"

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

size_t ops_compress_band_hiscoa(struct printer_state_s *state,
		void *band, size_t size,
		const void *pixels, unsigned line_size, unsigned num_lines, unsigned last_band_page_end)
{
	(void) state;
	return hiscoa_compress_band(band, size, pixels, line_size, num_lines,
					last_band_page_end, &hiscoa_default_params);
}

void ops_send_band_hiscoa(struct printer_state_s *state, const void *data, size_t size)
{
	const uint8_t *pdata = (const uint8_t *) data;
	const struct capt_status_s *status;
	int buflevel_slots;
	int chunks_since_poll = 0;
	int poll_interval;

	/* Determine chunk size limit from GetPrinterInfo Blk; default 65520 for LBP3000.
	 * Also cap at 0xFF00 (65280) for safety with the USB framing layer. */
	size_t chunk_max = state->printer_blk ? (size_t)state->printer_blk : 65520;
	if (chunk_max > 0xFF00)
		chunk_max = 0xFF00;

	/* Read initial BufLevel from GetBasicStatus. */
	status = capt_get_status();
	buflevel_slots = CAPT_BUFLEVEL_SLOTS(status->buf_level);
	fprintf(stderr, "DEBUG: CAPT: IC_VIDEO_DATA start, size=%u, chunk_max=%u, buflevel=%d\n",
		(unsigned)size, (unsigned)chunk_max, buflevel_slots);

	while (size) {
		size_t send = chunk_max;
		if (send > size)
			send = size;

		/* BufLevel-based back-pressure (protocol §2.18, §9 note 3).
		 * BufLevel == 0 means the printer buffer is completely full. */
		if (buflevel_slots == 0) {
			if (size > 0 && !state->startprint_sent) {
				/* Streaming mode: fire StartPrint(N) only after Start==N is confirmed.
				 * GetExtendedStatus is required — GetBasicStatus does NOT update
				 * page_decoding (Start counter). Spec §4.4: Start==N exact match. */
				uint8_t spbuf[2] = { LO(state->ipage), HI(state->ipage) };
				for (int w = 0; w < 20 && status->page_decoding != state->ipage; w++) {
					status = capt_get_xstatus_only();
					usleep(50000);
				}
				if (status->page_decoding == state->ipage) {
					fprintf(stderr, "DEBUG: CAPT: streaming mode: BufLevel=0, sending StartPrint(%u)\n",
						state->ipage);
					capt_sendrecv(CAPT_START_PRINT, spbuf, 2, NULL, 0);
					state->startprint_sent = true;
				}
			}
			/* Poll until BufLevel rises to >= 1.
			 * Track byte1 changes: when byte1 changes, call GetExtendedStatus so
			 * the driver observes Start counter advancing to N+1 during drain.
			 * Spec §4.3: optionally call GetExtendedStatus on byte1 change. */
			{
				uint8_t prev_byte1 = (uint8_t)(status->status[0] & 0xFF);
				unsigned drain_attempts = 0;
				do {
					status = capt_get_status();
					buflevel_slots = CAPT_BUFLEVEL_SLOTS(status->buf_level);
					{
						uint8_t cur_byte1 = (uint8_t)(status->status[0] & 0xFF);
						if (cur_byte1 != prev_byte1) {
							prev_byte1 = cur_byte1;
							status = capt_get_xstatus_only();
							buflevel_slots = CAPT_BUFLEVEL_SLOTS(status->buf_level);
							if (FLAG(status, CAPT_FL_NEED_INPUT_STATUS))
								capt_sendrecv(CAPT_GET_INPUT_STATUS, NULL, 0, NULL, 0);
						}
					}
					if (buflevel_slots == 0)
						capt_poll_wait(&drain_attempts, "USB buffer space");
				} while (buflevel_slots == 0);
			}
			chunks_since_poll = 0;
		}

		capt_send(CAPT_IC_VIDEO_DATA, pdata, send);
		pdata += send;
		size -= send;
		state->isend += 1;
		chunks_since_poll++;

		/* Adaptive polling interval: more frequent as BufLevel drops.
		 * buflevel >= 15: burst mode, poll every 5 chunks.
		 * buflevel  8-14: moderate, poll every 2 chunks.
		 * buflevel  1- 7: nearly full, poll every chunk.
		 * (protocol flow document: 7/8 boundary) */
		if (buflevel_slots >= 15)
			poll_interval = 5;
		else if (buflevel_slots >= 8)
			poll_interval = 2;
		else
			poll_interval = 1;

		if (chunks_since_poll >= poll_interval) {
			status = capt_get_status();
			buflevel_slots = CAPT_BUFLEVEL_SLOTS(status->buf_level);
			/* Honour secondary-flag requests from GetBasicStatus byte 2. */
			if (FLAG(status, CAPT_FL_XSTATUS_CHANGED))
				capt_get_xstatus_only();
			if (FLAG(status, CAPT_FL_NEED_INPUT_STATUS))
				capt_sendrecv(CAPT_GET_INPUT_STATUS, NULL, 0, NULL, 0);
			chunks_since_poll = 0;
		}
	}

	/* All chunks sent. Check if we need to send StartPrint for streaming mode.
	 * This handles the case where BufLevel dropped to 0 after sending the last chunk,
	 * so the streaming check at line 70 never triggered. Per spec §4.4, streaming mode
	 * requires StartPrint BEFORE IC_BLACK_END to tell engine to consume buffer. */
	if (buflevel_slots == 0 && !state->startprint_sent) {
		/* Poll GetExtendedStatus to confirm Start==N before sending StartPrint.
		 * Spec §4.4: StartPrint requires Start==N exact match. */
		uint8_t spbuf[2] = { LO(state->ipage), HI(state->ipage) };
		for (int w = 0; w < 20 && status->page_decoding != state->ipage; w++) {
			status = capt_get_xstatus_only();
			usleep(50000);
		}
		if (status->page_decoding == state->ipage) {
			fprintf(stderr, "DEBUG: CAPT: streaming mode (post-loop): BufLevel=0, sending StartPrint(%u)\n",
				state->ipage);
			capt_sendrecv(CAPT_START_PRINT, spbuf, 2, NULL, 0);
			state->startprint_sent = true;
		} else {
			fprintf(stderr, "WARNING: CAPT: BufLevel=0 after all chunks sent, but Start=%u != page=%u, cannot send StartPrint\n",
				status->page_decoding, state->ipage);
		}
	}

	/* Final BufLevel drain check for IC_BLACK_END.
	 * In streaming mode, StartPrint was sent above, so engine will consume and BufLevel will rise.
	 * In normal mode, BufLevel should already be >= 1. */
	if (buflevel_slots == 0) {
		uint8_t prev_byte1 = (uint8_t)(status->status[0] & 0xFF);
		unsigned drain_attempts = 0;
		do {
			status = capt_get_status();
			buflevel_slots = CAPT_BUFLEVEL_SLOTS(status->buf_level);
			/* Track byte1 changes during drain (spec §4.3, §8 observation). */
			uint8_t cur_byte1 = (uint8_t)(status->status[0] & 0xFF);
			if (cur_byte1 != prev_byte1) {
				prev_byte1 = cur_byte1;
				status = capt_get_xstatus_only();
				buflevel_slots = CAPT_BUFLEVEL_SLOTS(status->buf_level);
				if (FLAG(status, CAPT_FL_NEED_INPUT_STATUS))
					capt_sendrecv(CAPT_GET_INPUT_STATUS, NULL, 0, NULL, 0);
			}
			if (buflevel_slots == 0)
				capt_poll_wait(&drain_attempts, "final USB buffer drain");
		} while (buflevel_slots == 0);
	}

	fprintf(stderr, "DEBUG: CAPT: IC_VIDEO_DATA done, streaming=%d, buflevel=%d\n",
		(int)state->startprint_sent, buflevel_slots);
}
