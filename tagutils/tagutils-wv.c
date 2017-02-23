//=========================================================================
// FILENAME	: tagutils-wv.c
// DESCRIPTION	: WavPack metadata reader
//=========================================================================

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
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

static inline void _wv_assign_tag_value( char **field, const char* value)
{
	if (!*field)
	{
		*field = strdup(value);
	}
	else if (!**field)
	{
		free(*field);
		*field = strdup(value);
	}
}

static void _wv_add_tag(struct song_metadata *psong, const char* item, const char* value, size_t value_len)
{
	if (!strncmp(item, "ALBUM", 5))
	{
		_wv_assign_tag_value(&psong->album, value);
	}
	else if (!strncmp(item, "ARTIST", 6) )
	{
		_wv_assign_tag_value(&psong->contributor[ROLE_ARTIST], value);
	}
	else if (!strncmp(item, "ARTISTSORT", 10) )
        {
                _wv_assign_tag_value(&psong->contributor_sort[ROLE_ARTIST], value);
        }

        else if (!strncmp(item, "COMPOSER", 8) )
        {
                _wv_assign_tag_value(&psong->contributor[ROLE_COMPOSER], value);
        }
        else if (!strncmp(item, "CONDUCTOR", 9) )
        {
                _wv_assign_tag_value(&psong->contributor[ROLE_CONDUCTOR], value);
        }
        else if(!strncmp(item, "ALBUMARTIST", 11))
	{
		_wv_assign_tag_value(&psong->contributor[ROLE_ALBUMARTIST], value);
	}
	else if(!strncmp(item, "ALBUMARTISTSORT", 15))
        {
                _wv_assign_tag_value(&psong->contributor_sort[ROLE_ALBUMARTIST], value);
        }
	else if(!strncmp(item, "TITLE", 5))
	{
		_wv_assign_tag_value(&psong->title, value);
	}
	else if(!strncmp(item, "TRACKNUMBER", 11))
	{
		psong->track = atoi(value);
	}
	else if(!strncmp(item, "DISCNUMBER", 10))
	{
		// <disc>/<total discs>
		const char* div = strchr(value, '/');
		if (div)
		{
			char *disc = strndup(value, div-value);
			if (disc)
			{
				psong->disc = atoi(disc);
				psong->total_discs = atoi(disc+1);
			}
			free(disc);
		}
		else
		{
			psong->disc = atoi(value);
		}
	}
	else if(!strncmp(item, "GENRE", 5))
	{
		_wv_assign_tag_value(&psong->genre, value);
	}
	else if (!strncmp(item, "COMMENT", 7) || !strncmp(item, "DESCRIPION", 10))
	{
		_wv_assign_tag_value(&psong->comment, value);
	}
	else if(!strncmp(item, "MUSICBRAINZ_ALBUMID", 19))
	{
		_wv_assign_tag_value(&psong->musicbrainz_albumid, value);
	}
	else if(!strncmp(item, "MUSICBRAINZ_TRACKID", 19))
	{
		_wv_assign_tag_value(&psong->musicbrainz_trackid, value);
	}
	else if(!strncmp(item, "MUSICBRAINZ_ARTISTID", 20))
	{
		_wv_assign_tag_value(&psong->musicbrainz_artistid, value);
	}
	else if(!strncmp(item, "MUSICBRAINZ_ALBUMARTISTID", 25))
	{
		_wv_assign_tag_value(&psong->musicbrainz_albumartistid, value);
	}
	else if(!strncmp(item, "DATE", 4))
	{
		char *year = NULL;

		if(value_len >= 10 &&
		   isdigit(value[0]) && isdigit(value[1]) && ispunct(value[2]) &&
		   isdigit(value[3]) && isdigit(value[4]) && ispunct(value[5]) &&
		   isdigit(value[6]) && isdigit(value[7]) && isdigit(value[8]) && isdigit(value[9]))
		{
			year = strndup(value+6,4);
			// nn-nn-yyyy
		}
		else
		{
			year = strndup(value, 4);
			// year first. year is at most 4 digit.
		}

		if (year) psong->year = atoi(year);
		free(year);
	}
}

