/* Bluefish HTML Editor
 * bftextview2.c
 *
 * Copyright (C) 2008-2023 Olivier Sessink
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
/* indented with indent -ts4 -kr -l110 */
/* for the design docs see bftextview2.h */

#include <math.h>				/* log10() */
#include <string.h>				/* strlen() */
#include <locale.h>

#include "bftextview2.h"
#include "bftextview2_private.h"

#include "bluefish.h"
#include "bf_lib.h"
#include "bookmark.h"
#include "document.h"
#include "undo_redo.h"
#include "doc_text_tools.h"
#include "bfwin.h"
#include "bftextview2_scanner.h"
#include "bftextview2_patcompile.h"
#include "bftextview2_autocomp.h"
#include "bftextview2_langmgr.h"
#ifdef HAVE_LIBENCHANT
#include "bftextview2_spell.h"
#endif
#ifdef MARKREGION
#include "bftextview2_markregion.h"
#endif

/*#undef DEBUG_MSG
#define DEBUG_MSG g_print*/
/*#undef DBG_SIGNALS
#define DBG_SIGNALS g_print*/
/*#undef DBG_SCANCACHE
#define DBG_SCANCACHE g_print
#undef DBG_SCANNING
#define DBG_SCANNING g_print
#undef DBG_AUTOCOMP
#define DBG_AUTOCOMP g_print*/

#define USER_IDLE_EVENT_INTERVAL 480	/* milliseconds */

G_DEFINE_TYPE(BluefishTextView, bluefish_text_view, GTK_TYPE_TEXT_VIEW)
#if GTK_CHECK_VERSION(3,0,0)
static GdkRGBA st_whitespace_color, st_cline_color, st_cursor_highlight_color, st_margin_fg_color,
	st_margin_bg_color;
#else
static GdkColor st_whitespace_color, st_cline_color, st_cursor_highlight_color, st_margin_fg_color,
	st_margin_bg_color;
#endif

/****************************** utility functions ******************************/

const gchar *bluefish_text_view_get_lang_name(BluefishTextView * btv)
{
	if (!btv)
		return NULL;
	if (!btv->bflang)
		return NULL;
	return btv->bflang->name;
}

gboolean character_is_symbol(Tscantable * st, guint16 context, gunichar uc)
{
	if (uc > 127)
		return FALSE;
	if (!st) {
		return FALSE;
	}
	if (context > st->contexts->len) {
		return FALSE;
	}
	return (g_array_index((GArray *) g_array_index(st->contexts, Tcontext, context).table, Ttablerow, 1).row
			[uc] != 1);
}

static inline gboolean is_symbol(BluefishTextView * btv, gint contextnum, gunichar uc)
{
	if (G_UNLIKELY(uc > 127))
		return FALSE;
	return character_is_symbol(((Tscantable *) btv->bflang->st), contextnum, uc);
}

gchar *bf_get_identifier_at_iter(BluefishTextView * btv, GtkTextIter * iter, gint * contextnum)
{
	GQueue *contextstack;
	GtkTextIter so, eo;
	g_assert(btv == btv->master);
	so = eo = *iter;
	if (!btv->bflang || !btv->bflang->st) {
		while (gtk_text_iter_backward_char(&so) && !g_unichar_isspace(gtk_text_iter_get_char(&so))) {
		};
		gtk_text_iter_forward_char(&so);
		while (!g_unichar_isspace(gtk_text_iter_get_char(&eo)) && gtk_text_iter_forward_char(&eo)) {
		};

		return gtk_text_buffer_get_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(btv)), &so, &eo, TRUE);
	}
	contextstack = get_contextstack_at_position(btv, iter);
	if (g_queue_get_length(contextstack) > 0)
		*contextnum = GPOINTER_TO_INT(g_queue_peek_head(contextstack));
	else
		*contextnum = 1;

	while (gtk_text_iter_backward_char(&so) && !is_symbol(btv, *contextnum, gtk_text_iter_get_char(&so))) {
		/*g_print("evaluating char %c\n",gtk_text_iter_get_char(&so)); */
	};
	gtk_text_iter_forward_char(&so);
	while (gtk_text_iter_forward_char(&eo) && !is_symbol(btv, *contextnum, gtk_text_iter_get_char(&eo))) {
		/*g_print("evaluating char %c\n",gtk_text_iter_get_char(&so)); */
	};
	return gtk_text_buffer_get_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(btv)), &so, &eo, TRUE);
}


gboolean
bf_text_iter_line_start_of_text(BluefishTextView * btv, GtkTextIter * iter, GtkTextIter * realstart,
								gboolean use_wrapped)
{
	gboolean ret;
	/*g_print("bf_text_iter_line_start_of_text, started at offset %d\n",gtk_text_iter_get_offset(iter)); */
	if (use_wrapped && gtk_text_view_get_wrap_mode(GTK_TEXT_VIEW(btv)) != GTK_WRAP_NONE) {
		ret = gtk_text_view_backward_display_line_start(GTK_TEXT_VIEW(btv), iter);
		if (!ret) {
			/*g_print("bf_text_iter_line_start_of_text, failed to move iter to display_line_start, return FALSE\n"); */
			return FALSE;
		}
		/*g_print("bf_text_iter_line_start_of_text, line offset=%d\n",gtk_text_iter_get_line_offset(iter)); */
		*realstart = *iter;
		if (gtk_text_iter_get_line_offset(iter) > 0) {
			/* we have wrapped text, and we found the start of a wrapped part, so this is not the start of the real line */
			return TRUE;
		}
	} else {
		/*g_print("bf_text_iter_line_start_of_text, no wrap, or no-want-wrap, set line offset to 0\n"); */
		gtk_text_iter_set_line_offset(iter, 0);
		*realstart = *iter;
	}
	/* do the magic so we can toggle between the line start and the start of the text */
	ret = TRUE;
	while (ret && g_unichar_isspace(gtk_text_iter_get_char(iter))) {
		if (gtk_text_iter_ends_line(iter))
			return FALSE;
		ret = gtk_text_iter_forward_char(iter);
	}
	DEBUG_MSG("bf_text_iter_line_start_of_text, end of function, ret=%d, line offset=%d, offset=%d\n", ret,
			  gtk_text_iter_get_line_offset(iter), gtk_text_iter_get_offset(iter));
	return ret;
}

/* if BluefishTextView *btv is NULL we do not check for the wrap mode, and the function will always
return the real line, not the wrapped part  */
gboolean
bf_text_iter_line_end_of_text(BluefishTextView * btv, GtkTextIter * iter, GtkTextIter * realend,
							  gboolean use_wrapped)
{
	gboolean ret;
	if (use_wrapped && gtk_text_view_get_wrap_mode(GTK_TEXT_VIEW(btv)) != GTK_WRAP_NONE) {
		ret = gtk_text_view_forward_display_line_end(GTK_TEXT_VIEW(btv), iter);
		if (!ret) {
			/*g_print("bf_text_iter_line_end_of_text, failed to move iter to display_line_end, return FALSE\n"); */
			return FALSE;
		}
		/*g_print("bf_text_iter_line_end_of_text, line offset=%d\n",gtk_text_iter_get_line_offset(iter)); */
		*realend = *iter;
		if (!gtk_text_iter_ends_line(iter)) {
			/* we have wrapped text, and we found the end of a wrapped part, so this is not the end of the real line */
			return TRUE;
		}
	}

	if (!gtk_text_iter_ends_line(iter)) {
		gtk_text_iter_forward_to_line_end(iter);
	}
	if (gtk_text_iter_starts_line(iter)) {
		return FALSE;
	}
	*realend = *iter;
	if (gtk_text_iter_is_end(iter))
		gtk_text_iter_backward_char(iter);
	ret = TRUE;
	while (ret && g_unichar_isspace(gtk_text_iter_get_char(iter))) {
		if (gtk_text_iter_starts_line(iter))
			return FALSE;
		ret = gtk_text_iter_backward_char(iter);
	}
	if (ret && !gtk_text_iter_ends_line(iter) && !g_unichar_isspace(gtk_text_iter_get_char(iter)))
		gtk_text_iter_forward_char(iter);
	return ret;
}

/*************************** end of utility functions **************************/
static gboolean bftextview2_user_idle_timer(gpointer data)
{
	BluefishTextView *btv = data;
	guint elapsed = (guint) (1000.0 * g_timer_elapsed(btv->user_idle_timer, NULL));
	if ((elapsed + 20) >= USER_IDLE_EVENT_INTERVAL) {	/* avoid delaying again for less than 20 milliseconds */
		DBG_AUTOCOMP
			("bftextview2_user_idle_timer, user is > %d milliseconds idle, autocomp=%d, mode=%d, needs_autocomp=%d\n",
			 elapsed, BLUEFISH_TEXT_VIEW(btv->master)->auto_complete, main_v->props.autocomp_popup_mode,
			 btv->needs_autocomp);
		if (BLUEFISH_TEXT_VIEW(btv->master)->auto_complete && btv->needs_autocomp
			&& main_v->props.autocomp_popup_mode == 0) {
			autocomp_run(btv, FALSE);
			DBG_AUTOCOMP("bftextview2_user_idle_timer, after run, set needs_autocomp to FALSE\n");
			btv->needs_autocomp = FALSE;
		}
		btv->user_idle = 0;
		return FALSE;
	} else {
		guint nextcheck;
		nextcheck = USER_IDLE_EVENT_INTERVAL - elapsed;
		DBG_AUTOCOMP
			("bftextview2_user_idle_timer, not yet elapsed (%d milliseconds idle), rechecking in %d milliseconds\n",
			 elapsed, nextcheck);
		btv->user_idle = g_timeout_add(nextcheck, bftextview2_user_idle_timer, btv);
		return FALSE;
	}
}

static void bftextview2_reset_user_idle_timer(BluefishTextView * btv)
{
	DBG_AUTOCOMP("bftextview2_reset_user_idle_timer: timer reset\n");
	g_timer_start(btv->user_idle_timer);	/* the timer is used both for delayed scanning AND for the delayed popup */
	if (btv->user_idle == 0 && main_v->props.autocomp_popup_mode == 0 /*&&btv->autocomp */ ) {
		btv->user_idle = g_timeout_add(USER_IDLE_EVENT_INTERVAL, bftextview2_user_idle_timer, btv);
		DBG_AUTOCOMP("started user_idle timeout with event source %d and timeout %d\n", btv->user_idle,
					 USER_IDLE_EVENT_INTERVAL);
	}
}

static void bftextview2_set_margin_size(BluefishTextView * btv);
static gboolean bftextview2_scanner_idle(gpointer data);

static gboolean bftextview2_scanner_scan(BluefishTextView * btv, gboolean in_idle)
{
	if (!btv->bflang) {
		btv->scanner_idle = 0;
		btv->scanner_immediate = 0;
		btv->scanner_delayed = 0;
		DBG_MSG("bftextview2_scanner_scan, no bflang, return FALSE\n");
		return FALSE;
	}
	if (!btv->bflang->st
#ifdef HAVE_LIBENCHANT
		&& !btv->spell_check
#endif
		) {
		DBG_MSG("bftextview2_scanner_scan, no scantable or no spellcheck, return FALSE\n");
		btv->scanner_idle = 0;
		btv->scanner_immediate = 0;
		btv->scanner_delayed = 0;
		return FALSE;
	}
	DBG_DELAYSCANNING("bftextview2_scanner_scan, start scanning\n");
	{
		DBG_DELAYSCANNING
			("bftextview2_scanner_idle, running scanner idle function, scanner_idle=%d, scanner_immediate=%d\n",
			 btv->scanner_idle, btv->scanner_immediate);
		if (!(btv->enable_scanner && bftextview2_run_scanner(btv, NULL))
#ifdef HAVE_LIBENCHANT
			&& !bftextview2_run_spellcheck(btv)
#endif
			) {
			btv->scanner_idle = 0;
			btv->scanner_immediate = 0;
			DBG_DELAYSCANNING("bftextview2_scanner_idle, stopping scanner idle function\n");
			bftextview2_set_margin_size(btv);
			return FALSE;
		} else if (btv->scanner_immediate) {
			DBG_DELAYSCANNING
				("bftextview2_scanner_idle, stop immediate priority callback, start idle priority callback, priority=%d\n",
				 SCANNING_IDLE_AFTER_TIMEOUT_PRIORITY);
			btv->scanner_immediate = 0;
			btv->scanner_idle =
				g_idle_add_full(SCANNING_IDLE_AFTER_TIMEOUT_PRIORITY, bftextview2_scanner_idle, btv, NULL);
			DBG_DELAYSCANNING("bftextview2_scanner_idle, idle priority callback at %d\n", btv->scanner_idle);
			return FALSE;
		}
	}
	DBG_DELAYSCANNING("bftextview2_scanner_scan, end of function, return TRUE\n");
	return TRUE;				/* call me again */
}

static gboolean bftextview2_scanner_idle(gpointer data)
{
	BluefishTextView *btv = data;
#ifdef DEBUG_SIGNALS
	if (btv->scanner_immediate) {
		g_print("bftextview2_scanner_idle, immediate, priority=%d\n", SCANNING_IDLE_PRIORITY);
	} else {
		g_print("bftextview2_scanner_idle, idle, priority=%d\n", SCANNING_IDLE_AFTER_TIMEOUT_PRIORITY);
	}
#endif
	DBG_DELAYSCANNING("bftextview2_scanner_idle callback started\n");
	if (!btv->enable_scanner
#ifdef HAVE_LIBENCHANT
		&& !btv->spell_check
#endif
		) {
		btv->scanner_idle = 0;
		btv->scanner_immediate = 0;
		return FALSE;
	}
	return bftextview2_scanner_scan(btv, TRUE);
}

void bftextview2_schedule_scanning(BluefishTextView * btv)
{
	DBG_MSG
		("bftextview2_schedule_scanning, enable=%d, spell_check=%d, bflang=%p,scanner_idle=%d,scanner_immediate=%d\n",
		 BLUEFISH_TEXT_VIEW(btv->master)->enable_scanner,
#ifdef HAVE_LIBENCHANT
		 BLUEFISH_TEXT_VIEW(btv->master)->spell_check,
#else
		 0
#endif
		 BLUEFISH_TEXT_VIEW(btv->master)->bflang,
		 BLUEFISH_TEXT_VIEW(btv->master)->scanner_idle, BLUEFISH_TEXT_VIEW(btv->master)->scanner_immediate);
	if (BLUEFISH_TEXT_VIEW(btv->master)->scanner_idle) {
		DBG_MSG("bftextview2_schedule_scanning, remove scanning_idle callback\n");
		g_source_remove(BLUEFISH_TEXT_VIEW(btv->master)->scanner_idle);
		BLUEFISH_TEXT_VIEW(btv->master)->scanner_idle = 0;
	}

	if ((BLUEFISH_TEXT_VIEW(btv->master)->enable_scanner
#ifdef HAVE_LIBENCHANT
		 || BLUEFISH_TEXT_VIEW(btv->master)->spell_check
#endif
		)
		&& BLUEFISH_TEXT_VIEW(btv->master)->bflang
		&& BLUEFISH_TEXT_VIEW(btv->master)->scanner_idle == 0
		&& BLUEFISH_TEXT_VIEW(btv->master)->scanner_immediate == 0) {
		DBG_DELAYSCANNING("scheduling scanning in idle function with priority %d\n", SCANNING_IDLE_PRIORITY);
		BLUEFISH_TEXT_VIEW(btv->master)->scanner_immediate =
			g_idle_add_full(SCANNING_IDLE_PRIORITY, bftextview2_scanner_idle, btv->master, NULL);
	}
	DBG_DELAYSCANNING("bftextview2_schedule_scanning, scanner_immediate=%d\n",
					  BLUEFISH_TEXT_VIEW(btv->master)->scanner_immediate);
}

static void
bftextview2_get_iters_at_foundblock(GtkTextBuffer * buffer, Tfoundblock * fblock,
									GtkTextIter * it1, GtkTextIter * it2,
									GtkTextIter * it3, GtkTextIter * it4)
{
	gtk_text_buffer_get_iter_at_offset(buffer, it1, fblock->start1_o);
	gtk_text_buffer_get_iter_at_offset(buffer, it2, fblock->end1_o);
	gtk_text_buffer_get_iter_at_offset(buffer, it3, fblock->start2_o);
	gtk_text_buffer_get_iter_at_offset(buffer, it4, fblock->end2_o);
}

/*
static inline Tfoundblock *
bftextview2_get_block_at_offset(BluefishTextView * btv, Tfound **found, guint offset)
{
	GSequenceIter *siter;
	Tfound *rfound;
	rfound = get_foundcache_at_offset(btv, offset, &siter);
	*found = rfound;
	while (rfound) {
		DBG_BLOCKMATCH("bftextview2_get_block_at_offset, found %p at offset %d with blockchange %d contextchange %d\n", rfound, rfound->charoffset_o, rfound->numblockchange, rfound->numcontextchange);
		if (IS_FOUNDMODE_BLOCKPUSH((rfound))
				&& ((rfound)->fblock->start1_o == offset || (rfound)->fblock->end1_o == offset)) {
			*found = rfound;
			return (rfound)->fblock;
		} else if ((rfound)->numblockchange < 0) {
			/ * TODO: if multiple blocks are popped, usually the last popped one if the one that matches thje end-of-block-tag
			so that block should be returned * /
			Tfoundblock *tmpfblock = pop_blocks((rfound)->numblockchange+1, (rfound)->fblock);
			DBG_BLOCKMATCH("bftextview2_get_block_at_offset, found->fblock=%p, tmpfblock=%p\n",rfound->fblock,tmpfblock);
			if (tmpfblock && (tmpfblock->start2_o == offset || tmpfblock->end2_o == offset)) {
				*found = rfound;
				return tmpfblock;
			}
		}
		if ((rfound)->charoffset_o > offset)
			break;
		*found = rfound;
		rfound = get_foundcache_next(btv, &siter);
	}
	return NULL;
}*/

static Tfoundblock *first_fully_defined_block(Tfoundblock * fblock)
{
	while (fblock && fblock->start2_o == BF_OFFSET_UNDEFINED) {
		fblock = fblock->parentfblock;
	}
	return fblock;
}


/* if innerblock is TRUE we only return a block that we are between the matches, if innerblock is FALSE
we might return a block if we are within the startmatch or within the end match */
static Tfoundblock *bftextview2_get_active_block_at_offset(BluefishTextView * btv, gboolean innerblock,
														   guint offset)
{
	GSequenceIter *siter;
	Tfound *found1, *found2;
	Tfoundblock *fblock;
	found1 = get_foundcache_at_offset(btv, offset, &siter);
	if (!found1)
		return NULL;
	DEBUG_MSG("offset=%d, got found1 %p with offset %d and found1->fblock %p\n", offset, found1,
			  found1->charoffset_o, found1->fblock);
	if (innerblock) {
		if (!found1->fblock) {
			DEBUG_MSG
				("bftextview2_get_active_block_at_offset, found1 does not have an fblock, return NULL\n");
			return NULL;
		}
		fblock = first_fully_defined_block(found1->fblock);
		/* when innerblock is requested we have to check for the situation that we are in the middle of the end-of-block-match
		   because it means that we are outside the innerblock already, and thus we need the parent */
		if (fblock && found1->numblockchange < 0 && fblock->start2_o < offset) {
			DEBUG_MSG("bftextview2_get_active_block_at_offset, in end-of-block match, return parent\n");
			return fblock->parentfblock;
		}
	} else {
		/* when outerblock is requested we have to check if we are in the middle of a new start-of-block
		   which is stored in the next Tfound in the scancache */
		found2 = get_foundcache_next(btv, &siter);
		if (found2 && found2->numblockchange > 0 && found2->fblock->start1_o <= offset
			&& found2->fblock->start2_o != BF_OFFSET_UNDEFINED) {
			return found2->fblock;
		}
	}
	if (found1->numblockchange < 0) {
		fblock = first_fully_defined_block(found1->fblock);
		if (!fblock)
			return NULL;
		/* if outerblock is requested, we have to check for the situation that we are exactly at the end-of-block
		   in which case we don't have to pop the block yet */
		if (fblock->end2_o == offset) {
			return fblock;
		}
		DEBUG_MSG("return %d blocks popped from found1\n", found1->numblockchange);
		return pop_blocks(found1->numblockchange, found1->fblock);
	}
	DEBUG_MSG("return found1->fblock\n");
	return first_fully_defined_block(found1->fblock);
}

gboolean
bluefish_text_view_get_active_block_boundaries(BluefishTextView * btv, guint location, gboolean innerblock,
											   GtkTextIter * so, GtkTextIter * eo)
{
	GtkTextIter it1, it2;
	Tfoundblock *fblock = bftextview2_get_active_block_at_offset(btv, innerblock, location);
	if (!fblock)
		return FALSE;
	DEBUG_MSG("bluefish_text_view_get_active_block_boundaries, got block %p %d:%d-%d:%d\n",
			  fblock, fblock->start1_o, fblock->end1_o, fblock->start2_o, fblock->end2_o);
	fblock = first_fully_defined_block(fblock);
	if (!fblock)
		return FALSE;
	DEBUG_MSG("bluefish_text_view_get_active_block_boundaries, got fully defined block %p %d:%d-%d:%d\n",
			  fblock, fblock->start1_o, fblock->end1_o, fblock->start2_o, fblock->end2_o);
	if (innerblock)
		bftextview2_get_iters_at_foundblock(btv->buffer, fblock, &it1, so, eo, &it2);
	else
		bftextview2_get_iters_at_foundblock(btv->buffer, fblock, so, &it1, &it2, eo);
	return TRUE;
}

gboolean
bluefish_text_view_get_active_identifier(BluefishTextView * btv, GtkTextIter * currentlocation,
										 GtkTextIter * so, GtkTextIter * eo)
{
	BluefishTextView *master = BLUEFISH_TEXT_VIEW(btv->master);
	guint patnum;
	gint contextnum;
	GtkTextIter mstart;

	mstart = *currentlocation;
	gtk_text_iter_set_line_offset(&mstart, 0);
	patnum = scan_for_identifier_at_position(master, &mstart, currentlocation, &contextnum, so, eo);
	return (patnum != FALSE);
}

