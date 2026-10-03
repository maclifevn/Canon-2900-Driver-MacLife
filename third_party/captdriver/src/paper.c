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

#include "paper.h"
#include <cups/raster.h>
#include <stdio.h>

void page_set_dims(struct page_dims_s *dims, const struct cups_page_header2_s *header)
{
	dims->media_type = header->cupsMediaType;
	strncpy(dims->media_size, header->MediaType, sizeof(dims->media_size) - 1);
	dims->media_size[sizeof(dims->media_size) - 1] = '\0';
	dims->paper_width  = header->cupsPageSize[0] * header->HWResolution[0] / 72.0f;
	dims->paper_height = header->cupsPageSize[1] * header->HWResolution[1] / 72.0f;
	dims->toner_save = header->cupsInteger[0];
	dims->ink_k = header->cupsInteger[1];
	dims->manual_duplex = header->cupsInteger[2];
	dims->line_size = header->cupsBytesPerLine;
	dims->num_lines = header->cupsHeight;
	dims->band_size = header->cupsRowCount;
	dims->margin_height = (header->cupsPageSize[1] - header->cupsImagingBBox[3]) / 72.0f * header->HWResolution[1];
	dims->margin_width = header->cupsImagingBBox[0] / 72.0f * header->HWResolution[0];

	if (header->HWResolution[1] == 400)
	  dims->media_adapt  = 0x81;
	else
	  dims->media_adapt  = 0x11;
}
