/* Album art extraction, caching, and scaling
 *
 * Project : minidlna
 * Website : http://sourceforge.net/projects/minidlna/
 * Author  : Justin Maggard
 *
 * MiniDLNA media server
 * Copyright (C) 2008  Justin Maggard
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
#ifndef __ALBUMART_H__
#define __ALBUMART_H__

typedef enum {
	JPEG_TN,
	JPEG_SM,
	JPEG_MED,
	JPEG_LRG,
	JPEG_INV
} image_size_enum;

typedef union
{
	struct
	{
		uint8_t *data;
		size_t size;
	} blob;
	char *path;
} blob_or_path_t;

typedef struct
{
	int is_blob;
	blob_or_path_t image;
	uint32_t checksum;
	time_t timestamp;
} album_art_t;

const char* album_art_get_size_name(image_size_enum image_size);
int64_t album_art_add(const char *path, const uint8_t *image_data, size_t image_data_size);
int album_art_check(int64_t album_art_id);
album_art_t *album_art_get(int64_t album_art_id, image_size_enum image_size);
int64_t album_art_create_sized(int64_t album_art_id, image_size_enum image_size);
void album_art_update_cond(const char *path);
void album_art_free(album_art_t *album_art);

#endif
