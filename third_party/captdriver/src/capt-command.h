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

//A2A7 GetIllegalConnectionStatus
//A3A1 GetFlashData2
//E0AE SetFlashData
//E0AD Cleaning


enum capt_command {
	CAPT_NOP                = 0xA0A0, /* ? */
	CAPT_GET_INPUT_STATUS   = 0xA0A1, /* GetInputStatus */
	CAPT_GET_EXTENDED_STATUS = 0xA0A8, /* GetExtendedStatus */

	CAPT_GET_IEEE1284_ID    = 0xA1A0, /* raw reply */
	CAPT_GET_PRINTER_INFO   = 0xA1A1, /* GetPrinterInfo */

	CAPT_RESERVE_UNIT       = 0xA2A0, /* ReserveUnit */

	CAPT_START_0            = 0xA3A2,

	CAPT_IC_VIDEO_DATA      = 0xC0A0, /* IC_VIDEO_DATA */
	CAPT_IC_BLACK_END       = 0xC0A4, /* IC_BLACK_END */

	CAPT_IC_BEGIN_PAGE      = 0xD0A0, /* IC_BEGIN_PAGE */
	CAPT_IC_BEGIN_DATA      = 0xD0A1, /* IC_BEGIN_DATA */
	CAPT_IC_END_PAGE        = 0xD0A2, /* IC_END_PAGE */
	CAPT_IC_BLACK_PLANE     = 0xD0A4, /* IC_BLACK_PLANE */
	CAPT_MULTI_COMMAND      = 0xD0A9, /* MultiCommand — wraps IC_BEGIN_PAGE+IC_BLACK_PLANE+IC_BEGIN_DATA+IC_END_PAGE */

	CAPT_GET_BASIC_STATUS   = 0xE0A0, /* GetBasicStatus */
	CAPT_CLEAR_ERROR        = 0xE0A2, /* ClearError */
	CAPT_CLEAR_MIS_PRINT    = 0xE0A3, /* ClearMisPrint */
	CAPT_DISCARD_DATA       = 0xE0A4, /* DiscardData */
	CAPT_GO_ONLINE          = 0xE0A5, /* GoOnline */
	CAPT_GO_OFFLINE         = 0xE0A6, /* GoOffline */
	CAPT_START_PRINT        = 0xE0A7, /* StartPrint */
	CAPT_RELEASE_UNIT       = 0xE0A9, /* ReleaseUnit */
	CAPT_SET_POWER_DOWN     = 0xE0BA,

	CAPT_SET_JOB_INFO2      = 0xE1A1, /* SetJobInfo2 */
	CAPT_SET_LED_STATUS     = 0xE1A2, /* SetLEDStatus */
};


const char *capt_identify(void);

void capt_send(uint16_t cmd, const void *data, size_t size);
void capt_sendrecv(uint16_t cmd, const void *buf, size_t size, void *reply, size_t *reply_size);

void capt_multi_begin(uint16_t cmd);
void capt_multi_add(uint16_t cmd, const void *data, size_t size);
void capt_multi_send(void);
void capt_cleanup(void);