static void _wv_add_mtag(struct song_metadata *psong, const char* item, const char* values, int values_len)
{
	int clen = 0, len;
	while(clen < values_len)
	{
		len = strlen(values);
		if (len)
		{
			_wv_add_tag(psong, item, values, len);
		}
		clen += len + 1;
		values += len + 1;
	}
}

static void _wv_add_binary_tag(struct song_metadata *psong, const char* item, const char* value, int value_len)
{
	if (!strncmp(item, "COVER", 5) || !strncmp(item, "FRONT", 5))
	{
		if (!psong->image)
		{
			if ((psong->image = malloc(value_len)))
			{
				memcpy(psong->image, value, value_len);
			}
		}
	}
}

static int
_get_wvtags(char *filename, struct song_metadata *psong)
{
	WavpackContext *ctx = NULL;
	char *error = NULL;
	int err = 0;
	int sample_rate;
	int num_tag_items, i, item_len, value_len, len;
	char *item = NULL, *value = NULL;

	error = (char*)malloc(80+1);
	ctx = WavpackOpenFileInput(filename, error, OPEN_TAGS|OPEN_NO_CHECKSUM, 0); 

	if(!ctx)
	{
		DPRINTF(E_ERROR, L_SCANNER, "Cannot extract tag from %s [%s]\n", filename, error);
		err = -1;
		goto _exit;
	}

	psong->samplerate = WavpackGetNativeSampleRate(ctx);
	psong->channels = WavpackGetNumChannels(ctx);
	sample_rate = WavpackGetSampleRate(ctx);
	if (sample_rate)
	{
        	int64_t total_samples = WavpackGetNumSamples64(ctx);
        	unsigned int sec = (unsigned int)(total_samples / sample_rate);
        	unsigned int ms = (unsigned int)(((total_samples % sample_rate) * 1000) / sample_rate);
		psong->song_length = (sec * 1000) + ms;
		if (psong->song_length) psong->bitrate = (((uint64_t)(psong->file_size) * 1000) / (psong->song_length / 8));
	}

	num_tag_items = WavpackGetNumTagItems(ctx);
        item_len = 20;
	value_len = 100;
	item = (char*)malloc(item_len+1);
	value = (char*)malloc(value_len+1);

	for(i=0; i<num_tag_items; ++i)
	{
		len = WavpackGetTagItemIndexed(ctx, i, NULL, 0);
		if (!len) continue;
		if (len > item_len)
		{
			char *new_item = (char*)realloc(item, len+1);
			if (!new_item) continue;
			item = new_item;
			item_len = len;
		}

		len = WavpackGetTagItemIndexed(ctx, i, item, item_len);
		if (!len) continue;
		
		len = WavpackGetTagItem(ctx, item, NULL, 0);
		if (!len) continue;
		if (len > value_len)
		{
			char *new_value = (char*)realloc(value, len+1);
			if (!new_value) continue;
			value = new_value;
			value_len = len;
		}

		len = WavpackGetTagItem(ctx, item, value, value_len);
		if (!len) continue;

		// add item
		_wv_add_mtag(psong, item, value, len);
	}

	num_tag_items = WavpackGetNumBinaryTagItems(ctx);

	for(i=0; i<num_tag_items; ++i)
	{
		len = WavpackGetBinaryTagItemIndexed(ctx, i, NULL, 0);
		if (!len) continue;
		if (len > item_len)
		{
			char *new_item = (char*)realloc(item, len+1);
			if (!new_item) continue;
			item = new_item;
			item_len = len;
		}

		len = WavpackGetBinaryTagItemIndexed(ctx, i, item, item_len);
		if (!len) continue;
		
		len = WavpackGetBinaryTagItem(ctx, item, NULL, 0);
		if (!len) continue;
		if (len > value_len)
		{
			char *new_value = (char*)realloc(value, len);
			if (!new_value) continue;
			value = new_value;
			value_len = len;
		}

		len = WavpackGetTagItem(ctx, item, value, value_len);
		if (!len) continue;

		// add item 		
		_wv_add_binary_tag(psong, item, value, len);
	}	

 _exit:
	if(ctx) WavpackCloseFile(ctx);
	free(error);
	free(item);
	free(value);

	return err;
}

static int
_get_wvfileinfo(char *filename, struct song_metadata *psong)
{
	psong->lossless = 1;
	psong->vbr_scale = 1;

	return 0;
}
