/* Bluefish HTML Editor
 * compatibility.c
 *
 * Copyright (C) 2014-2023 Olivier Sessink
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "bluefish.h"

#if !GLIB_CHECK_VERSION(2,28,0)
 void g_list_free_full(GList *list,GDestroyNotify free_func)
 {
	GList *tmplist;
 	for (tmplist = g_list_first(list);tmplist;tmplist=tmplist->next) {
		free_func(tmplist->data);
	}
	g_list_free(list);
 }
 #endif

#ifndef HAVE_STRCASESTR
#include <ctype.h>
char *strcasestr(const char *a, const char *b)
{
	size_t l;
	char f[3];

	snprintf(f, sizeof(f), "%c%c", tolower(*b), toupper(*b));
	for (l = strcspn(a, f); l != strlen(a); l += strcspn(a + l + 1, f) + 1)
		if (strncasecmp(a + l, b, strlen(b)) == 0)
			return(a + l);
	return(NULL);
}
#endif

#if !GLIB_CHECK_VERSION(2,40,0)
gboolean
g_str_is_ascii(const gchar *str)
{
	gsize i;
	for (i = 0; str[i]; i++)
	if (str[i] & 0x80)
		return FALSE;
	return TRUE;
}

gchar *
g_str_to_ascii_minimal(const gchar *pattern)
{
	gsize i=0, j=0;
	gchar *retval = g_malloc(strlen(pattern));
	while(pattern[i]) {
		if (pattern[i] & 0x80)
			retval[j]=pattern[i];
			j++;
		i++;
	}
	retval[i] = '\0';
	return retval;
}

#endif
 