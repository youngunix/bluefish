/* Bluefish HTML Editor
 * document.c - the document
 *
 * Copyright (C) 1998 Olivier Sessink and Chris Mazuc
 * Copyright (C) 1999-2002 Olivier Sessink
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
#include <gtk/gtk.h>
#include <sys/types.h> /* stat() */
#include <sys/stat.h> /* stat() */
#include <unistd.h> /* stat() */
#include <stdio.h> /* fopen() */
#include <string.h> /* strchr() */

#include "bluefish.h"
#include "document.h"
#include "highlight.h" /* all highlight functions */
#include "gui.h" /* statusbar_message() */
#include "bf_lib.h"
#include "stringlist.h" /* free_stringlist() */
#include "gtk_easy.h" /* error_dialog() */
#include "undo_redo.h" /* doc_unre_init() */

/* gint documentlist_return_index_from_filename(gchar *filename)
 * returns -1 if the file is not open, else returns the index where
 * the file is in the documentlist
 */
gint documentlist_return_index_from_filename(gchar *filename) {
	GList *tmplist;
	gint count=0;

	if (!filename) {
		return -1;
	}
	
	tmplist = g_list_first(main_v->documentlist);
	while (tmplist) {
		if (((Tdocument *)tmplist->data)->filename &&(strcmp(filename, ((Tdocument *)tmplist->data)->filename) ==0)) {
			return count;
		}
		count++;
		tmplist = g_list_next(tmplist);
	}
	return -1;
}
/* void document_set_wrap(Tdocument * document, gint wraptype)
 * type=0 = none, type=1=word wrap
 * type=-1 means get type from main_v->props.word_wrap
 */

void document_set_wrap(Tdocument * doc, gint wraptype) {
	gint type;

	if (wraptype == -1) {
		type = main_v->props.word_wrap;
	} else {
		type = wraptype;
	}
	if (type) {
		gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(doc->view),GTK_WRAP_WORD);
	} else {
		gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(doc->view),GTK_WRAP_NONE);
	}
}

void doc_set_font(Tdocument *doc, gchar *fontstring) {
	PangoFontDescription *font_desc;
	font_desc = pango_font_description_from_string("courier 11");
	gtk_widget_modify_font(doc->view, font_desc);
	pango_font_description_free(font_desc);
}


gboolean doc_is_empty_non_modified_and_nameless(Tdocument *doc) {
#ifdef DEBUG
	g_assert(doc);
#endif
	if (doc->modified || doc->filename) {
		return FALSE;
	}
	if (gtk_text_buffer_get_char_count(doc->buffer) > 0) {
		return FALSE;
	}
	return TRUE;
}


/* gboolean test_docs_modified(GList *doclist)
 * if doclist is NULL it will use main_v->documentlist as doclist
 * returns TRUE if there are any modified documents in doclist
 * returns FALSE if there are no modified documents in doclist
 */
gboolean test_docs_modified(GList *doclist) {

	GList *tmplist;
	Tdocument *tmpdoc;

	if (doclist) {
		tmplist = g_list_first(doclist);
	} else {
		tmplist = g_list_first(main_v->documentlist);
	}
	
	while (tmplist) {
		tmpdoc = (Tdocument *) tmplist->data;
#ifdef DEBUG
		g_assert(tmpdoc);
#endif
		if (tmpdoc->modified) {
			return TRUE;
		}
		tmplist = g_list_next(tmplist);
	}
	return FALSE;
}
/* gint test_only_empty_doc_left()
 * returns TRUE if there is only 1 document open, and that document
 * is not modified and 0 bytes long and without filename
 * returns FALSE if there are multiple documents open, or 
 * a modified document is open, or a > 0 bytes document is open
 * or a document with filename is open
 */
gboolean test_only_empty_doc_left() {
	if (g_list_length(main_v->documentlist) > 1) {
		return FALSE;
	} else {
		Tdocument *tmpdoc;
		GList *tmplist = g_list_first(main_v->documentlist);
		if (tmplist) {
#ifdef DEBUG
			g_assert(tmplist->data);
#endif
			tmpdoc = tmplist->data;
			if (!doc_is_empty_non_modified_and_nameless(tmpdoc)) {
				return FALSE;
			}
		}
	}
	return TRUE;
}

static void doc_set_undo_redo_widget_state(Tdocument *doc) {
		gint redo, undo;
/*		redo = doc_has_redo_list(doc);
		undo = doc_has_undo_list(doc);
		if (main_v->props.v_main_tb) {
			gtk_widget_set_sensitive(main_v->toolb.redo, redo);
			gtk_widget_set_sensitive(main_v->toolb.undo, undo);
		}
		gtk_widget_set_sensitive(gtk_item_factory_get_widget(gtk_item_factory_from_widget(main_v->menubar), N_("/Edit/Undo")), undo);
		gtk_widget_set_sensitive(gtk_item_factory_get_widget(gtk_item_factory_from_widget(main_v->menubar), N_("/Edit/Undo all")), undo);
		gtk_widget_set_sensitive(gtk_item_factory_get_widget(gtk_item_factory_from_widget(main_v->menubar), N_("/Edit/Redo")), redo);
		gtk_widget_set_sensitive(gtk_item_factory_get_widget(gtk_item_factory_from_widget(main_v->menubar), N_("/Edit/Redo all")), redo);*/
}

