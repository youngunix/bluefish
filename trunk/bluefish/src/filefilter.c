/* Bluefish HTML Editor
 * filefilter.c - 
 *
 * Copyright (C) 2002,2003,2004,2005,2006 Olivier Sessink
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#define DEBUG


#include <gtk/gtk.h>
#include <string.h>

#include "bluefish.h"
#include "bf_lib.h"
#include "dialog_utils.h"
#include "document.h"
#include "file.h"
#include "filetype.h"
#include "file_dialogs.h"
#include "gtk_easy.h"        /* destroy_disposable_menu_cb() */
#include "stringlist.h"      /* count_array() */

static GHashTable *hashtable_from_string(const gchar *mimetypes) {
	GHashTable *filetypes = g_hash_table_new_full(g_str_hash,g_str_equal,g_free,NULL);
	if (mimetypes){
		gchar **types = g_strsplit(mimetypes, ":", 127);
		gchar **type = types;
		while (*type) {
			g_hash_table_replace(filetypes, g_strdup(*type), GINT_TO_POINTER(1));
			type++;
		}
		g_strfreev(types);
	}
	return filetypes;
}

static Tfilter *new_filter(const gchar *name, const gchar *mode, const gchar *mimetypes) {
	Tfilter *filter = g_new(Tfilter,1);
	filter->name = g_strdup(name);
	filter->mode = atoi(mode);
	filter->filetypes = hashtable_from_string(mimetypes);
	return filter;
}

static void filter_destroy(Tfilter *filter) {
	g_free(filter->name);
	g_hash_table_destroy(filter->filetypes);
	g_free(filter);
}

/* 
 * WARNING: these filter are also used in the filechooser dialog (file->open in the menu)
 */
void fb2_filters_rebuild(void) {
	GList *tmplist;
	/* free any existing filters */
	tmplist = g_list_first(main_v->filefilters);
	while (tmplist) {
		filter_destroy(tmplist->data);
		tmplist = g_list_next(tmplist);
	}
	g_list_free(main_v->filefilters);
	main_v->filefilters = NULL;

	/* build a list of filters */
	main_v->filefilters = g_list_prepend(NULL, new_filter(_("All files"), "0", NULL));
	tmplist = g_list_first(main_v->props.filefilters);
	while (tmplist) {
		gchar **strarr = (gchar **) tmplist->data;
		if (count_array(strarr) == 3) {
			Tfilter *filter = new_filter(strarr[0], strarr[1], strarr[2]);
			main_v->filefilters = g_list_prepend(main_v->filefilters, filter);
		}
		tmplist = g_list_next(tmplist);
	}
}

static void restore_filter_from_config(Tfilter *filter, const gchar *origname) {
	GList *tmplist;
	gchar **strarr=NULL;
	gint mode;
	
	if (!origname) return;
	
	tmplist = g_list_first(main_v->props.filefilters);
	while (tmplist) {
		strarr = (gchar **) tmplist->data;
		if (strarr && strcmp(strarr[0],origname)==0) {
			/* config string found */
			break;
		}
		tmplist = g_list_next(tmplist);
	}
	if (!strarr) return;
	
	if (strcmp(filter->name,origname)!=0) {
		g_free(filter->name);
		filter->name = g_strdup(origname);
	}
	mode = atoi(strarr[1]);
	if (mode != filter->mode) {
		filter->mode = mode;
	}
	
	g_hash_table_destroy(filter->filetypes);
	filter->filetypes = hashtable_from_string(strarr[2]);
}

static void hashtable_to_string(gpointer key,gpointer value,gpointer data) {
	g_string_append((GString* )data,(gchar *)key);
	g_string_append_c((GString* )data,':');
}

static void apply_filter_to_config(Tfilter *filter, const gchar *origname) {
	GList *tmplist;
	gchar **strarr = NULL;
	GString *gstr;
	if (origname) {
		/* find the config string, if it existed before */
		tmplist = g_list_first(main_v->props.filefilters);
		while (tmplist) {
			strarr = (gchar **) tmplist->data;
			if (strarr && strcmp(strarr[0],origname)==0) {
				/* config string found */
				break;
			}
			tmplist = g_list_next(tmplist);
		}
	}
	if (strarr == NULL) {
		/* no config string with this name, */
		strarr = g_new0(gpointer, 4);
		main_v->props.filefilters = g_list_prepend(main_v->props.filefilters, strarr);
	}
	/* the config string has three entries: the name, inverse filtering, filetypes */
	if (strarr[0]) g_free(strarr[0]);
	strarr[0] = g_strdup(filter->name);
	if (strarr[1]) g_free(strarr[1]);
	strarr[1] = g_strdup_printf("%d",filter->mode);
	gstr = g_string_new("");
	g_hash_table_foreach(filter->filetypes,hashtable_to_string,gstr);
	if (strarr[2]) g_free(strarr[2]);
	strarr[2] = g_string_free(gstr,FALSE);
}

