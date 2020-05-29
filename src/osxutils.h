/* Bluefish HTML Editor
 * osxutils.h - non-portable OSX support functions
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

#ifndef __OSXUTILS_H_
#define __OSXUTILS_H_

void osx_setenv (int *argc, char **argv[]);

#ifdef MAC_INTEGRATION
gboolean osx_open_file_cb(GtkosxApplication *app, gchar *path, gpointer user_data);
gboolean osx_block_termination_cb(GtkosxApplication *app, gpointer user_data);
void osx_will_terminate_cb(GtkosxApplication *app, gpointer user_data);
void osx_startup_finished_cb(gpointer data);
#endif

#endif /* __OSXUTILS_H_ */