static gchar *blockstack_string(BluefishTextView * btv, Tfoundblock * fblock, gint *level)
{
	GString *tmp;
	Tfoundblock *parent;
	if (!fblock)
		return NULL;

	parent = fblock;
	tmp = g_string_new("");
	while (parent) {
		(*level)++;
		if (parent->start2_o != BF_OFFSET_UNDEFINED) {
			if (parent != fblock)
				tmp = g_string_prepend(tmp, " ");
			if (g_array_index
				(BLUEFISH_TEXT_VIEW(btv->master)->bflang->st->matches, Tpattern,
				 parent->patternum).is_regex) {
				GtkTextIter it1, it2;
				gchar *tmp2;
				gtk_text_buffer_get_iter_at_offset(btv->buffer, &it1, parent->start1_o);
				gtk_text_buffer_get_iter_at_offset(btv->buffer, &it2, parent->end1_o);
				tmp2 = gtk_text_buffer_get_text(btv->buffer, &it1, &it2, TRUE);
				tmp = g_string_prepend(tmp, tmp2);
				g_free(tmp2);
			} else {
				tmp =
					g_string_prepend(tmp,
									 g_array_index(BLUEFISH_TEXT_VIEW(btv->master)->bflang->st->matches,
												   Tpattern, parent->patternum).pattern);
			}
		}
		parent = parent->parentfblock;
	}
	return g_string_free(tmp, FALSE);
}

gboolean foundblock_is_at_boundary_location(Tfoundblock *fblock, guint offset) {
	if (fblock->start2_o != BF_OFFSET_UNDEFINED
			&& (fblock->start1_o == offset || fblock->end1_o == offset || fblock->start2_o == offset
				|| fblock->end2_o == offset)) {
		return TRUE;
	}
	return FALSE;
}

/* because we don't want to expose the Tfoundblock to the external API we return gpointer
external functions should treat this as a boolean: NULL means there is no block, a pointer means there is a block
*/
gpointer
bftextview2_get_block_at_boundary_location(BluefishTextView * btv, guint offset, GtkTextIter * it1,
										   GtkTextIter * it2, GtkTextIter * it3, GtkTextIter * it4)
{
	Tfoundblock *fblock;

	fblock = bftextview2_get_active_block_at_offset(btv->master, FALSE, offset);
	if (fblock)
		fblock = first_fully_defined_block(fblock);
	if (fblock && foundblock_is_at_boundary_location(fblock, offset)) {
		bftextview2_get_iters_at_foundblock(btv->buffer, fblock, it1, it2, it3, it4);
		return fblock;
	}
	return NULL;
}

static gboolean mark_set_idle_lcb(gpointer widget)
{
	BluefishTextView *btv = widget;
	BluefishTextView *master = btv->master;
	GtkTextIter it1, it2, it3, it4, location;
	Tfoundblock *fblock;
	gint level=0;
	gchar *tmpstr = NULL;
	guint offset;

	gtk_text_buffer_get_iter_at_mark(btv->buffer, &location, gtk_text_buffer_get_insert(btv->buffer));
	if (btv->showing_blockmatch || main_v->props.highlight_cursor) {
		gtk_text_buffer_get_bounds(btv->buffer, &it1, &it2);
		gtk_text_buffer_remove_tag(btv->buffer, master->blockmatch, &it1, &it2);
		btv->showing_blockmatch = FALSE;
	}

	offset = gtk_text_iter_get_offset(&location);
	fblock = bftextview2_get_active_block_at_offset(btv->master, FALSE, offset);
	if (fblock)
		fblock = first_fully_defined_block(fblock);
	if (fblock) {
		if (foundblock_is_at_boundary_location(fblock, offset)) {
			bftextview2_get_iters_at_foundblock(btv->buffer, fblock, &it1, &it2, &it3, &it4);
			gtk_text_buffer_apply_tag(btv->buffer, master->blockmatch, &it1, &it2);
			gtk_text_buffer_apply_tag(btv->buffer, master->blockmatch, &it3, &it4);
			btv->showing_blockmatch = TRUE;
		}
		tmpstr = blockstack_string(btv, fblock, &level);
	}

	if (BFWIN(DOCUMENT(master->doc)->bfwin)->session->view_blockstack && tmpstr) {
		bfwin_statusbar_message(DOCUMENT(master->doc)->bfwin, tmpstr, 2);
	}
	g_free(tmpstr);
	doc_set_statusbar_lncol(DOCUMENT(master->doc), level);
	btv->mark_set_idle = 0;
	return FALSE;
}

/* this function slows down scrolling when you hold the cursor pressed, because
it is called for every cursor position change. This function is therefore
an ideal candidate for speed optimization */
static void
bftextview2_mark_set_lcb(GtkTextBuffer * buffer, GtkTextIter * location, GtkTextMark * mark, gpointer widget)
{
	BluefishTextView *btv = BLUEFISH_TEXT_VIEW(widget);
	DBG_SIGNALS("bftextview2_mark_set_lcb, mark=%p (insert=%p,selection=%p), location=%d\n", mark,
				gtk_text_buffer_get_insert(buffer), gtk_text_buffer_get_selection_bound(buffer),
				gtk_text_iter_get_offset(location));
	if (mark && gtk_text_buffer_get_insert(buffer) == mark) {

		if (btv->autocomp)
			autocomp_stop(btv);

		if (BLUEFISH_TEXT_VIEW(btv->master)->show_mbhl) {
			btv->needs_blockmatch = TRUE;
			DBG_BLOCKMATCH("set needs_blockmatch to TRUE for widget %p (master=%p)\n", btv, btv->master);
		}

		if (BLUEFISH_TEXT_VIEW(btv->master)->bflang && BLUEFISH_TEXT_VIEW(btv->master)->bflang->st) {
			if (btv->user_idle) {
				g_source_remove(btv->user_idle);
				btv->user_idle = 0;
			}
		}
	}
}

static void calc_pixels_per_char(BluefishTextView * btv)
{
	PangoLayout *panlay;
	panlay = gtk_widget_create_pango_layout(GTK_WIDGET(btv), "");
	pango_layout_set_text(panlay, "W", -1);
	pango_layout_get_pixel_size(panlay, &btv->margin_pixels_per_char, NULL);
	g_object_unref(G_OBJECT(panlay));
}

static void bftextview2_set_margin_size(BluefishTextView * btv)
{
	gint lines, count, newsize;

	g_assert(btv == btv->master);

	DBG_MSG("bftextview2_set_margin_size, called for %p\n", btv);
	if (BLUEFISH_TEXT_VIEW(btv->master)->margin_pixels_per_char == 0) {
		calc_pixels_per_char(btv->master);
	}
	if (BLUEFISH_TEXT_VIEW(btv->master)->show_line_numbers) {
		lines = gtk_text_buffer_get_line_count(gtk_text_view_get_buffer(GTK_TEXT_VIEW(btv)));
		if (lines >= 100)
			count = 1 + log10(lines);
		else
			count = 2;
		BLUEFISH_TEXT_VIEW(btv->master)->margin_pixels_chars =
			4 + count * BLUEFISH_TEXT_VIEW(btv->master)->margin_pixels_per_char;
	} else {
		BLUEFISH_TEXT_VIEW(btv->master)->margin_pixels_chars = 0;
	}
	if (BLUEFISH_TEXT_VIEW(btv->master)->show_blocks
		&& g_sequence_get_length(BLUEFISH_TEXT_VIEW(btv->master)->scancache.foundcaches) > 0) {
		BLUEFISH_TEXT_VIEW(btv->master)->margin_pixels_block = 12;
	} else {
		BLUEFISH_TEXT_VIEW(btv->master)->margin_pixels_block = 0;
	}
	if (BLUEFISH_TEXT_VIEW(btv->master)->showsymbols) {
		BLUEFISH_TEXT_VIEW(btv->master)->margin_pixels_symbol = 12;
	} else {
		BLUEFISH_TEXT_VIEW(btv->master)->margin_pixels_symbol = 0;
	}
	/*g_print("lines=%d,count=%d,pixels_per_char=%d\n",lines,count,btv->margin_pixels_per_char); */
	newsize = BLUEFISH_TEXT_VIEW(btv->master)->margin_pixels_chars
		+ BLUEFISH_TEXT_VIEW(btv->master)->margin_pixels_block
		+ BLUEFISH_TEXT_VIEW(btv->master)->margin_pixels_symbol;
	gtk_text_view_set_border_window_size(GTK_TEXT_VIEW(btv), GTK_TEXT_WINDOW_LEFT, newsize);
	if (btv->slave)
		gtk_text_view_set_border_window_size(GTK_TEXT_VIEW(btv->slave), GTK_TEXT_WINDOW_LEFT, newsize);
}

static inline gboolean char_in_allsymbols(BluefishTextView * btv, gunichar uc)
{
	if (uc > 127)
		return FALSE;
	if (!btv->bflang)
		return FALSE;
	if (btv->bflang->st)
		return btv->bflang->st->allsymbols[uc];
	else
		return (uc == ' ' || uc == '\t' || uc == '\n');
	return FALSE;
}

static void
bftextview2_insert_text_lcb(GtkTextBuffer * buffer, GtkTextIter * iter, gchar * string,
							gint stringlen, BluefishTextView * btv)
{
	DBG_SIGNALS("bftextview2_insert_text_lcb, btv=%p, master=%p, stringlen=%d\n", btv, btv->master,
				stringlen);
	gchar *wrongquote = "¨";
	guint charlen = g_utf8_strlen(string, stringlen);
	guint startpos = gtk_text_iter_get_offset(iter);

	if (main_v->props.editor_replace_unicode_quotes && charlen == 1 && stringlen == 2
		&& string[0] == wrongquote[0] && string[1] == wrongquote[1]) {
		DBG_MSG("Replace unicode quote ¨ with ascii quote \".\n");
		gtk_text_buffer_insert(buffer, iter, "\"", 1);
		g_signal_stop_emission_by_name(buffer, "insert_text");
	}

	if (btv == btv->master) {
		foundcache_update_offsets(BLUEFISH_TEXT_VIEW(btv->master), startpos, charlen);
	}
}

static void
bftextview2_insert_text_after_lcb(GtkTextBuffer * buffer, GtkTextIter * iter, gchar * string,
								  gint stringlen, BluefishTextView * btv)
{
	GtkTextIter start, end;
	guint charlen = g_utf8_strlen(string, stringlen);
	guint startpos;
	DBG_SIGNALS("bftextview2_insert_text_after_lcb, btv=%p, master=%p, stringlen=%d, string=%s\n", btv,
				btv->master, stringlen, string);
	if (DOCUMENT(BLUEFISH_TEXT_VIEW(btv->master)->doc)->in_paste_operation)
		btv->needs_autocomp = FALSE;
	if (BLUEFISH_TEXT_VIEW(btv->master)->enable_scanner && btv->needs_autocomp
		&& BLUEFISH_TEXT_VIEW(btv->master)->auto_complete && stringlen == 1
		&& (btv->autocomp || main_v->props.autocomp_popup_mode != 0)) {
		DBG_AUTOCOMP("bftextview2_insert_text_after_lcb: call autocomp_run\n");
		autocomp_run(btv, FALSE);
		DBG_AUTOCOMP("bftextview2_insert_text_after_lcb, set needs_autocomp to FALSE\n");
		btv->needs_autocomp = FALSE;
	}

	bftextview2_reset_user_idle_timer(btv);
	bftextview2_set_margin_size(BLUEFISH_TEXT_VIEW(btv->master));

	if (btv != btv->master)
		return;

	if (!main_v->props.reduced_scan_triggers || stringlen > 1
		|| (stringlen == 1 && char_in_allsymbols(btv, string[0]))) {
		bftextview2_schedule_scanning(btv);
	}
	/* mark the text that is changed */
	end = start = *iter;
	gtk_text_iter_backward_chars(&start, charlen);

	DBG_SIGNALS("bftextview2_insert_text_after_lcb: mark text from %d to %d with markregion\n",
				gtk_text_iter_get_offset(&start), gtk_text_iter_get_offset(iter));
	startpos = gtk_text_iter_get_offset(&start);
#ifdef MARKREGION
	markregion_insert(&btv->scanning, startpos, startpos + charlen);
	DBG_MARKREGION("bftextview2_insert_text_after_lcb, apply needscanning to %u:%u\n",
				   gtk_text_iter_get_offset(&start), gtk_text_iter_get_offset(iter));
#ifdef HAVE_LIBENCHANT
	markregion_insert(&btv->spellcheck, startpos, startpos + charlen);
#endif
#endif
	btv->needremovetags = 0;
}

/*static void print_found(Tfound * found)
{
	DBG_MARGIN("got found %p for next position", found);
	if (found) {
		DBG_MARGIN(" with line %d and charoffset %d and %d blocks", found->line, found->charoffset,
				g_queue_get_length(found->blockstack));
	}
	DBG_MARGIN("\n");
}*/

static inline void paint_margin_expand(BluefishTextView * btv, cairo_t * cr, gint w, gint height)
{
#if GTK_CHECK_VERSION(3,0,0)
	GtkStyleContext *cntxt;
	GdkRGBA rgba;

	cntxt = gtk_widget_get_style_context(GTK_WIDGET(btv));

	gtk_style_context_get_background_color(cntxt, gtk_widget_get_state_flags(GTK_WIDGET(btv)), &rgba);
	gdk_cairo_set_source_rgba(cr, &rgba);
#else
	gdk_cairo_set_source_color(cr,
							   &gtk_widget_get_style(GTK_WIDGET(btv))->base[gtk_widget_get_state
																			(GTK_WIDGET(btv))]);
#endif

	cairo_rectangle(cr, btv->margin_pixels_chars + btv->margin_pixels_symbol + 2, w + (height / 2) - 3, 7, 7);
	cairo_fill(cr);

#if GTK_CHECK_VERSION(3,0,0)
	gtk_style_context_get_color(cntxt, gtk_widget_get_state_flags(GTK_WIDGET(btv)), &rgba);
	gdk_cairo_set_source_rgba(cr, &rgba);
#else
	gdk_cairo_set_source_color(cr,
							   &gtk_widget_get_style(GTK_WIDGET(btv))->fg[gtk_widget_get_state
																		  (GTK_WIDGET(btv))]);
#endif

	cairo_rectangle(cr, btv->margin_pixels_chars + btv->margin_pixels_symbol + 1.5, w + (height / 2) - 3.5, 8,
					8);
	cairo_move_to(cr, btv->margin_pixels_chars + btv->margin_pixels_symbol + 5.5, w + (height / 2) + 5);
	cairo_line_to(cr, btv->margin_pixels_chars + btv->margin_pixels_symbol + 5.5, w + height + 0.5);
	cairo_move_to(cr, btv->margin_pixels_chars + btv->margin_pixels_symbol + 3, w + (height / 2) + 0.5);
	cairo_line_to(cr, btv->margin_pixels_chars + btv->margin_pixels_symbol + 8, w + (height / 2) + 0.5);
	cairo_stroke(cr);
}

static inline void paint_margin_collapse(BluefishTextView * btv, cairo_t * cr, gint w, gint height)
{
#if GTK_CHECK_VERSION(3,0,0)
	GtkStyleContext *cntxt;
	GdkRGBA rgba;

	cntxt = gtk_widget_get_style_context(GTK_WIDGET(btv));

	gtk_style_context_get_background_color(cntxt, gtk_widget_get_state_flags(GTK_WIDGET(btv)), &rgba);
	gdk_cairo_set_source_rgba(cr, &rgba);
#else
	gdk_cairo_set_source_color(cr,
							   &gtk_widget_get_style(GTK_WIDGET(btv))->base[gtk_widget_get_state
																			(GTK_WIDGET(btv))]);
#endif

	cairo_rectangle(cr, btv->margin_pixels_chars + btv->margin_pixels_symbol + 2, w + (height / 2) - 3, 7, 7);
	cairo_fill(cr);

#if GTK_CHECK_VERSION(3,0,0)
	gtk_style_context_get_color(cntxt, gtk_widget_get_state_flags(GTK_WIDGET(btv)), &rgba);
	gdk_cairo_set_source_rgba(cr, &rgba);
#else
	gdk_cairo_set_source_color(cr,
							   &gtk_widget_get_style(GTK_WIDGET(btv))->fg[gtk_widget_get_state
																		  (GTK_WIDGET(btv))]);
#endif

	cairo_rectangle(cr, btv->margin_pixels_chars + btv->margin_pixels_symbol + 1.5, w + (height / 2) - 3.5, 8,
					8);
	cairo_move_to(cr, btv->margin_pixels_chars + btv->margin_pixels_symbol + 5.5, w + (height / 2) - 2);
	cairo_line_to(cr, btv->margin_pixels_chars + btv->margin_pixels_symbol + 5.5, w + (height / 2) + 3);
	cairo_move_to(cr, btv->margin_pixels_chars + btv->margin_pixels_symbol + 5.5, w + (height / 2) + 5);
	cairo_line_to(cr, btv->margin_pixels_chars + btv->margin_pixels_symbol + 5.5, w + height + 0.5);
	cairo_move_to(cr, btv->margin_pixels_chars + btv->margin_pixels_symbol + 3, w + (height / 2) + 0.5);
	cairo_line_to(cr, btv->margin_pixels_chars + btv->margin_pixels_symbol + 8, w + (height / 2) + 0.5);
	cairo_stroke(cr);
}

static inline void paint_margin_blockend(BluefishTextView * btv, cairo_t * cr, gint w, gint height)
{
	/*gdk_draw_line(GDK_DRAWABLE(event->window),GTK_WIDGET(btv)->style->fg_gc[gtk_widget_get_state(GTK_WIDGET(btv))],btv->margin_pixels_chars+btv->margin_pixels_symbol+5, w, btv->margin_pixels_chars+btv->margin_pixels_symbol+5, w + (height/2));
	   gdk_draw_line(GDK_DRAWABLE(event->window),GTK_WIDGET(btv)->style->fg_gc[gtk_widget_get_state(GTK_WIDGET(btv))],btv->margin_pixels_chars+btv->margin_pixels_symbol+5, w+(height/2), btv->margin_pixels_chars+btv->margin_pixels_symbol+8, w + (height/2)); */
	cairo_move_to(cr, btv->margin_pixels_chars + btv->margin_pixels_symbol + 5.5, w);
	cairo_line_to(cr, btv->margin_pixels_chars + btv->margin_pixels_symbol + 5.5, w + (height / 2) + 0.5);
	cairo_move_to(cr, btv->margin_pixels_chars + btv->margin_pixels_symbol + 5.5, w + (height / 2) + 0.5);
	cairo_line_to(cr, btv->margin_pixels_chars + btv->margin_pixels_symbol + 8.5, w + (height / 2) + 0.5);
	cairo_stroke(cr);
}

static inline void paint_margin_line(BluefishTextView * btv, cairo_t * cr, gint w, gint height)
{
/*	gdk_draw_line(GDK_DRAWABLE(event->window),GTK_WIDGET(btv)->style->fg_gc[gtk_widget_get_state(GTK_WIDGET(btv))],btv->margin_pixels_chars+btv->margin_pixels_symbol+5, w, btv->margin_pixels_chars+btv->margin_pixels_symbol+5, w + height);*/
	cairo_move_to(cr, btv->margin_pixels_chars + btv->margin_pixels_symbol + 5.5, w);
	cairo_line_to(cr, btv->margin_pixels_chars + btv->margin_pixels_symbol + 5.5, w + height);
	cairo_stroke(cr);
}

static inline void paint_margin_symbol(BluefishTextView * btv, cairo_t * cr, gint w, gint height)
{
	cairo_rectangle(cr, btv->margin_pixels_chars + 2, w + (height / 2) - 4, 8, 8);
	cairo_fill(cr);
}

static gint get_num_foldable_blocks(Tfound * found)
{
	gint count = 0;
	Tfoundblock *tmpfblock = found->fblock;
	if (found->numblockchange < 0 && found->fblock->foldable)
		count = found->numblockchange;	/* don't count popped blocks */
	DBG_MARGIN("found->numblockchange=%d, initial count=%d\n", found->numblockchange, count);
	while (tmpfblock) {
		DBG_MARGIN("check block %p (%d:%d), foldable=%d, parent=%p\n", tmpfblock, tmpfblock->start1_o,
				   tmpfblock->end2_o, tmpfblock->foldable, tmpfblock->parentfblock);
		if (tmpfblock->foldable)
			count++;
		tmpfblock = (Tfoundblock *) tmpfblock->parentfblock;
	}
	return count;
}

