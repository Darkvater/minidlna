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

#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <libgen.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/param.h>
#include <fcntl.h>

#include <libexif/exif-loader.h>
#include <jpeglib.h>
#include <setjmp.h>
#include "libav.h"

#include "upnpglobalvars.h"
#include "tagutils/tagutils.h"
#include "image_utils.h"
#include "upnpreplyparse.h"
#include "tivo_utils.h"
#include "metadata.h"
#include "captions.h"
#include "albumart.h"
#include "utils.h"
#include "sql.h"
#include "log.h"
#include "scanner.h"

#define FLAG_TITLE       0x00000001
#define FLAG_ARTIST      0x00000002
#define FLAG_ALBUM       0x00000004
#define FLAG_GENRE       0x00000008
#define FLAG_COMMENT     0x00000010
#define FLAG_CREATOR     0x00000020
#define FLAG_DATE        0x00000040
#define FLAG_DLNA_PN     0x00000080
#define FLAG_MIME        0x00000100
#define FLAG_DURATION    0x00000200
#define FLAG_RESOLUTION  0x00000400
#define FLAG_DESCRIPTION 0x00000800
#define FLAG_RATING      0x00001000
#define FLAG_AUTHOR      0x00002000
#define FLAG_TRACK       0x00004000
#define FLAG_DISC        0x00008000
#define FLAG_PUBLISHER   0x00010000

#define ALL_FLAGS        0xFFFFFFFF

/* Audio profile flags */
enum audio_profiles {
	PROFILE_AUDIO_UNKNOWN,
	PROFILE_AUDIO_MP3,
	PROFILE_AUDIO_AC3,
	PROFILE_AUDIO_WMA_BASE,
	PROFILE_AUDIO_WMA_FULL,
	PROFILE_AUDIO_WMA_PRO,
	PROFILE_AUDIO_MP2,
	PROFILE_AUDIO_PCM,
	PROFILE_AUDIO_AAC,
	PROFILE_AUDIO_AAC_MULT5,
	PROFILE_AUDIO_AMR
};

/* This function shamelessly copied from libdlna */
#define MPEG_TS_SYNC_CODE 0x47
#define MPEG_TS_PACKET_LENGTH 188
#define MPEG_TS_PACKET_LENGTH_DLNA 192 /* prepends 4 bytes to TS packet */
int
dlna_timestamp_is_present(const char *filename, int *raw_packet_size)
{
	unsigned char buffer[3*MPEG_TS_PACKET_LENGTH_DLNA];
	int fd, i;

	/* read file header */
	fd = open(filename, O_RDONLY);
	if( fd < 0 )
		return 0;
	i = read(fd, buffer, MPEG_TS_PACKET_LENGTH_DLNA*3);
	close(fd);
	if( i < 0 )
		return 0;
	for( i = 0; i < MPEG_TS_PACKET_LENGTH_DLNA; i++ )
	{
		if( buffer[i] == MPEG_TS_SYNC_CODE )
		{
			if (buffer[i + MPEG_TS_PACKET_LENGTH_DLNA] == MPEG_TS_SYNC_CODE &&
			    buffer[i + MPEG_TS_PACKET_LENGTH_DLNA*2] == MPEG_TS_SYNC_CODE)
			{
			        *raw_packet_size = MPEG_TS_PACKET_LENGTH_DLNA;
				if (buffer[i+MPEG_TS_PACKET_LENGTH] == 0x00 &&
				    buffer[i+MPEG_TS_PACKET_LENGTH+1] == 0x00 &&
				    buffer[i+MPEG_TS_PACKET_LENGTH+2] == 0x00 &&
				    buffer[i+MPEG_TS_PACKET_LENGTH+3] == 0x00)
					return 0;
				else
					return 1;
			} else if (buffer[i + MPEG_TS_PACKET_LENGTH] == MPEG_TS_SYNC_CODE &&
				   buffer[i + MPEG_TS_PACKET_LENGTH*2] == MPEG_TS_SYNC_CODE) {
			    *raw_packet_size = MPEG_TS_PACKET_LENGTH;
			    return 0;
			}
		}
	}
	*raw_packet_size = 0;
	return 0;
}

int64_t
get_detailID_from_path_without_suffix(const char *path, char suffix)
{
	char file[MAXPATHLEN];
	strncpyt(file, path, sizeof(file));
	char *p = strrchr(file, suffix);
	if (p) *p = '\0';
	
	return sql_get_int64_field(db, "SELECT ID from DETAILS where PATH glob '%q%c*' and MIME glob 'video/*' limit 1", file, suffix);
}

void
check_for_metadata(const char *path)
{
	if (ends_with(path, "tvshow.nfo"))
	{
		int nrows;
		char **result;
		char file[MAXPATHLEN];
		strncpyt(file, path, sizeof(file));
		char * buf = sqlite3_mprintf("SELECT ID from DETAILS where PATH glob '%q/*' and MIME glob 'video/*'", dirname(file));

		if (sql_get_table(db, buf, &result, &nrows, NULL) == SQLITE_OK)
		{
			int i;
			for (i = 1; i <= nrows; i++)
			{
				int64_t detailID = atoll(result[i]);
				GetNfoMetadata(path, detailID);
			}
			sqlite3_free_table(result);
		}
		sqlite3_free(buf);
		return;
	}

	int64_t detailID = get_detailID_from_path_without_suffix(path, '.');
	if (detailID <= 0 && ends_with(path, "movie.nfo")) detailID = get_detailID_from_path_without_suffix(path, '/');
	if (detailID <= 0) return;

	GetNfoMetadata(path, detailID);
}

char *
escape_unescaped_tag(const char *tag)
{
	char *esc_tag = unescape_tag(tag, 1);
	char *dest = escape_tag(esc_tag, 1);
	free(esc_tag);
	return dest;
}

void
assign_value_if_exists(char **dest, const char *val)
{
	if (val)
	{
		free(*dest);
		*dest = strdup(val);
	}
}

void
assign_integer_if_exists(unsigned int *dest, const char *val)
{
	if (val) *dest = atoi(val);
}

void
set_value_from_xml_if_exists(char **dest, struct NameValueParserData *xml, const char *name)
{
	char *val = GetValueFromNameValueList(xml, name);
	if (val)
	{
		free(*dest);
		*dest = escape_unescaped_tag(val);
	}
}

void
set_value_from_xml_if_exists_no_overwrite(char **dest, struct NameValueParserData *xml, const char *name)
{
	if (!*dest) set_value_from_xml_if_exists(dest, xml, name);
}

void
set_value_list_from_xml_if_exists(char **dest, struct NameValueParserData *xml, const char *name)
{
	char *result = calloc(MAXPATHLEN, 1);
	const struct NameValue *resume = NULL;
	char *val;

	while ((val = GetValueFromNameValueListWithResumeSupport(xml, name, &resume)))
	{
		char *escaped_val = escape_unescaped_tag(val);
		x_strlcat(result, ",", MAXPATHLEN);
		x_strlcat(result, escaped_val, MAXPATHLEN);
		free(escaped_val);
	}

	if (*result)
	{
		free(*dest);
		*dest = strdup(&result[1]); /* get rid of starting comma */
	}
	free(result);
}

void
set_value_list_from_xml_if_exists_no_overwrite(char **dest, struct NameValueParserData *xml, const char *name)
{
	if (!*dest) set_value_list_from_xml_if_exists(dest, xml, name);
}

static int
read_nfo_data_from_xml(const char *path, struct NameValueParserData *xml)
{
	char buf[65536];
	size_t max_buf_size = sizeof(buf);
	struct stat file;

	if (stat(path, &file) != 0 || file.st_size > max_buf_size)
	{
		DPRINTF(E_INFO, L_METADATA, "Not parsing very large .nfo file %s\n", path);
		return 1;
	}

	DPRINTF(E_DEBUG, L_METADATA, "Parsing .nfo file: %s\n", path);
	{
		size_t nread;
		FILE *nfo = fopen(path, "r");

		if (!nfo) return 1;

		nread = fread(buf, 1, max_buf_size, nfo);
		ParseNameValue(buf, nread, xml);
		fclose(nfo);
	}
	return 0;
}

