/* MiniDLNA media server
 * Copyright (C) 2017  Edmunt Pienkowsky
 *
 * This file is part of MiniDLNA.
 *
 * MiniDLNA is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * MiniDLNA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MiniDLNA. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __FFIMG_H__
#define __FFIMG_H__

typedef struct
{
	enum AVCodecID id;
	AVPacket *packet;
	AVFrame *frame;
} ffimg_t;

ffimg_t *ffimg_alloc();
void ffimg_free(ffimg_t *img);
int ffimg_is_valid(const ffimg_t *img);
ffimg_t *ffimg_load_from_file(const char *imgpath);
ffimg_t *ffimg_load_from_blob(const void *data, size_t data_size);
ffimg_t *ffimg_resize(const ffimg_t *img, int width, int height, int to_jpeg);
ffimg_t *ffimg_clone(const ffimg_t *img);
int ffimg_is_supported(const ffimg_t *img);
void ffimg_get_dimensions(const ffimg_t *img, int *width, int *height);

#endif
