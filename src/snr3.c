/*#define DEBUG*/
/*#define SNR3_PROFILING*/

#define _GNU_SOURCE

#include <gtk/gtk.h>
#include <string.h>
#include "config.h"
#include "bluefish.h"
#include "document.h"
#include "bf_lib.h"
#include "dialog_utils.h"
#include "gtk_easy.h"

#include "snr3.h"

#ifdef SNR3_PROFILING

typedef struct {
	GTimer *timer;
	gint numresults;
} Tsnr3profiling;

Tsnr3profiling s3profiling= {NULL};

#endif


static void
move_window_away_from_cursor(Tdocument * doc, GtkWindow * win, GtkTextIter * iter)
{
	GdkRectangle winrect;
	GdkRectangle itrect;

	/* get window coordinates, try to include the decorations */
	gdk_window_get_frame_extents(gtk_widget_get_window(GTK_WIDGET(win)), &winrect);

	doc_get_iter_location(doc, iter, &itrect);
	DEBUG_MSG("move_window_away_from_cursor, itx=%d-%d,ity=%d-%d, winx=%d-%d, winy=%d-%d\n", itrect.x,
			  itrect.x + itrect.width, itrect.y, itrect.y + itrect.height, winrect.x,
			  winrect.x + winrect.width, winrect.y, winrect.y + winrect.height);
	if (itrect.x + itrect.width > winrect.x && itrect.x < winrect.x + winrect.width
		&& itrect.y + itrect.height > winrect.y && itrect.y < winrect.y + winrect.height) {
		if (itrect.y > winrect.height + 48) {	/* the 48 is there to avoid crashing into a top-menu-bar */
			DEBUG_MSG("move_window_away_from_cursor, move window up to %d,%d\n", winrect.x,
					  itrect.y - winrect.height);
			gtk_window_move(win, winrect.x, itrect.y - winrect.height - 10);	/* add pixels 10 spacing */
		} else {
			DEBUG_MSG("move_window_away_from_cursor, move window down to %d,%d\n", winrect.x,
					  itrect.y + itrect.height);
			gtk_window_move(win, winrect.x, itrect.y + itrect.height + 10);	/* add pixels 10 spqacing */
		}
	}
}

static void scroll_to_result(Tsnr3result *s3result, GtkWindow *dialog) {
	GtkTextIter itstart, itend;
	DEBUG_MSG("scroll_to_result, started for s3result %p\n",s3result);
	gtk_text_buffer_get_iter_at_offset(DOCUMENT(s3result->doc)->buffer, &itstart, s3result->so);
	gtk_text_buffer_get_iter_at_offset(DOCUMENT(s3result->doc)->buffer, &itend, s3result->eo);
	gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(DOCUMENT(s3result->doc)->view), &itstart, 0.25, FALSE, 0.5, 0.10);
	gtk_text_buffer_select_range(DOCUMENT(s3result->doc)->buffer,&itstart,&itend);
	if (dialog)
		move_window_away_from_cursor(s3result->doc, dialog, &itstart);
	DEBUG_MSG("scroll_to_result, finished for s3result %p\n",s3result);
}

static void
remove_all_highlights_in_doc(Tdocument * doc)
{
	GtkTextTagTable *tagtable;
	GtkTextTag *tag;
	GtkTextIter itstart, itend;

	if (!doc)
		return;
		
	tagtable = gtk_text_buffer_get_tag_table(doc->buffer);
	tag = gtk_text_tag_table_lookup(tagtable, "snr3match");
	if (!tag)
		return;
	
	gtk_text_buffer_get_bounds(doc->buffer, &itstart, &itend);
	gtk_text_buffer_remove_tag(doc->buffer, tag, &itstart, &itend);
}