static void
parse_movie_nfo(struct NameValueParserData *xml, metadata_t *m)
{
	if (strcmp("movie", GetValueFromNameValueList(xml, "rootElement")) != 0) return;

	set_value_from_xml_if_exists(&m->title, xml, "title");
	set_value_from_xml_if_exists(&m->date, xml, "year");
	set_value_from_xml_if_exists(&m->date, xml, "premiered");
	set_value_from_xml_if_exists(&m->comment, xml, "tagline");
	set_value_from_xml_if_exists(&m->description, xml, "plot");
	set_value_from_xml_if_exists(&m->creator, xml, "director");
	set_value_from_xml_if_exists(&m->publisher, xml, "studio");
	set_value_from_xml_if_exists(&m->rating, xml, "mpaa");
	set_value_list_from_xml_if_exists(&m->author, xml, "writer");
	set_value_list_from_xml_if_exists(&m->genre, xml, "genre");
	set_value_list_from_xml_if_exists(&m->artist, xml, "name");
	m->videotype = MOVIE;
}

static void
parse_tvshow_nfo(struct NameValueParserData *xml, metadata_t *m)
{
	if (strcmp("tvshow", GetValueFromNameValueList(xml, "rootElement")) != 0) return;

	set_value_from_xml_if_exists(&m->album, xml, "title");
	set_value_from_xml_if_exists_no_overwrite(&m->date, xml, "premiered");
	set_value_from_xml_if_exists_no_overwrite(&m->description, xml, "plot");
	set_value_from_xml_if_exists_no_overwrite(&m->creator, xml, "director");
	set_value_from_xml_if_exists_no_overwrite(&m->publisher, xml, "studio");
	set_value_from_xml_if_exists(&m->rating, xml, "mpaa");
	set_value_list_from_xml_if_exists_no_overwrite(&m->author, xml, "writer");
	set_value_list_from_xml_if_exists(&m->genre, xml, "genre");
	set_value_list_from_xml_if_exists(&m->artist, xml, "name");

	set_value_from_xml_if_exists(&m->date, xml, "capturedate"); // TiVO-specific
	m->videotype = TVSERIES;
}

static void
parse_tvepisode_nfo(struct NameValueParserData *xml, metadata_t *m)
{
	if (strcmp("episodedetails", GetValueFromNameValueList(xml, "rootElement")) != 0) return;

	const char *season = GetValueFromNameValueList(xml, "season");
	const char *episode = GetValueFromNameValueList(xml, "episode");
	const char *episode_title = GetValueFromNameValueList(xml, "episodetitle"); // TiVO-specific

	m->disc = season ? atoi(season) : 0;
	m->track = episode ? atoi(episode) : 0;
	set_value_from_xml_if_exists(&m->title, xml, "title");
	if (m->disc && m->track)
	{
		char *title = m->title;
		xasprintf(&m->title, "S%02dE%02d - %s", m->disc, m->track, title);
		free(title);
	}
	else if (episode_title)
	{
		char *title = m->title;
		xasprintf(&m->title, "%s - %s", title, episode_title);
		free(title);
	}

	set_value_list_from_xml_if_exists(&m->author, xml, "credits");
	set_value_from_xml_if_exists(&m->description, xml, "plot");
	set_value_from_xml_if_exists(&m->creator, xml, "director");
	set_value_from_xml_if_exists(&m->publisher, xml, "studio");
	set_value_from_xml_if_exists(&m->date, xml, "aired");
	m->videotype = TVEPISODE;
}

static void
parse_nfo(const char *path, metadata_t *m)
{
	char *root_element;
	struct stat st;
	struct NameValueParserData xml;
	if (read_nfo_data_from_xml(path, &xml) != 0) return;

	root_element = GetValueFromNameValueList(&xml, "rootElement");
	if (root_element == NULL) return;

	DPRINTF(E_MAXDEBUG, L_METADATA, ".nfo type: %s\n", root_element);
	parse_movie_nfo(&xml, m);
	parse_tvshow_nfo(&xml, m);
	parse_tvepisode_nfo(&xml, m);

	set_value_from_xml_if_exists(&m->date, &xml, "mime");

	if (m->videotype == 0)
	{
		DPRINTF(E_WARN, L_METADATA, "Not a valid .nfo file of type %s: %s\n", root_element, path);
	}

	ClearNameValueList(&xml);

	if (lstat(path, &st) == 0)
	{
		int64_t ret = sql_get_int64_field(db, "SELECT ID from METADATA where PATH = %Q", path);
		if (ret == 0)
		{
			sql_exec(db, "INSERT into METADATA (PATH, TIMESTAMP) VALUES (%Q, %d)", path, st.st_mtime);
		}
		else
		{
			sql_exec(db, "UPDATE METADATA set TIMESTAMP = %d where ID = %lld", st.st_mtime, ret);
		}
	}
}

void
check_for_nfo_name(const char *path, const char *name, metadata_t *m)
{
	char *nfo = malloc(MAXPATHLEN);
	char *path_cpy = strdup(path);
	char *dir = dirname(path_cpy);

	snprintf(nfo, MAXPATHLEN, "%s/movie.nfo", dir);
	if (access(nfo, R_OK) != 0) snprintf(nfo, MAXPATHLEN, "%s/%s.nfo", dir, name);
	if (access(nfo, R_OK) == 0) parse_nfo(nfo, m);

	free(path_cpy);
	free(nfo);
}

void
check_for_folder_nfo_name(const char *path, const char *name, metadata_t *m)
{
	char *nfo = malloc(MAXPATHLEN);

	snprintf(nfo, MAXPATHLEN, "%s/tvshow.nfo", path);
	if (access(nfo, R_OK) == 0) parse_nfo(nfo, m);

	free(nfo);
}

void
add_nfo_from_parent(const char *parentID, metadata_t *m)
{
	char *my_parentID = strdup(parentID);
	char buf[256];
	char * p;
	while ((p = strrchr(my_parentID, '$')) != NULL)
	{
		int nrows;
		char **result;
		*p = '\0';
		snprintf(buf, sizeof(buf), "SELECT D.ALBUM, D.ARTIST, D.AUTHOR, D.GENRE, D.RATING FROM DETAILS D, OBJECTS O "
		                           "WHERE O.OBJECT_ID = '%s%s' AND O.DETAIL_ID = D.ID AND D.ALBUM NOT NULL LIMIT 1",
		                           "64", my_parentID);

		if (sql_get_table(db, buf, &result, &nrows, NULL) == SQLITE_OK)
		{
			if (nrows == 1)
			{
				assign_value_if_exists(&m->album, result[5]);
				assign_value_if_exists(&m->artist, result[6]);
				assign_value_if_exists(&m->author, result[7]);
				assign_value_if_exists(&m->genre, result[8]);
				assign_value_if_exists(&m->rating, result[9]);
			}
			sqlite3_free_table(result);
			if (nrows == 1) break;
		}
	}
	free(my_parentID);
}

static int
add_entry_to_details(const char *path, off_t entry_size, time_t entry_timestamp, metadata_t *m, int64_t album_art_id)
{
	int ret = sql_exec(db, "INSERT into DETAILS"
	                       " (PATH, SIZE, TIMESTAMP, DURATION, DATE, CHANNELS, BITRATE, SAMPLERATE, RESOLUTION,"
	                       "  TITLE, CREATOR, PUBLISHER, AUTHOR, ARTIST, GENRE, COMMENT, DESCRIPTION, RATING,"
	                       "  ALBUM, TRACK, DISC, DLNA_PN, MIME, ALBUM_ART, VIDEO_TYPE) "
	                       "VALUES"
	                       " (%Q, %lld, %lld, %Q, %Q, %u, %u, %u, %Q, %Q, %Q, %Q, %Q, %Q, %Q, %Q, %Q, %Q, %Q, %u, %u, %Q, %Q, %lld, %d);",
	                       path, (long long)entry_size, (long long)entry_timestamp, m->duration, m->date, m->channels, m->bitrate, m->frequency, m->resolution,
	                       m->title, m->creator, m->publisher, m->author, m->artist, m->genre, m->comment, m->description, m->rating,
	                       m->album, m->track, m->disc, m->dlna_pn, m->mime, (long long)album_art_id, (int)m->videotype);

	if (ret != SQLITE_OK)
	{
		DPRINTF(E_ERROR, L_METADATA, "Error inserting details for '%s'!\n", path);
		ret = 0;
	}
	else
	{
		ret = sqlite3_last_insert_rowid(db);
	}
	return ret;
}