void doc_set_modified(Tdocument *doc, gint value) {
	if (doc->modified != value) {
		gchar *label_string;
		doc->modified = value;
		if (doc->modified) {
			if (doc->filename) {
				gchar *tmpstr = g_path_get_basename(doc->filename);
				label_string = g_strconcat(tmpstr, " *", NULL);
				g_free(tmpstr);
			} else {
				label_string = g_strdup(_("Untitled *"));
			}
		} else {
			if (doc->filename) {
				gchar *tmpstr = g_path_get_basename(doc->filename);
				label_string = g_strdup(tmpstr);
				g_free(tmpstr);
			} else {
				label_string = g_strdup(_("Untitled"));
			}
		}
		
		
		if (doc->filename) {
			if (doc->modified) {
				gchar *tmpstr = g_path_get_basename(doc->filename);
				label_string = g_strconcat(tmpstr, " *", NULL);
				g_free(tmpstr);
			} else {
				gchar *tmpstr = g_path_get_basename(doc->filename);
				label_string = g_strdup(tmpstr);
				g_free(tmpstr);
			}
			gtk_label_set(GTK_LABEL(doc->tab_menu),doc->filename);
		} else {
			if (doc->modified) {
				label_string = g_strdup(_("Untitled *"));
			} else {
				label_string = g_strdup(_("Untitled"));
			}
			gtk_label_set(GTK_LABEL(doc->tab_menu),label_string);
		}
		gtk_label_set(GTK_LABEL(doc->tab_label),label_string);
		g_free(label_string);
	}
	/* only when this is the current document we have to change these */
	if (doc == main_v->current_document) {
		doc_set_undo_redo_widget_state(doc);
	}
}

inline static void doc_update_mtime(Tdocument *doc) {
	if (doc->filename) {
		struct stat statbuf;
		if (stat(doc->filename, &statbuf) == 0) {
			doc->mtime = statbuf.st_mtime;
		}
	} else {
		doc->mtime = 0;
	}
}
/* returns 1 if the time didn't change, returns 0 
if the file is modified by another process, returns
1 if there was no previous mtime information available */
static gint doc_check_mtime(Tdocument *doc) {
	if (doc->filename && 0 != doc->mtime) {
		struct stat statbuf;
		if (stat(doc->filename, &statbuf) == 0) {
			if (doc->mtime < statbuf.st_mtime) {
				DEBUG_MSG("doc_check_mtime, doc->mtime=%d, statbuf.st_mtime=%d\n", (int)doc->mtime, (int)statbuf.st_mtime);
				return 0;
			}
		}
	} 
	return 1;
}

/* doc_set_stat_info() includes setting the mtime field, so there is no need
to call doc_update_mtime() as well */
static void doc_set_stat_info(Tdocument *doc) {
	if (doc->filename) {
		struct stat statbuf;
		if (lstat(doc->filename, &statbuf) == 0) {
			if (S_ISLNK(statbuf.st_mode)) {
				doc->is_symlink = 1;
				stat(doc->filename, &statbuf);
			} else {
				doc->is_symlink = 0;
			}
			doc->mtime = statbuf.st_mtime;
			doc->owner_uid = statbuf.st_uid;
			doc->owner_gid = statbuf.st_gid;
			doc->mode = statbuf.st_mode;
		}
	}
}

static void doc_scroll_to_line(Tdocument *doc, gint linenum, gboolean select_line) {
	GtkTextIter itstart;

	gtk_text_buffer_get_iter_at_line(doc->buffer,&itstart,linenum);
	gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(doc->view),&itstart,0.25,FALSE,0.5,0.5);
	if (select_line) {
		GtkTextIter itend = itstart;
		gtk_text_iter_forward_to_line_end(&itend);
		gtk_text_buffer_place_cursor (doc->buffer, &itstart);
		gtk_text_buffer_move_mark_by_name(doc->buffer,"selection_bound",&itend);
	}
}

/* gchar *doc_get_chars(Tdocument *doc, gint startpos, gint len)
 * returns len characters from startpos
 * if len == -1 it will return everrything to the end
 */
gchar *doc_get_chars(Tdocument *doc, gint start, gint len) {
	GtkTextIter itstart, itend;
	gchar *string;

	gtk_text_buffer_get_iter_at_offset(doc->buffer, &itstart,start);
	if (len > 0) {
		gtk_text_buffer_get_iter_at_offset(doc->buffer, &itend, start + len);
	} else if (len == -1) {
		gtk_text_buffer_get_end_iter(doc->buffer, &itend);
	} else {
		return NULL;
	}
	string = gtk_text_buffer_get_text(doc->buffer, &itstart, &itend,FALSE);
	return string;
}

