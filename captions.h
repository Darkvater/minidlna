/* Caption support
*
* Project : minidlna
* Website : http://sourceforge.net/projects/minidlna/
* Author  : Justin Maggard
*
* MiniDLNA media server
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
#ifndef __CAPTIONS_H__
#define __CAPTIONS_H__

void
check_for_captions(const char *path, int64_t sID);

void
add_caption_if_has_media(const char *path);

int
has_caption_with_id(int64_t ID);

char*
get_caption(int64_t ID);

int
delete_caption(const char *path);

#endif /* __CAPTIONS_H__ */