static inline void
paint_margin(BluefishTextView * btv, cairo_t * cr, GtkTextIter * startvisible, GtkTextIter * endvisible)
{
	Tfound *found = NULL;
	BluefishTextView *master = btv->master;
	GSequenceIter *siter = NULL;
	guint num_blocks;
	gint cursor_line = -1;
	GtkTextIter it;
	GtkTextTag *folded;
	gint i;
	PangoLayout *panlay;
	gpointer bmark;
	gint bmarkline = -1;
	guint startvisible_offset;

#if GTK_CHECK_VERSION(3,0,0)
	GtkStyleContext *cntxt;
	GdkRGBA rgba;
#endif

	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(btv));
	DBG_MSG("paint_margin called for %p\n", btv);

	cairo_set_line_width(cr, 1);
	if (main_v->props.use_system_colors) {
#if GTK_CHECK_VERSION(3,0,0)
		cntxt = gtk_widget_get_style_context(GTK_WIDGET(btv));
		gtk_style_context_get_color(cntxt, gtk_widget_get_state_flags(GTK_WIDGET(btv)), &rgba);
		gdk_cairo_set_source_rgba(cr, &rgba);
#else
		gdk_cairo_set_source_color(cr,
								   &gtk_widget_get_style(GTK_WIDGET(btv))->fg[gtk_widget_get_state
																			  (GTK_WIDGET(btv))]);
#endif
	} else {
#if GTK_CHECK_VERSION(3,0,0)
		gdk_cairo_set_source_rgba(cr, &st_margin_fg_color);
#else
		gdk_cairo_set_source_color(cr, &st_margin_fg_color);
#endif
	}
	if (master->show_line_numbers) {
		GtkTextIter cursorit;
		gtk_text_buffer_get_iter_at_mark(buffer, &cursorit, gtk_text_buffer_get_insert(buffer));
		cursor_line = gtk_text_iter_get_line(&cursorit);
	}
	startvisible_offset = gtk_text_iter_get_offset(startvisible);
	/* to see how many blocks are active here */
	if (G_UNLIKELY(gtk_text_iter_is_start(startvisible)
				   && (g_sequence_get_length(master->scancache.foundcaches) != 0))) {
		siter = g_sequence_get_begin_iter(master->scancache.foundcaches);
		if (!g_sequence_iter_is_end(siter)) {
			found = g_sequence_get(siter);
		}
		num_blocks = 0;
		DBG_MARGIN("EXPOSE: start at begin, set num_blocks %d, found=%p\n", num_blocks, found);
	} else {
		found = get_foundcache_at_offset(master, startvisible_offset, &siter);
		if (found) {
			num_blocks = get_num_foldable_blocks(found);
			DBG_MARGIN("EXPOSE: got %d foldable blocks at found %p at offset %d\n", num_blocks, found,
					   found->charoffset_o);
		} else {
			DBG_MARGIN("EXPOSE: no found for position %d, siter=%p\n",
					   startvisible_offset, siter);
			num_blocks = 0;
		}
	}
	/* in the case that the *first* found is relevant, we don't need
	   the 'next' found */
	if (!found || found->charoffset_o < startvisible_offset) {
		DBG_MARGIN("get next found..\n");
		if (siter)
			found = get_foundcache_next(master, &siter);
	}
	/*DBG_MARGIN("first found ");
	   print_found(found); */

	it = *startvisible;
	panlay = gtk_widget_create_pango_layout(GTK_WIDGET(btv), "x");

	folded = gtk_text_tag_table_lookup(langmgr_get_tagtable(), "_folded_");
	if (master->showsymbols) {
		bmarkline = bmark_margin_get_first_bookmark((Tdocument *) master->doc, &bmark);
	}

	for (i = gtk_text_iter_get_line(startvisible); i <= gtk_text_iter_get_line(endvisible); i++) {
		gint w, height;
		gchar *string;

		gtk_text_iter_set_line(&it, i);

		if (G_UNLIKELY(gtk_text_iter_has_tag(&it, folded))) {
			DBG_FOLD("line %d is hidden\n", i);
			num_blocks = -1;	/* -1 means invalid */
		} else {
			gtk_text_view_get_line_yrange(GTK_TEXT_VIEW(btv), &it, &w, &height);
			gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(btv), GTK_TEXT_WINDOW_LEFT, 0, w, NULL, &w);

			/* line numbers */
			if (master->show_line_numbers) {
				if (i == cursor_line)
					string = g_strdup_printf("<b>%d</b>", 1 + i);
				else
					string = g_strdup_printf("%d", 1 + i);
				pango_layout_set_markup(panlay, string, -1);
				cairo_move_to(cr, 2, w);
				pango_cairo_show_layout(cr, panlay);
				g_free(string);
			}
			/* symbols */
			if (master->showsymbols && bmarkline != -1) {
				while (bmarkline != -1 && bmarkline < i) {
					bmarkline = bmark_margin_get_next_bookmark((Tdocument *) master->doc, &bmark);
				}
				if (G_UNLIKELY(bmarkline == i)) {
					paint_margin_symbol(master, cr, w, height);
				}
			}
			/* block folding.
			   - to find out if we need an expander/collapse, we need to see if there is a pushedblock on the
			   blockstack which has 'foldable' for any foundcache that is on this line.
			   - to find if we need an end-of-block we need to see if there is a poppedblock on this line
			   which has 'foldable'
			   - to find out if we need a line or nothing we need to know the number of expanded blocks on the stack
			 */
			if (master->show_blocks) {
				GtkTextIter nextline;
				Tfound *oldfound;
				gint paint = (num_blocks > 0) ? 1 : 0;	/* 0=nothing, 1=line, 2=expand, 3=collapse, 4=blockend */
				guint nextline_o, curline_o;
				/*g_print("line %d, num_blocks=%d, default paint=%d\n",i,num_blocks,paint); */
				curline_o = gtk_text_iter_get_offset(&it);
				nextline = it;
				if (!gtk_text_iter_ends_line(&nextline)) {
					gtk_text_iter_forward_to_line_end(&nextline);
				}
				nextline_o = gtk_text_iter_get_offset(&nextline);
				while (found) {
					guint foundpos = found->charoffset_o;
					if (IS_FOUNDMODE_BLOCKPUSH(found)) {
						/* on a pushedblock we should look where the block match start, charoffset_o is the end of the
						   match, so multiline patterns are drawn on the wrong line */
						foundpos = found->fblock->start1_o;
					}
					/*g_print("search block for line %d, curline_o=%d, nextline_o=%d, foundpos=%d, num_blocks=%d\n",i,curline_o,nextline_o, foundpos, num_blocks); */
					if (foundpos > nextline_o) {
						break;
					}

					if (foundpos <= nextline_o && foundpos >= curline_o) {
						/*g_print("line %d, looking at found at position %d which has numblockchange=%d\n",i,found->charoffset_o,found->numblockchange); */
						if (IS_FOUNDMODE_BLOCKPUSH(found) && found->fblock->foldable) {
							paint = found->fblock->folded ? 3 : 2;
							num_blocks = get_num_foldable_blocks(found);
							/*g_print("paint_margin, pushed block, folded=%d, so paint=%d\n",found->fblock->folded,paint); */
							break;
						} else if (IS_FOUNDMODE_BLOCKPOP(found) && found->fblock->foldable) {
							guint new_num_blocks = get_num_foldable_blocks(found);
							if (new_num_blocks < num_blocks)
								paint = 4;
							/*else
							   paint=1; */
							/*g_print("paint_margin, line %d, new_num_blocks=%d, num_blocks=%d so paint=%d\n",i,new_num_blocks,num_blocks, paint); */
							num_blocks = new_num_blocks;
							/*break; */
						}
					}
					oldfound = found;
					found = get_foundcache_next(master, &siter);
					/* I'm not 100% sure about the !found ||  that I added to the next line.. */
					if (num_blocks == -1 && (!found || found->charoffset_o >= curline_o)) {
						num_blocks = get_num_foldable_blocks(oldfound);
						/*g_print("re-set num_blocks to %d using found at %d, next found at %d\n", num_blocks, oldfound->charoffset_o, found->charoffset_o); */
						paint = (num_blocks > 0) ? 1 : 0;
					}
				}
				switch (paint) {
				case 0:
					break;
				case 1:
					paint_margin_line(master, cr, w, height);
					break;
				case 2:
					paint_margin_expand(master, cr, w, height);
					break;
				case 3:
					paint_margin_collapse(master, cr, w, height);
					break;
				case 4:
					paint_margin_blockend(master, cr, w, height);
					break;
				}
			}
		}
	}
	g_object_unref(G_OBJECT(panlay));
}

typedef struct {
	gpointer parentindent;
	guint level;
	guint x; // the x position of the GtkTextIter where this indenting started 
	guint y;
} Tindent;

static void 
get_indent_paint_position_from_char_offset(BluefishTextView * btv, guint32 charoffset, gboolean beginpos, guint *x, guint *y) {
	GtkTextIter iter;
	GdkRectangle rect;
	gtk_text_buffer_get_iter_at_offset(btv->buffer,&iter,charoffset);
	gtk_text_view_get_iter_location(GTK_TEXT_VIEW(btv), &iter, &rect);
	*x = rect.x;
	if (beginpos)
		rect.height=0;
	*y = rect.y + rect.height;
}
#ifdef PAINTDEBUG
static void 
print_indent_stack(Tindent *ind){
	while(ind) {
		g_print("have ind %p with parent %p at level %d\n",ind,ind->parentindent,ind->level);
		ind = ind->parentindent;
	}
}
#endif
static Tindent *
new_indent(BluefishTextView *btv, guint level, guint charoffset_o, Tindent *parentindent) {
	Tindent *ind = g_slice_new0(Tindent);
	ind->level = level;
	ind->parentindent = parentindent;
	get_indent_paint_position_from_char_offset(btv, charoffset_o, TRUE, &ind->x, &ind->y);
	return ind;
} 

static Tindent * 
init_indent_stack(BluefishTextView * btv, GSequenceIter *siter) {
	GSequenceIter *tmpsiter = siter;
	Tfound *found;
	gboolean have_so_indent = FALSE; /* once set to true we have found the start-of the indenting*/
	Tindent *ind=NULL, *topind=NULL;
	guint curlevel;
	/* see if we have an indenting level here, and then move up to find lower indenting levels until 
	we find a 0 indent level --> at that point we have a complete indent stack */
	if (siter == NULL)
		return NULL;
	DBG_PAINTINDENT("init_indent_stack, tmpsiter=%p\n",tmpsiter);
	while (!have_so_indent && !g_sequence_iter_is_begin(tmpsiter)) {
		found = g_sequence_get(tmpsiter);
		if (!found) return ind;
		if (found->indentlevel == 0) {
			have_so_indent = TRUE;
			DBG_PAINTINDENT("init_indent_stack, found indent 0 at offset %d, returning\n",found->charoffset_o);
		} else if (found->indentlevel != NO_INDENT_FOUND && (!ind || found->indentlevel < curlevel)) {
			Tindent *newind;
			curlevel = found->indentlevel;
			newind = new_indent(btv, found->indentlevel, found->charoffset_o, NULL);
			/* should not be pushed on the top of stack, but pushed under the stack */
			if (ind) {
				ind->parentindent=newind;
			} else {
				topind=newind;
			}
			ind = newind;
			DBG_PAINTINDENT("init_indent_stack, added indent %p with level %d at offset %d\n", newind, found->indentlevel, found->charoffset_o);
		}
		tmpsiter = g_sequence_iter_prev(tmpsiter);
	}
	/*print_indent_stack(ind);*/
	return topind;
} 

static void
free_indent_stack(Tindent *indent) {
	/* cleanup the indentstack */
	while (indent) {
		Tindent *parent = (Tindent *)indent->parentindent;
		g_slice_free(Tindent, indent);
		indent = parent;
	}
}

static void 
paint_indent_line(BluefishTextView * btv, cairo_t * cr, gint xs, gint ys, gint xe, gint ye) {
	gint lxs, lys, lxe, lye;
	
	gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(btv), GTK_TEXT_WINDOW_TEXT, xs, ys, &lxs, &lys);
	gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(btv), GTK_TEXT_WINDOW_TEXT, xe, ye, &lxe, &lye);
#if GTK_CHECK_VERSION(3, 0, 0)
	guint marginoffset;
	marginoffset = (BLUEFISH_TEXT_VIEW(btv->master)->margin_pixels_chars +
				  BLUEFISH_TEXT_VIEW(btv->master)->margin_pixels_block +
				  BLUEFISH_TEXT_VIEW(btv->master)->margin_pixels_symbol);
	lxe += marginoffset;
	lxs += marginoffset;
#endif
	DBG_PAINTINDENT("paint_indent_line, paint from %d,%d to %d,%d\n",xs,ys,xe,ye);
	cairo_move_to(cr, lxs-3, lys+2);
	cairo_line_to(cr, lxe-3, lye-2);
}

static void
paint_indenting(BluefishTextView * btv, cairo_t * cr, GtkTextIter * startvisible, GtkTextIter * endvisible)
{
	static const double dash[] = {4.0};
	Tindent *ind;
	Tfound *found = NULL;
	BluefishTextView *master = btv->master;
	GSequenceIter *siter = NULL;
	guint startvisible_offset, endvisible_o;
	
	cairo_set_line_width(cr, 0.5);	/* 1.0 looks the best, smaller gives a half-transparent color */
	cairo_set_dash(cr, dash, 1, 0);
	gdk_cairo_set_source_color(cr,&gtk_widget_get_style(GTK_WIDGET(btv))->fg[gtk_widget_get_state
																				  (GTK_WIDGET(btv))]);
	/*cairo_rectangle(cr, event->area.x, event->area.y, event->area.width, event->area.height);
	cairo_clip(cr);*/
	startvisible_offset = gtk_text_iter_get_offset(startvisible);
	endvisible_o = gtk_text_iter_get_offset(endvisible);
	DBG_PAINTINDENT("paint_indenting, search in cache for offset %d\n",startvisible_offset);
	
	if (G_UNLIKELY(gtk_text_iter_is_start(startvisible)
				   && (g_sequence_get_length(master->scancache.foundcaches) != 0))) {
		siter = g_sequence_get_begin_iter(master->scancache.foundcaches);
		if (!g_sequence_iter_is_end(siter)) {
			found = g_sequence_get(siter);
		}
	} else {
		found = get_foundcache_at_offset(master, startvisible_offset, &siter);
	}
	ind = init_indent_stack(btv, siter);
	while (found) {
		DBG_PAINTINDENT("paint indenting, found at offset %d has indentlevel %d\n",found->charoffset_o, found->indentlevel == NO_INDENT_FOUND ? -1 : found->indentlevel);
		if (G_UNLIKELY(found->charoffset_o > endvisible_o && !ind))
			break;
		
		if (found->indentlevel == NO_INDENT_FOUND) {
			// skip this found
		} else if (found->indentlevel == 0) {
			if (ind) {
				DBG_PAINTINDENT("found indent level 0 at position %d, empty the stack\n",found->charoffset_o);
				free_indent_stack(ind);
				ind=NULL;
			}
		} else if (!ind || found->indentlevel > ind->level){
			// new indenting level, push to stack
			DBG_PAINTINDENT("paint_indenting, indent was at %d, found has new indenting level %d at offset %d, push to stack\n",ind?ind->level:-1,found->indentlevel, found->charoffset_o);
			ind = new_indent(btv, found->indentlevel, found->charoffset_o, ind);
		} else if (found->indentlevel == ind->level){
			guint x2,y2;
			// same level as before
			DBG_PAINTINDENT("paint_indenting, found level is still %d, paint line\n",found->indentlevel);
			get_indent_paint_position_from_char_offset(btv, found->charoffset_o, FALSE, &x2, &y2);
			paint_indent_line(btv, cr, ind->x,ind->y,x2,y2);
			//g_assert(x2 == ind->x);
		} else if (found->indentlevel < ind->level) {
			DBG_PAINTINDENT("paint_indenting, found has lower indenting level %d\n",found->indentlevel);
			// pop from stack until we have the level from this find and paint the line for that level
			do {
				Tindent *oldind = ind;
				ind = (Tindent *)ind->parentindent;
				DBG_PAINTINDENT("paint_indenting, popped indent level %d from stack, new level is %d\n",oldind->level, ind?ind->level:-1);
				g_slice_free(Tindent, oldind);
			} while (ind && found->indentlevel < ind->level);
			DBG_PAINTINDENT("paint_indenting, after pop, indent level on stack is %d\n",ind?ind->level:-1);
			if (ind && found->indentlevel == ind->level) {
				guint x2,y2;
				get_indent_paint_position_from_char_offset(btv, found->charoffset_o, FALSE, &x2, &y2);
				//g_assert(x2 == ind->x);
				paint_indent_line(btv, cr, ind->x,ind->y,x2,y2);
			}
		}
		found = get_foundcache_next(master, &siter);
	}
	cairo_stroke(cr);
}

/* whitespace macro. Possibly include: '/n', 8206-8207, maybe others */
#define BTV_ISWS(c) ( \
  ((c) == '\r') || \
  ((c) == '\n') || \
  ((c) == '\t') || \
  ((c) == ' ') || \
  ((c) == 160) || \
  ((c) == 8239) || \
  ((c) == 12288) || \
  ((c) >= 8192 && (c) <= 8205) \
)
/*
main_v->props.visible_ws_mode:
0 = All
1 = All except spaces
2 = All trailing
3 = All except non-trailing spaces
*/
static inline void
paint_spaces(BluefishTextView * btv, cairo_t * cr, GtkTextIter * startvisible, GtkTextIter * endvisible)
{
	GtkTextIter iter;
	gunichar uc;
	gboolean trailing = FALSE;

	cairo_set_line_width(cr, 1.0);
#if GTK_CHECK_VERSION(3,0,0)
	gdk_cairo_set_source_rgba(cr, &st_whitespace_color);
#else
	gdk_cairo_set_source_color(cr, &st_whitespace_color);
#endif
	iter = *endvisible;
	if (!gtk_text_iter_ends_line(&iter))
		gtk_text_iter_forward_to_line_end(&iter);

	while (!gtk_text_iter_equal(&iter, startvisible)) {	/* equal is faster than compare */
		GdkRectangle rect;
		gint x, y;
		uc = gtk_text_iter_get_char(&iter);
		if (G_UNLIKELY(BTV_ISWS(uc))) {
			gtk_text_view_get_iter_location(GTK_TEXT_VIEW(btv), &iter, &rect);
			gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(btv), GTK_TEXT_WINDOW_TEXT, rect.x,
												  rect.y + rect.height / 1.5, &x, &y);
#if GTK_CHECK_VERSION(3, 0, 0)
			x += (BLUEFISH_TEXT_VIEW(btv->master)->margin_pixels_chars +
				  BLUEFISH_TEXT_VIEW(btv->master)->margin_pixels_block +
				  BLUEFISH_TEXT_VIEW(btv->master)->margin_pixels_symbol);
#endif
			if ((uc == '\n' || uc == '\r') && main_v->props.visible_ws_mode != 2) {
				gint fourtheight = rect.height / 4;
				/* draw newline or carriage return */
				cairo_move_to(cr, x + 0.5, y - 0.5 - fourtheight);
				cairo_rel_line_to(cr, fourtheight, 0);
				cairo_rel_line_to(cr, 0, 2 * fourtheight);
			} else if (uc == '\t' && (trailing || main_v->props.visible_ws_mode != 2)) {
				/* draw tab */
				cairo_move_to(cr, x + 3.5, y - 2.5);
				cairo_rel_line_to(cr, 0, 3);
				cairo_rel_line_to(cr, rect.width - 6, 0);
				cairo_rel_line_to(cr, 0, -3);
			} else if ((uc == 160 || uc == 8239) && (trailing || main_v->props.visible_ws_mode != 2)) {
				/* draw nbsp (8239= narrow-nbsp) */
				cairo_move_to(cr, x + 1, y - 0.5);
				cairo_rel_line_to(cr, rect.width - 2, 0);
			} else if (main_v->props.visible_ws_mode == 0 || (main_v->props.visible_ws_mode != 1 && trailing)) {
				/* draw space */
				x += rect.width / 2;
				cairo_move_to(cr, x, y);
				cairo_arc(cr, x, y, 0.75, 0, 2 * M_PI);
			}
		} else if (G_UNLIKELY(uc != '\n' && uc != '\r')) {
			trailing = FALSE;
		}

		if (G_UNLIKELY(gtk_text_iter_starts_line(&iter))) {
			trailing = TRUE;
		}
		gtk_text_iter_backward_char(&iter);
	}
	cairo_stroke(cr);
}

#if GTK_CHECK_VERSION(3,14,0)
static void bluefish_text_view_draw_layer(GtkTextView * text_view, GtkTextViewLayer layer, cairo_t * cr)
{
	cairo_save(cr);

	if (layer == GTK_TEXT_VIEW_LAYER_BELOW) {
		/* in gtk 3.20 GTK_TEXT_VIEW_LAYER_BELOW_TEXT is added which works in buffer coordinates. 
		   In gtk 3.14 GTK_TEXT_VIEW_LAYER_BELOW is added which works in viewport coordinates */
		BluefishTextView *btv = BLUEFISH_TEXT_VIEW(text_view);
		BluefishTextView *master = BLUEFISH_TEXT_VIEW(btv->master);

		if (gtk_widget_is_sensitive(GTK_WIDGET(btv))
			&& (BFWIN(DOCUMENT(master->doc)->bfwin)->session->view_cline || main_v->props.highlight_cursor)) {
			gint y_wincoord, x_wincoord;
			GtkTextIter it;
			GdkRectangle itrect;


			DBG_SIGNALS
				("bluefish_text_view_draw_layer_event, current line highlighting, code for gtk >= 3.14\n");
			gtk_text_buffer_get_iter_at_mark(master->buffer, &it, gtk_text_buffer_get_insert(master->buffer));
			gtk_text_view_get_iter_location(text_view, &it, &itrect);
			gtk_text_view_buffer_to_window_coords(text_view, GTK_TEXT_WINDOW_TEXT, itrect.x, itrect.y,
												  &x_wincoord, &y_wincoord);
			if (BFWIN(DOCUMENT(master->doc)->bfwin)->session->view_cline && !DOCUMENT(master->doc)->readonly) {
				gdouble x1, y1, x2, y2;

				cairo_clip_extents(cr, &x1, &y1, &x2, &y2);
				gdk_cairo_set_source_rgba(cr, &st_cline_color);
				DBG_SIGNALS
					("bluefish_text_view_draw_event, GTK >= 3.14 draw current line at x=%d,y=%d,width=%d,height=%d\n",
					 x1 + .5, itrect.y + .5, x2 - x1 - 1, itrect.height - 1);
				cairo_rectangle(cr, x1 + .5, y_wincoord + .5, x2 - x1 - 1, itrect.height - 1);
				cairo_fill(cr);
			}
			if (main_v->props.highlight_cursor) {
				gint width;
				width = itrect.width > 5 ? itrect.width : master->margin_pixels_per_char;
				gdk_cairo_set_source_rgba(cr, &st_cursor_highlight_color);
				/* use y instead of itrect.y, because y is already converted to window coords */
				DBG_SIGNALS
					("bluefish_text_view_draw_event, GTK >= 3.14 draw highlight_cursor block, draw at x=%d\n",
					 itrect.x);
				cairo_rectangle(cr, x_wincoord, y_wincoord, width, itrect.height);
				cairo_fill(cr);
			}
		}
	}

	cairo_restore(cr);
}
#endif

