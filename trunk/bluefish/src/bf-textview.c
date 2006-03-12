/* Bluefish HTML Editor
 * bf-textview.c - Bluefish text widget
 * 
 * Copyright (C) 2005 Oskar Świda swida@aragorn.pb.bialystok.pl
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
 *
 * indenting is done with
 * indent --line-length 100 --k-and-r-style --tab-size 4 -bbo --ignore-newlines bf-textview.c
 */

/* #define DEBUG */

/*
Typical scanner in compiler is an automata. To implement automata you
need a TABLE. Such a table
is an implementation of function:

    ( state , character ) ------>  next state

where "next state" can be simply next automata state or "finishing
state" which means we are accepting some word.
For example: we want to build scanner recognizing three words "some",
"somebody" and "any"

Sample table

|state|| space| a | b | d | e | m | n | o | s | y |
|  0  ||      | 1 |   |   |   |   |   |   | 3 |   |
|  1  ||      |   |   |   |   |   | 2 |   |   |   |
|  2  ||      |   |   |   |   |   |   |   |   |   |
|  3  ||      |   |   |   |   |   |   | 4 |   |   |
|  4  ||      |   |   |   |   | 5 |   |   |   |   |
|  5  ||      |   |   |   | 6 |   |   |   |   |   |
|  6  ||   *  | * | 7 | * | * | * | * | * | * | * |
|  7  ||      |   |   |   |   |   |   | 8 |   |   |
|  8  ||      |   |   | 9 |   |   |   |   |   |   |
|  9  ||      |   |   |   |   |   |   |   |   | # |

* = 'some'
# = 'somebody'

Let's analyze: given a phrase "Does any some somebody " we can start
scanner.
First state is 0, so:
  (0,D)-> 0 , (0,o)->0, (0,e)->0, (0,s)->3  but (3,space)->0  again
then:
  (0,a)->1 , (1,n)->2, (2,y)->word "any" found

And so on. As you see the case of "some" and "somebody" is different
because these words have the same prefix, so
when in state 5 I have "e" character I'm going to state 6 where the
decision is made. If next character is "b" it means it
could be "somebody", everything else means we have found word "some".

In BfTextView I'm building such table from configuration file (it is a
bit more complex because of regular expressions) and
that is all what I need. Next if I want find any tokens in editor text I
simply set scanner state to 0 and start to iterate through
characters, changing scanner states according the table. If I find
"finishing state" I'm marking a token.

*/

#include "config.h"
#ifdef USE_SCANNER

#include "bf-textview.h"
#include "bluefish.h"
#include "textstyle.h"
#include "bf_lib.h"
#include "gtk_easy.h" /* gdk_color_to_hexstring */

#include <stdarg.h>
#include <string.h>
#include <libxml/xmlmemory.h>
#include <libxml/parser.h>

#ifdef HL_PROFILING				/* scanning profiling information, interesting for users making a new pattern */
#include <sys/times.h>
#include <unistd.h>
#endif

#ifdef HL_PROFILING
struct tms tms1;
struct tms tms2;
#endif							/* HL_PROFILING */


static char folded_xbm[] = {
  0x02, 0x01
};

static void bf_textview_init(BfTextView * o);
static void bf_textview_class_init(BfTextViewClass * c);
static gboolean bf_textview_expose_cb(GtkWidget * widget, GdkEventExpose * event, gpointer doc);
/* static void bf_textview_changed_cb (GtkTextBuffer * textbuffer,gpointer user_data);*/
static gboolean bf_textview_mouse_cb(GtkWidget * widget, GdkEvent * event, gpointer user_data);
static void bf_textview_delete_range_cb(GtkTextBuffer * textbuffer, GtkTextIter * arg1,
										GtkTextIter * arg2, gpointer user_data);
static void bf_textview_delete_range_after_cb(GtkTextBuffer * textbuffer, GtkTextIter * arg1,
										GtkTextIter * arg2, gpointer user_data);										
static void bftv_delete_blocks_from_area(BfTextView * view, GtkTextIter * arg1, GtkTextIter * arg2);
static void bf_textview_insert_text_cb(GtkTextBuffer * textbuffer, GtkTextIter * iter, gchar * string,
                                       gint stringlen, BfTextView * view);
static void bftv_fold(BfTextView *self,GtkTextMark * mark, gboolean move_cursor);
static void bf_textview_move_cursor_cb(GtkTextView * widget, GtkMovementStep step, gint count,
									   gboolean extend_selection, gpointer user_data);

#define BFTV_TOKEN_IDS		10000
#define BFTV_BLOCK_IDS		14000

static gshort tid_token = 10;
/*static gshort tid_tag_start = 11;*/
/*static gshort tid_tag_end = 12;*/
static gshort tid_tag_attr = 13;
static gshort tid_block_start = 14;
static gshort tid_block_end = 15;
static gshort tid_true = 16;
static gshort tid_false = 17;


static GtkTextViewClass *parent_class = NULL;

GType bf_textview_get_type(void)
{
	static GType type = 0;

	if (type == 0) {
		static const GTypeInfo info = {
			sizeof(BfTextViewClass),
			(GBaseInitFunc) NULL,
			(GBaseFinalizeFunc) NULL,
			(GClassInitFunc) bf_textview_class_init,
			(GClassFinalizeFunc) NULL,
			NULL /* class_data */ ,
			sizeof(BfTextView),
			0 /* n_preallocs */ ,
			(GInstanceInitFunc) bf_textview_init,
			NULL
		};
		type = g_type_register_static(GTK_TYPE_TEXT_VIEW, "BfTextView", &info, (GTypeFlags) 0);
	}
	return type;
}

#define GET_NEW ((BfTextView *)g_object_new(bf_textview_get_type(), NULL))

static void bftv_expand_all(GtkWidget * widget, BfTextView * view)
{
	GtkTextIter it;
	bf_textview_fold_blocks(view, FALSE);
	gtk_text_buffer_get_start_iter(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), &it);
	gtk_text_buffer_place_cursor(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), &it);
	gtk_widget_queue_draw(GTK_WIDGET(view));
}

static void bftv_collapse_all(GtkWidget * widget, BfTextView * view)
{
	GtkTextIter it;
	bf_textview_fold_blocks(view, TRUE);
	gtk_text_buffer_get_start_iter(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), &it);
	gtk_text_buffer_place_cursor(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), &it);
	gtk_widget_queue_draw(GTK_WIDGET(view));
}



static void bf_textview_init(BfTextView * o)
{
	GtkWidget *item;

	o->lw_size_lines = o->lw_size_blocks = o->lw_size_sym = 0;
	o->symbols = g_hash_table_new(g_str_hash, g_str_equal);
	o->symbol_lines = g_hash_table_new(g_direct_hash, g_direct_equal);
	o->lang = NULL;
	g_stpcpy(o->bkg_color,"#FFFFFF");
	g_stpcpy(o->fg_color,"#000000");
	o->fold_menu = gtk_menu_new();
	item = gtk_menu_item_new_with_label(_("Expand all"));
	gtk_menu_append(GTK_MENU(o->fold_menu), item);
	g_signal_connect(GTK_OBJECT(item), "activate", G_CALLBACK(bftv_expand_all), o);
	item = gtk_menu_item_new_with_label(_("Collapse all"));
	gtk_menu_append(GTK_MENU(o->fold_menu), item);
	gtk_widget_show_all(o->fold_menu);
	g_signal_connect(GTK_OBJECT(item), "activate", G_CALLBACK(bftv_collapse_all), o);
	o->group_tags = g_hash_table_new(g_str_hash, g_str_equal);
	o->block_tags = g_hash_table_new(g_str_hash, g_str_equal);
	o->token_tags = g_hash_table_new(g_str_hash, g_str_equal);
	o->highlight = TRUE;
	o->mark_tokens = FALSE;
	o->match_blocks = TRUE;
   o->paste_operation = FALSE;
	o->hl_mode = BFTV_HL_MODE_VISIBLE;
	o->show_current_line = main_v->props.view_cline;
	o->tag_autoclose = FALSE;
	o->fbal_cache = g_hash_table_new(g_int_hash,g_int_equal);
	o->lbal_cache = g_hash_table_new(g_int_hash,g_int_equal);
	o->last_matched_block = NULL;
	o->show_rmargin = FALSE;
	o->rmargin_at = 0;
	o->tag_ac_state = FALSE;
}


static void bf_textview_class_init(BfTextViewClass * c)
{
	parent_class = g_type_class_ref(GTK_TYPE_TEXT_VIEW);
}

/**
 * bftv_get_block_at_iter:
 *
 * this documentation is a guess,  *** Oskar please confirm is this is right ***
 * ******* CONFIRMED *************
 *
 * this function tries if iter 'it' is at the position of a block start or block end
 * if so, it will return the GtkTextMark that marks the start or end of that block
 *
 * profiling reveils that this function takes 9.51% of the 
 * time during file loading, because it is called *very* often
 * so this is a good candidate for speed optimisation, or
 * the calling function should be optimised to cache the
 * calls to this function ???
 * O. - I've declared this function as inline - should improve efficiency.
 */
inline GtkTextMark *bftv_get_block_at_iter(GtkTextIter * it)
{
	GSList *lst = gtk_text_iter_get_marks(it);
	gpointer ptr = NULL;

	while (lst) {
		ptr = g_object_get_data(G_OBJECT(lst->data), "_type_");
		if (ptr && (ptr == &tid_block_start || ptr == &tid_block_end)) {
			return GTK_TEXT_MARK(lst->data);
		}
		lst = g_slist_next(lst);
	}
	g_slist_free(lst);
	return NULL;
}
/*
 *
 * this documentation is a guess,  *** Oskar please confirm is this is right ***
 * ******* CONFIRMED ********
 * this function searches for the first block that is started or ended on the line 
 * where iter 'it' is located. It returns the GtkTextMark that marks this block
 *
 * hmm this function is called quite heavily during scrolling through a document
 * which makes scrolling quite slow.. (16% of the cpu time during a profiling run 
 * with loits of scrolling)
 */
GtkTextMark *bftv_get_first_block_at_line(BfTextView *view,GtkTextIter * it, gboolean not_single)
{
	GtkTextIter it2, it3;
	GtkTextMark *mark = NULL;
	gint *ln = NULL; 
	gpointer ptr = NULL;
	

	if (!it) return NULL;
	ln = g_new0(gint,1);
	*ln = gtk_text_iter_get_line(it);
	ptr = g_hash_table_lookup(view->fbal_cache,ln);	
	if ( ptr )
	{
		g_free(ln);
		return GTK_TEXT_MARK(ptr);
	}
	it2 = it3 = *it;
	gtk_text_iter_set_line(&it2, gtk_text_iter_get_line(it));
	gtk_text_iter_forward_to_line_end(&it3);
	gtk_text_iter_backward_char(&it3);

	while (gtk_text_iter_compare(&it2, &it3) <= 0
		   && gtk_text_iter_get_line(&it2) == gtk_text_iter_get_line(it)) {
		mark = bftv_get_block_at_iter(&it2);
		if (mark) {
			if (not_single) {
				if (g_object_get_data(G_OBJECT(mark), "single-line") != &tid_true)
				{
					g_hash_table_insert(view->fbal_cache,ln,mark);
					return mark;
				}	
			} else
			{
				g_hash_table_insert(view->fbal_cache,ln,mark);
				return mark;
			}	
		}
		if (!gtk_text_iter_forward_char(&it2))
			break;
	}
	g_free(ln);
	return NULL;
}
/*
 *
 * this documentation is a guess,  *** Oskar please confirm is this is right ***
 * ******** CONFIRMED ***************
 * this function searches for the last block that is started or ended on the line 
 * where iter 'it' is located. It returns the GtkTextMark that marks this block
 */
GtkTextMark *bftv_get_last_block_at_line(BfTextView *view, GtkTextIter * it)
{
	GtkTextIter it2, it3;
	GtkTextMark *mark = NULL, *mark2 = NULL;
	gint *ln = NULL;
	gpointer ptr = NULL;

	if (!it)	return NULL;
	/* I see a very intermittent memory leak here.
	   The memory is being freed inside if() statements.
	   Is it possible there is occasionally a branch when it is not being freed? 
	   
	   I don't suppose or I'm misunderstanding GHashTable.
	   "ln" is used as a key, so when inserting into GHashTable, I'm not freeing it (there is no key copying in glib code),
	   but  it is freed in "remove_item_cache" later.
	   
	   */
	ln = g_new0(gint,1);
	*ln = gtk_text_iter_get_line(it);
	ptr = g_hash_table_lookup(view->lbal_cache,ln);	
	if ( ptr )
	{
		g_free(ln);
		return GTK_TEXT_MARK(ptr);
	}
	
	it2 = it3 = *it;
	gtk_text_iter_set_line(&it2, gtk_text_iter_get_line(it));
	gtk_text_iter_forward_to_line_end(&it3);
	gtk_text_iter_backward_char(&it3);

	while (gtk_text_iter_compare(&it2, &it3) <= 0
		   && gtk_text_iter_get_line(&it2) == gtk_text_iter_get_line(it)) {
		mark = bftv_get_block_at_iter(&it2);
		if (mark) {
			mark2 = mark;
		}
		if (!gtk_text_iter_forward_char(&it2))
			break;
	}
	if (mark2) g_hash_table_insert(view->fbal_cache,ln,mark2);
	else g_free(ln);
	return mark2;
}

/*
 * ------------------------ SCANNING ---------------------------- 
 */


/**
*	bf_textview_scan:
*	@self:  BfTextView widget 
*
*	Scan whole buffer text.
*/
void bf_textview_scan(BfTextView * self)
{
	GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self));
	GtkTextIter its, ite;

	g_return_if_fail(self != NULL);
	g_return_if_fail(BF_IS_TEXTVIEW(self));
	g_return_if_fail(buf != NULL);
	gtk_text_buffer_get_start_iter(buf, &its);
	gtk_text_buffer_get_end_iter(buf, &ite);
	
	if (gtk_text_iter_equal(&its, &ite))
		return;
	self->need_rescan = FALSE;
	bf_textview_scan_area(self, &its, &ite);
}
#ifdef HL_PROFILING
/* this function is here for debugging purposes */
static void bftv_dump_location_info(gint line, GtkTextBuffer *buffer, GtkTextIter *it) {
	GSList *ss;
	ss = gtk_text_iter_get_marks(it);
	while (ss) {
		GtkTextMark *m = (GtkTextMark *)ss->data;
		g_print("bftv_dump_location_info, called from line %d, location %d has mark %p (%s)\n",line,gtk_text_iter_get_offset(it),m,gtk_text_mark_get_name(m));
		ss = g_slist_next(ss);
	}
	g_slist_free(ss);
}
#endif /* HL_DEBUG */

typedef struct {
	GtkTextBuffer *buffer;
	GtkTextIter *start;
	GtkTextIter *end;
} Trts;

static void bftv_remove_t_tag(gpointer key,gpointer value,gpointer data) {
	BfLangToken *t = (BfLangToken*)value;
	Trts *s = (Trts*)data;
	if ( t->tag )
		gtk_text_buffer_remove_tag(s->buffer,t->tag,s->start,s->end);
}

static void bftv_remove_b_tag(gpointer key,gpointer value,gpointer data) {
	BfLangBlock *b = (BfLangBlock*)value;
	Trts *s = (Trts*)data;
	if ( b->tag )
		gtk_text_buffer_remove_tag(s->buffer,b->tag,s->start,s->end);
}

static gboolean bftv_remove_cache_item(gpointer key,gpointer value,gpointer data) {
	g_free((gint*)key);
	return TRUE;
}
/**
 * bf_textview_scan_area:
 * @self:  BfTextView widget 
 * @start: textbuffer iterator at start of area
 * @end: textbuffer iterator at end of area
 *
 * Scan buffer area - this is main scanning function, the rest of "scans" uses this method.
 *
 * after some profiling, this functions takes up 77.45% of the time during the loading of files
 * so this is an interesting candidate for speed optimisations
 *
 */
void bf_textview_scan_area(BfTextView * self, GtkTextIter * start, GtkTextIter * end)
{
	GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self));
	GtkTextIter its, ita, pit;
	gunichar c;
	gboolean block_found = FALSE, token_found = FALSE, recognizing = FALSE;
	GtkTextMark *mark, *mark2;
	gshort magic = 0;
	gshort currstate = 0;
	TBfBlock *bf = NULL;
	GtkTextTag *tag = NULL;
	gshort **currtable;
	Trts rts;


	g_return_if_fail(self != NULL);
	g_return_if_fail(BF_IS_TEXTVIEW(self));
	if (!self->lang)
		return;

	its = *start;
	ita = *start;
	self->scanner.current_context = NULL;
	self->scanner.state = BFTV_STATE_NORMAL;
	self->scanner.current_block = NULL;
	currstate = 0;

#ifdef HL_PROFILING
	times(&tms1);
