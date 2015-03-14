/* File notification events
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
#ifndef __NOTIFY_EVENT_H__
#define __NOTIFY_EVENT_H__

typedef int(*insert_callback)(const char *path);

int notify_event_insert_directory(const char *name, const char *path, media_types types, insert_callback callback);
int notify_event_remove_directory(const char *path);
int notify_event_insert_file(char* name, const char* path, media_types types);
int notify_event_remove_file(const char *path);

#endif /* __NOTIFY_EVENT_H__ */