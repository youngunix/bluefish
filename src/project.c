/* Bluefish HTML Editor
 * project.c - project functionality
 *
 * Copyright (C) 2003 Olivier Sessink
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
/* #define DEBUG */

#include <gtk/gtk.h>
#include <string.h>
#include <stdlib.h>

#include "bluefish.h"
#include "project.h"
#include "stringlist.h"
#include "gui.h"
#include "document.h"
#include "gtk_easy.h"
#include "rcfile.h"
#include "bf_lib.h"
#include "filebrowser.h"
#include "menu.h"

Tbfwin *project_is_open(gchar *filename) {
	GList *tmplist;
	tmplist = g_list_first(main_v->bfwinlist);
	while(tmplist){
		if (BFWIN(tmplist->data)->project 
				&& BFWIN(tmplist->data)->project->filename
				&& strcmp(BFWIN(tmplist->data)->project->filename, filename)==0) {
			return BFWIN(tmplist->data);
		}
		tmplist = g_list_next(tmplist);
	}
	return NULL;
}

static void update_project_filelist(Tbfwin *bfwin, Tproject *prj) {
	free_stringlist(prj->files);
	prj->files = return_filenamestringlist_from_doclist(bfwin->documentlist);
}

/* bfwin is allowed to be NULL for an empty project */
static Tproject *create_new_project(Tbfwin *bfwin) {
	Tproject *prj;
	prj = g_new0(Tproject,1);
	prj->name = g_strdup(_("New project"));
	if (bfwin) {
		update_project_filelist(bfwin,prj);
		prj->session = bfwin->session;
	} else {
		prj->session = g_new0(Tsessionvars,1);
	}
	if (prj->files) {
		gint len;
		gchar *somefile, *prefix;
		len = find_common_prefixlen_in_stringlist(prj->files);
		somefile = (gchar *)prj->files->data;
		prefix = g_strndup(somefile, len);
		if (prefix[strlen(prefix)-1] == '/') {
			prj->basedir = g_strdup(prefix);
		} else {
			prj->basedir = g_path_get_dirname(prefix);
		}
		g_free(prefix);
	} else {
		prj->basedir = g_strdup("");
	}
	prj->webdir = g_strdup("");
	prj->template = g_strdup("");
	prj->view_main_toolbar = main_v->props.view_main_toolbar;
	prj->view_left_panel = main_v->props.view_left_panel;
	prj->view_custom_menu = main_v->props.view_custom_menu;
	prj->view_html_toolbar = main_v->props.view_html_toolbar;
	prj->word_wrap = main_v->props.word_wrap;
	return prj;
}

gboolean project_save(Tbfwin *bfwin, gboolean save_as) {
	gboolean retval;
	DEBUG_MSG("project_save, bfwin=%p, save_as=%d\n",bfwin,save_as);
	if (!bfwin->project) {
		/* there is no project yet, we have to create one */
		bfwin->project= create_new_project(bfwin);
	}
	DEBUG_MSG("project_save, num files was %d\n", g_list_length(bfwin->project->files));
	update_project_filelist(bfwin, bfwin->project);
	bfwin->project->recentfiles = limit_stringlist(bfwin->project->recentfiles, main_v->props.max_recent_files, TRUE);
	
	bfwin->project->session->searchlist = limit_stringlist(bfwin->project->session->searchlist, 10, TRUE);
	bfwin->project->session->replacelist = limit_stringlist(bfwin->project->session->replacelist, 10, TRUE);

	if (save_as || bfwin->project->filename == NULL) {
		gchar *filename = return_file_w_title(NULL, _("Enter Bluefish project filename"));
		gint suflen,filen;
		if (!filename) {
			return FALSE;
		}
		suflen = strlen(main_v->props.project_suffix);
		filen = strlen(filename);
		if (filen > suflen && strcmp(&filename[filen - suflen], main_v->props.project_suffix)==0) {
			bfwin->project->filename = filename;
		} else {
			bfwin->project->filename = g_strconcat(filename, main_v->props.project_suffix,NULL);
			g_free(filename);
		}
	}
	
	DEBUG_MSG("project_save, saving project %p to file %s\n",bfwin->project,bfwin->project->filename);
	retval = rcfile_save_project(bfwin->project, bfwin->project->filename);
	add_to_recent_list(bfwin,bfwin->project->filename, FALSE, TRUE);
	return retval;
}