#endif


	while (g_queue_pop_head(&(self->scanner.block_stack)) != NULL) {};
	while (g_queue_pop_head(&(self->scanner.block_stack2)) != NULL) {};
	while (g_queue_pop_head(&(self->scanner.tag_stack)) != NULL) {};
	g_hash_table_foreach_remove(self->fbal_cache,bftv_remove_cache_item,NULL);
	g_hash_table_foreach_remove(self->lbal_cache,bftv_remove_cache_item,NULL);
	if (self->last_matched_block)
	{
		GtkTextIter it,it2;
		gtk_text_buffer_get_iter_at_mark(buf, &it, self->last_matched_block);
		gtk_text_buffer_get_iter_at_mark(buf, &it2,
										 GTK_TEXT_MARK(g_object_get_data(G_OBJECT(self->last_matched_block), "ref")));
		gtk_text_buffer_remove_tag(buf, self->block_match_tag, &it, &it2);
		if (g_object_get_data(G_OBJECT(self->last_matched_block), "_type_") == &tid_block_start) {
			gtk_text_buffer_get_iter_at_mark(buf, &it,
											 GTK_TEXT_MARK(g_object_get_data
														   (G_OBJECT(self->last_matched_block), "ref_e1")));
			gtk_text_buffer_get_iter_at_mark(buf, &it2,
											 GTK_TEXT_MARK(g_object_get_data
														   (G_OBJECT(self->last_matched_block), "ref_e2")));
		} else if (g_object_get_data(G_OBJECT(self->last_matched_block), "_type_") == &tid_block_end) {
			gtk_text_buffer_get_iter_at_mark(buf, &it,
											 GTK_TEXT_MARK(g_object_get_data
														   (G_OBJECT(self->last_matched_block), "ref_b1")));
			gtk_text_buffer_get_iter_at_mark(buf, &it2,
											 GTK_TEXT_MARK(g_object_get_data
														   (G_OBJECT(self->last_matched_block), "ref_b2")));
		}
		gtk_text_buffer_remove_tag(buf, self->block_match_tag, &it, &it2);		
	}
	self->last_matched_block = NULL;
	bftv_delete_blocks_from_area(self, start, end);
	if ( self->lang->tag_begin ) gtk_text_buffer_remove_tag(buf,self->lang->tag_begin,start,end);
	if ( self->lang->tag_end ) gtk_text_buffer_remove_tag(buf,self->lang->tag_end,start,end);
	if ( self->lang->attr_name ) gtk_text_buffer_remove_tag(buf,self->lang->attr_name,start,end);
	if ( self->lang->attr_val ) gtk_text_buffer_remove_tag(buf,self->lang->attr_val,start,end);		
	rts.buffer = buf;
	rts.start = start;
	rts.end = end;
	g_hash_table_foreach(self->lang->tokens,bftv_remove_t_tag,&rts);
	g_hash_table_foreach(self->lang->blocks,bftv_remove_b_tag,&rts);

	currtable = self->lang->scan_table;
	magic = 0;
	while (gtk_text_iter_compare(&ita, end) <= 0) {	/* main loop */
		 if (gtk_text_iter_equal(&ita, end)) {
			magic++;
			if (magic>=2) break;
		}

		c = gtk_text_iter_get_char(&ita);
		
			/* This is a trick. Character of code 3(ETX) is not printable, so will not appear in the text,
			    but automata has entries for it, so if at the end of text is token - it will 
			    be recognized */
		if (magic==1)	c = 3;
				
		if ((gint) c < 0 || (gint) c > BFTV_UTF8_RANGE) {
			recognizing = FALSE;
			/* currstate = 0; */
			c = 0;
		}

		while (gtk_text_iter_compare(&ita, end) <= 0 && self->lang->escapes[(gint) c]) {	/* remove escapes */
			gtk_text_iter_forward_char(&ita);
			gtk_text_iter_forward_char(&ita);
			if (magic==0) c = gtk_text_iter_get_char(&ita);
		}						/* remove escapes */


/* #####################     RECOGNIZE TOKENS AND BLOCKS ###################*/
		token_found = FALSE;
		block_found = FALSE;


		if (currstate > BFTV_TOKEN_IDS)	currstate = 0;
		if ( self->scanner.state == BFTV_STATE_DONT_SCAN && currstate>0 &&
				self->scanner.maxs>0 && self->scanner.mins>0)
		{
			if ( currstate < self->scanner.mins || currstate > self->scanner.maxs  )
				currstate = 0;
		}
		if (currstate>0 && self->lang->token_allowed_states[currstate])
		{
			guint ss,i;
			gboolean ctxok = FALSE;
			if ( self->lang->token_allowed_states[currstate] == self->scanner.current_context ) ctxok=TRUE;
			ss = g_queue_get_length(&(self->scanner.block_stack));
			for (i = 0; i < ss; i++) {
						bf = (TBfBlock *) g_queue_peek_nth(&(self->scanner.block_stack), i);
						if (bf->def == self->lang->token_allowed_states[currstate] ) {
								ctxok = TRUE;
								break;
						}
			}
			if (!ctxok) currstate = 0;  
		}	
		
		if (self->lang->case_sensitive)
			currstate = currtable[currstate][(gint) c];
		else
			currstate = currtable[currstate][(gint) g_unichar_toupper(c)];
		if (currstate != 0) {
			recognizing = TRUE;
			gint x = abs(currstate);
			if (x > BFTV_TOKEN_IDS && x < BFTV_BLOCK_IDS && self->scanner.state != BFTV_STATE_DONT_SCAN) {	/* token ? */
				token_found = TRUE;
				recognizing = FALSE;
				BfLangToken *t = (BfLangToken *) g_hash_table_lookup(self->lang->tokens, &x);
				gboolean ctxok = FALSE;
				if (t) {
					if (t->context == NULL)
						ctxok = TRUE;
					if (t->spec_type == 0) {
						guint ss = g_queue_get_length(&(self->scanner.block_stack));
						gint i;
						for (i = 0; i < ss; i++) {
							bf = (TBfBlock *) g_queue_peek_nth(&(self->scanner.block_stack), i);
							if (bf->def == t->context) {
								ctxok = TRUE;
								break;
							}
						}
					} else
						ctxok = TRUE;
					if (!ctxok) {	/* last chance - perhaps defined earlier */
						GtkTextMark *m =
							bf_textview_get_nearest_block_of_type(self, t->context, &its, TRUE,
																  BF_GNB_DOCUMENT, TRUE);
						if (m && g_object_get_data(G_OBJECT(m), "_type_") == &tid_block_start)
							ctxok = TRUE;
					}
				}

				if (ctxok) {
					switch (t->spec_type) {
					case 0:
						if (self->mark_tokens) {
#ifdef HL_PROFILING
							bftv_dump_location_info(__LINE__,buf, &its);
							bftv_dump_location_info(__LINE__,buf, &ita);
#endif
							mark = gtk_text_buffer_create_mark(buf, NULL, &its, TRUE);
							mark2 = gtk_text_buffer_create_mark(buf, NULL, &ita, TRUE);
							g_object_set_data(G_OBJECT(mark), "_type_", &tid_token);
							g_object_set_data(G_OBJECT(mark), "info", t);
							g_object_set_data(G_OBJECT(mark), "ref", mark2);
						}
						if (self->highlight /*&& self->token_styles && self->group_styles */ ) {
							if (t->tag)
								gtk_text_buffer_apply_tag(buf, t->tag, &its, &ita);
						} 
						break;
					case 1: /* tag end */
					{
						gchar *txt = gtk_text_buffer_get_text(buf, &its, &ita, FALSE);
						gchar *pc = txt+2;
						gchar **arr = g_strsplit(pc,">",-1);
						gchar **arr2 = g_strsplit(arr[0]," ",-1);						

						if (self->scanner.current_context && !self->scanner.current_context->markup)
							break;						
						/* mark end of tag */
						if (!g_queue_is_empty(&(self->scanner.block_stack2)))	
						{
							/* Have to get tags from stack, because HTML allows not closed tags */
							bf = NULL;
							while (!g_queue_is_empty(&(self->scanner.block_stack2)))
							{
								bf = g_queue_pop_head(&(self->scanner.block_stack2));
								if (bf && strcmp(bf->tagname,arr2[0])==0) break;
								g_free(bf->tagname);
								g_free(bf);
								bf=NULL;
							}
							if (bf) {
								mark = mark2 = NULL;
								GtkTextMark *mark3, *mark4;
								gboolean do_mark = TRUE;

								mark3 = bftv_get_block_at_iter(&bf->b_start);
								if (mark3) {
									if (g_object_get_data(G_OBJECT(mark3), "_type_") ==
										&tid_block_start
										&& strcmp((gchar*)g_object_get_data(G_OBJECT(mark3), "tagname"),arr2[0])==0)
										do_mark = FALSE;
								}
								
								if (do_mark) {

									mark =
										gtk_text_buffer_create_mark(buf, NULL, &bf->b_start, FALSE);
									g_object_set_data(G_OBJECT(mark), "_type_", &tid_block_start);
									g_object_set_data(G_OBJECT(mark), "folded", &tid_false);
									/* BUG: the following line could cause a memory leak !!!!!  - FIXED:freed when deleting block  */
									g_object_set_data(G_OBJECT(mark), "tagname", g_strdup(arr2[0]));
									mark2 =
										gtk_text_buffer_create_mark(buf, NULL, &bf->b_end, FALSE);
									g_object_set_data(G_OBJECT(mark), "ref", mark2);
									mark3 = gtk_text_buffer_create_mark(buf, NULL, &its, FALSE);
									g_object_set_data(G_OBJECT(mark), "ref_e1", mark3);
									mark4 = gtk_text_buffer_create_mark(buf, NULL, &ita, FALSE);
									g_object_set_data(G_OBJECT(mark), "ref_e2", mark4);
									g_object_set_data(G_OBJECT(mark3), "_type_", &tid_block_end);
									g_object_set_data(G_OBJECT(mark3), "folded", &tid_false);
									g_object_set_data(G_OBJECT(mark3), "tagname", g_strdup(arr2[0])); /*BUG: Houston, we have a memory leak here! - FIXED: freed when deleting block */
									g_object_set_data(G_OBJECT(mark3), "ref", mark4);
									g_object_set_data(G_OBJECT(mark3), "ref_b1", mark);
									g_object_set_data(G_OBJECT(mark3), "ref_b2", mark2);
									if (gtk_text_iter_get_line(&bf->b_start) ==
										gtk_text_iter_get_line(&its)) {
										g_object_set_data(G_OBJECT(mark), "single-line", &tid_true);
										g_object_set_data(G_OBJECT(mark3), "single-line",
														  &tid_true);
									} else {
										g_object_set_data(G_OBJECT(mark), "single-line",
														  &tid_false);
										g_object_set_data(G_OBJECT(mark3), "single-line",
														  &tid_false);
									}
								}

								gtk_text_buffer_apply_tag(buf, self->block_tag, &bf->b_end, &its);
								g_free(bf->tagname);
								g_free(bf);
							}
						} /* queue empty */
					   g_strfreev(arr);
					   g_strfreev(arr2);
					   g_free(txt);
						
						if (self->highlight ) {
							/* get this tag from the Tfiletype struct !! */
							tag = self->lang->tag_end;
							if (tag)
								gtk_text_buffer_apply_tag(buf, tag, &its, &ita);
						}
						
						} /* end of case 1 */
						break;
					case 2:
					case 3:
						if (self->scanner.current_context && !self->scanner.current_context->markup)
							break;
						{
							/* !!! attribute found */
							gchar *txt = gtk_text_buffer_get_text(buf, &its, &ita, FALSE);
							gchar **arr = g_strsplit(txt, "=", -1);
							pit = its;
							gtk_text_iter_forward_chars(&pit, g_utf8_strlen(arr[0], -1));
							if (self->mark_tokens) {
								/* need more documentation here, what is 'pit', what is 'ita', what is 'its' ? */
								/* 'its' marks begining of attribute text and 'ita' - end of this. But it decribes whole attribute i.e. 'attr=value' string. */
								/* I need aux variable 'pit' to find where attribute value string begins */
#ifdef HL_PROFILING
								bftv_dump_location_info(__LINE__,buf, &its);
								bftv_dump_location_info(__LINE__,buf, &pit);
								bftv_dump_location_info(__LINE__,buf, &ita);
#endif
								mark = gtk_text_buffer_create_mark(buf, NULL, &its, TRUE);
								mark2 = gtk_text_buffer_create_mark(buf, NULL, &pit, TRUE);
								g_object_set_data(G_OBJECT(mark), "_type_", &tid_tag_attr);
								g_object_set_data(G_OBJECT(mark), "info", t);
								g_object_set_data(G_OBJECT(mark), "ref", mark2);
								mark2 = gtk_text_buffer_create_mark(buf, NULL, &pit, TRUE);
								g_object_set_data(G_OBJECT(mark), "ref_v1", mark2);
								mark2 = gtk_text_buffer_create_mark(buf, NULL, &ita, TRUE);
								g_object_set_data(G_OBJECT(mark), "ref_v2", mark2);
							}
							g_strfreev(arr);
							g_free(txt);
							if (self->highlight /*&& self->tag_styles */ ) {
								/* get from Tfiletype struct */
								tag = self->lang->attr_name;
								if (tag)
									gtk_text_buffer_apply_tag(buf, tag, &its, &pit);
								tag = self->lang->attr_val;
								if (tag)
									gtk_text_buffer_apply_tag(buf, tag, &pit, &ita);

							}
						};
						break;
					case 4:	/* tag begin end in tag context    :) */
						bf = g_queue_pop_head(&(self->scanner.tag_stack));
						if (bf) {
							self->scanner.current_context = NULL;
							gboolean just_ended = FALSE; 				
							TBfBlock *bf_2 = g_new0(TBfBlock, 1); /* BUG: valgrind indicates this is never freed... memory leak? */
							                                      /* I'm still seeing a leak here. */
                                                          /* O. - freed when recognizing end of tag  OR at the end of scanning - I'm cleaning stacks */
							bf_2->tagname = g_strdup(bf->tagname); /* BUG: valgrind indicates this is never freed... memory leak? -  freed later */
							bf_2->b_start = bf->b_start;
							bf_2->b_end = ita;
							g_queue_push_head(&(self->scanner.block_stack2), bf_2);
							pit = ita;
							while (gtk_text_iter_get_char(&pit)!='>') gtk_text_iter_backward_char(&pit);
							gtk_text_iter_backward_char(&pit);
							if (gtk_text_iter_get_char(&pit) == '/') just_ended = TRUE;
							
							if (self->highlight ) {
								/* get tag from Tfiletype struct */
								tag = self->lang->tag_begin;
								if (tag)
									gtk_text_buffer_apply_tag(buf, tag, &bf->b_start, &ita);
							}
							/* TAG autoclose */
							if ( self->tag_autoclose && !self->delete_rescan && 
								  !self->paste_operation && self->tag_ac_state &&
								  !just_ended)
							{
								GtkTextIter it9;
								gtk_text_buffer_get_iter_at_mark(buf,&it9,gtk_text_buffer_get_insert(buf));
								if ( gtk_text_iter_equal(&it9,&ita) )
								{
                           gchar *pp = g_strjoin("","</",bf_2->tagname,">",NULL);
                           self->tag_ac_state = FALSE;
								   gtk_text_buffer_insert(buf,&ita,pp,g_utf8_strlen(pp,-1));
								   gtk_text_buffer_get_iter_at_mark (buf, &it9, gtk_text_buffer_get_insert(buf));
								   gtk_text_iter_backward_chars (&it9, strlen(pp));
								   gtk_text_buffer_place_cursor (buf, &it9);
								   g_free(pp);
								   return; /* I have to return from scan, because it has been performed after latest insert */
								}
							}					
							if (bf->tagname) g_free(bf->tagname);		
							g_free(bf);
						}
						currtable = self->lang->scan_table;	/* TABLE RETURN !!! */
						break;
					case 5:
						/* fake token - do nothing */
						break;
					}			/* switch */
				}
				its = ita;
				currstate = 0;

			} /* token ? */
			else if (x > BFTV_BLOCK_IDS) {	/* block ? */
				block_found = TRUE;
				recognizing = FALSE;
				BfLangBlock *tmp = (BfLangBlock *) g_hash_table_lookup(self->lang->blocks_id, &x);
				if (tmp) {
					switch (tmp->spec_type) {
					case 1:	/* begin of tag */
						if (self->scanner.current_context
							&& (!self->scanner.current_context->markup
								|| !self->scanner.current_context->scanned))
							break;
						if (currstate > 0) {
							/* !!! start of tag begin */
							/* Where and when is the memory from this g_new0() call freed?  - freed in "case 4" in switch above */
							/* Hmm, still see an intermittent leak here. - Nope - it is pushed on the stack, 
							if not taken during recognition, freed at the end of   function - stack cleaning. */
							bf = g_new0(TBfBlock, 1);
							bf->def = tmp;
							bf->b_start = its;
							bf->b_end = ita;
							pit = its;
							gtk_text_iter_forward_char(&pit);
							bf->tagname = gtk_text_buffer_get_text(buf, &pit, &ita, FALSE);
							bf->tagname = g_strstrip(bf->tagname);
							g_queue_push_head(&(self->scanner.tag_stack), bf);
							self->scanner.current_context = tmp;
							currtable = self->lang->tag_scan_table;	/* TABLE CHANGE !!! */
						} 
						break;
					case 0:
						if (currstate > 0 && self->scanner.state != BFTV_STATE_DONT_SCAN) {
							bf = g_new0(TBfBlock, 1);
							bf->def = tmp;
							bf->b_start = its;
							bf->b_end = ita;
							g_queue_push_head(&(self->scanner.block_stack), bf);
							self->scanner.current_context = tmp;
							if (!tmp->scanned) {							
								self->scanner.state = BFTV_STATE_DONT_SCAN;
								self->scanner.mins = tmp->min_state;
								self->scanner.maxs = tmp->max_state;
							}	
							else {
								self->scanner.state = BFTV_STATE_NORMAL;
								self->scanner.mins = 0;
								self->scanner.maxs = 0;								
							}	
							
						} else {
							if (g_queue_is_empty(&(self->scanner.block_stack)))	break;
							
							bf = g_queue_peek_head(&(self->scanner.block_stack));
							if (bf && bf->def == tmp) {
								g_queue_pop_head(&(self->scanner.block_stack));
								mark = mark2 = NULL;
								GtkTextMark *mark3, *mark4;
								gboolean do_mark = TRUE;

								mark3 = bftv_get_block_at_iter(&bf->b_start);
								if (mark3) {
									if (g_object_get_data(G_OBJECT(mark3), "_type_") ==
										&tid_block_start
										&& g_object_get_data(G_OBJECT(mark3), "info") == tmp)
										do_mark = FALSE;
								}

								if (do_mark) {
#ifdef HL_PROFILING
									bftv_dump_location_info(__LINE__,buf, &bf->b_start);
									bftv_dump_location_info(__LINE__,buf, &bf->b_end);
									bftv_dump_location_info(__LINE__,buf, &its);
									bftv_dump_location_info(__LINE__,buf, &ita);
#endif

									mark =
										gtk_text_buffer_create_mark(buf, NULL, &bf->b_start, FALSE);
									g_object_set_data(G_OBJECT(mark), "_type_", &tid_block_start);
									g_object_set_data(G_OBJECT(mark), "folded", &tid_false);
									g_object_set_data(G_OBJECT(mark), "info", tmp);
									mark2 =
										gtk_text_buffer_create_mark(buf, NULL, &bf->b_end, FALSE);
									g_object_set_data(G_OBJECT(mark), "ref", mark2);
									mark3 = gtk_text_buffer_create_mark(buf, NULL, &its, FALSE);
									g_object_set_data(G_OBJECT(mark), "ref_e1", mark3);
									mark4 = gtk_text_buffer_create_mark(buf, NULL, &ita, FALSE);
									g_object_set_data(G_OBJECT(mark), "ref_e2", mark4);
									g_object_set_data(G_OBJECT(mark3), "_type_", &tid_block_end);
									g_object_set_data(G_OBJECT(mark3), "folded", &tid_false);
									g_object_set_data(G_OBJECT(mark3), "info", tmp);
									g_object_set_data(G_OBJECT(mark3), "ref", mark4);
									g_object_set_data(G_OBJECT(mark3), "ref_b1", mark);
									g_object_set_data(G_OBJECT(mark3), "ref_b2", mark2);
									if (gtk_text_iter_get_line(&bf->b_start) ==
										gtk_text_iter_get_line(&its)) {
										g_object_set_data(G_OBJECT(mark), "single-line", &tid_true);
										g_object_set_data(G_OBJECT(mark3), "single-line",
														  &tid_true);
									} else {
										g_object_set_data(G_OBJECT(mark), "single-line",
														  &tid_false);
										g_object_set_data(G_OBJECT(mark3), "single-line",
														  &tid_false);
									}
								}

								gtk_text_buffer_apply_tag(buf, self->block_tag, &bf->b_end, &its);

								if (self->
									highlight /*&& self->block_styles  && self->group_styles */ ) {
									if (tmp->tag)
										gtk_text_buffer_apply_tag(buf, tmp->tag, &bf->b_start,
																  &ita);
								}


								if (g_queue_is_empty(&(self->scanner.block_stack))) {
									self->scanner.current_context = NULL;
									self->scanner.state = BFTV_STATE_NORMAL;
									self->scanner.mins = 0;
									self->scanner.maxs = 0;
								} else {
									self->scanner.current_context =
										((TBfBlock *)
										 g_queue_peek_head(&(self->scanner.block_stack)))->def;
									if (!self->scanner.current_context->scanned) {
										self->scanner.state = BFTV_STATE_DONT_SCAN;
										self->scanner.mins = self->scanner.current_context->min_state;
										self->scanner.maxs = self->scanner.current_context->max_state;																				
									}	
									else {
										self->scanner.state = BFTV_STATE_NORMAL;
										self->scanner.mins = 0;
										self->scanner.maxs = 0;										
									}	
								}
								g_free(bf);
							}	/* bf */
						}
						break;
					}			/* switch */
				}
				its = ita;
				currstate = 0;

			}

			/* block? */
		} /* currstate != 0 */
		else {
			its = ita;
			gtk_text_iter_forward_char(&its);
			/* move back because we didn't manage to recognize anything */
			if (recognizing) {
				gtk_text_iter_backward_char(&its);
				gtk_text_iter_backward_char(&ita);
				recognizing = FALSE;
			}

		}
/* #####################     END OF RECOGNIZE TOKENS AND BLOCKS ###################*/
		if (!token_found && !block_found)
			gtk_text_iter_forward_char(&ita);
	}							/* main loop */