int
update_entry_in_details(const char *path, metadata_t *m, int64_t detailID)
{
	int ret = sql_exec(db, "UPDATE DETAILS set"
		" DATE=%Q, CHANNELS=%u, BITRATE=%u, SAMPLERATE=%u, RESOLUTION=%Q,"
		" TITLE=%Q, CREATOR=%Q, PUBLISHER=%Q, AUTHOR=%Q, ARTIST=%Q, GENRE=%Q, COMMENT=%Q, DESCRIPTION=%Q, RATING=%Q,"
		" ALBUM=%Q, TRACK=%u, DISC=%u, DLNA_PN=%Q, MIME=%Q, VIDEO_TYPE=%u where ID=%lld",
		m->date, m->channels, m->bitrate, m->frequency, m->resolution,
		m->title, m->creator, m->publisher, m->author, m->artist, m->genre, m->comment, m->description, m->rating,
		m->album, m->track, m->disc, m->dlna_pn, m->mime, m->videotype, detailID);

	if (ret != SQLITE_OK)
	{
		DPRINTF(E_ERROR, L_METADATA, "Error updating details for '%s'!\n", path);
	}

	char* ref_id = sql_get_text_field(db, "SELECT OBJECT_ID from OBJECTS where DETAIL_ID = %lld and OBJECT_ID like '"BROWSEDIR_ID"$%%'", detailID);
	if (ref_id != NULL)
	{
		insert_containers_for_video(m->title, ref_id, "item.videoItem", detailID);
		sqlite3_free(ref_id);
	}
	return detailID;
}

void
free_metadata(metadata_t *m, uint32_t flags)
{
	if( flags & FLAG_TITLE )
		free(m->title);
	if( flags & FLAG_ARTIST )
		free(m->artist);
	if( flags & FLAG_ALBUM )
		free(m->album);
	if( flags & FLAG_GENRE )
		free(m->genre);
	if( flags & FLAG_CREATOR )
		free(m->creator);
	if( flags & FLAG_PUBLISHER )
		free(m->publisher);
	if (flags & FLAG_AUTHOR)
		free(m->author);
	if( flags & FLAG_DATE )
		free(m->date);
	if( flags & FLAG_COMMENT )
		free(m->comment);
	if( flags & FLAG_DESCRIPTION )
		free(m->description);
	if( flags & FLAG_RATING )
		free(m->rating);
	if( flags & FLAG_DLNA_PN )
		free(m->dlna_pn);
	if( flags & FLAG_MIME )
		free(m->mime);
	if( flags & FLAG_DURATION )
		free(m->duration);
	if( flags & FLAG_RESOLUTION )
		free(m->resolution);
}

int64_t
GetNfoMetadata(const char *path, int64_t detailID)
{
	metadata_t m;
	memset(&m, 0, sizeof(m));
	int nrows;
	char **result;

	char * sql = sqlite3_mprintf("SELECT d.TITLE, d.ARTIST, d.CREATOR, d.PUBLISHER, d.AUTHOR, d.ALBUM, d.GENRE, d.COMMENT, "
		"d.DESCRIPTION, d.RATING, d.DISC, d.TRACK, d.CHANNELS, d.BITRATE, d.SAMPLERATE, d.ROTATION, d.RESOLUTION, "
		"d.DURATION, d.DATE, d.MIME, d.DLNA_PN from DETAILS d WHERE d.ID=%lld", detailID);

	if (sql_get_table(db, sql, &result, &nrows, NULL) == SQLITE_OK)
	{
		if (nrows == 1)
		{
			assign_value_if_exists(&m.title, result[21]);
			assign_value_if_exists(&m.artist, result[22]);
			assign_value_if_exists(&m.creator, result[23]);
			assign_value_if_exists(&m.publisher, result[24]);
			assign_value_if_exists(&m.author, result[25]);
			assign_value_if_exists(&m.album, result[26]);
			assign_value_if_exists(&m.genre, result[27]);
			assign_value_if_exists(&m.comment, result[28]);
			assign_value_if_exists(&m.description, result[29]);
			assign_value_if_exists(&m.rating, result[30]);
			assign_integer_if_exists(&m.disc, result[31]);
			assign_integer_if_exists(&m.track, result[32]);
			assign_integer_if_exists(&m.channels, result[33]);
			assign_integer_if_exists(&m.bitrate, result[34]);
			assign_integer_if_exists(&m.frequency, result[35]);
			assign_integer_if_exists(&m.rotation, result[36]);
			assign_value_if_exists(&m.resolution, result[37]);
			assign_value_if_exists(&m.duration, result[38]);
			assign_value_if_exists(&m.date, result[39]);
			assign_value_if_exists(&m.mime, result[40]);
			assign_value_if_exists(&m.dlna_pn, result[41]);
		}
		sqlite3_free_table(result);
	} else
	{
		return 0;
	}
	sqlite3_free(sql);

	parse_nfo(path, &m);

	update_entry_in_details(path, &m, detailID);
	free_metadata(&m, ALL_FLAGS);
	return detailID;
}

int64_t
GetFolderMetadata(const char *name, const char *path, const char *artist, const char *genre, int64_t album_art_id)
{
	metadata_t m;
	memset(&m, 0, sizeof(m));
	assign_value_if_exists(&m.title, name);
	assign_value_if_exists(&m.creator, artist);
	assign_value_if_exists(&m.artist, artist);
	assign_value_if_exists(&m.genre, genre);

	if (path != NULL) check_for_folder_nfo_name(path, name, &m);

	int ret = add_entry_to_details(path, 0, 0, &m, album_art_id);
	free_metadata(&m, ALL_FLAGS);
	return ret;
}

int64_t
GetAudioMetadata(const char *path, char *name)
{
	char type[4];
	static char lang[6] = { '\0' };
	struct stat file;
	int64_t ret;
	char *esc_tag;
	int i;
	int64_t album_art = 0;
	struct song_metadata song;
	metadata_t m;
	uint32_t free_flags = FLAG_MIME|FLAG_DURATION|FLAG_DLNA_PN|FLAG_DATE;
	memset(&m, 0, sizeof(m));

	if ( stat(path, &file) != 0 )
		return 0;
	strip_ext(name);

	if( ends_with(path, ".mp3") )
	{
		strcpy(type, "mp3");
		m.mime = strdup("audio/mpeg");
	}
	else if( ends_with(path, ".m4a") || ends_with(path, ".mp4") ||
	         ends_with(path, ".aac") || ends_with(path, ".m4p") )
	{
		strcpy(type, "aac");
		m.mime = strdup("audio/mp4");
	}
	else if( ends_with(path, ".3gp") )
	{
		strcpy(type, "aac");
		m.mime = strdup("audio/3gpp");
	}
	else if( ends_with(path, ".wma") || ends_with(path, ".asf") )
	{
		strcpy(type, "asf");
		m.mime = strdup("audio/x-ms-wma");
	}
	else if( ends_with(path, ".flac") || ends_with(path, ".fla") || ends_with(path, ".flc") )
	{
		strcpy(type, "flc");
		m.mime = strdup("audio/x-flac");
	}
	else if( ends_with(path, ".wav") )
	{
		strcpy(type, "wav");
		m.mime = strdup("audio/x-wav");
	}
	else if( ends_with(path, ".ogg") || ends_with(path, ".oga") )
	{
		strcpy(type, "ogg");
		m.mime = strdup("audio/ogg");
	}
	else if( ends_with(path, ".pcm") )
	{
		strcpy(type, "pcm");
		m.mime = strdup("audio/L16");
	}
#ifdef HAVE_WAVPACK
	else if ( ends_with(path, ".wv") )
	{
		strcpy(type, "wv");
		m.mime = strdup("audio/x-wavpack");
	}
#endif
	else
	{
		DPRINTF(E_WARN, L_METADATA, "Unhandled file extension on %s\n", path);
		return 0;
	}

	if( !(*lang) )
	{
		if( !getenv("LANG") )
			strcpy(lang, "en_US");
		else
			strncpyt(lang, getenv("LANG"), sizeof(lang));
	}

	if( readtags((char *)path, &song, &file, lang, type) != 0 )
	{
		DPRINTF(E_WARN, L_METADATA, "Cannot extract tags from %s!\n", path);
        	freetags(&song);
		free_metadata(&m, free_flags);
		return 0;
	}

	if( song.dlna_pn )
		m.dlna_pn = strdup(song.dlna_pn);
	if( song.year )
		xasprintf(&m.date, "%04d-01-01", song.year);
	xasprintf(&m.duration, "%d:%02d:%02d.%03d",
	                      (song.song_length/3600000),
	                      (song.song_length/60000%60),
	                      (song.song_length/1000%60),
	                      (song.song_length%1000));
	if( song.title && *song.title )
	{
		m.title = trim(song.title);
		if( (esc_tag = escape_tag(m.title, 0)) )
		{
			free_flags |= FLAG_TITLE;
			m.title = esc_tag;
		}
	}
	else
	{
		m.title = name;
	}
	for( i = ROLE_START; i < N_ROLE; i++ )
	{
		if( song.contributor[i] && *song.contributor[i] )
		{
			m.creator = trim(song.contributor[i]);
			if( strlen(m.creator) > 48 )
			{
				m.creator = strdup("Various Artists");
				free_flags |= FLAG_CREATOR;
			}
			else if( (esc_tag = escape_tag(m.creator, 0)) )
			{
				m.creator = esc_tag;
				free_flags |= FLAG_CREATOR;
			}
			m.artist = m.creator;
			break;
		}
	}
	/* If there is a album artist or band associated with the album,
	   use it for virtual containers. */
	if( i < ROLE_ALBUMARTIST )
	{
		for( i = ROLE_ALBUMARTIST; i <= ROLE_BAND; i++ )
		{
	        	if( song.contributor[i] && *song.contributor[i] )
				break;
		}
	        if( i <= ROLE_BAND )
		{
			m.artist = trim(song.contributor[i]);
			if( strlen(m.artist) > 48 )
			{
				m.artist = strdup("Various Artists");
				free_flags |= FLAG_ARTIST;
			}
			else if( (esc_tag = escape_tag(m.artist, 0)) )
			{
				m.artist = esc_tag;
				free_flags |= FLAG_ARTIST;
			}
		}
	}
	if( song.album && *song.album )
	{
		m.album = trim(song.album);
		if( (esc_tag = escape_tag(m.album, 0)) )
		{
			free_flags |= FLAG_ALBUM;
			m.album = esc_tag;
		}
	}
	if( song.genre && *song.genre )
	{
		m.genre = trim(song.genre);
		if( (esc_tag = escape_tag(m.genre, 0)) )
		{
			free_flags |= FLAG_GENRE;
			m.genre = esc_tag;
		}
	}
	if( song.comment && *song.comment )
	{
		m.comment = trim(song.comment);
		if( (esc_tag = escape_tag(m.comment, 0)) )
		{
			free_flags |= FLAG_COMMENT;
			m.comment = esc_tag;
		}
	}

	m.channels = song.channels;
	m.bitrate = song.bitrate;
	m.frequency = song.samplerate;
	m.disc = song.disc;
	m.track = song.track;
	if ( song.mime )
	{
		free(m.mime);
		m.mime = strdup(song.mime);
	}

	album_art = find_album_art(path, song.image, song.image_size);
	ret = add_entry_to_details(path, file.st_size, file.st_mtime, &m, album_art);

	freetags(&song);
	free_metadata(&m, free_flags);

	return ret;
}