static void highlight_result(Tsnr3result *s3result) {
	GtkTextIter itstart, itend;
	static const gchar *tagname = "snr3match";
	static GtkTextTag *tag=NULL;
	
	gtk_text_buffer_get_iter_at_offset(DOCUMENT(s3result->doc)->buffer, &itstart, s3result->so);
	gtk_text_buffer_get_iter_at_offset(DOCUMENT(s3result->doc)->buffer, &itend, s3result->eo);

	if (!tag) {
		tag = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(DOCUMENT(s3result->doc)->buffer), tagname);
		if (!tag) {
			tag =
				gtk_text_buffer_create_tag(DOCUMENT(s3result->doc)->buffer, tagname, "background", "#FFFF57", "foreground", "#000000",
										   NULL);
		}
	}
	gtk_text_buffer_apply_tag(DOCUMENT(s3result->doc)->buffer, tag, &itstart, &itend);
}

static void highlight_run_in_doc(Tsnr3run *s3run, Tdocument *doc) {
	GList *tmplist;
	
	DEBUG_MSG("s3run=%p, highlight all results that have doc %p\n",s3run, doc);
	for (tmplist=s3run->results.head;tmplist;tmplist=g_list_next(tmplist)) {
		Tsnr3result *s3result = tmplist->data;
		if (s3result->doc == doc) {
			highlight_result(s3result);
		}
	}
	DEBUG_MSG("highlight_run_in_doc, finished\n");
}

typedef struct {
	guint startingpoint;
	gint offset;
} Toffsetupdate;

static void snr3run_update_offsets(gpointer s3result, gpointer offsetupdate) {
	if (((Tsnr3result *)s3result)->so > ((Toffsetupdate *)offsetupdate)->startingpoint) {
		((Tsnr3result *)s3result)->so += ((Toffsetupdate *)offsetupdate)->offset;
		((Tsnr3result *)s3result)->eo += ((Toffsetupdate *)offsetupdate)->offset;
	}
}

static Toffsetupdate s3result_replace(Tsnr3run *s3run, Tsnr3result *s3result, gint offset) {
	Toffsetupdate offsetupdate;
	if (s3run->type == snr3type_string) {
		doc_replace_text_backend(s3result->doc, s3run->replace, s3result->so+offset, s3result->eo+offset);
		offsetupdate.offset = g_utf8_strlen(s3run->replace, -1)-(s3result->eo - s3result->so);
	} else {
		/* TODO: IMPLEMENT PCRE */
	}
	offsetupdate.startingpoint = s3result->eo;
	return offsetupdate;
}


static void snr3result_free(gpointer s3result, gpointer s3run) {
	/* BUG TODO: for pcre patterns we should free the 'extra' data */
	DEBUG_MSG("free result %p\n",s3result);
	g_slice_free(Tsnr3result, (Tsnr3result *)s3result);
}


static void s3run_replace_current(Tsnr3run *s3run) {
	Toffsetupdate offsetupdate;
	Tsnr3result *s3result=NULL;
	GList *next, *current;
	
	if (s3run->current) {
		current = s3run->current;
		next = s3run->current->next;
	}
	if (!current)
		return;
	s3result = current->data;
	offsetupdate = s3result_replace(s3run, s3result, 0);

	snr3result_free(s3result, s3run);
	g_queue_delete_link(&s3run->results, current);
	s3run->current = next;
	
	if (s3run->current) {
		scroll_to_result(s3run->current->data, NULL);
	}
	
	/* now re-calculate all the offsets in the results lists!!!!!!!!!!! */
	g_queue_foreach(&s3run->results, snr3run_update_offsets,&offsetupdate);
}

static void sn3run_add_result(Tsnr3run *s3run, gulong so, gulong eo, gpointer doc, gpointer extra) {
	Tsnr3result *s3result;
	s3result = g_slice_new(Tsnr3result);
	s3result->so = so;
	s3result->eo = eo;
	s3result->doc = doc;
	s3result->extra = extra;
	g_queue_push_tail(&s3run->results, s3result);
} 