/* Clear stacks */
while (!g_queue_is_empty(&self->scanner.block_stack))
{
	bf = (TBfBlock*)g_queue_pop_head(&self->scanner.block_stack);
	if (bf->tagname) g_free(bf->tagname);
	g_free(bf);
}
while (!g_queue_is_empty(&self->scanner.block_stack2))
{
	bf = (TBfBlock*)g_queue_pop_head(&self->scanner.block_stack2);
	if (bf->tagname) g_free(bf->tagname);
	g_free(bf);
}


#ifdef HL_PROFILING
	{
		glong tot_ms=0, marks=0,allmarks=0,tags=0;
		GSList *ss = NULL;
		times(&tms2);
		tot_ms = (glong) (double) ((tms2.tms_utime - tms1.tms_utime) * 1000 / sysconf(_SC_CLK_TCK));
		g_print("PROFILING: total time %ld ms\n", tot_ms);
		gtk_text_buffer_get_bounds(buf,&its,&ita);	
		pit = its;
		while ( gtk_text_iter_compare(&pit,&ita)<0 ) {
			ss =   gtk_text_iter_get_marks(&pit);
			while ( ss ) {
				allmarks++;
				if (!gtk_text_mark_get_deleted(ss->data)) {
					marks++;
				}
				ss = g_slist_next(ss); 
			}
			g_slist_free(ss);
			ss = gtk_text_iter_get_toggled_tags(&pit,TRUE);
			while ( ss ) {
				tags++;
				ss = g_slist_next(ss); 
			}
			g_slist_free(ss);
			gtk_text_iter_forward_char(&pit);
		}
		g_print("Total number of existing marks: %ld, all marks: %ld total number of tags: %ld\n",marks,allmarks,tags);
	}
#endif
}

/**
*	bf_textview_scan_visible:
*	@self:  BfTextView widget 
*
*	Scan buffer in visible window. 
*/
void bf_textview_scan_visible(BfTextView * self)
{
	GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self));
	GtkTextIter its, ite;
	GtkTextMark *mark = NULL;
	gpointer ptr;
	GdkRectangle rect;
	GtkTextIter l_start, l_end;

	g_return_if_fail(self != NULL);
	g_return_if_fail(BF_IS_TEXTVIEW(self));
	g_return_if_fail(buf != NULL);

	gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(self), &rect);
	gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(self), &l_start, rect.y, NULL);
	gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(self), &l_end, rect.y + rect.height, NULL);
	its = l_start;
	ite = l_end;
	gtk_text_iter_forward_to_line_end(&ite);

	if (self->lang->scan_blocks) {
		mark = bftv_get_first_block_at_line(self,&its, TRUE);
		if (mark && g_object_get_data(G_OBJECT(mark), "_type_") == &tid_block_end) {
			ptr = g_object_get_data(G_OBJECT(mark), "ref_b1");
			if (ptr)
				gtk_text_buffer_get_iter_at_mark(buf, &its, GTK_TEXT_MARK(ptr));
		}
		mark = bftv_get_last_block_at_line(self,&ite);
		if (mark && g_object_get_data(G_OBJECT(mark), "_type_") == &tid_block_start) {
			ptr = g_object_get_data(G_OBJECT(mark), "ref_e2");
			if (ptr)
				gtk_text_buffer_get_iter_at_mark(buf, &ite, GTK_TEXT_MARK(ptr));
		}
	}

	bf_textview_scan_area(self, &its, &ite);
}


/*
 * ----------------------- LANGUAGE CONFIGURATION -------------------- 
 */

static gboolean bftv_xml_bool(xmlChar * text)
{
	if (text == NULL)
		return FALSE;
	if (xmlStrcmp(text, (const xmlChar *) "true") == 0
		|| xmlStrcmp(text, (const xmlChar *) "TRUE") == 0
		|| xmlStrcmp(text, (const xmlChar *) "1") == 0) {
		return TRUE;
	}
	return FALSE;
}


#define BFTV_DFA_TYPE_TOKEN						0
#define BFTV_DFA_TYPE_BLOCK_BEGIN		1
#define BFTV_DFA_TYPE_BLOCK_END			2

/* 
what does this function do Oskar?
what does 'dfa' stand for?

***** Hmm, this function should not be called during editing !
 It is used to build Determinstic  Finite Automata for scanner (DFA) and this should be performed only 
 when language configuration is loaded and scanner table prepared.
 Can be time consuming regarding that I'm doing two things in one place:
  We are using regular expressions in config files. The only automata you can build for that is Non-deterministic (NFA),but
   then you can transform NFA into DFA using for example algorithm included in Aho,Sethi,Ullman dragon book (about compilers)
   (geee never really managed to analyze it precisely.... :-) ).
   I'm doing everything at once here.
 *********
 

it seems this function takes quite some cpu time, I just did some 
profiling (30-okt-2005) with much scrolling through documents and 
this function took 63% of the cpu time..

The pointer gshort *z is allocating memory with g_malloc0 in 
two different places in this function. This appears to be a 
large memory leak. Is this freed somewhere else?
These leaks are still present.

Hmm, the problem is that there is no code cleaning scanner tables, z is allocating memory for that tables, 
because their size is unknown until I build DFA.
I'll think about it, this cleanup should be performed at the very end of Bluefish.
*/
static void bftv_put_into_dfa(GArray * dfa, BfLangConfig * cfg, gpointer data, gint type,
							  gboolean tag)
{
	gint i, j, k, m, s;
	guchar *ptr = NULL;
	gunichar *inp, *inp2, *inp3;
	glong size;
	BfLangToken *token = NULL;
	BfLangBlock *block = NULL;
	gboolean regexp = FALSE, reverse_set = FALSE;
	gshort *x, y, *z;
	gint currstate = 0;
	gboolean charset[BFTV_UTF8_RANGE];
	gboolean states[10000], pstates[10000];
	gboolean done = FALSE;
	gint counter = 0, mins=0,maxs=0;

	switch (type) {
	case BFTV_DFA_TYPE_TOKEN:
		token = (BfLangToken *) data;
		token->tabid = cfg->tokennum;
		cfg->tokennum++;
		regexp = token->regexp;
		ptr = token->text;
		mins=maxs=cfg->tabnum;
		break;
	case BFTV_DFA_TYPE_BLOCK_BEGIN:
		block = (BfLangBlock *) data;
		block->tabid = cfg->blocknum;
		regexp = block->regexp;
		ptr = block->begin;
		break;
	case BFTV_DFA_TYPE_BLOCK_END:
		block = (BfLangBlock *) data;
		block->tabid = cfg->blocknum;
		regexp = block->regexp;
		cfg->blocknum++;
		ptr = block->end;
		mins=maxs=cfg->tabnum;
		break;
	}

	if (!ptr)
		return;
	if (!xmlCheckUTF8(ptr))
		return;

	for (m = 0; m < 10000; m++) {
		states[m] = FALSE;
		pstates[m] = FALSE;
	}
	pstates[0] = TRUE;

	if (cfg->case_sensitive || (token && token->spec_type == 5)) {
		inp2 = g_utf8_to_ucs4_fast(ptr, -1, &size);
	} else {
	    gchar *tmp = g_utf8_strup (ptr, -1);
		inp2 = g_utf8_to_ucs4_fast (tmp, -1, &size);
		g_free (tmp);
	}
	inp = inp2;

	if (!regexp) {
		if (xmlUTF8Strlen(ptr) > cfg->counter)
			cfg->counter = xmlUTF8Strlen(ptr);
	}

	i = 0;
	j = 0;
	while (i < size) {
		if (!regexp) {
			x = g_array_index(dfa, gshort *, currstate);
			y = x[(gshort) * inp];
			if (y == 0 || y > BFTV_TOKEN_IDS) {	/* also rewrite shorter token */
				if (tag)
					cfg->tag_tabnum++;
				else
					cfg->tabnum++;
				z = (gshort *) g_malloc0(BFTV_UTF8_RANGE * sizeof(gshort));
				g_array_append_val(dfa, z);
				if (tag) {
					x[(gshort) * inp] = cfg->tag_tabnum;
					currstate = cfg->tag_tabnum;
				} else {
					x[(gshort) * inp] = cfg->tabnum;
					currstate = cfg->tabnum;
				}
			} else {
				currstate = y;
			}
		} /* if !regexp */
		else {					/* regexp */
			reverse_set = FALSE;
			for (m = 0; m < BFTV_UTF8_RANGE; m++)
				charset[m] = FALSE;

			if (*inp == '[') {
				k = i + 1;
				inp3 = inp + 1;
				if (*inp3 == '^') {
					reverse_set = TRUE;
					inp3++;
					k++;
				}
				while (k < size && *inp3 != ']') {
					if (g_unichar_isalnum(*inp3) && *(inp3 + 1) == '-' && g_unichar_isalnum(*(inp3 + 2))) {	/* range */
						for (m = (gint) (*inp3); m <= (gint) (*(inp3 + 2)); m++)
							charset[m] = TRUE;
						inp3 = inp3 + 2;
						k += 2;
					} else {	/* char */

						if (*inp3 == '\\') {
							inp3++;
							k++;
						}
						charset[(gint) * inp3] = TRUE;
					}
					inp3++;
					k++;
				}				/* while k<size */
				i = k;
				inp = inp3;
				if (reverse_set) {
					for (m = 0; m < BFTV_UTF8_RANGE; m++)
						charset[m] = !charset[m];
				}
			} /* *inp == '[' */
			else {
				charset[(gint) * inp] = TRUE;
			}

			/* charset determined, now create states */

			z = (gshort *) g_malloc0(BFTV_UTF8_RANGE * sizeof(gshort));
			done = FALSE;

			for (k = 0; k < 10000; k++)
				states[k] = FALSE;

			if (i < size - 1 && (*(inp + 1) == '?' || *(inp + 1) == '*')) {
				if (tag)
					s = cfg->tag_tabnum;
				else
					s = cfg->tabnum;
				for (m = 0; m < s + 1; m++)
					if (pstates[m])
						states[m] = TRUE;
			}

			if (tag)
				s = cfg->tag_tabnum;
			else
				s = cfg->tabnum;

			for (k = 0; k < s + 1; k++)
				if (pstates[k]) {
					x = g_array_index(dfa, gshort *, k);
					if (x) {
						for (m = 0; m < BFTV_UTF8_RANGE; m++)
							if (charset[m]) {
								if (x[m] == 0 || x[m] > BFTV_TOKEN_IDS) {	/* rewrite longer token */
									if (!done) {
										if (tag)
											cfg->tag_tabnum++;
										else
											cfg->tabnum++;
										g_array_append_val(dfa, z);
										done = TRUE;
									}
									if (tag) {
										x[m] = cfg->tag_tabnum;
										states[cfg->tag_tabnum] = TRUE;
									} else {
										x[m] = cfg->tabnum;
										states[cfg->tabnum] = TRUE;
									}
								} else {
									states[x[m]] = TRUE;
								}
							}	/* charset */
					}
				}



			if (i < size - 1 && (*(inp + 1) == '*' || *(inp + 1) == '+'))
				for (m = 0; m < BFTV_UTF8_RANGE; m++)
					if (charset[m]) {
						if (tag)
							z[m] = cfg->tag_tabnum;
						else
							z[m] = cfg->tabnum;
					}


			if (tag)
				s = cfg->tag_tabnum;
			else
				s = cfg->tabnum;


			if (i == size - 1 || (i == size - 2 && *(inp + 1) == '+')
				|| (i == size - 2 && *(inp + 1) == '*') || (i == size - 2 && *(inp + 1) == '?')) {
				for (k = 0; k < s + 1; k++)
					if (states[k]) {
						x = g_array_index(dfa, gshort *, k);
						for (m = 0; m < BFTV_UTF8_RANGE; m++)
							if (x[m] == 0 /*|| x[m]>BFTV_TOKEN_IDS */ ) {
								switch (type) {
								case BFTV_DFA_TYPE_TOKEN:
									x[m] = token->tabid;
									break;
								case BFTV_DFA_TYPE_BLOCK_BEGIN:
									x[m] = block->tabid;
									break;
								case BFTV_DFA_TYPE_BLOCK_END:
									x[m] = -block->tabid;
									break;
								}
							}
					}			/* if states[k] */
			}

			if (i < size - 1 && (*(inp + 1) == '?' || *(inp + 1) == '*' || *(inp + 1) == '+')) {
				inp++;
				i++;
			}

			for (k = 0; k < 10000; k++) {
				pstates[k] = states[k];
			}


		}						/* regexp */

		if (regexp)
			counter++;
		if (i < size)
			inp++;
		i++;
		j++;

	}							/* while i < size */

	if (regexp) {
		if (counter > cfg->counter)
			cfg->counter = counter;
	}

	if (!regexp) {
		x = g_array_index(dfa, gshort *, currstate);
		for (m = 0; m < BFTV_UTF8_RANGE; m++)
			if (x[m] == 0 /*|| x[m]>BFTV_TOKEN_IDS */ ) {
				switch (type) {
				case BFTV_DFA_TYPE_TOKEN:
					x[m] = token->tabid;
					break;
				case BFTV_DFA_TYPE_BLOCK_BEGIN:
					x[m] = block->tabid;
					break;
				case BFTV_DFA_TYPE_BLOCK_END:
					x[m] = -block->tabid;
					break;
				}

			}
	}

	g_free(inp2);
   if ( type == BFTV_DFA_TYPE_BLOCK_END )
   {
   			maxs=cfg->tabnum;
   			block->min_state = mins;
   			block->max_state = maxs;
   } else if ( type == BFTV_DFA_TYPE_TOKEN)
   {
     			maxs=cfg->tabnum;
   			token->min_state = mins;
   			token->max_state = maxs;
   }


}


