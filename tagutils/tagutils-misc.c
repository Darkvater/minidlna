//=========================================================================
// FILENAME	: tagutils-misc.c
// DESCRIPTION	: Misc routines for supporting tagutils
//=========================================================================
// Copyright (c) 2008- NETGEAR, Inc. All Rights Reserved.
//=========================================================================

/* This program is free software; you can redistribute it and/or modify
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

/**************************************************************************
* Language
**************************************************************************/

static const size_t MAX_ICONV_BUF = 1024;

typedef enum {
	ICONV_OK,
	ICONV_TRYNEXT,
	ICONV_FATAL
} iconv_result;

#ifdef HAVE_ICONV
static iconv_result
do_iconv(const char* to_ces, const char* from_ces,
	 ICONV_CONST char *inbuf,  size_t inbytesleft,
	 char *outbuf_orig, size_t outbytesleft_orig)
{
	size_t rc;
	iconv_result ret = ICONV_OK;

	size_t outbytesleft = outbytesleft_orig - 1;
	char* outbuf = outbuf_orig;

	iconv_t cd  = iconv_open(to_ces, from_ces);

	if(cd == (iconv_t)-1)
	{
		return ICONV_FATAL;
	}
	rc = iconv(cd, &inbuf, &inbytesleft, &outbuf, &outbytesleft);
	if(rc == (size_t)-1)
	{
		if(errno == E2BIG)
		{
			ret = ICONV_FATAL;
		}
		else
		{
			ret = ICONV_TRYNEXT;
			memset(outbuf_orig, '\0', outbytesleft_orig);
		}
	}
	iconv_close(cd);

	return ret;
}
#else // HAVE_ICONV
static iconv_result
do_iconv(const char* to_ces, const char* from_ces,
	 char *inbuf,  size_t inbytesleft,
	 char *outbuf_orig, size_t outbytesleft_orig)
{
	return ICONV_FATAL;
}
#endif // HAVE_ICONV

#define N_LANG_ALT 8
typedef struct {
	const char *lang;
	const char *cpnames[N_LANG_ALT];
} iconv_map_t;

const iconv_map_t iconv_map[] = {
	{ "ja_JP",     { "CP932", "CP950", "CP936", "ISO-8859-1", 0 } },
	{ "zh_CN",  { "CP936", "CP950", "CP932", "ISO-8859-1", 0 } },
	{ "zh_TW",  { "CP950", "CP936", "CP932", "ISO-8859-1", 0 } },
	{ "ko_KR",  { "CP949", "ISO-8859-1", 0 } },
	{ 0,        { 0 } }
};
static int lang_index = -1;

static int
_lang2cp(const char *lang)
{
	int cp;

	if(!lang || lang[0] == '\0')
		return -1;
	for(cp = 0; iconv_map[cp].lang; cp++)
	{
		if(!strcasecmp(iconv_map[cp].lang, lang))
			return cp;
	}
	return -2;
}

static unsigned char*
_get_utf8_text(const id3_ucs4_t* native_text)
{
	unsigned char *utf8_text = NULL;
	char *in, *in8, *iconv_buf;
	iconv_result rc;
	int i, n;

	in = (char*)id3_ucs4_latin1duplicate(native_text);
	if(!in)
	{
		goto out;
	}

	in8 = (char*)id3_ucs4_utf8duplicate(native_text);
	if(!in8)
	{
		free(in);
		goto out;
	}

	iconv_buf = (char*)calloc(MAX_ICONV_BUF, sizeof(char));
	if(!iconv_buf)
	{
		free(in); free(in8);
		goto out;
	}

	i = lang_index;
	// (1) try utf8 -> default
	rc = do_iconv(iconv_map[i].cpnames[0], "UTF-8", in8, strlen(in8), iconv_buf, MAX_ICONV_BUF);
	if(rc == ICONV_OK)
	{
		utf8_text = (unsigned char*)in8;
		free(iconv_buf);
	}
	else if(rc == ICONV_TRYNEXT)
	{
		// (2) try default -> utf8
		rc = do_iconv("UTF-8", iconv_map[i].cpnames[0], in, strlen(in), iconv_buf, MAX_ICONV_BUF);
		if(rc == ICONV_OK)
		{
			utf8_text = (unsigned char*)iconv_buf;
		}
		else if(rc == ICONV_TRYNEXT)
		{
			// (3) try other encodes
			for(n = 1; n < N_LANG_ALT && iconv_map[i].cpnames[n]; n++)
			{
				rc = do_iconv("UTF-8", iconv_map[i].cpnames[n], in, strlen(in), iconv_buf, MAX_ICONV_BUF);
				if(rc == ICONV_OK)
				{
					utf8_text = (unsigned char*)iconv_buf;
					break;
				}
			}
			if(!utf8_text)
			{
				// cannot iconv
				utf8_text = (unsigned char*)id3_ucs4_utf8duplicate(native_text);
				free(iconv_buf);
			}
		}
		free(in8);
	}
	free(in);

 out:
	if(!utf8_text)
	{
		utf8_text = (unsigned char*)strdup("UNKNOWN");
	}

	return utf8_text;
}

static const size_t _VC_MAX_VALUE_LEN = 1024;

static inline int _strncasecmp(const char *name, const size_t name_len, const char *tag_name, const size_t tag_name_len)
{
	if (name_len != tag_name_len) return -1;
	return strncasecmp(name, tag_name, tag_name_len);
}