#if GTK_CHECK_VERSION(3,0,0)
/* this is the GTK 3 version for this function */
static gboolean bluefish_text_view_draw(GtkWidget * widget, cairo_t * cr)
{
	BluefishTextView *btv = BLUEFISH_TEXT_VIEW(widget);
	BluefishTextView *master = BLUEFISH_TEXT_VIEW(btv->master);
	gboolean event_handled = FALSE;
	GdkWindow *wleft, *wtext;
	GdkRectangle rect;
	gint rect2y, rect2x;
	GtkTextIter startvisible, endvisible;

#if GTK_CHECK_VERSION(3,14,0)
	if (GTK_WIDGET_CLASS(bluefish_text_view_parent_class)->draw)
		event_handled = GTK_WIDGET_CLASS(bluefish_text_view_parent_class)->draw(widget, cr);
#endif

	wleft = gtk_text_view_get_window(GTK_TEXT_VIEW(widget), GTK_TEXT_WINDOW_LEFT);
	gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(widget), &rect);
	gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(widget), &startvisible, rect.y, NULL);
	gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(widget), &endvisible, rect.y + rect.height, NULL);
	gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(widget), GTK_TEXT_WINDOW_TEXT, rect.x,
										  rect.y, &rect2x, &rect2y);

	if (wleft && gtk_cairo_should_draw_window(cr, wleft)) {
	   /******** the painting in the MARGIN area of the widget *********/
		DBG_SIGNALS("bluefish_text_view_draw_event, GTK_TEXT_WINDOW_LEFT\n");
		paint_margin(btv, cr, &startvisible, &endvisible);
		event_handled = TRUE;
	}
	wtext = gtk_text_view_get_window(GTK_TEXT_VIEW(widget), GTK_TEXT_WINDOW_TEXT);
	if (wtext && gtk_cairo_should_draw_window(cr, wtext)) {
	   /******** the painting in the TEXT area of the widget ********/
#if !GTK_CHECK_VERSION(3,14,0)
		if (gtk_widget_is_sensitive(GTK_WIDGET(btv))
			&& (BFWIN(DOCUMENT(master->doc)->bfwin)->session->view_cline || main_v->props.highlight_cursor)) {
			gint y2, x2;
			GtkTextIter it;
			GdkRectangle itrect;
			DBG_SIGNALS("bluefish_text_view_draw_event, gtk < 3.14 current line highlighting\n");
			gtk_text_buffer_get_iter_at_mark(master->buffer, &it, gtk_text_buffer_get_insert(master->buffer));
			gtk_text_view_get_iter_location((GtkTextView *) widget, &it, &itrect);
			gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(widget), GTK_TEXT_WINDOW_TEXT,
												  itrect.x, itrect.y, &x2, &y2);
			if (BFWIN(DOCUMENT(master->doc)->bfwin)->session->view_cline && !DOCUMENT(master->doc)->readonly) {
				/*g_print("cline highlight, got itrect.y=%d, y2=%d, itrect.x=%d, x2=%d, itrect.height=%d, itrect.width=%d\n",itrect.y,y2,itrect.x, x2, itrect.height, itrect.width); */
				gdk_cairo_set_source_rgba(cr, &st_cline_color);
				cairo_rectangle(cr,
								(gfloat) (master->margin_pixels_chars + master->margin_pixels_block +
										  master->margin_pixels_symbol), (gfloat) y2,
								(gfloat) gtk_widget_get_allocated_width(widget), (gfloat) itrect.height);
				cairo_fill(cr);
			}
			if (main_v->props.highlight_cursor) {
				gint width = itrect.width > 5 ? itrect.width : master->margin_pixels_per_char;
				gdk_cairo_set_source_rgba(cr, &st_cursor_highlight_color);
				DBG_SIGNALS("bluefish_text_view_draw_event, GTK < 3.14 draw highlight_cursor block\n");
				cairo_rectangle(cr,
								(gfloat) x2 - width + master->margin_pixels_chars +
								master->margin_pixels_block + master->margin_pixels_symbol, (gfloat) y2,
								(gfloat) (width * 2), (gfloat) itrect.height);
				cairo_fill(cr);
			}
		}

		if (GTK_WIDGET_CLASS(bluefish_text_view_parent_class)->draw)
			event_handled = GTK_WIDGET_CLASS(bluefish_text_view_parent_class)->draw(widget, cr);
#endif
		if (master->visible_spacing) {
			DBG_SIGNALS("bluefish_text_view_draw_event, paint visible spacing\n");
			paint_spaces(btv, cr, &startvisible, &endvisible);
		}

		if (master->show_right_margin) {
			GtkStyleContext *cntxt;
			GdkRGBA rgba;
			guint pix =
				master->margin_pixels_per_char * main_v->props.right_margin_pos +
				master->margin_pixels_block + master->margin_pixels_symbol + master->margin_pixels_chars;

			cairo_set_line_width(cr, 1.0);	/* 1.0 looks the best, smaller gives a half-transparent color */
			cntxt = gtk_widget_get_style_context(GTK_WIDGET(btv));
			gtk_style_context_get_color(cntxt, gtk_widget_get_state_flags(GTK_WIDGET(btv)), &rgba);
			gdk_cairo_set_source_rgba(cr, &rgba);
			cairo_move_to(cr, pix, rect2y);
			cairo_line_to(cr, pix, rect2y + rect.height);
			cairo_stroke(cr);
		}
		if (master->show_indenting) {
			paint_indenting(btv, cr, &startvisible, &endvisible);
		}
	}

	return event_handled;
}
#else
/* this is the GTK 2 version for this function */
static gboolean bluefish_text_view_expose_event(GtkWidget * widget, GdkEventExpose * event)
{
	BluefishTextView *btv = BLUEFISH_TEXT_VIEW(widget);
	BluefishTextView *master = BLUEFISH_TEXT_VIEW(btv->master);
	gboolean event_handled = FALSE;
	GdkWindow *wleft, *wtext;
	GdkRectangle rect;
	gint rect2y, rect2x;
	GtkTextIter startvisible, endvisible;

	cairo_t *cr = gdk_cairo_create(event->window);

	wleft = gtk_text_view_get_window(GTK_TEXT_VIEW(widget), GTK_TEXT_WINDOW_LEFT);
	gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(widget), &rect);
	gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(widget), &startvisible, rect.y, NULL);
	gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(widget), &endvisible, rect.y + rect.height, NULL);
	gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(widget), GTK_TEXT_WINDOW_TEXT, rect.x,
										  rect.y, &rect2x, &rect2y);

	if (event->window == wleft) {
	   /******** the painting in the MARGIN area of the widget *********/
		DBG_SIGNALS("bluefish_text_view_expose_event, GTK_TEXT_WINDOW_LEFT\n");
		paint_margin(btv, cr, &startvisible, &endvisible);
		event_handled = TRUE;
	}
	wtext = gtk_text_view_get_window(GTK_TEXT_VIEW(widget), GTK_TEXT_WINDOW_TEXT);
	if (event->window == wtext) {
	   /******** the painting in the TEXT area of the widget ********/
		if (gtk_widget_is_sensitive(GTK_WIDGET(btv))
			&& (BFWIN(DOCUMENT(master->doc)->bfwin)->session->view_cline || main_v->props.highlight_cursor)) {
			gint y2, x2;
			GtkTextIter it;
			GdkRectangle itrect;
			DBG_SIGNALS("bluefish_text_view_expose_event, current line highlighting\n");
			gtk_text_buffer_get_iter_at_mark(master->buffer, &it, gtk_text_buffer_get_insert(master->buffer));
			gtk_text_view_get_iter_location((GtkTextView *) widget, &it, &itrect);
			gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW(widget), GTK_TEXT_WINDOW_TEXT,
												  itrect.x, itrect.y, &x2, &y2);
			if (BFWIN(DOCUMENT(master->doc)->bfwin)->session->view_cline) {
				/*g_print("cline highlight, got itrect.y=%d, y2=%d, itrect.x=%d, x2=%d, itrect.height=%d, itrect.width=%d\n",itrect.y,y2,itrect.x, x2, itrect.height, itrect.width); */
				gdk_cairo_set_source_color(cr, &st_cline_color);
				/*g_print("draw cline rectangle %f:%f %f-%f\n",rect2x + .5, y2 + .5, (gfloat)(rect.width - 1), (gfloat)(itrect.height - 1)); */
				cairo_rectangle(cr, (gfloat) rect2x, (gfloat) y2, (gfloat) (rect.width),
								(gfloat) (itrect.height));
				cairo_fill(cr);
			}
			if (main_v->props.highlight_cursor) {
				gint width = itrect.width > 5 ? itrect.width : master->margin_pixels_per_char;
				gdk_cairo_set_source_color(cr, &st_cursor_highlight_color);
				cairo_rectangle(cr, (gfloat) x2 - width, (gfloat) y2, (gfloat) (width * 2),
								(gfloat) itrect.height);
				cairo_fill(cr);
			}
		}

		if (GTK_WIDGET_CLASS(bluefish_text_view_parent_class)->expose_event)
			event_handled = GTK_WIDGET_CLASS(bluefish_text_view_parent_class)->expose_event(widget, event);

		if (master->visible_spacing) {
			DBG_SIGNALS("bluefish_text_view_expose_event, paint visible spacing\n");
			paint_spaces(btv, cr, &startvisible, &endvisible);
		}

		if (master->show_right_margin) {
			guint pix = master->margin_pixels_per_char * main_v->props.right_margin_pos;
			cairo_set_line_width(cr, 1.0);	/* 1.0 looks the best, smaller gives a half-transparent color */
			gdk_cairo_set_source_color(cr,
									   &gtk_widget_get_style(GTK_WIDGET(btv))->fg[gtk_widget_get_state
																				  (GTK_WIDGET(btv))]);

			cairo_rectangle(cr, event->area.x, event->area.y, event->area.width, event->area.height);
			cairo_clip(cr);
			cairo_move_to(cr, pix, rect2y);
			cairo_line_to(cr, pix, rect2y + rect.height);
			cairo_stroke(cr);
		}
		if (master->show_indenting) {
			paint_indenting(btv, cr, &startvisible, &endvisible);
		}
	}

	cairo_destroy(cr);

	return event_handled;
}
#endif							/* gtk3 */

static void
bftextview2_delete_range_lcb(GtkTextBuffer * buffer, GtkTextIter * obegin,
							 GtkTextIter * oend, gpointer user_data)
{
	BluefishTextView *btv = user_data;
	gint so, eo, mso, meo, loop, offset;
	GtkTextIter begin = *obegin, end = *oend;
	DBG_SIGNALS("bftextview2_delete_range_lcb\n");

	if (btv->master != btv) {
		return;
	}

	so = gtk_text_iter_get_offset(obegin);	/* re-use the loop variable */
	eo = gtk_text_iter_get_offset(oend);
	DBG_SIGNALS("bftextview2_delete_range_lcb, delete from %d to %d\n", so, eo);
	foundcache_update_offsets(BLUEFISH_TEXT_VIEW(btv->master), so, so - eo);


	/* mark the surroundings of the text that will be deleted */

	/* the 'word start' algorithm of pango becomes very slow in a situation where
	   the file is filled with funny unicode characters such as 'box' symbol characters.
	   This happens in search and replace with many replaces (this function is called for
	   each replace).
	   I have to see why this is. We could also mark from the beginning of the line, but that
	   would be excessive on very long lines...... what is best?? */
	loop = 0;
	while (loop < 32 && gtk_text_iter_backward_char(&begin)
		   && !g_unichar_isspace(gtk_text_iter_get_char(&begin)))
		loop++;
	loop = 0;
	while (loop < 32 && gtk_text_iter_forward_char(&end) && !g_unichar_isspace(gtk_text_iter_get_char(&end)))
		loop++;
	mso = gtk_text_iter_get_offset(&begin);
	meo = gtk_text_iter_get_offset(&end);
	/*gtk_text_iter_backward_word_start(&begin);
	   gtk_text_iter_forward_word_end(&end); */
#ifdef MARKREGION
	offset = so - eo;
	markregion_delete(&btv->scanning, mso, meo + offset, offset);
	DBG_MARKREGION("bftextview2_delete_range_lcb, apply needscanning (before offset is applied!) to %u:%u\n",
				   gtk_text_iter_get_offset(&begin), gtk_text_iter_get_offset(&end));
#ifdef HAVE_LIBENCHANT
	markregion_delete(&btv->spellcheck, mso, meo + offset, offset);
#endif
#endif
	btv->needremovetags = 0;
}

static void
bftextview2_delete_range_after_lcb(GtkTextBuffer * buffer, GtkTextIter * obegin,
								   GtkTextIter * oend, gpointer user_data)
{
	BluefishTextView *btv = user_data;
	/* in delete_range_after the text has been altered, so obegin and oend now both point to
	   the same location, where the text was deleted */

	DBG_SIGNALS("bftextview2_delete_range_after_lcb, btv=%p, master=%p, needs_autocomp=%d\n", btv,
				btv->master, btv->needs_autocomp);
	if (BLUEFISH_TEXT_VIEW(btv->master)->enable_scanner && btv->needs_autocomp
		&& BLUEFISH_TEXT_VIEW(btv->master)->auto_complete && (btv->autocomp
															  || main_v->props.autocomp_popup_mode != 0)) {
		DBG_AUTOCOMP("bftextview2_delete_range_after_lcb, before autocomp_run()\n");
		autocomp_run(btv, FALSE);
	}
	DBG_AUTOCOMP("bftextview2_delete_range_after_lcb, after run, set needs_autocomp to FALSE\n");
	btv->needs_autocomp = FALSE;

	bftextview2_reset_user_idle_timer(btv);
	if (btv->master != btv) {
		return;
	}
	bftextview2_schedule_scanning(btv);


	/* because compare_markregion_needscanning() compares needscanning and markregion code, the offset needs to be adjusted in both.
	   for needscanning the offset is only adjusted in the 'after' callback */
}

gboolean last_undo_is_spacingtoclick(BluefishTextView * btv)
{
	return (btv->spacingtoclickstart != -1 && btv->spacingtoclickend != -1
			&& doc_unre_test_last_entry(btv->doc, UndoInsert, btv->spacingtoclickstart,
										btv->spacingtoclickend));
}

void bluefish_text_view_remove_spacingtoclick(BluefishTextView * btv)
{
	BluefishTextView *master = BLUEFISH_TEXT_VIEW(btv->master);
	if (master->spacingtoclickstart != -1 && master->spacingtoclickend != -1) {
		if (doc_unre_test_last_entry
			(master->doc, UndoInsert, master->spacingtoclickstart, master->spacingtoclickend)) {
			/*g_print("bluefish_text_view_remove_spacingtoclick -> undo last spacing from %d to %d\n",master->spacingtoclickstart, master->spacingtoclickend); */
			/* last text action in the document was a spacingtoclick, so undo it */
			undo_doc(master->doc, TRUE);
		}
		master->spacingtoclickstart = -1;
		master->spacingtoclickend = -1;
	}
}

static void spacingtoclick_insert_spacing(BluefishTextView * btv, gint numchars, GtkTextIter * iter)
{
	BluefishTextView *master = BLUEFISH_TEXT_VIEW(btv->master);
	if (numchars > 0) {
		gchar *tmpstr;
		/*g_print("inserting numchars=%d spaces\n",numchars); */
		tmpstr = bf_str_repeat(" ", numchars);
		master->spacingtoclickstart = gtk_text_iter_get_offset(iter);
		master->spacingtoclickend = master->spacingtoclickstart + numchars;
		gtk_text_buffer_insert(master->buffer, iter, tmpstr, -1);
		g_free(tmpstr);
	}
}


static gboolean spacingtoclick_handle_keypress(BluefishTextView * btv, GdkEventKey * kevent)
{
	BluefishTextView *master = BLUEFISH_TEXT_VIEW(btv->master);
	GtkTextMark *imark;
	GtkTextIter iter;
	imark = gtk_text_buffer_get_insert(master->buffer);
	gtk_text_buffer_get_iter_at_mark(master->buffer, &iter, imark);

	if (kevent->keyval == GDK_Right) {
		g_print("spacingtoclick_handle_keypress, GDK_Right\n");
		if (gtk_text_iter_ends_line(&iter)) {
			gint curoffset = gtk_text_iter_get_offset(&iter);
			gint requested = curoffset + 1;
			gint oldstart = master->spacingtoclickstart;
			gint oldend = master->spacingtoclickend;
			g_print("oldstart=%d, oldend=%d, curoffset=%d\n", oldstart, oldend, curoffset);
			bluefish_text_view_remove_spacingtoclick(master);
			if (oldend == curoffset && oldstart > 0) {
				gtk_text_buffer_get_iter_at_offset(master->buffer, &iter, oldstart);
				/* increase current spacing with 1 character */
				spacingtoclick_insert_spacing(master, requested - oldstart, &iter);
				g_print("increased existing spacing at %d to %d\n", oldstart, requested - oldstart);
			} else {
				gtk_text_buffer_get_iter_at_offset(master->buffer, &iter, curoffset);
				/* new spacing */
				spacingtoclick_insert_spacing(master, requested - curoffset, &iter);
				g_print("added new spacing at %d to %d\n", curoffset, requested - curoffset);
			}
			return TRUE;
		}
	} else if (kevent->keyval == GDK_Left) {
		/* reduce spacing one character */
		g_print("spacingtoclick_handle_keypress, GDK_Left\n");
		if (gtk_text_iter_ends_line(&iter)) {
			gint curoffset = gtk_text_iter_get_offset(&iter);
			gint requested = curoffset - 1;
			gint oldstart = master->spacingtoclickstart;
			gint oldend = master->spacingtoclickend;
			g_print("oldstart=%d, oldend=%d, curoffset=%d\n", oldstart, oldend, curoffset);
			bluefish_text_view_remove_spacingtoclick(master);
			if (oldend == curoffset && oldstart > 0 && oldstart < requested) {
				gtk_text_buffer_get_iter_at_offset(master->buffer, &iter, oldstart);
				/* decrease current spacing with 1 character */
				spacingtoclick_insert_spacing(master, requested - oldstart, &iter);
				g_print("re-inserted spacing at %d to %d\n", oldstart, requested - oldstart);
				return TRUE;
			}
			if (requested == oldstart) {
				/*removing will put the cursor in the requested spot */
				return TRUE;
			}
			g_print
				("GDK_Left, only removed existing spacing, no new spacing, oldend=%d, oldstart=%d, curoffset=%d, requested=%d\n",
				 oldend, oldstart, curoffset, requested);
		}
	} else if (kevent->keyval == GDK_Up || kevent->keyval == GDK_Down) {
		GdkRectangle loc, loc2;
		gboolean ret;
		g_print
			("spacingtoclick_handle_keypress, GDK_Up or GDK_Down, iter is at cursor, gtk_text_iter_get_line()=%d, offset=%d\n",
			 gtk_text_iter_get_line(&iter), gtk_text_iter_get_offset(&iter));
		/* see if line above has same amount of characters */
		gtk_text_view_get_iter_location(GTK_TEXT_VIEW(btv), &iter, &loc);
		g_print("iter (at cursor) location, loc.x=%d, loc.y=%d\n", loc.x, loc.y);
		if (kevent->keyval == GDK_Up) {
			ret = gtk_text_iter_backward_line(&iter);
		} else {
			ret = gtk_text_iter_forward_line(&iter);
		}
		if (ret) {
			gint offset;
			g_print("after line forward/backward, line is %d\n", gtk_text_iter_get_line(&iter));
			if (!gtk_text_iter_ends_line(&iter)) {
				g_print("iter at line %d and offset %d does not end line, forward to line end\n",
						gtk_text_iter_get_line(&iter), gtk_text_iter_get_offset(&iter));
				gtk_text_iter_forward_to_line_end(&iter);
			}
			offset = gtk_text_iter_get_offset(&iter);
			gtk_text_view_get_iter_location(GTK_TEXT_VIEW(btv), &iter, &loc2);
			g_print("loc.x=%d, loc2.x=%d (line=%d, offset=%d)\n", loc.x, loc2.x,
					gtk_text_iter_get_line(&iter), gtk_text_iter_get_offset(&iter));
			if (master->spacingtoclickstart != -1 && master->spacingtoclickstart < offset
				&& master->spacingtoclickend <= offset) {
				/* removing the spacing will change the offset */
				g_print
					("there is already spacing before our current offset (start=%d, end=%d) which will be removed, so reduce offset by %d\n",
					 master->spacingtoclickstart, master->spacingtoclickend,
					 master->spacingtoclickend - master->spacingtoclickstart);
				offset -= (master->spacingtoclickend - master->spacingtoclickstart);
			}
			bluefish_text_view_remove_spacingtoclick(master);
			if (loc.x > loc2.x) {
				gint numchars;
				numchars = (loc.x - loc2.x) / master->margin_pixels_per_char;
				gtk_text_buffer_get_iter_at_offset(master->buffer, &iter, offset);
				g_print("insert %d spacing at offset %d (line %d)\n", numchars, offset,
						gtk_text_iter_get_line(&iter));
				spacingtoclick_insert_spacing(master, numchars, &iter);
			}
			g_print("place cursor at x=%d,y=%d\n", loc.x, loc2.y);
			gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(btv), &iter, loc.x, loc2.y);
			gtk_text_buffer_place_cursor(master->buffer, &iter);
			gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(btv), imark);
			return TRUE;
		} else {
			g_print("failed to forward/backward line\n");
		}
		bluefish_text_view_remove_spacingtoclick(master);
	} else {
		bluefish_text_view_remove_spacingtoclick(master);
	}
	return FALSE;
}

