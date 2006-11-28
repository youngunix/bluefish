/* Bluefish HTML Editor
 * snippets_wizard.c - plugin for snippets sidebar
 *
 * Copyright (C) 2006 Olivier Sessink
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

#include <string.h>

#include "snippets.h"
#include "snippets_load.h"
#include "snippets_leaf_insert.h"
#include "../gtk_easy.h"

typedef enum {
	page_type, /* 0 */
	page_branch, /* 1 */
	page_insert, /* 2 */
	page_finished
} Tpagenum;

typedef struct {
	GtkWidget *table;
	GtkWidget *entries[10];
	GtkWidget *before_v;
	GtkWidget *after_v;
	GtkTextBuffer *before;
	GtkTextBuffer *after;

} Tpage2;

static gpointer snippets_build_page2(Tsnippetswin *snw, GtkWidget *dialog_action) {
	GtkWidget *scrolwin, *label;
	gint i;
	gchar *tmpstr;
	Tpage2 *p2 = g_new(Tpage2,1);
	
	p2->table = gtk_table_new(12, 4, FALSE);
	gtk_table_set_row_spacings(GTK_TABLE (p2->table), 6);
	gtk_table_set_col_spacings(GTK_TABLE (p2->table), 12);
	gtk_box_pack_start(GTK_BOX(dialog_action), p2->table, TRUE, TRUE, 0);
	
	label = gtk_label_new(_("The <i>before</i> text will be inserted before the cursor position or the current selection, the <i>after</i> text will be inserted after the cursor position or the current selection. You may use %0, %1, ...%9 placeholders to ask for values when you activate this item. Give these placeholders an appropriate name on the right."));
	gtk_label_set_use_markup(GTK_LABEL(label),TRUE);
	gtk_label_set_line_wrap(GTK_LABEL(label),TRUE);
	gtk_table_attach(GTK_TABLE(p2->table),label, 0,3,0,1
					,GTK_FILL,GTK_FILL,0,0);
	
	label = gtk_label_new(_("<i>Before</i> text:"));
	gtk_label_set_use_markup(GTK_LABEL(label),TRUE);
	gtk_misc_set_alignment(GTK_MISC(label),0,0.5);
	gtk_table_attach(GTK_TABLE(p2->table),label, 0,1,1,2,GTK_FILL,GTK_FILL,0,0);
	scrolwin = textview_buffer_in_scrolwin(&p2->before_v, -1, -1, NULL, GTK_WRAP_NONE);
	gtk_table_attach(GTK_TABLE(p2->table), scrolwin, /*left*/0,/*right*/1,/*top*/2,/*bottom*/6
				,/*xoptions*/GTK_EXPAND|GTK_FILL,/*yoptions*/GTK_EXPAND|GTK_FILL
				,/*xpadding*/0,/*ypadding*/0);
	p2->before = gtk_text_view_get_buffer(GTK_TEXT_VIEW(p2->before_v));
	
	label = gtk_label_new(_("<i>After</i> text:"));
	gtk_label_set_use_markup(GTK_LABEL(label),TRUE);
	gtk_misc_set_alignment(GTK_MISC(label),0,0.5);
	gtk_table_attach(GTK_TABLE(p2->table),label, 0,1,6,7,GTK_FILL,GTK_FILL,0,0);
	scrolwin = textview_buffer_in_scrolwin(&p2->after_v, -1, -1, NULL, GTK_WRAP_NONE);
	gtk_table_attach(GTK_TABLE(p2->table), scrolwin, 0,1,7,11
				,GTK_EXPAND|GTK_FILL,GTK_EXPAND|GTK_FILL,0,0);
	p2->after = gtk_text_view_get_buffer(GTK_TEXT_VIEW(p2->after_v));
	
	for (i = 0; i <  10; i++) {
		tmpstr = g_strdup_printf("%%%d: ", i);
		label = gtk_label_new(tmpstr);
		gtk_misc_set_alignment(GTK_MISC(label),1,0.5);
		gtk_table_attach(GTK_TABLE(p2->table),label, 1,2,i+1,i+2
					,GTK_FILL,GTK_FILL,0,0);
		g_free(tmpstr);
		p2->entries[i] = gtk_entry_new();
		gtk_table_attach(GTK_TABLE(p2->table),p2->entries[i], 2,3,i+1,i+2
					,GTK_FILL,GTK_FILL,0,0);
	}
	gtk_widget_show_all(p2->table);
	return p2;
}

typedef struct {
	GtkWidget *entry;
	GtkWidget *vbox;
} Tpage1;

static gpointer snippets_build_page1(Tsnippetswin *snw, GtkWidget *dialog_action) {
	GtkWidget *label;
	Tpage1 *p1 = g_new(Tpage1,1);
	p1->vbox = gtk_vbox_new(TRUE,12);
	gtk_container_add(GTK_CONTAINER(dialog_action),p1->vbox);
	label = gtk_label_new(_("Enter the name of the branch:"));
	gtk_box_pack_start(GTK_BOX(p1->vbox),label,TRUE,TRUE,12);
	p1->entry = gtk_entry_new();
	gtk_box_pack_start(GTK_BOX(p1->vbox),p1->entry,TRUE,TRUE,12);
	
	gtk_widget_show_all(p1->vbox);
	return p1;
}

static void snippets_delete_page1(Tsnippetswin *snw, gpointer data) {
	Tpage1 *p1 = (Tpage1 *)data;
	gtk_widget_destroy(p1->vbox);
}