/* For libjpeg error handling */
jmp_buf setjmp_buffer;
static void
libjpeg_error_handler(j_common_ptr cinfo)
{
	cinfo->err->output_message (cinfo);
	longjmp(setjmp_buffer, 1);
	return;
}

int64_t
GetImageMetadata(const char *path, char *name)
{
	ExifData *ed;
	ExifEntry *e = NULL;
	ExifLoader *l;
	struct jpeg_decompress_struct cinfo;
	struct jpeg_error_mgr jerr;
	FILE *infile;
	int width=0, height=0, thumb=0;
	char make[32], model[64] = {'\0'};
	char b[1024];
	struct stat file;
	int64_t ret;
	image_s *imsrc;
	metadata_t m;
	uint32_t free_flags = ALL_FLAGS;
	memset(&m, '\0', sizeof(metadata_t));

	//DEBUG DPRINTF(E_DEBUG, L_METADATA, "Parsing %s...\n", path);
	if ( stat(path, &file) != 0 )
		return 0;
	strip_ext(name);
	//DEBUG DPRINTF(E_DEBUG, L_METADATA, " * size: %jd\n", file.st_size);

	/* MIME hard-coded to JPEG for now, until we add PNG support */
	m.mime = strdup("image/jpeg");

	l = exif_loader_new();
	exif_loader_write_file(l, path);
	ed = exif_loader_get_data(l);
	exif_loader_unref(l);
	if( !ed )
		goto no_exifdata;

	e = exif_content_get_entry (ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_DATE_TIME_ORIGINAL);
	if( e || (e = exif_content_get_entry(ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_DATE_TIME_DIGITIZED)) )
	{
		m.date = strdup(exif_entry_get_value(e, b, sizeof(b)));
		if( strlen(m.date) > 10 )
		{
			m.date[4] = '-';
			m.date[7] = '-';
			m.date[10] = 'T';
		}
		else {
			free(m.date);
			m.date = NULL;
		}
	}
	else {
		/* One last effort to get the date from XMP */
		image_get_jpeg_date_xmp(path, &m.date);
	}
	//DEBUG DPRINTF(E_DEBUG, L_METADATA, " * date: %s\n", m.date);

	e = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_MAKE);
	if( e )
	{
		strncpyt(make, exif_entry_get_value(e, b, sizeof(b)), sizeof(make));
		e = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_MODEL);
		if( e )
		{
			strncpyt(model, exif_entry_get_value(e, b, sizeof(b)), sizeof(model));
			if( !strcasestr(model, make) )
				snprintf(model, sizeof(model), "%s %s", make, exif_entry_get_value(e, b, sizeof(b)));
			m.creator = escape_tag(trim(model), 1);
		}
	}
	//DEBUG DPRINTF(E_DEBUG, L_METADATA, " * model: %s\n", model);

	e = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_ORIENTATION);
	if( e )
	{
		switch( exif_get_short(e->data, exif_data_get_byte_order(ed)) )
		{
		case 3:
			m.rotation = 180;
			break;
		case 6:
			m.rotation = 90;
			break;
		case 8:
			m.rotation = 270;
			break;
		default:
			m.rotation = 0;
			break;
		}
	}

	if( ed->size )
	{
		/* We might need to verify that the thumbnail is 160x160 or smaller */
		if( ed->size > 12000 )
		{
			imsrc = image_new_from_jpeg(NULL, 0, ed->data, ed->size, 1, ROTATE_NONE);
			if( imsrc )
			{
 				if( (imsrc->width <= 160) && (imsrc->height <= 160) )
					thumb = 1;
				image_free(imsrc);
			}
		}
		else
			thumb = 1;
	}
	//DEBUG DPRINTF(E_DEBUG, L_METADATA, " * thumbnail: %d\n", thumb);

	exif_data_unref(ed);

no_exifdata:
	/* If SOF parsing fails, then fall through to reading the JPEG data with libjpeg to get the resolution */
	if( image_get_jpeg_resolution(path, &width, &height) != 0 || !width || !height )
	{
		infile = fopen(path, "r");
		if( infile )
		{
			cinfo.err = jpeg_std_error(&jerr);
			jerr.error_exit = libjpeg_error_handler;
			jpeg_create_decompress(&cinfo);
			if( setjmp(setjmp_buffer) )
				goto error;
			jpeg_stdio_src(&cinfo, infile);
			jpeg_read_header(&cinfo, TRUE);
			jpeg_start_decompress(&cinfo);
			width = cinfo.output_width;
			height = cinfo.output_height;
			error:
			jpeg_destroy_decompress(&cinfo);
			fclose(infile);
		}
	}
	//DEBUG DPRINTF(E_DEBUG, L_METADATA, " * resolution: %dx%d\n", width, height);

	if( !width || !height )
	{
		free_metadata(&m, free_flags);
		return 0;
	}
	if( width <= 640 && height <= 480 )
		m.dlna_pn = strdup("JPEG_SM");
	else if( width <= 1024 && height <= 768 )
		m.dlna_pn = strdup("JPEG_MED");
	else if( (width <= 4096 && height <= 4096) || !GETFLAG(DLNA_STRICT_MASK) )
		m.dlna_pn = strdup("JPEG_LRG");
	xasprintf(&m.resolution, "%dx%d", width, height);

	ret = sql_exec(db, "INSERT into DETAILS"
	                   " (PATH, TITLE, SIZE, TIMESTAMP, DATE, RESOLUTION,"
	                    " ROTATION, THUMBNAIL, CREATOR, DLNA_PN, MIME) "
	                   "VALUES"
	                   " (%Q, '%q', %lld, %lld, %Q, %Q, %u, %d, %Q, %Q, %Q);",
	                   path, name, (long long)file.st_size, (long long)file.st_mtime, m.date,
	                   m.resolution, m.rotation, thumb, m.creator, m.dlna_pn, m.mime);
	if( ret != SQLITE_OK )
	{
		DPRINTF(E_ERROR, L_METADATA, "Error inserting details for '%s'!\n", path);
		ret = 0;
	}
	else
	{
		ret = sqlite3_last_insert_rowid(db);
	}
	free_metadata(&m, free_flags);

	return ret;
}