/*  void doc_select_region(Tdocument *doc, gint start, gint end, gboolean do_scroll)
 *  selects from start to end in the doc, and if do_scroll is set it will make
 *  sure the selection is visible to the user
 */
void doc_select_region(Tdocument *doc, gint start, gint end, gboolean do_scroll) {
	/* how do we select a region in gtk2?? */
	GtkTextIter itstart, itend;
	gtk_text_buffer_get_iter_at_offset(doc->buffer, &itstart,start);
	gtk_text_buffer_get_iter_at_offset(doc->buffer, &itend,end);
	gtk_text_buffer_move_mark_by_name(doc->buffer, "insert", &itstart);
	gtk_text_buffer_move_mark_by_name(doc->buffer, "selection_bound", &itend);
	gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(doc->view),&itstart,0.25,FALSE,0.5,0.5);
}

/*  gboolean doc_get_selection(Tdocument *, gint *start, gint *end)
 *  returns FALSE if there is no selection
 *  returns TRUE if there is a selection, and start and end will be set
 *  to the current selection
 */
gboolean doc_get_selection(Tdocument *doc, gint *start, gint *end) {
	GtkTextIter itstart, itend;
	GtkTextMark *mark = gtk_text_buffer_get_insert(doc->buffer);
	gtk_text_buffer_get_iter_at_mark(doc->buffer,&itstart,mark);
	mark = gtk_text_buffer_get_selection_bound(doc->buffer);
	gtk_text_buffer_get_iter_at_mark(doc->buffer,&itend,mark);
	*start = gtk_text_iter_get_offset(&itstart);
	*end = gtk_text_iter_get_offset(&itend);
	if (*start == *end) {
		return FALSE;
	}
	return TRUE;
}

gint doc_get_cursor_position(Tdocument *doc) {
	GtkTextIter iter;
	GtkTextMark *mark = gtk_text_buffer_get_insert(doc->buffer);
	gtk_text_buffer_get_iter_at_mark(doc->buffer, &iter, mark);
	return gtk_text_iter_get_offset(&iter);
}

void doc_replace_text_backend(Tdocument *doc, const gchar * newstring, gint start, gint end) {
	doc_unbind_signals(doc);
	
	/* delete region, and add that to undo/redo list */
	{
		gchar *buf;
		GtkTextIter itstart, itend;
		gtk_text_buffer_get_iter_at_offset(doc->buffer, &itstart,start);
		gtk_text_buffer_get_iter_at_offset(doc->buffer, &itend,end);
		buf = gtk_text_buffer_get_text(doc->buffer, &itstart, &itend,FALSE);
		gtk_text_buffer_delete(doc->buffer,&itstart,&itend);
		DEBUG_MSG("doc_replace_text_backend, calling doc_unre_add for buf=%s, start=%d and end=%d\n", buf, start, end);
		doc_unre_add(doc, buf, start, end, UndoDelete);
		g_free(buf);
		DEBUG_MSG("doc_replace_text_backend, text deleted from %d to %d\n", start, end);
	}

	/* add new text to this region, the buffer is changed so re-calculate itstart */
	{
		GtkTextIter itstart;
		gtk_text_buffer_get_iter_at_offset(doc->buffer, &itstart,start);
		gtk_text_buffer_insert(doc->buffer,&itstart,newstring,-1);
	}
	doc_unre_add(doc, newstring, start, start + strlen(newstring), UndoInsert);
	doc_bind_signals(doc);
	doc_set_modified(doc, 1);
}					  

void doc_replace_text(Tdocument * doc, const gchar * newstring, gint start, gint end) {
	doc_unre_new_group(doc);

	doc_replace_text_backend(doc, newstring, start, end);
	
	doc_set_modified(doc, 1);
	doc_unre_new_group(doc);
/*	doc_need_highlighting(doc);*/
}


#define STARTING_BUFFER_SIZE 2048
gboolean doc_file_to_textbox(Tdocument * doc, gchar * filename, gboolean enable_undo)
{
	FILE *fd;
	gchar *errmessage, line[STARTING_BUFFER_SIZE], *message;

	if (!enable_undo) {
		doc_unbind_signals(doc);
	}
	message = g_strconcat(_("Opening file "), filename, NULL);
	statusbar_message(message, 1000);
	g_free(message);
	/* This opens the contents of a file to a textbox */
	change_dir(filename);
	fd = fopen(filename, "r");
	if (fd == NULL) {
		DEBUG_MSG("file_to_textbox, cannot open file %s\n", filename);
		errmessage =
			g_strconcat(_("Could not open file:\n"), filename, NULL);
		error_dialog(_("Error"), errmessage);	/* 7 */
		g_free(errmessage);
		if (!enable_undo) {
			doc_bind_signals(doc);
		}
		return FALSE;
	}

	while (fgets(line, STARTING_BUFFER_SIZE, fd) != NULL) {
		gtk_text_buffer_insert_at_cursor(doc->buffer,line,-1);
	}
	fclose(fd);
	doc->need_highlighting=TRUE;
	if (!enable_undo) {
		doc_bind_signals(doc);
	}
	return TRUE;
}