static gshort **bftv_make_scan_table(GArray * dfa, BfLangConfig * cfg)
{
	gshort **table;
	gint i/*, j*/;
/*  glong cnt = 0;*/
	gint size = cfg->tabnum + 1;

	table = g_new0(gshort *, size);
	for (i = 0; i < size; i++) {
		table[i] = g_array_index(dfa, gshort *, i);
	}



/*  g_print ("Token table size: %d\n", size  * BFTV_UTF8_RANGE * sizeof (gshort));
  for (i = 0; i < size; i++)
    for (j = 0; j < BFTV_UTF8_RANGE; j++)
		if (table[i][j] != 0)  cnt++;
  g_print ("Number of states: %d\n", size);
  g_print ("Size of efective entries: %ld\n", cnt * sizeof (gshort));*/
	return table;
}

static gshort **bftv_make_tag_scan_table(GArray * dfa, BfLangConfig * cfg)
{
	gshort **table;
	gint i /*, j */ ;
/*  gshort cnt = 0;*/
	gint size = cfg->tag_tabnum + 1;

	table = g_new0(gshort *, size);
	for (i = 0; i < size; i++) {
		table[i] = g_array_index(dfa, gshort *, i);
	}


/*  g_print ("Tag table size: %d\n", size  * BFTV_UTF8_RANGE * sizeof (gshort));
  for (i = 0; i < size; i++)
    for (j = 0; j < BFTV_UTF8_RANGE; j++)
		if (table[i][j] != 0)  cnt++;
  g_print ("Number of states: %d\n", size);
  g_print ("Size of efective entries: %d\n", cnt * sizeof (gshort));*/
	return table;
}



static gpointer bftv_make_entity(xmlDocPtr doc, xmlNodePtr node, BfLangConfig * cfg, gint type,
								 guchar * group, const guchar *groupstyle, guchar * text)
{
	xmlChar *tmps, *tmps2;
	gpointer ptr;

	switch (type) {
	case BFTV_DFA_TYPE_TOKEN:
		if (text == NULL)
			tmps = xmlNodeListGetString(doc, node->xmlChildrenNode, 1);
		else
			tmps = text;
		BfLangToken *t = g_new0(BfLangToken, 1);
		t->group = group;
		tmps2 = xmlGetProp(node, (const xmlChar *) "regexp");
		t->regexp = bftv_xml_bool(tmps2);
		if (tmps2)
			xmlFree(tmps2);
		tmps2 = xmlGetProp(node, (const xmlChar *) "name");
		if (tmps2 && text == NULL) {
			t->name = tmps2;
		} else {
			t->name = tmps;
			if (tmps2) xmlFree(tmps2);
		}
		t->text = tmps;
		tmps2 = xmlGetProp(node, (const xmlChar *) "context");
		if (tmps2) {
			ptr = g_hash_table_lookup(cfg->blocks, tmps2);
			if (!ptr)
				g_warning("Token (%s) context defined as %s but such a block does not exists.",
						  t->text, tmps2);
			t->context = (BfLangBlock *) ptr;
			xmlFree(tmps2);
		} else
			t->context = NULL;
		
		tmps2 = xmlGetProp(node, (const xmlChar *) "defaultstyle");
		t->tag = get_tag_for_scanner_style((gchar *) cfg->name, "t", (gchar *) t->name, (gchar *) tmps2);
		if (!t->tag) {
			t->tag = get_tag_for_scanner_style((gchar *) cfg->name, "g", (gchar *) t->group, (gchar *) groupstyle);
		}
		if (tmps2) xmlFree(tmps2);
		
		bftv_put_into_dfa(cfg->dfa, cfg, t, BFTV_DFA_TYPE_TOKEN, FALSE);
		g_hash_table_insert(cfg->tokens, &t->tabid, t);
		return t;
		break;
	case BFTV_DFA_TYPE_BLOCK_BEGIN:
		tmps = xmlGetProp(node, (const xmlChar *) "id");
		if (tmps) {
			BfLangBlock *b = g_new0(BfLangBlock, 1);
			b->id = tmps;
			b->group = group;
			b->begin = xmlGetProp(node, (const xmlChar *) "begin");
			b->end = xmlGetProp(node, (const xmlChar *) "end");
			tmps2 = xmlGetProp(node, (const xmlChar *) "scanned");
			b->scanned = bftv_xml_bool(tmps2);
			if (tmps2)
				xmlFree(tmps2);
			tmps2 = xmlGetProp(node, (const xmlChar *) "markup");
			b->markup = bftv_xml_bool(tmps2);
			if (tmps2)
				xmlFree(tmps2);
			tmps2 = xmlGetProp(node, (const xmlChar *) "foldable");
			b->foldable = bftv_xml_bool(tmps2);
			if (tmps2)
				xmlFree(tmps2);
			tmps2 = xmlGetProp(node, (const xmlChar *) "case");
			b->case_sensitive = bftv_xml_bool(tmps2);
			if (tmps2)
				xmlFree(tmps2);
			tmps2 = xmlGetProp(node, (const xmlChar *) "regexp");
			b->regexp = bftv_xml_bool(tmps2);
			if (tmps2)
				xmlFree(tmps2);

			/* try to retrieve the tag based on the name of the block, if no tag is found, 
			   try to retrieve a tag for the group name */
			tmps2 = xmlGetProp(node, (const xmlChar *) "defaultstyle");
			b->tag = get_tag_for_scanner_style((gchar *) cfg->name, "b", (gchar *) b->id, (gchar *) tmps2);
			if (!b->tag) {
				b->tag = get_tag_for_scanner_style((gchar *) cfg->name, "g", (gchar *) b->group, (gchar *) groupstyle);
			}
			if (tmps2) xmlFree(tmps2);
			
			g_hash_table_insert(cfg->blocks, tmps, b);
			bftv_put_into_dfa(cfg->dfa, cfg, b, BFTV_DFA_TYPE_BLOCK_BEGIN, FALSE);
			bftv_put_into_dfa(cfg->dfa, cfg, b, BFTV_DFA_TYPE_BLOCK_END, FALSE);
			g_hash_table_insert(cfg->blocks_id, &b->tabid, b);
			return b;
		}
		break;
	}
	return NULL;
}

static void bftv_fill_token_states(gpointer key,gpointer value,gpointer data)
{
	BfLangConfig *cfg = (BfLangConfig*)data;
	BfLangToken *token = (BfLangToken*)value;
	gint i;
	for(i=token->min_state;i<=token->max_state;i++)
		cfg->token_allowed_states[i] = token->context;
}

static BfLangConfig *bftv_load_config(gchar * filename, const gchar * filetype_name)
{
	xmlDocPtr doc;
	xmlNodePtr cur, cur2;
	xmlChar *tmps, *tmps2 = NULL, *tmps3 = NULL, *tmps4;
	BfLangConfig *cfg = NULL;
	gint i;
	gshort *x;

	g_return_val_if_fail(filename != NULL, (BfLangConfig *) 0);
	xmlLineNumbersDefault(1);
	doc = xmlReadFile(filename, "UTF-8", XML_PARSE_RECOVER | XML_PARSE_NOENT);
	cur = xmlDocGetRootElement(doc);
	if (xmlStrcmp(cur->name, (const xmlChar *) "bflang") == 0) {
		cfg = g_new0(BfLangConfig, 1);
		cfg->name = xmlGetProp(cur, (const xmlChar *) "name");
		cfg->description = xmlGetProp(cur, (const xmlChar *) "description");
		cfg->blocks = g_hash_table_new(g_str_hash, g_str_equal);
		cfg->blocks_id = g_hash_table_new(g_int_hash, g_int_equal);
		cfg->tokens = g_hash_table_new(g_int_hash, g_int_equal);
		cfg->groups = g_hash_table_new(g_str_hash, g_str_equal);

		cfg->tag_begin = get_tag_for_scanner_style((gchar *) cfg->name, "m", "tag_begin", "tag begin");
		cfg->tag_end = get_tag_for_scanner_style((gchar *) cfg->name, "m", "tag_end", "tag end");
		cfg->attr_name = get_tag_for_scanner_style((gchar *) cfg->name, "m", "attr_name", "attribute");
		cfg->attr_val = get_tag_for_scanner_style((gchar *) cfg->name, "m", "attr_val", "attribute value");

		cfg->restricted_tags = g_hash_table_new(g_int_hash, g_int_equal);
		cfg->line_indent = g_array_new(TRUE, TRUE, sizeof(gint));

		cfg->dfa = g_array_new(FALSE, TRUE, sizeof(gshort *));
		x = (gshort *) g_malloc0(BFTV_UTF8_RANGE * sizeof(gshort));
		g_array_append_val(cfg->dfa, x);
		cfg->tabnum = 0;

		cfg->tag_dfa = g_array_new(FALSE, TRUE, sizeof(gshort *));
		x = (gshort *) g_malloc0(BFTV_UTF8_RANGE * sizeof(gshort));
		g_array_append_val(cfg->tag_dfa, x);
		cfg->tag_tabnum = 0;

		cfg->counter = 0;

		cfg->tokennum = BFTV_TOKEN_IDS + 1;
		cfg->blocknum = BFTV_BLOCK_IDS + 1;
		for (i = 0; i < BFTV_UTF8_RANGE; i++) {
			cfg->as_triggers[i] = 0;
			cfg->escapes[i] = 0;
		}
		cur = cur->xmlChildrenNode;
		while (cur != NULL) {
			if (xmlStrcmp(cur->name, (const xmlChar *) "options") == 0) {
				cur2 = cur->xmlChildrenNode;
				while (cur2 != NULL) {
					if (xmlStrcmp(cur2->name, (const xmlChar *) "option") == 0) {
						tmps = xmlGetProp(cur2, (const xmlChar *) "name");
						tmps2 = xmlNodeListGetString(doc, cur2->xmlChildrenNode, 1);
						if (tmps) {
							if (xmlStrcmp(tmps, (const xmlChar *) "case-sensitive") == 0)
								cfg->case_sensitive = bftv_xml_bool(tmps2);
							else if (xmlStrcmp(tmps, (const xmlChar *) "scan-markup-tags") == 0)
								cfg->scan_tags = bftv_xml_bool(tmps2);
							else if (xmlStrcmp(tmps, (const xmlChar *) "scan-blocks") == 0)
								cfg->scan_blocks = bftv_xml_bool(tmps2);
							else if (xmlStrcmp(tmps, (const xmlChar *) "restricted-tags-only") == 0)
								cfg->restricted_tags_only = bftv_xml_bool(tmps2);
							else if (xmlStrcmp(tmps, (const xmlChar *) "indent-blocks") == 0)
								cfg->indent_blocks = bftv_xml_bool(tmps2);
							else if (xmlStrcmp(tmps, (const xmlChar *) "extensions") == 0)
								cfg->extensions = xmlStrdup(tmps2);
							else if (xmlStrcmp(tmps, (const xmlChar *) "auto-scan-triggers") == 0) {
								const guchar *p = tmps2;
								i = 0;
								while (i < xmlUTF8Strlen(tmps2)) {
									cfg->as_triggers[(gint) * p] = 1;
									p = xmlUTF8Strpos(tmps2, i++);
								}
							} else if (xmlStrcmp(tmps, (const xmlChar *) "escape-characters") == 0) {
								const guchar *p = tmps2;
								i = 0;
								while (i < xmlUTF8Strlen(tmps2)) {
									cfg->escapes[(gint) * p] = 1;
									p = xmlUTF8Strpos(tmps2, i++);
								}
							}
							if (tmps)
								xmlFree(tmps);
						}
						if (tmps2)
							xmlFree(tmps2);
					}
					cur2 = cur2->next;
				}				/* end of cur2 */
			} /* options */
			else if (xmlStrcmp(cur->name, (const xmlChar *) "block-group") == 0) {	/* blocks  */
				tmps3 = xmlGetProp(cur, (const xmlChar *) "id");
				tmps4 = xmlGetProp(cur, (const xmlChar *) "defaultstyle");
				g_hash_table_insert(cfg->groups, xmlStrdup(tmps3), "b");
				cur2 = cur->xmlChildrenNode;
				while (cur2 != NULL) {
					if (xmlStrcmp(cur2->name, (const xmlChar *) "block") == 0) {
						bftv_make_entity(doc, cur2, cfg, BFTV_DFA_TYPE_BLOCK_BEGIN, tmps3, tmps4, NULL);
					}
					cur2 = cur2->next;
				}				/* while */
				xmlFree(tmps4);
			} else if (xmlStrcmp(cur->name, (const xmlChar *) "token-group") == 0) {	/* tokens  */
				tmps3 = xmlGetProp(cur, (const xmlChar *) "id");
				tmps4 = xmlGetProp(cur, (const xmlChar *) "defaultstyle");
				g_hash_table_insert(cfg->groups, xmlStrdup(tmps3), "t");
				cur2 = cur->xmlChildrenNode;
				while (cur2 != NULL) {
					if (xmlStrcmp(cur2->name, (const xmlChar *) "token") == 0) {
						bftv_make_entity(doc, cur2, cfg, BFTV_DFA_TYPE_TOKEN, tmps3, tmps4, NULL);
					} else if (xmlStrcmp(cur2->name, (const xmlChar *) "token-list") == 0) {
						tmps2 = xmlGetProp(cur2, (const xmlChar *) "separator");
						if (tmps2) {
							tmps = xmlNodeListGetString(doc, cur2->xmlChildrenNode, 1);
							gchar **arr = g_strsplit(tmps, tmps2, -1);
							xmlFree(tmps2);
							if (arr) {
								gint i = 0;
								while (arr[i] != NULL) {
									bftv_make_entity(doc, cur2, cfg, BFTV_DFA_TYPE_TOKEN, tmps3,tmps4,
													 g_strdup(arr[i]));
									i++;
								}	/* while */
								g_strfreev(arr);
							}
							if (tmps)
								xmlFree(tmps);
						}
					}			/* token-list */
					cur2 = cur2->next;
				}				/* while cur2 */
				xmlFree(tmps4);
			} /* token group */
			else if (xmlStrcmp(cur->name, (const xmlChar *) "block") == 0) {	/* block  * without  * a  * group  */
				bftv_make_entity(doc, cur, cfg, BFTV_DFA_TYPE_BLOCK_BEGIN, NULL, NULL, NULL);
			} /* block without a group */
			else if (xmlStrcmp(cur->name, (const xmlChar *) "token") == 0) {
				bftv_make_entity(doc, cur, cfg, BFTV_DFA_TYPE_TOKEN, NULL, NULL, NULL);
			} /* token without a group */
			else if (xmlStrcmp(cur->name, (const xmlChar *) "token-list") == 0) {	/* token  * list  * without  * a  * group  */
				tmps2 = xmlGetProp(cur, (const xmlChar *) "separator");
				if (tmps2) {
					tmps = xmlNodeListGetString(doc, cur->xmlChildrenNode, 1);
					gchar **arr = g_strsplit(tmps, tmps2, -1);
					xmlFree(tmps2);
					if (arr) {
						gint i = 0;
						while (arr[i] != NULL) {
							bftv_make_entity(doc, cur, cfg, BFTV_DFA_TYPE_TOKEN, NULL, NULL,
											 g_strdup(arr[i]));
							i++;
						}
						g_strfreev(arr);
					}
					if (tmps)
						xmlFree(tmps);
				}
			} /* token-list without a group */
			else if (xmlStrcmp(cur->name, (const xmlChar *) "restricted-tags") == 0) {	/* restricted tags */
				cur2 = cur->xmlChildrenNode;
				while (cur2 != NULL) {
					if (xmlStrcmp(cur2->name, (const xmlChar *) "tag") == 0) {
						tmps = xmlGetProp(cur2, (const xmlChar *) "name");
						if (tmps) {
							GHashTable *h = g_hash_table_new(g_str_hash, g_str_equal);
							gchar **arr;
							gint i = 0;
							g_hash_table_insert(cfg->restricted_tags, xmlStrdup(tmps), h);
							xmlFree(tmps);
							tmps = xmlGetProp(cur2, (const xmlChar *) "attributes");
							arr = g_strsplit(tmps, ",", -1);
							while (arr[i] != NULL) {
								g_hash_table_insert(h, g_strdup(arr[i]), "1");
								i++;
							}
							xmlFree(tmps);
							g_strfreev(arr);
						}
					}
					cur2 = cur2->next;
				}				/* while cur2 */
			}

			/*
			 * restricted tags 
			 */
			cur = cur->next;
		}						/* while cur */
	}
	if (cfg->indent_blocks) {
		cfg->iblock = g_new0(BfLangBlock, 1);
		cfg->iblock->id = (guchar *) "Indent block";
		cfg->iblock->begin = cfg->iblock->end = xmlCharStrdup(" ");
		cfg->iblock->group = NULL;
		cfg->iblock->tabid = cfg->blocknum;
		cfg->blocknum++;
		g_hash_table_insert(cfg->blocks, cfg->iblock->id, cfg->iblock);
		g_hash_table_insert(cfg->blocks_id, &cfg->iblock->tabid, cfg->iblock);
	}

	/* fill scan table for tags */
	if (cfg->scan_tags) {
		BfLangToken *t = g_new0(BfLangToken, 1);
		t->group = NULL;
		t->regexp = TRUE;
		t->name = xmlCharStrdup("_tag_end_");
		t->text = xmlCharStrdup("</[a-zA-Z][a-zA-Z0-9]*[ ]*>");
		t->context = NULL;
		t->spec_type = 1;
		bftv_put_into_dfa(cfg->dfa, cfg, t, BFTV_DFA_TYPE_TOKEN, FALSE);
		g_hash_table_insert(cfg->tokens, &t->tabid, t);

		BfLangBlock *b = g_new0(BfLangBlock, 1);
		b->id = xmlCharStrdup("_tag_begin_");
		b->group = NULL;
		b->regexp = TRUE;
		b->begin = xmlCharStrdup("<[a-zA-Z][a-zA-Z0-9]*[ ]*");
		b->end = xmlCharStrdup("/?>");
		b->scanned = TRUE;
		b->foldable = FALSE;
		b->case_sensitive = FALSE;
		b->spec_type = 1;
		b->markup = TRUE;
		g_hash_table_insert(cfg->blocks, b->id, b);
		bftv_put_into_dfa(cfg->dfa, cfg, b, BFTV_DFA_TYPE_BLOCK_BEGIN, FALSE);
		bftv_put_into_dfa(cfg->dfa, cfg, b, BFTV_DFA_TYPE_BLOCK_END, FALSE);
		g_hash_table_insert(cfg->blocks_id, &b->tabid, b);

		t = g_new0(BfLangToken, 1);
		t->group = NULL;
		t->regexp = TRUE;
		t->name = xmlCharStrdup("_attr2_");
		t->spec_type = 3;
		t->text = xmlCharStrdup("[a-zA-Z-]+=\"[^\"]*\"");
		t->context = b;
		bftv_put_into_dfa(cfg->tag_dfa, cfg, t, BFTV_DFA_TYPE_TOKEN, TRUE);
		g_hash_table_insert(cfg->tokens, &t->tabid, t);


		t = g_new0(BfLangToken, 1);
		t->group = NULL;
		t->regexp = TRUE;
		t->name = xmlCharStrdup("_attr_");
		t->spec_type = 2;
		t->text = xmlCharStrdup("[a-zA-Z-]+=[^\" ><]+");
		t->context = b;
		bftv_put_into_dfa(cfg->tag_dfa, cfg, t, BFTV_DFA_TYPE_TOKEN, TRUE);
		g_hash_table_insert(cfg->tokens, &t->tabid, t);

		t = g_new0(BfLangToken, 1);
		t->group = NULL;
		t->regexp = TRUE;
		t->name = xmlCharStrdup("_attr_tag_begin_end_");
		t->spec_type = 4;
		t->text = xmlCharStrdup("/?>");
		t->context = b;
		bftv_put_into_dfa(cfg->tag_dfa, cfg, t, BFTV_DFA_TYPE_TOKEN, TRUE);
		g_hash_table_insert(cfg->tokens, &t->tabid, t);


	}


	{							/* fake identifier */
		gunichar c;
		gchar *pstr, *tofree, *pstr2;
		gchar out[6];

		pstr = g_strdup("");
		for (c = 0; c < BFTV_UTF8_RANGE; c++) {
			if (g_unichar_isalpha(c) || (gchar) c == '_') {
				tofree = pstr;
				for (i = 0; i < 6; i++)
					out[i] = '\0';
				g_unichar_to_utf8(c, out);
				pstr = g_strdup_printf("%s%s", pstr, out);
				g_free(tofree);
			}
		}
		pstr2 = g_strdup("");
		for (c = 0; c < BFTV_UTF8_RANGE; c++) {
			if (g_unichar_isalnum(c) || (gchar) c == '_') {
				tofree = pstr2;
				for (i = 0; i < 6; i++)
					out[i] = '\0';
				g_unichar_to_utf8(c, out);
				pstr2 = g_strdup_printf("%s%s", pstr2, out);
				g_free(tofree);
			}
		}
		tofree = pstr;
		pstr = g_strdup_printf("[%s]", pstr);
		g_free(tofree);
		for (i = 0; i < cfg->counter - 3; i++) {
			tofree = pstr;
			pstr = g_strdup_printf("%s[%s]", pstr, pstr2);
			g_free(tofree);
		}

		tofree = pstr;
		pstr = g_strdup_printf("%s[%s]*", pstr, pstr2);
		g_free(tofree);
		g_free(pstr2);

		BfLangToken *t = g_new0(BfLangToken, 1);
		t->group = NULL;
		t->regexp = TRUE;
		t->name = xmlCharStrdup("_fake_ident_");
		t->text = pstr;
		t->context = NULL;
		t->spec_type = 5;
		bftv_put_into_dfa(cfg->dfa, cfg, t, BFTV_DFA_TYPE_TOKEN, FALSE);
		g_hash_table_insert(cfg->tokens, &t->tabid, t);

	}

	cfg->token_allowed_states = g_new0(BfLangBlock*,cfg->tabnum+1);
	g_hash_table_foreach(cfg->tokens,bftv_fill_token_states,cfg);
	if (doc)
		xmlFreeDoc(doc);
	return cfg;
}