static gboolean bluefish_text_view_key_press_event(GtkWidget * widget, GdkEventKey * kevent)
{
	gboolean retval;
	BluefishTextView *btv = BLUEFISH_TEXT_VIEW(widget);
	BluefishTextView *master = btv->master;
	DBG_SIGNALS("bluefish_text_view_key_press_event, keyval=%d\n", kevent->keyval);

	if (master->margin_pixels_per_char <= 0) {
		calc_pixels_per_char(master);
	}
	/* following code handles key press events on the autocompletion popup */
	if (btv->autocomp) {
		if (acwin_check_keypress(btv, kevent)) {
			btv->key_press_inserted_char = FALSE;
			return TRUE;
		}
	}
	/* following code handles the manual autocompletion popup accelerator key, default <ctrl><space> */
	if (master->enable_scanner && (kevent->state & main_v->autocomp_accel_mods)
		&& kevent->keyval == main_v->autocomp_accel_key) {
		DBG_AUTOCOMP("bluefish_text_view_key_press_event, autocomp key combination\n");
		autocomp_run(btv, TRUE);
		return TRUE;
	}
	/* avoid the autocompletion popup for certain keys such as the delete key */
	if (kevent->keyval != GDK_Delete
		&& kevent->keyval != GDK_Up
		&& kevent->keyval != GDK_Down
		&& kevent->keyval != GDK_Left
		&& kevent->keyval != GDK_Right
		&& kevent->keyval != GDK_Page_Up
		&& kevent->keyval != GDK_Page_Down
		&& kevent->keyval != GDK_Home
		&& kevent->keyval != GDK_End
		&& kevent->keyval != GDK_Alt_L
		&& kevent->keyval != GDK_Alt_R
		&& kevent->keyval != GDK_Control_L
		&& kevent->keyval != GDK_Control_R
		&& !(kevent->state & GDK_CONTROL_MASK) && !(kevent->state & GDK_MOD1_MASK)) {
		DBG_AUTOCOMP("bluefish_text_view_key_press_event, keyval=%d, state=%d, set needs_autocomp to TRUE\n",
					 kevent->keyval, kevent->state);
		btv->needs_autocomp = TRUE;
	} else {
		if (main_v->props.editor_spacingtoclick && !(kevent->state & GDK_CONTROL_MASK)
			&& !(kevent->state & GDK_MOD1_MASK) && !(kevent->state & GDK_SHIFT_MASK)) {
			g_print
				("bluefish_text_view_key_press_event, handling spacingtoclick keypress, kevent->state=%d\n",
				 kevent->state);
			if (spacingtoclick_handle_keypress(btv, kevent)) {
				return TRUE;
			}
		}

	}
	/* following code does smart cursor placement */
	if (main_v->props.editor_smart_cursor && !(kevent->state & GDK_CONTROL_MASK)
		&& ((kevent->keyval == GDK_Home) || (kevent->keyval == GDK_KP_Home)
			|| (kevent->keyval == GDK_End)
			|| (kevent->keyval == GDK_KP_End))) {
		GtkTextMark *imark;
		gboolean ret;
		GtkTextIter iter, currentpos, linestart;

		imark = gtk_text_buffer_get_insert(master->buffer);
		gtk_text_buffer_get_iter_at_mark(master->buffer, &currentpos, imark);
		iter = currentpos;
		/* if you hold ALT and you   have wrapped text, bluefish will jump to the previous/next newline, else
		   it will jump the the start/end of the wrapped part of the line */
		if ((kevent->keyval == GDK_Home) || (kevent->keyval == GDK_KP_Home)) {
			ret = bf_text_iter_line_start_of_text(btv, &iter, &linestart, !(kevent->state & GDK_MOD1_MASK));
		} else {				/* (kevent->keyval == GDK_End) || (kevent->keyval == GDK_KP_End) */
			ret = bf_text_iter_line_end_of_text(btv, &iter, &linestart, !(kevent->state & GDK_MOD1_MASK));
		}
		if (ret) {
			if (gtk_text_iter_compare(&currentpos, &iter) == 0)
				iter = linestart;
			if (kevent->state & GDK_SHIFT_MASK)
				gtk_text_buffer_move_mark(master->buffer, imark, &iter);
			else
				gtk_text_buffer_place_cursor(master->buffer, &iter);
			gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(btv),
											   gtk_text_buffer_get_insert(master->buffer));
			return TRUE;
		}
	}
	/* following code indents on tab */
	if (main_v->props.editor_tab_indent_sel
		&& (kevent->keyval == GDK_Tab || kevent->keyval == GDK_KP_Tab || kevent->keyval == GDK_ISO_Left_Tab)
		&& (!(kevent->state & GDK_CONTROL_MASK))) {	/* shift-tab is also known as GDK_ISO_Left_Tab */
		GtkTextIter so, eo;
		gboolean have_selection;
		have_selection = gtk_text_buffer_get_selection_bounds(master->buffer, &so, &eo);
		if (have_selection) {
			GtkTextIter eol1, eol2, sol1, sol2;
			if (kevent->state & GDK_SHIFT_MASK) {
				/* unindent block */
				doc_indent_selection(master->doc, TRUE);
				return TRUE;
			}
			if (gtk_text_iter_get_line(&so) != gtk_text_iter_get_line(&eo)) {
				doc_indent_selection(master->doc, FALSE);
				return TRUE;
			}
			/* if the start and end are *around* the text (so either at the start or end or
			   in the indenting) we indent */
			eol1 = eo;
			sol2 = so;
			if (bf_text_iter_line_end_of_text(btv, &eol1, &eol2, FALSE)
				&& bf_text_iter_line_start_of_text(btv, &sol2, &sol1, FALSE)) {
				if ((gtk_text_iter_equal(&so, &sol1) || gtk_text_iter_equal(&so, &sol2)
					 || gtk_text_iter_in_range(&so, &sol1, &sol2))
					&& (gtk_text_iter_equal(&eo, &eol1) || gtk_text_iter_equal(&eo, &eol2)
						|| gtk_text_iter_in_range(&eo, &eol1, &eol2))) {
					doc_indent_selection(master->doc, FALSE);
					return TRUE;
				}
			}
		}
	}
	/* following code replaces tab with spaces */
	if ((kevent->keyval == GDK_Tab && !(kevent->state & GDK_SHIFT_MASK)
		 && !(kevent->state & GDK_CONTROL_MASK))
		&& BFWIN(DOCUMENT(master->doc)->bfwin)->session->editor_indent_wspaces) {
		GtkTextMark *imark;
		GtkTextIter iter;
		gchar *string;
		gint numchars;
		/* replace the tab with spaces if the user wants that.
		   However, some users want the tab key to arrive at the next tab stop. so if the tab width is
		   4 and there are two characters already, bluefish should insert only 2 characters */
		string = bf_str_repeat(" ", BFWIN(DOCUMENT(master->doc)->bfwin)->session->editor_tab_width);
		imark = gtk_text_buffer_get_insert(master->buffer);
		gtk_text_buffer_get_iter_at_mark(master->buffer, &iter, imark);
		numchars =
			BFWIN(DOCUMENT(master->doc)->bfwin)->session->editor_tab_width -
			(gtk_text_iter_get_line_offset(&iter) %
			 BFWIN(DOCUMENT(master->doc)->bfwin)->session->editor_tab_width);
		gtk_text_buffer_insert(master->buffer, &iter, string, numchars);
		g_free(string);
		return TRUE;
	}
	/* following code closes brackets */
	if (main_v->props.editor_auto_close_brackets &&
		(kevent->keyval == '[' || kevent->keyval == '{' || kevent->keyval == '(')
		&& !(kevent->state & GDK_CONTROL_MASK)) {
		gboolean noclose = FALSE;
		GtkTextMark *imark = gtk_text_buffer_get_insert(master->buffer);
		if (main_v->props.editor_auto_close_brackets == 2 /* smart */ ) {
			GtkTextIter iter;
			gunichar uc;
			/* check the character that follows the cursor */
			gtk_text_buffer_get_iter_at_mark(master->buffer, &iter, imark);
			uc = gtk_text_iter_get_char(&iter);
			/*g_print("smart bracket closing, uc='%c'\n",(gchar)uc); */
			if (uc != ' ' && uc != '\0' && uc != '\n' && uc != '\t') {
				noclose = TRUE;
			}
		}
		if (!noclose) {
			const gchar *insert;
			GtkTextIter tmpit;
			if (kevent->keyval == '{')
				insert = "{}";
			else if (kevent->keyval == '[')
				insert = "[]";
			else
				insert = "()";
			gtk_text_buffer_insert_at_cursor(master->buffer, insert, 2);
			gtk_text_buffer_get_iter_at_mark(master->buffer, &tmpit, imark);
			if (gtk_text_iter_backward_char(&tmpit)) {
				gtk_text_buffer_place_cursor(master->buffer, &tmpit);
			}
			return TRUE;
		}
	}
	/* following code moves a selected block */
	if (kevent->state & GDK_CONTROL_MASK) {
		if (kevent->keyval == GDK_Up) {
			doc_move_selection(master->doc, TRUE, TRUE);
			return TRUE;
		}
		if (kevent->keyval == GDK_Down) {
			doc_move_selection(master->doc, FALSE, TRUE);
			return TRUE;
		}
	}
	/* following code marks the location if the 'menu' key is used as if it was a right click button event */
	if (kevent->keyval == GDK_KEY_Menu) {
		GtkTextMark *imark = gtk_text_buffer_get_insert(master->buffer);
		GtkTextIter tmpit;
		gtk_text_buffer_get_iter_at_mark(master->buffer, &tmpit, imark);
		main_v->bevent_doc = master->doc;
		main_v->bevent_charoffset = gtk_text_iter_get_offset(&tmpit);
	}

	retval = GTK_WIDGET_CLASS(bluefish_text_view_parent_class)->key_press_event(widget, kevent);
	if (retval) {
		DBG_SIGNALS("parent handled the event, set key_press_inserted_char to TRUE\n");
		btv->key_press_inserted_char = TRUE;
	}
	return retval;
}

typedef enum {
	foldtags_fold,
	foldtags_expand,
	foldtags_expand_hidden
} Tfoldtags;

static void block_fold_tags(BluefishTextView * btv, Tfoundblock * fblock, Tfoldtags mode)
{
	GtkTextBuffer *buffer = btv->buffer;
	GtkTextIter it1, it2, it3, it4;

	bftextview2_get_iters_at_foundblock(buffer, fblock, &it1, &it2, &it3, &it4);
	if (main_v->props.block_folding_mode == 1 && !gtk_text_iter_ends_line(&it2)
		&& !gtk_text_iter_starts_line(&it2)) {
		gtk_text_iter_forward_to_line_end(&it2);
	}
	if (gtk_text_iter_ends_line(&it4)) {
		gtk_text_iter_forward_line(&it4);
	}

	if (mode != foldtags_fold) {
		DBG_FOLD("block_fold_apply_tags, REMOVE folded tags from %d:%d\n", fblock->start1_o, fblock->end2_o);
		gtk_text_buffer_remove_tag_by_name(buffer, "foldheader", &it1, &it2);
		if (main_v->props.block_folding_mode == 0) {
			if (mode == foldtags_expand)
				gtk_text_buffer_remove_tag_by_name(buffer, "_folded_", &it2, &it3);
			gtk_text_buffer_remove_tag_by_name(buffer, "foldheader", &it3, &it4);
		} else if (main_v->props.block_folding_mode == 1 && mode == foldtags_expand) {
			gtk_text_buffer_remove_tag_by_name(buffer, "_folded_", &it2, &it4);
		}
	} else {
		DBG_FOLD("block_fold_apply_tags, APPLY folded tags from %d:%d\n", fblock->start1_o, fblock->end2_o);
		gtk_text_buffer_apply_tag_by_name(buffer, "foldheader", &it1, &it2);
		if (main_v->props.block_folding_mode == 0) {
			gtk_text_buffer_apply_tag_by_name(buffer, "_folded_", &it2, &it3);
			gtk_text_buffer_apply_tag_by_name(buffer, "foldheader", &it3, &it4);
		} else if (main_v->props.block_folding_mode == 1) {
			gtk_text_buffer_apply_tag_by_name(buffer, "_folded_", &it2, &it4);
		}
		/*g_print("done applying tags to fold block\n"); */
	}
}

static void
reapply_folded_tag_to_folded_blocks(BluefishTextView * btv, Tfoundblock * fblock, GSequenceIter ** siter)
{
	Tfound *found;
	GSequenceIter *tsiter = *siter;
	found = get_foundcache_next(btv, &tsiter);
	DBG_FOLD("reapply_folded_tag from %d:%d, starting with found at %d\n", fblock->start1_o, fblock->start2_o,
			 found ? found->charoffset_o : 0);
	while (found && found->charoffset_o < fblock->start2_o) {
		if (IS_FOUNDMODE_BLOCKPUSH(found) && found->fblock->folded) {
			block_fold_tags(btv, found->fblock, foldtags_fold);
		}
		found = get_foundcache_next(btv, &tsiter);
	}
}

static gboolean parent_block_is_folded(Tfoundblock * fblock)
{
	Tfoundblock *tmpfblock = (Tfoundblock *) fblock->parentfblock;
	while (tmpfblock) {
		if (tmpfblock->folded) {
			DBG_FOLD("parent_block_is_folded, return TRUE\n");
			return TRUE;
		}
		tmpfblock = (Tfoundblock *) tmpfblock->parentfblock;
	}
	return FALSE;
}

static void
bftextview2_block_toggle_fold(BluefishTextView * btv, Tfoundblock * fblock, GSequenceIter ** siter)
{
	Tfoldtags mode;
	fblock->folded = (!fblock->folded);
	if (fblock->folded) {
		mode = foldtags_fold;
	} else {
		mode = parent_block_is_folded(fblock) ? foldtags_expand_hidden : foldtags_expand;
	}
	DBG_FOLD("bftextview2_block_toggle_fold, block %d:%d has now folded=%d\n", fblock->start1_o,
			 fblock->end2_o, fblock->folded);
	block_fold_tags(btv, fblock, mode);
	if (mode != foldtags_fold) {
		reapply_folded_tag_to_folded_blocks(btv, fblock, siter);
	}
}

static void bftextview2_toggle_fold(BluefishTextView * btv, GtkTextIter * iter)
{
	Tfound *found;
	GSequenceIter *siter;
	GtkTextIter tmpiter;
	guint offset, nextline_o;

	if (!btv->bflang)
		return;
	tmpiter = *iter;
	gtk_text_iter_set_line_offset(&tmpiter, 0);
	offset = gtk_text_iter_get_offset(&tmpiter);
	if (!gtk_text_iter_ends_line(&tmpiter)) {
		gtk_text_iter_forward_to_line_end(&tmpiter);
	}
	nextline_o = gtk_text_iter_get_offset(&tmpiter);
	/* returns the found PRIOR to iter, or the found excactly at iter,
	   but this fails if the iter is the start of the buffer */
	found = get_foundcache_at_offset(btv, offset, &siter);
	if (!found) {
		/* is this 'if' block still required? I think get_foundcache_at_offset() now returns the first iter already */
		DBG_FOLD("no found, retrieve first iter\n");
		found = get_foundcache_first(btv, &siter);
	}
	while (found && found->charoffset_o < nextline_o) {
		if (IS_FOUNDMODE_BLOCKPUSH(found) && found->fblock->foldable && found->fblock->start1_o >= offset)
			break;
		found = get_foundcache_next(btv, &siter);	/* should be the first found AFTER iter */
	}
	/*while (found && (found->charoffset_o < offset || !found->pushedblock || !found->pushedblock->foldable)) {
	   found = get_foundcache_next(btv, &siter); / * should be the first found AFTER iter * /
	   if (found && found->pushedblock && found->pushedblock->foldable)
	   break;
	   } */
	if (found && IS_FOUNDMODE_BLOCKPUSH(found) && found->fblock->start1_o >= offset
		&& found->fblock->start1_o <= nextline_o && found->fblock->foldable) {
		DBG_FOLD("toggle fold on found=%p\n", found);
		bftextview2_block_toggle_fold(btv, found->fblock, &siter);
	}
}

/* if we keep the tooltips enabled we trigger a race condition or something like that in
the text hiding and query tooltip code. So we stop the tooltips, and enable them again
in a low priority callback */
static gboolean enable_tooltip_idle_lcb(gpointer data)
{
	g_object_set(G_OBJECT(data), "has-tooltip", TRUE, NULL);
	return FALSE;
}

/*
 * the 'name' pointer should be the identical pointer as the pointer found in st->blocks
 * it is not compared on it's contents but on the pointer address
 */
static void bftextview2_collapse_expand_toggle(BluefishTextView * btv, const gchar * name, gboolean collapse)
{
	GSequenceIter *siter = NULL;
	Tfound *found;
	g_object_set(btv, "has-tooltip", FALSE, NULL);
	found = get_foundcache_first(btv, &siter);
	while (found) {
		if (IS_FOUNDMODE_BLOCKPUSH(found) && found->fblock->foldable && found->fblock->folded != collapse) {
			if (name) {
				if (name ==
					g_array_index(btv->bflang->st->blocks, Tpattern_block,
								  g_array_index(btv->bflang->st->matches, Tpattern,
												found->fblock->patternum).block).name)
					bftextview2_block_toggle_fold(btv, found->fblock, &siter);
			} else {
				bftextview2_block_toggle_fold(btv, found->fblock, &siter);
			}
		}
		found = get_foundcache_next(btv, &siter);
	}
	g_idle_add_full(G_PRIORITY_LOW, enable_tooltip_idle_lcb, btv, NULL);
}

static void bftextview2_collapse_lcb(GtkMenuItem * mitem, BluefishTextView * btv)
{
	bftextview2_collapse_expand_toggle(btv, g_object_get_data(G_OBJECT(mitem), "block_name"), TRUE);
}

static void bftextview2_expand_lcb(GtkMenuItem * mitem, BluefishTextView * btv)
{
	bftextview2_collapse_expand_toggle(btv, g_object_get_data(G_OBJECT(mitem), "block_name"), FALSE);
}

static GtkWidget *bftextview2_fold_menu(BluefishTextView * btv)
{
	gint i;
	GtkWidget *mitem, *menu = gtk_menu_new();
	mitem = gtk_menu_item_new_with_label(_("Collapse all"));
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), mitem);
	g_signal_connect(G_OBJECT(mitem), "activate", G_CALLBACK(bftextview2_collapse_lcb), btv);
	mitem = gtk_menu_item_new_with_label(_("Expand all"));
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), mitem);
	g_signal_connect(G_OBJECT(mitem), "activate", G_CALLBACK(bftextview2_expand_lcb), btv);

	/* loop over the found blocks */
	for (i = 0; i < (btv->bflang->st->blocks->len); i++) {
		if (g_array_index(btv->bflang->st->blocks, Tpattern_block, i).name
			&& g_array_index(btv->bflang->st->blocks, Tpattern_block, i).foldable) {
			gchar *tmp = g_strdup_printf(_("Collapse %s"),
										 g_array_index(btv->bflang->st->blocks, Tpattern_block, i).name);
			mitem = gtk_menu_item_new_with_label(tmp);
			g_free(tmp);
			g_object_set_data(G_OBJECT(mitem), "block_name",
							  g_array_index(btv->bflang->st->blocks, Tpattern_block, i).name);
			gtk_menu_shell_append(GTK_MENU_SHELL(menu), mitem);
			g_signal_connect(G_OBJECT(mitem), "activate", G_CALLBACK(bftextview2_collapse_lcb), btv);

			tmp =
				g_strdup_printf(_("Expand %s"),
								g_array_index(btv->bflang->st->blocks, Tpattern_block, i).name);
			mitem = gtk_menu_item_new_with_label(tmp);
			g_free(tmp);
			g_object_set_data(G_OBJECT(mitem), "block_name",
							  g_array_index(btv->bflang->st->blocks, Tpattern_block, i).name);
			gtk_menu_shell_append(GTK_MENU_SHELL(menu), mitem);
			g_signal_connect(G_OBJECT(mitem), "activate", G_CALLBACK(bftextview2_expand_lcb), btv);
		}
	}


	gtk_widget_show_all(menu);
	/* only required for submenu's that have a radioitem ????? g_signal_connect(G_OBJECT(menu), "destroy", destroy_disposable_menu_cb, menu); */
	return menu;
}

static void
bftextview2_get_iter_at_bevent(BluefishTextView * btv, GdkEventButton * bevent, GtkTextIter * iter)
{
	gint xpos, ypos;
	GtkTextWindowType wintype;

	wintype = gtk_text_view_get_window_type(GTK_TEXT_VIEW(btv), gtk_widget_get_window(GTK_WIDGET(btv)));
	gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(btv), wintype, bevent->x, bevent->y, &xpos, &ypos);
	xpos += gtk_text_view_get_border_window_size(GTK_TEXT_VIEW(btv), GTK_TEXT_WINDOW_LEFT);
	gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(btv), iter, xpos, ypos);
}

static void select_from_line_to_eventy(BluefishTextView * btv, gint line, guint eventy)
{
	GtkTextIter so, eo;
	gint x, y;
	gtk_text_buffer_get_iter_at_line(btv->buffer, &so, line);
	gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(btv), GTK_TEXT_WINDOW_TEXT, 0, eventy, &x, &y);
	gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(btv), &eo, y, &x);
	gtk_text_iter_forward_to_line_end(&eo);
	DEBUG_MSG("select_from_line_to_eventy, line=%d, eventy=%d,select from so=%d to eo=%d\n", line, eventy,
			  gtk_text_iter_get_offset(&so), gtk_text_iter_get_offset(&eo));
	gtk_text_buffer_select_range(btv->buffer, &so, &eo);
}

static gboolean bluefish_text_view_button_press_event(GtkWidget * widget, GdkEventButton * event)
{
	BluefishTextView *btv = BLUEFISH_TEXT_VIEW(widget);
	BluefishTextView *master = BLUEFISH_TEXT_VIEW(btv->master);

	if (main_v->props.editor_spacingtoclick) {
		bluefish_text_view_remove_spacingtoclick(master);
	}
	if (master->margin_pixels_per_char <= 0) {
		calc_pixels_per_char(master);
	}

	DBG_SIGNALS("bluefish_text_view_button_press_event, widget=%p, btv=%p, master=%p, x=%f, y=%f\n", widget,
				btv, master, event->x, event->y);
	btv->button_press_line = -1;
	if (event->window == gtk_text_view_get_window(GTK_TEXT_VIEW(btv), GTK_TEXT_WINDOW_LEFT)) {

		if (event->button == 1) {
			gint x, y;
			GtkTextIter it;

			gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(btv), GTK_TEXT_WINDOW_TEXT, 0, event->y, &x,
												  &y);
			gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(widget), &it, y, &x);

			if (event->type == GDK_2BUTTON_PRESS && (event->x > (master->margin_pixels_chars))
				&& (event->x < (master->margin_pixels_chars + master->margin_pixels_symbol))) {
#if GTK_CHECK_VERSION(3,0,0)
				cairo_region_t *region;
				GdkWindow *window = gtk_text_view_get_window(GTK_TEXT_VIEW(btv), GTK_TEXT_WINDOW_LEFT);
				bmark_toggle(btv->doc, gtk_text_iter_get_offset(&it), NULL, NULL);
				region = gdk_window_get_clip_region(window);
				gdk_window_invalidate_region(window, region, FALSE);
				cairo_region_destroy(region);
#else
				GdkRegion *region;
				bmark_toggle(btv->doc, gtk_text_iter_get_offset(&it), NULL, NULL);
				/* redraw margin */
				region = gdk_drawable_get_clip_region(event->window);
				gdk_window_invalidate_region(event->window, region, FALSE);
				gdk_region_destroy(region);
#endif							/* gtk3 */

				return TRUE;
			}
			if (btv->show_blocks && (event->x > (master->margin_pixels_chars + master->margin_pixels_symbol))) {	/* get the offset that equals the folding area */

				gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(widget), &it, y, &x);
				DBG_FOLD("fold/unfold at offset %d (line %d)\n", gtk_text_iter_get_offset(&it),
						 gtk_text_iter_get_line(&it));
				bftextview2_toggle_fold(btv, &it);
				return TRUE;
			}
			if (event->x < master->margin_pixels_chars) {
				master->button_press_line = gtk_text_iter_get_line(&it);
			}
		} else if (event->button == 3 && master->show_blocks && (event->x > master->margin_pixels_chars)) {
#if GTK_CHECK_VERSION(3,22,0)
			gtk_menu_popup_at_pointer(GTK_MENU(bftextview2_fold_menu(btv)), NULL);
#else
			gtk_menu_popup(GTK_MENU(bftextview2_fold_menu(btv)), NULL, NULL, NULL, NULL, event->button,
						   event->time);
#endif
			return TRUE;
		}
	}
	if (event->button == 1) {
		
		if (event->type == GDK_3BUTTON_PRESS && event->window == gtk_text_view_get_window(GTK_TEXT_VIEW(btv), GTK_TEXT_WINDOW_TEXT)) {
			/* select current line */
			GtkTextIter sit1, sit2;
			gint x, y;
			gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(btv), GTK_TEXT_WINDOW_TEXT, 0, event->y, &x,
												  &y);
			gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(widget), &sit1, y, &x);
			sit2 = sit1;
			gtk_text_iter_forward_to_line_end(&sit2);
			gtk_text_buffer_select_range(btv->buffer, &sit1, &sit2);
		}
	
		if (master->show_mbhl) {
			btv->needs_blockmatch = TRUE;
			if (!btv->mark_set_idle)
				btv->mark_set_idle = g_idle_add_full(G_PRIORITY_HIGH_IDLE, mark_set_idle_lcb, btv, NULL);
		}

		if (main_v->props.editor_spacingtoclick) {
			gint bufx, bufy;
			gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(btv), GTK_TEXT_WINDOW_TEXT, event->x,
												  event->y, &bufx, &bufy);
			master->spacingtoclickend = bufx;
		}

	} else if (event->button == 3) {
		GtkTextIter iter;
		/* store the location of the right mouse button click for menu items like 'edit tag'
		   or 'edit color' */
		bftextview2_get_iter_at_bevent(btv, event, &iter);
		main_v->bevent_doc = master->doc;
		main_v->bevent_charoffset = gtk_text_iter_get_offset(&iter);
	}
	/* here we ask any plugins to do any processing */
	if (main_v->doc_view_button_press_cbs) {
		GSList *tmplist = main_v->doc_view_button_press_cbs;
		while (tmplist) {
			void *(*func) () = tmplist->data;
			DEBUG_MSG
				("bluefish_text_view_button_press_event, calling plugin func %p for widget %p, master %p and doc %p\n",
				 tmplist->data, widget, master, master->doc);
			func(widget, event, (Tdocument *) master->doc);
			tmplist = g_slist_next(tmplist);
		}
	}
	DEBUG_MSG("bluefish_text_view_button_press_event, call parent button_press_event for widget %p\n",
			  widget);
	return GTK_WIDGET_CLASS(bluefish_text_view_parent_class)->button_press_event(widget, event);
}