/* static gint doc_check_backup(Tdocument *doc)
 * returns 1 on success, 0 on failure
 * if no backup is required, or no filename known, 1 is returned
 */
static gint doc_check_backup(Tdocument *doc) {
	gint res = 1;

	if (main_v->props.backup_file && doc->filename && file_exists_and_readable(doc->filename)) {
		gchar *backupfilename;
		backupfilename = g_strconcat(doc->filename, main_v->props.backup_filestring, NULL);
		res = file_copy(doc->filename, backupfilename);
		if (doc->owner_uid != -1 && !doc->is_symlink) {
			chmod(backupfilename, doc->mode);
			chown(backupfilename, doc->owner_uid, doc->owner_gid);
		}
		g_free(backupfilename);
	}
	return res;
}

static void doc_buffer_insert_text_lcb(GtkTextBuffer *textbuffer,GtkTextIter * iter,gchar * string,gint len, Tdocument * doc) {
	gint i = 0;
	doc_set_modified(doc, 1);
	
	/* highlighting stuff */
	if (string && doc->hl->update_chars) {
		while (string[i] != '\0') {
			if (strchr(doc->hl->update_chars, string[i])) {
				doc_highlight_line(doc);
				break;
			}
			i++;
		}
	}
	
	/* undo_redo stuff */
	if ((len == 1 && (string[0] == ' ' || string[0] == '\n' || string[0] == '\t')) || !doc_undo_op_compare(doc,UndoInsert)) {
		DEBUG_MSG("doc_buffer_insert_text_lcb, need a new undogroup\n");
		doc_unre_new_group(doc);
	}
	{
		gint pos = gtk_text_iter_get_offset(iter);
		doc_unre_add(doc, string, pos, pos+len, UndoInsert);
	}

}

static void doc_buffer_delete_range_lcb(GtkTextBuffer *textbuffer,GtkTextIter * itstart,GtkTextIter * itend, Tdocument * doc) {
	gchar *string;
	gint i=0;
	doc_set_modified(doc, 1);
	string = gtk_text_buffer_get_text(doc->buffer, itstart, itend, FALSE);
	if (string) {
		/* highlighting stuff */
		if (doc->hl->update_chars) {
			while (string[i] != '\0') {
				if (strchr(doc->hl->update_chars, string[i])) {
					doc_highlight_line(doc);
					break;
				}
				i++;
			}
		}

		/* undo_redo stuff */
		{
			gint start, end, len;
			start = gtk_text_iter_get_offset(itstart);
			end = gtk_text_iter_get_offset(itend);	
			len = end - start;
			if ((len == 1 && (string[0] == ' ' || string[0] == '\n' || string[0] == '\t')) || !doc_undo_op_compare(doc,UndoDelete)) {
				DEBUG_MSG("doc_buffer_delete_range_lcb, need a new undogroup\n");
				doc_unre_new_group(doc);
			}
			doc_unre_add(doc, string, start, end, UndoDelete);
		}

		g_free(string);
	}
}

void doc_bind_signals(Tdocument *doc) {
	doc->ins_txt_id = g_signal_connect(G_OBJECT(doc->buffer),
					 "insert-text",
					 G_CALLBACK(doc_buffer_insert_text_lcb), doc);
	doc->del_txt_id = g_signal_connect(G_OBJECT(doc->buffer),
					 "delete-range",
					 G_CALLBACK(doc_buffer_delete_range_lcb), doc);
}

void doc_unbind_signals(Tdocument *doc) {
/*	g_print("doc_unbind_signals, before unbind ins=%lu, del=%lu\n", doc->ins_txt_id, doc->del_txt_id);*/
	if (doc->ins_txt_id != 0) {
		g_signal_handler_disconnect(G_OBJECT(doc->buffer),doc->ins_txt_id);
		doc->ins_txt_id = 0;
	}
	if (doc->del_txt_id != 0) {
		g_signal_handler_disconnect(G_OBJECT(doc->buffer),doc->del_txt_id);
		doc->del_txt_id = 0;
	}
}

static void doc_close_but_clicked_lcb(GtkWidget *wid, gpointer data) {
	doc_close(data, 0);
}


