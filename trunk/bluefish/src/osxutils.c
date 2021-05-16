/* Bluefish HTML Editor
 * osxutils.c - non-portable OSX support functions
 *
 * Copyright (C) 2014-2020 Olivier Sessink, Andrius
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

#define DEBUG

#include <gtk/gtk.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/types.h>
#include <string.h>	
#include "bluefish.h"

#ifdef PLATFORM_DARWIN

#include <CoreFoundation/CFPreferences.h>
#include <CoreFoundation/CFPropertyList.h>
#include <CoreFoundation/CFString.h>

#ifdef MAC_INTEGRATION
#include <gtkosxapplication.h>
#endif

#include "osxutils.h"
#include "file.h" /* file_handle() */
#include "bfwin.h" /* bfwin_osx */
#include "gtk_easy.h"			/* flush_queue() */
#include "rcfile.h"				/* rcfile_parse_main() */


/*Assign locales for bluefish available languages*/

struct locale_table
{
  const char *code;
  const char *locale;
};
static const struct locale_table code_table[] =
  {
    { "ar", "ar_AE" },
    { "bg", "bg_BG" },
    { "ca", "ca_ES" },
    { "cs", "cs_CZ" },
    { "da", "da_DK" },
    { "de", "de_DE" },
    { "el", "el_GR" },
    { "en", "en_US" },
    { "es", "es_ES" },
    { "eu", "eu_ES" },
    { "fi", "fi_FI" },
    { "fr", "fr_FR" },
    { "gl", "gl_ES" },
    { "hu", "hu_HU" },
    { "it", "it_IT" },
    { "ja", "ja_JP" },
    { "ko", "ko_KR" },
    { "nb", "nb_NO" },
    { "nl", "nl_NL" },
    { "nn", "nn_NO" },
    { "it", "it_IT" },
    { "pl", "pl_PL" },
    { "pt", "pt_PT" },
    { "ro", "ro_RO" },
    { "ru", "ru_RU" },
    { "sk", "sk_SK" },
    { "sr", "sr_YU" },
    { "sv", "sv_SE" },
    { "ta", "ta_IN" },
    { "tr", "tr_TR" },
    { "uk", "uk_UA" },
    { "zh", "zh_CN" }
 };


static void 
match_locale (const struct locale_table *table, size_t table_size, const char *string, char **result) {
	if (*result != NULL) {
		g_free(*result);
		*result = NULL;
	}
	for(int i=0; i < table_size; i++) {
		if (strcmp (table[i].code, string) == 0) {
        	*result = strdup(table[i].locale);
        	break;	
		}
	}
}

static void 
osx_set_env_on_path(const gchar *dirn, const gchar *relpath, const gchar *envname, gboolean extenddir) {
	gchar tmppath[PATH_MAX];
	gchar *tmp = g_strconcat(dirn, relpath, NULL);
	if (realpath(tmp, tmppath)) {
		if (extenddir) {
			const gchar *curval;
			gchar *newval;
			curval = g_getenv(envname);
			if (curval) {
				newval = g_strconcat(tmppath, ":", curval, NULL);
				DEBUG_MSG("osx_setenv, extending environment %s:%s\n", envname, newval);
				g_setenv(envname, newval, TRUE);
				g_free(newval);
			} else {
				DEBUG_MSG("osx_setenv, setting environment %s:%s\n", envname, tmppath);
				g_setenv(envname, tmppath, TRUE);
			}
		} else {
			DEBUG_MSG("osx_setenv, setting environment %s:%s\n", envname, tmppath);
			g_setenv(envname, tmppath, TRUE);
		}
	}
	g_free(tmp);
}

