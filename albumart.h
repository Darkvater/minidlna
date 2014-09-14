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
} image_size_type_enum;

typedef struct
{
	image_size_type_enum type;
	const char* name;
	int width;
	int height;
} image_size_type_t;

void update_if_album_art(const char *path);
int64_t find_album_art(const char *path, uint8_t *image_data, int image_size);
const image_size_type_t *get_image_size_type(image_size_type_enum size_type);
char *get_path_from_image_size_type(const char *path, const image_size_type_t *image_size_type);
char *save_resized_album_art_to(const char *src_file, const char *dst_file, const image_size_type_t *image_size_type);

#endif