Tdocument *doc_new() {
	GtkWidget *scroll;
	Tdocument *newdoc = g_new0(Tdocument, 1);
	DEBUG_MSG("doc_new, main_v is at %p, newdoc at %p\n", main_v, newdoc);

	newdoc->hl = hl_get_highlightset_by_filename(NULL);
	newdoc->buffer = gtk_text_buffer_new(main_v->tagtable);
	newdoc->view = gtk_text_view_new_with_buffer(newdoc->buffer);
	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
									   GTK_POLICY_AUTOMATIC,
									   GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW
											(scroll), GTK_SHADOW_IN);
	gtk_container_add(GTK_CONTAINER(scroll), newdoc->view);

	newdoc->tab_label = gtk_label_new(NULL);
	GTK_WIDGET_UNSET_FLAGS(newdoc->tab_label, GTK_CAN_FOCUS);
	newdoc->tab_menu = gtk_label_new(NULL);

	doc_unre_init(newdoc);
	doc_set_font(newdoc, NULL);
	document_set_wrap(newdoc, -1);

/* this will force function doc_set_modified to update the tab label*/
	newdoc->modified = 1;
	doc_set_modified(newdoc, 0);
	newdoc->filename = NULL;
	newdoc->need_highlighting = 0;
	newdoc->mtime = 0;
	newdoc->owner_uid = -1;
	newdoc->owner_gid = -1;
	newdoc->is_symlink = 0;
	doc_bind_signals(newdoc);
/*	newdoc->highlightstate = main_v->props.defaulthighlight;*/

	main_v->documentlist = g_list_append(main_v->documentlist, newdoc);

	gtk_widget_show(newdoc->view);
	gtk_widget_show(newdoc->tab_label);
	gtk_widget_show(scroll);

	DEBUG_MSG("doc_new, appending doc to notebook\n");
	{
		GtkWidget *hbox, *but;
		hbox = gtk_hbox_new(FALSE,0);
		but = gtk_button_new_with_label("x");
		gtk_button_set_relief(GTK_BUTTON(but), GTK_RELIEF_NONE);
		g_signal_connect(G_OBJECT(but), "clicked", G_CALLBACK(doc_close_but_clicked_lcb), newdoc);
		gtk_box_pack_start(GTK_BOX(hbox), newdoc->tab_label, FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(hbox), but, FALSE, FALSE, 0);
		gtk_widget_show(hbox);
		gtk_widget_show(but);
		gtk_notebook_append_page_menu(GTK_NOTEBOOK(main_v->notebook), scroll ,hbox, newdoc->tab_menu);
	}
	DEBUG_MSG("doc_new, set notebook page to %d\n", g_list_length(main_v->documentlist) - 1);
	gtk_notebook_set_page(GTK_NOTEBOOK(main_v->notebook),g_list_length(main_v->documentlist) - 1);

	if (main_v->current_document == NULL) {
		notebook_changed(-1);
	}

	gtk_widget_grab_focus(newdoc->view);	
	return newdoc;
}

/*
 * gint doc_textbox_to_file(Tdocument * doc, gchar * filename)
 * returns 1 on success
 * returns 2 on success but the backup failed
 * returns -1 if the backup failed and save was aborted
 * returns -2 if the file pointer could not be opened
 * returns -3 if the backup failed and save was aborted by the user
 */

gint doc_textbox_to_file(Tdocument * doc, gchar * filename) {
	FILE *fd;
	gchar *tmpchar;
	gint backup_retval;

	statusbar_message(_("Saving file"), 1000);

	/* This writes the contents of a textbox to a file */
	backup_retval = doc_check_backup(doc);

	if (!backup_retval) {
		if (strcmp(main_v->props.backup_abort_style, "abort")==0) {
			DEBUG_MSG("doc_textbox_to_file, backup failure, abort!\n");
			return -1;
		} else if (strcmp(main_v->props.backup_abort_style, "ask")==0) {
			gchar *options[] = {N_("Abort save"), N_("Continue save"), NULL};
			gint retval;
			gchar *tmpstr =  g_strdup_printf(_("Backup %s failed"), filename);
			retval = multi_button_dialog(_("Bluefish warning, file backup failure"), 1, tmpstr, options);
			g_free(tmpstr);
			if (retval == 0) {
				DEBUG_MSG("doc_textbox_to_file, backup failure, user aborted!\n");
				return -3;
			}
		}
	}
	change_dir(filename);
	fd = fopen(filename, "w");
	if (fd == NULL) {
		DEBUG_MSG("textbox_to_file, cannot open file %s\n", filename);
		return -2;
	} else {
		GtkTextIter itstart, itend;
		gtk_text_buffer_get_bounds(doc->buffer,&itstart,&itend);
		tmpchar = gtk_text_buffer_get_text(doc->buffer,&itstart,&itend,FALSE);
		fputs(tmpchar, fd);
		g_free(tmpchar);
		fclose(fd);

		doc_set_modified(doc, 0);
		if (main_v->props.clear_undo_on_save) {
			doc_unre_clear_all(doc);
		}
		if (!backup_retval) {
			return 2;
		} else {
			return 1;
		}
	}
}