static void snr3_run_pcre_in_doc(Tsnr3run *s3run, Tdocument *doc, gint so, gint eo) {
	gchar *buf;
	GError *gerror = NULL;
	GRegex *gregex;
	GMatchInfo *match_info = NULL;
	gpointer extra=NULL;
		
	DEBUG_MSG("snr3_run_pcre_in_doc, started for query %s\n",s3run->query);
	if (s3run->results.head) {
		g_warning("s3run has results already\n");
	}
	buf = doc_get_chars(doc, so, eo);
	utf8_offset_cache_reset();
	gregex = g_regex_new(s3run->query,
							 (s3run->is_case_sens ? G_REGEX_MULTILINE | G_REGEX_DOTALL : G_REGEX_CASELESS |
							  G_REGEX_MULTILINE | G_REGEX_DOTALL), G_REGEX_MATCH_NEWLINE_ANY, &gerror);

	g_regex_match_full(gregex, buf, -1, 0, G_REGEX_MATCH_NEWLINE_ANY, &match_info, NULL);
	DEBUG_MSG("snr3_run_pcre_in_doc, start loop\n");
	while (g_match_info_matches(match_info)) {
		gint so, eo;
		g_match_info_fetch_pos(match_info, 0, &so, &eo);
		so = utf8_byteoffset_to_charsoffset_cached(buf, so);
		eo = utf8_byteoffset_to_charsoffset_cached(buf, eo);
		if (s3run->want_submatches) {
			guint num = g_match_info_get_match_count(match_info);
			DEBUG_MSG("got %d submatches\n",num);
			if (num>1) {
				Tsnr3submatches *tmp;
				tmp = g_slice_alloc(num*sizeof(Tsnr3submatches));
				tmp[num].so=0;
				tmp[num].eo=0;
				num--;
				while (num!=0) {
					g_match_info_fetch_pos(match_info, num, &tmp[num].so, &tmp[num].eo);
					tmp[num].so = utf8_byteoffset_to_charsoffset_cached(buf, tmp[num].so);
					tmp[num].eo = utf8_byteoffset_to_charsoffset_cached(buf, tmp[num].eo);
					DEBUG_MSG("submatch %d so=%d, eo=%d\n",num, tmp[num].so, tmp[num].eo);
					num--;
				}
				extra = tmp;
			}
		}
		sn3run_add_result(s3run, so, eo, doc, extra);
		g_match_info_next (match_info, &gerror);
	}
	g_match_info_free(match_info);
	g_regex_unref(gregex);
	if (gerror) {
		g_print("regex error %s\n",gerror->message);
		/*gchar *errstring;
		errstring = g_strdup_printf(_("Regular expression error: %s"), gerror->message);
		message_dialog_new(s3run->bfwin->main_window,
						   GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, _("Search failed"), errstring);
		g_free(errstring);*/
		g_error_free(gerror);
		return;
	}
	DEBUG_MSG("snr3_run_pcre_in_doc, finished with %d results\n",s3run->results.length);
}

static void snr3_run_string_in_doc(Tsnr3run *s3run, Tdocument *doc, gint so, gint eo) {
	gchar *buf, *result;
	gint querylen;
	char *(*f) ();
	DEBUG_MSG("snr3_run_string_in_doc, s3run=%p, started for query %s\n",s3run, s3run->query);
#ifdef SNR3_PROFILING
	s3profiling.timer = g_timer_new();
	s3profiling.numresults=0;
#endif
	if (s3run->results.head) {
		g_warning("s3run has results already\n");
	}
	result = buf = doc_get_chars(doc, so, eo);
	utf8_offset_cache_reset();
	if (s3run->is_case_sens) {
		f = strstr;
	} else {
		f = strcasestr;
	}
	querylen = g_utf8_strlen(s3run->query, -1);
	do {
		result = f(result, s3run->query);
		if (result) {
			glong char_o = utf8_byteoffset_to_charsoffset_cached(buf, (result-buf));
			sn3run_add_result(s3run, char_o+so, char_o+querylen+so, doc, NULL);
#ifdef SNR3_PROFILING
			s3profiling.numresults++;
#endif
			result++;
		}		
	} while (result);
	

#ifdef SNR3_PROFILING
	g_print("search %ld bytes with %d results run took %f s, %f bytes/s\n",(glong)strlen(buf),s3profiling.numresults,g_timer_elapsed(s3profiling.timer, NULL),strlen(buf)/g_timer_elapsed(s3profiling.timer, NULL));
	g_timer_destroy(s3profiling.timer);
#endif
	g_free(buf);
	DEBUG_MSG("snr3_run_string_in_doc, finished with %d results\n",s3run->results.length);
}

