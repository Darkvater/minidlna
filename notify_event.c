/* MiniDLNA media server
* Copyright (C) 2008-2010  Justin Maggard
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
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <libgen.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include "libav.h"
#include "upnpglobalvars.h"
#include "utils.h"
#include "sql.h"
#include "scanner.h"
#include "captions.h"
#include "metadata.h"
#include "albumart.h"
#include "log.h"
#include "notify_event.h"

int
notify_event_insert_directory(const char *name, const char *path, media_types types, insert_callback callback)
{
	char *id;
	struct dirent **namelist;
	enum file_types type;
	int n;
	char path_buf[PATH_MAX];

	DPRINTF(E_DEBUG, L_INOTIFY, "Scanning to add directory %s\n", path);

	if (access(path, R_OK | X_OK) != 0)
	{
		DPRINTF(E_WARN, L_INOTIFY, "Could not access %s [%s]\n", path, strerror(errno));
		return -1;
	}
	if (sql_get_int_field(db, "SELECT ID from DETAILS where PATH = '%q'", path) > 0)
	{
		DPRINTF(E_DEBUG, L_INOTIFY, "%s already exists\n", path);
		return 0;
	}

	DPRINTF(E_INFO, L_INOTIFY, "Processing adding directory event [%s]\n", path);

	x_strlcpy(path_buf, path, sizeof(path_buf));
	id = sql_get_text_field(db, "SELECT OBJECT_ID from OBJECTS o left join DETAILS d on (d.ID = o.DETAIL_ID)"
		" where d.PATH = %Q and REF_ID is NULL", dirname(path_buf));
	if (id == NULL) id = sqlite3_mprintf("%s", BROWSEDIR_ID);
	insert_directory(name, path, BROWSEDIR_ID, id + 2, get_next_available_id("OBJECTS", id));
	sqlite3_free(id);

	if (callback != NULL) callback(path);

	n = get_directory_entries(&namelist, path, types);
	if (n < 0)
	{
		DPRINTF(E_WARN, L_INOTIFY, "Error scanning %s\n", path);
		return -1;
	}

	for (n--; n >= 0; n--)
	{
		char *esc_name = escape_tag(namelist[n]->d_name, 1);
		snprintf(path_buf, sizeof(path_buf), "%s/%s", path, namelist[n]->d_name);
		type = resolve_unknown_type(path_buf, types);

		if (type == TYPE_DIR)
		{
			notify_event_insert_directory(esc_name, path_buf, types, callback);
		}
		else if (type == TYPE_FILE)
		{
			struct stat st;
			if ((stat(path_buf, &st) == 0) && (st.st_blocks << 9 >= st.st_size))
			{
				notify_event_insert_file(esc_name, path_buf, types);
			}
		}
		free(esc_name);
		free(namelist[n]);
	}
	free(namelist);

	return 0;
}

int
notify_event_remove_directory(const char *path)
{
	char * sql;
	char **result;
	int nrows, ret = 1;

	DPRINTF(E_INFO, L_INOTIFY, "Processing remove directory event [%s]\n", path);

	/* Invalidate the scanner cache so we don't insert files into non-existent containers */
	valid_cache = 0;
	sql = sqlite3_mprintf("SELECT ID from DETAILS where (PATH > '%q/' and PATH <= '%q/%c') or PATH = %Q", path, path, 0xFF, path);
	if (sql_get_table(db, sql, &result, &nrows, NULL) == SQLITE_OK)
	{
		for (; nrows > 0; nrows--)
		{
			int64_t detailID = strtoll(result[nrows], NULL, 10);
			sql_exec(db, "DELETE from DETAILS where ID = %lld", detailID);
			sql_exec(db, "DELETE from OBJECTS where DETAIL_ID = %lld", detailID);
			ret = 0;
		}
		sqlite3_free_table(result);
	}
	sqlite3_free(sql);
	/* Clean up any album art entries in the deleted directory */
	sql_exec(db, "DELETE from ALBUM_ART where (PATH > '%q/' and PATH <= '%q/%c')", path, path, 0xFF);

	return ret;
}