void doc_destroy(Tdocument * doc)
{
	if (doc->filename) {
/*		add_to_recent_list(doc->filename, 1);*/
	}

	gtk_notebook_remove_page(GTK_NOTEBOOK(main_v->notebook),
							 gtk_notebook_page_num(GTK_NOTEBOOK(main_v->notebook),doc->view->parent));

	if (g_list_length(main_v->documentlist) > 1) {
		main_v->documentlist = g_list_remove(main_v->documentlist, doc);
	} else {
		main_v->documentlist = g_list_remove(main_v->documentlist, doc);
		DEBUG_MSG("doc_destroy, last document removed from documentlist\n");
		g_list_free(main_v->documentlist);
		DEBUG_MSG("doc_destroy, freed documentlist\n");
		main_v->documentlist = NULL;
		DEBUG_MSG("doc_destroy, documentlist = NULL\n");
	}
	DEBUG_MSG("doc_destroy, g_list_length(documentlist)=%d\n",g_list_length(main_v->documentlist));
	if (doc->filename) {
		g_free(doc->filename);
	}
	DEBUG_MSG("doc_destroy, this is kind of tricky, I don't know if the Buffer is also detroyed when you destroy the view\n");
	g_object_unref(doc->buffer);
	DEBUG_MSG("doc_destroy, if this line shows up well I guess we needed to unref the buffer\n");
	
	doc_unre_destroy(doc);
	g_free(doc);

/*	notebook_changed();*/
}

/* gint doc_save(Tdocument * doc, gint do_save_as, gint do_move)
 * returns 1 on success
 * returns 2 on success but the backup failed
 * returns 3 on user abort
 * returns -1 if the backup failed and save was aborted
 * returns -2 if the file pointer could not be opened 
 * returns -3 if the backup failed and save was aborted by the user
 * returns -4 if there is no filename, after asking one from the user
 * returns -5 if another process modified the file, and the user chose cancel
 */

gint doc_save(Tdocument * doc, gint do_save_as, gint do_move)
{
	gchar *oldfilename = NULL;
	gint retval;
#ifdef DEBUG
	g_assert(doc);
#endif

	DEBUG_MSG("doc_save, doc=%p, save_as=%d, do_move=%d\n", doc, do_save_as, do_move);
	if (doc->filename == NULL) {
		do_save_as = 1;
	}
	if (do_move) {
		do_save_as = 1;
	}

	if (do_save_as) {
		gchar *newfilename = NULL;
		gint index;
		statusbar_message(_("Save as..."), 1);
		oldfilename = doc->filename;
		doc->filename = NULL;
		newfilename = return_file_w_title(NULL, _("Save document as"));
		index = documentlist_return_index_from_filename(newfilename);
		DEBUG_MSG("doc_save, index=%d, filename=%p\n", index, newfilename);

		if (index != -1) {
			gchar *tmpstr;
			gint retval;
			gchar *options[] = {N_("Overwrite"), N_("Cancel"), NULL};
			tmpstr = g_strdup_printf(_("File %s is open, overwrite?"), newfilename);
			retval = multi_button_dialog(_("Bluefish: Warning, file is open"), 1, tmpstr, options);
			g_free(tmpstr);
			if (retval == 1) {
				g_free(newfilename);
				doc->filename = oldfilename;
				return 3;
			} else {
				Tdocument *tmpdoc;
				tmpdoc = (Tdocument *)g_list_nth_data(main_v->documentlist, index);
				DEBUG_MSG("doc_save, tmpdoc=%p\n", tmpdoc);
#ifdef DEBUG
				g_assert(tmpdoc);
				g_assert(tmpdoc->filename);
#endif
				g_free(tmpdoc->filename);
				tmpdoc->filename = NULL;
				doc_set_modified(tmpdoc, 1);
				{
					gchar *tmpstr2 = g_path_get_basename (newfilename);
					tmpstr = g_strconcat(_("Previously: "), tmpstr2, NULL);
					g_free(tmpstr2);
				}
				gtk_label_set(GTK_LABEL(tmpdoc->tab_label),tmpstr);
				g_free(tmpstr);
			}
		}
		doc->filename = newfilename;
		doc->modified = 1;
	}

	if (doc->filename == NULL) {
		doc->filename = oldfilename;
		return -4;
	}
	
	if (doc_check_mtime(doc) == 0) {
		gchar *tmpstr;
		gint retval;
		gchar *options[] = {N_("Overwrite"), N_("Cancel"), NULL};

		tmpstr = g_strdup_printf(_("File %s\nis modified by another process, overwrite?"), doc->filename);
		retval = multi_button_dialog(_("Bluefish: Warning, file is modified"), 0, tmpstr, options);
		g_free(tmpstr);
		if (retval == 1) {
			if (oldfilename) {
				g_free(oldfilename);
			}
			return -5;
		}
	}
	
	DEBUG_MSG("doc_save, returned file %s\n", doc->filename);
	if (do_save_as && oldfilename && main_v->props.link_management) {
/*		update_filenames_in_file(doc, oldfilename, doc->filename, 1);*/
	}
	statusbar_message(_("Save in progress"), 1);
	retval = doc_textbox_to_file(doc, doc->filename);

	switch (retval) {
		gchar *errmessage;
		case -1:
			/* backup failed and aborted */
			errmessage = g_strconcat(_("File save aborted, could not backup file:\n"), doc->filename, NULL);
			error_dialog(_("Error"), errmessage);
			g_free(errmessage);
		break;
		case -2:
			/* could not open the file pointer */
			errmessage = g_strconcat(_("File save error, could not write file:\n"), doc->filename, NULL);
			error_dialog(_("Error"), errmessage);
			g_free(errmessage);
		break;
		default:
			doc_update_mtime(doc);
			DEBUG_MSG("doc_save, received return value %d from doc_textbox_to_file\n", retval);
		break;
	}
	if (oldfilename) {
		hl_reset_highlighting_type(doc, doc->filename);
/*		populate_dir_file_list();*/
		if (do_move && (retval > 0)) {
			if (main_v->props.link_management) {
/*				all_documents_update_links(doc, oldfilename,
							   doc->filename);*/
			}
			unlink(oldfilename);
		}

		g_free(oldfilename);
	}
	return retval;
}