static gboolean bluefish_text_view_motion_notify_event(GtkWidget * widget, GdkEventMotion * event)
{
	if (((BluefishTextView *) widget)->button_press_line != -1
		&& event->x < ((BluefishTextView *) ((BluefishTextView *) widget)->master)->margin_pixels_chars) {
		DBG_SIGNALS("bluefish_text_view_motion_notify_event, event->x=%d, event->y=%d\n", event->x, event->y);
		select_from_line_to_eventy((BluefishTextView *) widget,
								   ((BluefishTextView *) widget)->button_press_line, event->y);
		return TRUE;
	}
	return GTK_WIDGET_CLASS(bluefish_text_view_parent_class)->motion_notify_event(widget, event);
}

static gboolean bluefish_text_view_button_release_event(GtkWidget * widget, GdkEventButton * event)
{
	BluefishTextView *master = BLUEFISH_TEXT_VIEW(widget)->master;
	BluefishTextView *btv = BLUEFISH_TEXT_VIEW(widget);
	if (((BluefishTextView *) widget)->button_press_line != -1
		&& event->x < ((BluefishTextView *) ((BluefishTextView *) widget)->master)->margin_pixels_chars
		&& event->button == 1) {
		if (!gtk_text_buffer_get_has_selection(((BluefishTextView *) widget)->buffer)) {
			DBG_SIGNALS("bluefish_text_view_button_release_event, event->x=%d, event->y=%d\n", event->x,
						event->y);
			select_from_line_to_eventy((BluefishTextView *) widget,
									   ((BluefishTextView *) widget)->button_press_line, event->y);
			((BluefishTextView *) widget)->button_press_line = -1;
			return TRUE;
		}
		((BluefishTextView *) widget)->button_press_line = -1;
	}

	if (event->button == 1 && main_v->props.editor_spacingtoclick) {
		gint bufx, bufy, numchars;
		GtkTextIter iter;
		
		gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(btv), GTK_TEXT_WINDOW_TEXT, event->x, event->y,
											  &bufx, &bufy);
		if (master->spacingtoclickend == bufx) {
			/*g_print("bufx=%d,bufy=%d\n",bufx,bufy); */
			gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(btv), &iter, bufx, bufy);
			if (gtk_text_iter_ends_line(&iter)) {
				GdkRectangle loc;
				/* the difference between the location of the iter and the requested locations defines the number of characters
				   that need to be inserted */
				gtk_text_view_get_iter_location(GTK_TEXT_VIEW(btv), &iter, &loc);
				/*g_print("iter at line %d, line offset=%d\n",gtk_text_iter_get_line(&iter),gtk_text_iter_get_line_offset(&iter)); */
				numchars = ((bufx - loc.x) / master->margin_pixels_per_char);
				spacingtoclick_insert_spacing(master, numchars, &iter);
			} else {
				master->spacingtoclickend = -1;
			}
		} else {
			master->spacingtoclickend = -1;
		}
	}

	return GTK_WIDGET_CLASS(bluefish_text_view_parent_class)->button_release_event(widget, event);
}

gchar *get_line_indenting(GtkTextBuffer * buffer, GtkTextIter * iter, gboolean prevline)
{
	gchar *string;
	gchar *indenting;
	GtkTextIter itstart, itend;

	itstart = itend = *iter;
	if (prevline) {
		gtk_text_iter_backward_line(&itend);
	}
	/* set to the beginning of the line */
	gtk_text_iter_set_line_index(&itstart, 0);
	string = gtk_text_buffer_get_text(buffer, &itstart, &itend, TRUE);
	if (!string)
		return NULL;
	/*g_print("get_line_indenting, got line '%s' len %d\n",string,strlen(string)); */
	/* now count the indenting in this string */
	indenting = string;
	while (*indenting == '\t' || *indenting == ' ') {
		indenting++;
	}
	/* ending search, non-whitespace found, so terminate at this position */
	*indenting = '\0';
	return string;
}

/*
adds the (smart) indenting after an enter key is released
iter is set to the cursor position
*/

static inline void auto_add_indenting(BluefishTextView * btv, GtkTextIter * iter)
{
	gchar *string;
	gchar lastchar = '\0';
	gboolean next_is_outdent = FALSE, prev_is_outdent = FALSE, prev_is_indent = FALSE;
	GtkTextIter iter2 = *iter;
	string = get_line_indenting(btv->buffer, iter, TRUE);
	if (!string)
		return;

	if (main_v->props.smartindent) {
		gtk_text_iter_backward_chars(&iter2, 2);
		gunichar uc = gtk_text_iter_get_char(&iter2);
		lastchar = (uc < 255) ? uc : 127;	/* 127 = DEL is a not used character */
		if (BLUEFISH_TEXT_VIEW(btv->master)->bflang && lastchar != '\0') {
			if (BLUEFISH_TEXT_VIEW(btv->master)->bflang->smartoutdentchars) {
				next_is_outdent =
					(strchr
					 (BLUEFISH_TEXT_VIEW(btv->master)->bflang->smartoutdentchars,
					  (char) gtk_text_iter_get_char(iter)) != NULL);
				prev_is_outdent =
					(strchr(BLUEFISH_TEXT_VIEW(btv->master)->bflang->smartoutdentchars, lastchar) != NULL);
			}
			if (BLUEFISH_TEXT_VIEW(btv->master)->bflang->smartindentchars) {
				prev_is_indent =
					(strchr(BLUEFISH_TEXT_VIEW(btv->master)->bflang->smartindentchars, lastchar) != NULL);
			}
		}
		/*g_print("auto_add_indenting, previous indenting '%s' strlen=%d\n",string,(int)strlen(string));
		   g_print("auto_add_indenting, lastchar=%c, smartindentchars=%s\n",lastchar, btv->bflang->smartindentchars);
		   g_print("next_is_outdent=%d, prev_is_indent=%d, prev_is_outdent=%d\n",next_is_outdent, prev_is_indent, prev_is_outdent); */
		if (!next_is_outdent && prev_is_indent) {
			gchar *tmp, *tmp2;
			if (BFWIN(DOCUMENT(BLUEFISH_TEXT_VIEW(btv->master)->doc)->bfwin)->session->editor_indent_wspaces)
				tmp2 =
					bf_str_repeat(" ",
								  BFWIN(DOCUMENT(BLUEFISH_TEXT_VIEW(btv->master)->doc)->bfwin)->
								  session->editor_tab_width);
			else
				tmp2 = g_strdup("	");
			tmp = g_strconcat(string, tmp2, NULL);
			g_free(string);
			g_free(tmp2);
			string = tmp;
		} else if (main_v->props.adv_smart_indent_mode == 2 && prev_is_outdent) {
			/* if main_v->props.adv_smart_indent_mode is set to 2 bluefish will unindent if you
			   hit enter after a closing bracket like }
			 */
			gint len;
			/* reduce the indenting in 'string' by one level */
			len = strlen(string);
			if (string[len - 1] == '\t') {
				string[len - 1] = '\0';
			} else if (string[len - 1] == ' ') {
				gint i = len - 1;
				while (string[i] == ' '
					   && i > len - 1 - BFWIN(DOCUMENT(btv->doc)->bfwin)->session->editor_tab_width
					   && i >= 0) {
					i--;
				}
				string[i] = '\0';
			}
		}
	}
	if (string && string[0] != '\0') {
		gboolean in_paste = DOCUMENT(BLUEFISH_TEXT_VIEW(btv->master)->doc)->in_paste_operation;
		/*g_print("bluefish_text_view_key_release_event, autoindent, insert indenting\n"); */
		/* a dirty trick: if in_paste_operation is set, there will be no call
		   for doc_unre_new_group when indenting is inserted */
		if (!in_paste)
			DOCUMENT(BLUEFISH_TEXT_VIEW(btv->master)->doc)->in_paste_operation = TRUE;
		gtk_text_buffer_insert(btv->buffer, iter, string, -1);
		if (!in_paste)
			DOCUMENT(BLUEFISH_TEXT_VIEW(btv->master)->doc)->in_paste_operation = FALSE;
		btv->insert_was_auto_indent = TRUE;
	}
	g_free(string);
}

static inline void auto_decrease_indenting(BluefishTextView * btv, GtkTextIter * iter)
{
	GtkTextIter itend, itstart;
	gunichar uc;
	/* reduce the indenting one level back */
	itend = *iter;
	gtk_text_iter_backward_char(&itend);
	itstart = itend;
	gtk_text_iter_backward_char(&itstart);
	uc = gtk_text_iter_get_char(&itstart);
	/*g_print("found indenting char '%c'\n",uc); */
	if (uc == '\t') {
		gtk_text_buffer_delete(btv->buffer, &itstart, &itend);
	} else if (uc == ' ') {
		int i = 1;
		/* if a space was the previous char, we need N spaces to unindent */
		while (uc == ' ' && i < BFWIN(DOCUMENT(btv->doc)->bfwin)->session->editor_tab_width) {
			gtk_text_iter_backward_char(&itstart);
			uc = gtk_text_iter_get_char(&itstart);
			i++;
		}

		gtk_text_buffer_delete(btv->buffer, &itstart, &itend);
	}
}

static inline void auto_indent_blockstackbased(BluefishTextView * btv)
{
	gchar *tmp2;
	GtkTextIter iter;
	guint offset, num = 0;
	Tfound *found;
	GSequenceIter *siter;
	Tfoundblock *fblock;
	gboolean in_paste;
	DBG_MSG("auto_indent_blockstackbased, started\n");
	gtk_text_buffer_get_iter_at_mark(btv->buffer, &iter, gtk_text_buffer_get_insert(btv->buffer));
	offset = gtk_text_iter_get_offset(&iter);
	found = get_foundcache_at_offset(BLUEFISH_TEXT_VIEW(btv->master), offset, &siter);
	DBG_MSG("auto_indent_blockstackbased, found=%p\n", found);
	if (!found || found->charoffset_o > offset)
		return;
	fblock = found->fblock;
	if (found->numblockchange < 0)
		num = found->numblockchange;
	while (fblock) {
		fblock = (Tfoundblock *) fblock->parentfblock;
		num++;
	}
	DBG_MSG("auto_indent_blockstackbased, num blocks=%d\n", num);
	if (num <= 0)
		return;
	if (BFWIN(DOCUMENT(BLUEFISH_TEXT_VIEW(btv->master)->doc)->bfwin)->session->editor_indent_wspaces)
		tmp2 =
			bf_str_repeat(" ",
						  BFWIN(DOCUMENT(BLUEFISH_TEXT_VIEW(btv->master)->doc)->bfwin)->
						  session->editor_tab_width * num);
	else
		tmp2 = bf_str_repeat("\t", num);
	in_paste = DOCUMENT(BLUEFISH_TEXT_VIEW(btv->master)->doc)->in_paste_operation;
	if (!in_paste)
		DOCUMENT(BLUEFISH_TEXT_VIEW(btv->master)->doc)->in_paste_operation = TRUE;
	gtk_text_buffer_insert(btv->buffer, &iter, tmp2, -1);
	if (!in_paste)
		DOCUMENT(BLUEFISH_TEXT_VIEW(btv->master)->doc)->in_paste_operation = FALSE;
	btv->insert_was_auto_indent = TRUE;
}

static gboolean bluefish_text_view_key_release_event(GtkWidget * widget, GdkEventKey * kevent)
{
	BluefishTextView *btv = BLUEFISH_TEXT_VIEW(widget);
	gboolean prev_insert_was_auto_indent = btv->insert_was_auto_indent;
	btv->insert_was_auto_indent = FALSE;

	DBG_SIGNALS("bluefish_text_view_key_release_event for widget %p, keyval=%d, needs_blockmatch=%d\n",
				widget, kevent->keyval, btv->needs_blockmatch);
	DBG_BLOCKMATCH("bluefish_text_view_key_release_event, master->show_bhl=%d, mark_set_idle=%d, \n",
				   BLUEFISH_TEXT_VIEW(btv->master)->show_mbhl, btv->mark_set_idle);
	if (BLUEFISH_TEXT_VIEW(btv->master)->show_mbhl && !btv->mark_set_idle && btv->needs_blockmatch) {
		btv->mark_set_idle = g_idle_add_full(G_PRIORITY_HIGH_IDLE, mark_set_idle_lcb, btv, NULL);
	}

	/* sometimes we receive a release event for a key that was not pressed in the textview widget!
	   for example if you use the keyboard to navigate the menu, and press enter to activate an item, a
	   key release event is received in the textview widget.... so we have to check that ! */
	if (!btv->key_press_inserted_char)
		return GTK_WIDGET_CLASS(bluefish_text_view_parent_class)->key_release_event(widget, kevent);
	btv->key_press_inserted_char = FALSE;	/* after the check we set this to FALSE */

	if (!BLUEFISH_TEXT_VIEW(btv->master)->auto_indent)
		return GTK_WIDGET_CLASS(bluefish_text_view_parent_class)->key_release_event(widget, kevent);
	/*g_print("bluefish_text_view_key_release_event, working on keyval %d\n",kevent->keyval); */

	if (main_v->props.smartindent == 2 && (kevent->keyval == GDK_Return || kevent->keyval == GDK_KP_Enter)
		&& !((kevent->state & GDK_SHIFT_MASK) || (kevent->state & GDK_CONTROL_MASK)
			 || (kevent->state & GDK_MOD1_MASK))) {
		/* 2 = indent based on the number of blocks on the stack */
		g_print("indent blockstackbased\n");
		auto_indent_blockstackbased(btv);
	} else {
		GtkTextIter iter;
		gtk_text_buffer_get_iter_at_mark(btv->buffer, &iter, gtk_text_buffer_get_insert(btv->buffer));
		if ((kevent->keyval == GDK_Return || kevent->keyval == GDK_KP_Enter)
			&& !((kevent->state & GDK_SHIFT_MASK) || (kevent->state & GDK_CONTROL_MASK)
				 || (kevent->state & GDK_MOD1_MASK))) {
			auto_add_indenting(btv, &iter);
		} else if (main_v->props.smartindent == 1 && prev_insert_was_auto_indent
				   && btv->bflang && btv->bflang->smartoutdentchars
				   && strchr(btv->bflang->smartoutdentchars, kevent->keyval) != NULL) {
			auto_decrease_indenting(btv, &iter);
		}
	}
	return GTK_WIDGET_CLASS(bluefish_text_view_parent_class)->key_release_event(widget, kevent);
}

/* called for example by doc_reload() */
void bluefish_text_view_scan_cleanup(BluefishTextView * btv)
{
	cleanup_scanner(btv);
}

void bluefish_text_view_rescan(BluefishTextView * btv)
{
	DBG_MSG("bluefish_text_view_rescan, btv=%p, lang=%p\n", btv, btv->bflang);
	cleanup_scanner(btv->master);
	if (BLUEFISH_TEXT_VIEW(btv->master)->bflang) {
		GtkTextIter start, end;
		GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(btv->master));
		gtk_text_buffer_get_bounds(buffer, &start, &end);
#ifdef MARKREGION
		markregion_nochange(&btv->scanning, gtk_text_iter_get_offset(&start), gtk_text_iter_get_offset(&end));
		DBG_MARKREGION("bluefish_text_view_rescan, apply needscanning to %d:%d\n",
					   gtk_text_iter_get_offset(&start), gtk_text_iter_get_offset(&end));
#ifdef HAVE_LIBENCHANT
		markregion_nochange(&btv->spellcheck, gtk_text_iter_get_offset(&start),
							gtk_text_iter_get_offset(&end));
#endif
#endif
		btv->needremovetags = 0;
		bftextview2_schedule_scanning(btv);
	}
}

	/* returns TRUE if
	   there is a selection and a comment start and end is inside the selection
	   OR no selection and cursor is inside a comment */
gboolean bluefish_text_view_in_comment(BluefishTextView * btv, GtkTextIter * its, GtkTextIter * ite)
{
	GtkTextIter tmpits, tmpite;
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(btv));
	GtkTextTag *comment_tag = gtk_text_tag_table_lookup(langmgr_get_tagtable(), "comment");
	if (gtk_text_buffer_get_selection_bounds(buffer, &tmpits, &tmpite)) {
		*its = tmpits;
		*ite = tmpite;
		gtk_text_iter_order(&tmpits, &tmpite);
		DEBUG_MSG("bluefish_text_view_in_comment, testing selection from %d:%d\n",
				  gtk_text_iter_get_offset(&tmpits), gtk_text_iter_get_offset(&tmpite));
		/* first test if the selection starts the tag and selection ends ends the tag */
		if (gtk_text_iter_begins_tag(&tmpits, comment_tag)) {
			/*g_print("tmpite at %d is_end=%d, ends_tag=%d, ends_line=%d\n", gtk_text_iter_get_offset(&tmpite), gtk_text_iter_is_end(&tmpite), gtk_text_iter_ends_tag(&tmpite, comment_tag), gtk_text_iter_ends_line(&tmpite)); */
			if (gtk_text_iter_ends_tag(&tmpite, comment_tag) || gtk_text_iter_is_end(&tmpite)) {
				DEBUG_MSG("bluefish_text_view_in_comment, selection %d:%d toggles comment, return TRUE\n",
						  gtk_text_iter_get_offset(&tmpits), gtk_text_iter_get_offset(&tmpite));
				return TRUE;
			}
			/* in line comments, a comment may start after the newline, but the selection may not include the newline */
			if (gtk_text_iter_ends_line(&tmpite)) {
				gtk_text_iter_forward_line(&tmpite);
				/*g_print("tmpite at %d is_end=%d, ends_tag=%d, ends_line=%d\n", gtk_text_iter_get_offset(&tmpite), gtk_text_iter_is_end(&tmpite), gtk_text_iter_ends_tag(&tmpite, comment_tag), gtk_text_iter_ends_line(&tmpite)); */
				if (gtk_text_iter_ends_tag(&tmpite, comment_tag) || gtk_text_iter_is_end(&tmpite)) {
					DEBUG_MSG
						("bluefish_text_view_in_comment, selection %d:%d (including newline) toggles comment, return TRUE\n",
						 gtk_text_iter_get_offset(&tmpits), gtk_text_iter_get_offset(&tmpite));
					return TRUE;
				}
			}
		}
		DEBUG_MSG("bluefish_text_view_in_comment, selection %d:%d does NOT toggle comment, return FALSE\n",
				  gtk_text_iter_get_offset(&tmpits), gtk_text_iter_get_offset(&tmpite));
		return FALSE;
	} else {
		gboolean retval;
		gtk_text_buffer_get_iter_at_mark(buffer, &tmpits, gtk_text_buffer_get_insert(buffer));
		/* for line comments the \n is usually not part of the comment anymore, so move back one char */
		if (!gtk_text_iter_starts_line(&tmpits) && gtk_text_iter_ends_line(&tmpits))
			gtk_text_iter_backward_char(&tmpits);
		retval = gtk_text_iter_has_tag(&tmpits, comment_tag);
		*ite = *its = tmpits;
		if (retval) {
			gtk_text_iter_forward_to_tag_toggle(ite, comment_tag);
			gtk_text_iter_backward_to_tag_toggle(its, comment_tag);
			return TRUE;
		}
	}
	return FALSE;
}