int
notify_event_insert_file(char *name, const char *path, media_types types)
{
	int len;
	char * last_dir;
	char * path_buf;
	char * base_name;
	char * id = NULL;
	int depth = 1;
	int ts;
	struct stat st;

	DPRINTF(E_DEBUG, L_INOTIFY, "Scanning to add file %s\n", path);

	/* Is it cover art for another file? */
	if (is_image(path))
		album_art_update_cond(path);
	else if (is_caption(path))
		add_caption_if_has_media(path);
	else if (is_metadata(path))
		check_for_metadata(path);

	/* Check if we're supposed to be scanning for this file type in this directory */
	if (resolve_unknown_type(path, types) != TYPE_FILE) return -1;

	/* If it's already in the database and hasn't been modified, skip it. */
	if (stat(path, &st) != 0)
		return -1;

	DPRINTF(E_INFO, L_INOTIFY, "Processing add file event [%s]\n", path);

	ts = sql_get_int_field(db, "SELECT TIMESTAMP from DETAILS where PATH = %Q", path);
	if (!ts && is_playlist(path) && (sql_get_int_field(db, "SELECT ID from PLAYLISTS where PATH = %Q", path) > 0))
	{
		notify_event_remove_file(path);
	}
	else if (ts < st.st_mtime)
	{
		if (ts > 0)
			DPRINTF(E_DEBUG, L_INOTIFY, "%s is newer than the last db entry.\n", path);
		notify_event_remove_file(path);
	}

	/* Find the parentID.  If it's not found, create all necessary parents. */
	len = strlen(path) + 1;
	if (!(path_buf = malloc(len)) ||
		!(last_dir = malloc(len)) ||
		!(base_name = malloc(len)))
		return -1;

	while (depth)
	{
		depth = 0;
		strcpy(path_buf, path);
		char *parent_buf = dirname(path_buf);

		do
		{
			//DEBUG DPRINTF(E_DEBUG, L_INOTIFY, "Checking %s\n", parent_buf);
			id = sql_get_text_field(db, "SELECT OBJECT_ID from OBJECTS o left join DETAILS d on (d.ID = o.DETAIL_ID)"
				" where d.PATH = '%q' and REF_ID is NULL", parent_buf);
			if (id)
			{
				if (!depth) break;
				DPRINTF(E_DEBUG, L_INOTIFY, "Found first known parentID: %s [%s]\n", id, parent_buf);
				/* Insert newly-found directory */
				strcpy(base_name, last_dir);
				insert_directory(basename(base_name), last_dir, BROWSEDIR_ID, id + 2, get_next_available_id("OBJECTS", id));
				sqlite3_free(id);
				break;
			}
			depth++;
			strcpy(last_dir, parent_buf);
			parent_buf = dirname(parent_buf);
		} while (strcmp(parent_buf, "/") != 0);

		if (strcmp(parent_buf, "/") == 0)
		{
			id = sqlite3_mprintf("%s", BROWSEDIR_ID);
			break;
		}
	}
	free(last_dir);
	free(path_buf);
	free(base_name);

	//DEBUG DPRINTF(E_DEBUG, L_INOTIFY, "Inserting %s\n", name);
	insert_file(name, path, id + 2, get_next_available_id("OBJECTS", id), types);
	sqlite3_free(id);
	return 0;
}

int
notify_event_remove_file(const char *path)
{
	char art_cache[PATH_MAX];
	int64_t detailID;
	int playlist;

	DPRINTF(E_DEBUG, L_INOTIFY, "Scanning to remove file %s\n", path);

	if (is_caption(path)) return delete_caption(path);

	playlist = is_playlist(path);
	detailID = sql_get_int64_field(db, "SELECT ID from %s where PATH = %Q", playlist ? "PLAYLISTS" : "DETAILS", path);
	if (detailID == 0) return 1;

	/* Invalidate the scanner cache so we don't insert files into non-existent containers */
	valid_cache = 0;
	DPRINTF(E_INFO, L_INOTIFY, "Processing remove file event [%s]\n", path);

	if (playlist)
	{
		sql_exec(db, "DELETE from PLAYLISTS where ID = %lld", detailID);
		sql_exec(db, "DELETE from DETAILS where ID ="
			" (SELECT DETAIL_ID from OBJECTS where OBJECT_ID = '%s$%llX')",
			MUSIC_PLIST_ID, detailID);
		sql_exec(db, "DELETE from OBJECTS where OBJECT_ID = '%s$%llX' or PARENT_ID = '%s$%llX'",
			MUSIC_PLIST_ID, detailID, MUSIC_PLIST_ID, detailID);
	}
	else
	{
		char sql[128];
		char **result;
		int nrows;

		/* Delete the parent containers if we are about to empty them. */
		snprintf(sql, sizeof(sql), "SELECT PARENT_ID from OBJECTS where DETAIL_ID = %lld"
			" and PARENT_ID not like '"BROWSEDIR_ID"$%%'",
			(long long int)detailID);
		if (sql_get_table(db, sql, &result, &nrows, NULL) == SQLITE_OK)
		{
			int children;
			for (; nrows > 0; nrows--)
			{
				/* If it's a playlist item, adjust the item count of the playlist */
				if (strncmp(result[nrows], MUSIC_PLIST_ID, strlen(MUSIC_PLIST_ID)) == 0)
				{
					sql_exec(db, "UPDATE PLAYLISTS set FOUND = (FOUND-1) where ID = %d", atoi(strrchr(result[nrows], '$') + 1));
				}

				children = sql_get_int_field(db, "SELECT count(*) from OBJECTS where PARENT_ID = '%s'", result[nrows]);
				if (children < 0)
					continue;
				if (children < 2)
				{
					char *ptr = strrchr(result[nrows], '$');

					sql_exec(db, "DELETE from OBJECTS where OBJECT_ID = '%s'", result[nrows]);
					if (ptr)
						*ptr = '\0';
					if (sql_get_int_field(db, "SELECT count(*) from OBJECTS where PARENT_ID = '%s'", result[nrows]) == 0)
					{
						sql_exec(db, "DELETE from OBJECTS where OBJECT_ID = '%s'", result[nrows]);
					}
				}
			}
			sqlite3_free_table(result);
		}
		/* Now delete the actual objects */
		sql_exec(db, "DELETE from DETAILS where ID = %lld", detailID);
		sql_exec(db, "DELETE from OBJECTS where DETAIL_ID = %lld", detailID);
	}

	snprintf(art_cache, sizeof(art_cache), "%s/art_cache%s", db_path, path);
	remove(art_cache);

	return 0;
}