void set_project_menu_widgets(Tbfwin *bfwin, gboolean win_has_project) {
	GtkItemFactory * ifactory;
	ifactory = gtk_item_factory_from_widget(bfwin->menubar);
	gtk_widget_set_sensitive(gtk_item_factory_get_widget(ifactory, _("/Project/Save")), win_has_project);
	gtk_widget_set_sensitive(gtk_item_factory_get_widget(ifactory, _("/Project/Save as...")), win_has_project);
	gtk_widget_set_sensitive(gtk_item_factory_get_widget(ifactory, _("/Project/Close & save")), win_has_project);
	gtk_widget_set_sensitive(gtk_item_factory_get_widget(ifactory, _("/Project/Edit Project Options...")), win_has_project);
}

void project_open_from_file(Tbfwin *bfwin, gchar *fromfilename) {
	Tbfwin *prwin;
	Tproject *prj;
	gboolean retval;

	/* first we test if the project is already open */
	prwin = project_is_open(fromfilename);
	if (prwin != NULL) {
		DEBUG_MSG("project_open_from_file, project is open in bfwin=%p\n",prwin);
		gtk_window_present(GTK_WINDOW(prwin->main_window));
		return;
	}

	prj = g_new0(Tproject,1);
	prj->session = g_new0(Tsessionvars,1);
	retval = rcfile_parse_project(prj, fromfilename);
	if (!retval) {
		DEBUG_MSG("project_open_from_file, failed parsing the project at file %s\n",fromfilename);
		g_free(prj);
		return;
	}
	add_to_recent_list(bfwin,fromfilename, FALSE, TRUE);
	prj->filename = g_strdup(fromfilename);
	DEBUG_MSG("project_open_from_file, basedir=%s\n",prj->basedir);
	if (bfwin->project == NULL && test_only_empty_doc_left(bfwin->documentlist)) {
		/* we will use this Bluefish window to open the project */
		prwin = bfwin;
		/* now we need to clean the session, and reset it to the session from the project */
		free_session(bfwin->session);
		bfwin->session = prj->session;
		DEBUG_MSG("project_open_from_file, calling docs_new_from_files for existing bfwin=%p\n",prwin);
		prwin->project = prj;
		
		gui_set_html_toolbar_visible(prwin, prj->view_html_toolbar, TRUE);
		gui_set_main_toolbar_visible(prwin, prj->view_main_toolbar, TRUE);
		gui_set_custom_menu_visible(prwin, prj->view_custom_menu, TRUE);
		left_panel_show_hide_toggle(prwin,FALSE,prj->view_left_panel, TRUE);
		filebrowser_set_basedir(prwin, prj->basedir);
		docs_new_from_files(prwin, prj->files, TRUE);
	} else {
		/* we will open a new Bluefish window for this project */
		DEBUG_MSG("project_open_from_file, we need a new window\n");
		prwin = gui_new_window(prj->files, prj);
		DEBUG_MSG("project_open_from_file, new window with files ready\n");
		gui_set_title(prwin, prwin->current_document);
	}
	set_project_menu_widgets(prwin, TRUE);
	recent_menu_init_project(prwin);
}

static void project_open(Tbfwin *bfwin) {
	/* first we ask for a filename */
	gchar *filename = return_file_w_title(NULL, _("Select Bluefish project filename"));
	if (filename) {
		DEBUG_MSG("project_open, for filename %s\n",filename);
		project_open_from_file(bfwin,filename);
		g_free(filename);
	} else {
		DEBUG_MSG("project_open, no filename.., returning\n");
	}
}

static void project_destroy(Tproject *project) {
	free_stringlist(project->files);
	free_session(project->session);
	g_free(project->filename);
	g_free(project->name);
	g_free(project->basedir);
	g_free(project->webdir);
	g_free(project->template);
	g_free(project);
}