void snr3_run_go(Tsnr3run *s3run, gboolean forward) {
	GList *next=NULL;
	DEBUG_MSG("snr3_run_go, s3run=%p\n",s3run);
	if (s3run->current) {
		if (forward) {
			next = g_list_next(s3run->current);
		} else {
			next = g_list_previous(s3run->current);
		}
	}
	if (!next) {
		next = forward ? s3run->results.head : s3run->results.tail;
	}
	DEBUG_MSG("scroll to result %p\n",next);
	if (next) {
		s3run->current = next;
		scroll_to_result(next->data, NULL);
	}
}


static void snr3_run(Tsnr3run *s3run) {
	gint so,eo;
	GList *tmplist;
	DEBUG_MSG("snr3_run, s3run=%p\n",s3run);
	switch(s3run->scope) {
		case snr3scope_doc:
			if (s3run->type == snr3type_string) 
				snr3_run_string_in_doc(s3run, s3run->bfwin->current_document, 0, -1);
			else if (s3run->type == snr3type_pcre)
				snr3_run_pcre_in_doc(s3run, s3run->bfwin->current_document, 0, -1);
		break;
		case snr3scope_cursor:
			so = doc_get_cursor_position(s3run->bfwin->current_document);
			if (s3run->type == snr3type_string) 
				snr3_run_string_in_doc(s3run, s3run->bfwin->current_document, so, -1);
			else if (s3run->type == snr3type_pcre)
				snr3_run_pcre_in_doc(s3run, s3run->bfwin->current_document, so, -1);
		break;
		case snr3scope_selection:
			if (doc_get_selection(s3run->bfwin->current_document, &so, &eo)) {
				if (s3run->type == snr3type_string) 
					snr3_run_string_in_doc(s3run, s3run->bfwin->current_document, so, eo);
				else if (s3run->type == snr3type_pcre)
					snr3_run_pcre_in_doc(s3run, s3run->bfwin->current_document, so, eo);
			} else {
				g_print("Find in selection, but there is no selection\n");
			}
		break;
		case snr3scope_alldocs:
			for (tmplist=g_list_first(s3run->bfwin->documentlist);tmplist;tmplist=g_list_next(tmplist)) {
				/* TODO: run in idle loop to avoid blocking the GUI */
				if (s3run->type == snr3type_string) 
					snr3_run_string_in_doc(s3run, s3run->bfwin->current_document, 0, -1);
				else if (s3run->type == snr3type_pcre)
					snr3_run_pcre_in_doc(s3run, s3run->bfwin->current_document, 0, -1);
			}
		break;
		case snr3scope_files:
			/* TODO: implement background file loading and saving for search and replace */
			g_print("TODO: implement background file loading for replace\n");
		break;
	}
}

void snr3run_free(Tsnr3run *s3run) {
	DEBUG_MSG("snr3run_free, started for %p\n",s3run);
	DEBUG_MSG("snr3run_free, query at %p\n",s3run->query);
	g_free(s3run->query);
	g_free(s3run->replace);
	remove_all_highlights_in_doc(s3run->bfwin->current_document);
	
	g_queue_foreach(&s3run->results, snr3result_free,s3run);
	g_slice_free(Tsnr3run, s3run);
}