/*
returning 0 --> cancel or abort
returning 1 --> ok, closed or saved & closed
*/
gint doc_close(Tdocument * doc, gint warn_only)
{
	gchar *text;
	gint retval;

	if (!doc) {
		return 0;
	}

	if (doc_is_empty_non_modified_and_nameless(doc)) {
		/* no need to close this doc, it's an Untitled empty document */
		return 0;
	}

	if (doc->modified) {
		if (doc->filename) {
			text =
				g_strdup_printf(_("Are you sure you want to close\n%s ?"),
								doc->filename);
		} else {
			text =
				g_strdup(_
						 ("Are you sure you want to close\nthis untitled file ?"));
		}
	
		{
			gchar *buttons[] = {N_("Save"), N_("Close"), N_("Cancel"), NULL};
			retval = multi_button_dialog(_("Bluefish warning: file is modified!"), 2, text, buttons);
		}
		g_free(text);

		switch (retval) {
		case 2:
			DEBUG_MSG("doc_close, retval=2 (cancel) , returning\n");
			return 2;
			break;
		case 0:
			doc_save(doc, 0, 0);
			if (doc->modified == 1) {
				/* something went wrong it's still not saved */
				return 0;
			}
			if (!warn_only) {
				doc_destroy(doc);
			}
			break;
		case 1:
			if (!warn_only) {
				doc_destroy(doc);
			}
			break;
		default:
			return 0;			/* something went wrong */
			break;
		}
	} else {
		DEBUG_MSG("doc_close, closing doc=%p\n", doc);
		if (!warn_only) {
			doc_destroy(doc);
		}
	}

/*	notebook_changed();*/
	return 1;
}


void doc_new_with_file(gchar * filename) {

	Tdocument *doc;
	
	if ((filename == NULL) || (!file_exists_and_readable(filename))) {
        error_dialog("Error",filename);
		statusbar_message(_("Unable to open file"), 2000);
		return;
	}
	if (!main_v->props.allow_multi_instances) {
		gboolean res;
		res = switch_to_document_by_filename(filename);
		if (res){
			return;
		}
	}
	DEBUG_MSG("doc_new_with_file, filename=%s exists\n", filename);
/*	file_and_dir_history_add(filename);*/
	doc = doc_new();
	doc->filename = g_strdup(filename);
	hl_reset_highlighting_type(doc, doc->filename);	
	doc_file_to_textbox(doc, doc->filename, FALSE);
	doc->modified = 1; /* force doc_set_modified() to update the tab-label */
	doc_set_modified(doc, 0);
	doc_set_stat_info(doc); /* also sets mtime field */
/*	notebook_changed();*/
}

void docs_new_from_files(GList * file_list) {

	GList *tmplist;

	tmplist = g_list_first(file_list);
	while (tmplist) {
		doc_new_with_file((gchar *) tmplist->data);
		tmplist = g_list_next(tmplist);
	}
}

void doc_reload(Tdocument *doc) {
	if ((doc->filename == NULL) || (!file_exists_and_readable(doc->filename))) {
		statusbar_message(_("Unable to open file"), 2000);
		return;
	}
	{
		GtkTextIter itstart, itend;
		gtk_text_buffer_get_bounds(doc->buffer,&itstart,&itend);
		gtk_text_buffer_delete(doc->buffer,&itstart,&itend);
	}
	
	doc_file_to_textbox(doc, doc->filename, FALSE);
	doc_set_modified(doc, 0);
	doc_set_stat_info(doc); /* also sets mtime field */
}