static gint snippets_test_page1(Tsnippetswin *snw, gpointer data) {
	Tpage1 *p1 = (Tpage1 *)data;
	gchar *name;
	GtkTreePath *parentp=NULL;
	xmlNodePtr parent=NULL, child;
	GtkTreeIter piter, citer;	
	/* lets build the branch! */
	name = gtk_editable_get_chars(GTK_EDITABLE(p1->entry),0,-1);
	if (snw->lastclickedpath) {
		parentp = gtk_tree_path_copy(snw->lastclickedpath);
	}
	if (snw->lastclickednode) {
		if (xmlStrEqual(snw->lastclickednode->name, (const xmlChar *)"leaf")) {
			DEBUG_MSG("clicked node was a leaf\n");
			parent = snw->lastclickednode->parent;
			if (parentp) {
				if (!gtk_tree_path_up(parentp)) {
					DEBUG_MSG("could not go up, set parentp NULL\n");
					gtk_tree_path_free(parentp);
					parentp = NULL;
				}
			}
		} else {
			parent = snw->lastclickednode;
			DEBUG_MSG("clicked node was a branch\n");
		}
	} 
	child = xmlNewChild(parent,NULL,(const xmlChar *)"branch",NULL);
	xmlSetProp(child, (const xmlChar *)"title", (const xmlChar *)name);
	/* add this branch to the treestore */

	if ((parentp && gtk_tree_model_get_iter(GTK_TREE_MODEL(snippets_v.store),&piter,parentp)) 
						|| gtk_tree_model_get_iter_first(GTK_TREE_MODEL(snippets_v.store),&piter)) {
		gtk_tree_store_append(snippets_v.store, &citer, &piter);
		gtk_tree_store_set(snippets_v.store, &citer, 0, name,1, child,-1);
	}
	DEBUG_MSG("add branch with title %s\n",name);
	g_free(name);
	snippets_store();
	return page_finished;
}


typedef struct {
	GtkWidget *radio[2];
	GtkWidget *vbox;
} Tpage0;

static gpointer snippets_build_page0(Tsnippetswin *snw, GtkWidget *dialog_action) {
	GtkWidget *label;
	Tpage0 *p0 = g_new(Tpage0,1);
	p0->vbox = gtk_vbox_new(FALSE,12);
	gtk_container_add(GTK_CONTAINER(dialog_action),p0->vbox);
	label = gtk_label_new(_("Select what you want to add"));
	gtk_box_pack_start(GTK_BOX(p0->vbox),label,TRUE,TRUE,12);
	p0->radio[0] = gtk_radio_button_new_with_label(NULL, _("Branch"));
	gtk_box_pack_start(GTK_BOX(p0->vbox),p0->radio[0],TRUE,TRUE,12);
	p0->radio[1] = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(p0->radio[0]),_("Insert string"));
	gtk_box_pack_start(GTK_BOX(p0->vbox),p0->radio[1],TRUE,TRUE,12);
	gtk_widget_show_all(p0->vbox);
	return p0;
}

static void snippets_delete_page0(Tsnippetswin *snw, gpointer data) {
	Tpage0 *p0 = (Tpage0 *)data;
	gtk_widget_destroy(p0->vbox);
}

static gint snippets_test_page0(Tsnippetswin *snw, gpointer data) {
	Tpage0 *p0 = (Tpage0 *)data;
	int i;
	for (i=0;i<2;i++) {
		if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p0->radio[i]))) {
			return i+1; /* branch =1, insert=2 */
		}
	}
	return page_type;
}

void snippets_new_item_dialog(Tsnippetswin *snw) {
	GtkWidget* dialog;
	gboolean cont=TRUE;
	gint response;
	gpointer pagestruct;
	Tpagenum pagenum = page_type, newpagenum = page_type; 
	
	dialog = gtk_dialog_new_with_buttons(_("New snippet"),GTK_WINDOW(snw->bfwin->main_window),
					GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
					GTK_STOCK_CANCEL,GTK_RESPONSE_REJECT,
					GTK_STOCK_GO_FORWARD,1,
					NULL);
	gtk_window_set_default_size(GTK_WINDOW(dialog),500,400);
	pagestruct = snippets_build_page0(snw,GTK_DIALOG(dialog)->vbox);
	gtk_widget_show_all(dialog);
	while (cont) {
		response = gtk_dialog_run(GTK_DIALOG(dialog));
		if (response == GTK_RESPONSE_REJECT) {
			cont = FALSE;
			break;
		} 
		switch (pagenum) { /* test the current results */
			case page_type:
				newpagenum = snippets_test_page0(snw,pagestruct);
				if (newpagenum != page_type) {
					snippets_delete_page0(snw,pagestruct);
				}
			break;
			case page_branch:
				newpagenum = snippets_test_page1(snw,pagestruct);
				if (newpagenum != page_type) {
					snippets_delete_page1(snw,pagestruct);
				}
			break;
			case page_insert:
			break;
		}
		if (pagenum != newpagenum) {
			switch (newpagenum) { /* build a new page */
				case page_type:
					pagestruct = snippets_build_page0(snw,GTK_DIALOG(dialog)->vbox);
				break;
				case page_branch:
					pagestruct = snippets_build_page1(snw,GTK_DIALOG(dialog)->vbox);
				break;
				case page_insert:
					pagestruct = snippets_build_page2(snw,GTK_DIALOG(dialog)->vbox);
				break;
				case page_finished:
					cont = FALSE;
				break;
			}
			pagenum = newpagenum;
		}
	}
	gtk_widget_destroy(dialog);
}