/*
the filefilter gui has one listmodel with all filetypes currently known in bluefish, 
and two filtermodels, in_model shows all types in the filter, out_model shows all other filetypes
and two treeviews.

*/

typedef struct {
	GtkWidget *win;
	GtkTreeModel *lmodel;
	GtkTreeModelFilter *in_model; /* shows all the types IN the filter */
	GtkTreeModelFilter *out_model; /* shows all types OUTside the filter */
	GtkWidget *in_view;
	GtkWidget *out_view;
	
	Tfilter *curfilter;
	gchar *origname;
} Tfilefiltergui;

static void filefiltergui_destroy_lcb(GtkWidget *widget, gpointer data) {
	Tfilefiltergui *ffg = data;
	g_free(ffg->origname);
	g_object_unref(ffg->lmodel);
	window_destroy(ffg->win);
	g_free(ffg);
	DEBUG_MSG("filefiltergui_destroy_lcb, done\n");	
}

static void filefiltergui_cancel_clicked(GtkWidget *widget, gpointer data) {
	Tfilefiltergui *ffg = data;
	restore_filter_from_config(ffg->curfilter, ffg->origname);
	filefiltergui_destroy_lcb(widget, data);
}

static void filefiltergui_ok_clicked(GtkWidget *widget, gpointer data) {
	Tfilefiltergui *ffg = data;
	apply_filter_to_config(ffg->curfilter, ffg->origname);
	filefiltergui_destroy_lcb(widget, data);
}

static gboolean filefiltergui_infilter_visiblefunc(GtkTreeModel *model,GtkTreeIter *iter,gpointer data) {
	Tfilefiltergui *ffg = data;
	gboolean retval = FALSE;
	gchar *mimetype=NULL;
	gtk_tree_model_get(model, iter, 0, &mimetype, -1);
	if (mimetype) {
		DEBUG_MSG("filter %p, lookup %s in hashtable %p\n",ffg->curfilter,mimetype,ffg->curfilter->filetypes);
		retval = (g_hash_table_lookup(ffg->curfilter->filetypes, mimetype) != NULL);
		g_free(mimetype);
	}
	
	return retval;
}
static gboolean filefiltergui_outfilter_visiblefunc(GtkTreeModel *model,GtkTreeIter *iter,gpointer data) {
	return !filefiltergui_infilter_visiblefunc(model,iter,data);
}

static void filefiltergui_add_filetypes(gpointer key,gpointer value,gpointer data) {
	Tfilefiltergui *ffg = data;
	
	if (g_hash_table_lookup(main_v->filetypetable, key) == NULL) {
		GtkTreeIter it;
		Tfiletype *ft = get_filetype_for_mime_type(key);
		gtk_list_store_prepend(GTK_LIST_STORE(ffg->lmodel),&it);
		gtk_list_store_set(GTK_LIST_STORE(ffg->lmodel),&it,0,ft->mime_type,1,ft->icon,-1);
	}
}

static void filefiltergui_2right_clicked(GtkWidget *widget, gpointer data) {
	Tfilefiltergui *ffg = data;
	GtkTreeIter iter;
	GtkTreeSelection *select;
	GtkTreeModel *model;

	/* get the selection */
	select = gtk_tree_view_get_selection(GTK_TREE_VIEW(ffg->out_view));
	if (gtk_tree_selection_get_selected(select, &model, &iter)) {
		gchar *mime_type;
		gtk_tree_model_get(model, &iter, 0, &mime_type, -1);
		/* add the selection to the filter */
		DEBUG_MSG("filefiltergui_2right_clicked, adding %s\n",mime_type);
		g_hash_table_replace(ffg->curfilter->filetypes, mime_type, GINT_TO_POINTER(1));

		DEBUG_MSG("filefiltergui_2right_clicked, refilter\n");
		/* refilter */
		gtk_tree_model_filter_refilter(ffg->in_model);
		gtk_tree_model_filter_refilter(ffg->out_model);
	} else {
		DEBUG_MSG("filefiltergui_2right_clicked, nothing selected\n");
	}
}

static void filefiltergui_2left_clicked(GtkWidget *widget, gpointer data) {
	Tfilefiltergui *ffg = data;
	GtkTreeIter iter;
	GtkTreeSelection *select;
	GtkTreeModel *model;

	/* get the selection */
	select = gtk_tree_view_get_selection(GTK_TREE_VIEW (ffg->in_view));
	if (gtk_tree_selection_get_selected(select, &model, &iter)) {
		gchar *mime_type;
		gtk_tree_model_get(model, &iter, 0, &mime_type, -1);
		/* add the selection to the filter */
		DEBUG_MSG("filefiltergui_2left_clicked, removing %s\n",mime_type);
		g_hash_table_remove(ffg->curfilter->filetypes, mime_type);

		g_free (mime_type);
		DEBUG_MSG("filefiltergui_2left_clicked, refilter\n");
		/* refilter */
		gtk_tree_model_filter_refilter(ffg->in_model);
		gtk_tree_model_filter_refilter(ffg->out_model);
	} else {
		DEBUG_MSG("filefiltergui_2left_clicked, nothing selected\n");
	}
}