void doc_activate(Tdocument *doc) {
	if (doc_check_mtime(doc) == 0) {
		gchar *tmpstr;
		gint retval;
		gchar *options[] = {N_("Reload"), N_("Ignore"), NULL};

		tmpstr = g_strdup_printf(_("File %s\nis modified by another process"), doc->filename);
		retval = multi_button_dialog(_("Bluefish: Warning, file is modified"), 0, tmpstr, options);
		g_free(tmpstr);
		if (retval == 1) {
			doc_set_stat_info(doc);
		} else {
			doc_reload(doc);
		}
	}
	doc_set_undo_redo_widget_state(doc);

	/* if highlighting is needed for this document do this now !! */
	if (TRUE || doc->need_highlighting && doc->highlightstate) {
		doc_highlight_full(doc);
	}

	gtk_widget_grab_focus(GTK_WIDGET(doc->view));

}


/**************************************************************************/
/* the start of the callback functions for the menu, acting on a document */
/**************************************************************************/

void file_save_cb(GtkWidget * widget, gpointer data) {
	doc_save(main_v->current_document, 0, 0);
}


void file_save_as_cb(GtkWidget * widget, gpointer data) {
	doc_save(main_v->current_document, 1, 0);
}

void file_move_to_cb(GtkWidget * widget, gpointer data) {
	doc_save(main_v->current_document, 1, 1);
}

void file_open_cb(GtkWidget * widget, gpointer data) {
	GList *tmplist;
	if (GPOINTER_TO_INT(data) == 1) {
/*		tmplist = return_files_advanced();*/
		tmplist = NULL;
	} else {
		tmplist = return_files(NULL);
	}
	if (!tmplist) {
		return;
	}
	docs_new_from_files(tmplist);
	free_stringlist(tmplist);
}

void file_revert_to_saved_cb(GtkWidget * widget, gpointer data) {
	doc_reload(main_v->current_document);
}

void file_insert_cb(GtkWidget * widget, gpointer data) {
	gchar *tmpfilename;

	tmpfilename = return_file(NULL);
	if (tmpfilename == NULL) {
		error_dialog(_("Bluefish error"), _("No filename known"));
		return;
	} else {
		/* do we need to set the insert point in some way ?? */
		doc_file_to_textbox(main_v->current_document, tmpfilename, TRUE);
		g_free(tmpfilename);
		doc_set_modified(main_v->current_document, 1);
	}
}

void file_new_cb(GtkWidget * widget, gpointer data) {
	Tdocument *doc;

	doc = doc_new();
/*	project management needs a rewite so this is not included yet */
/* 	if ((main_v->current_project.template) && (file_exists_and_readable(main_v->current_project.template) == 1)) {
             doc_file_to_textbox(doc, main_v->current_project.template);
 	}*/
}

void file_close_cb(GtkWidget * widget, gpointer data) {
	doc_close(main_v->current_document, 0);
}

void file_close_all_cb(GtkWidget * widget, gpointer data)
{
	GList *tmplist;
	Tdocument *tmpdoc;
	gint retval = -1;

	DEBUG_MSG("file_close_all_cb, started\n");

	/* first a warning loop */
	if (test_docs_modified(NULL)) {
		gchar *options[] = {N_("save all"), N_("close all"), N_("choose per file"), N_("cancel"), NULL};
		retval =	multi_button_dialog(_("Bluefish: Warning, some file(s) are modified!"), 3, _("Some file(s) are modified\nplease choose your action"), options);
		if (retval == 3) {
			DEBUG_MSG("file_close_all_cb, cancel clicked, returning 0\n");
			return;
		}
	} else {
		retval = 1;
	}
	DEBUG_MSG("file_close_all_cb, after the warnings, retval=%d, now close all the windows\n", retval);

	tmplist = g_list_first(main_v->documentlist);
	while (tmplist) {
		tmpdoc = (Tdocument *) tmplist->data;
		if (test_only_empty_doc_left()) {
			return;
		}
		
		switch (retval) {
		case 0:
			doc_save(tmpdoc, 0, 0);
			if (!tmpdoc->modified) {
				doc_destroy(tmpdoc);
			} else {
				return;
			}
			tmplist = g_list_first(main_v->documentlist);
		break;
		case 1:
			doc_destroy(tmpdoc);
			tmplist = g_list_first(main_v->documentlist);
		break;
		case 2:
			if (doc_close(tmpdoc, 0)) {
				tmplist = g_list_first(main_v->documentlist);
			} else {
/*				notebook_changed();*/
				return;
			}
		break;
		default:
/*			notebook_changed();*/
			return;
		break;
		}
	}
	notebook_changed(-1);
	DEBUG_MSG("file_close_all_cb, finished\n");
}


/*
 * Function: file_save_all_cb
 * Arguments:
 * 	widget	- callback widget
 * 	data	- data for callback function
 * Return value:
 * 	void
 * Description:
 * 	Save all editor notebooks
 */
void file_save_all_cb(GtkWidget * widget, gpointer data)
{

	GList *tmplist;
	Tdocument *tmpdoc;

	tmplist = g_list_first(main_v->documentlist);
	while (tmplist) {
		tmpdoc = (Tdocument *) tmplist->data;
		if (tmpdoc->modified) {
			doc_save(tmpdoc, 0, 0);
		}
		tmplist = g_list_next(tmplist);
	}
}



