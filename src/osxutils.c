/* Bluefish HTML Editor
 * osxutils.c - non-portable OSX support functions
 *
 * Copyright (C) 2014-2020 Olivier Sessink
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

#ifdef MAC_INTEGRATION
#include <limits.h>
#include <stdlib.h>


#include "bluefish.h"
#include "osxutils.h"

static void set_env_on_path(const gchar *dirn, const gchar *relpath, const gchar *envname, gboolean extenddir) {
	gchar tmppath[PATH_MAX];
	gchar *tmp = g_strconcat(dirn, relpath, NULL);
	if (realpath(tmp, tmppath)) {
		if (extenddir) {
			gchar *curval, *newval;
			curval = g_getenv(envname);
			newval = g_strconcat(tmppath, ":", curval, NULL);
			g_setenv(envname, newval, TRUE);
			g_free(newval);
		} else {
			g_setenv(envname, tmppath, TRUE);
		}
	}
	g_free(tmp);
}

void
osx_setenv(const gchar *progname)
{
	/* removing launcher.sh and moving the code to the binary, required in Catalina */
	gchar tmppath[PATH_MAX];
	gchar *dirn;
	realpath(progname, tmppath);
	dirn = g_path_get_dirname(tmppath);
	
	set_env_on_path(dirn, "/../../Contents/Resources/lib/", "DYLD_LIBRARY_PATH", TRUE);
	set_env_on_path(dirn, "/../../Contents/Resources/etc/", "XDG_CONFIG_DIRS", TRUE);
	set_env_on_path(dirn, "/../../Contents/Resources/share/", "XDG_DATA_DIRS", TRUE);
	set_env_on_path(dirn, "/../../Contents/Resources/", "GTK_DATA_PREFIX", FALSE);
	set_env_on_path(dirn, "/../../Contents/Resources/", "GTK_EXE_PREFIX", FALSE);
	set_env_on_path(dirn, "/../../Contents/Resources/", "GTK_PATH", FALSE);
	set_env_on_path(dirn, "/../../Contents/Resources/etc/gtk-2.0/gtkrc", "GTK2_RC_FILES", FALSE);
	set_env_on_path(dirn, "/../../Contents/Resources/etc/gtk-2.0/gtk.immodules", "GTK_IM_MODULE_FILE", FALSE);
	set_env_on_path(dirn, "/../../Contents/Resources/etc/gtk-2.0/gdk-pixbuf.loaders", "GDK_PIXBUF_MODULE_FILE", FALSE);
	set_env_on_path(dirn, "/../../Contents/Resources/etc/pango/pangorc", "PANGO_RC_FILE", FALSE);
	
	g_free(dirn);
}

gboolean osx_open_file_cb(GtkosxApplication *app, gchar *path, gpointer user_data) {
	GList *tmplist;
	Tbfwin *bfwin;
	GFile *uri = g_file_new_for_path(path);
	//Find toplevel in focus
	tmplist = g_list_first(main_v->bfwinlist);
	while(tmplist) {
		bfwin= BFWIN(tmplist->data);
		if (gtk_window_is_active(GTK_WINDOW(bfwin->main_window))) {
			break;
		}
		tmplist = g_list_next(tmplist);
	}
	g_print("osx_open_file_cb, open %s\n",path);
	file_handle(uri, bfwin , NULL, TRUE, TRUE);
	g_object_unref(uri);
	return TRUE;
}

gboolean osx_block_termination_cb(GtkosxApplication *app, gpointer user_data) {
	GList *tmplist, *duplist;
	main_v->osx_status = 1;
	DEBUG_MSG("osx_block_termination, started\n");
	duplist = g_list_copy(main_v->bfwinlist); /* Copy bfwinlist first, since using just tmplist causes unexpected behavior as bfwin is destroyed */
	tmplist = g_list_first(duplist);
	while (tmplist) {
		Tbfwin *bfwin = BFWIN(tmplist->data);
		bfwin_osx_terminate_event(NULL,NULL,bfwin);
		tmplist = g_list_next(tmplist);
	}
	g_list_free(duplist);
	return FALSE;
}

void osx_will_terminate_cb(GtkosxApplication *app, gpointer user_data) {
	/* Connect final shutdown routine here */
	g_print("gtkosx terminating application");
	flush_queue();
	rcfile_save_global_session();
	gtk_main_quit();
}





#endif /* MAC_INTEGRATION */