gboolean project_save_and_close(Tbfwin *bfwin) {
	if (project_save(bfwin, FALSE)) {
		DEBUG_MSG("project_save_and_close, save returned TRUE\n");
		file_close_all_cb(NULL,bfwin);
		if (test_only_empty_doc_left(bfwin->documentlist)) {
			DEBUG_MSG("project_save_and_close, all documents are closed\n");
			add_to_recent_list(bfwin,bfwin->project->filename, TRUE, TRUE);
			project_destroy(bfwin->project);
			/* the window gets a new fresh session */
			bfwin->session = g_new0(Tsessionvars,1);
			bfwin->project = NULL;
			gui_set_title(bfwin, bfwin->current_document);
			filebrowser_set_basedir(bfwin, NULL);
			recent_menu_from_file(bfwin, "/.bluefish/recentlist", FALSE);
			DEBUG_MSG("project_save_and_close, returning TRUE\n");
			return TRUE;
		}
	}
	DEBUG_MSG("project_save_and_close, returning FALSE\n");
	return FALSE;
}

typedef enum {
	name,
	basedir,
	webdir,
	template,
	word_wrap,
	projecteditor_entries_num
} Tprojecteditor_entries;

typedef struct {
	GtkWidget *win;
	Tbfwin *bfwin;
	Tproject *project;
	GtkWidget *entries[projecteditor_entries_num];
	gboolean project_created_by_editor;
} Tprojecteditor;

static void project_edit_destroy_lcb(GtkWidget *widget, Tprojecteditor *pred) {
	DEBUG_MSG("project_edit_destroy_lcb, called for pred=%p\n",pred);
/*	gtk_widget_destroy(pred->win);*/
	if (pred->project) {
		pred->project->editor = NULL;
	}
	g_free(pred);
}

static void project_edit_cancel_clicked_lcb(GtkWidget *widget, Tprojecteditor *pred) {
	if (pred->project_created_by_editor) {
		DEBUG_MSG("project_edit_cancel_clicked_lcb, destroy project\n");
		project_destroy(pred->project);
		if (pred->bfwin) pred->bfwin->project = NULL;
	}
	gtk_widget_destroy(pred->win);
}

static void project_edit_ok_clicked_lcb(GtkWidget *widget, Tprojecteditor *pred) {
	gchar *oldbasedir;
	gchar *tmp;
	Tproject *prj = pred->project;
	if (pred->bfwin == NULL) {
		pred->bfwin = gui_new_window(NULL, pred->project);
	}
	DEBUG_MSG("project_edit_ok_clicked_lcb, Tproject at %p\n",prj);
	oldbasedir = g_strdup(prj->basedir);
	string_apply(&prj->name, pred->entries[name]);
	string_apply(&prj->basedir, pred->entries[basedir]);
	tmp = prj->basedir;
	prj->basedir = ending_slash(prj->basedir);
	g_free(tmp);
	string_apply(&prj->webdir, pred->entries[webdir]);
	string_apply(&prj->template, pred->entries[template]);
	integer_apply(&prj->word_wrap, pred->entries[word_wrap], TRUE);
	DEBUG_MSG("project_edit_ok_clicked_lcb, name=%s, basedir=%s, webdir=%s\n",prj->name,prj->basedir,prj->webdir);
	if (strcmp(prj->basedir, oldbasedir)!=0 && strlen(prj->basedir) > 2) {
		filebrowser_set_basedir(pred->bfwin, prj->basedir);
	}
	g_free(oldbasedir);
	gui_set_title(pred->bfwin, pred->bfwin->current_document);
	set_project_menu_widgets(pred->bfwin, TRUE);
	gtk_widget_destroy(pred->win);
	project_save(pred->bfwin,FALSE);
}