Tcomment *bluefish_text_view_get_comment(BluefishTextView * btv, GtkTextIter * it,
										 Tcomment_type preferred_type)
{
	/* get the context, and then retrieve the preferred comment type for that context */
	GQueue *contextstack;
	GList *tmplist;
	guint16 contextnum = 0;

	if (!btv->bflang)
		return NULL;

	if (!btv->bflang->st)
		return NULL;

	if (btv->master != btv)
		g_warning("bluefish_text_view_get_comment should only be called for the master widget\n");

	contextstack = get_contextstack_at_position(btv, it);
/*	g_print("bluefish_text_view_get_comment, got contextstack %p with len %d\n",contextstack,contextstack->length);*/
	if (!contextstack)
		return NULL;

	for (tmplist = g_list_first(contextstack->head); contextnum != 1; tmplist = g_list_next(tmplist)) {
		guint8 line, block;
		if (tmplist) {
			contextnum = GPOINTER_TO_INT(tmplist->data);
		} else {
			contextnum = 1;
		}
		DEBUG_MSG("bluefish_text_view_get_comment, contextnum=%d\n", contextnum);
		line = g_array_index(btv->bflang->st->contexts, Tcontext, contextnum).comment_line;
		block = g_array_index(btv->bflang->st->contexts, Tcontext, contextnum).comment_block;
		DEBUG_MSG
			("bluefish_text_view_get_comment, type %d (line) has index %d, type %d (block) has index %d\n",
			 comment_type_line, line, comment_type_block, block);
		if (line == COMMENT_INDEX_NONE && block == COMMENT_INDEX_NONE)
			return NULL;

		if ((line == COMMENT_INDEX_INHERIT && block == COMMENT_INDEX_INHERIT)
			|| (line == COMMENT_INDEX_INHERIT && block == COMMENT_INDEX_NONE)
			|| (line == COMMENT_INDEX_NONE && block == COMMENT_INDEX_INHERIT)
			)
			continue;
		DEBUG_MSG("preferred_type %d\n", preferred_type);
		if (preferred_type == comment_type_block) {
			if (block == COMMENT_INDEX_NONE) {
				return &g_array_index(btv->bflang->st->comments, Tcomment, line);
			} else if (block == COMMENT_INDEX_INHERIT) {
				continue;
			} else {
				return &g_array_index(btv->bflang->st->comments, Tcomment, block);
			}
		} else {
			if (line == COMMENT_INDEX_NONE) {
				return &g_array_index(btv->bflang->st->comments, Tcomment, block);
			} else if (line == COMMENT_INDEX_INHERIT) {
				continue;
			} else {
				return &g_array_index(btv->bflang->st->comments, Tcomment, line);
			}
		}
	}
	return NULL;
}


gboolean bluefish_text_view_get_auto_complete(BluefishTextView * btv)
{
	return (BLUEFISH_TEXT_VIEW(btv->master)->auto_complete);
}

void bluefish_text_view_set_auto_complete(BluefishTextView * btv, gboolean enable)
{
	g_return_if_fail(btv != NULL);

	if (enable == BLUEFISH_TEXT_VIEW(btv->master)->auto_complete) {
		return;
	}

	BLUEFISH_TEXT_VIEW(btv->master)->auto_complete = enable;
}

gboolean bluefish_text_view_get_auto_indent(BluefishTextView * btv)
{
	return (BLUEFISH_TEXT_VIEW(btv->master)->auto_indent);
}

void bluefish_text_view_set_auto_indent(BluefishTextView * btv, gboolean enable)
{
	g_return_if_fail(btv != NULL);

	if (enable == BLUEFISH_TEXT_VIEW(btv->master)->auto_indent) {
		return;
	}

	BLUEFISH_TEXT_VIEW(btv->master)->auto_indent = enable;
}

static void bftextview2_parse_static_colors(void)
{
#if GTK_CHECK_VERSION(3,0,0)
	if (!(main_v->props.btv_color_str[BTV_COLOR_CURRENT_LINE]
		  && gdk_rgba_parse(&st_cline_color, main_v->props.btv_color_str[BTV_COLOR_CURRENT_LINE]))) {
		gdk_rgba_parse(&st_cline_color, "#e0e0e0");
	}
	if (!(main_v->props.btv_color_str[BTV_COLOR_WHITESPACE]
		  && gdk_rgba_parse(&st_whitespace_color, main_v->props.btv_color_str[BTV_COLOR_WHITESPACE]))) {
		gdk_rgba_parse(&st_whitespace_color, "#ff0000");
	}
	if (!(main_v->props.btv_color_str[BTV_COLOR_CURSOR_HIGHLIGHT]
		  && gdk_rgba_parse(&st_cursor_highlight_color,
							main_v->props.btv_color_str[BTV_COLOR_CURSOR_HIGHLIGHT]))) {
		gdk_rgba_parse(&st_cursor_highlight_color, "#ffff33");
	}
	if (!(main_v->props.btv_color_str[BTV_COLOR_MARGIN_FG]
		  && gdk_rgba_parse(&st_margin_fg_color, main_v->props.btv_color_str[BTV_COLOR_MARGIN_FG]))) {
		gdk_rgba_parse(&st_margin_fg_color, "#000000");
	}
	if (!(main_v->props.btv_color_str[BTV_COLOR_MARGIN_BG]
		  && gdk_rgba_parse(&st_margin_bg_color, main_v->props.btv_color_str[BTV_COLOR_MARGIN_BG]))) {
		gdk_rgba_parse(&st_margin_bg_color, "#dddddd");
	}
#else
	GString *str;

	if (!(main_v->props.btv_color_str[BTV_COLOR_CURRENT_LINE]
		  && gdk_color_parse(main_v->props.btv_color_str[BTV_COLOR_CURRENT_LINE], &st_cline_color))) {
		gdk_color_parse("#e0e0e0", &st_cline_color);
	}
	if (!(main_v->props.btv_color_str[BTV_COLOR_WHITESPACE]
		  && gdk_color_parse(main_v->props.btv_color_str[BTV_COLOR_WHITESPACE], &st_whitespace_color))) {
		gdk_color_parse("#ff0000", &st_whitespace_color);
	}
	if (!(main_v->props.btv_color_str[BTV_COLOR_CURSOR_HIGHLIGHT]
		  && gdk_color_parse(main_v->props.btv_color_str[BTV_COLOR_CURSOR_HIGHLIGHT],
							 &st_cursor_highlight_color))) {
		gdk_color_parse("#ffff33", &st_cursor_highlight_color);
	}
	if (!(main_v->props.btv_color_str[BTV_COLOR_MARGIN_FG]
		  && gdk_color_parse(main_v->props.btv_color_str[BTV_COLOR_MARGIN_FG], &st_margin_fg_color))) {
		gdk_color_parse("#000000", &st_margin_fg_color);
	}
	if (!(main_v->props.btv_color_str[BTV_COLOR_MARGIN_BG]
		  && gdk_color_parse(main_v->props.btv_color_str[BTV_COLOR_MARGIN_BG], &st_margin_bg_color))) {
		gdk_color_parse("#000000", &st_margin_bg_color);
	}

	str = g_string_new("style \"bluefish-cursor\" {");
	if (!main_v->props.use_system_colors && main_v->props.btv_color_str[BTV_COLOR_CURSOR] != NULL
		&& main_v->props.btv_color_str[BTV_COLOR_CURSOR][0] != '\0') {
		g_string_append_printf(str, " GtkTextView::cursor-color = \"%s\"",
							   main_v->props.btv_color_str[BTV_COLOR_CURSOR]);
	}
	g_string_append_printf(str,
						   " GtkWidget::cursor-aspect-ratio = %f }class \"GtkTextView\" style \"bluefish-cursor\"",
						   ((gfloat) (main_v->props.cursor_size) / 100.0));
	gtk_rc_parse_string(str->str);
	g_string_free(str, TRUE);
#endif
}

void bftextview2_init_globals(void)
{
	DBG_MSG("sizeof(Tfound)=%ld, sizeof(Tfoundcontext)=%ld,sizeof(Tfoundblock)=%ld\n", (glong) sizeof(Tfound),
			(glong) sizeof(Tfoundcontext), (glong) sizeof(Tfoundblock));
	bftextview2_parse_static_colors();
	if (main_v->props.autocomp_accel_string && main_v->props.autocomp_accel_string[0] != '\0') {
		gtk_accelerator_parse(main_v->props.autocomp_accel_string, &main_v->autocomp_accel_key,
							  &main_v->autocomp_accel_mods);
		if (gtk_accelerator_valid(main_v->autocomp_accel_key, main_v->autocomp_accel_mods)) {
			return;
		}
		g_warning("%s is not a valid shortcut key combination\n", main_v->props.autocomp_accel_string);
	}
	main_v->autocomp_accel_key = ' ';
	main_v->autocomp_accel_mods = GDK_CONTROL_MASK;
}

void bluefish_text_view_set_colors(BluefishTextView * btv, gchar * const *colors)
{
	gchar *curlocale = g_strdup(setlocale(LC_NUMERIC, NULL));
	setlocale(LC_NUMERIC, "C");
#if GTK_CHECK_VERSION(3,0,0)
	GdkRGBA color;
	GString *str = g_string_new("");
#if GTK_CHECK_VERSION(3,20,0)
	g_string_append_printf(str, "BluefishTextView {-gtk-cursor-aspect-ratio: %f;}",
						   (gfloat) (main_v->props.cursor_size / 100.0));
#else							/* GTK_CHECK_VERSION(3,20,0) */
	/* in 3.18 I found that this works: "BluefishTextView {-GtkWidget-cursor-aspect-ratio: 0.4;}" */
	g_string_append_printf(str, "BluefishTextView {-GtkWidget-cursor-aspect-ratio: %f;}",
						   (gfloat) (main_v->props.cursor_size / 100.0));
#endif							/* GTK_CHECK_VERSION(3,20,0) */
	if (!main_v->props.use_system_colors) {
#if GTK_CHECK_VERSION(3,20,0)
		if (colors[BTV_COLOR_ED_BG] && colors[BTV_COLOR_ED_BG][0] != '\0') {
			g_string_append_printf(str, "bluefishtextview text {background-color: %s;}",
								   colors[BTV_COLOR_ED_BG]);
		}
		if (colors[BTV_COLOR_SELECTION] && colors[BTV_COLOR_SELECTION][0] != '\0') {
			g_string_append_printf(str,
								   "bluefishtextview selection {background-color: %s;} bluefishtextview selection:focus {background-color: %s;}",
								   colors[BTV_COLOR_SELECTION], colors[BTV_COLOR_SELECTION]);
		}
		if (colors[BTV_COLOR_ED_FG] && gdk_rgba_parse(&color, colors[BTV_COLOR_ED_FG])) {
			g_string_append_printf(str, "bluefishtextview text {color: %s;}", colors[BTV_COLOR_ED_FG]);
		}
		if (colors[BTV_COLOR_CURSOR] && gdk_rgba_parse(&color, colors[BTV_COLOR_CURSOR])) {
			g_string_append_printf(str, "bluefishtextview text {caret-color: %s;}", colors[BTV_COLOR_CURSOR]);
		}
#else							/* GTK_CHECK_VERSION(3,20,0) */
		if (colors[BTV_COLOR_ED_BG] && colors[BTV_COLOR_ED_BG][0] != '\0') {
			g_string_append_printf(str, "BluefishTextView.view {background-color: %s;}",
								   colors[BTV_COLOR_ED_BG]);
		}
		if (colors[BTV_COLOR_SELECTION] && colors[BTV_COLOR_SELECTION][0] != '\0') {
			g_string_append_printf(str,
								   "BluefishTextView.view:selected {background-color: %s;} BluefishTextView.view:selected:focused {background-color: %s;}",
								   colors[BTV_COLOR_SELECTION], colors[BTV_COLOR_SELECTION]);
		}
		if (colors[BTV_COLOR_ED_FG] && gdk_rgba_parse(&color, colors[BTV_COLOR_ED_FG])) {
			gtk_widget_override_color(GTK_WIDGET(btv), GTK_STATE_FLAG_NORMAL /*0x7F */ , &color);
			if (btv->slave)
				gtk_widget_override_color(GTK_WIDGET(btv->slave), GTK_STATE_FLAG_NORMAL /*0x7f */ , &color);
		}
		if (colors[BTV_COLOR_CURSOR] && gdk_rgba_parse(&color, colors[BTV_COLOR_CURSOR])) {
			gtk_widget_override_cursor(GTK_WIDGET(btv), &color, &color);
			if (btv->slave)
				gtk_widget_override_cursor(GTK_WIDGET(btv->slave), &color, &color);
		}
#endif							/* GTK_CHECK_VERSION(3,20,0) */
	}
	if (str->len > 0) {
		GtkStyleContext *stc;
		GtkCssProvider *cssp = gtk_css_provider_new();
		DBG_MSG("gtk >= 3.0.0, about to apply CSS %s\n", str->str);
		gtk_css_provider_load_from_data(cssp, str->str, -1, NULL);
		stc = gtk_widget_get_style_context(GTK_WIDGET(btv));
		gtk_style_context_add_provider(stc, GTK_STYLE_PROVIDER(cssp),
									   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
		if (btv->slave) {
			stc = gtk_widget_get_style_context(GTK_WIDGET(btv));
			gtk_style_context_add_provider(stc, GTK_STYLE_PROVIDER(cssp),
										   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
		}
		g_object_unref(G_OBJECT(cssp));
	}
	g_string_free(str, TRUE);
#else
	if (!main_v->props.use_system_colors) {
		GdkColor color;
		if (colors[BTV_COLOR_ED_BG] && gdk_color_parse(colors[BTV_COLOR_ED_BG], &color)) {
			gtk_widget_modify_base(GTK_WIDGET(btv), GTK_STATE_NORMAL, &color);
			if (btv->slave)
				gtk_widget_modify_base(GTK_WIDGET(btv->slave), GTK_STATE_NORMAL, &color);
		}
		if (colors[BTV_COLOR_ED_FG] && gdk_color_parse(colors[BTV_COLOR_ED_FG], &color)) {
			gtk_widget_modify_text(GTK_WIDGET(btv), GTK_STATE_NORMAL, &color);
			if (btv->slave)
				gtk_widget_modify_text(GTK_WIDGET(btv->slave), GTK_STATE_NORMAL, &color);
		}
		if (colors[BTV_COLOR_SELECTION] && gdk_color_parse(colors[BTV_COLOR_SELECTION], &color)) {
			gtk_widget_modify_base(GTK_WIDGET(btv), GTK_STATE_SELECTED, &color);
			gtk_widget_modify_base(GTK_WIDGET(btv), GTK_STATE_ACTIVE, &color);
			if (btv->slave) {
				gtk_widget_modify_base(GTK_WIDGET(btv->slave), GTK_STATE_SELECTED, &color);
				gtk_widget_modify_base(GTK_WIDGET(btv->slave), GTK_STATE_ACTIVE, &color);
			}
		}
	}
#endif
	setlocale(LC_NUMERIC, curlocale);
	g_free(curlocale);
}

void bluefish_text_view_select_language(BluefishTextView * btv, const gchar * mime, const gchar * filename)
{
	GtkTextIter start, end;
	GtkTextBuffer *buffer;
	BluefishTextView *master = btv->master;
	Tbflang *bflang = langmgr_get_bflang(mime, filename);

	if (bflang == master->bflang) {
		DBG_MSG("bluefish_text_view_set_mimetype, nothing to do for btv %p\n", btv);
		return;
	}
	buffer = master->buffer;
	/* remove all highlighting */
	cleanup_scanner(master);
	DBG_MSG("bluefish_text_view_set_mimetype, found bflang %p for mimetype %s\n", bflang, mime);
	if (bflang) {
		/* set new language */
		master->bflang = bflang;
		/* restart scanning */
		gtk_text_buffer_get_bounds(buffer, &start, &end);
#ifdef MARKREGION
		markregion_nochange(&master->scanning, gtk_text_iter_get_offset(&start),
							gtk_text_iter_get_offset(&end));
		DBG_MARKREGION("bluefish_text_view_set_mimetype, apply needscanning to %d:%d\n",
					   gtk_text_iter_get_offset(&start), gtk_text_iter_get_offset(&end));
#ifdef HAVE_LIBENCHANT
		markregion_nochange(&master->spellcheck, gtk_text_iter_get_offset(&start),
							gtk_text_iter_get_offset(&end));
#endif
#endif
		btv->needremovetags = 0;
		if (master->enable_scanner) {
			DBG_MSG("bluefish_text_view_select_language, schedule scanning\n");
			bftextview2_schedule_scanning(master);
		}
	} else {
		master->bflang = NULL;
	}
	DBG_MSG("bluefish_text_view_set_mimetype, done for btv %p\n", btv);
}

gboolean bluefish_text_view_get_show_blocks(BluefishTextView * btv)
{
	return (btv->show_blocks);
}

void bluefish_text_view_set_show_blocks(BluefishTextView * btv, gboolean show)
{
	g_return_if_fail(btv != NULL);

	if (show == btv->show_blocks) {
		return;
	}

	btv->show_blocks = show;
	bftextview2_set_margin_size(btv);
	gtk_widget_queue_draw(GTK_WIDGET(btv));
	if (btv->slave)
		gtk_widget_queue_draw(GTK_WIDGET(btv->slave));
}

void bluefish_text_view_set_show_symbols_redraw(BluefishTextView * btv, gboolean show)
{
	g_return_if_fail(btv != NULL);

	if (show != btv->showsymbols) {
		btv->showsymbols = show;
		bftextview2_set_margin_size(btv);
	}
	gtk_widget_queue_draw(GTK_WIDGET(btv));
	if (btv->slave)
		gtk_widget_queue_draw(GTK_WIDGET(btv->slave));
}

gboolean bluefish_text_view_get_show_line_numbers(BluefishTextView * btv)
{
	return (btv->show_line_numbers);
}

void bluefish_text_view_set_show_line_numbers(BluefishTextView * btv, gboolean show)
{
	g_return_if_fail(btv != NULL);

	if (show == btv->show_line_numbers) {
		return;
	}

	btv->show_line_numbers = show;
	bftextview2_set_margin_size(btv);
	gtk_widget_queue_draw(GTK_WIDGET(btv));
	if (btv->slave)
		gtk_widget_queue_draw(GTK_WIDGET(btv->slave));
}

gboolean bluefish_text_view_get_show_visible_spacing(BluefishTextView * btv)
{
	return (btv->visible_spacing);
}

void bluefish_text_view_set_show_visible_spacing(BluefishTextView * btv, gboolean show)
{
	g_return_if_fail(btv != NULL);

	if (show == btv->visible_spacing) {
		return;
	}

	btv->visible_spacing = show;
	gtk_widget_queue_draw(GTK_WIDGET(btv));
	if (btv->slave)
		gtk_widget_queue_draw(GTK_WIDGET(btv->slave));
}

gboolean bluefish_text_view_get_show_indenting(BluefishTextView * btv)
{
	return (btv->show_indenting);
}

void bluefish_text_view_set_show_indenting(BluefishTextView * btv, gboolean show)
{
	g_return_if_fail(btv != NULL);

	if (show == btv->show_indenting) {
		return;
	}

	btv->show_indenting = show;
	gtk_widget_queue_draw(GTK_WIDGET(btv));
	if (btv->slave)
		gtk_widget_queue_draw(GTK_WIDGET(btv->slave));
}

gboolean bluefish_text_view_get_show_right_margin(BluefishTextView * btv)
{
	return (btv->show_right_margin);
}

void bluefish_text_view_set_show_right_margin(BluefishTextView * btv, gboolean show)
{
	g_return_if_fail(btv != NULL);

	if (show == btv->show_right_margin) {
		return;
	}

	btv->show_right_margin = show;
	gtk_widget_queue_draw(GTK_WIDGET(btv));
	if (btv->slave)
		gtk_widget_queue_draw(GTK_WIDGET(btv->slave));
}

void bluefish_text_view_set_font(BluefishTextView * btv, PangoFontDescription * font_desc)
{
	gtk_widget_modify_font(GTK_WIDGET(btv), font_desc);
	if (btv->slave)
		gtk_widget_modify_font(btv->slave, font_desc);
	btv->margin_pixels_per_char = 0;
	bftextview2_set_margin_size(btv);
}

gboolean bluefish_text_view_get_show_mbhl(BluefishTextView * btv)
{
	return (btv->show_mbhl);
}

void bluefish_text_view_set_show_mbhl(BluefishTextView * btv, gboolean show)
{
	g_return_if_fail(btv != NULL);

	if (show == btv->show_mbhl) {
		return;
	}
	btv->show_mbhl = show;
	if (!show && btv->showing_blockmatch) {
		GtkTextIter it1, it2;
		gtk_text_buffer_get_bounds(btv->buffer, &it1, &it2);
		gtk_text_buffer_remove_tag(btv->buffer, BLUEFISH_TEXT_VIEW(btv)->blockmatch, &it1, &it2);
		btv->showing_blockmatch = FALSE;
	}
	gtk_widget_queue_draw(GTK_WIDGET(btv));
	if (btv->slave)
		gtk_widget_queue_draw(GTK_WIDGET(btv->slave));
}

#ifdef HAVE_LIBENCHANT
void bluefish_text_view_set_spell_check(BluefishTextView * btv, gboolean spell_check)
{
	GtkTextIter start, end;
	BluefishTextView *master = btv->master;

	g_return_if_fail(btv != NULL);

	if (master->spell_check == spell_check) {
		return;
	}

	master->spell_check = spell_check;
	gtk_text_buffer_get_bounds(master->buffer, &start, &end);

	if (master->spell_check) {
#ifdef MARKREGION
		markregion_nochange(&master->spellcheck, 0, gtk_text_iter_get_offset(&end));
#endif
		bftextview2_schedule_scanning(master);
	} else {
		gtk_text_buffer_remove_tag_by_name(master->buffer, "_spellerror_", &start, &end);
	}
}
#endif

static gboolean
bluefish_text_view_query_tooltip(GtkWidget * widget, gint x, gint y, gboolean keyboard_tip,
								 GtkTooltip * tooltip)
{
	BluefishTextView *btv = BLUEFISH_TEXT_VIEW(widget);
	BluefishTextView *master = BLUEFISH_TEXT_VIEW(btv->master);

	if (!keyboard_tip
		&& x < (master->margin_pixels_chars + master->margin_pixels_block + master->margin_pixels_symbol)) {
		gint bx, by, trailing;
		GtkTextIter iter;
		gchar *str;
		gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(btv), GTK_TEXT_WINDOW_LEFT, x, y, &bx, &by);
		/*g_print("bluefish_text_view_query_tooltip, get bookmark popup for %d:%d got buffer coordinates %d:%d\n",x,y,bx,by); */
		gtk_text_view_get_iter_at_position(GTK_TEXT_VIEW(btv), &iter, &trailing, MIN(bx, 0), by);
		str = bmark_get_tooltip_for_line(master->doc, gtk_text_iter_get_line(&iter));
		if (str) {
			gtk_tooltip_set_markup(tooltip, str);
			g_free(str);
			return TRUE;
		}
		return FALSE;
	}

	if (master->bflang && master->bflang->st && master->enable_scanner && master->scanner_idle == 0
		&& main_v->props.show_tooltip_reference) {
		GtkTextIter iter, mstart;
		GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(master));
		gint contextnum;
		guint matchnum;
		/* get position */
		if (keyboard_tip) {
			gint offset;
			g_object_get(buffer, "cursor-position", &offset, NULL);
			gtk_text_buffer_get_iter_at_offset(buffer, &iter, offset);
		} else {
			gint bx, by, trailing;
			/*g_print("get iter at mouse position x=%d-margin=%d y=%d\n",x,x - (btv->margin_pixels_chars + btv->margin_pixels_block +
			   btv->margin_pixels_symbol),y); */
			gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(btv), GTK_TEXT_WINDOW_TEXT,
												  x - (btv->margin_pixels_chars + btv->margin_pixels_block +
													   btv->margin_pixels_symbol), y, &bx, &by);
			/*g_print("bluefish_text_view_query_tooltip, get reference popup for %d:%d got buffer coordinates %d:%d\n",x,y,bx,by); */
			if (bx < 0)
				return FALSE;
			/* if I don't do this check, I get the following error during 'Collapse all'
			   (bluefish-unstable:3866): Gtk-WARNING **: /build/buildd/gtk+2.0-2.16.1/gtk/gtktextbtree.c:4017: byte index off the end of the line
			   Gtk-ERROR **: Byte index 533 is off the end of the line aborting...

			   #0  0xb80bf430 in __kernel_vsyscall ()
			   #1  0xb761f6d0 in raise () from /lib/tls/i686/cmov/libc.so.6
			   #2  0xb7621098 in abort () from /lib/tls/i686/cmov/libc.so.6
			   #3  0xb77b3eac in IA__g_logv (log_domain=0xb7fcba77 "Gtk",
			   log_level=G_LOG_LEVEL_ERROR,
			   format=0xb8076f74 "Byte index %d is off the end of the line",
			   args1=0xbfcdc06c "\206\002")
			   at /build/buildd/glib2.0-2.20.1/glib/gmessages.c:506
			   #4  0xb77b3ee6 in IA__g_log (log_domain=0xb7fcba77 "Gtk",
			   log_level=G_LOG_LEVEL_ERROR,
			   format=0xb8076f74 "Byte index %d is off the end of the line")
			   at /build/buildd/glib2.0-2.20.1/glib/gmessages.c:526
			   #5  0xb7ed4ee7 in iter_set_from_byte_offset (iter=0xbfcdc0c4, line=0x9b1cbb8,
			   byte_offset=646) at /build/buildd/gtk+2.0-2.16.1/gtk/gtktextiter.c:110
			   #6  0xb7ed967f in IA__gtk_text_iter_set_visible_line_index (iter=0xbfcdc1e4,
			   byte_on_line=4) at /build/buildd/gtk+2.0-2.16.1/gtk/gtktextiter.c:3906
			   #7  0xb7edc8c0 in line_display_index_to_iter (layout=0x9711728,
			   display=0x9c30618, iter=0xbfcdc1e4, index=69, trailing=0)
			   at /build/buildd/gtk+2.0-2.16.1/gtk/gtktextlayout.c:2549
			   #8  0xb7ee099c in IA__gtk_text_layout_get_iter_at_position (layout=0x9711728,
			   target_iter=0xbfcdc1e4, trailing=0xbfcdc21c, x=-5, y=<value optimized out>)
			   at /build/buildd/gtk+2.0-2.16.1/gtk/gtktextlayout.c:2670
			   #9  0x080646cd in bluefish_text_view_query_tooltip (widget=0x9783050, x=47,
			   y=88, keyboard_tip=0, tooltip=0x97d0a38) at bftextview2.c:1187

			   I guess this is a race condition, during collapse all a lot of text is made hidden
			   and for the tooltip we request an iter somewhere in the text that is becoming hidden
			 */
			/*g_print("get iter at buffer position bx=%d by=%d\n",bx,by); */
			/* gtk 2.14 cannot handle a NULL instead of &trailing. so although the docs tell
			   that if you don't need it you can pass NULL, we will not do so. */
			gtk_text_view_get_iter_at_position(GTK_TEXT_VIEW(btv), &iter, &trailing, bx, by);
			/*g_print("done\n"); */
		}
		mstart = iter;
		gtk_text_iter_set_line_offset(&mstart, 0);
		gtk_text_iter_forward_char(&iter);
		DBG_TOOLTIP("scan for tooltip: start at %d, position=%d...\n", gtk_text_iter_get_offset(&mstart),
					gtk_text_iter_get_offset(&iter));
		matchnum = scan_for_identifier_at_position(master, &mstart, &iter, &contextnum, NULL, NULL);
		if (matchnum) {
			if (g_array_index(master->bflang->st->matches, Tpattern, matchnum).reference) {
				gtk_tooltip_set_markup(tooltip,
									   g_array_index(master->bflang->st->matches, Tpattern,
													 matchnum).reference);
				return TRUE;
			}



/*			DBG_TOOLTIP("we have a match in context %d, has_patternhash=%d\n", contextnum,
						(g_array_index(master->bflang->st->contexts, Tcontext, contextnum).patternhash !=
						 NULL));
			if (g_array_index(master->bflang->st->contexts, Tcontext, contextnum).patternhash) {
				gint pattern_id;
				gchar *key = gtk_text_buffer_get_text(buffer, &mstart, &iter, TRUE);
				g_print("lookup reference for %s\n",key);
				pattern_id =
					GPOINTER_TO_INT(g_hash_table_lookup
									(g_array_index
									 (master->bflang->st->contexts, Tcontext, contextnum).patternhash, key));
				if (pattern_id && g_array_index(master->bflang->st->matches, Tpattern, pattern_id).reference) {
					DBG_TOOLTIP("key=%s, value=%s\n", key,
								g_array_index(master->bflang->st->matches, Tpattern, pattern_id).reference);
					gtk_tooltip_set_markup(tooltip,
										   g_array_index(master->bflang->st->matches, Tpattern,
														 pattern_id).reference);
					g_free(key);
					return TRUE;
				}
				g_free(key);
			}*/
		}
	}

	return FALSE;
}