static void bftv_make_config_tables(BfLangConfig * cfg)
{
/*  gint i, j;*/
	if (!cfg)
		return;
/*  g_print("Scan table for %s\n",cfg->name);  */
	cfg->scan_table = bftv_make_scan_table(cfg->dfa, cfg);
	cfg->tag_scan_table = bftv_make_tag_scan_table(cfg->tag_dfa, cfg);
/*  g_print("st,"); 
  for(j=32;j<120;j++) g_print("%c,",j);
  g_print("\n"); 
  for(i=0;i<cfg->tabnum+1;i++) { 
     g_print("%.2d,",i);
     for(j=32;j<120;j++) 
        g_print("%d,",cfg->scan_table[i][j]);
    g_print("\n"); 
   }*/


/*  for(i=0;i<cfg->tabnum+1;i++) 
     for(j=36;j<120;j++) 
       g_print("[%d][%c]->%d\n",i,j,cfg->scan_table[i][j]);*/
}


/*
static gboolean
bftv_free_config_del_block (gpointer key, gpointer value, gpointer data)
{
  BfLangBlock *b = (BfLangBlock *) value;
  xmlFree (b->id);
  xmlFree (b->begin);
  xmlFree (b->end);
  g_free (b);
  return TRUE;
}

static gboolean
bftv_free_config_del_token (gpointer key, gpointer value, gpointer data)
{
  BfLangToken *b = (BfLangToken *) value;
  xmlFree (b->text);
  g_free (b);
  return TRUE;
}


static void
bftv_free_config (BfLangConfig * cfg)
{
  if (!cfg)
    return;
  xmlFree (cfg->name);
  xmlFree (cfg->description);
  g_free (cfg->extensions);
  g_hash_table_foreach_remove (cfg->blocks, bftv_free_config_del_block, NULL);
  g_hash_table_foreach_remove (cfg->tokens, bftv_free_config_del_token, NULL);
  g_free (cfg);
}*/


void bf_textview_set_language_ptr(BfTextView * self, BfLangConfig * cfg)
{
	self->lang = cfg;
	if (cfg)
		bf_textview_scan(self);
}

/*
 * ------------------------- BLOCK FOLDING ----------------------- 
 */

/**
*	bf_textview_fold_blocks:
*	@self:  BfTextView widget 
* @fold: if TRUE blocks should be folded, FALSE means unfold.
*
*	Fold/unfold all blocks in buffer.
*
*/
void bf_textview_fold_blocks(BfTextView * self, gboolean fold)
{
	GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self));
	GtkTextIter its, ite;

	g_return_if_fail(self != NULL);
	g_return_if_fail(BF_IS_TEXTVIEW(self));
	gtk_text_buffer_get_start_iter(buf, &its);
	gtk_text_buffer_get_end_iter(buf, &ite);
	bf_textview_fold_blocks_area(self, &its, &ite, fold);
}


/**
*	bf_textview_fold_blocks_area:
*	@self:  BfTextView widget 
*	@start: textbuffer iterator denoting area start
*	@end: textbuffer iterator denoting area end
* @fold: if TRUE blocks should be folded, FALSE means unfold.
*
*	Fold/unfold all blocks in buffer area specified by "start" and "end" iterators.
*
*/
void bf_textview_fold_blocks_area(BfTextView * self, GtkTextIter * start, GtkTextIter * end,
								  gboolean fold)
{
	GtkTextMark *mark;
	GtkTextIter it;
	gint i;
	gpointer ptr;

	g_return_if_fail(self != NULL);
	g_return_if_fail(BF_IS_TEXTVIEW(self));
	it = *start;
	for (i = gtk_text_iter_get_line(start); i <= gtk_text_iter_get_line(end); i++) {
		gtk_text_iter_set_line(&it, i);
		mark = bftv_get_first_block_at_line(self,&it, TRUE);
		if (mark && g_object_get_data(G_OBJECT(mark), "_type_") == &tid_block_start) {
			ptr = g_object_get_data(G_OBJECT(mark), "_type_");
			if (ptr == &tid_block_start) {
				if (fold)
					g_object_set_data(G_OBJECT(mark), "folded", &tid_false);
				else
					g_object_set_data(G_OBJECT(mark), "folded", &tid_true);
				bftv_fold(self,mark, FALSE);
			}
		}
	}
}

/**
*	bf_textview_fold_blocks_lines:
*	@self:  BfTextView widget 
*	@start: beginning line
*	@end: ending line
* @fold: if TRUE blocks should be folded, FALSE means unfold.
*
*	Fold/unfold all blocks in buffer area specified by "start" and "end" lines.
*
*/
void bf_textview_fold_blocks_lines(BfTextView * self, gint start, gint end, gboolean fold)
{
	GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self));
	GtkTextIter its, ite;

	g_return_if_fail(self != NULL);
	g_return_if_fail(BF_IS_TEXTVIEW(self));
	gtk_text_buffer_get_iter_at_line(buf, &its, start);
	gtk_text_buffer_get_iter_at_line(buf, &ite, end);
	gtk_text_iter_forward_to_line_end(&ite);
	bf_textview_fold_blocks_area(self, &its, &ite, fold);
}

/*
 * -------------------------------- CREATING THE VIEW
 * ------------------------- 
 */

/**
*	bf_textview_new:
*
*	Create new BfTextView widget.
*
* Returns: new BfTextView widget.
*/
GtkWidget *bf_textview_new(void)
{
	BfTextView *o = GET_NEW;
	GdkBitmap *bmp;

	g_signal_connect(G_OBJECT(o), "expose-event", G_CALLBACK(bf_textview_expose_cb), NULL);


	o->insert_signal_id = g_signal_connect_after(G_OBJECT(gtk_text_view_get_buffer(GTK_TEXT_VIEW(o))), "insert-text",
						   G_CALLBACK(bf_textview_insert_text_cb), o);
						   /* not after */
	g_signal_connect(G_OBJECT(gtk_text_view_get_buffer(GTK_TEXT_VIEW(o))), "delete-range",
						   G_CALLBACK(bf_textview_delete_range_cb), o);
	g_signal_connect_after(G_OBJECT(gtk_text_view_get_buffer(GTK_TEXT_VIEW(o))), "delete-range",
						   G_CALLBACK(bf_textview_delete_range_after_cb), o);
						   
	g_signal_connect_after(G_OBJECT(o), "move-cursor", G_CALLBACK(bf_textview_move_cursor_cb),
						   NULL);

	g_signal_connect(G_OBJECT(o), "button-press-event", G_CALLBACK(bf_textview_mouse_cb), NULL);
	o->folded_tag =
		gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table
								  (gtk_text_view_get_buffer(GTK_TEXT_VIEW(o))), "_folded_");
	if (!o->folded_tag)
		o->folded_tag =
			gtk_text_buffer_create_tag(gtk_text_view_get_buffer(GTK_TEXT_VIEW(o)), "_folded_",
									   "editable", FALSE, "invisible", TRUE, NULL);
	o->fold_header_tag =
		gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table
								  (gtk_text_view_get_buffer(GTK_TEXT_VIEW(o))), "_fold_header_");
	if (!o->fold_header_tag)
	{
		bmp = gdk_bitmap_create_from_data (NULL,folded_xbm, 2,2);
		o->fold_header_tag =
			gtk_text_buffer_create_tag(gtk_text_view_get_buffer(GTK_TEXT_VIEW(o)), "_fold_header_",
									   "editable", FALSE, "background", "#F7F3D2", "foreground-stipple",bmp,NULL);
		g_object_unref(bmp);									   
	}								   
	o->block_tag =
		gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table
								  (gtk_text_view_get_buffer(GTK_TEXT_VIEW(o))), "_block_");
	if (!o->block_tag)
		o->block_tag =
			gtk_text_buffer_create_tag(gtk_text_view_get_buffer(GTK_TEXT_VIEW(o)), "_block_", NULL);
	o->block_match_tag =
		gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table
								  (gtk_text_view_get_buffer(GTK_TEXT_VIEW(o))), "_block_match_");
	if (!o->block_match_tag)
		o->block_match_tag =
			gtk_text_buffer_create_tag(gtk_text_view_get_buffer(GTK_TEXT_VIEW(o)), "_block_match_",
									   "background", "#F7F3D2", NULL);

	bf_textview_recolor(o,"#000000","#FFFFFF");
	return (GtkWidget *) o;
}

