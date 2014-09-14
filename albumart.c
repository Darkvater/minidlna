/* MiniDLNA media server
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
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <limits.h>
#include <libgen.h>
#include <setjmp.h>
#include <errno.h>

#include <jpeglib.h>

#include "upnpglobalvars.h"
#include "albumart.h"
#include "sql.h"
#include "utils.h"
#include "image_utils.h"
#include "log.h"

image_size_type_t image_size_types[] = {
	{ JPEG_TN, "JPEG_TN", 160, 160 },
	{ JPEG_SM, "JPEG_SM", 640, 480 },
	{ JPEG_MED, "JPEG_MED", 1024, 768 },
	{ JPEG_LRG, "JPEG_LRG", 4096, 4096 },
	{ JPEG_INV, "", 0, 0 }
};

const image_size_type_t *get_image_size_type(image_size_type_enum size_type)
{
	if (size_type < JPEG_TN || size_type > JPEG_MED) size_type = JPEG_INV;
	return &image_size_types[size_type];
}

char *get_path_from_image_size_type(const char *path, const image_size_type_t *image_size_type)
{
	char *albumart_path;
	xasprintf(&albumart_path, "%s.%s.jpg", path, image_size_type->name);
	return albumart_path;
}

static int
art_cache_exists(const char *orig_path, char **cache_file)
{
	if( xasprintf(cache_file, "%s/art_cache%s", db_path, orig_path) < 0 )
		return 0;

	strcpy(strchr(*cache_file, '\0')-4, ".jpg");

	return (!access(*cache_file, F_OK));
}

static int
link_file(const char *src_file, const char *dst_file)
{
	if (link(src_file, dst_file) == 0)
	{
		return 0;
	}
	else
	{
		if (errno == ENOENT)
		{
			char *dir = strdup(dst_file);
			make_dir(dirname(dir), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
			free(dir);
			if (link(src_file, dst_file) == 0)
				return 0;
		}
		DPRINTF(E_WARN, L_METADATA, "Linking %s to %s failed [%s]\n", src_file, dst_file, strerror(errno));
	}
	return -1;
}

static char *
save_album_art(const image_s *imsrc, const char *path)
{
	char *cache_file;
	char cache_dir[MAXPATHLEN];

	if (!imsrc)
		return NULL;

	if (art_cache_exists(path, &cache_file))
		return cache_file;

	strncpyt(cache_dir, cache_file, sizeof(cache_dir));
	make_dir(dirname(cache_dir), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);

	return image_save_to_jpeg_file(imsrc, cache_file);
}

static char *
save_resized_album_art_from_imsrc_to(const image_s *imsrc, const char *src_file, const char *dst_file, const image_size_type_t *image_size_type)
{
	int dstw, dsth;

	if (!imsrc || !image_size_type)
		return NULL;

	if (imsrc->width > imsrc->height)
	{
		dstw = image_size_type->width;
		dsth = (imsrc->height << 8) / ((imsrc->width << 8) / dstw);
	}
	else
	{
		dsth = image_size_type->height;
		dstw = (imsrc->width << 8) / ((imsrc->height << 8) / dsth);
	}

	if (dstw > imsrc->width && dsth > imsrc->height)
	{
		int ret = link_file(src_file, dst_file);
		return (ret == 0) ? (char*)dst_file : image_save_to_jpeg_file(imsrc, dst_file);
	}


	image_s *imdst = image_resize(imsrc, dstw, dsth);
	char *result = image_save_to_jpeg_file(imdst, dst_file);
	image_free(imdst);

	return result;
}

char *
save_resized_album_art_to(const char *src_file, const char *dst_file, const image_size_type_t *image_size_type)
{
	image_s *imsrc = image_new_from_jpeg(src_file, 1, NULL, 0, 1, ROTATE_NONE);
	char *dst = save_resized_album_art_from_imsrc_to(imsrc, src_file, dst_file, image_size_type);
	free(imsrc);
	return dst;
}

static void
save_resized_album_art(const image_s *imsrc, const char *path, const image_size_type_t *image_size_type)
{
	char *cache_file, *resized_cache_file;

	if (!imsrc || !image_size_type)
		return;

	art_cache_exists(path, &cache_file);
	resized_cache_file = get_path_from_image_size_type(cache_file, image_size_type);

	save_resized_album_art_from_imsrc_to(imsrc, path, resized_cache_file, image_size_type);
	free(resized_cache_file);
}

/* And our main album art functions */
void
update_if_album_art(const char *path)
{
	char *dir;
	char *match;
	char file[MAXPATHLEN];
	char fpath[MAXPATHLEN];
	char dpath[MAXPATHLEN];
	int ncmp = 0;
	int album_art;
	DIR *dh;
	struct dirent *dp;
	enum file_types type = TYPE_UNKNOWN;
	int64_t art_id = 0;
	int ret;

	strncpyt(fpath, path, sizeof(fpath));
	match = basename(fpath);
	/* Check if this file name matches a specific audio or video file */
	if( ends_with(match, ".cover.jpg") )
	{
		ncmp = strlen(match)-10;
	}
	else
	{
		ncmp = strrchr(match, '.') - match;
	}
	/* Check if this file name matches one of the default album art names */
	album_art = is_album_art(match);

	strncpyt(dpath, path, sizeof(dpath));
	dir = dirname(dpath);
	dh = opendir(dir);
	if( !dh )
		return;
	while ((dp = readdir(dh)) != NULL)
	{
		if (is_reg(dp) == 1)
		{
			type = TYPE_FILE;
		}
		else if (is_dir(dp) == 1)
		{
			type = TYPE_DIR;
		}
		else
		{
			snprintf(file, sizeof(file), "%s/%s", dir, dp->d_name);
			type = resolve_unknown_type(file, ALL_MEDIA);
		}
		if( type != TYPE_FILE )
			continue;
		if( (dp->d_name[0] != '.') &&
		    (is_video(dp->d_name) || is_audio(dp->d_name)) &&
		    (album_art || strncmp(dp->d_name, match, ncmp) == 0) )
		{
			DPRINTF(E_DEBUG, L_METADATA, "New file %s looks like cover art for %s\n", path, dp->d_name);
			snprintf(file, sizeof(file), "%s/%s", dir, dp->d_name);
			art_id = find_album_art(file, NULL, 0);
			ret = sql_exec(db, "UPDATE DETAILS set ALBUM_ART = %lld where PATH = '%q'", (long long)art_id, file);
			if( ret != SQLITE_OK )
				DPRINTF(E_WARN, L_METADATA, "Error setting %s as cover art for %s\n", match, dp->d_name);
		}
	}
	closedir(dh);
}