gpointer simple_search_run(Tbfwin *bfwin, const gchar *string) {
	Tsnr3run *s3run;
	
	s3run = g_slice_new0(Tsnr3run);
	s3run->bfwin = bfwin;
	/*s3run->query = g_regex_escape_string(string,-1);
	s3run->type = snr3type_pcre;*/
	s3run->query = g_strdup(string);
	DEBUG_MSG("snr3run at %p, query at %p\n",s3run, s3run->query);
	s3run->type = snr3type_string;
	s3run->scope = snr3scope_doc;
	g_queue_init(&s3run->results);
	snr3_run(s3run);
	highlight_run_in_doc(s3run, bfwin->current_document);
	snr3_run_go(s3run, TRUE);
	return s3run;
}

/***************************************************************************/
/***************************** GUI *****************************************/
/***************************************************************************/

typedef struct {
	GtkWidget *dialog;
	GtkWidget *expander;
	GtkWidget *search;
	GtkWidget *replace;
	GtkWidget *scope;
	GtkWidget *basedir;
	GtkWidget *filepattern;
	GtkWidget *countlabel;
	GtkWidget *warninglabel;
	GtkWidget *matchPattern;
	GtkWidget *replaceType;
	GtkWidget *overlappingMatches;
	GtkWidget *matchCase;
	GtkWidget *escapeChars;
	GtkWidget *select_match;
	GtkWidget *bookmarks;
	GtkWidget *findButton;
	GtkWidget *findAllButton;
	GtkWidget *replaceButton;
	GtkWidget *replaceAllButton;
	Tbfwin *bfwin;
} TSNRWin;

enum {
	SNR_RESPONSE_FIND = 0,
	SNR_RESPONSE_REPLACE,
	SNR_RESPONSE_REPLACE_ALL,
	SNR_RESPONSE_FIND_ALL
};