static gboolean bluefish_text_view_focus_out_event(GtkWidget * widget, GdkEventFocus * event)
{
	if (BLUEFISH_TEXT_VIEW(widget)->autocomp) {
		autocomp_stop(BLUEFISH_TEXT_VIEW(widget));
	}
	return GTK_WIDGET_CLASS(bluefish_text_view_parent_class)->focus_out_event(widget, event);
}

#if GTK_CHECK_VERSION(3,16,0)

#define bf_text_iter_char_in_string(iter, string) (string?strchr(string, gtk_text_iter_get_char(iter)):FALSE)

static gboolean bf_gtk_text_iter_forward_visible_word_end(GtkTextIter * iter, const gchar *smartselectionchars)
{
	gboolean ret=TRUE;
	DBG_MSG("bf_gtk_text_iter_forward_visible_word_start, iter=%c ends_word=%d\n",gtk_text_iter_get_char(iter),gtk_text_iter_starts_word(iter));
	if (!gtk_text_iter_ends_word(iter) && gtk_text_iter_inside_word(iter)) {
		ret = gtk_text_iter_forward_visible_word_end(iter);
		DBG_MSG("bf_gtk_text_iter_forward_visible_word_start, iter=%c\n",gtk_text_iter_get_char(iter));
	}
	while (!gtk_text_iter_is_end(iter)) {
		if (bf_text_iter_char_in_string(iter, smartselectionchars)) {
			gtk_text_iter_forward_char(iter);
		} else if (gtk_text_iter_starts_word(iter)) {
			ret = gtk_text_iter_forward_visible_word_end(iter);
		} else {
			break;
		}
	}
	return ret;
}
static gboolean bf_gtk_text_iter_starts_word(GtkTextIter * iter, const gchar *smartselectionchars)
{
	GtkTextIter before = *iter;
	if (bf_text_iter_char_in_string(iter, smartselectionchars)
		|| (gtk_text_iter_backward_char(&before) && bf_text_iter_char_in_string(&before, smartselectionchars))) {
		DBG_MSG("starts: before has %c, return FALSE\n", gtk_text_iter_get_char(&before));
		return FALSE;
	}
	return gtk_text_iter_starts_word(iter);
}

static gboolean bf_gtk_text_iter_backward_visible_word_start(GtkTextIter * iter, const gchar *smartselectionchars)
{
	gboolean ret=TRUE;
	GtkTextIter before;
	DBG_MSG("bf_gtk_text_iter_backward_visible_word_start, iter=%c ends_word=%d\n",gtk_text_iter_get_char(iter),gtk_text_iter_starts_word(iter));
	if (!gtk_text_iter_starts_word(iter) && gtk_text_iter_inside_word(iter)) {
		ret = gtk_text_iter_backward_visible_word_start(iter);
		DBG_MSG("bf_gtk_text_iter_backward_visible_word_start, iter=%c\n",gtk_text_iter_get_char(iter));
	}
	while (!gtk_text_iter_is_start(iter)) {
		before = *iter;
		gtk_text_iter_backward_char(&before);
		DBG_MSG("bf_gtk_text_iter_backward_visible_word_start, in loop, iter=%c, before=%c\n",gtk_text_iter_get_char(iter),gtk_text_iter_get_char(&before));
		if (bf_text_iter_char_in_string(&before, smartselectionchars)) {
			/* one step back */
			*iter = before;
		} else if (gtk_text_iter_ends_word(iter)) {
			ret = gtk_text_iter_backward_visible_word_start(iter);
		} else {
			break;
		}
	}
	return ret;
}


static gboolean bf_gtk_text_iter_ends_word(GtkTextIter * iter, const gchar *smartselectionchars)
{
	if (bf_text_iter_char_in_string(iter, smartselectionchars)) {
		DBG_MSG("ends: iter has %c, return FALSE\n", gtk_text_iter_get_char(iter));
		return FALSE;
	}
	return gtk_text_iter_ends_word(iter);
}

static gboolean bf_gtk_text_iter_inside_word(GtkTextIter * iter, const gchar *smartselectionchars)
{
	if (bf_text_iter_char_in_string(iter, smartselectionchars)) {
		DBG_MSG("inside: iter has %c, return TRUE\n", gtk_text_iter_get_char(iter));
		return TRUE;
	}
	return gtk_text_iter_inside_word(iter);
}

static gboolean
bluefish_text_view_extend_selection(GtkTextView * widget, GtkTextExtendSelection granularity,
									const GtkTextIter * location, GtkTextIter * start, GtkTextIter * end)
{
	if (granularity != GTK_TEXT_EXTEND_SELECTION_WORD) {
		/* ignore line or character selection */
		return GDK_EVENT_PROPAGATE;
	}
	BluefishTextView *btv = BLUEFISH_TEXT_VIEW(widget);
	BluefishTextView *master = BLUEFISH_TEXT_VIEW(btv->master);
	gchar *smartselectionchars;
	DBG_MSG("\n\nbluefish_text_view_extend_selection, started, location at %d\n",
			gtk_text_iter_get_offset(location));
	smartselectionchars = master->bflang ? master->bflang->smartselectionchars:NULL;

	*start = *end = *location;
	if (bf_gtk_text_iter_inside_word(start, smartselectionchars)) {
		DBG_MSG("inside word\n");
		if (!bf_gtk_text_iter_starts_word(start, smartselectionchars))
			bf_gtk_text_iter_backward_visible_word_start(start, smartselectionchars);
		DBG_MSG("bluefish_text_view_extend_selection, started, start is at %d\n",gtk_text_iter_get_offset(start));
		if (!bf_gtk_text_iter_ends_word(end, smartselectionchars)) {
			if (!bf_gtk_text_iter_forward_visible_word_end(end, smartselectionchars))
				gtk_text_iter_forward_to_end(end);
			DBG_MSG("bluefish_text_view_extend_selection, started, end is at %d\n",gtk_text_iter_get_offset(end));
		}
	} else {
		GtkTextIter tmp;
		DBG_MSG("not inside word\n");
		tmp = *start;
		if (bf_gtk_text_iter_backward_visible_word_start(&tmp, smartselectionchars))
			bf_gtk_text_iter_forward_visible_word_end(&tmp, smartselectionchars);

		if (gtk_text_iter_get_line(&tmp) == gtk_text_iter_get_line(start))
			*start = tmp;
		else
			gtk_text_iter_set_line_offset(start, 0);

		tmp = *end;
		if (!bf_gtk_text_iter_forward_visible_word_end(&tmp, smartselectionchars))
			gtk_text_iter_forward_to_end(&tmp);

		if (gtk_text_iter_ends_word(&tmp))
			bf_gtk_text_iter_backward_visible_word_start(&tmp, smartselectionchars);

		if (gtk_text_iter_get_line(&tmp) == gtk_text_iter_get_line(end))
			*end = tmp;
		else
			gtk_text_iter_forward_to_line_end(end);
	}
	/*g_print("bluefish_text_view_extend_selection, started, location=%d, start=%d,end=%d\n",
			gtk_text_iter_get_offset(location), gtk_text_iter_get_offset(start),
			gtk_text_iter_get_offset(end));*/
	return GDK_EVENT_STOP;
}
#endif /*GTK_CHECK_VERSION(3,16,0) for expand-selection signal */

static void bluefish_text_view_finalize(GObject * object)
{
	BluefishTextView *btv;

	g_return_if_fail(object != NULL);
	btv = BLUEFISH_TEXT_VIEW(object);
	DEBUG_MSG("bluefish_text_view_finalize, destroy BluefishTextView btv=%p, btv->slave=%p, btv->master=%p\n",
			  btv, btv->slave, btv->master);
	if (btv->master != btv && BLUEFISH_TEXT_VIEW(btv->master)->slave == btv) {
		BLUEFISH_TEXT_VIEW(btv->master)->slave = NULL;
	}

	if (btv->scanner_delayed) {
		g_source_remove(btv->scanner_delayed);
		btv->scanner_delayed = 0;
	}
	if (btv->scanner_idle) {
		g_source_remove(btv->scanner_idle);
		btv->scanner_idle = 0;
	}
	if (btv->user_idle) {
		g_source_remove(btv->user_idle);
		btv->user_idle = 0;
	}
	if (btv->mark_set_idle) {
		g_source_remove(btv->mark_set_idle);
		btv->mark_set_idle = 0;
	}
	if (btv->autocomp) {
		autocomp_stop(btv);
	}
	if (btv->scancache.foundcaches) {
		scancache_destroy(btv);
	}
	if (btv->user_idle_timer) {
		g_timer_destroy(btv->user_idle_timer);
		btv->user_idle_timer = NULL;
	}
	if (btv->buffer) {
		DEBUG_MSG("bluefish_text_view_finalize %p, disconnect signals from buffer %p\n", btv, btv->buffer);
		if (btv->insert_text_id)
			g_signal_handler_disconnect(btv->buffer, btv->insert_text_id);
		if (btv->insert_text_after_id)
			g_signal_handler_disconnect(btv->buffer, btv->insert_text_after_id);
		if (btv->mark_set_id)
			g_signal_handler_disconnect(btv->buffer, btv->mark_set_id);
		if (btv->delete_range_id)
			g_signal_handler_disconnect(btv->buffer, btv->delete_range_id);
		if (btv->delete_range_after_id)
			g_signal_handler_disconnect(btv->buffer, btv->delete_range_after_id);
	}
	btv->bflang = NULL;
	btv->enable_scanner = FALSE;
	if (G_OBJECT_CLASS(bluefish_text_view_parent_class)->finalize) {
		DEBUG_MSG("call parent class finalize() on %p\n", object);
		G_OBJECT_CLASS(bluefish_text_view_parent_class)->finalize(object);
	}
}

/* *************************************************************** */
/* widget stuff below */
/* *************************************************************** */
/*
static void bluefish_text_view_finalize(GObject * object)
{
	G_OBJECT_CLASS(bluefish_text_view_parent_class)->finalize(object);
}*/
/*
static GObject *bluefish_text_view_create(GType type, guint n_construct_properties,
										  GObjectConstructParam * construct_properties)
{
	BluefishTextViewClass *klass =
		BLUEFISH_TEXT_VIEW_CLASS(g_type_class_peek(BLUEFISH_TYPE_TEXT_VIEW));
	GObjectClass *parent_class = G_OBJECT_CLASS(g_type_class_peek_parent(klass));
	GObject *obj = parent_class->constructor(type,
											 n_construct_properties,
											 construct_properties);

	/ * This constructor is not needed right now * /

	return (obj);
}*/

static void bluefish_text_view_class_init(BluefishTextViewClass * klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
#if GTK_CHECK_VERSION(3,14,0)
	GtkTextViewClass *textview_class = GTK_TEXT_VIEW_CLASS(klass);
#endif
#if GTK_CHECK_VERSION(3,20,0)
	gtk_widget_class_set_css_name(widget_class, "bluefishtextview");
#endif
/*	object_class->constructor = bluefish_text_view_create;*/
	object_class->finalize = bluefish_text_view_finalize;

	widget_class->button_press_event = bluefish_text_view_button_press_event;
	widget_class->motion_notify_event = bluefish_text_view_motion_notify_event;
	widget_class->button_release_event = bluefish_text_view_button_release_event;
#if GTK_CHECK_VERSION(3,0,0)
	widget_class->draw = bluefish_text_view_draw;
#else
	widget_class->expose_event = bluefish_text_view_expose_event;
#endif							/* gtk3 */
	widget_class->key_press_event = bluefish_text_view_key_press_event;
	widget_class->key_release_event = bluefish_text_view_key_release_event;
	widget_class->query_tooltip = bluefish_text_view_query_tooltip;
	widget_class->focus_out_event = bluefish_text_view_focus_out_event;
#if GTK_CHECK_VERSION(3,14,0)
	textview_class->draw_layer = bluefish_text_view_draw_layer;
#endif
#if GTK_CHECK_VERSION(3,16,0)
	textview_class->extend_selection = bluefish_text_view_extend_selection;
#endif

}

void
bluefish_text_view_multiset(BluefishTextView * btv, gpointer doc, gint view_line_numbers,
							gint view_blocks, gint autoindent, gint autocomplete, gint show_mbhl,
							gint enable_scanner)
{
	BLUEFISH_TEXT_VIEW(btv->master)->doc = doc;
	BLUEFISH_TEXT_VIEW(btv->master)->show_line_numbers = view_line_numbers;
	BLUEFISH_TEXT_VIEW(btv->master)->show_blocks = view_blocks;
	BLUEFISH_TEXT_VIEW(btv->master)->auto_indent = autoindent;
	BLUEFISH_TEXT_VIEW(btv->master)->auto_complete = autocomplete;
	BLUEFISH_TEXT_VIEW(btv->master)->show_mbhl = show_mbhl;
	BLUEFISH_TEXT_VIEW(btv->master)->enable_scanner = enable_scanner;
}

static void bluefish_text_view_init(BluefishTextView * textview)
{
	GtkTextTagTable *ttt;
/*	PangoFontDescription *font_desc;*/
	textview->user_idle_timer = g_timer_new();
	textview->scancache.foundcaches = g_sequence_new(NULL);
	textview->scancache.offsetupdates = NULL;
	bluefish_text_view_set_colors(textview, main_v->props.btv_color_str);
	textview->showsymbols = FALSE;
	textview->button_press_line = -1;
	ttt = langmgr_get_tagtable();
	textview->blockmatch = gtk_text_tag_table_lookup(ttt, "blockmatch");
	textview->cursortag = gtk_text_tag_table_lookup(ttt, "cursor_highlight");
	textview->enable_scanner = FALSE;
	/*font_desc = pango_font_description_from_string("Monospace 10");
	   gtk_widget_modify_font(GTK_WIDGET(textview), font_desc);
	   pango_font_description_free(font_desc); */
}

GtkWidget *bftextview2_new(void)
{
	BluefishTextView *textview = g_object_new(BLUEFISH_TYPE_TEXT_VIEW,
											  "has-tooltip", TRUE, NULL);

	g_return_val_if_fail(textview != NULL, NULL);
	textview->master = textview;
	textview->slave = NULL;
	return GTK_WIDGET(textview);
}

GtkWidget *bftextview2_new_with_buffer(GtkTextBuffer * buffer)
{
	BluefishTextView *textview = (BluefishTextView *) bftextview2_new();

	g_return_val_if_fail(textview != NULL, NULL);
	DBG_MSG("bftextview2_new_with_buffer, textview=%p, buffer=%p\n", textview, buffer);
	gtk_text_view_set_buffer(GTK_TEXT_VIEW(textview), buffer);
	textview->buffer = buffer;
	textview->insert_text_id =
		g_signal_connect(G_OBJECT(buffer), "insert-text", G_CALLBACK(bftextview2_insert_text_lcb), textview);
	textview->insert_text_after_id =
		g_signal_connect_after(G_OBJECT(buffer), "insert-text", G_CALLBACK(bftextview2_insert_text_after_lcb),
							   textview);
	textview->mark_set_id =
		g_signal_connect_after(G_OBJECT(buffer), "mark-set", G_CALLBACK(bftextview2_mark_set_lcb), textview);
	textview->delete_range_id =
		g_signal_connect(G_OBJECT(buffer), "delete-range", G_CALLBACK(bftextview2_delete_range_lcb),
						 textview);
	textview->delete_range_after_id =
		g_signal_connect_after(G_OBJECT(buffer), "delete-range",
							   G_CALLBACK(bftextview2_delete_range_after_lcb), textview);
	return GTK_WIDGET(textview);
}

GtkWidget *bftextview2_new_slave(BluefishTextView * master)
{
	BluefishTextView *textview;
	GtkTextBuffer *buffer;

	g_return_val_if_fail(master != NULL, NULL);
	buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(master));
	DEBUG_MSG("bftextview2_new_slave, master=%p, buffer=%p\n", master, buffer);
	textview = (BluefishTextView *) bftextview2_new_with_buffer(buffer);
	g_return_val_if_fail(textview != NULL, NULL);

	textview->master = master;
	master->slave = textview;
	DEBUG_MSG("bftextview2_new_slave, created slave %p for master %p\n", textview, master);
	gtk_text_view_set_border_window_size(GTK_TEXT_VIEW(textview), GTK_TEXT_WINDOW_LEFT,
										 BLUEFISH_TEXT_VIEW(master)->margin_pixels_chars
										 + BLUEFISH_TEXT_VIEW(master)->margin_pixels_block
										 + BLUEFISH_TEXT_VIEW(master)->margin_pixels_symbol);
	return GTK_WIDGET(textview);
}
