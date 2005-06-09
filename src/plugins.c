/* Copyright (C) 2005 Olivier Sessink
 * plugins.c - handle plugin loading
 *
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#ifdef ENABLEPLUGINS
#define DEBUG

#include "config.h"

#include <gtk/gtk.h>
#include <string.h>
#include "plugins.h"
#include "stringlist.h"

typedef struct {
	gchar *filename;
} TPrivatePluginData;

#define PRIVATE(var) ((TPrivatePluginData *)var->private)

static TBluefishPlugin *load_plugin(const gchar *filename) {
	GModule *module;
	TBluefishPlugin *bfplugin;
	gpointer func;
	TBluefishPlugin *(*getplugin)();
	
	module = g_module_open(filename,0);
	if (!module) {
		DEBUG_MSG("load_plugin, failed to load %s with error %s\n",filename, g_module_error());
		return NULL;
	}

	if (!g_module_symbol(module, "getplugin",&func)) {
		DEBUG_MSG("load_plugin: module %s does not have getplugin(): %s\n",filename,g_module_error());
		g_module_close(module);
		return NULL;
	}
	getplugin = func;
	bfplugin = getplugin();
	if (!bfplugin) {
		DEBUG_MSG("load_plugin: %s getplugin() returned NULL\n",filename);
		g_module_close(module);
		return NULL;
	}
	bfplugin->private = g_new0(TPrivatePluginData,1);
	PRIVATE(bfplugin)->filename = g_strdup(filename);
	return bfplugin;
}

#ifndef PLUGINDIR
#define PLUGINDIR "/home/olivier/cvsbluefish/src/plugins/"
#endif

static const gchar *plugin_from_filename(const gchar *path) {
	TBluefishPlugin *bfplugin;
	bfplugin = load_plugin(path);
	if (bfplugin && bfplugin->bfplugin_version == BFPLUGIN_VERSION
					&& bfplugin->document_size == sizeof(Tdocument)
					&& bfplugin->sessionvars_size == sizeof(Tsessionvars)
					&& bfplugin->globalsession_size == sizeof(Tglobalsession)
					&& bfplugin->bfwin_size == sizeof(Tbfwin)
					&& bfplugin->project_size == sizeof(Tproject)
					&& bfplugin->main_size == sizeof(Tmain)) {
		DEBUG_MSG("bluefish_load_plugins, found htmlbar.so, init!\n");
		bfplugin->init();
		main_v->plugins = g_slist_prepend(main_v->plugins,bfplugin);
		return bfplugin->name;
	} 
	return NULL;
}

void bluefish_load_plugins(void) {
	GError *error = NULL;
	GPatternSpec* patspec;
	GDir* gdir;
	GList *oldlist;
	const gchar *tmp;

	DEBUG_MSG("bluefish_load_plugins, loading from %s\n",PLUGINDIR);
	gdir = g_dir_open(PLUGINDIR ,0,&error);
	if (error) {
		/* BUG: handle the error  */
		return;
	}
	patspec = g_pattern_spec_new("*.so");
	oldlist = main_v->props.plugin_config;
	main_v->props.plugin_config = NULL;
	tmp = g_dir_read_name(gdir);
	while (tmp) {
		if (g_pattern_match(patspec, strlen(tmp), tmp, NULL)) {
			gchar *path = g_strconcat(PLUGINDIR, tmp, NULL);
			gchar *compare[] = {path, NULL}, **arr;

			arr = arraylist_value_exists(oldlist, compare, 1, FALSE);
			if (arr) {
				GList *link;
				DEBUG_MSG("bluefish_load_plugins, found %s in configfile (len(oldlist)=%d, len(plugin_config=%d)\n",arr[0],g_list_length(oldlist),g_list_length(main_v->props.plugin_config));
				link = g_list_find(oldlist,arr);
				oldlist = g_list_remove_link(oldlist, link);
				main_v->props.plugin_config = g_list_concat(main_v->props.plugin_config, link);
			}
			if (arr && arr[1][0] == '0') {
				DEBUG_MSG("plugin %s is disabled\n",path);
			} else {
				const gchar *plugname;
				DEBUG_MSG("try loading plugin %s\n",path);
				plugname = plugin_from_filename(path);
				if (plugname) {
					if (!arr) {
						arr = array_from_arglist(path, "1", plugname, NULL);
						main_v->props.plugin_config = g_list_append(main_v->props.plugin_config, arr);
					}
				} else { /* no plugname ==> failed to load */
					DEBUG_MSG("bluefish_load_plugins, load failed\n");
					if (arr && count_array(arr)>=2) {
						g_free(arr[1]);
						arr[1] = g_strdup("0");
					} else {
						arr = array_from_arglist(path, "0", "error loading plugin", NULL);
						main_v->props.plugin_config = g_list_append(main_v->props.plugin_config, arr);
					}
				}
			}
			g_free(path);
		}
		tmp = g_dir_read_name(gdir);
	}
	g_dir_close(gdir);
	g_pattern_spec_free(patspec);
	free_arraylist(oldlist);
}

void bluefish_cleanup_plugins(void) {
	/* BUG: should be finished when plugins are really working */
}

/* can be called by g_list_foreach() */
void bfplugins_gui(gpointer data, gpointer user_data) {
	Tbfwin *bfwin = user_data;
	TBluefishPlugin *bfplugin = data;
	DEBUG_MSG("bluefish_plugins_gui, init_gui for plugin %p and bfwin %p\n",bfplugin,bfwin);	
	bfplugin->init_gui(bfwin);
}

GList *bfplugins_register_globses_config(GList *list) {
	GSList *tmplist = main_v->plugins;
	while (tmplist) {
		TBluefishPlugin *bfplugin = tmplist->data;
		if (bfplugin->register_globses_config) {
			list = bfplugin->register_globses_config(list);
		}
		tmplist =  g_slist_next(tmplist);
	}
	return list;
}
GList *bfplugins_register_session_config(GList *list) {
	GSList *tmplist = main_v->plugins;
	while (tmplist) {
		TBluefishPlugin *bfplugin = tmplist->data;
		if (bfplugin->register_session_config) {
			list = bfplugin->register_session_config(list);
		}
		tmplist =  g_slist_next(tmplist);
	}
	return list;
}

#endif /* ENABLEPLUGINS */