/**
*	bf_textview_new_with_buffer:
*	@buffer
*
*	Create new BfTextView widget setting given buffer.
*
* Returns: new BfTextView widget.
*/
GtkWidget *bf_textview_new_with_buffer(GtkTextBuffer * buffer)
{
	BfTextView *o = GET_NEW;
	gtk_text_view_set_buffer(GTK_TEXT_VIEW(o), buffer);
	GdkBitmap *bmp;

	g_signal_connect(G_OBJECT(o), "expose-event", G_CALLBACK(bf_textview_expose_cb), NULL);
	o->insert_signal_id = 	g_signal_connect_after(G_OBJECT(gtk_text_view_get_buffer(GTK_TEXT_VIEW(o))), "insert-text",
						   G_CALLBACK(bf_textview_insert_text_cb), o);
						   /* not after */
	g_signal_connect(G_OBJECT(gtk_text_view_get_buffer(GTK_TEXT_VIEW(o))), "delete-range",
						   G_CALLBACK(bf_textview_delete_range_cb), o);
	g_signal_connect_after(G_OBJECT(gtk_text_view_get_buffer(GTK_TEXT_VIEW(o))), "delete-range",
						   G_CALLBACK(bf_textview_delete_range_after_cb), o);
						   
/*  g_signal_connect (G_OBJECT (gtk_text_view_get_buffer (GTK_TEXT_VIEW(o))),
  			 "changed", G_CALLBACK(bf_textview_changed_cb), o);*/
	g_signal_connect_after(G_OBJECT(o), "move-cursor", G_CALLBACK(bf_textview_move_cursor_cb),
						   NULL);

	g_signal_connect(G_OBJECT(o), "button-press-event", G_CALLBACK(bf_textview_mouse_cb), NULL);
	o->folded_tag =
		gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table
								  (gtk_text_view_get_buffer(GTK_TEXT_VIEW(o))), "_folded_");
	if (!o->folded_tag)
		o->folded_tag =
			gtk_text_buffer_create_tag(gtk_text_view_get_buffer(GTK_TEXT_VIEW(o)), "_folded_",
									   "editable", FALSE, "invisible", TRUE, NULL);
	o->fold_header_tag =
		gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table
								  (gtk_text_view_get_buffer(GTK_TEXT_VIEW(o))), "_fold_header_");
	if (!o->fold_header_tag)
	{
		bmp = gdk_bitmap_create_from_data (NULL,folded_xbm, 2,2);
		o->fold_header_tag =
			gtk_text_buffer_create_tag(gtk_text_view_get_buffer(GTK_TEXT_VIEW(o)), "_fold_header_",
									   "editable", FALSE, "background", "#F7F3D2","foreground-stipple",bmp,NULL);
		g_object_unref(bmp);									   
	}									   
	o->block_tag =
		gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table
								  (gtk_text_view_get_buffer(GTK_TEXT_VIEW(o))), "_block_");
	if (!o->block_tag)
		o->block_tag =
			gtk_text_buffer_create_tag(gtk_text_view_get_buffer(GTK_TEXT_VIEW(o)), "_block_", NULL);
	o->block_match_tag =
		gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table
								  (gtk_text_view_get_buffer(GTK_TEXT_VIEW(o))), "_block_match_");
	if (!o->block_match_tag)
		o->block_match_tag =
			gtk_text_buffer_create_tag(gtk_text_view_get_buffer(GTK_TEXT_VIEW(o)), "_block_match_",
									   "background", "#F7F3D2", NULL);

	bf_textview_recolor(o,"#000000","#FFFFFF");
	return (GtkWidget *) o;
}


/*
 * --------------------------------- EVENTS
 * ---------------------------------------
 */

static gboolean bf_textview_expose_cb(GtkWidget * widget, GdkEventExpose * event, gpointer doc)
{
	GdkWindow *left_win;
	GdkRectangle rect;
	GtkTextIter l_start, l_end, it, it2;
	GtkTextMark *block_mark;
	gint l_top1, numlines, w, l_top2, i, w2;
	PangoLayout *l;
	gchar *pomstr = NULL;
	gint pt_lines, pt_sym, pt_blocks, currline=0;
	GdkGC *gc=NULL;
	gpointer aux;

	GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget));

	DEBUG_MSG("bf_textview_expose_cb, started\n");
	if (BF_TEXTVIEW(widget)->need_rescan) {
		DEBUG_MSG("bf_textview_expose_cb, need rescan!\n");
		bf_textview_scan(BF_TEXTVIEW(widget));
	}

	left_win = gtk_text_view_get_window(GTK_TEXT_VIEW(widget), GTK_TEXT_WINDOW_LEFT);
	
	if (left_win != event->window) {	
	    
		if (event->window == gtk_text_view_get_window(GTK_TEXT_VIEW(widget), GTK_TEXT_WINDOW_TEXT) )
		{
			if ( BF_TEXTVIEW(widget)->show_current_line ) 
			{
				/* current line highlighting - thanks gtksourceview team :) */	
				gtk_text_buffer_get_iter_at_mark(buf, &it, gtk_text_buffer_get_insert(buf));
				gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(widget), &rect);
				gtk_text_view_get_line_yrange(GTK_TEXT_VIEW(widget), &it, &w, &w2);
				gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(widget), GTK_TEXT_WINDOW_TEXT,
												  rect.x, rect.y, &rect.x, &rect.y);
				gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(widget), GTK_TEXT_WINDOW_TEXT, 0, w,
												  NULL, &w);
				gdk_draw_rectangle(event->window, widget->style->bg_gc[GTK_WIDGET_STATE(widget)], TRUE,
							   rect.x, w, rect.width, w2);
			}
			if ( BF_TEXTVIEW(widget)->show_rmargin ) 
			{
				l = gtk_widget_create_pango_layout(widget, "x");
				pango_layout_get_pixel_size(l, &w, NULL);
				gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(widget), &rect);
				gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(widget), GTK_TEXT_WINDOW_TEXT,
												  rect.x, rect.y, NULL, &rect.y);									  
				gdk_draw_line(event->window, widget->style->bg_gc[GTK_WIDGET_STATE(widget)], 
							   w*BF_TEXTVIEW(widget)->rmargin_at,rect.y,w*BF_TEXTVIEW(widget)->rmargin_at,rect.y+rect.height);
				g_object_unref(G_OBJECT(l));							   				
			}			
		}	
		return FALSE;
	}
	
	if (!BF_TEXTVIEW(widget)->show_lines && !BF_TEXTVIEW(widget)->show_symbols
		&& !BF_TEXTVIEW(widget)->show_blocks)
		return FALSE;

	
	gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(widget), &rect);
	gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(widget), &l_start, rect.y, &l_top1);
	gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(widget), &l_end, rect.y + rect.height, &l_top2);
	l = gtk_widget_create_pango_layout(widget, "");
	if (BF_TEXTVIEW(widget)->show_lines) {
		numlines = gtk_text_buffer_get_line_count(gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget)));
		pomstr = g_strdup_printf("%d", MAX(99, numlines));
		pango_layout_set_text(l, pomstr, -1);
		g_free(pomstr);
		pango_layout_get_pixel_size(l, &w, NULL);
		BF_TEXTVIEW(widget)->lw_size_lines = w + 4;
		gtk_text_buffer_get_iter_at_mark(buf,&it,gtk_text_buffer_get_insert(buf));
		currline = gtk_text_iter_get_line(&it);
	}
	gtk_text_view_set_border_window_size(GTK_TEXT_VIEW(widget), GTK_TEXT_WINDOW_LEFT,
										 BF_TEXTVIEW(widget)->lw_size_lines +
										 BF_TEXTVIEW(widget)->lw_size_blocks +
										 BF_TEXTVIEW(widget)->lw_size_sym);
	pt_lines = 2;
	if (BF_TEXTVIEW(widget)->show_lines)
		pt_sym = BF_TEXTVIEW(widget)->lw_size_lines + 2;
	else
		pt_sym = 2;
	pt_blocks = 2;
	if (BF_TEXTVIEW(widget)->show_lines)
		pt_blocks = BF_TEXTVIEW(widget)->lw_size_lines + pt_blocks;
	if (BF_TEXTVIEW(widget)->show_symbols)
		pt_blocks = BF_TEXTVIEW(widget)->lw_size_sym + pt_blocks;


	it = l_start;
	for (i = gtk_text_iter_get_line(&l_start); i <= gtk_text_iter_get_line(&l_end); i++) {
		gtk_text_iter_set_line(&it, i);
		gtk_text_view_get_line_yrange(GTK_TEXT_VIEW(widget), &it, &w, NULL);
		gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(widget), GTK_TEXT_WINDOW_LEFT, 0, w,
											  NULL, &w);

		if (BF_TEXTVIEW(widget)->show_lines) {	/* show line numbers */
			DEBUG_MSG("checking for folded tag %p\n", BF_TEXTVIEW(widget)->folded_tag);
			if (!gtk_text_iter_has_tag(&it, BF_TEXTVIEW(widget)->folded_tag)) {		
				if ( currline == i )
					pomstr = g_strdup_printf("<b>%d</b>",1+i); /* line numbers should start at 1 */		
				else
					pomstr = g_strdup_printf("%d",1+i);
				pango_layout_set_markup(l, pomstr, -1);
				gdk_draw_layout(GDK_DRAWABLE(left_win),
                                widget->style->text_gc[GTK_WIDGET_STATE(widget)],
                                pt_lines,w,l);
				g_free(pomstr);
			}
		}
		if (BF_TEXTVIEW(widget)->show_symbols) {	/* show symbols */
			
			if (!gtk_text_iter_has_tag(&it, BF_TEXTVIEW(widget)->folded_tag)) {
				gc = gdk_gc_new(GDK_DRAWABLE(left_win));
				GSList *lst3 = gtk_text_iter_get_marks(&it);
				aux = NULL;
				while (lst3) {
										aux = g_hash_table_lookup(BF_TEXTVIEW(widget)->symbol_lines,lst3->data);
										if ( aux ) break;
										lst3 = g_slist_next(lst3);
				}
				g_slist_free(lst3);
				if (aux) {
					gdk_pixbuf_render_to_drawable(((BfTextViewSymbol *) aux)->pixmap, GDK_DRAWABLE(left_win), gc, 0, 0, pt_sym,
												  w + 2, 10, 10, GDK_RGB_DITHER_NORMAL, 0, 0);
				}
				g_object_unref(gc);			
			}
			
		}
		if (BF_TEXTVIEW(widget)->show_blocks) {	/* show block markers */
			GtkTextMark *mark_begin = NULL, *mark_end = NULL;
			gpointer b_type=NULL, b_folded=NULL;
			block_mark = bftv_get_first_block_at_line(BF_TEXTVIEW(widget),&it, TRUE);
			if (block_mark) {
				b_type = g_object_get_data(G_OBJECT(block_mark), "_type_");
				b_folded = g_object_get_data(G_OBJECT(block_mark), "folded");
				mark_begin = block_mark;
				mark_end = GTK_TEXT_MARK(g_object_get_data(G_OBJECT(block_mark), "ref"));
			}
			if (block_mark && mark_end && mark_begin
				&& !gtk_text_iter_has_tag(&it, BF_TEXTVIEW(widget)->folded_tag)) {
				gtk_text_buffer_get_iter_at_mark(buf, &it2, mark_end);
				gtk_text_view_get_line_yrange(GTK_TEXT_VIEW(widget), &it2, &w2, NULL);
				gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(widget), GTK_TEXT_WINDOW_LEFT,
													  0, w2, NULL, &w2);
				if (b_type == &tid_block_start) {
					gdk_draw_rectangle(GDK_DRAWABLE(left_win), widget->style->base_gc[GTK_WIDGET_STATE(widget)],
					 TRUE, pt_blocks, w + 2, 10, 10);
					gdk_draw_rectangle(GDK_DRAWABLE(left_win), widget->style->fg_gc[GTK_WIDGET_STATE(widget)],
					 FALSE, pt_blocks, w + 2, 10, 10);

					if (b_folded == &tid_true) {
						gdk_draw_line(GDK_DRAWABLE(left_win), widget->style->fg_gc[GTK_WIDGET_STATE(widget)],
						 pt_blocks + 3, w + 7,pt_blocks + 7, w + 7);
						gdk_draw_line(GDK_DRAWABLE(left_win), widget->style->fg_gc[GTK_WIDGET_STATE(widget)], 
						pt_blocks + 5, w + 5, pt_blocks + 5, w + 9);
					} else
						gdk_draw_line(GDK_DRAWABLE(left_win), widget->style->fg_gc[GTK_WIDGET_STATE(widget)], 
						pt_blocks + 3, w + 7, pt_blocks + 7, w + 7);
				} else {		/* block end */
					gdk_draw_line(GDK_DRAWABLE(left_win), widget->style->fg_gc[GTK_WIDGET_STATE(widget)], 
					pt_blocks + 5, w, pt_blocks + 5, w + 6);
					gdk_draw_line(GDK_DRAWABLE(left_win), widget->style->fg_gc[GTK_WIDGET_STATE(widget)], 
					pt_blocks + 5, w + 6, pt_blocks + 10, w + 6);
				}
			} else {			/* not block begin or end, but perhaps inside */
				DEBUG_MSG("checking if we're in tag %p\n", BF_TEXTVIEW(widget)->block_tag);
				if (gtk_text_iter_has_tag(&it, BF_TEXTVIEW(widget)->block_tag)
					&& !gtk_text_iter_has_tag(&it, BF_TEXTVIEW(widget)->folded_tag)) {
					gdk_draw_line(GDK_DRAWABLE(left_win), widget->style->fg_gc[GTK_WIDGET_STATE(widget)], 
					pt_blocks + 5, w, pt_blocks + 5, w + 16);
				}
			}
		}
		/* block markers */
	}							/* end of lines loop */

    
	g_object_unref(G_OBJECT(l));
	return TRUE;
}

static void bf_textview_insert_text_cb(GtkTextBuffer * textbuffer, GtkTextIter * iter, gchar * string,
                                       gint stringlen, BfTextView * view)
{
	gint len;
	gboolean trigger = FALSE;
	gchar *p = string;
	DEBUG_MSG("bf_textview_insert_text_cb, started, string=\"%s\", stringlen=%d\n", string, stringlen);
	if (!view->lang)
		return;
	view->delete_rescan = FALSE;
	if (GTK_WIDGET_VISIBLE(view)) {
		len = 0;
		while (len < g_utf8_strlen(string, stringlen)) {
			if (view->lang->as_triggers[(gint) * p] == 1) {
				trigger = TRUE;
				break;
			}
			len++;
			p = g_utf8_next_char(p);
		}
	
		if (!trigger)
			return;

		if (stringlen == 1 && *string == '>')
			view->tag_ac_state = TRUE;
		else
			view->tag_ac_state = FALSE;
					
		if (view->hl_mode == BFTV_HL_MODE_ALL || view->need_rescan) {
			bf_textview_scan(view);
		} else {
			bf_textview_scan_visible(view);
		}
	} else {
		DEBUG_MSG("bf_textview_insert_text_cb, postpone the scanning, setting need_rescan\n");
		view->need_rescan = TRUE;
	}	
}

/* this function does the actual folding based on a GtkTextMark 
which should be the start of the block start *** Oskar, is that correct??? *** 
**** CONFIRMED ********

it uses properties ref, ref_e1 and ref_e2
all these three properties contain other GtkTextMarks

ref is the mark that is at the end of the block start
ref_e1 is the mark that is at the beginning of the block end
ref_e2 is the mark that is at the end of the block end

 */
static void bftv_fold(BfTextView * self, GtkTextMark * mark, gboolean move_cursor)
{
	GtkTextBuffer *buf = gtk_text_mark_get_buffer(mark);
	gpointer ptr = g_object_get_data(G_OBJECT(mark), "folded");
	gpointer single = g_object_get_data(G_OBJECT(mark), "single-line");
	gboolean f = FALSE;
	GtkTextIter it1, it2, it3, it4, it5;
	gpointer br = NULL;

	if (!ptr || single == &tid_true || g_object_get_data(G_OBJECT(mark), "_type_") != &tid_block_start)
		return;

	if (ptr == &tid_true) {
		g_object_set_data(G_OBJECT(mark), "folded", &tid_false);
		br = g_object_get_data(G_OBJECT(mark), "ref_e1");
		if (br)
			g_object_set_data(G_OBJECT(br), "folded", &tid_false);
		f = FALSE;
	} else {
		g_object_set_data(G_OBJECT(mark), "folded", &tid_true);
		br = g_object_get_data(G_OBJECT(mark), "ref_e1");
		if (br)
			g_object_set_data(G_OBJECT(br), "folded", &tid_true);
		f = TRUE;
	}
	gtk_text_buffer_get_iter_at_mark(buf, &it1, mark);
	gtk_text_iter_set_line(&it1, gtk_text_iter_get_line(&it1));
	ptr = g_object_get_data(G_OBJECT(mark), "ref");
	gtk_text_buffer_get_iter_at_mark(buf, &it2, GTK_TEXT_MARK(ptr));
/*  gtk_text_iter_forward_to_line_end(&it2);*/
	ptr = g_object_get_data(G_OBJECT(mark), "ref_e1");
	gtk_text_buffer_get_iter_at_mark(buf, &it3, GTK_TEXT_MARK(ptr));
	
	/* move it3 backward to offset 0 such that the indenting of the block end is preserved */
	gtk_text_iter_set_line_offset(&it3, 0);
	
	ptr = g_object_get_data(G_OBJECT(mark), "ref_e2");
	gtk_text_buffer_get_iter_at_mark(buf, &it4, GTK_TEXT_MARK(ptr));

	if (f) {
		if (move_cursor) {
			it5 = it4;
			gtk_text_iter_forward_char(&it5);
			gtk_text_buffer_place_cursor(buf, &it5);
		}
		gtk_text_buffer_apply_tag_by_name(buf, "_folded_", &it2, &it3);
		gtk_text_buffer_apply_tag_by_name(buf, "_fold_header_", &it1, &it2);
		gtk_text_buffer_apply_tag_by_name(buf, "_fold_header_", &it3, &it4);
	} else {
		gtk_text_buffer_remove_tag_by_name(buf, "_fold_header_", &it1, &it4);
		gtk_text_buffer_remove_tag_by_name(buf, "_folded_", &it1, &it4);
		/* unfold also all internal blocks */
		gtk_text_iter_forward_line(&it2);
		gtk_text_iter_backward_line(&it3);
		bf_textview_fold_blocks_area(self, &it2, &it3,FALSE);
	}
}



