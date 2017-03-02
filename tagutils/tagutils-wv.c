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

static inline void _wv_assign_tag_value(char **field, const char *value)
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

static void _wv_extract_fraction(int *pnominator, int *pdenominator, const char *value)
{
	const char* div = strchr(value, '/');
	if (div)
	{
		char* nominator = strndup(value, div-value);
		if (nominator)
		{
			*pnominator = atoi(nominator);
			*pdenominator = atoi(div+1);
			free(nominator);
		}
		else
		{
			*pnominator = *pdenominator = 0;
		}
	}
	else
	{
		*pnominator = atoi(value);
		*pdenominator = 0;
	}
}

static void _wv_add_tag(struct song_metadata *psong, const char *item, const char *value, size_t value_len)
{
	if (!strcasecmp(item, "ALBUM"))
	{
		_wv_assign_tag_value(&psong->album, value);
	}
	else if (!strcasecmp(item, "ARTIST") )
	{
		_wv_assign_tag_value(&(psong->contributor[ROLE_ARTIST]), value);
	}
	else if (!strcasecmp(item, "ARTISTSORT") )
        {
                _wv_assign_tag_value(&(psong->contributor_sort[ROLE_ARTIST]), value);
        }
        else if (!strcasecmp(item, "COMPOSER") )
        {
                _wv_assign_tag_value(&(psong->contributor[ROLE_COMPOSER]), value);
        }
        else if (!strcasecmp(item, "CONDUCTOR") )
        {
                _wv_assign_tag_value(&(psong->contributor[ROLE_CONDUCTOR]), value);
        }
        else if (!strcasecmp(item, "ALBUMARTIST"))
	{
		_wv_assign_tag_value(&psong->contributor[ROLE_BAND], value);
	}
	else if (!strcasecmp(item, "ALBUMARTISTSORT"))
        {
                _wv_assign_tag_value(&(psong->contributor_sort[ROLE_BAND]), value);
        }
	else if (!strcasecmp(item, "TITLE"))
	{
		_wv_assign_tag_value(&psong->title, value);
	}
	else if (!strcasecmp(item, "TRACK"))
	{
		// <track>/<total tracks>
		_wv_extract_fraction(&psong->track, &psong->total_tracks, value);
	}
	else if (!strcasecmp(item, "DISC"))
	{
		// <disc>/<total discs>
		_wv_extract_fraction(&psong->disc, &psong->total_discs, value);
	}
	else if (!strcasecmp(item, "GENRE"))
	{
		_wv_assign_tag_value(&psong->genre, value);
	}
	else if (!strcasecmp(item, "COMMENT") || !strcasecmp(item, "DESCRIPION"))
	{
		_wv_assign_tag_value(&psong->comment, value);
	}
	else if (!strcasecmp(item, "DESCRIPTION") || !strcasecmp(item, "DESC"))
	{
		_wv_assign_tag_value(&psong->description, value);
	}
	else if (!strcasecmp(item, "MUSICBRAINZ_ALBUMID"))
	{
		_wv_assign_tag_value(&psong->musicbrainz_albumid, value);
	}
	else if (!strcasecmp(item, "MUSICBRAINZ_TRACKID"))
	{
		_wv_assign_tag_value(&psong->musicbrainz_trackid, value);
	}
	else if (!strcasecmp(item, "MUSICBRAINZ_ARTISTID"))
	{
		_wv_assign_tag_value(&psong->musicbrainz_artistid, value);
	}
	else if(!strcasecmp(item, "MUSICBRAINZ_ALBUMARTISTID"))
	{
		_wv_assign_tag_value(&psong->musicbrainz_albumartistid, value);
	}
	else if (!strcasecmp(item, "DATE") || !strcasecmp(item, "YEAR"))
	{
		_wv_assign_tag_value(&psong->date, value);
	}
}

static void _wv_add_mtag(struct song_metadata *psong, const char *item, char *values, int values_len)
{
	if (values_len>0)
	{
		int i;

		// maybe many strings
		for(i=0; i<values_len; ++i)
		{
			if (!values[i]) values[i] = ',';
		}
		_wv_add_tag(psong, item, values, values_len);
	}
}

static void _wv_add_binary_tag(struct song_metadata *psong, const char *item, const char *value, int value_len)
{
	if (!strcasecmp(item, "COVER") || !strcasecmp(item, "FRONT") || !strcasecmp(item, "COVER ART (FRONT)"))
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

static int _get_wvtags(const char *filename, struct song_metadata *psong)
{
	WavpackContext *ctx = NULL;
	char *error = NULL;
	int err = 0;
	int sample_rate;
	int num_tag_items, i, item_len, value_len, len;
	char *item = NULL, *value = NULL;

	error = (char*)malloc(80+1);
	ctx = WavpackOpenFileInput(filename, error, OPEN_WVC|OPEN_TAGS|OPEN_DSD_NATIVE|OPEN_NO_CHECKSUM, 0); 

	if (!ctx)
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
	}

	psong->bitrate = (int)WavpackGetAverageBitrate(ctx, (WavpackGetMode(ctx) & MODE_WVC) != 0);
	psong->lossless = ((WavpackGetMode(ctx) & MODE_LOSSLESS) != 0);

	if (!(WavpackGetMode(ctx) & MODE_VALID_TAG))
	{
		DPRINTF(E_WARN, L_SCANNER, "%s contains no valid tags\n", filename);
		goto _exit;
	}

        item_len = 20;
	value_len = 200;
	item = (char*)malloc(item_len+1);
	value = (char*)malloc(value_len+1);

	num_tag_items = WavpackGetNumTagItems(ctx);
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

		len = WavpackGetTagItemIndexed(ctx, i, item, item_len+1);
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

		len = WavpackGetTagItem(ctx, item, value, value_len+1);
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

		len = WavpackGetBinaryTagItemIndexed(ctx, i, item, item_len+1);
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

	if (psong->title)
	{
		unsigned char fmt = WavpackGetFileFormat(ctx);
		if (fmt == WP_FORMAT_DFF || fmt == WP_FORMAT_DSF)
		{ // DSD
			char *dst_title = NULL;
			int res = xasprintf(&dst_title, "%s [DSD]", psong->title);
			if (res)
			{
				free(psong->title);
				psong->title = dst_title;
			}
		}
	}

 _exit:
	if (ctx) WavpackCloseFile(ctx);
	free(error);
	free(item);
	free(value);

	return err;
}

static int _get_wvfileinfo(const char *filename, struct song_metadata *psong)
{
	psong->vbr_scale = 1;
	return 0;
}