void 
snr3_advanced_dialog(Tbfwin * bfwin)
{
	TSNRWin *snrwin;
	GtkWidget *table, *vbox, *vbox2, *button;
	gint currentrow=0;
	GtkListStore *history, *lstore;
	GList *list;
	GtkTreeIter iter;
	/*GtkTextIter start, end; */
	unsigned int i = 0;

	const gchar *scope[] = {
		N_("Entire document"),
		N_("Forward from cursor position"),
		N_("Selection"),
		N_("All open documents"),
		N_("Files on disk")
	};

	const gchar *matchPattern[] = {
		N_("Normal"),
		N_("PERL style regular expression"),
	};

	const gchar *replaceType[] = {
		N_("Normal"),
		N_("Uppercase"),
		N_("Lowercase"),
	};

	snrwin = g_slice_new0(TSNRWin);
	snrwin->bfwin = bfwin;

	snrwin->dialog =
		gtk_dialog_new_with_buttons(_("Find and Replace"), GTK_WINDOW(bfwin->main_window),
									GTK_DIALOG_DESTROY_WITH_PARENT, NULL);
/*	gtk_window_set_resizable(GTK_WINDOW(snrwin->dialog), FALSE);
	gtk_dialog_set_has_separator(GTK_DIALOG(snrwin->dialog), FALSE);*/
	window_delete_on_escape(GTK_WINDOW(snrwin->dialog));
	/*g_signal_connect(G_OBJECT(snrwin->dialog), "response", G_CALLBACK(snr_response_lcb), snrwin);
	g_signal_connect_after(G_OBJECT(snrwin->dialog), "focus-in-event", G_CALLBACK(snr_focus_in_lcb), snrwin);*/
	table =
		dialog_table_in_vbox(10, 3, 6, gtk_dialog_get_content_area(GTK_DIALOG(snrwin->dialog)), FALSE,
							 FALSE, 0);
	
	
	history = gtk_list_store_new(1, G_TYPE_STRING);
	list = g_list_last(bfwin->session->searchlist);
	while (list) {
		DEBUG_MSG("snr_dialog_real: adding search history %s\n", (gchar *) list->data);
		gtk_list_store_append(history, &iter);
		gtk_list_store_set(history, &iter, 0, list->data, -1);
		list = g_list_previous(list);
	}
	snrwin->search = gtk_combo_box_entry_new_with_model(GTK_TREE_MODEL(history), 0);
	/* this kills the primary selection, which is annoying if you want to 
	   search/replace within the selection  */
	/*if (bfwin->session->searchlist)
	   gtk_combo_box_set_active(GTK_COMBO_BOX(snrwin->search), 0); */
	g_object_unref(history);
	dialog_mnemonic_label_in_table(_("<b>_Search for</b>"), snrwin->search, table, 0, 1, currentrow, currentrow+1);
	gtk_table_attach(GTK_TABLE(table), snrwin->search, 0, 3, currentrow+1, currentrow+2, GTK_EXPAND | GTK_FILL, GTK_SHRINK, 0, 0);
/*	g_signal_connect(snrwin->search, "changed", G_CALLBACK(snr_comboboxentry_changed), snrwin);
	g_signal_connect(snrwin->search, "realize", G_CALLBACK(realize_combo_set_tooltip),
					 _("The pattern to look for"));
	g_signal_connect(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(snrwin->search))), "activate",
					 G_CALLBACK(snr_combo_activate_lcb), snrwin);*/

	currentrow+=2;

	history = gtk_list_store_new(1, G_TYPE_STRING);
	list = g_list_last(bfwin->session->replacelist);
	while (list) {
		DEBUG_MSG("snr_dialog_real: adding replace history %s\n", (gchar *) list->data);
		gtk_list_store_append(history, &iter);
		gtk_list_store_set(history, &iter, 0, list->data, -1);
		list = g_list_previous(list);
	}
	snrwin->replace = gtk_combo_box_entry_new_with_model(GTK_TREE_MODEL(history), 0);
	g_object_unref(history);
	dialog_mnemonic_label_in_table(_("<b>Replace _with</b>"), snrwin->replace, table, 0, 1, currentrow, currentrow+1);
	gtk_table_attach(GTK_TABLE(table), snrwin->replace, 0, 3, currentrow+1, currentrow+2, GTK_EXPAND | GTK_FILL,
					 GTK_SHRINK, 0, 0);
/*	g_signal_connect(snrwin->replace, "changed", G_CALLBACK(snr_comboboxentry_changed), snrwin);
	g_signal_connect(snrwin->replace, "realize", G_CALLBACK(realize_combo_set_tooltip),
					 _("Replace matching text with"));
	g_signal_connect(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(snrwin->replace))), "activate",
					 G_CALLBACK(snr_combo_activate_lcb), snrwin);
*/
	currentrow+=2;

	dialog_mnemonic_label_in_table(_("<b>Options</b>"), NULL, table, 0, 1, currentrow, currentrow+1);
	
	currentrow++;

	snrwin->matchPattern = gtk_combo_box_new_text();
	for (i = 0; i < G_N_ELEMENTS(matchPattern); i++) {
		gtk_combo_box_append_text(GTK_COMBO_BOX(snrwin->matchPattern), _(matchPattern[i]));
	}
	dialog_mnemonic_label_in_table(_("Match Patter_n: "), snrwin->matchPattern, table, 0, 1, currentrow, currentrow+1);
	gtk_table_attach(GTK_TABLE(table), snrwin->matchPattern, 1, 3, currentrow, currentrow+1, GTK_EXPAND | GTK_FILL,
					 GTK_SHRINK, 0, 0);
/*	g_signal_connect(snrwin->matchPattern, "changed", G_CALLBACK(snr_combobox_changed), snrwin);
	g_signal_connect(snrwin->matchPattern, "realize", G_CALLBACK(realize_combo_set_tooltip),
					 _("How to interpret the pattern."));
*/
	currentrow++;
	
	snrwin->scope = gtk_combo_box_new_text();
	for (i = 0; i < G_N_ELEMENTS(scope); i++) {
		gtk_combo_box_append_text(GTK_COMBO_BOX(snrwin->scope), _(scope[i]));
	}
	dialog_mnemonic_label_in_table(_("Sco_pe: "), snrwin->scope, table, 0, 1, currentrow, currentrow+1);
	gtk_table_attach(GTK_TABLE(table), snrwin->scope, 1, 3, currentrow, currentrow+1,GTK_EXPAND | GTK_FILL, GTK_SHRINK, 0, 0);
	/*g_signal_connect(snrwin->scope, "changed", G_CALLBACK(snr_combobox_changed), snrwin);
	g_signal_connect(snrwin->scope, "realize", G_CALLBACK(realize_combo_set_tooltip),
					 _("Where to look for the pattern."));*/
	
	currentrow++;
	lstore = gtk_list_store_new(1, G_TYPE_STRING);
	/*for (i = 0; i < G_N_ELEMENTS(fileExts); i++) {
		gtk_list_store_append(GTK_LIST_STORE(lstore), &iter);
		gtk_list_store_set(GTK_LIST_STORE(lstore), &iter, 0, fileExts[i], -1);
	};*/
	snrwin->filepattern = gtk_combo_box_entry_new_with_model(GTK_TREE_MODEL(lstore), 0);
	g_object_unref(lstore);
	gtk_table_attach_defaults(GTK_TABLE(table), snrwin->filepattern, 0, 1, currentrow, currentrow+1);
	button = dialog_button_new_with_image_in_table(NULL, -1, GTK_STOCK_OPEN, GTK_ICON_SIZE_MENU, table, 2, 3, currentrow, currentrow+1);
	snrwin->basedir = dialog_entry_in_table("", table, 1, 2, currentrow, currentrow+1);
	/* add a basedir and file pattern widget here */
	
	currentrow++;

	snrwin->replaceType = gtk_combo_box_new_text();
	for (i = 0; i < G_N_ELEMENTS(replaceType); i++) {
		gtk_combo_box_append_text(GTK_COMBO_BOX(snrwin->replaceType), _(replaceType[i]));
	}
	dialog_mnemonic_label_in_table(_("Replace T_ype: "), snrwin->replaceType, table, 0, 1, currentrow, currentrow+1);
	gtk_table_attach(GTK_TABLE(table), snrwin->replaceType, 1, 3, currentrow, currentrow+1, GTK_EXPAND | GTK_FILL,
					 GTK_SHRINK, 0, 0);
	/*g_signal_connect(snrwin->replaceType, "changed", G_CALLBACK(snr_combobox_changed), snrwin);
	g_signal_connect(snrwin->replaceType, "realize", G_CALLBACK(realize_combo_set_tooltip),
					 _("What to replace with."));*/
	
	currentrow++;
	
	snrwin->matchCase = dialog_check_button_in_table(_("Case sensitive _matching"), FALSE, table,
										0, 3, currentrow, currentrow+1);
	/*g_signal_connect(snrwin->matchCase, "toggled", G_CALLBACK(snr_option_toggled), snrwin);*/
	gtk_widget_set_tooltip_text(snrwin->matchCase, _("Only match if case (upper/lower) is identical."));

	currentrow++;

	snrwin->escapeChars = dialog_check_button_in_table(_("Pattern contains escape-se_quences"), FALSE, table,
										0, 3, currentrow, currentrow+1);
	/*g_signal_connect(snrwin->escapeChars, "toggled", G_CALLBACK(snr_option_toggled), snrwin);*/
	gtk_widget_set_tooltip_text(snrwin->escapeChars,
								_("Pattern contains backslash escaped characters such as \\n \\t etc."));

	currentrow++;


/*	snrwin->overlappingMatches = gtk_check_button_new_with_mnemonic(_("Allow o_verlapping matches"));
	gtk_box_pack_start(GTK_BOX(vbox2), snrwin->overlappingMatches, FALSE, FALSE, 0);
	g_signal_connect(snrwin->overlappingMatches, "toggled", G_CALLBACK(snr_option_toggled), snrwin);
	gtk_widget_set_tooltip_text(snrwin->overlappingMatches,
								_("After a match is found, start next search within that match."));


	snrwin->select_match = gtk_check_button_new_with_mnemonic(_("Select matc_hes"));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(snrwin->select_match), main_v->globses.snr_select_match);
	gtk_box_pack_start(GTK_BOX(vbox2), snrwin->select_match, FALSE, FALSE, 0);
	gtk_widget_set_tooltip_text(snrwin->select_match,
								_("Select the matching text instead of just highlighting it"));

	snrwin->bookmarks = gtk_check_button_new_with_mnemonic(_("_Bookmark matches"));
	gtk_box_pack_start(GTK_BOX(vbox2), snrwin->bookmarks, FALSE, FALSE, 0);
	gtk_widget_set_tooltip_text(snrwin->bookmarks, _("Create a bookmark for each match"));
*/
	gtk_dialog_add_button(GTK_DIALOG(snrwin->dialog), GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE);
	snrwin->replaceAllButton =
			gtk_dialog_add_button(GTK_DIALOG(snrwin->dialog), _("Replace _All"), SNR_RESPONSE_REPLACE_ALL);
	snrwin->replaceButton =
			gtk_dialog_add_button(GTK_DIALOG(snrwin->dialog), _("_Replace"), SNR_RESPONSE_REPLACE);
	snrwin->findButton = gtk_dialog_add_button(GTK_DIALOG(snrwin->dialog), GTK_STOCK_FIND, SNR_RESPONSE_FIND);
	/*gtk_dialog_set_response_sensitive(GTK_DIALOG(snrwin->dialog), SNR_RESPONSE_FIND, FALSE); */
	/*snr_comboboxentry_changed(GTK_COMBO_BOX_ENTRY(snrwin->search), snrwin);*/
	gtk_widget_show_all(GTK_WIDGET(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(snrwin->dialog)))));
	/* this kills the primary selection, which is annoying if you want to 
	   search/replace within the selection  */