static gboolean bf_textview_mouse_cb(GtkWidget * widget, GdkEvent * event, gpointer user_data)
{
	GdkWindow *win = gtk_text_view_get_window(GTK_TEXT_VIEW(widget),
											  GTK_TEXT_WINDOW_LEFT);
	gint x, y;
	GtkTextIter it;
	GtkTextMark *block_mark = NULL;
	gint pt_lines, pt_sym, pt_blocks;
	DEBUG_MSG("bf_textview_mouse_cb, started\n");
	if (win != event->button.window)
		return FALSE;

	pt_lines = 2;
	if (BF_TEXTVIEW(widget)->show_lines)
		pt_sym = BF_TEXTVIEW(widget)->lw_size_lines + 2;
	else
		pt_sym = 2;
	pt_blocks = 2;
	if (BF_TEXTVIEW(widget)->show_lines)
		pt_blocks = BF_TEXTVIEW(widget)->lw_size_lines + pt_blocks;
	if (BF_TEXTVIEW(widget)->show_symbols)
		pt_blocks = BF_TEXTVIEW(widget)->lw_size_sym + pt_blocks;

	if (event->button.button == 1) {
		if (event->button.x >= pt_blocks) {
			gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(widget), GTK_TEXT_WINDOW_TEXT, 0,
												  event->button.y, &x, &y);
			gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(widget), &it, y, &x);
			block_mark = bftv_get_first_block_at_line(BF_TEXTVIEW(widget),&it, TRUE);
			if (block_mark)
				bftv_fold(BF_TEXTVIEW(widget),block_mark, TRUE);
			return TRUE;
		}
	} else if (event->button.button == 3) {
		if (event->button.x >= pt_blocks) {
			gtk_menu_popup(GTK_MENU(BF_TEXTVIEW(widget)->fold_menu), NULL, NULL, NULL, NULL, 1,
						   gtk_get_current_event_time());
			return TRUE;
		}
	}
	return FALSE;
}

static void bftv_delete_blocks_from_area(BfTextView * view, GtkTextIter * arg1, GtkTextIter * arg2)
{
	GtkTextBuffer *textbuffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
	GtkTextMark *mark = NULL, *mark2 = NULL, *mark3, *mark4;
	gboolean deleted = FALSE;
	GtkTextIter it, it2, it3;

	if (!view->lang || !view->lang->scan_blocks)
		return;
#ifdef HL_PROFILING
		g_print("bftv_delete_blocks_from_area start\n" );
#endif		
	it = *arg1;
	while (gtk_text_iter_compare(&it, arg2) <= 0) {
		mark = bftv_get_block_at_iter(&it);
		if (mark) {
			if (g_object_get_data(G_OBJECT(mark), "folded") != &tid_true) {
				if (g_object_get_data(G_OBJECT(mark), "_type_") == &tid_block_start) {
					mark2 = GTK_TEXT_MARK(g_object_get_data(G_OBJECT(mark), "ref"));
					mark3 = GTK_TEXT_MARK(g_object_get_data(G_OBJECT(mark), "ref_e1"));
					mark4 = GTK_TEXT_MARK(g_object_get_data(G_OBJECT(mark), "ref_e2"));

					gtk_text_buffer_get_iter_at_mark(textbuffer, &it2, mark);
					gtk_text_buffer_get_iter_at_mark(textbuffer, &it3, mark4);
					if (gtk_text_iter_in_range(&it2, arg1, arg2)
						&& gtk_text_iter_in_range(&it3, arg1, arg2)) {
						if (!gtk_text_iter_has_tag(&it2, view->block_tag)) {
							gtk_text_buffer_get_iter_at_mark(textbuffer, &it2, mark2);
							gtk_text_buffer_get_iter_at_mark(textbuffer, &it3, mark3);
							gtk_text_buffer_remove_tag(textbuffer, view->block_tag, &it2, &it3);
						}
#ifdef HL_PROFILING
						g_print("bftv_delete_blocks_from_area, called for mark %p, %p, %p and %p\n",mark,mark2,mark3,mark4);
#endif
						if (g_object_get_data(G_OBJECT(mark), "tagname"))
							g_free((gchar*)g_object_get_data(G_OBJECT(mark), "tagname"));
						if (g_object_get_data(G_OBJECT(mark2), "tagname"))
							g_free((gchar*)g_object_get_data(G_OBJECT(mark2), "tagname"));
						if (g_object_get_data(G_OBJECT(mark3), "tagname"))
							g_free((gchar*)g_object_get_data(G_OBJECT(mark3), "tagname"));
						if (g_object_get_data(G_OBJECT(mark4), "tagname"))
							g_free((gchar*)g_object_get_data(G_OBJECT(mark4), "tagname"));

						gtk_text_buffer_delete_mark(textbuffer, mark);
						gtk_text_buffer_delete_mark(textbuffer, mark2);
						gtk_text_buffer_delete_mark(textbuffer, mark3);
						gtk_text_buffer_delete_mark(textbuffer, mark4);
					}
				}
				deleted = TRUE;
			}					/* folded */
		}
		gtk_text_iter_forward_char(&it);
		if (gtk_text_iter_is_end(&it))
			break;
	}
}

static void bf_textview_delete_range_cb(GtkTextBuffer * textbuffer, GtkTextIter * arg1,
										GtkTextIter * arg2, gpointer user_data)
{
	BfTextView *view = BF_TEXTVIEW(user_data);
	gboolean trigger = FALSE;
	gint len, pomlen;
	gchar *p, *pomstr;

	view->delete_rescan = FALSE;
	DEBUG_MSG("bf_textview_delete_range_cb, started\n");
	if (!view->lang)
		return;
	p = pomstr = gtk_text_buffer_get_text(textbuffer, arg1, arg2, TRUE);
	len = 0;
	pomlen = g_utf8_strlen(pomstr, -1);
	while (len < pomlen) {
		if (view->lang->as_triggers[(gint) * p] == 1) {
			trigger = TRUE;
			break;
		}
		len++;
		p = g_utf8_next_char(p);
	}
	g_free(pomstr);
	if (trigger) view->delete_rescan = TRUE;


}

static void bf_textview_delete_range_after_cb(GtkTextBuffer * textbuffer, GtkTextIter * arg1,
										GtkTextIter * arg2, gpointer user_data)
{
	BfTextView *view = BF_TEXTVIEW(user_data);

	DEBUG_MSG("bf_textview_delete_range_cb, started\n");
	if (!view->lang)	return;
	if (GTK_WIDGET_VISIBLE(view) && view->delete_rescan) {
		if (view->hl_mode == BFTV_HL_MODE_ALL || view->need_rescan) {			
			bf_textview_scan(view);
			view->delete_rescan = FALSE;
		} else {
			bf_textview_scan_visible(view);
			view->delete_rescan = FALSE;
		}
	} else if (view->delete_rescan) {
		view->need_rescan = TRUE;
	}
}


/* this function does highlight the matching braces (block end and start) */
static void bf_textview_move_cursor_cb(GtkTextView * widget, GtkMovementStep step, gint count,
									   gboolean extend_selection, gpointer user_data)
{
	GtkTextBuffer *buf = gtk_text_view_get_buffer(widget);
	GtkTextIter it, it2;
	GtkTextMark *block = NULL;
	DEBUG_MSG("bf_textview_move_cursor_cb, started\n");
	
	if (BF_TEXTVIEW(widget)->last_matched_block)
	{
		block = BF_TEXTVIEW(widget)->last_matched_block;
		gtk_text_buffer_get_iter_at_mark(buf, &it, block);
		gtk_text_buffer_get_iter_at_mark(buf, &it2,
										 GTK_TEXT_MARK(g_object_get_data(G_OBJECT(block), "ref")));
		gtk_text_buffer_remove_tag(buf, BF_TEXTVIEW(widget)->block_match_tag, &it, &it2);
		if (g_object_get_data(G_OBJECT(block), "_type_") == &tid_block_start) {
			gtk_text_buffer_get_iter_at_mark(buf, &it,
											 GTK_TEXT_MARK(g_object_get_data
														   (G_OBJECT(block), "ref_e1")));
			gtk_text_buffer_get_iter_at_mark(buf, &it2,
											 GTK_TEXT_MARK(g_object_get_data
														   (G_OBJECT(block), "ref_e2")));
		} else if (g_object_get_data(G_OBJECT(block), "_type_") == &tid_block_end) {
			gtk_text_buffer_get_iter_at_mark(buf, &it,
											 GTK_TEXT_MARK(g_object_get_data
														   (G_OBJECT(block), "ref_b1")));
			gtk_text_buffer_get_iter_at_mark(buf, &it2,
											 GTK_TEXT_MARK(g_object_get_data
														   (G_OBJECT(block), "ref_b2")));
		}
		gtk_text_buffer_remove_tag(buf, BF_TEXTVIEW(widget)->block_match_tag, &it, &it2);		
		block = NULL;
	}
	gtk_text_buffer_get_iter_at_mark(buf, &it, gtk_text_buffer_get_insert(buf));
	block = bftv_get_block_at_iter(&it);
	if (block && g_object_get_data(G_OBJECT(block),"folded")==&tid_true) block = NULL;
	if (block) {
		
		gtk_text_buffer_get_iter_at_mark(buf, &it, block);
		gtk_text_buffer_get_iter_at_mark(buf, &it2,
										 GTK_TEXT_MARK(g_object_get_data(G_OBJECT(block), "ref")));
		gtk_text_buffer_apply_tag(buf, BF_TEXTVIEW(widget)->block_match_tag, &it, &it2);
		if (g_object_get_data(G_OBJECT(block), "_type_") == &tid_block_start) {
			gtk_text_buffer_get_iter_at_mark(buf, &it,
											 GTK_TEXT_MARK(g_object_get_data
														   (G_OBJECT(block), "ref_e1")));
			gtk_text_buffer_get_iter_at_mark(buf, &it2,
											 GTK_TEXT_MARK(g_object_get_data
														   (G_OBJECT(block), "ref_e2")));
		} else if (g_object_get_data(G_OBJECT(block), "_type_") == &tid_block_end) {
			gtk_text_buffer_get_iter_at_mark(buf, &it,
											 GTK_TEXT_MARK(g_object_get_data
														   (G_OBJECT(block), "ref_b1")));
			gtk_text_buffer_get_iter_at_mark(buf, &it2,
											 GTK_TEXT_MARK(g_object_get_data
														   (G_OBJECT(block), "ref_b2")));
		}
		gtk_text_buffer_apply_tag(buf, BF_TEXTVIEW(widget)->block_match_tag, &it, &it2);
		BF_TEXTVIEW(widget)->last_matched_block = block;
	}
}


/*
 * ---------------------------- MISC
 * --------------------------------------------- 
 */

/**
*	bf_textview_show_lines:
*	@self:  BfTextView widget 
* @show: if TRUE line numbers will be shown, FALSE means line numbers are hidden.
*
*	Show/hide line numbers at the left side of widget.
*
*/
void bf_textview_show_lines(BfTextView * self, gboolean show)
{
	PangoLayout *l = gtk_widget_create_pango_layout(GTK_WIDGET(self), "");
	gint numlines = gtk_text_buffer_get_line_count(gtk_text_view_get_buffer(GTK_TEXT_VIEW(self)));
	gchar *pomstr = g_strdup_printf("%d", MAX(99, numlines));
	gint w;

	pango_layout_set_text(l, pomstr, -1);
	g_free(pomstr);
	pango_layout_get_pixel_size(l, &w, NULL);
	g_object_unref(G_OBJECT(l));

	self->show_lines = show;
	if (show) {
		self->lw_size_lines = w + 4;
	} else {
		self->lw_size_lines = 0;
	}
	gtk_text_view_set_border_window_size(GTK_TEXT_VIEW(self), GTK_TEXT_WINDOW_LEFT,
										 self->lw_size_lines + self->lw_size_blocks +
										 self->lw_size_sym);
}


/**
*	bf_textview_show_symbols:
*	@self:  BfTextView widget 
* @show: if TRUE symbols will be shown, FALSE means symbols are hidden.
*
*	Show/hide line symbols at the left side of widget.
*	Symbols are any other marks you want to show in BfTextView lines,
*	they can be for example: bookmarks
*
*/
void bf_textview_show_symbols(BfTextView * self, gboolean show)
{
	self->show_symbols = show;
	if (show) {
		self->lw_size_sym = 14;
	} else {
		self->lw_size_sym = 0;
	}
	gtk_text_view_set_border_window_size(GTK_TEXT_VIEW(self), GTK_TEXT_WINDOW_LEFT,
										 self->lw_size_lines + self->lw_size_blocks +
										 self->lw_size_sym);
	gtk_widget_queue_draw(GTK_WIDGET(self));
}

/**
*	bf_textview_add_symbol:
*	@self:  BfTextView widget 
* @name: name of the symbol - this symbol will be recognized by given name in symbol database.
* @pix:  pixmap to show for this symbol
*
*	Add symbol to BfTextView symbols database.
*
* Returns: TRUE if symbol added or FALSE if widget cannot add symbol (symbol of that name already exists).	
*/
gboolean bf_textview_add_symbol(BfTextView * self, gchar * name, GdkPixbuf * pix)
{
	gpointer ptr = g_hash_table_lookup(self->symbols, name);
	if (ptr)
		return FALSE;
	BfTextViewSymbol *sym = g_new0(BfTextViewSymbol, 1);
	sym->name = g_strdup(name);
	sym->pixmap = gdk_pixbuf_scale_simple(pix, 10, 10,
												GDK_INTERP_BILINEAR);
	g_hash_table_insert(self->symbols, name, sym);
	return TRUE;
}


static gboolean bftv_hash_remove(gpointer key, gpointer val, gpointer udata)
{
	BfTextViewSymbol *sym = (BfTextViewSymbol *) val;
	if (strcmp(sym->name, (gchar *) udata) == 0)
		return TRUE;
	return FALSE;
}

/**
*	bf_textview_remove_symbol:
*	@self:  BfTextView widget 
* @name: name of the symbol.
*
*	Remove symbol from BfTextView symbols database.
*	
*/
void bf_textview_remove_symbol(BfTextView * self, gchar * name)
{
	gpointer ptr = g_hash_table_lookup(self->symbols, name);
	if (ptr) {
		g_hash_table_foreach_remove(self->symbol_lines, bftv_hash_remove, name);
		g_hash_table_remove(self->symbols, name);
		g_free(((BfTextViewSymbol *) ptr)->name);
		g_free(((BfTextViewSymbol *) ptr)->pixmap);
		g_free((BfTextViewSymbol *) ptr);
		gtk_widget_queue_draw(GTK_WIDGET(self));
	}
}


/**
*	bf_textview_set_symbol:
*	@self:  BfTextView widget 
* @name: name of the symbol.
* @line: line number in which symbol should be set
* @set: if TRUE - symbol should be shown, FALSE - hidden.
*
*	Set symbol visibility for a given line.
*	
*/
void bf_textview_set_symbol(BfTextView * self, gchar * name, gint line, gboolean set)
{
	gpointer ptr = g_hash_table_lookup(self->symbols, name);
	GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self));
	GtkTextIter it;
	GtkTextMark *mark;
	GSList *lst;
	
	if (!ptr) return;
	gtk_text_buffer_get_iter_at_line(buf,&it,line);
	
	if (set) {			
			mark = gtk_text_buffer_create_mark(buf,NULL,&it,TRUE);
			g_hash_table_insert(self->symbol_lines, mark, ptr);
	}	
	else {
			mark = NULL;
			lst = gtk_text_iter_get_marks(&it);
			while (lst) {			
					if (g_hash_table_lookup(self->symbol_lines,lst->data)) {
						mark = GTK_TEXT_MARK(lst->data);
						break;
					}
					lst = g_slist_next(lst);
			}
			g_slist_free(lst);
			if ( mark )
				g_hash_table_remove(self->symbol_lines, mark);
	}
	gtk_widget_queue_draw(GTK_WIDGET(self));

}

/**
*	bf_textview_show_blocks:
*	@self:  BfTextView widget 
* @show: TRUE - blocks are shown, FALSE - hidden.
*
*	Switch blocks visibility on/off.
*	
*/
void bf_textview_show_blocks(BfTextView * self, gboolean show)
{
	self->show_blocks = show;
	if (show) {
		self->lw_size_blocks = 15;
	} else {
		self->lw_size_blocks = 0;
	}
	gtk_text_view_set_border_window_size(GTK_TEXT_VIEW(self), GTK_TEXT_WINDOW_LEFT,
										 self->lw_size_lines + self->lw_size_blocks +
										 self->lw_size_sym);
}


/**
*	bf_textview_show_rmargin:
*	@self:  BfTextView widget 
* @show: TRUE - right margin is shown, FALSE - hidden.
* @column: number of column after which right margin is placed
*	Switch right margin visibility on/off.
*	
*/
void bf_textview_show_rmargin(BfTextView * self, gboolean show, gint column)
{
	self->show_rmargin = show;
	if ( column > 0 )
		self->rmargin_at = column;
}