/*
if NULL is passed as bfwin, a new window will be created for this project once OK is clicked
*/
void project_edit(Tbfwin *bfwin) {
	GtkWidget *vbox, *but, *hbox, *label, *table;
	gchar *wintitle = NULL;
	Tprojecteditor *pred;

	if (bfwin && bfwin->project && bfwin->project->editor) {
		gtk_window_present(GTK_WINDOW(((Tprojecteditor *)bfwin->project->editor)->win));
		return;
	}
	pred = g_new(Tprojecteditor,1);
	if (!bfwin || !bfwin->project) {
		/* there is no project yet, we have to create one */
		DEBUG_MSG("project_edit, no project yet, create one\n");
		pred->project= create_new_project(bfwin);
		if (bfwin) {
			bfwin->project = pred->project;
		}
		pred->project_created_by_editor = TRUE;
		wintitle = g_strdup(_("Create New Project"));
	} else {
		pred->project_created_by_editor = FALSE;
		wintitle = g_strdup(_("Edit Project"));
		if (bfwin) {
			pred->project = bfwin->project;
		}
	}
	DEBUG_MSG("project_edit, Tproject at %p\n",bfwin->project);
	pred->bfwin = bfwin;
	pred->project->editor = pred;
	
	pred->win = window_full2(wintitle, GTK_WIN_POS_CENTER_ALWAYS, 5
			, G_CALLBACK(project_edit_destroy_lcb), pred, TRUE, NULL);
	vbox = gtk_vbox_new(FALSE,0);
	gtk_container_add(GTK_CONTAINER(pred->win),vbox);
	
	if (wintitle) {
		g_free(wintitle);
	}
	
	label = gtk_label_new(NULL);
	gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, TRUE, 0);
	gtk_label_set_line_wrap(GTK_LABEL(label),TRUE);
	{
		gchar *message;
		message = g_strdup_printf(_("This project contains %d files"), g_list_length(pred->project->files));
		gtk_label_set_markup(GTK_LABEL(label), message);
	}
	
	table = gtk_table_new (5, 3, FALSE);
	gtk_table_set_col_spacings (GTK_TABLE (table), 12);
	gtk_table_set_row_spacings (GTK_TABLE (table), 6);	
	gtk_box_pack_start (GTK_BOX (vbox), table, FALSE, FALSE, 12);
	
	pred->entries[name] = entry_with_text(pred->project->name, 255);
	gtk_widget_set_size_request(GTK_WIDGET(pred->entries[name]), 250, -1);
	bf_mnemonic_label_tad_with_alignment(_("Project _Name:"), pred->entries[name], 1, 0.5, table, 0, 1, 0, 1);
	gtk_table_attach_defaults(GTK_TABLE(table), pred->entries[name], 2, 3, 0, 1);

	pred->entries[basedir] = entry_with_text(pred->project->basedir, 255);
	bf_mnemonic_label_tad_with_alignment(_("_Local Directory:"), pred->entries[basedir], 1, 0.5, table, 0, 1, 1, 2);
	gtk_table_attach_defaults(GTK_TABLE(table), pred->entries[basedir], 2, 3, 1, 2);

	pred->entries[webdir] = entry_with_text(pred->project->webdir, 255);
	bf_mnemonic_label_tad_with_alignment(_("_Remote Directory:"), pred->entries[webdir], 1, 0.5, table, 0, 1, 2, 3);
	gtk_table_attach_defaults(GTK_TABLE(table), pred->entries[webdir], 2, 3, 2, 3);
	
	pred->entries[template] = entry_with_text(pred->project->template, 255);
	bf_mnemonic_label_tad_with_alignment(_("_Template:"), pred->entries[template], 1, 0.5, table, 0, 1, 3, 4);
	gtk_table_attach_defaults(GTK_TABLE(table), pred->entries[template], 2, 3, 3, 4);	

	gtk_table_set_row_spacing(GTK_TABLE(table), 3, 18);
	pred->entries[word_wrap] = checkbut_with_value(_("_Word wrap by default"), pred->project->word_wrap);
	gtk_table_attach_defaults(GTK_TABLE(table), pred->entries[word_wrap], 0, 3, 4, 5);
		
	gtk_box_pack_start (GTK_BOX (vbox), gtk_hseparator_new(), FALSE, FALSE, 12);
	hbox = gtk_hbutton_box_new();
	gtk_hbutton_box_set_layout_default(GTK_BUTTONBOX_END);
	gtk_button_box_set_spacing(GTK_BUTTON_BOX(hbox), 6);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
	but = bf_stock_cancel_button(G_CALLBACK(project_edit_cancel_clicked_lcb), pred);
	gtk_box_pack_start(GTK_BOX(hbox), but, FALSE, FALSE, 0);
	if (pred->project_created_by_editor == TRUE) {
		but = bf_generic_button_with_image(_("Create _Project"), 0, G_CALLBACK(project_edit_ok_clicked_lcb), pred);
	} else {
		but = bf_stock_ok_button(G_CALLBACK(project_edit_ok_clicked_lcb), pred);
	}
	gtk_box_pack_start(GTK_BOX(hbox), but, FALSE, FALSE, 0);
	gtk_widget_grab_default(but);
	gtk_widget_show_all(pred->win);
}