char *
check_embedded_art(const char *path, uint8_t *image_data, int image_size)
{
	char *art_path = NULL;
	image_s *imsrc;
	static char last_path[PATH_MAX];
	static unsigned int last_hash = 0;
	static int last_success = 0;
	unsigned int hash;

	if( !image_data || !image_size || !path )
	{
		return NULL;
	}
	/* If the embedded image matches the embedded image from the last file we
	 * checked, just make a hard link.  Better than storing it on the disk twice. */
	hash = DJBHash(image_data, image_size);
	if( hash == last_hash )
	{
		if( !last_success )
			return NULL;
		art_cache_exists(path, &art_path);

		int ret = link_file(last_path, art_path);
		if (ret == 0)
		{
			imsrc = image_new_from_jpeg(NULL, 0, image_data, image_size, 1, ROTATE_NONE);
			goto save_resized;
		}
		free(art_path);
	}
	last_hash = hash;

	imsrc = image_new_from_jpeg(NULL, 0, image_data, image_size, 1, ROTATE_NONE);
	if( !imsrc )
	{
		last_success = 0;
		return NULL;
	}

	art_path = save_album_art(imsrc, path);
save_resized:
	/* add a thumbnail version anticipiating a bit for the most likely access.
	 * The webservice will generate other thumbs on the fly if not available */
	save_resized_album_art(imsrc, path, get_image_size_type(JPEG_TN));

	image_free(imsrc);
	if( !art_path )
	{
		DPRINTF(E_WARN, L_METADATA, "Invalid embedded album art in %s\n", basename((char *)path));
		last_success = 0;
		return NULL;
	}
	DPRINTF(E_DEBUG, L_METADATA, "Found new embedded album art in %s\n", basename((char *)path));
	last_success = 1;
	strcpy(last_path, art_path);

	return(art_path);
}

static char *
check_for_album_file(const char *path)
{
	char file[MAXPATHLEN];
	char mypath[MAXPATHLEN];
	struct album_art_name_s *album_art_name;
	char *p;
	const char *dir;
	struct stat st;
	int ret;

	if( stat(path, &st) != 0 )
		return NULL;

	if( S_ISDIR(st.st_mode) )
	{
		dir = path;
		goto check_dir;
	}
	strncpyt(mypath, path, sizeof(mypath));
	dir = dirname(mypath);

	/* First look for file-specific cover art */
	snprintf(file, sizeof(file), "%s.cover.jpg", path);
	ret = access(file, R_OK);
	if( ret != 0 )
	{
		strncpyt(file, path, sizeof(file));
		p = strrchr(file, '.');
		if( p )
		{
			strcpy(p, ".jpg");
			ret = access(file, R_OK);
		}
		if( ret != 0 )
		{
			p = strrchr(file, '/');
			if( p )
			{
				memmove(p+2, p+1, file+MAXPATHLEN-p-2);
				p[1] = '.';
				ret = access(file, R_OK);
			}
		}
	}
	if (ret == 0) goto add_cached_image;

check_dir:
	/* Then fall back to possible generic cover art file names */
	for (album_art_name = album_art_names; album_art_name; album_art_name = album_art_name->next)
	{
		snprintf(file, sizeof(file), "%s/%s", dir, album_art_name->name);
		if (access(file, R_OK) == 0)
add_cached_image:
		{
			image_s *imsrc = image_new_from_jpeg(file, 1, NULL, 0, 1, ROTATE_NONE);
			char *art_file = save_album_art(imsrc, file);
			/* add a thumbnail version anticipiating a bit for the most likely access.
			* The webservice will generate other thumbs on the fly if not available */
			save_resized_album_art(imsrc, file, get_image_size_type(JPEG_TN));
			free(imsrc);
			return art_file;
		}
	}

	return NULL;
}

int64_t
find_album_art(const char *path, uint8_t *image_data, int image_size)
{
	
	char *album_art = check_embedded_art(path, image_data, image_size);
	if (album_art == NULL) album_art = check_for_album_file(path);
	if (album_art == NULL) return 0;

	int64_t ret = sql_get_int_field(db, "SELECT ID from ALBUM_ART where PATH = '%q'", album_art);
	if (ret == 0)
	{
		if (sql_exec(db, "INSERT into ALBUM_ART (PATH) VALUES ('%q')", album_art) == SQLITE_OK)
		{
			ret = sqlite3_last_insert_rowid(db);
		}
		else
		{
			DPRINTF(E_WARN, L_METADATA, "Error setting %s as cover art for %s\n", album_art, path);
			ret = 0;
		}
	}
	
	free(album_art);
	return ret;
}