static void _vc_assign_value(char **field, const char *value, const size_t value_len)
{
	if (!*field)
	{
		*field = strndup(value, value_len);
	}
	else
	{ // append ',' and value
		size_t field_len, new_len;
		char *value_str = strndup(value, value_len);
		DPRINTF(E_ERROR, L_SCANNER, "Appending VC1 %s to %s\n", value_str, *field);
		free(value_str);

		field_len = strlen(*field);
		new_len = field_len + value_len + 2;
		DPRINTF(E_ERROR, L_SCANNER, "Appending VC2 %d -> %d [%d]\n", (int)(field_len+1), (int)new_len, (int)value_len);
		if (new_len > _VC_MAX_VALUE_LEN) return;
		char *new_val = (char*)realloc(*field, new_len);
		if (new_val)
		{
			new_val[field_len] = ',';
			strncpy(new_val + field_len + 1, value, value_len);
			new_val[new_len] = '\0';
			*field = new_val;

			DPRINTF(E_ERROR, L_SCANNER, "Appending VC3 %s.\n", new_val);
		}
	}
}

static int _vc_assign_int_value(int *field, const char *value, const size_t value_len)
{
	char *v = strndup(value, value_len);
	if (v) *field = atoi(v);
	free(v);
	return *field;
}

static void
vc_scan(struct song_metadata *psong, const char *comment, const size_t length)
{
	const char *eq = (const char*)memchr(comment, '=', length);
	size_t name_len, value_len;
	const char *value;

	if (!eq || (eq == comment)) return;
	name_len = eq - comment;
	value_len = length - (name_len+1);
	if (!value_len || (value_len>_VC_MAX_VALUE_LEN)) return;
	value = eq + 1;

	if (!_strncasecmp(comment, name_len, "ALBUM", 5))
	{
		_vc_assign_value(&psong->album, value, value_len);
	}
	else if (!_strncasecmp(comment, name_len, "ARTIST", 6))
	{
		_vc_assign_value(&(psong->contributor[ROLE_ARTIST]), value, value_len);
	}
	else if (!_strncasecmp(comment, name_len, "ARTISTSORT", 10))
	{
		_vc_assign_value(&(psong->contributor_sort[ROLE_ARTIST]), value, value_len);
	} 
	else if (!_strncasecmp(comment, name_len, "ALBUMARTIST", 11))
	{
		_vc_assign_value(&(psong->contributor[ROLE_BAND]), value, value_len);
	}
	else if (!_strncasecmp(comment, name_len, "ALBUMARTISTSORT", 15))
	{
		_vc_assign_value(&(psong->contributor_sort[ROLE_BAND]), value, value_len);
	}
	else if (!_strncasecmp(comment, name_len, "TITLE", 5))
	{
		_vc_assign_value(&psong->title, value, value_len);
	}
	else if (!_strncasecmp(comment, name_len, "TRACKNUMBER", 11))
	{
		_vc_assign_int_value(&psong->track, value, value_len);
	}
	else if (!_strncasecmp(comment, name_len, "TOTALTRACKS", 11))
	{
		_vc_assign_int_value(&psong->total_tracks, value, value_len);
	}
	else if (!_strncasecmp(comment, name_len, "TOTALDISCS", 10))
	{
		_vc_assign_int_value(&psong->total_discs, value, value_len);
	}
	else if (!_strncasecmp(comment, name_len, "DISCNUMBER", 10))
	{
		_vc_assign_int_value(&psong->disc, value, value_len);
	}
	else if (!_strncasecmp(comment, name_len, "COMPOSER", 8))
	{
		_vc_assign_value(&(psong->contributor[ROLE_COMPOSER]), value, value_len);
	}
	else if (!_strncasecmp(comment, name_len, "CONDUCTOR", 9))
	{
		_vc_assign_value(&(psong->contributor[ROLE_CONDUCTOR]), value, value_len);
	}
	else if (!_strncasecmp(comment, name_len, "GENRE", 5))
	{
		_vc_assign_value(&psong->genre, value, value_len);
	}
	else if (!_strncasecmp(comment, name_len, "DATE", 4) || !_strncasecmp(comment, name_len, "YEAR", 4))
	{
		_vc_assign_value(&psong->date, value, value_len);
	}
	else if (!_strncasecmp(comment, name_len, "COMMENT", 7))
	{
		_vc_assign_value(&psong->comment, value, value_len);
	}
	else if (!_strncasecmp(comment, name_len, "DESCRIPTION", 11) || !_strncasecmp(comment, name_len, "DESC", 4))
	{
		_vc_assign_value(&psong->description, value, value_len);
	}
	else if (!_strncasecmp(comment, name_len, "MUSICBRAINZ_ALBUMID", 19))
	{
		_vc_assign_value(&psong->musicbrainz_albumid, value, value_len);
	}
	else if (!_strncasecmp(comment, name_len, "MUSICBRAINZ_TRACKID", 19))
	{
		_vc_assign_value(&psong->musicbrainz_trackid, value, value_len);
	}
	else if (!_strncasecmp(comment, name_len, "MUSICBRAINZ_ARTISTID", 20))
	{
		_vc_assign_value(&psong->musicbrainz_artistid, value, value_len);
	}
	else if (!_strncasecmp(comment, name_len, "MUSICBRAINZ_ALBUMARTISTID", 25))
	{
		_vc_assign_value(&psong->musicbrainz_albumartistid, value, value_len);
	}
	else
	{
		char *name = strndup(comment, name_len);
		char *value_str = strndup(value, value_len);
		if (name && value_str) DPRINTF(E_ERROR, L_SCANNER, "Unhandled Vorbis Comment %s=%s\n", name, value_str);
		free(name);
		free(value_str);

		_vc_assign_value(&psong->description, value, value_len);
	}
}