void
osx_setenv(int *argc, char **argv[])
{
	int i=0, n=0;
	gchar *region = NULL, *tmp=NULL;
	gchar *localeString = NULL;
	gchar *short_locale = NULL;
	gchar *matched_locale = NULL;
	gchar *locale_dir = NULL;
	gchar tmppath[PATH_MAX];
	gchar tmploc[PATH_MAX];
	gchar tmptest[PATH_MAX];
	gchar *dirn;
	struct stat sb;

/* on some OSX installations open file limit is 256 and we might need more during search */
	struct rlimit limit;
	limit.rlim_cur = 10000;
	limit.rlim_max = 10000;
	setrlimit (RLIMIT_NOFILE, &limit);
	gint newargc = 0;
	for ( i = 0; i < *argc; i++) {
		DEBUG_MSG("osx_setenv, printing argument #%i = %s\n", i, (*argv)[i]);
		if (strcmp((*argv)[i], "-AppleLanguages") == 0) {
			i++;
			if (i < *argc) { //avoid crash if no option is provided
				DEBUG_MSG("osx_setenv, printing argument #%i = %s\n", i, (*argv)[i]);
				if ((tmp = strchr((*argv)[i], '(')) != NULL) {
					if (strlen(tmp) > 2) { //Check if we have at least two characters for constructing short locale string
						short_locale = strndup(tmp+1, 2);
					}
				} 
				else if (strlen((*argv)[i]) ==2) { //If we have exactly two symbols, we try to use them
					short_locale = strdup((*argv)[i]);
				}
			}
		}
/* remove MacOS session identifier from the command line args */
		else if (!g_str_has_prefix ((*argv)[i], "-psn_")) {
          (*argv)[newargc] = (*argv)[i];
          newargc++;
		}
	}
	if (*argc > newargc) {
		(*argv)[newargc] = NULL; /* glib expects NULL terminated array */
		*argc = newargc;
    }
	
	realpath((*argv)[0], tmppath);
	dirn = g_path_get_dirname(tmppath);
/*It is necessary to set current working directory in order to have relative paths resolved properly
* We default here to xxx.app/Contents/MacOS, so relative paths should be like "../../Contents/Resources/...." */
	chdir(dirn);

	osx_set_env_on_path(dirn, "/../../Contents/Resources/lib/enchant", "ENCHANT_MODULE_PATH", FALSE);
	osx_set_env_on_path(dirn, "/../../Contents/Resources/etc/", "XDG_CONFIG_HOME", TRUE);
	osx_set_env_on_path(dirn, "/../../Contents/Resources/share/", "XDG_DATA_HOME", TRUE);
	osx_set_env_on_path(dirn, "/../../Contents/Resources/", "GTK_DATA_PREFIX", FALSE);
	osx_set_env_on_path(dirn, "/../../Contents/Resources/", "GTK_EXE_PREFIX", FALSE);
	osx_set_env_on_path(dirn, "/../../Contents/Resources/", "GTK_PATH", FALSE);
	osx_set_env_on_path(dirn, "/../../Contents/Resources/share/glib-2.0/schemas", "GSETTINGS_SCHEMA_DIR", FALSE);
	osx_set_env_on_path(dirn, "/../../Contents/Resources/etc/gtk-3.0/gtk.immodules", "GTK_IM_MODULE_FILE", FALSE);
	osx_set_env_on_path(dirn, "/../../Contents/Resources/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache", "GDK_PIXBUF_MODULE_FILE", FALSE);
	osx_set_env_on_path(dirn, "/../../Contents/Resources/lib", "PANGO_LIBDIR", FALSE);
	osx_set_env_on_path(dirn, "/../../Contents/Resources/etc", "PANGO_SYSCONFDIR", FALSE);
	osx_set_env_on_path(dirn, "/../../Contents/Resources/lib", "CHARSETALIASDIR", FALSE);
	
/* MacOSX AppleLocale string is constructed using preferred language (first 2 symbols) and region (last two symbols); this is all we need
* AppleLocale should be set on all systems, however, not all 5 symbol locales are valid, for example there is no locale en_EN
* MacOSX AppleLanguages differs on different versions of MacOS, on old systems it is 2-symbol list, on newer it is 5-symbol list with last two symbols specifying region; we do not use it at the moment
* 5-symbol locale is required in order to avoid "default to "C" locale" error message
* If there is no matching locale->language, bluefish defaults to english
* 1. If -AppleLanguages option provides valid 2-symbol string, we check if this locale exist and use it.
* 2. We take AppleLocale and if there is matching locale for full string, then just use it.
* 3. If there is available bluefish language for first 2-symbol string, construct 5-symbol string from code_table.
* 4. If there is available bluefish language for last 2-symbol string, construct 5-symbol string from it using code_table
* 5. Default to "en_US"
* Locale strings are checked against ones available in bluefish bundle; it is not really necessary if we have correct list in code_table; it is precaution if something is broken. Locale folder in bundle is /share/locale.	 
*/
	
	locale_dir = g_strconcat(dirn, "/../../Contents/Resources/share/locale", NULL);
	if (short_locale) {
		if (realpath (locale_dir, tmploc) && !stat (tmploc,&sb) && S_ISDIR (sb.st_mode)) {
			g_snprintf (tmptest, sizeof(tmptest), "%s/%s", tmploc, short_locale);
			if (!stat (tmptest,&sb) && S_ISDIR (sb.st_mode)) {
				match_locale(code_table, sizeof (code_table) / sizeof (code_table[0]), short_locale, &matched_locale);
				DEBUG_MSG("osx_setenv, user-supplied locale is %s\n", matched_locale);
				if (matched_locale) {
					if (setlocale(LC_ALL, matched_locale)) {
						DEBUG_MSG("osx_setenv, user-supplied -AppleLanguages option is valid, setting environment for %s.\n", matched_locale);
					} else {
						DEBUG_MSG("osx_setenv, user-supplied -AppleLanguages option does not have valid locale, will use it as supplied.\n");
					}
					g_setenv("LC_ALL",  matched_locale, TRUE);
					g_free(short_locale);
					g_free(matched_locale);
					g_free(locale_dir);
					g_free(dirn);
					return;
				}
			}
		}
	}
	
	CFTypeRef CFLocaleString = CFPreferencesCopyAppValue (CFSTR ("AppleLocale"), kCFPreferencesCurrentApplication);
	if (CFLocaleString != NULL && (CFGetTypeID (CFLocaleString) == CFStringGetTypeID ())) {
		CFIndex length = CFStringGetLength (CFLocaleString);
  		CFIndex maxlen = CFStringGetMaximumSizeForEncoding (length, kCFStringEncodingUTF8);
  		localeString = g_malloc (maxlen + 1);
  		Boolean done = CFStringGetCString (CFLocaleString, (char *) localeString, maxlen, kCFStringEncodingUTF8);
		if (done) {
			DEBUG_MSG("osx_setenv, AppleLocale found, %s.\n", localeString);
			tmp = strrchr(localeString, '_');
			if (tmp != NULL) {
				region = strdup(tmp+1);
	      } else {
				region = strdup("US");
	      }
	   } else {
   		g_free (localeString);
   		DEBUG_MSG("osx_setenv, AppleLocale string is null, using fallback locale string.\n");
   		localeString = strdup("en_US"); //default to en_US 
			region = strdup("US");
	   }
		CFRelease(CFLocaleString);
	} else {
		localeString = strdup("en_US"); //default to en_US 
		region = strdup("US");
	}
	if (setlocale(LC_ALL, localeString)) {
		DEBUG_MSG("osx_setenv, long locale string %s matched.\n", localeString);
		g_setenv("LC_ALL",  localeString, TRUE);
		g_free(localeString);
		g_free(region);
		return;
	}
	DEBUG_MSG("osx_setenv, AppleLocale is %s, trying to mach first two-symbols.\n", localeString);

/* Now try to match preferred locales to the ones available in bluefish bundle; it is not really necessary if we have correct list in code_table; it is precaution if something is broken;
*  We check if locale folder in /share/locale exist */	 
	if (realpath (locale_dir, tmploc) && !stat (tmploc,&sb) && S_ISDIR (sb.st_mode)) {
		short_locale = g_strndup(localeString, 2);		
		g_snprintf (tmptest, sizeof(tmptest), "%s/%s", tmploc, short_locale);
		if (!stat (tmptest,&sb) && S_ISDIR (sb.st_mode)) {
			match_locale(code_table, sizeof (code_table) / sizeof (code_table[0]), short_locale, &matched_locale);
			DEBUG_MSG("osx_setenv, 2-symbol string %s matched.\n", matched_locale);
			if (matched_locale) {
				if (setlocale(LC_ALL, matched_locale)) {
					DEBUG_MSG("osx_setenv, locale %s is valid, setting environment.\n", matched_locale);
					g_setenv("LC_ALL",  matched_locale, TRUE);
					g_free(localeString);
					g_free(region);
					g_free(short_locale);
					g_free(matched_locale);
					g_free(locale_dir);
					g_free(dirn);
					return;
				}
			}
		}
/* Still no luck, try now region part */
		g_free(short_locale);
		short_locale = g_ascii_strdown (region, -1);
		g_snprintf (tmptest, sizeof(tmptest), "%s/%s", tmploc, short_locale);
		if (!stat (tmptest,&sb) && S_ISDIR (sb.st_mode)) {
			match_locale(code_table, sizeof (code_table) / sizeof (code_table[0]), short_locale, &matched_locale);
			if (matched_locale) {
				if (setlocale(LC_ALL, matched_locale)) {
					DEBUG_MSG("osx_setenv, region locale %s valid, setting environment. \n", matched_locale);
					g_setenv("LC_ALL",  matched_locale, TRUE);
					g_free(localeString);
					g_free(region);
					g_free(short_locale);
					g_free(matched_locale);
					g_free(locale_dir);
					g_free(dirn);
					return;
				}
			}
		}	
	}
// Fallback locale that should be existing on all systems
	matched_locale = strdup("en_US");	
	DEBUG_MSG("osx_setenv, using fallback locale %s. \n", matched_locale);
	g_setenv("LC_ALL",  matched_locale, TRUE);
	g_free(localeString);
	g_free(region);
	g_free(short_locale);
	g_free(matched_locale);
	g_free(locale_dir);
	g_free(dirn);
}

#ifdef MAC_INTEGRATION
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
	duplist = g_list_copy(main_v->bfwinlist); /* Copy bfwinlist first, since using just tmplist causes unexpected behaviour as bfwin is destroyed */
	tmplist = g_list_first(duplist);
	while (tmplist) {
		Tbfwin *bfwin = BFWIN(tmplist->data);
		bfwin_osx_terminate_event(NULL,NULL,bfwin);
		tmplist = g_list_next(tmplist);
	}
	g_list_free(duplist);
	return TRUE;
}

void osx_will_terminate_cb(GtkosxApplication *app, gpointer user_data) {
	/* Connect final shutdown routine here */
	g_print("gtkosx terminating application");
	flush_queue();
	rcfile_save_global_session();
	gtk_main_quit();
}

void osx_startup_finished_cb(gpointer data) {
	Tstartup *startup=data;
	g_main_loop_quit (startup->startup_main_loop);
	g_main_loop_unref (startup->startup_main_loop);
	g_free(startup);
}

#endif /* MAC_INTEGRATION */
#endif