/*    if (gtk_text_buffer_get_selection_bounds(bfwin->current_document->buffer, &start, &end)) {
        gchar * buffer = gtk_text_buffer_get_text(bfwin->current_document->buffer, &start, &end, FALSE);

        if (strchr(buffer, '\n') == NULL) {
            gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(snrwin->search))), buffer);
            gtk_editable_select_region(GTK_EDITABLE(gtk_bin_get_child(GTK_BIN(snrwin->search))), 0, -1);
        }
        if (buffer)    g_free(buffer);
    }*/

	gtk_combo_box_set_active(GTK_COMBO_BOX(snrwin->scope), 0);
	gtk_combo_box_set_active(GTK_COMBO_BOX(snrwin->matchPattern), 0);
	gtk_combo_box_set_active(GTK_COMBO_BOX(snrwin->replaceType), 0);
	gtk_dialog_set_default_response(GTK_DIALOG(snrwin->dialog), SNR_RESPONSE_FIND);
	DEBUG_MSG("snr_dialog_real: display the dialog\n");
	gtk_widget_show(snrwin->dialog);

/*	if ((bfwin->session->snr_position_x >= 0) && (bfwin->session->snr_position_y >= 0) &&
		(bfwin->session->snr_position_x < (gdk_screen_width() - 50))
		&& (bfwin->session->snr_position_y < (gdk_screen_height() - 50))) {
		gtk_window_move(GTK_WINDOW(snrwin->dialog), bfwin->session->snr_position_x,
						bfwin->session->snr_position_y);
	}
*/
}