int64_t
GetVideoMetadata(const char *path, char *name, const char *parentID)
{
	struct stat file;
	int ret, i;
	struct tm *modtime;
	AVFormatContext *ctx = NULL;
	AVStream *astream = NULL, *vstream = NULL;
	int audio_stream = -1, video_stream = -1;
	enum audio_profiles audio_profile = PROFILE_AUDIO_UNKNOWN;
	char fourcc[4];
	int64_t album_art = 0;
	struct song_metadata video;
	metadata_t m;
	uint32_t free_flags = ALL_FLAGS;
	char *path_cpy, *basepath;

	memset(&m, '\0', sizeof(m));
	memset(&video, '\0', sizeof(video));

	//DEBUG DPRINTF(E_DEBUG, L_METADATA, "Parsing video %s...\n", name);
	if ( stat(path, &file) != 0 )
		return 0;
	strip_ext(name);
	//DEBUG DPRINTF(E_DEBUG, L_METADATA, " * size: %jd\n", file.st_size);

	ret = lav_open(&ctx, path);
	if( ret != 0 )
	{
		char err[128];
		av_strerror(ret, err, sizeof(err));
		DPRINTF(E_WARN, L_METADATA, "Opening %s failed! [%s]\n", path, err);
		return 0;
	}
	//dump_format(ctx, 0, NULL, 0);
	for( i=0; i<ctx->nb_streams; i++)
	{
		if( lav_codec_type(ctx->streams[i]) == AVMEDIA_TYPE_AUDIO && audio_stream == -1)
		{
			audio_stream = i;
			astream = ctx->streams[audio_stream];
			continue;
		}
		else if( lav_codec_type(ctx->streams[i]) == AVMEDIA_TYPE_VIDEO &&
		         !lav_is_thumbnail_stream(ctx->streams[i], &m.thumb_data, &m.thumb_size) &&
				 video_stream == -1)
		{
			video_stream = i;
			vstream = ctx->streams[video_stream];
			continue;
		}
	}
	path_cpy = strdup(path);
	basepath = basename(path_cpy);
	if( !vstream )
	{
		/* This must not be a video file. */
		lav_close(ctx);
		if( !is_audio(path) )
			DPRINTF(E_DEBUG, L_METADATA, "File %s does not contain a video stream.\n", basepath);
		free(path_cpy);
		return 0;
	}

	if( astream )
	{
		aac_object_type_t aac_type = AAC_INVALID;
		switch( lav_codec_id(astream) )
		{
			case AV_CODEC_ID_MP3:
				audio_profile = PROFILE_AUDIO_MP3;
				break;
			case AV_CODEC_ID_AAC:
				if( !lav_codec_extradata(astream) )
				{
					DPRINTF(E_DEBUG, L_METADATA, "No AAC type\n");
				}
				else
				{
					uint8_t data;
					memcpy(&data, lav_codec_extradata(astream), 1);
					aac_type = data >> 3;
				}
				switch( aac_type )
				{
					/* AAC Low Complexity variants */
					case AAC_LC:
					case AAC_LC_ER:
						if( lav_sample_rate(astream) < 8000 ||
						    lav_sample_rate(astream) > 48000 )
						{
							DPRINTF(E_DEBUG, L_METADATA, "Unsupported AAC: sample rate is not 8000 < %d < 48000\n",
								lav_sample_rate(astream));
							break;
						}
						/* AAC @ Level 1/2 */
						if( lav_channels(astream) <= 2 &&
						    lav_bit_rate(astream) <= 576000 )
							audio_profile = PROFILE_AUDIO_AAC;
						else if( lav_channels(astream) <= 6 &&
							 lav_bit_rate(astream) <= 1440000 )
							audio_profile = PROFILE_AUDIO_AAC_MULT5;
						else
							DPRINTF(E_DEBUG, L_METADATA, "Unhandled AAC: %lld channels, %lld bitrate\n",
								(long long)lav_channels(astream),
								(long long)lav_bit_rate(astream));
						break;
					default:
						DPRINTF(E_DEBUG, L_METADATA, "Unhandled AAC type [%d]\n", aac_type);
						break;
				}
				break;
			case AV_CODEC_ID_AC3:
			case AV_CODEC_ID_DTS:
				audio_profile = PROFILE_AUDIO_AC3;
				break;
			case AV_CODEC_ID_WMAV1:
			case AV_CODEC_ID_WMAV2:
				/* WMA Baseline: stereo, up to 48 KHz, up to 192,999 bps */
				if ( lav_bit_rate(astream) <= 193000 )
					audio_profile = PROFILE_AUDIO_WMA_BASE;
				/* WMA Full: stereo, up to 48 KHz, up to 385 Kbps */
				else if ( lav_bit_rate(astream) <= 385000 )
					audio_profile = PROFILE_AUDIO_WMA_FULL;
				break;
			case AV_CODEC_ID_WMAPRO:
				audio_profile = PROFILE_AUDIO_WMA_PRO;
				break;
			case AV_CODEC_ID_MP2:
				audio_profile = PROFILE_AUDIO_MP2;
				break;
			case AV_CODEC_ID_AMR_NB:
				audio_profile = PROFILE_AUDIO_AMR;
				break;
			default:
				if( (lav_codec_id(astream) >= AV_CODEC_ID_PCM_S16LE) &&
				    (lav_codec_id(astream) < AV_CODEC_ID_ADPCM_IMA_QT) )
					audio_profile = PROFILE_AUDIO_PCM;
				else
					DPRINTF(E_DEBUG, L_METADATA, "Unhandled audio codec [0x%X]\n", lav_codec_id(astream));
				break;
		}
		m.frequency = lav_sample_rate(astream);
		m.channels = lav_channels(astream);
	}
	if( vstream )
	{
		int off;
		int duration, hours, min, sec, ms;
		ts_timestamp_t ts_timestamp = NONE;
		DPRINTF(E_DEBUG, L_METADATA, "Container: '%s' [%s]\n", ctx->iformat->name, basepath);
		xasprintf(&m.resolution, "%dx%d", lav_width(vstream), lav_height(vstream));
		if( ctx->bit_rate > 8 )
			m.bitrate = ctx->bit_rate / 8;
		if( ctx->duration > 0 ) {
			duration = (int)(ctx->duration / AV_TIME_BASE);
			hours = (int)(duration / 3600);
			min = (int)(duration / 60 % 60);
			sec = (int)(duration % 60);
			ms = (int)(ctx->duration / (AV_TIME_BASE/1000) % 1000);
			xasprintf(&m.duration, "%d:%02d:%02d.%03d", hours, min, sec, ms);
		}

		/* NOTE: The DLNA spec only provides for ASF (WMV), TS, PS, and MP4 containers.
		 * Skip DLNA parsing for everything else. */
		if( strcmp(ctx->iformat->name, "avi") == 0 )
		{
			xasprintf(&m.mime, "video/x-msvideo");
			if( lav_codec_id(vstream) == AV_CODEC_ID_MPEG4 )
			{
				fourcc[0] = lav_codec_tag(vstream)     & 0xff;
				fourcc[1] = lav_codec_tag(vstream)>>8  & 0xff;
				fourcc[2] = lav_codec_tag(vstream)>>16 & 0xff;
				fourcc[3] = lav_codec_tag(vstream)>>24 & 0xff;
				if( memcmp(fourcc, "XVID", 4) == 0 ||
				    memcmp(fourcc, "DX50", 4) == 0 ||
				    memcmp(fourcc, "DIVX", 4) == 0 )
					xasprintf(&m.creator, "DiVX");
			}
		}
		else if( strcmp(ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == 0 &&
		         ends_with(path, ".mov") )
			xasprintf(&m.mime, "video/quicktime");
		else if( strncmp(ctx->iformat->name, "matroska", 8) == 0 )
			xasprintf(&m.mime, "video/x-matroska");
		else if( strcmp(ctx->iformat->name, "flv") == 0 )
			xasprintf(&m.mime, "video/x-flv");
		if( m.mime )
			goto video_no_dlna;

		switch( lav_codec_id(vstream) )
		{
			case AV_CODEC_ID_MPEG1VIDEO:
				if( strcmp(ctx->iformat->name, "mpeg") == 0 )
				{
					if( (lav_width(vstream)  == 352) &&
					    (lav_height(vstream) <= 288) )
					{
						m.dlna_pn = strdup("MPEG1");
					}
					xasprintf(&m.mime, "video/mpeg");
				}
				break;
			case AV_CODEC_ID_MPEG2VIDEO:
				m.dlna_pn = malloc(64);
				off = sprintf(m.dlna_pn, "MPEG_");
				if( strcmp(ctx->iformat->name, "mpegts") == 0 )
				{
					int raw_packet_size;
					int dlna_ts_present = dlna_timestamp_is_present(path, &raw_packet_size);
					DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is %s MPEG2 TS packet size %d\n",
						video_stream, basepath, m.resolution, raw_packet_size);
					off += sprintf(m.dlna_pn+off, "TS_");
					if( (lav_width(vstream)  >= 1280) &&
					    (lav_height(vstream) >= 720) )
					{
						off += sprintf(m.dlna_pn+off, "HD_NA");
					}
					else
					{
						off += sprintf(m.dlna_pn+off, "SD_");
						if( (lav_height(vstream) == 576) ||
						    (lav_height(vstream) == 288) )
							off += sprintf(m.dlna_pn+off, "EU");
						else
							off += sprintf(m.dlna_pn+off, "NA");
					}
					if( raw_packet_size == MPEG_TS_PACKET_LENGTH_DLNA )
					{
						if (dlna_ts_present)
							ts_timestamp = VALID;
						else
							ts_timestamp = EMPTY;
					}
					else if( raw_packet_size != MPEG_TS_PACKET_LENGTH )
					{
						DPRINTF(E_DEBUG, L_METADATA, "Unsupported DLNA TS packet size [%d] (%s)\n",
							raw_packet_size, basepath);
						free(m.dlna_pn);
						m.dlna_pn = NULL;
					}
					switch( ts_timestamp )
					{
						case NONE:
							xasprintf(&m.mime, "video/mpeg");
							if( m.dlna_pn )
								off += sprintf(m.dlna_pn+off, "_ISO");
							break;
						case VALID:
							off += sprintf(m.dlna_pn+off, "_T");
						case EMPTY:
							xasprintf(&m.mime, "video/vnd.dlna.mpeg-tts");
						default:
							break;
					}
				}
				else if( strcmp(ctx->iformat->name, "mpeg") == 0 )
				{
					DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is %s MPEG2 PS\n",
						video_stream, basepath, m.resolution);
					off += sprintf(m.dlna_pn+off, "PS_");
					if( (lav_height(vstream) == 576) ||
					    (lav_height(vstream) == 288) )
						off += sprintf(m.dlna_pn+off, "PAL");
					else
						off += sprintf(m.dlna_pn+off, "NTSC");
					xasprintf(&m.mime, "video/mpeg");
				}
				else
				{
					DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s [%s] is %s non-DLNA MPEG2\n",
						video_stream, basepath, ctx->iformat->name, m.resolution);
					free(m.dlna_pn);
					m.dlna_pn = NULL;
				}
				break;
			case AV_CODEC_ID_H264:
				m.dlna_pn = malloc(128);
				off = sprintf(m.dlna_pn, "AVC_");

				if( strcmp(ctx->iformat->name, "mpegts") == 0 )
				{
					AVRational display_aspect_ratio;
					int fps, interlaced;
					int raw_packet_size;
					int dlna_ts_present = dlna_timestamp_is_present(path, &raw_packet_size);

					off += sprintf(m.dlna_pn+off, "TS_");
					if (lav_sample_aspect_ratio(vstream).num) {
						av_reduce(&display_aspect_ratio.num, &display_aspect_ratio.den,
						          lav_width(vstream) * lav_sample_aspect_ratio(vstream).num,
						          lav_height(vstream) * lav_sample_aspect_ratio(vstream).den,
						          1024*1024);
					}
					fps = lav_get_fps(vstream);
					interlaced = lav_get_interlaced(vstream);
					if( ((((lav_width(vstream) == 1920 || lav_width(vstream) == 1440) && lav_height(vstream) == 1080) ||
					      (lav_width(vstream) == 720 && lav_height(vstream) == 480)) && fps == 59 && interlaced) ||
					    ((lav_width(vstream) == 1280 && lav_height(vstream) == 720) && fps == 59 && !interlaced) )
					{
						if( (lav_profile(vstream) == FF_PROFILE_H264_MAIN || lav_profile(vstream) == FF_PROFILE_H264_HIGH) &&
						    audio_profile == PROFILE_AUDIO_AC3 )
						{
							off += sprintf(m.dlna_pn+off, "HD_60_");
							lav_profile(vstream) = FF_PROFILE_SKIP;
						}
					}
					else if( ((lav_width(vstream) == 1920 && lav_height(vstream) == 1080) ||
					          (lav_width(vstream) == 1440 && lav_height(vstream) == 1080) ||
					          (lav_width(vstream) == 1280 && lav_height(vstream) ==  720) ||
					          (lav_width(vstream) ==  720 && lav_height(vstream) ==  576)) &&
					          interlaced && fps == 50 )
					{
						if( (lav_profile(vstream) == FF_PROFILE_H264_MAIN || lav_profile(vstream) == FF_PROFILE_H264_HIGH) &&
						    audio_profile == PROFILE_AUDIO_AC3 )
						{
							off += sprintf(m.dlna_pn+off, "HD_50_");
							lav_profile(vstream) = FF_PROFILE_SKIP;
						}
					}
					switch( lav_profile(vstream) )
					{
						case FF_PROFILE_H264_BASELINE:
						case FF_PROFILE_H264_CONSTRAINED_BASELINE:
							off += sprintf(m.dlna_pn+off, "BL_");
							if( lav_width(vstream)  <= 352 &&
							    lav_height(vstream) <= 288 &&
							    lav_bit_rate(vstream) <= 384000 )
							{
								off += sprintf(m.dlna_pn+off, "CIF15_");
								break;
							}
							else if( lav_width(vstream)  <= 352 &&
							         lav_height(vstream) <= 288 &&
							         lav_bit_rate(vstream) <= 3000000 )
							{
								off += sprintf(m.dlna_pn+off, "CIF30_");
								break;
							}
							/* Fall back to Main Profile if it doesn't match a Baseline DLNA profile. */
							else
								off -= 3;
						default:
						case FF_PROFILE_H264_MAIN:
							off += sprintf(m.dlna_pn+off, "MP_");
							if( lav_profile(vstream) != FF_PROFILE_H264_BASELINE &&
							    lav_profile(vstream) != FF_PROFILE_H264_CONSTRAINED_BASELINE &&
							    lav_profile(vstream) != FF_PROFILE_H264_MAIN )
							{
								DPRINTF(E_DEBUG, L_METADATA, "Unknown AVC profile %d; assuming MP. [%s]\n",
									lav_profile(vstream), basepath);
							}
							if( lav_width(vstream)  <= 720 &&
							    lav_height(vstream) <= 576 &&
							    lav_bit_rate(vstream) <= 10000000 )
							{
								off += sprintf(m.dlna_pn+off, "SD_");
							}
							else if( lav_width(vstream)  <= 1920 &&
							         lav_height(vstream) <= 1152 &&
							         lav_bit_rate(vstream) <= 20000000 )
							{
								off += sprintf(m.dlna_pn+off, "HD_");
							}
							else
							{
								DPRINTF(E_DEBUG, L_METADATA, "Unsupported h.264 video profile! [%s, %dx%d, %lldbps : %s]\n",
									m.dlna_pn, lav_width(vstream), lav_height(vstream),
									(long long)lav_bit_rate(vstream), basepath);
								free(m.dlna_pn);
								m.dlna_pn = NULL;
							}
							break;
						case FF_PROFILE_H264_HIGH:
							off += sprintf(m.dlna_pn+off, "HP_");
							if( lav_width(vstream)  <= 1920 &&
							    lav_height(vstream) <= 1152 &&
							    lav_bit_rate(vstream) <= 30000000 &&
							    audio_profile == PROFILE_AUDIO_AC3 )
							{
								off += sprintf(m.dlna_pn+off, "HD_");
							}
							else
							{
								DPRINTF(E_DEBUG, L_METADATA, "Unsupported h.264 HP video profile! [%lldbps, %d audio : %s]\n",
									(long long)lav_bit_rate(vstream), audio_profile, basepath);
								free(m.dlna_pn);
								m.dlna_pn = NULL;
							}
							break;
						case FF_PROFILE_SKIP:
							break;
					}
					if( !m.dlna_pn )
						break;
					switch( audio_profile )
					{
						case PROFILE_AUDIO_MP3:
							off += sprintf(m.dlna_pn+off, "MPEG1_L3");
							break;
						case PROFILE_AUDIO_AC3:
							off += sprintf(m.dlna_pn+off, "AC3");
							break;
						case PROFILE_AUDIO_AAC:
						case PROFILE_AUDIO_AAC_MULT5:
							off += sprintf(m.dlna_pn+off, "AAC_MULT5");
							break;
						default:
							DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for %s file [%s]\n",
								m.dlna_pn, basepath);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
							break;
					}
					if( !m.dlna_pn )
						break;
					if( raw_packet_size == MPEG_TS_PACKET_LENGTH_DLNA )
					{
						if( lav_profile(vstream) == FF_PROFILE_H264_HIGH ||
						    dlna_ts_present )
							ts_timestamp = VALID;
						else
							ts_timestamp = EMPTY;
					}
					else if( raw_packet_size != MPEG_TS_PACKET_LENGTH )
					{
						DPRINTF(E_DEBUG, L_METADATA, "Unsupported DLNA TS packet size [%d] (%s)\n",
							raw_packet_size, basepath);
						free(m.dlna_pn);
						m.dlna_pn = NULL;
					}
					switch( ts_timestamp )
					{
						case NONE:
							if( m.dlna_pn )
								off += sprintf(m.dlna_pn+off, "_ISO");
							break;
						case VALID:
							off += sprintf(m.dlna_pn+off, "_T");
						case EMPTY:
							xasprintf(&m.mime, "video/vnd.dlna.mpeg-tts");
						default:
							break;
					}
				}
				else if( strcmp(ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == 0 )
				{
					off += sprintf(m.dlna_pn+off, "MP4_");

					switch( lav_profile(vstream) ) {
					case FF_PROFILE_H264_BASELINE:
					case FF_PROFILE_H264_CONSTRAINED_BASELINE:
						if( lav_width(vstream)  <= 352 &&
						    lav_height(vstream) <= 288 )
						{
							if( ctx->bit_rate < 600000 )
								off += sprintf(m.dlna_pn+off, "BL_CIF15_");
							else if( ctx->bit_rate < 5000000 )
								off += sprintf(m.dlna_pn+off, "BL_CIF30_");
							else
								goto mp4_mp_fallback;

							if( audio_profile == PROFILE_AUDIO_AMR )
							{
								off += sprintf(m.dlna_pn+off, "AMR");
							}
							else if( audio_profile == PROFILE_AUDIO_AAC )
							{
								off += sprintf(m.dlna_pn+off, "AAC_");
								if( ctx->bit_rate < 520000 )
								{
									off += sprintf(m.dlna_pn+off, "520");
								}
								else if( ctx->bit_rate < 940000 )
								{
									off += sprintf(m.dlna_pn+off, "940");
								}
								else
								{
									off -= 13;
									goto mp4_mp_fallback;
								}
							}
							else
							{
								off -= 9;
								goto mp4_mp_fallback;
							}
						}
						else if( lav_width(vstream)  <= 720 &&
						         lav_height(vstream) <= 576 )
						{
							if( lav_level(vstream) == 30 &&
							    audio_profile == PROFILE_AUDIO_AAC &&
							    ctx->bit_rate <= 5000000 )
								off += sprintf(m.dlna_pn+off, "BL_L3L_SD_AAC");
							else if( lav_level(vstream) <= 31 &&
							         audio_profile == PROFILE_AUDIO_AAC &&
							         ctx->bit_rate <= 15000000 )
								off += sprintf(m.dlna_pn+off, "BL_L31_HD_AAC");
							else
								goto mp4_mp_fallback;
						}
						else if( lav_width(vstream)  <= 1280 &&
						         lav_height(vstream) <= 720 )
						{
							if( lav_level(vstream) <= 31 &&
							    audio_profile == PROFILE_AUDIO_AAC &&
							    ctx->bit_rate <= 15000000 )
								off += sprintf(m.dlna_pn+off, "BL_L31_HD_AAC");
							else if( lav_level(vstream) <= 32 &&
							         audio_profile == PROFILE_AUDIO_AAC &&
							         ctx->bit_rate <= 21000000 )
								off += sprintf(m.dlna_pn+off, "BL_L32_HD_AAC");
							else
								goto mp4_mp_fallback;
						}
						else
							goto mp4_mp_fallback;
						break;
					case FF_PROFILE_H264_MAIN:
					mp4_mp_fallback:
						off += sprintf(m.dlna_pn+off, "MP_");
						/* AVC MP4 SD profiles - 10 Mbps max */
						if( lav_width(vstream)  <= 720 &&
						    lav_height(vstream) <= 576 &&
						    lav_bit_rate(vstream) <= 10000000 )
						{
							sprintf(m.dlna_pn+off, "SD_");
							if( audio_profile == PROFILE_AUDIO_AC3 )
								off += sprintf(m.dlna_pn+off, "AC3");
							else if( audio_profile == PROFILE_AUDIO_AAC ||
							         audio_profile == PROFILE_AUDIO_AAC_MULT5 )
								off += sprintf(m.dlna_pn+off, "AAC_MULT5");
							else if( audio_profile == PROFILE_AUDIO_MP3 )
								off += sprintf(m.dlna_pn+off, "MPEG1_L3");
							else
								m.dlna_pn[10] = '\0';
						}
						else if( lav_width(vstream)  <= 1280 &&
						         lav_height(vstream) <= 720 &&
						         lav_bit_rate(vstream) <= 15000000 &&
						         audio_profile == PROFILE_AUDIO_AAC )
						{
							off += sprintf(m.dlna_pn+off, "HD_720p_AAC");
						}
						else if( lav_width(vstream)  <= 1920 &&
						         lav_height(vstream) <= 1080 &&
						         lav_bit_rate(vstream) <= 21000000 &&
						         audio_profile == PROFILE_AUDIO_AAC )
						{
							off += sprintf(m.dlna_pn+off, "HD_1080i_AAC");
						}
						if( strlen(m.dlna_pn) <= 11 )
						{
							DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for %s file %s\n",
								m.dlna_pn, basepath);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
						}
						break;
					case FF_PROFILE_H264_HIGH:
						if( lav_width(vstream)  <= 1920 &&
						    lav_height(vstream) <= 1080 &&
						    lav_bit_rate(vstream) <= 25000000 &&
						    audio_profile == PROFILE_AUDIO_AAC )
						{
							off += sprintf(m.dlna_pn+off, "HP_HD_AAC");
						}
						break;
					default:
						DPRINTF(E_DEBUG, L_METADATA, "AVC profile [%d] not recognized for file %s\n",
							lav_profile(vstream), basepath);
						free(m.dlna_pn);
						m.dlna_pn = NULL;
						break;
					}
				}
				else
				{
					free(m.dlna_pn);
					m.dlna_pn = NULL;
				}
				DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is h.264\n", video_stream, basepath);
				break;
			case AV_CODEC_ID_MPEG4:
				fourcc[0] = lav_codec_tag(vstream)     & 0xff;
				fourcc[1] = lav_codec_tag(vstream)>>8  & 0xff;
				fourcc[2] = lav_codec_tag(vstream)>>16 & 0xff;
				fourcc[3] = lav_codec_tag(vstream)>>24 & 0xff;
				DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is MPEG4 [%c%c%c%c/0x%X]\n",
					video_stream, basepath,
					isprint(fourcc[0]) ? fourcc[0] : '_',
					isprint(fourcc[1]) ? fourcc[1] : '_',
					isprint(fourcc[2]) ? fourcc[2] : '_',
					isprint(fourcc[3]) ? fourcc[3] : '_',
					lav_codec_tag(vstream));

				if( strcmp(ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == 0 )
				{
					m.dlna_pn = malloc(128);
					off = sprintf(m.dlna_pn, "MPEG4_P2_");

					if( ends_with(path, ".3gp") )
					{
						xasprintf(&m.mime, "video/3gpp");
						switch( audio_profile )
						{
							case PROFILE_AUDIO_AAC:
								off += sprintf(m.dlna_pn+off, "3GPP_SP_L0B_AAC");
								break;
							case PROFILE_AUDIO_AMR:
								off += sprintf(m.dlna_pn+off, "3GPP_SP_L0B_AMR");
								break;
							default:
								DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for MPEG4-P2 3GP/0x%X file %s\n",
								        lav_codec_id(astream), basepath);
								free(m.dlna_pn);
								m.dlna_pn = NULL;
								break;
						}
					}
					else
					{
						if( ctx->bit_rate <= 1000000 &&
						    audio_profile == PROFILE_AUDIO_AAC )
						{
							off += sprintf(m.dlna_pn+off, "MP4_ASP_AAC");
						}
						else if( ctx->bit_rate <= 4000000 &&
						         lav_width(vstream)  <= 640 &&
						         lav_height(vstream) <= 480 &&
						         audio_profile == PROFILE_AUDIO_AAC )
						{
							off += sprintf(m.dlna_pn+off, "MP4_SP_VGA_AAC");
						}
						else
						{
							DPRINTF(E_DEBUG, L_METADATA, "Unsupported h.264 video profile! [%dx%d, %lldbps]\n",
								lav_width(vstream),
								lav_height(vstream),
								(long long)ctx->bit_rate);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
						}
					}
				}
				break;
			case AV_CODEC_ID_WMV3:
				/* I'm not 100% sure this is correct, but it works on everything I could get my hands on */
				if( lav_codec_extradata(vstream) )
				{
					if( !((lav_codec_extradata(vstream)[0] >> 3) & 1) )
						lav_level(vstream) = 0;
					if( !((lav_codec_extradata(vstream)[0] >> 6) & 1) )
						lav_profile(vstream) = 0;
				}
			case AV_CODEC_ID_VC1:
				if( strcmp(ctx->iformat->name, "asf") != 0 )
				{
					DPRINTF(E_DEBUG, L_METADATA, "Skipping DLNA parsing for non-ASF VC1 file %s\n", path);
					break;
				}
				m.dlna_pn = malloc(64);
				off = sprintf(m.dlna_pn, "WMV");
				DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is VC1\n", video_stream, basepath);
				xasprintf(&m.mime, "video/x-ms-wmv");
				if( (lav_width(vstream)  <= 176) &&
				    (lav_height(vstream) <= 144) &&
				    (lav_level(vstream) == 0) )
				{
					off += sprintf(m.dlna_pn+off, "SPLL_");
					switch( audio_profile )
					{
						case PROFILE_AUDIO_MP3:
							off += sprintf(m.dlna_pn+off, "MP3");
							break;
						case PROFILE_AUDIO_WMA_BASE:
							off += sprintf(m.dlna_pn+off, "BASE");
							break;
						default:
							DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for WMVSPLL/0x%X file %s\n",
								audio_profile, basepath);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
							break;
					}
				}
				else if( (lav_width(vstream)  <= 352) &&
				         (lav_height(vstream) <= 288) &&
				         (lav_profile(vstream) == 0) &&
				         (ctx->bit_rate/8 <= 384000) )
				{
					off += sprintf(m.dlna_pn+off, "SPML_");
					switch( audio_profile )
					{
						case PROFILE_AUDIO_MP3:
							off += sprintf(m.dlna_pn+off, "MP3");
							break;
						case PROFILE_AUDIO_WMA_BASE:
							off += sprintf(m.dlna_pn+off, "BASE");
							break;
						default:
							DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for WMVSPML/0x%X file %s\n",
								audio_profile, basepath);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
							break;
					}
				}
				else if( (lav_width(vstream)  <= 720) &&
				         (lav_height(vstream) <= 576) &&
				         (ctx->bit_rate/8 <= 10000000) )
				{
					off += sprintf(m.dlna_pn+off, "MED_");
					switch( audio_profile )
					{
						case PROFILE_AUDIO_WMA_PRO:
							off += sprintf(m.dlna_pn+off, "PRO");
							break;
						case PROFILE_AUDIO_WMA_FULL:
							off += sprintf(m.dlna_pn+off, "FULL");
							break;
						case PROFILE_AUDIO_WMA_BASE:
							off += sprintf(m.dlna_pn+off, "BASE");
							break;
						default:
							DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for WMVMED/0x%X file %s\n",
								audio_profile, basepath);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
							break;
					}
				}
				else if( (lav_width(vstream)  <= 1920) &&
				         (lav_height(vstream) <= 1080) &&
				         (ctx->bit_rate/8 <= 20000000) )
				{
					off += sprintf(m.dlna_pn+off, "HIGH_");
					switch( audio_profile )
					{
						case PROFILE_AUDIO_WMA_PRO:
							off += sprintf(m.dlna_pn+off, "PRO");
							break;
						case PROFILE_AUDIO_WMA_FULL:
							off += sprintf(m.dlna_pn+off, "FULL");
							break;
						default:
							DPRINTF(E_DEBUG, L_METADATA, "No DLNA profile found for WMVHIGH/0x%X file %s\n",
								audio_profile, basepath);
							free(m.dlna_pn);
							m.dlna_pn = NULL;
							break;
					}
				}
				break;
			case AV_CODEC_ID_MSMPEG4V3:
				xasprintf(&m.mime, "video/x-msvideo");
			default:
				DPRINTF(E_DEBUG, L_METADATA, "Stream %d of %s is %s [type %d]\n",
					video_stream, basepath, m.resolution, lav_codec_id(vstream));
				break;
		}
	}

	if( strcmp(ctx->iformat->name, "asf") == 0 )
	{
		if( readtags((char *)path, &video, &file, "en_US", "asf") == 0 )
		{
			if( video.title && *video.title )
			{
				m.title = escape_tag(trim(video.title), 1);
			}
			if( video.genre && *video.genre )
			{
				m.genre = escape_tag(trim(video.genre), 1);
			}
			if( video.contributor[ROLE_TRACKARTIST] && *video.contributor[ROLE_TRACKARTIST] )
			{
				m.artist = escape_tag(trim(video.contributor[ROLE_TRACKARTIST]), 1);
			}
			if( video.contributor[ROLE_ALBUMARTIST] && *video.contributor[ROLE_ALBUMARTIST] )
			{
				m.creator = escape_tag(trim(video.contributor[ROLE_ALBUMARTIST]), 1);
			}
			else
			{
				m.creator = m.artist;
				free_flags &= ~FLAG_CREATOR;
			}
			if (!m.thumb_data)
			{
				m.thumb_data = video.image;
				m.thumb_size = video.image_size;
			}
		}
	}
	#ifndef NETGEAR
	#if LIBAVFORMAT_VERSION_INT >= ((52<<16)+(31<<8)+0)
	else if( strcmp(ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == 0 )
	{
		if( ctx->metadata )
		{
			AVDictionaryEntry *tag = NULL;

			//DEBUG DPRINTF(E_DEBUG, L_METADATA, "Metadata:\n");
			while( (tag = av_dict_get(ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)) )
			{
				//DEBUG DPRINTF(E_DEBUG, L_METADATA, "  %-16s: %s\n", tag->key, tag->value);
				if( strcmp(tag->key, "title") == 0 )
					m.title = escape_tag(trim(tag->value), 1);
				else if( strcmp(tag->key, "genre") == 0 )
					m.genre = escape_tag(trim(tag->value), 1);
				else if( strcmp(tag->key, "artist") == 0 )
					m.artist = escape_tag(trim(tag->value), 1);
				else if( strcmp(tag->key, "comment") == 0 )
					m.comment = escape_tag(trim(tag->value), 1);
			}
		}
	}
	#endif
	#endif
video_no_dlna:

#ifdef TIVO_SUPPORT
	if( ends_with(path, ".TiVo") && is_tivo_file(path) )
	{
		if( m.dlna_pn )
		{
			free(m.dlna_pn);
			m.dlna_pn = NULL;
		}
		m.mime = realloc(m.mime, 21);
		strcpy(m.mime, "video/x-tivo-mpeg");
	}
#endif

	add_nfo_from_parent(parentID, &m);
	check_for_nfo_name(path, name, &m);

	if( !m.mime )
	{
		if( strcmp(ctx->iformat->name, "avi") == 0 )
			xasprintf(&m.mime, "video/x-msvideo");
		else if( strncmp(ctx->iformat->name, "mpeg", 4) == 0 )
			xasprintf(&m.mime, "video/mpeg");
		else if( strcmp(ctx->iformat->name, "asf") == 0 )
			xasprintf(&m.mime, "video/x-ms-wmv");
		else if( strcmp(ctx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") == 0 )
			if( ends_with(path, ".mov") )
				xasprintf(&m.mime, "video/quicktime");
			else
				xasprintf(&m.mime, "video/mp4");
		else if( strncmp(ctx->iformat->name, "matroska", 8) == 0 )
			xasprintf(&m.mime, "video/x-matroska");
		else if( strcmp(ctx->iformat->name, "flv") == 0 )
			xasprintf(&m.mime, "video/x-flv");
		else
			DPRINTF(E_WARN, L_METADATA, "%s: Unhandled format: %s\n", path, ctx->iformat->name);
	}

	if( !m.date )
	{
		m.date = malloc(20);
		modtime = localtime(&file.st_mtime);
		strftime(m.date, 20, "%FT%T", modtime);
	}

	if( !m.title )
		m.title = strdup(name);

	album_art = find_album_art(path, m.thumb_data, m.thumb_size);
	freetags(&video);
	lav_close(ctx);

	ret = add_entry_to_details(path, file.st_size, file.st_mtime, &m, album_art);
	if (ret != 0) check_for_captions(path, ret);

	free_metadata(&m, free_flags);
	free(path_cpy);

	return ret;
}