typedef struct {
	GtkWidget *win;
	GtkWidget *rad[2];
	Tbfwin *bfwin;
} Tpc;

static void project_create_destroy_lcb(GtkWidget *widget, Tpc *pc) {
	g_free(pc);
}

static void project_create_cancel_clicked_lcb(GtkWidget *widget, Tpc *pc) {
	gtk_widget_destroy(pc->win);
}

static void project_create_ok_clicked_lcb(GtkWidget *widget, Tpc *pc) {
	gtk_widget_hide(pc->win);
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(pc->rad[0]))) {
		project_edit(pc->bfwin);
	} else {
		project_edit(NULL);
	}
	gtk_widget_destroy(pc->win);
}


void project_create_gui(Tbfwin *bfwin) {
	GtkWidget *vbox, *hbox, *but;
	Tpc *pc;
	
	pc = g_new(Tpc,1);
	pc->bfwin = bfwin;
	pc->win = window_full2(_("Create project"), GTK_WIN_POS_NONE, 5
			, G_CALLBACK(project_create_destroy_lcb), pc, TRUE, NULL);
	vbox = gtk_vbox_new(FALSE,0);
	gtk_container_add(GTK_CONTAINER(pc->win),vbox);
	pc->rad[0] = boxed_radiobut_with_value(_("Create project with currently opened documents"), TRUE, NULL, vbox);
	pc->rad[1] = boxed_radiobut_with_value(_("Create empty project"), FALSE, GTK_RADIO_BUTTON(pc->rad[0]), vbox);
	hbox = gtk_hbutton_box_new();
	gtk_hbutton_box_set_layout_default(GTK_BUTTONBOX_END);
	gtk_button_box_set_spacing(GTK_BUTTON_BOX(hbox), 6);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, TRUE, 0);
	but = bf_stock_cancel_button(G_CALLBACK(project_create_cancel_clicked_lcb), pc);
	gtk_box_pack_start(GTK_BOX(hbox), but, FALSE, TRUE, 0);
	but = bf_stock_ok_button(G_CALLBACK(project_create_ok_clicked_lcb), pc);
	gtk_box_pack_start(GTK_BOX(hbox), but, FALSE, TRUE, 0);
	gtk_widget_show_all(pc->win);
}

void project_menu_cb(Tbfwin *bfwin,guint callback_action, GtkWidget *widget) {
	DEBUG_MSG("project_menu_cb, bfwin=%p, callback_action=%d\n",bfwin,callback_action);
	switch (callback_action) {
	case 1:
		project_open(bfwin);
	break;
	case 2:
		project_save(bfwin, FALSE);
	break;
	case 3:
		project_save(bfwin, TRUE);
	break;
	case 4:
		project_save_and_close(bfwin);
	break;
	case 5:
		project_edit(bfwin);
	break;
	case 6:
		if (bfwin->project) {
			project_edit(NULL);
		} else {
			if (test_only_empty_doc_left(bfwin->documentlist)) {
				project_edit(bfwin);
			} else {
				project_create_gui(bfwin);
			}
		}
	break;
	default:
		DEBUG_MSG("project_menu_cb, no such callback_action %d\n",callback_action);
		exit(54);
	break;
	}
}