void filefilter_gui(Tfilter *filter) {
	GtkCellRenderer *renderer;
	GtkTreeViewColumn *column;
	GList *tmplist;
	GtkWidget *table,*hbox,*but,*vbox;

	Tfilefiltergui *ffg = g_new0(Tfilefiltergui,1);
	ffg->curfilter = filter;
	if (filter) {
		ffg->origname = g_strdup(filter->name);
	}
	DEBUG_MSG("filefilter_gui, editing filter %p\n",ffg->curfilter); 
	ffg->win = window_full2(_("Edit filter"), GTK_WIN_POS_MOUSE, 10, filefiltergui_destroy_lcb,ffg, TRUE, NULL);
	ffg->lmodel = gtk_list_store_new(2, G_TYPE_STRING, GDK_TYPE_PIXBUF);
	
	/* fill the list model from the currently known filetypes */
	tmplist = g_list_first(main_v->filetypelist);
	while (tmplist) {
		GtkTreeIter it;
		Tfiletype *ft = tmplist->data;
		if (strncmp(ft->mime_type,"x-directory",11)!=0) {
			gtk_list_store_prepend(GTK_LIST_STORE(ffg->lmodel),&it);
			DEBUG_MSG("filefilter_gui, adding %s\n",ft->mime_type);
			gtk_list_store_set(GTK_LIST_STORE(ffg->lmodel),&it,0,ft->mime_type,1,ft->icon,-1);
		}
		tmplist = g_list_next(tmplist);
	}
	/* make sure that all filetypes that exist in the current filter are shown */
	g_hash_table_foreach(ffg->curfilter->filetypes,filefiltergui_add_filetypes,ffg);
	
	ffg->in_model = gtk_tree_model_filter_new(ffg->lmodel, NULL);
	gtk_tree_model_filter_set_visible_func(ffg->in_model,filefiltergui_infilter_visiblefunc,ffg,NULL);
	ffg->out_model = gtk_tree_model_filter_new(ffg->lmodel, NULL);
	gtk_tree_model_filter_set_visible_func(ffg->out_model,filefiltergui_outfilter_visiblefunc,ffg,NULL);

	table = gtk_table_new(4,3,FALSE);
	
	ffg->in_view = gtk_tree_view_new_with_model(ffg->in_model);
	renderer = gtk_cell_renderer_text_new();
	column = gtk_tree_view_column_new_with_attributes(_("Mime type"),renderer,"text", 0,NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW(ffg->in_view), column);
	renderer = gtk_cell_renderer_pixbuf_new();
	column = gtk_tree_view_column_new_with_attributes(_("Icon"),renderer,"pixbuf", 1,NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW(ffg->in_view), column);
	gtk_table_attach_defaults(table,ffg->in_view,2,3,1,2);
	
	ffg->out_view = gtk_tree_view_new_with_model(ffg->out_model);
	renderer = gtk_cell_renderer_text_new();
	column = gtk_tree_view_column_new_with_attributes(_("Mime type"),renderer,"text", 0,NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW(ffg->out_view), column);
	renderer = gtk_cell_renderer_pixbuf_new();
	column = gtk_tree_view_column_new_with_attributes(_("Icon"),renderer,"pixbuf", 1,NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW(ffg->out_view), column);
	gtk_table_attach_defaults(table,ffg->out_view,0,1,1,2);
	
	vbox = gtk_vbox_new(TRUE,5);
	but = gtk_button_new_with_label("->");
	g_signal_connect(but,"clicked",filefiltergui_2right_clicked,ffg);
	gtk_box_pack_start(vbox,but,TRUE,TRUE,0);
	but = gtk_button_new_with_label("<-");
	g_signal_connect(but,"clicked",filefiltergui_2left_clicked,ffg);
	gtk_box_pack_start(vbox,but,TRUE,TRUE,0);
	gtk_table_attach(table,vbox,1,2,1,2,GTK_EXPAND|GTK_FILL,GTK_EXPAND,5,5);
	
	hbox = gtk_hbutton_box_new();
	gtk_hbutton_box_set_layout_default(GTK_BUTTONBOX_END);
	gtk_button_box_set_spacing(GTK_BUTTON_BOX(hbox), 12);
	but = bf_stock_cancel_button(G_CALLBACK(filefiltergui_cancel_clicked), ffg);
	gtk_box_pack_start(GTK_BOX(hbox),but, FALSE, FALSE, 0);
	but = bf_stock_ok_button(G_CALLBACK(filefiltergui_ok_clicked), ffg);
	gtk_box_pack_start(GTK_BOX(hbox),but, FALSE, FALSE, 0);
	
	gtk_table_attach_defaults(GTK_TABLE(table), hbox, 0, 3, 3, 4);


	gtk_container_add(ffg->win, table);
	gtk_widget_show_all(ffg->win);

	
}
