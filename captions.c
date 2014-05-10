/* MiniDLNA media server
* Copyright (C) 2008-2009  Justin Maggard
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

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <libgen.h>
#include <string.h>
#include <sys/param.h>
#include <dirent.h>

#include "upnpglobalvars.h"
#include "utils.h"
#include "captions.h"
#include "sql.h"
#include "log.h"

static void
add_caption(const char *path, int64_t detailID)
{
	if (detailID && access(path, R_OK) == 0)
	{
		DPRINTF(E_DEBUG, L_METADATA, "Adding caption file: %s\n", path);
		sql_exec(db, "INSERT into CAPTIONS"
			" (ID, PATH) "
			"VALUES"
			" (%lld, %Q)", detailID, path);
	}
}

static char *
get_filename_only(char *path)
{
	path = basename(path);
	strip_ext(path);
	return path;
}

void
check_for_captions(const char *path, int64_t detailID)
{
	char *mypath = strdup(path);
	char *dir_name = strdup(dirname(mypath));
	char *media_name;
	DIR *dirp;
	struct dirent *dp;

	DPRINTF(E_MAXDEBUG, L_METADATA, "Looking for caption for media %s.\n", path);

	dirp = opendir(dir_name);
	if (dirp == NULL) goto return_from_function;

	strncpyt(mypath, path, strlen(path));
	media_name = get_filename_only(mypath);
	while ((dp = readdir(dirp)) != NULL)
	{
		const char *filename = dp->d_name;
		if (is_caption(filename)) {
			DPRINTF(E_MAXDEBUG, L_METADATA, "New file %s looks like a caption file.\n", filename);
			if (strncmp(media_name, filename, strlen(media_name)) == 0)
			{
				char caption_path[MAXPATHLEN];
				snprintf(caption_path, sizeof(caption_path), "%s/%s", dir_name, filename);
				add_caption(caption_path, detailID);
			}
		}
	}

	closedir(dirp);
return_from_function:
	free(dir_name);
	free(mypath);
}

void
add_caption_if_has_media(const char *path)
{
	char *file = strdup(path);
	int nRows = 0;

	DPRINTF(E_MAXDEBUG, L_METADATA, "New file %s looks like a caption file.\n", path);

	/* caption must match media name plus dot then anything until the extension
	 * this allows for adding language suffixes to captions */
	for (; nRows == 0 && strip_ext(file) != NULL;) {
		char buf[MAXPATHLEN];
		char **sql_result = NULL;

		snprintf(buf, sizeof(buf), "SELECT ID, PATH from DETAILS where (PATH > '%s.' and PATH <= '%s.z') and MIME glob 'video/*' limit 1", file, file);
		sql_get_table(db, buf, &sql_result, &nRows, NULL);

		// found a potential match, but must see if actually matches media name
		if (nRows == 1)
		{
			char *caption_path = strdup(path);
			char *caption = get_filename_only(caption_path);
			char *media = get_filename_only(sql_result[3]);

			DPRINTF(E_MAXDEBUG, L_METADATA, "New file %s looks like a caption file matching media %s.\n", path, media);

			if (strncmp(media, caption, strlen(media)) == 0) {
				char *id = sql_result[2];
				int64_t detailID = strtoll(id, NULL, 10);
				add_caption(path, detailID);
			}
			free(caption_path);
		}
		sqlite3_free_table(sql_result);
	};

	free(file);
	if (nRows == 0) DPRINTF(E_MAXDEBUG, L_METADATA, "No media file found for caption %s.\n", path);
}