/**
*	bf_textview_set_bg_color:
*	@self:  BfTextView widget 
* @color: - hexadecimal (HTML) representation of a color.
*
*	Set BfTextView background color. 
* 	
*/
void bf_textview_set_bg_color(BfTextView * self, gchar * color)
{
	bf_textview_recolor(self,self->fg_color,color);
}

/**
*	bf_textview_set_fg_color:
*	@self:  BfTextView widget 
* @color: - hexadecimal (HTML) representation of a color.
*
*	Set BfTextView foreground (text) color. 
* 	
*/
void bf_textview_set_fg_color(BfTextView * self, gchar * color)
{
	bf_textview_recolor(self,color,self->bkg_color);
}


/* These functions are taken stright from GTK+ sources (www.gtk.org), because I need to
compute color shading for line highlight and left window  */

static void
rgb_to_hls (gdouble *r,
            gdouble *g,
            gdouble *b)
{
  gdouble min;
  gdouble max;
  gdouble red;
  gdouble green;
  gdouble blue;
  gdouble h, l, s;
  gdouble delta;
  
  red = *r;
  green = *g;
  blue = *b;
  
  if (red > green)
    {
      if (red > blue)
        max = red;
      else
        max = blue;
      
      if (green < blue)
        min = green;
      else
        min = blue;
    }
  else
    {
      if (green > blue)
        max = green;
      else
        max = blue;
      
      if (red < blue)
        min = red;
      else
        min = blue;
    }
  
  l = (max + min) / 2;
  s = 0;
  h = 0;
  
  if (max != min)
    {
      if (l <= 0.5)
        s = (max - min) / (max + min);
      else
        s = (max - min) / (2 - max - min);
      
      delta = max -min;
      if (red == max)
        h = (green - blue) / delta;
      else if (green == max)
        h = 2 + (blue - red) / delta;
      else if (blue == max)
        h = 4 + (red - green) / delta;
      
      h *= 60;
      if (h < 0.0)
        h += 360;
    }
  
  *r = h;
  *g = l;
  *b = s;
}

static void
hls_to_rgb (gdouble *h,
            gdouble *l,
            gdouble *s)
{
  gdouble hue;
  gdouble lightness;
  gdouble saturation;
  gdouble m1, m2;
  gdouble r, g, b;
  
  lightness = *l;
  saturation = *s;
  
  if (lightness <= 0.5)
    m2 = lightness * (1 + saturation);
  else
    m2 = lightness + saturation - lightness * saturation;
  m1 = 2 * lightness - m2;
  
  if (saturation == 0)
    {
      *h = lightness;
      *l = lightness;
      *s = lightness;
    }
  else
    {
      hue = *h + 120;
      while (hue > 360)
        hue -= 360;
      while (hue < 0)
        hue += 360;
      
      if (hue < 60)
        r = m1 + (m2 - m1) * hue / 60;
      else if (hue < 180)
        r = m2;
      else if (hue < 240)
        r = m1 + (m2 - m1) * (240 - hue) / 60;
      else
        r = m1;
      
      hue = *h;
      while (hue > 360)
        hue -= 360;
      while (hue < 0)
        hue += 360;
      
      if (hue < 60)
        g = m1 + (m2 - m1) * hue / 60;
      else if (hue < 180)
        g = m2;
      else if (hue < 240)
        g = m1 + (m2 - m1) * (240 - hue) / 60;
      else
        g = m1;
      
      hue = *h - 120;
      while (hue > 360)
        hue -= 360;
      while (hue < 0)
        hue += 360;
      
      if (hue < 60)
        b = m1 + (m2 - m1) * hue / 60;
      else if (hue < 180)
        b = m2;
      else if (hue < 240)
        b = m1 + (m2 - m1) * (240 - hue) / 60;
      else
        b = m1;
      
      *h = r;
      *l = g;
      *s = b;
    }
}

/* modified function to darken/lighten given color */
static void
gdk_color_dl (GdkColor *a,
                 GdkColor *b,
                 gdouble   k)
{
  gdouble red;
  gdouble green;
  gdouble blue;
  
  red = (gdouble) a->red / 65535.0;
  green = (gdouble) a->green / 65535.0;
  blue = (gdouble) a->blue / 65535.0;
  rgb_to_hls (&red, &green, &blue);
    
  if ( green > 0.5 )
  		green -= k;
  else 
  		green += k;		
    
  hls_to_rgb (&red, &green, &blue);
  
  b->red = red * 65535.0;
  b->green = green * 65535.0;
  b->blue = blue * 65535.0;
}


/**
*	bf_textview_recolor:
*	@self:  BfTextView widget 
*
*	Apply color changes to view 
* 	
*/



void bf_textview_recolor(BfTextView *view, gchar *fg_color, gchar *bg_color )
{
	gchar *str;
	GdkColor c1,c2,c3,c4;
	GtkRcStyle *style;
	
	g_stpcpy(view->fg_color,fg_color);
	g_stpcpy(view->bkg_color,bg_color);
	gdk_color_parse(fg_color,&c1);
	gdk_color_parse(bg_color,&c2);
	gdk_color_dl(&c1,&c3,0.07);
	gdk_color_dl(&c2,&c4,0.07);
	str = g_strdup_printf(
	          "style \"bfish\" {\nGtkWidget::cursor_color=\"%s\"\nbase[NORMAL]=\"%s\"\nbase[ACTIVE]=\"%s\"\ntext[NORMAL]=\"%s\"\ntext[ACTIVE]=\"%s\"\nfg[NORMAL]=\"%s\"\nfg[ACTIVE]=\"%s\"\nbg[NORMAL]=\"%s\"\nbg[ACTIVE]=\"%s\"\n}\nclass \"BfTextView\" style \"bfish\"",
				 view->fg_color, view->bkg_color, view->bkg_color,view->fg_color,view->fg_color,
				  gdk_color_to_hexstring(&c1,FALSE),gdk_color_to_hexstring(&c3,FALSE),
				  gdk_color_to_hexstring(&c4,FALSE),gdk_color_to_hexstring(&c4,FALSE));
	gtk_rc_parse_string(str);			
	g_free(str);
	style = gtk_widget_get_modifier_style(GTK_WIDGET(view));
	gtk_widget_modify_style(GTK_WIDGET(view),style);
}



/**
*	bf_textview_get_nearest_block:
*	@self:  BfTextView widget 
* @iter: - GtkTextIter indicating begin of a search.
*	@backward - if TRUE search is performed backward else forward
*	@mode - how much should we search - look at BF_GNB defines in header file
*	Find block nearest to a given iterator. 
* 	
* Returns: @TBfBlock structure or NULL if block not found.
*/
GtkTextMark *bf_textview_get_nearest_block(BfTextView * self, GtkTextIter * iter, gboolean backward,
										   gint mode, gboolean not_single)
{
	GtkTextMark *mark = bftv_get_block_at_iter(iter);
	GtkTextIter it = *iter, endit;
	gboolean found = FALSE;


	endit = it;
	switch (mode) {
	case BF_GNB_HERE:
		return mark;
	case BF_GNB_LINE:
		if (backward)
			gtk_text_iter_set_line(&endit, gtk_text_iter_get_line(iter));
		else
			gtk_text_iter_forward_to_line_end(&endit);
		break;
	case BF_GNB_DOCUMENT:
		if (backward)
			gtk_text_iter_set_offset(&endit, 0);
		else
			gtk_text_iter_forward_to_end(&endit);
		break;
	case BF_GNB_CHAR:
		if (backward)
			gtk_text_iter_backward_char(&endit);
		else
			gtk_text_iter_forward_char(&endit);
		break;
	}

	if (backward) {
		gboolean cont = TRUE;
		while (!found && !gtk_text_iter_compare(&it, &endit) >= 0 && cont) {
			/* I (Olivier) have added a check for the return value if this function, because there is 
			   an indefinite loop somewhere here */
			cont = gtk_text_iter_backward_char(&it);
			mark = bftv_get_block_at_iter(&it);
			if (mark) {
				if (not_single) {
					if (g_object_get_data(G_OBJECT(mark), "single-line") == &tid_false)
						found = TRUE;
				} else
					found = TRUE;
			}
		}						/* while */
	} else {
		gboolean cont = TRUE;
		while (!found && !gtk_text_iter_compare(&it, &endit) <= 0 && cont) {
			/* I (Olivier) have added a check for the return value if this function, because there is 
			   an indefinite loop somewhere here */
			cont = gtk_text_iter_forward_char(&it);
			mark = bftv_get_block_at_iter(&it);
			if (mark) {
				if (not_single) {
					if (g_object_get_data(G_OBJECT(mark), "single-line") == &tid_false)
						found = TRUE;
				} else
					found = TRUE;
			}
		}						/* while */
	}
	return mark;
}

GtkTextMark *bf_textview_get_nearest_block_of_type(BfTextView * self, BfLangBlock * block,
												   GtkTextIter * iter, gboolean backward, gint mode,
												   gboolean not_single)
{
	GtkTextMark *mark = NULL, *prev_mark = NULL;
	GtkTextIter it;
	mark = bf_textview_get_nearest_block(self, iter, backward, mode, not_single);
	if (mark)
		gtk_text_buffer_get_iter_at_mark(gtk_text_iter_get_buffer(iter), &it, mark);
	if (mark && g_object_get_data(G_OBJECT(mark), "info") == block)
		return mark;
	while (prev_mark != mark) {
		prev_mark = mark;
		mark = bf_textview_get_nearest_block(self, &it, backward, mode, not_single);
		if (mark)
			gtk_text_buffer_get_iter_at_mark(gtk_text_iter_get_buffer(iter), &it, mark);
		if (mark && g_object_get_data(G_OBJECT(mark), "info") == block)
			return mark;
	}
	return NULL;
}



void bf_textview_set_highlight(BfTextView * self, gboolean on)
{
	self->highlight = on;
}

void bf_textview_set_match_blocks(BfTextView * self, gboolean on)
{
	self->match_blocks = on;
}

void bf_textview_set_mark_tokens(BfTextView * self, gboolean on)
{
	self->mark_tokens = on;
}


/*
 *      ---------------------------------------------------------------------------------------------------------------
 *                                                                              LANG MANAGER
 *  ---------------------------------------------------------------------------------------------------------------
 */

BfLangManager *bf_lang_mgr_new()
{
	BfLangManager *ret = g_new0(BfLangManager, 1);
	ret->languages = g_hash_table_new(g_str_hash, g_str_equal);
	return ret;
}

BfLangConfig *bf_lang_mgr_load_config(BfLangManager * mgr, const gchar * filename)
{
	gchar *fname, *fname1, *fname2;
	BfLangConfig *cfg = NULL;

	fname1 = g_strconcat(PKGDATADIR, filename, NULL);
	fname2 = g_strconcat(g_get_home_dir(), "/." PACKAGE "/", filename, NULL);
	fname = return_first_existing_filename(fname1, fname2, NULL);
	if (fname) {
		cfg = bftv_load_config(fname, filename);
		if (cfg != NULL) {
			bftv_make_config_tables(cfg);
			DEBUG_MSG("bf_lang_mgr_load_config, adding %s to hashtable\n", filename);
			g_hash_table_replace(mgr->languages, (gpointer) filename, cfg);
		}
		g_free(fname);
	}
	g_free(fname1);
	g_free(fname2);
	return (cfg);
}


BfLangConfig *bf_lang_mgr_get_config(BfLangManager * mgr, const gchar * filename)
{
	return g_hash_table_lookup(mgr->languages, filename);
}

/*
*				Auxiliary functions 
*/

static void bftv_ins_key(gpointer key, gpointer value, gpointer udata)
{
	GList **lst = (GList **) udata;
	*lst = g_list_append(*lst, key);
}


GList *bf_lang_get_groups(BfLangConfig * cfg)
{
	GList *lst = NULL;
	g_hash_table_foreach(cfg->groups, bftv_ins_key, &lst);
	return lst;
}

typedef struct {
	GList **list;
	guchar *grpcrit;
} Thf;

static void bftv_ins_block(gpointer key, gpointer value, gpointer udata)
{
	Thf *d = (Thf *) udata;
	BfLangBlock *t = (BfLangBlock *) value;
	if (d->grpcrit == NULL && t->group == NULL) {
		if (xmlStrcmp(t->id, (const xmlChar *) "_tag_begin_") != 0)
			*(d->list) = g_list_append(*(d->list), t->id);
	} else if (d->grpcrit != NULL && t->group != NULL && xmlStrcmp(t->group, d->grpcrit) == 0) {
		if (xmlStrcmp(t->id, (const xmlChar *) "_tag_begin_") != 0)
			*(d->list) = g_list_append(*(d->list), t->id);
	}
}

/* If you want to get list of "unaligned" blocks 
*  i.e. blocks which are defined without a group - 
*  just use NULL for group param 
*/
GList *bf_lang_get_blocks_for_group(BfLangConfig * cfg, guchar * group)
{
	GList *lst = NULL;
	Thf *hf = g_new0(Thf, 1);

	hf->list = &lst;
	hf->grpcrit = group;
	g_hash_table_foreach(cfg->blocks, bftv_ins_block, hf);
	g_free(hf);
	return lst;
}


static void bftv_ins_token(gpointer key, gpointer value, gpointer udata)
{
	Thf *d = (Thf *) udata;
	BfLangToken *t = (BfLangToken *) value;
	if (d->grpcrit == NULL && t->group == NULL) {
		if (xmlStrcmp(t->name, (const xmlChar *) "_tag_end_") != 0 && xmlStrcmp(t->name, (const xmlChar *) "_attr_") != 0
			&& xmlStrcmp(t->name, (const xmlChar *) "_attr2_") != 0 && xmlStrcmp(t->name, (const xmlChar *) "_attr_tag_begin_end_") != 0
			&& xmlStrcmp(t->name, (const xmlChar *) "_fake_ident_") != 0)
			*(d->list) = g_list_append(*(d->list), t->name);
	} else if (d->grpcrit != NULL && t->group != NULL && xmlStrcmp(t->group, d->grpcrit) == 0) {
		if (xmlStrcmp(t->name, (const xmlChar *) "_tag_end_") != 0 && xmlStrcmp(t->name, (const xmlChar *) "_attr_") != 0
			&& xmlStrcmp(t->name, (const xmlChar *) "_attr2_") != 0 && xmlStrcmp(t->name, (const xmlChar *) "_attr_tag_begin_end_") != 0
			&& xmlStrcmp(t->name, (const xmlChar *) "_fake_ident_") != 0)
			*(d->list) = g_list_append(*(d->list), t->name);
	}
}

/* If you want to get list of "unaligned" tokens 
*  i.e. tokens which are defined without a group - 
*  just use NULL for group param 
*/
GList *bf_lang_get_tokens_for_group(BfLangConfig * cfg, guchar * group)
{
	GList *lst = NULL;
	Thf *hf = g_new0(Thf, 1);

	hf->list = &lst;
	hf->grpcrit = group;
	g_hash_table_foreach(cfg->tokens, bftv_ins_token, hf);
	g_free(hf);
	return lst;
}


gboolean bf_lang_needs_tags(BfLangConfig * cfg)
{
	return cfg->scan_tags;
}

static void bflang_retag_token(gpointer key, gpointer value, gpointer udata)
{
	BfLangToken *t = (BfLangToken*)value;
	BfLangConfig *cfg = (BfLangConfig*)udata;
	t->tag = get_tag_for_scanner_style((gchar *) cfg->name, "t", (gchar *) t->name, NULL);
	if (!t->tag) {
		t->tag = get_tag_for_scanner_style((gchar *) cfg->name, "g", (gchar *) t->group, NULL);
	}
}

static void bflang_retag_block(gpointer key, gpointer value, gpointer udata)
{
	BfLangBlock *b = (BfLangBlock *) value;
	BfLangConfig *cfg = (BfLangConfig*)udata;
	b->tag = get_tag_for_scanner_style((gchar *) cfg->name, "b", (gchar *) b->id, NULL);
	if (!b->tag) {
		b->tag = get_tag_for_scanner_style((gchar *) cfg->name, "g", (gchar *) b->group, NULL);
	}
}

static void bf_lang_retag(gpointer key, gpointer value, gpointer udata)
{
	BfLangConfig *cfg = (BfLangConfig *)value;
	g_hash_table_foreach(cfg->tokens, bflang_retag_token, cfg);
	g_hash_table_foreach(cfg->blocks, bflang_retag_block, cfg);
	cfg->tag_begin = get_tag_for_scanner_style((gchar *) cfg->name, "m", "tag_begin", NULL);
	cfg->tag_end = get_tag_for_scanner_style((gchar *) cfg->name, "m", "tag_end", NULL);
	cfg->attr_name = get_tag_for_scanner_style((gchar *) cfg->name, "m", "attr_name", NULL);
	cfg->attr_val = get_tag_for_scanner_style((gchar *) cfg->name, "m", "attr_val", NULL);

	/* we should perhaps also retag some of the internal tags such as _block_match_ ? */
}

void bf_lang_mgr_retag(void) {
	g_hash_table_foreach(main_v->lang_mgr->languages, bf_lang_retag, NULL);
}

#endif							/* USE_SCANNER */
