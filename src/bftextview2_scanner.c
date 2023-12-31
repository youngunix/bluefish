/* Bluefish HTML Editor
 * bftextview2_scanner.c
 *
 * Copyright (C) 2008,2009,2010,2011,2012,2013,2014,2015,2017,2023 Olivier Sessink
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
/*#define DEVELOPMENT*/
/*#define HL_PROFILING*/
/*#define VALGRIND_PROFILING*/

/*#define DUMP_SCANCACHE*/
/*#define DUMP_SCANCACHE_UPDATE_OFFSET*/
/*#define DUMP_HIGHLIGHTING*/

#ifdef VALGRIND_PROFILING
#include <valgrind/callgrind.h>
#endif

#ifdef HL_PROFILING
#include <unistd.h>
#endif
/* for the design docs see bftextview2.h */
#include "bluefish.h"
#include "bf_lib.h"
#include "bftextview2_private.h"
#include "bftextview2_scanner.h"
#include "bftextview2_identifier.h"

#ifdef MARKREGION
#include "bftextview2_markregion.h"
#endif

/*#undef DBG_SCANNING
#define DBG_SCANNING g_print*/
/*#undef DBG_SCANCACHE
#define DBG_SCANCACHE g_print*/

/* use
G_SLICE=always-malloc G_DEBUG=gc-friendly valgrind --tool=memcheck --num-callers=32 src/bluefish
to memory-debug this code

for memory leak debugging:
G_SLICE=always-malloc G_DEBUG=gc-friendly valgrind --tool=memcheck --leak-check=full --num-callers=32 src/bluefish

if you have the suppressions file from http://www.gnome.org/~johan/gtk.suppression

G_SLICE=always-malloc G_DEBUG=gc-friendly,resident-modules valgrind --tool=memcheck --leak-check=full --leak-resolution=high --num-callers=40 --track-origins=yes --suppressions=~/gtk.suppression src/bluefish

*/
#ifdef VALGRIND_PROFILING
#define MAX_CONTINUOUS_SCANNING_INTERVAL 1.0	/* float in seconds */
#else
#define MAX_CONTINUOUS_SCANNING_INTERVAL 0.1	/* float in seconds */
#endif

#define NUM_TIMER_CHECKS_PER_RUN 10

#ifdef DEVELOPMENT
static void scancache_check_integrity(BluefishTextView * btv, GTimer *timer);
#endif

typedef struct {
	GtkTextIter start;
	GtkTextIter end;
	guint16 patternum;
} Tmatch;

typedef struct {
	Tfoundcontext *curfcontext;
	Tfoundblock *curfblock;
	Tfound *nextfound;			/* items from the cache */
	GSequenceIter *siter;		/* an iterator to get the next item from the cache */
	GQueue *indentstack;
	GTimer *timer;
	GtkTextIter start;			/* start of area to scan */
	GtkTextIter end;			/* end of area to scan */
	gint16 context;
	guint8 identmode;
	guint8 identaction;
#ifdef HL_PROFILING
	guint startpos;
	gdouble stage1;
	gdouble stage2;
	gdouble stage3;
	gdouble stage4;
#endif
} Tscanning;

#ifdef HL_PROFILING
typedef struct {
	gint longest_contextstack;
	gint longest_blockstack;
	gint numcontextstart;
	gint numcontextend;
	gint numblockstart;
	gint numblockend;
	gint numchars;
	gint numloops;
	gint fblock_refcount;
	gint fcontext_refcount;
	gint found_refcount;
	guint total_runs;
	guint total_chars;
	guint total_time_ms;
} Thl_profiling;

Thl_profiling hl_profiling = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, };
GTimer *totalruntimer = NULL;
#endif

guint loops_per_timer = 1000;	/* a tunable to avoid asking time too often. this is auto-tuned. */

#ifdef DEVELOPMENT
void
dump_scancache(BluefishTextView * btv)
{
	const gint startoutput_o = 0;
	const gint endoutput_o = G_MAXINT32;
	/*const gint startoutput_o = 6500;
	const gint endoutput_o = 16500;*/
	GSequenceIter *siter = g_sequence_get_begin_iter(btv->scancache.foundcaches);
	g_print("\nDUMP SCANCACHE\nwith length %d, document length=%d. Dumping region %d:%d\n\n",
			g_sequence_get_length(btv->scancache.foundcaches), gtk_text_buffer_get_char_count(btv->buffer), startoutput_o, endoutput_o);
	while (siter && !g_sequence_iter_is_end(siter)) {
		Tfound *found = g_sequence_get(siter);
		if (!found)
			break;
		if (found->charoffset_o >= startoutput_o && found->charoffset_o <= endoutput_o) {
			g_print("%3d: %p, fblock %p, fcontext %p, siter %p\n", found->charoffset_o, found, found->fblock,
					found->fcontext, siter);
			if (found->numcontextchange != 0) {
				g_print("\tnumcontextchange=%d", found->numcontextchange);
				if (found->fcontext) {
					g_print(",context %d", found->fcontext->context);
					g_print(", highlight %s, parent=%p, %d:%d",
							g_array_index(btv->bflang->st->contexts, Tcontext,
										  found->fcontext->context).contexthighlight,
							found->fcontext->parentfcontext, found->fcontext->start_o, found->fcontext->end_o);
				}
				g_print("\n");
			}
			if (found->numblockchange != 0) {
				g_print("\tnumblockchange=%d", found->numblockchange);
				if (found->fblock) {
					g_print(", pattern %d ", found->fblock->patternum);
					if (g_array_index(btv->bflang->st->matches, Tpattern,
																	found->fblock->patternum).is_regex) {
						GtkTextIter it1, it2;
						gchar *tmp2;
						if (found->numblockchange > 0) {
							gtk_text_buffer_get_iter_at_offset(btv->buffer, &it1, found->fblock->start1_o);
							gtk_text_buffer_get_iter_at_offset(btv->buffer, &it2, found->fblock->end1_o);
							tmp2 = gtk_text_buffer_get_text(btv->buffer, &it1, &it2, TRUE);
						} else if (found->fblock->start2_o != -1) {
							gtk_text_buffer_get_iter_at_offset(btv->buffer, &it1, found->fblock->start2_o);
							gtk_text_buffer_get_iter_at_offset(btv->buffer, &it2, found->fblock->end2_o);
							tmp2 = gtk_text_buffer_get_text(btv->buffer, &it1, &it2, TRUE);
						} else {
							tmp2 = g_strdup(g_array_index(btv->bflang->st->matches, Tpattern,
																	found->fblock->patternum).pattern);
						}
						g_print("%s", tmp2);
						g_free(tmp2);
					} else {
						g_print("%s", g_array_index(btv->bflang->st->matches, Tpattern,
																	found->fblock->patternum).pattern);
					}
					g_print(", parent=%p, %d:%d-%d:%d",
							found->fblock->parentfblock, found->fblock->start1_o, found->fblock->end1_o,
							found->fblock->start2_o, found->fblock->end2_o);
				} else {
					g_print("BUT NO FBLOCK ???");
				}
				g_print("\n");
			}
			if (found->indentlevel != NO_INDENT_FOUND) {
				g_print("\tindentlevel=%d\n",found->indentlevel);
			}
		}
		siter = g_sequence_iter_next(siter);
	}
	g_print("END OF DUMP\n\n");
}
#endif							/* DUMP_SCANCACHE */

#ifdef DUMP_HIGHLIGHTING
void
dump_highlighting(BluefishTextView * btv)
{
	GtkTextIter it;
	gboolean cont=TRUE;
	gtk_text_buffer_get_iter_at_offset(btv->buffer,&it,0);
	g_print("START OF HIGHLIGHTING DUMP\n");
	while (cont) {
		if (gtk_text_iter_toggles_tag(&it, NULL)) {
			GSList *lst, *tmplst;
			g_print("pos %d has tags ", gtk_text_iter_get_offset(&it));
			tmplst = lst = gtk_text_iter_get_tags(&it);
			while (tmplst) {
				gchar *name=NULL;
				g_object_get(tmplst->data, "name", &name, NULL);
				g_print("%p(%s) ",tmplst->data, name);
				tmplst=tmplst->next;
			}
			g_slist_free(lst);
			g_print("\n");
		}/* else {
			g_print("pos %d\n", gtk_text_iter_get_offset(&it));
		}*/
		cont = gtk_text_iter_forward_char(&it);
	}
	g_print("END OF DUMP\n\n");
}
#endif

Tfound *
get_foundcache_next(BluefishTextView * btv, GSequenceIter ** siter)
{
	DBG_MSG("get_foundcache_next, *siter=%p\n", *siter);
	*siter = g_sequence_iter_next(*siter);
	if G_LIKELY((*siter && !g_sequence_iter_is_end(*siter))) {
		DBG_MSG("get_foundcache_next, next *siter=%p, return found %p\n", *siter, g_sequence_get(*siter));
		return g_sequence_get(*siter);
	}
	DBG_MSG("get_foundcache_next, *siter=%p is end, return NULL\n", *siter);
	return NULL;
}

Tfound *
get_foundcache_first(BluefishTextView * btv, GSequenceIter ** retsiter)
{
	*retsiter = g_sequence_get_begin_iter(btv->scancache.foundcaches);
	if (*retsiter && !g_sequence_iter_is_end(*retsiter)) {
		return g_sequence_get(*retsiter);
	}
	DBG_MSG("get_foundcache_first, return NULL\n");
	return NULL;
}

static gint
foundcache_compare_charoffset_o(gconstpointer a, gconstpointer b, gpointer user_data)
{
	return ((Tfound *) a)->charoffset_o - ((Tfound *) b)->charoffset_o;
}

Tfound *
get_foundcache_at_offset(BluefishTextView * btv, guint offset, GSequenceIter ** retsiter)
{
	GSequenceIter *siter;
	Tfound fakefound;
	Tfound *found = NULL;

	g_assert(btv == btv->master);

	fakefound.charoffset_o = offset;
	siter = g_sequence_search(btv->scancache.foundcaches, &fakefound, foundcache_compare_charoffset_o, NULL);
	if (G_LIKELY(!g_sequence_iter_is_begin(siter))) {
		/* now get the previous position, and get the stack at that position */
		DBG_SCANCACHE("search for offset %d returned iter-position %d (cache length %d)\n", offset,
					  g_sequence_iter_get_position(siter), g_sequence_get_length(btv->scancache.foundcaches));
		siter = g_sequence_iter_prev(siter);
		if (siter && !g_sequence_iter_is_end(siter)) {
			found = g_sequence_get(siter);
			if (retsiter)
				*retsiter = siter;
			DBG_SCANCACHE("found nearest stack %p with charoffset_o %d\n", found, found->charoffset_o);
		} else {
			DBG_SCANCACHE("no siter no cache\n");
		}
	} else if (!g_sequence_iter_is_end(siter)) {
		DBG_SCANCACHE("got begin siter\n");
		found = g_sequence_get(siter);	/* get the first found */
		if (retsiter)
			*retsiter = siter;
	}
	return found;
}

void found_free_lcb(gpointer data, gpointer btv);

static gboolean
is_fblock_on_stack(Tfoundblock * topfblock, Tfoundblock * searchfblock)
{
	Tfoundblock *fblock = topfblock;
	while (fblock) {
		if (fblock == searchfblock)
			return TRUE;
		fblock = (Tfoundblock *) fblock->parentfblock;
	}
	return FALSE;
}

static gboolean
is_fcontext_on_stack(Tfoundcontext * topfcontext, Tfoundcontext * searchfcontext)
{
	Tfoundcontext *fcontext = topfcontext;
	while (fcontext) {
		if (fcontext == searchfcontext)
			return TRUE;
		fcontext = (Tfoundcontext *) fcontext->parentfcontext;
	}
	return FALSE;
}

static guint
remove_cache_entry(BluefishTextView * btv, Tfound ** found, GSequenceIter ** siter, Tfoundblock *curblockstack, Tfoundcontext *curcontextstack)
{
	Tfound *tmpfound1 = *found;
	GSequenceIter *tmpsiter1 = *siter;
	guint invalidoffset;
	gint blockstackcount = 0, contextstackcount = 0;

	if (!tmpfound1)
		return 0;

	*found = get_foundcache_next(btv, siter);
	DBG_SCANCACHE("remove_cache_entry, STARTED, remove %p at offset %d and any children, numblockchange=%d, numcontextchange=%d\n", tmpfound1,
				  tmpfound1->charoffset_o, tmpfound1->numblockchange, tmpfound1->numcontextchange);
	invalidoffset = tmpfound1->charoffset_o;
	/* if this entry pops blocks or contexts, mark the ends of those as undefined */
	if (tmpfound1->numblockchange < 0) {
		Tfoundblock *tmpfblock = tmpfound1->fblock;
		DBG_SCANCACHE("remove_cache_entry, found %p pops blocks, mark end of %d fblock's as undefined, fblock=%p\n",
					  tmpfound1, tmpfound1->numblockchange, tmpfound1->fblock);
		blockstackcount = tmpfound1->numblockchange;
		while (tmpfblock && blockstackcount < 0) {
			if (!curblockstack || is_fblock_on_stack(curblockstack, tmpfblock)) {
				DBG_SCANCACHE("remove_cache_entry, mark end of fblock %p as undefined\n", tmpfblock);
				tmpfblock->start2_o = BF_POSITION_UNDEFINED;
				tmpfblock->end2_o = BF_POSITION_UNDEFINED;
			}
			tmpfblock = tmpfblock->parentfblock;
			blockstackcount++;
		}
	}
	if (tmpfound1->numcontextchange < 0) {
		Tfoundcontext *tmpfcontext = tmpfound1->fcontext;
		DBG_SCANCACHE("remove_cache_entry, found %p pops contexts, mark end of %d fcontext's as undefined, fcontext=%p\n",
					  tmpfound1, tmpfound1->numcontextchange, tmpfound1->fcontext);
		contextstackcount = tmpfound1->numcontextchange;
		while (tmpfcontext && contextstackcount < 0) {
			if (!curcontextstack || is_fcontext_on_stack(curcontextstack, tmpfcontext)) {
				DBG_SCANCACHE("remove_cache_entry, mark end of fcontext %p as undefined\n", tmpfcontext);
				tmpfcontext->end_o = BF_POSITION_UNDEFINED;
			}
			tmpfcontext = tmpfcontext->parentfcontext;
			contextstackcount++;
		}
	}

	blockstackcount = MAX(0, tmpfound1->numblockchange);
	contextstackcount = MAX(0, tmpfound1->numcontextchange);
	/* 	if this found has pushed a block or a context on the stack, we will delete all further
	blocks until we find the block that pops this block or context again */
	DBG_SCANCACHE
		("remove_cache_entry, remove in loop all children of found %p (numblockchange=%d,numcontextchange=%d) with offset %d, next found in cache=%p\n",
		 tmpfound1, tmpfound1->numblockchange, tmpfound1->numcontextchange, tmpfound1->charoffset_o, *found);
	while (*found && (blockstackcount > 0 || contextstackcount > 0)) {
		Tfound *tmpfound2 = *found;
		GSequenceIter *tmpsiter2 = *siter;
		*found = get_foundcache_next(btv, siter);
		blockstackcount += tmpfound2->numblockchange;
		contextstackcount += tmpfound2->numcontextchange;

		DBG_SCANCACHE
			("in loop: remove Tfound %p with offset %d, fcontext=%p, numcontextchange=%d, fblock=%p, numblockchange=%d, from the cache and free, contextstackcount=%d, blockstackcount=%d, nextfound=%p\n",
			 tmpfound2, tmpfound2->charoffset_o, tmpfound2->fcontext, tmpfound2->numcontextchange, tmpfound2->fblock, tmpfound2->numblockchange, contextstackcount, blockstackcount, *found);
		if (tmpfound2->numblockchange < 0 && blockstackcount < 0) {
			Tfoundblock *tmpfblock;
			/* a blockstackcount < 0 probably means that this found pops a
			block that started before the found that we started to remove
			in this function, so let's set the end to undefined for these blocks.
			
			Since (tmpfound2->numblockchange < 0) the memory in tmpfound2->fblock is likely to be free'ed already
			earlier in this loop. So we must look at the blocks pointed to by tmpfound1->fblock.  
			*/
			tmpfblock = tmpfound1->fblock;
			while (tmpfblock && tmpfblock->end2_o <= tmpfound2->charoffset_o) {
				tmpfblock->start2_o = BF_POSITION_UNDEFINED;
				tmpfblock->end2_o = BF_POSITION_UNDEFINED;
				tmpfblock = tmpfblock->parentfblock;
			}
		}

		invalidoffset = tmpfound2->charoffset_o;
		g_sequence_remove(tmpsiter2);
		found_free_lcb(tmpfound2, btv);
	}

	DBG_SCANCACHE("remove_cache_entry, finally remove found %p itself with offset %d and return invalidoffset %d\n", tmpfound1,
				  tmpfound1->charoffset_o, invalidoffset);

	g_sequence_remove(tmpsiter1);
	found_free_lcb(tmpfound1, btv);
	return invalidoffset;
}

static void
mark_needscanning(BluefishTextView * btv, gint startpos, gint endpos)
{
	GtkTextIter it1, it2;
	gint bufferendpos;
	/* if text is deleted, mark_needscanning could be called for a Tfound that is in the deleted region.
	Sometimes after applying the offset to a deleted Tfound, the final region is negative. Correct for that
	situation */
	/*DBG_SCANCACHE("mark_needscanning, called for %d:%d\n",startpos,endpos);*/
	if (startpos < 0) startpos = 0;
	if (endpos <= 0) return;
	/*DBG_SCANCACHE("mark_needscanning, marking %d:%d\n",startpos,endpos);*/
#ifdef MARKREGION
	gtk_text_buffer_get_end_iter(btv->buffer, &it2);
	bufferendpos = gtk_text_iter_get_offset(&it2);
	DBG_MARKREGION("mark_needscanning, markregion_needscanning() was called with %d:%d,  buffer end at %u\n",
				startpos,endpos,bufferendpos);
	if (endpos == -1 || endpos > bufferendpos) {
		endpos = bufferendpos;
		/* it is possible that mark_needscanning() is called with positions beyond the end of the buffer,
		if for example a block is removed that ended near the end and the deleted region is large. Check
		for that and return in this situation */
		if (endpos <= startpos) {
			DBG_MARKREGION("mark_needscanning, endpos=%d <= startpos=%d, return\n",endpos,startpos);
			return;
		}
	}
	markregion_nochange(&btv->scanning, startpos, endpos);
#endif
}

typedef struct {
	BF_ELIST_HEAD;
	guint32 prevpos; /* the last location where the accumulated offset was changed */
	gint32 prevoffset; /* the accumulated offset from all previous processed offset updates */
	GSequenceIter *siter; /* the last position in the scancache that had it's offset updated */
	Tfound *found; /* the Tfound at that same position in the scancache */
} Tscancache_offset_update;

/*
 * scancache_update_single_offset updates the offset in the scancache, it (is|might be) called
 * multiple times if there have been multiple changes to the offsets (multiple inserts or deletes)
 * it is called from foundcache_process_offsetupdates() which loops over all changes that can be
 * combined:
 *
 * if it will be called a second time, nextpos will be set to indicate the position where
 * the offset will change. However (!!) nextpos is defined as the position *after* the current offset
 * has been applied. So you cannot simply compare with charoffset_o, you have to compare with 
 * charoffset_o+prevoffset+offset !!
 *
 * This function may thus return
 * for a found > nextpos because the it will be called again for the next new offset.
 *
 * when it is called again for a second change, for any Tfound with charoffset_o > sou->prevpos the new
 * offset will be offset + prevoffset;
 *
 * if there was a replace (a delete and insert on the same location) the deleteoffset indicates the size that was 
 * deleted and offset indicated the combined offset of the delete and insert. The deleteoffset is used to determine
 * which part of the cache is nowinvalid and should be removed.
 */
static void
scancache_update_single_offset(BluefishTextView * btv, Tscancache_offset_update *sou, guint32 startpos, gint32 offset, gint32 deleteoffset, guint32 nextpos)
{
	gint comparepos;
	gint handleoffset=sou->prevoffset;
		/* handleoffset is the offset that needs to be applied. Before prevpos it equals prevoffset, beyond that it equals offset+prevoffset */
	Tfound *nextfound;
	GSequenceIter *nextsiter;

	g_assert(btv == btv->master);

	comparepos = (offset < 0) ? startpos - offset : startpos;
	DBG_SCANCACHE
		("scancache_update_single_offset, update with offset %d starting at startpos %u, cache length=%u, comparepos=%d, prevpos=%u, prevoffset=%d, nextpos=%u\n",
		 offset, startpos, g_sequence_get_length(btv->scancache.foundcaches), comparepos, sou->prevpos, sou->prevoffset, nextpos);

	/* the first thing to do is to initialize sou->found with the next Tfound that we have to change.

	if this is the first call to scancache_update_single_offset() we will simply get the Tfound at offset

	if it is a later call, we can use sou->found (which is always beyond the last position that was handled), but we have to
	check if sou->found is beyond nextpos, in which case it also needs to be updated for the next change in the scancache, handled
	by the next call to this function */

	if (sou->found == NULL) {
		DBG_SCANCACHE("scancache_update_single_offset, no found, get new found for startpos %u\n",startpos);
		sou->found = get_foundcache_at_offset(btv, startpos, &sou->siter);
	} else {
		DBG_SCANCACHE("scancache_update_single_offset, continue with found %p at charpos %u\n",sou->found, sou->found->charoffset_o);
	}

#ifdef DEVELOPMENT
	if (sou->found)
		DBG_SCANCACHE("scancache_update_single_offset, got found %p with offset %u\n", sou->found, sou->found->charoffset_o);
	else
		DBG_SCANCACHE("scancache_update_single_offset, got NULL found (%p)\n", sou->found);
#endif

	if (!sou->found)
		return;

	if (sou->found->charoffset_o > startpos && ((gint)sou->found->charoffset_o+(gint)offset) >= ((gint)nextpos)) {
		DBG_SCANCACHE("current found at %d is bigger then startpos(%u) and bigger then nextpos(%u), return\n",sou->found->charoffset_o,startpos,nextpos);
		sou->prevoffset += offset;
		return;
	}

	/*
	We have sou->found now set.
	either found is returned by get_foundcache_at_offset() and prevpos and prevoffset are 0
	or found is defined in the previous call to scancache_update_single_offset()
			if it was defined, and it has a position before startpos, this position should equal prevpos
						and prevoffset is included already
			if it was defined, and it has a position beyond startpos, that essentially means it is the
						first found in the cache, so prevpos should be still at 0

	first update all block ends and context ends that are on the stack already before the position where the offset changes
	if there is a found before the offset changes it should have an updated prevpos already, and prevpos should equal
	this location.*/
#ifdef DEVELOPMENT
	if (sou->found && sou->prevpos!=0 && sou->found->charoffset_o != sou->prevpos) {
		g_print("ABORT: we have a found(%u) before startpos(%u), but the found is not at prevpos(%u)\n",
											sou->found->charoffset_o, startpos, sou->prevpos);
		g_assert_not_reached();
	}
#endif
	if (sou->found && sou->found->charoffset_o <= startpos) {
		Tfoundcontext *tmpfcontext;
		Tfoundblock *tmpfblock;
		DBG_SCANCACHE
			("scancache_update_single_offset, handle first found %p with offset %u, complete stack fcontext %p fblock %p\n",
			 sou->found, sou->found->charoffset_o, sou->found->fcontext, sou->found->fblock);
		/* for the first found (if we have one) we have to update the end-offsets for all contexts/blocks on the stack.
		These are pushed on the stack at an earlier found already, and are _thus_ corrected for sou->prevpos
		*/
		tmpfcontext = sou->found->fcontext;
		while (tmpfcontext) {
			DBG_SCANCACHE("scancache_update_single_offset, fcontext on stack=%p, start_o=%u end_o=%u\n",
						  tmpfcontext, tmpfcontext->start_o, tmpfcontext->end_o);
			if (tmpfcontext->end_o > startpos && tmpfcontext->end_o != BF_OFFSET_UNDEFINED) {
				DBG_SCANCACHE("scancache_update_single_offset, update fcontext %p end from %u to %u\n",
							  tmpfcontext, tmpfcontext->end_o, tmpfcontext->end_o + offset);
				tmpfcontext->end_o += offset;
			}
			tmpfcontext = (Tfoundcontext *) tmpfcontext->parentfcontext;
		}

		tmpfblock = sou->found->fblock;
		while (tmpfblock) {
			DBG_SCANCACHE("scancache_update_single_offset, fblock on stack=%p, %u:%u-%u:%u\n", tmpfblock,
						  tmpfblock->start1_o, tmpfblock->end1_o,tmpfblock->start2_o, tmpfblock->end2_o);
			/* there is a special situation for a block: it might be a tretched block, in which
			case end1_o possibly needs updating too */
			DBG_SCANCACHE("comparepos=%d, end1_o=%d for pattern %d (%s)\n",comparepos,tmpfblock->end1_o,tmpfblock->patternum,g_array_index(btv->bflang->st->matches, Tpattern, tmpfblock->patternum).pattern);
			if (G_UNLIKELY(tmpfblock->end1_o >= comparepos && tmpfblock == sou->found->fblock && tmpfblock->end1_o > sou->found->charoffset_o)) {
				tmpfblock->end1_o += offset;
			}
			if (G_UNLIKELY(tmpfblock->start2_o != BF_POSITION_UNDEFINED)) {
				if (G_UNLIKELY(offset < 0 && tmpfblock->start2_o < comparepos && tmpfblock->end2_o >= comparepos)) {
					/* the end of the block might be within the deleted region, if so, set the end as undefined */
					DBG_SCANCACHE("update end of fblock %p %u:%u-%u:%u to UNDEFINED\n",tmpfblock
									, tmpfblock->start1_o, tmpfblock->end1_o, tmpfblock->start2_o, tmpfblock->end2_o );
					tmpfblock->start2_o = BF_POSITION_UNDEFINED;
					tmpfblock->end2_o = BF_POSITION_UNDEFINED;
				} else {
					if (tmpfblock->start2_o >= startpos) {
					/* notice the difference: the start needs >= startpos and the end needs > startpos */
						DBG_SCANCACHE
							("scancache_update_single_offset, update fblock %p from start2_o=%u to start2_o=%u\n",
						 	tmpfblock, tmpfblock->start2_o, tmpfblock->start2_o + offset);
						tmpfblock->start2_o += offset;
					}
					if (tmpfblock->end2_o > startpos) {
						DBG_SCANCACHE
							("scancache_update_single_offset, update fblock %p from end2_o=%u to end2_o=%u\n",
						 	tmpfblock, tmpfblock->end2_o, tmpfblock->end2_o + offset);
						tmpfblock->end2_o += offset;
					}
				}
			}
			tmpfblock = (Tfoundblock *) tmpfblock->parentfblock;
		}
	}

	/* if text was deleted, or text was replaced, we might need to remove a few entries from the cache before we start updating 
	the offsets. This is indicated by deleteoffset being smaller than zero, and is handled by the following loop: */
	if (deleteoffset < 0) {
		Tfound *tmpfound = sou->found;
		GSequenceIter *tmpsiter=sou->siter;

		/*
		in the situation that there was a Tfound before the deleted region, sou->found points to this found that does not need to be deleted
		BUT in the case that there was no found before the deleted region (for example when the deleted region starts at 0), the current found
		could be in the deleted region already!!!!
		*/
		if (sou->found->charoffset_o > startpos) {
			/* no need to go the next item, the current items needs to be deleted too */
			DBG_SCANCACHE("sou->found(%d) is beyond startpos(%d), set found and siter to NULL\n",(gint)sou->found->charoffset_o, (gint)startpos);
			sou->found = NULL;
			sou->siter = NULL;
		} else {
			DBG_SCANCACHE("before get_foundcache_next, tmpfound is at %u\n",tmpfound?tmpfound->charoffset_o:-1);
			tmpfound = get_foundcache_next(btv, &tmpsiter);
			DBG_SCANCACHE("after get_foundcache_next, tmpfound is at %u\n",tmpfound?tmpfound->charoffset_o:-1);
		}

#ifdef DEVELOPMENT
		if (tmpfound)
			DBG_SCANCACHE("tmpfound->charoffset_o=%d, startpos=%u\n",(gint)tmpfound->charoffset_o, (gint)startpos);

		/* we should have a tmpfound now that has offset > startpos and thus it should be beyond prevpos as well */
		if (tmpfound && ((gint)(tmpfound->charoffset_o+sou->prevoffset)) < startpos) {
			g_print("ABORT: tmpfound->charoffset_o(%u)+sou->prevoffset(%d)=%u < startpos(%u) (sou->prevpos=%u, deleteoffset=%d, nextpos=%d)\n",
								(gint)tmpfound->charoffset_o, (gint)sou->prevoffset,
								(gint)tmpfound->charoffset_o+sou->prevoffset,
								(gint)startpos, (gint)sou->prevpos, (gint)deleteoffset, (gint)nextpos);
			g_assert_not_reached();
		}
#endif

		/* loop over any items within the deleted region and remove them. because these
		Tfound offsets are not yet updated we don't compare with 'startpos-deleteoffset' but we
		compare with 'startpos-deleteoffset-sou->prevoffset' */
		DBG_SCANCACHE("before loop, tmpfound(%p) at position %d +deleteoffset+prevoffset=%d\n",tmpfound,tmpfound?tmpfound->charoffset_o:-1,tmpfound?tmpfound->charoffset_o+offset+sou->prevoffset:-1);
		while (tmpfound && ((gint)tmpfound->charoffset_o+deleteoffset+sou->prevoffset) <= ((gint)startpos)) {
			if (G_LIKELY(((gint)tmpfound->charoffset_o+sou->prevoffset) > ((gint)startpos))) {
				gint numblockchange = tmpfound->numblockchange;
				DBG_SCANCACHE("scancache_update_single_offset, remove found %p at %d from the cache\n",tmpfound, tmpfound->charoffset_o);
				if (numblockchange > 0 && tmpfound->fblock->end2_o+sou->prevoffset > startpos - deleteoffset) {
					/* we have to enlarge needscanning to the place where this was popped, unless the complete block was within the deleted region */
					DBG_SCANCACHE("scancache_update_single_offset, found pushed a block, mark obsolete block %u:%u as needscanning\n",tmpfound->fblock->start1_o, tmpfound->fblock->end2_o);
					mark_needscanning(btv
							, tmpfound->fblock->start1_o+sou->prevoffset<=startpos ? tmpfound->fblock->start1_o+sou->prevoffset : tmpfound->fblock->start1_o+sou->prevoffset+deleteoffset
							, tmpfound->fblock->end2_o==BF_OFFSET_UNDEFINED ? BF_OFFSET_UNDEFINED : tmpfound->fblock->end2_o+sou->prevoffset+deleteoffset);
				}
				if (tmpfound->numcontextchange > 0 && tmpfound->fcontext->end_o+sou->prevoffset > startpos - deleteoffset) {
					/* we have to enlarge needscanning to the place where this was popped, unless the complete context was within the deleted region */
					DBG_SCANCACHE("scancache_update_single_offset, found pushed a context, mark obsolete context %u:%u as needscanning\n",tmpfound->fcontext->start_o, tmpfound->fcontext->end_o);
					mark_needscanning(btv
							, tmpfound->fcontext->start_o+sou->prevoffset<=startpos ? tmpfound->fcontext->start_o+sou->prevoffset : tmpfound->fcontext->start_o+sou->prevoffset+deleteoffset
							, tmpfound->fcontext->end_o==BF_OFFSET_UNDEFINED ? BF_OFFSET_UNDEFINED : tmpfound->fcontext->end_o+sou->prevoffset+deleteoffset);
				}
				remove_cache_entry(btv, &tmpfound, &tmpsiter, NULL, NULL);
				if (!tmpfound && (numblockchange < 0)) {
					mark_needscanning(btv, startpos, BF_OFFSET_UNDEFINED);
					/* there is a special situation: if this is the last found in the cache, and it pops a block,
					   we have to enlarge the scanning region to the end of the text */
					DBG_SCANCACHE("scancache_update_single_offset, mark area from %u to end (-1) with needscanning\n",
								  startpos);
				}
			} else {
				tmpfound = get_foundcache_next(btv, &tmpsiter);
				DBG_SCANCACHE("in loop, tmpfound(%p) at position %d +deleteoffset+prevoffset=%d\n",tmpfound,tmpfound?tmpfound->charoffset_o:-1,tmpfound?tmpfound->charoffset_o+deleteoffset+sou->prevoffset:-1);
			}
		}
	}

	/* from this point on: see if we have to update offsets for any other founds, or if we are ready in the case that the found
	is beyond nextpos (in that case the next call to scancache_update_single_offset() will update their offsets) */
	if (!sou->found) {
		/* possibly text was deleted starting at offset 0, in that case all previous founds have been deleted, so get the first one */
		DBG_SCANCACHE("scancache_update_single_offset, after delete loop, sou->found==NULL, so get the first found\n");
		nextfound = get_foundcache_first(btv, &nextsiter);
	} else {
		nextsiter=sou->siter;
		nextfound = get_foundcache_next(btv, &nextsiter);
	}
	/* see if nextpos is at startpos, in which case it only needs correction with prevoffset */
	DBG_SCANCACHE("nextfound(%u)+prevoffset(%d)=%u, startpos=%u\n",nextfound?nextfound->charoffset_o:0,sou->prevoffset,nextfound?(nextfound->charoffset_o+sou->prevoffset):0,startpos);
	if (nextfound && ((gint)nextfound->charoffset_o + sou->prevoffset) == ((gint)startpos)) {
		DBG_SCANCACHE("nextfound(%u)+prevoffset(%d)=%u is at the startpos(%u) itself, so handleoffset=%d\n",
					nextfound->charoffset_o,
					sou->prevoffset,
					nextfound->charoffset_o+sou->prevoffset,
					startpos,
					handleoffset);
		/* nextfound is at the startpos itself, beyond here we need to correct with 'offset+prevoffset', but this one needs only correction with 'prevoffset' */
	} else {
		DBG_SCANCACHE("update handleoffset(%d)+offset(%d)=%d\n",handleoffset,offset,handleoffset+offset);
		handleoffset += offset;
	}

	DBG_SCANCACHE("found(%p) was at %d, nextfound(%d)+handleoffset(%d)=%d, nextpos=%u\n",sou->found
							, sou->found?sou->found->charoffset_o:-1
							, nextfound?nextfound->charoffset_o:-1
							, handleoffset
							, nextfound?nextfound->charoffset_o+handleoffset:-1
							, nextpos);
	/* if the nextfound is already beyond nextpos, we mark the current found as prevpos, set sou->prevoffset and return */
	if (!nextfound || (((gint)nextfound->charoffset_o) + handleoffset) >= ((gint)nextpos)) {
		if (sou->found) {
			sou->prevpos = sou->found->charoffset_o;
			DBG_SCANCACHE("nextfound(%d)+handleoffset(%d)=%d >= nextpos(%d), so we return at the current found(%d) and set prevpos to %d as well\n"
					,nextfound?nextfound->charoffset_o:-1
					, handleoffset
					, nextfound?nextfound->charoffset_o+handleoffset:-1
					,nextpos
					, sou->found->charoffset_o
					, sou->prevpos);
		} else {
			sou->prevpos = 0;
		}
		sou->prevoffset += offset;
		return;
	}
	sou->found = nextfound;
	sou->siter = nextsiter;

	/* if we reach this point, sou->found is NULL or points to a position before nextpos but beyond prevpos */
#ifdef DEVELOPMENT
	if (sou->found) {
		/*g_print("charoffset_o=%d, handleoffset=%d, prevoffset=%d, startpos=%d, prevpos=%d\n",sou->found->charoffset_o,handleoffset,sou->prevoffset, startpos, sou->prevpos);*/
		if (((gint)sou->found->charoffset_o+handleoffset) > sou->prevpos) {
			if (((gint)sou->found->charoffset_o + handleoffset) < startpos) {
				g_print("ABORT: scancache_update_single_offset, sou->found->charoffset_o(%u)+handleoffset(%d)=%d < startpos(%u) (prevpos=%u)\n",
							(gint)sou->found->charoffset_o, (gint)handleoffset,sou->found->charoffset_o + handleoffset, (gint)startpos, (gint)sou->prevpos);
				g_assert_not_reached();
			}
		} else {

			/*if (sou->found->charoffset_o < startpos) {*/
			g_print("ABORT: scancache_update_single_offset, sou->found(%p)->charoffset_o(%u)+sou->prevoffset(%d)+offset(%d)=%u <= prevpos(%u) (startpos=%u, prevoffset=%d)\n",
							sou->found,sou->found->charoffset_o,sou->prevoffset,offset,sou->found->charoffset_o+sou->prevoffset+offset, (gint)sou->prevpos, (gint)startpos, (gint)sou->prevoffset);
			g_assert_not_reached();
			/*}*/
		}
	}
#endif


	while (sou->found && ((gint)sou->found->charoffset_o+handleoffset) < ((gint)nextpos)) {
		Tfound *tmpfound = sou->found;
		GSequenceIter *tmpsiter=sou->siter;
		/* loop over all the remaining entries in the cache < nextpos
		for all further founds, we only handle the pushedblock and pushedcontext, and
		we update both start and end of the blocks and contexts */

		DBG_SCANCACHE
			("scancache_update_single_offset, about to update found %p with charoffset %u to %u, fcontext %p and fblock %p\n",
			 sou->found, sou->found->charoffset_o,
			 sou->found->charoffset_o + handleoffset,
			 sou->found->fcontext, sou->found->fblock);
		if (G_UNLIKELY(IS_FOUNDMODE_CONTEXTPUSH(sou->found))) {
			DBG_SCANCACHE("scancache_update_single_offset, contextpush %p update from %u:%u to %u:%u\n",
						  sou->found->fcontext, sou->found->fcontext->start_o, sou->found->fcontext->end_o,
						  sou->found->fcontext->start_o + handleoffset,
						  sou->found->fcontext->end_o + handleoffset);
			if (G_LIKELY(sou->found->fcontext->start_o != BF_OFFSET_UNDEFINED))
				sou->found->fcontext->start_o += handleoffset;
			if (G_LIKELY(sou->found->fcontext->end_o != BF_OFFSET_UNDEFINED))
				sou->found->fcontext->end_o += handleoffset;
		}
		if (G_UNLIKELY(IS_FOUNDMODE_BLOCKPUSH(sou->found))) {
			DBG_SCANCACHE("scancache_update_single_offset, blockpush %p update from %u:%u to %u:%u\n",
						  sou->found->fblock, sou->found->fblock->start1_o, sou->found->fblock->end2_o
						  , sou->found->fblock->start1_o + handleoffset
						  , sou->found->fblock->end2_o + handleoffset);
			if (G_LIKELY(sou->found->fblock->start1_o != BF_POSITION_UNDEFINED))
				sou->found->fblock->start1_o += handleoffset;
			if (G_LIKELY(sou->found->fblock->end1_o != BF_POSITION_UNDEFINED))
				sou->found->fblock->end1_o += handleoffset;
			if (G_LIKELY(sou->found->fblock->start2_o != BF_POSITION_UNDEFINED))
				sou->found->fblock->start2_o += handleoffset;
			if (G_LIKELY(sou->found->fblock->end2_o != BF_POSITION_UNDEFINED))
				sou->found->fblock->end2_o += handleoffset;
		}
		DBG_SCANCACHE("startpos=%u, offset=%d, prevoffset=%d, found=%p, update charoffset_o from %u to %u\n",startpos,offset,sou->prevoffset, sou->found,sou->found->charoffset_o,sou->found->charoffset_o+handleoffset);
#ifdef DEVELOPMENT
		if (((gint)(handleoffset+((gint)sou->found->charoffset_o))) < 0) {
			g_print("ABORT: scancache_update_single_offset, sou->found(%p)->charoffset_o(%d) will become < 0 ?!?!?! offset=%d, prevoffset=%d\n",
							sou->found,sou->found->charoffset_o,offset,sou->prevoffset);
			g_assert_not_reached();
		}
#endif
		sou->found->charoffset_o += handleoffset;

		tmpfound = get_foundcache_next(btv, &tmpsiter);
		if (tmpfound && tmpfound->charoffset_o > sou->prevpos && handleoffset == sou->prevoffset) {
			DBG_SCANCACHE("tmpfound(%u) > prevpos(%u), update handleoffset\n",tmpfound->charoffset_o,sou->prevpos);
			handleoffset += offset;
		}

		if (!tmpfound || ((gint)tmpfound->charoffset_o+handleoffset) >= ((gint)nextpos)) {
#ifdef DEVELOPMENT
			if (tmpfound)
				DBG_SCANCACHE("abort loop, tmpfound->charoffset=%u+handleoffset=%d=%d >= nextpos=%u\n",tmpfound->charoffset_o,handleoffset,
						tmpfound->charoffset_o+handleoffset,
						nextpos);
#endif
			break;
		}
		sou->found = tmpfound;
		sou->siter = tmpsiter;
	}

	if (sou->found && sou->found->charoffset_o < nextpos) {
		sou->prevpos = sou->found->charoffset_o;
	}
	sou->prevoffset += offset;
	DBG_SCANCACHE("return prevpos %d, prevoffset=%d, nextpos=%u, found->charoffset=%d\n",sou->prevpos,sou->prevoffset, nextpos, sou->found?sou->found->charoffset_o:-1);
}

static void
foundcache_process_offsetupdates(BluefishTextView * btv)
{
	Tscancache_offset_update sou = {NULL, NULL, 0,0,NULL,NULL};
	Toffsetupdate *ou = (Toffsetupdate *)bf_elist_first((Toffsetupdate *)btv->scancache.offsetupdates);
	while(ou) {
		Toffsetupdate *nextou = ou->next;
		gint32 offset, deleteoffset;
		offset = ou->offset;
		deleteoffset = ou->offset;
		/* now see if we can merge this update with the next update if it was on the same location (a replace, or delete and insert on the same location */
		if (nextou && nextou->startpos == ou->startpos) {
			Toffsetupdate *tmp;
			offset += nextou->offset;
			deleteoffset = MIN(ou->offset, nextou->offset);
			tmp = nextou;
			nextou = nextou->next;
			g_slice_free(Toffsetupdate, tmp);
			
		}
		DBG_SCANCACHE("foundcache_process_offsetupdates, update position %u with offset %d (deleteoffset=%d), next position at %u, prevpos=%u, prevoffset=%d\n",
							(guint) ou->startpos, (gint) offset, (gint)deleteoffset,
							(guint) (nextou ? OFFSETUPDATE(nextou)->startpos : BF_POSITION_UNDEFINED),
							sou.prevpos, sou.prevoffset);
		scancache_update_single_offset(btv, &sou, ou->startpos, offset, deleteoffset, nextou ? OFFSETUPDATE(nextou)->startpos : BF_POSITION_UNDEFINED);
		g_slice_free(Toffsetupdate, ou);
		ou = nextou;
	}
	btv->scancache.offsetupdates = NULL;
#ifdef DUMP_SCANCACHE_UPDATE_OFFSET
	dump_scancache(btv);
#endif
}

/**
 * foundcache_update_offsets 
 *
 * runs for every call to the insert_text and delete_text signal
 * this version only registers the offset changes, and processes them later
 * this reduces the latency on the insert_text and delete_text signals
 *
 * startpos is the lowest position
 *  so on insert it is the point _after_ which the insert will be (and offset is a positive number)
 *  on delete it is the point _after_ which the delete area starts (and offset is a negative number)
*/
void
foundcache_update_offsets(BluefishTextView * btv, guint startpos, gint offset)
{
	Toffsetupdate *ou;
	if (offset == 0)
		return;

#ifdef HL_PROFILING
	if (totalruntimer == NULL) {
		totalruntimer = g_timer_new();
	}
#endif

	/* check if the current offsetupdate has an offset greater than the last one, if not, first process the previous offsetupdates */
	if (btv->scancache.offsetupdates && OFFSETUPDATE(btv->scancache.offsetupdates)->startpos > startpos) {
		DBG_SCANCACHE("foundcache_update_offsets, startpos=%d, tail startpos=%d, call process_offsetupdates()\n", startpos, OFFSETUPDATE(btv->scancache.offsetupdates)->startpos);
		/* handle all the offsets */
		foundcache_process_offsetupdates(btv);
	} else if (btv->scancache.offsetupdates && OFFSETUPDATE(btv->scancache.offsetupdates)->startpos == startpos) {
 		/* merging can only be done in certain circumstances, for example: if a region is deleted, and
 		another region is inserted at that same position (a very common thing during replace, or filter), the now
		outdated region of the scancache should be cleared, so you cannot merge!!!!
		*/
		if ((OFFSETUPDATE(btv->scancache.offsetupdates)->offset < 0 && offset < 0)
				|| (OFFSETUPDATE(btv->scancache.offsetupdates)->offset > 0 && offset > 0)) {
			DBG_SCANCACHE("foundcache_update_offsets, last offset was also on position %d, merge old offset %d and new offset %d together\n",startpos, OFFSETUPDATE(btv->scancache.offsetupdates)->offset, offset);
			OFFSETUPDATE(btv->scancache.offsetupdates)->offset += offset;
			return;
		} else if (OFFSETUPDATE(btv->scancache.offsetupdates)->offset < 0 && offset > 0) {
			DEBUG_MSG("foundcache_update_offsets, last startpos equals current startpos (%d) - this is the insert from a replace action\n",startpos);
		} else {
			/* not a normal replace, first an insert and then the delete ?? process old cache first */
			DBG_SCANCACHE("foundcache_update_offsets, startpos=%d|offset=%d, tail startpos=%d|offset=%d, call process_offsetupdates()\n", startpos,offset, OFFSETUPDATE(btv->scancache.offsetupdates)->startpos, OFFSETUPDATE(btv->scancache.offsetupdates)->offset);
			foundcache_process_offsetupdates(btv);
		}
	}
	ou = g_slice_new(Toffsetupdate);
	ou->startpos = (guint32)startpos;
	ou->offset = (gint32)offset;
	DBG_SCANCACHE("foundcache_update_offsets, push offset %d at position %d on offsetupdates\n",offset,startpos);
	btv->scancache.offsetupdates = bf_elist_append(btv->scancache.offsetupdates, ou);
}

void
found_free_lcb(gpointer data, gpointer btv)
{
	Tfound *found = data;
	DBG_SCANCACHE("found_free_lcb, btv=%p, destroy found=%p\n", btv, found);
	if (IS_FOUNDMODE_BLOCKPUSH(found)) {
#ifdef HL_PROFILING
		hl_profiling.fblock_refcount--;
#endif
		DBG_SCANCACHE("found_free_lcb, btv=%p, free fblock=%p\n", btv, found->fblock);
		g_slice_free(Tfoundblock, found->fblock);
	}
	if (IS_FOUNDMODE_CONTEXTPUSH(found)) {
#ifdef HL_PROFILING
		hl_profiling.fcontext_refcount--;
#endif
		DBG_SCANCACHE("found_free_lcb, btv=%p, free fcontext=%p\n", btv, found->fcontext);
		g_slice_free(Tfoundcontext, found->fcontext);
	}
	
#ifdef HL_PROFILING
	hl_profiling.found_refcount--;
#endif
	g_slice_free(Tfound, found);
}

static void
remove_all_highlighting_in_area(BluefishTextView * btv, GtkTextIter * start, GtkTextIter * end, guint endoffset)
{
	GList *tmplist = g_list_first(btv->bflang->tags);
	DEBUG_MSG("remove_all_highlighting_in_area %d:%d\n",gtk_text_iter_get_offset(start),gtk_text_iter_get_offset(end));
	while (tmplist) {
		gtk_text_buffer_remove_tag(btv->buffer, (GtkTextTag *) tmplist->data, start, end);
		tmplist = g_list_next(tmplist);
	}
#ifdef HAVE_LIBENCHANT
	if (!btv->spell_check) {
		/* could be here if text was pasted */	
		gtk_text_buffer_remove_tag_by_name(btv->buffer,"_spellerror_",start,end);
	}
#endif
	btv->needremovetags = endoffset;
}

/*
static void print_blockstack(BluefishTextView * btv, Tscanning *scanning) {
	GList *tmplist;
	Tfoundblock *fblock;
	g_print("blockstack:");
	for (tmplist=scanning->blockstack->tail;tmplist;tmplist=tmplist->prev) {
		fblock = tmplist->data;
		g_print(" %s",g_array_index(btv->bflang->st->matches, Tpattern, fblock->patternum).pattern);
	}
	g_print("\n");
}*/

Tfoundblock *
pop_blocks(gint numchange, Tfoundblock * curblock)
{
	gint num = numchange;
	Tfoundblock *fblock = curblock;
	while (num < 0 && fblock) {
		fblock = (Tfoundblock *) fblock->parentfblock;
		num++;
	}
	return fblock;
}

static inline Tfoundblock *
found_start_of_block(BluefishTextView * btv, Tmatch * match, Tscanning * scanning)
{
	Tfoundblock *fblock;
#ifdef HL_PROFILING
	hl_profiling.numblockstart++;
	hl_profiling.fblock_refcount++;
#endif
	fblock = g_slice_new0(Tfoundblock);
	fblock->start1_o = gtk_text_iter_get_offset(&match->start);
	fblock->end1_o = gtk_text_iter_get_offset(&match->end);
	/*g_print("found blockstart with start_1 %d end1 %d\n",fblock->start1_o,fblock->end1_o); */
	fblock->start2_o = BF_POSITION_UNDEFINED;
	fblock->end2_o = BF_POSITION_UNDEFINED;
	fblock->patternum = match->patternum;
	DBG_BLOCKMATCH("found_start_of_block, %d:%d, put block for pattern %d (%s) on blockstack\n",
					fblock->start1_o,fblock->start2_o,match->patternum,
				   g_array_index(btv->bflang->st->matches, Tpattern, match->patternum).pattern);
	fblock->parentfblock = scanning->curfblock;
	DBG_BLOCKMATCH("found_start_of_block, new block at %p with parent %p\n", fblock, fblock->parentfblock);
	scanning->curfblock = fblock;
	return fblock;
}

static gboolean enlarge_scanning_region(BluefishTextView * btv, Tscanning * scanning, guint offset);

static inline Tfoundblock *
found_end_of_block(BluefishTextView * btv, Tmatch * match, Tscanning * scanning, Tpattern * pat,
				   gint * numblockchange)
{
	Tfoundblock *retfblock, *fblock = scanning->curfblock;
	GtkTextIter iter;
	gboolean allowfold=TRUE;
	guint match_start_o, match_end_o;

	DBG_BLOCKMATCH("found_end_of_block(), found %d (%s), blockstartpattern %d, curfblock=%p\n",
					match->patternum,
					g_array_index(btv->bflang->st->matches, Tpattern, match->patternum).pattern,
					pat->blockstartpattern,
				   scanning->curfblock);

	if (G_UNLIKELY(!scanning->curfblock))
		return NULL;

	retfblock = scanning->curfblock;
#ifdef HL_PROFILING
	hl_profiling.numblockend++;
#endif
	while (fblock && fblock->patternum != pat->blockstartpattern && pat->blockstartpattern != -1) {
		DBG_BLOCKMATCH("pop fblock %p (%d:%d-%d:%d)with patternum %d and parent %p\n", fblock
						, fblock->start1_o, fblock->end1_o , fblock->start2_o, fblock->end2_o
						, fblock->patternum, fblock->parentfblock);
		fblock = (Tfoundblock *) fblock->parentfblock;
		(*numblockchange)--;
	}

	if (G_UNLIKELY(!fblock)) {
		*numblockchange = 0;
		DBG_BLOCKMATCH("no matching start-of-block found\n");
		return NULL;
	}

	DBG_BLOCKMATCH("found the matching start-of-block fblock %p, patternum %d, parent %p, end2_o=%d\n",
				   fblock, fblock->patternum, fblock->parentfblock, fblock->end2_o);
	match_start_o = gtk_text_iter_get_offset(&match->start);
	match_end_o = gtk_text_iter_get_offset(&match->end);
	if (G_UNLIKELY(fblock->end1_o > match_start_o)) {
		/* possibly the block was stretched with stretch_block, undo the stretch */
		fblock->end1_o = match_start_o;
	}


	if (G_UNLIKELY(fblock->start2_o != BF_POSITION_UNDEFINED)) {
		Tfound *ifound;
		GSequenceIter *isiter = NULL, *cursiter;
		DBG_SCANCACHE
			("found_end_of_block, block (start at %d:%d) has an end already (old end %d:%d)! invalidate and enlarge region to previous end at end2_o %d\n",
					fblock->start1_o, fblock->end1_o,fblock->start2_o, fblock->end2_o, fblock->end2_o);
		/* this block was previously larger, so now we have to invalidate the previous
		   end of block in the cache */
		if (fblock->end2_o < match_end_o) {
			g_print("BUG: this block %p was ended already on a lower offset (%d) then we have right now (%d), so our blockstack is broken??\n", fblock, fblock->end2_o, gtk_text_iter_get_offset(&match->end));
			g_print("BUG: block %p started at %d:%d, ended at %d:%d\n",fblock, fblock->start1_o, fblock->end1_o, fblock->start2_o, fblock->end2_o);
		} else {
			enlarge_scanning_region(btv, scanning, fblock->end2_o);
			ifound = get_foundcache_at_offset(btv, fblock->end2_o, &isiter);
			if (ifound && ifound->charoffset_o == fblock->end2_o && isiter) {
				DBG_SCANCACHE("found_end_of_block, invalidate ifound=%p at offset %d\n", ifound,
							  ifound->charoffset_o);
				cursiter = scanning->siter;
				if (scanning->siter) {
					scanning->nextfound = get_foundcache_next(btv, &isiter);
					scanning->siter = isiter;
				}
				DBG_SCANCACHE("found_end_of_block, btv=%p, remove cache in range, nextfound=%p\n", btv, scanning->nextfound);
				g_sequence_foreach_range(cursiter, isiter, found_free_lcb, btv);
				DBG_SCANCACHE("found_end_of_block, remove range\n");
				g_sequence_remove_range(cursiter, isiter);
				DBG_SCANCACHE("found_end_of_block, check nextfound %p\n",scanning->nextfound);
				if (scanning->nextfound) {
					DBG_SCANCACHE("nextfound %p is now set to charoffset %d\n", scanning->nextfound,
								  scanning->nextfound->charoffset_o);
				}
			}
		}
	}

	fblock->start2_o = match_start_o;
	fblock->end2_o = match_end_o;
	gtk_text_buffer_get_iter_at_offset(btv->buffer, &iter, fblock->end1_o);
	if (G_UNLIKELY(g_array_index(btv->bflang->st->matches, Tpattern, fblock->patternum).block)) {
		if (g_array_index(btv->bflang->st->blocks, Tpattern_block, g_array_index(btv->bflang->st->matches, Tpattern, fblock->patternum).block).tag
			 	) {
			gtk_text_buffer_apply_tag(btv->buffer, g_array_index(btv->bflang->st->blocks, Tpattern_block, g_array_index(btv->bflang->st->matches, Tpattern, fblock->patternum).block).tag, &iter, &match->start);
		}
		allowfold = g_array_index(btv->bflang->st->blocks, Tpattern_block, g_array_index(btv->bflang->st->matches, Tpattern, fblock->patternum).block).foldable;
	}
	if (allowfold && (gtk_text_iter_get_line(&iter)) < gtk_text_iter_get_line(&match->start)) {
		fblock->foldable = TRUE;
	}
	DBG_BLOCKMATCH("found_end_of_block, set end for block %p to %d:%d, foldable=%d\n", fblock, fblock->start2_o, fblock->end2_o, fblock->foldable);
	scanning->curfblock = fblock->parentfblock;
	(*numblockchange)--;
	return retfblock;
}
/* pop_contexts expects a negative number !!!!!!!!!! */
static Tfoundcontext *
pop_contexts(gint numchange, Tfoundcontext * curcontext)
{
	gint num = numchange;
	Tfoundcontext *fcontext = curcontext;
	while (num < 0 && fcontext) {
		fcontext = (Tfoundcontext *) fcontext->parentfcontext;
		num++;
	}
	return fcontext;
}

static Tfoundcontext *
pop_and_apply_contexts(BluefishTextView * btv, gint numchange, Tfoundcontext * curcontext,
					   GtkTextIter * matchstart, gint * numchanged)
{
	gint num = numchange;
	guint offset = gtk_text_iter_get_offset(matchstart);
	Tfoundcontext *fcontext = curcontext;
	while (num < 0 && fcontext) {	/* pop, but don't pop if there is nothing to pop (because of an error in the language file) */
		DBG_SCANNING("pop_and_apply_contexts, end context %d at %d:%d, has tag %p and parent %p\n",
					 fcontext->context, fcontext->start_o, gtk_text_iter_get_offset(matchstart),
					 g_array_index(btv->bflang->st->contexts, Tcontext, fcontext->context).contexttag,
					 fcontext->parentfcontext);
		fcontext->end_o = offset;
		if (G_UNLIKELY(g_array_index(btv->bflang->st->contexts, Tcontext, fcontext->context).contexttag)) {
			GtkTextIter iter;
			gtk_text_buffer_get_iter_at_offset(btv->buffer, &iter, fcontext->start_o);
			gtk_text_buffer_apply_tag(btv->buffer,
									  g_array_index(btv->bflang->st->contexts, Tcontext,
													fcontext->context).contexttag, &iter, matchstart);
		}
		fcontext = (Tfoundcontext *) fcontext->parentfcontext;
		(*numchanged)--;
		num++;
	}
	return fcontext;
}

static inline Tfoundcontext *
found_context_change(BluefishTextView * btv, Tmatch * match, Tscanning * scanning, Tpattern * pat,
					 gint * numcontextchange)
{
	/* check if we change up or down the stack */
	if (pat->nextcontext < 0) {
		Tfoundcontext *retcontext = scanning->curfcontext;
#ifdef HL_PROFILING
		hl_profiling.numcontextend++;
#endif
		DBG_SCANNING("found_context_change, should pop %d contexts, curfcontext=%p\n",
					 (-1 * pat->nextcontext), scanning->curfcontext);
		*numcontextchange = 0;
		scanning->curfcontext =
			pop_and_apply_contexts(btv, pat->nextcontext, scanning->curfcontext, &match->start,
								   numcontextchange);
		scanning->context = scanning->curfcontext ? scanning->curfcontext->context : 1;
		return retcontext;
	} else {
		Tfoundcontext *fcontext;
#ifdef HL_PROFILING
		hl_profiling.numcontextstart++;
		hl_profiling.fcontext_refcount++;
#endif
		fcontext = g_slice_new0(Tfoundcontext);
		fcontext->start_o = gtk_text_iter_get_offset(&match->end);
		fcontext->end_o = BF_OFFSET_UNDEFINED;
		fcontext->parentfcontext = scanning->curfcontext;
		DBG_SCANNING("found_context_change, new fcontext %p with context %d onto the stack, parent=%p\n",
					 fcontext, pat->nextcontext, fcontext->parentfcontext);
		scanning->curfcontext = fcontext;
		scanning->context = fcontext->context = pat->nextcontext;
		*numcontextchange = 1;
		return fcontext;
	}
}

static gboolean
nextcache_valid(Tscanning * scanning)
{
	if (G_UNLIKELY(!scanning->nextfound))
		return FALSE;
	if (G_UNLIKELY(scanning->nextfound->numblockchange <= 0 && scanning->nextfound->fblock != scanning->curfblock)) {
		DBG_SCANCACHE("nextcache_valid, next found %p with numblockchange=%d has fblock=%p, current fblock=%p, return FALSE\n",
					  scanning->nextfound, scanning->nextfound->numblockchange, scanning->nextfound->fblock, scanning->curfblock);
		return FALSE;
	}
	DBG_SCANCACHE("nextcache_valid, next found %p with numcontextchange=%d has fcontext=%p, current fcontext=%p\n",
		scanning->nextfound, scanning->nextfound->numcontextchange, scanning->nextfound->fcontext, scanning->curfcontext);
	if (G_UNLIKELY(scanning->nextfound->numcontextchange <= 0 && scanning->nextfound->fcontext != scanning->curfcontext)) {
		DBG_SCANCACHE("nextcache_valid, next found %p with numcontextchange=%d has fcontext=%p, current fcontext=%p, return FALSE\n",
					  scanning->nextfound, scanning->nextfound->numcontextchange, scanning->nextfound->fcontext, scanning->curfcontext);
		return FALSE;
	}
	if (G_UNLIKELY(scanning->nextfound->numcontextchange > 0 && (!scanning->nextfound->fcontext
													  || scanning->nextfound->fcontext->parentfcontext !=
													  scanning->curfcontext))) {
		DBG_SCANCACHE
			("nextcache_valid, next found %p doesn't push context on top of current fcontext %p, return FALSE\n",
			 scanning->nextfound, scanning->curfcontext);
		return FALSE;
	}
	if (G_UNLIKELY(scanning->nextfound->numblockchange > 0 && (!scanning->nextfound->fblock
													|| scanning->nextfound->fblock->parentfblock !=
													scanning->curfblock))) {
		DBG_SCANCACHE
			("nextcache_valid, next found %p doesn't push block on top of current fblock %p, return FALSE\n",
			 scanning->nextfound, scanning->curfblock);
		return FALSE;
	}

	DBG_SCANCACHE("nextcache_valid, next found %p with offset=%d,numcontextchange=%d,numblockchange=%d seems valid\n", scanning->nextfound,
				  scanning->nextfound->charoffset_o,
				  scanning->nextfound->numcontextchange,
				  scanning->nextfound->numblockchange
				  );
	return TRUE;
}


static inline gboolean
cached_found_is_valid(BluefishTextView * btv, Tmatch * match, Tscanning * scanning, guint match_start_o, guint match_end_o)
{
	Tpattern *pat = &g_array_index(btv->bflang->st->matches, Tpattern, match->patternum);

	if (G_UNLIKELY(!scanning->nextfound))
		return FALSE;

	DBG_SCANCACHE("cached_found_is_valid, testing %p at offset %d\n", scanning->nextfound,
				  scanning->nextfound->charoffset_o);

	if (G_UNLIKELY(!nextcache_valid(scanning)))
		return FALSE;

	if (G_UNLIKELY(scanning->nextfound->indentlevel != match_end_o - match_start_o))
		return FALSE;

	DBG_SCANCACHE("with numcontextchange=%d, numblockchange=%d\n", scanning->nextfound->numcontextchange,
				  scanning->nextfound->numblockchange);
	if (IS_FOUNDMODE_BLOCKPUSH(scanning->nextfound)
		&& (!pat->starts_block || scanning->nextfound->fblock->patternum != match->patternum)) {
		DBG_SCANCACHE("cached_found_is_valid, cached entry %p does not push the same block\n",
					  scanning->nextfound);
		DBG_SCANCACHE
			("pat->startsblock=%d, nextfound->fblock->patternum=%d,match->patternum=%d,nextfound->fblock->parentfblock=%p,scanning->curfblock=%p\n",
			 pat->starts_block, scanning->nextfound->fblock->patternum, match->patternum,
			 scanning->nextfound->fblock->parentfblock, scanning->curfblock);
		return FALSE;
	}
	if (IS_FOUNDMODE_BLOCKPOP(scanning->nextfound) && !pat->ends_block) {
		/* TODO: add more checks for popped blocks */
		DBG_SCANCACHE("cached_found_is_valid, cached entry %p does not pop a block\n", scanning->nextfound);
		return FALSE;
	}
	if (IS_FOUNDMODE_CONTEXTPUSH(scanning->nextfound) && (pat->nextcontext <= 0
														  /*|| pat.nextcontext != scanning->context */
														  || scanning->nextfound->fcontext->context !=
														  pat->nextcontext)) {
		DBG_SCANCACHE("cached_found_is_valid, cached entry %p does not push the same context\n",
					  scanning->nextfound);
		DBG_SCANCACHE("cached pat->nextcontext=%d, fcontext->context=%d, current context=%d\n",
					  pat->nextcontext, scanning->nextfound->fcontext->context, scanning->context);
		return FALSE;
	}
	if ((scanning->nextfound->numcontextchange < 0)
		&& (pat->nextcontext != scanning->nextfound->numcontextchange)) {
		/* TODO: use more checks for popped contexts */
		DBG_SCANCACHE("cached_found_is_valid, cached entry %p does not pop the same context\n",
					  scanning->nextfound);
		DBG_SCANCACHE("cached numcontextchange=%d, pat.nextcontext=%d\n",
					  scanning->nextfound->numcontextchange, pat->nextcontext);
		return FALSE;
	}
	return TRUE;
}

/* called from found_match() if an entry is in the cache, but the scanner continued in the text beyond
this offset (and thus this is outdated), or this entry is invalid for some reason */
static guint
remove_invalid_cache(BluefishTextView * btv, guint match_end_o, Tscanning * scanning)
{
	guint invalidoffset=0;
	DBG_SCANNING("remove_invalid_cache, match_end_o=%d, scanning->nextfound=%p, scanning->curfblock=%p, scanning->curfblock=%p\n", match_end_o, scanning->nextfound, scanning->curfblock, scanning->curfblock);
	do {
		gint ret = remove_cache_entry(btv, &scanning->nextfound, &scanning->siter, scanning->curfblock, scanning->curfcontext);
		/* remove cache entry may return 0 if nothing was removed */
		DBG_SCANNING("remove_invalid_cache, scanning->nextfound=%p with offset %d\n", scanning->nextfound, scanning->nextfound ? scanning->nextfound->charoffset_o : -1);
		if (ret > invalidoffset)
			invalidoffset = ret;
	} while (scanning->nextfound && (scanning->nextfound->charoffset_o <= match_end_o || !nextcache_valid(scanning)));
	/* if there is no nextfound (so we removed the last items in the cache, we should return up to the end of the buffer as invalid */
	if (!scanning->nextfound) {
		GtkTextIter iter;
		gtk_text_buffer_get_end_iter(btv->buffer, &iter);
		invalidoffset = gtk_text_iter_get_offset(&iter);
	}

	DBG_SCANNING("remove_invalid_cache, return invalidoffset %d\n", invalidoffset);
	return invalidoffset;
}

static gboolean
enlarge_scanning_region_to_iter(BluefishTextView * btv, Tscanning * scanning, GtkTextIter * iter)
{
	if (gtk_text_iter_compare(iter, &scanning->end) > 0) {
		DBG_SCANCACHE("enlarge_scanning_region to offset %d\n", gtk_text_iter_get_offset(iter));
		remove_all_highlighting_in_area(btv, &scanning->end, iter, gtk_text_iter_get_offset(iter));
		scanning->end = *iter;
		return TRUE;
	}
	DBG_SCANCACHE("no need to increase scanning region to %d, is at %d already\n",gtk_text_iter_get_offset(iter), gtk_text_iter_get_offset(&scanning->end));
	return FALSE;
}

static gboolean
enlarge_scanning_region(BluefishTextView * btv, Tscanning * scanning, guint offset)
{
	GtkTextIter iter;
	gtk_text_buffer_get_iter_at_offset(btv->buffer, &iter, offset);
	return enlarge_scanning_region_to_iter(btv, scanning, &iter);
}

gboolean
test_condition(Tfoundcontext *curfcontext, Tfoundblock *curfblock, Tpattern_condition *pcond)
{
	if (pcond->relationtype == 1 || pcond->relationtype == 2) { /* context */
		Tfoundcontext *fcontext = curfcontext;
		if (pcond->parentrelation == -1) {
			while (fcontext) {
				DBG_SCANNING("compare context %d with context ref=%d\n",fcontext->context,pcond->ref);
				if (pcond->ref == fcontext->context) {
					return (pcond->relationtype == 1);
				}
				fcontext = fcontext->parentfcontext;
			}
		} else {
			gint i;
			for (i=0;i<pcond->parentrelation;i++) {
				DBG_SCANNING("pop context %d before comparing\n",fcontext->context);
				fcontext = fcontext->parentfcontext;
				if (!fcontext)
					return (pcond->relationtype == 2);
			}
			DBG_SCANNING("compare context %d with context ref=%d\n",fcontext?fcontext->context:0,pcond->ref);
			if (pcond->ref == fcontext->context) {
				return (pcond->relationtype == 1);
			}
		}
		return (pcond->relationtype == 2);
	} else if (pcond->relationtype == 3 || pcond->relationtype == 4) { /* pattern */
		Tfoundblock *fblock = curfblock;
		DBG_AUTOCOMP("test_condition, relationtype=%d, ref is %d, curfblock->patternum=%d\n", pcond->relationtype, pcond->ref,fblock->patternum);
		if (pcond->parentrelation == -1) {
			while (fblock) {
				if (pcond->ref == fblock->patternum) {
					return (pcond->relationtype == 3);
				}
				fblock = fblock->parentfblock;
			}
		} else {
			gint i;
			for (i=0;i<pcond->parentrelation;i++) {
				fblock = fblock->parentfblock;
				/*g_print("i=%d, fblock->patternum=%d\n",i,fblock?fblock->patternum:NULL);*/
				if (!fblock)
					return (pcond->relationtype == 4);
			}
			if (pcond->ref == fblock->patternum) {
				return (pcond->relationtype == 3);
			}
		}
		return (pcond->relationtype == 4);
	}
	return TRUE;
}


static gboolean
match_conditions_satisfied(BluefishTextView * btv, Tscanning *scanning, Tpattern *pat) {
	Tpattern_condition *pcond = &g_array_index(btv->bflang->st->conditions, Tpattern_condition, pat->condition);
	DBG_SCANNING("match_conditions_satisfied, called for pattern %s pcond %d with mode=%d, ref=%d and parentrelation=%d\n",pat->pattern,pat->condition, pcond->relationtype, pcond->ref, pcond->parentrelation);
	return test_condition(scanning->curfcontext, scanning->curfblock, pcond);
}

static inline int
found_match(BluefishTextView * btv, Tmatch * match, Tscanning * scanning)
{
	Tfoundblock *fblock = scanning->curfblock;
	Tfoundcontext *fcontext = scanning->curfcontext;
	guint match_end_o, match_start_o;
	gboolean cleanup_obsolete_cache_items=FALSE;
	gint numblockchange = 0, numcontextchange = 0, numindentchange=0;
	Tfound *found;
	GtkTextIter iter;
	Tpattern *pat = &g_array_index(btv->bflang->st->matches, Tpattern, match->patternum);

	if (pat->condition) {
		DBG_SCANNING("test for condition: pat %d has condition=%d\n",match->patternum,pat->condition );
		/* lookup the condition, and see if it matches */
		if (!match_conditions_satisfied(btv, scanning, pat)) {
			return scanning->context;
		}
	}
	DBG_SCANNING
		("found_match for pattern %d %s at charoffset %d-%d, starts_block=%d,ends_block=%d,is_indent=%d nextcontext=%d (current=%d)\n",
		 match->patternum, pat->pattern, gtk_text_iter_get_offset(&match->start),gtk_text_iter_get_offset(&match->end), pat->starts_block,
		 pat->ends_block, (pat->block == BLOCK_SPECIAL_INDENT), pat->nextcontext, scanning->context);
/*	DBG_MSG("pattern no. %d (%s) matches (%d:%d) --> nextcontext=%d\n", match->patternum, scantable.matches[match->patternum].message,
			gtk_text_iter_get_offset(&match->start), gtk_text_iter_get_offset(&match->end), scantable.matches[match->patternum].nextcontext);*/
	scanning->identmode = pat->identmode;
	scanning->identaction = pat->identaction;

	match_end_o = gtk_text_iter_get_offset(&match->end);
	match_start_o = gtk_text_iter_get_offset(&match->start);
	if (pat->selftag) {
		DBG_SCANNING("found_match, apply tag %p from %d to %d\n", pat->selftag,
					 gtk_text_iter_get_offset(&match->start), gtk_text_iter_get_offset(&match->end));
		gtk_text_buffer_apply_tag(btv->buffer, pat->selftag, &match->start, &match->end);
	}
	/* the conditions when to apply stretch_blocktag:
		- currently found pattern (pat) has stretch_blockstart set
		- there must be a block on the stack
		- the blockstartpattern for the current found pattern must refer to the block on the stack
		- the block on the stack must not have an end that is before the current position. start2_o
		  could be equal to the current position if the end starts where the start ends as in  <p></p>
		  or it could be undefined
	*/
	if (G_UNLIKELY(pat->stretch_blockstart
				&& scanning->curfblock
				&& scanning->curfblock->patternum == pat->blockstartpattern
				&& (scanning->curfblock->start2_o == BF_POSITION_UNDEFINED || scanning->curfblock->start2_o >= match_end_o))) {
		/* get the current block on the stack and stretch the end-of-blockstart to the end of the match */
		DBG_SCANNING("found_match, pat->stretch_blockstart=%d, pat->blockstartpattern=%d, update curfblock(%d:%d-%d:%d) with patternum=%d from end1_o from %d to %d\n",
						pat->stretch_blockstart,pat->blockstartpattern,
						scanning->curfblock->start1_o, scanning->curfblock->end1_o, scanning->curfblock->start2_o, scanning->curfblock->end2_o,
						scanning->curfblock->patternum, scanning->curfblock->end1_o, match_end_o);
		scanning->curfblock->end1_o = match_end_o;
	}

	if G_LIKELY((!pat->starts_block && !pat->ends_block && pat->block != BLOCK_SPECIAL_INDENT
		&& (pat->nextcontext == 0 || pat->nextcontext == scanning->context))) {
		DBG_SCANNING("found_match, pattern does not start block or context, return\n");
		return scanning->context;
	}

	/* There are three situations comparing the current scan to the cached results:
	   1: the cache entry has an offset lower than the current offset or equal but a different patternum and
	   is thus not valid anymore. That means that the region that needs scanning should be enlarged up
	   to the fcontext or fblock end from the cache.
	   2: the cache entry has the same offset and the same patternum and is thus valid, we only highlight and
	   store nothing in the cache
	   3: the cache entry has a higher offset -> do nothing with the cached one,
	   and enlarge the area to scan because we have a context change or new block that
	   previously didn't exist
	 */
	if (scanning->nextfound) {
		DBG_SCANCACHE("found_match, testing nextfound %p\n", scanning->nextfound);
		if (scanning->nextfound->charoffset_o > match_end_o) {
			DBG_SCANCACHE
				("found_match, next item in the cache (offset %d) is not relevant yet (offset now %d), set scanning end to %d\n",
				 scanning->nextfound->charoffset_o, match_end_o, scanning->nextfound->charoffset_o);
			enlarge_scanning_region(btv, scanning, scanning->nextfound->charoffset_o);
		} else if (scanning->nextfound->charoffset_o == match_end_o
				   && cached_found_is_valid(btv, match, scanning, match_start_o, match_end_o)) {
			Tfoundcontext *tmpfcontext;
			/* BUG? tmpfcontext is not always initialised ??*/
			gint context;
			DBG_SCANCACHE("found_match, cache item at offset %d is still valid\n",
						  scanning->nextfound->charoffset_o);
			if (scanning->nextfound->numcontextchange >= 0) {
				context = scanning->nextfound->fcontext ? scanning->nextfound->fcontext->context : 1;
				tmpfcontext = scanning->nextfound->fcontext;
			} else if (pat->nextcontext < 0) {
				gint tmp = 0;
				tmpfcontext =
					pop_and_apply_contexts(btv, pat->nextcontext, scanning->curfcontext, &match->start, &tmp);
				Tfoundcontext *tmpfcontext2 = pop_contexts(pat->nextcontext, scanning->nextfound->fcontext);
				context = tmpfcontext ? tmpfcontext->context : 1;
				if (tmpfcontext != tmpfcontext2) {
					g_warning
						("found_match, ERROR: popped context from cache does not equal popped context from current scan\n");
				}
			} else {
				g_warning("bug in syntax scanner, tmpfcontext is not initialised, please report this\n");
				tmpfcontext = NULL;
			}
			if (scanning->nextfound->numblockchange < 0) {
				scanning->curfblock = pop_blocks(scanning->nextfound->numblockchange, fblock);
			} else {
				scanning->curfblock = scanning->nextfound->fblock;
			}
			scanning->curfcontext = tmpfcontext;
			scanning->nextfound = get_foundcache_next(btv, &scanning->siter);
			DBG_SCANNING("found at %d is still valid, returning\n",scanning->nextfound->charoffset_o);
			return context;
		} else {				/* either a smaller offset, or invalid */
			cleanup_obsolete_cache_items = TRUE;
		}
	}
	/* TODO: in all cases: if we find a previously unknown match and there is no nextfound we
	   have to scan to the end of the text */
	if (!scanning->nextfound) {
		DBG_SCANNING("no nextfound, so enlarge scanning region to end iter\n");
		gtk_text_buffer_get_end_iter(btv->buffer, &iter);
		enlarge_scanning_region_to_iter(btv, scanning, &iter);
	}

	if ((pat->starts_block)) {
		fblock = found_start_of_block(btv, match, scanning);
		numblockchange = 1;
	} else if (pat->ends_block) {
		fblock = found_end_of_block(btv, match, scanning, pat, &numblockchange);
		if (!fblock) {
			fblock = scanning->curfblock;
		}
	}
#ifdef CHECK_CONSISTENCY
	if (numblockchange == 0) {
		g_assert(fblock == scanning->curfblock);
	} else if (numblockchange == 1) {
		g_assert(fblock == scanning->curfblock);
	} else if (numblockchange > 1) {
		g_assert(FALSE);
	} else if (numblockchange < 0) {
		Tfoundblock *tmpfblock = pop_blocks(numblockchange, fblock);
		g_assert(tmpfblock == scanning->curfblock);
	}
#endif
	if (pat->nextcontext != 0 && pat->nextcontext != scanning->context) {
		fcontext = found_context_change(btv, match, scanning, pat, &numcontextchange);
	} else {
		numcontextchange = 0;
	}
	if (numblockchange == 0 
				&& numcontextchange == 0 
				&& numindentchange == 0 
				&& (pat->block != BLOCK_SPECIAL_INDENT || pat->block == BLOCK_SPECIAL_INDENT && gtk_text_iter_ends_line(&match->end))) {
		DBG_SCANCACHE("found_match, no context change, no block change, no indent or indent on further enmpty line, return\n");
		return scanning->context;
	}
	

	if (cleanup_obsolete_cache_items && scanning->nextfound) {
		/* we have to do the cleanup *after* found_start_of_block/found_end_of_block/found_context_change so
		Tscanning will reflext the up-to-date situation at this offset */
		guint invalidoffset;
		DBG_SCANCACHE("found_match, found %p with offset %d will be removed\n", scanning->nextfound,
						  scanning->nextfound->charoffset_o);
		invalidoffset = remove_invalid_cache(btv, match_end_o, scanning);
		DBG_SCANCACHE("found_match, now enlarge scanning region to %d\n", invalidoffset);
		enlarge_scanning_region(btv, scanning, invalidoffset);
	}

	found = g_slice_new0(Tfound);
#ifdef HL_PROFILING
	hl_profiling.found_refcount++;
#endif
	found->numblockchange = numblockchange;
	found->fblock = fblock;
	found->numcontextchange = numcontextchange;
	found->fcontext = fcontext;
	found->charoffset_o = match_end_o;
	if (G_UNLIKELY(pat->block == BLOCK_SPECIAL_INDENT)) {
		/* indentlevel is currently a 8bit unsigned integer, so we never store a value higher than MAX_INDENT_LEVEL */
		found->indentlevel = MIN(match_end_o - match_start_o,MAX_INDENT_LEVEL);
		DBG_SCANNING("set indentlevel=%d for found at offset %d\n",found->indentlevel, found->charoffset_o);
	} else {
		found->indentlevel = NO_INDENT_FOUND;
	}
	DBG_SCANCACHE
		("found_match, put found %p in the cache charoffset_o=%d fblock=%p numblockchange=%d fcontext=%p numcontextchange=%d\n",
		 found, found->charoffset_o, found->fblock, found->numblockchange, found->fcontext,
		 found->numcontextchange);
	g_sequence_insert_sorted(btv->scancache.foundcaches, found, foundcache_compare_charoffset_o, NULL);
	g_assert(found->numblockchange == 0 || found->fblock);
	g_assert(found->numcontextchange == 0 || found->fcontext);
	return scanning->context;
}

#ifdef MARKREGION
static gboolean
markregion_find_region2scan(BluefishTextView * btv, GtkTextIter * sit, GtkTextIter * eit)
{
	gpointer tmp=NULL;
	gboolean cont=TRUE;
	guint start,end;
	tmp = markregion_get_region(&btv->scanning, tmp, &start, &end);
	if (start == BF_OFFSET_UNDEFINED) {
		return FALSE;
	}
	DBG_MARKREGION("markregion_find_region2scan, got region %u:%u\n",start,end);
	while (tmp && cont) {
		guint start2, end2;
		cont=FALSE;
		tmp = markregion_get_region(&btv->scanning, tmp, &start2, &end2);
		if (start2-end < (loops_per_timer) && (start2 - start) < (NUM_TIMER_CHECKS_PER_RUN * loops_per_timer)) {
			DBG_MARKREGION("markregion_find_region2scan, current region %u:%u, next region begins at %u, ends at %u, merge!\n",start,end,start2,end2);
			end = end2;
			cont=TRUE;
		}
	}
	gtk_text_buffer_get_iter_at_offset(btv->buffer, sit, start);
	gtk_text_buffer_get_iter_at_offset(btv->buffer, eit, end);
	DBG_MARKREGION("markregion_find_region2scan, tried iters at %u:%u, got iters at %u:%u\n",start,end,gtk_text_iter_get_offset(sit),gtk_text_iter_get_offset(eit));
	return TRUE;
}
#endif

static guint
reconstruct_scanning(BluefishTextView * btv, GtkTextIter * position, Tscanning * scanning)
{
	Tfound *found;
	guint offset = gtk_text_iter_get_offset(position);
	DBG_SCANNING("reconstruct_scanning at position %d\n", offset);
	found = get_foundcache_at_offset(btv, offset, &scanning->siter);
	DBG_SCANCACHE("reconstruct_stack, got found %p at offset %d to reconstruct stack at position %d\n", found, found?found->charoffset_o:-1, offset);
	if (G_LIKELY(found && found->charoffset_o <= offset)) {
		if (found->numcontextchange < 0) {
			scanning->curfcontext = pop_contexts(found->numcontextchange, found->fcontext);
		} else {
			scanning->curfcontext = found->fcontext;
		}
		if (found->numblockchange < 0) {
			scanning->curfblock = pop_blocks(found->numblockchange, found->fblock);
		} else {
			scanning->curfblock = found->fblock;
		}
		scanning->context = (scanning->curfcontext) ? scanning->curfcontext->context : 1;
		scanning->nextfound = get_foundcache_next(btv, &scanning->siter);
		DBG_SCANNING("reconstruct_stack, found at offset %d, curfblock=%p, curfcontext=%p, context=%d\n",
					 found->charoffset_o, scanning->curfblock, scanning->curfcontext, scanning->context);
		
		return found->charoffset_o;
	} else {
		DBG_SCANNING("nothing to reconstruct\n");
		scanning->curfcontext = NULL;
		scanning->curfblock = NULL;
		scanning->context = 1;
		scanning->nextfound = get_foundcache_first(btv, &scanning->siter);
		DBG_SCANNING("reconstruct_scanning, nextfound=%p\n", scanning->nextfound);
		return 0;
	}
}

/* if visible_end is set (not NULL) we will scan only the visible area and nothing else.
this can be used to delay scanning everything until the editor is idle for several milliseconds */
gboolean
bftextview2_run_scanner(BluefishTextView * btv, GtkTextIter * visible_end)
{
	GtkTextIter iter;
	GtkTextIter mstart;
	Tscanning scanning;
	guint pos = 0, newpos, reconstruction_o, endoffset;
	gboolean end_of_region = FALSE, last_character_run = FALSE, continue_loop = TRUE, finished;
	gint loop = 0;
	GtkTextIter itcursor;
#ifdef HL_PROFILING
	guint startpos;
	gdouble stage1 = 0;
	gdouble stage2;
	gdouble stage3;
	gdouble stage4;
	hl_profiling.longest_contextstack = 0;
	hl_profiling.longest_blockstack = 0;
	hl_profiling.numcontextstart = 0;
	hl_profiling.numcontextend = 0;
	hl_profiling.numblockstart = 0;
	hl_profiling.numblockend = 0;
	hl_profiling.numchars = 0;
	hl_profiling.numloops = 0;
#endif

	scanning.context = 1;
	scanning.identmode = 0;

	DBG_MSG("bftextview2_run_scanner for btv %p..\n", btv);
	if (!btv->bflang->st) {
		DBG_MSG("no scantable, nothing to scan, returning...\n");
		return FALSE;
	}
#ifdef VALGRIND_PROFILING
	CALLGRIND_START_INSTRUMENTATION;
#endif							/* VALGRIND_PROFILING */

	DEBUG_MSG("bftextview2_run_scanner, first call foundcache_process_offsetupdates()\n");
	foundcache_process_offsetupdates(btv);

	if (!markregion_find_region2scan(btv, &scanning.start, &scanning.end)) {
		DBG_MSG("nothing to scan here.. return FALSE\n");
#ifdef VALGRIND_PROFILING
		CALLGRIND_STOP_INSTRUMENTATION;
#endif							/* VALGRIND_PROFILING */
#ifdef HL_PROFILING
		if (totalruntimer) {
			g_print("Nothing to scan anymore, total run timer took %d ms\n",(gint) (1000.0 * g_timer_elapsed(totalruntimer,NULL)));
			g_timer_destroy(totalruntimer);
			totalruntimer = NULL;
		}
#endif /*HL_PROFILING*/
		return FALSE;
	}
	DBG_SCANNING("markregion_find_region2scan returned region %d:%d\n",gtk_text_iter_get_offset(&scanning.start),gtk_text_iter_get_offset(&scanning.end));
	/* start timer */
	scanning.timer = g_timer_new();

#ifdef HL_PROFILING
	if (totalruntimer == NULL) {
		totalruntimer = g_timer_new();
	}
#endif

	if (visible_end) {
		/* make sure that we only scan up to visible_end and no further */
		if (gtk_text_iter_compare(&scanning.start, visible_end) > 0) {
			DBG_DELAYSCANNING("start of region that needs scanning is beyond visible_end, return TRUE\n");
			g_timer_destroy(scanning.timer);
#ifdef VALGRIND_PROFILING
			CALLGRIND_STOP_INSTRUMENTATION;
#endif							/* VALGRIND_PROFILING */
			return TRUE; /* call us again */
		}
		if (gtk_text_iter_compare(&scanning.end, visible_end) > 0) {
			DBG_DELAYSCANNING
				("end of region that needs scanning (%d) is beyond visible_end (%d), reset end\n",
				 gtk_text_iter_get_offset(&scanning.end), gtk_text_iter_get_offset(visible_end));
			scanning.end = *visible_end;
		}
	}
#ifdef HL_PROFILING
	stage1 = g_timer_elapsed(scanning.timer, NULL);
#endif
	iter = mstart = scanning.start;
	DBG_SCANNING("set iter=mstart=scaning.start all to %d\n",gtk_text_iter_get_offset(&scanning.start));
	if (gtk_text_iter_is_start(&scanning.start)) {
		DBG_SCANNING("start scanning at start iter\n");
		scanning.siter = g_sequence_get_begin_iter(btv->scancache.foundcaches);
		scanning.nextfound = get_foundcache_first(btv, &scanning.siter);
		scanning.curfcontext = NULL;
		scanning.curfblock = NULL;
		reconstruction_o = 0;
	} else {
		/* we do not want to reconstruct a blockstart exactly at the point where scanning.start is right now, otherwise
		if we previously found <b and now there is <bo and we reconstruct the stack between the b and the o and we would not
		detect that the tag has changed. so we move scanning.start one position up. */
		gtk_text_iter_backward_char(&iter);
		mstart = scanning.start = iter;
		DBG_SCANNING("moved scanning.start, mstart, iter back to %d\n",gtk_text_iter_get_offset(&scanning.start));
		/* reconstruct the context stack and the block stack */
		reconstruction_o = reconstruct_scanning(btv, &iter, &scanning);
		pos = 0;
		DBG_SCANNING("reconstructed stacks, context=%d, startstate=%d, nextfound=%p\n", scanning.context, pos, scanning.nextfound);
		/* now move the start position either to the start of the line, or to the position
		   where the stack was reconstructed, the largest offset */
		gtk_text_buffer_get_iter_at_offset(btv->buffer, &iter, reconstruction_o);
		gtk_text_iter_set_line_offset(&scanning.start, 0);
		DBG_SCANNING("move scanning.start to start of line, new position %d\n",gtk_text_iter_get_offset(&scanning.start));
		DBG_SCANNING("compare possible start positions %d and %d\n",
					 gtk_text_iter_get_offset(&scanning.start), gtk_text_iter_get_offset(&iter));
		DBG_SCANNING("should we start scanning at iter=%d (the scancache reconstruction point) or scanning.start=%d?\n", gtk_text_iter_get_offset(&iter), gtk_text_iter_get_offset(&scanning.start));
		if (gtk_text_iter_compare(&iter, &scanning.start) > 0) {
			mstart = scanning.start = iter;
		} else {
			iter = mstart = scanning.start;
		}
	}
	if (!gtk_text_iter_is_end(&scanning.end)) {
		/* the end position should be the largest of the end of the line and the 'end' iter */
		gtk_text_iter_forward_to_line_end(&iter);
		/*gtk_text_iter_forward_line(&iter);*/
		g_print("should we end scanning at iter=%d (the end-of-line after the startpos) or scanning.end=%d?\n", gtk_text_iter_get_offset(&iter), gtk_text_iter_get_offset(&scanning.end));
		if (gtk_text_iter_compare(&iter, &scanning.end) >= 0)
			scanning.end = iter;
		iter = scanning.start;
	}
	DBG_SCANNING("scanning from %d to %d\n", gtk_text_iter_get_offset(&scanning.start),
				 gtk_text_iter_get_offset(&scanning.end));
#ifdef HL_PROFILING
	startpos = gtk_text_iter_get_offset(&scanning.start);
	stage2 = g_timer_elapsed(scanning.timer, NULL);
#endif
	endoffset = gtk_text_iter_get_offset(&scanning.end);
	if (btv->needremovetags < endoffset) {
		remove_all_highlighting_in_area(btv, &scanning.start, &scanning.end, endoffset);
	}
#ifdef HL_PROFILING
	stage3 = g_timer_elapsed(scanning.timer, NULL);
#endif
/*	if (!visible_end)
		gtk_text_iter_forward_to_end(&end);
	else
		end = *visible_end;*/
	gtk_text_buffer_get_iter_at_mark(btv->buffer, &itcursor, gtk_text_buffer_get_insert(btv->buffer));
/* ******************************************************************************
in the following loop we do the actual scanning. At the current offset (iter) we get a character (uc)

every loop, we lookup the next position in the table (newpos), using the character (uc), context (scanning.context), and previous position (pos)

if newpos==0 we have a symbol (see bftextview2.h for an explanation of symbols and identifiers)
   a symbol can be the start or the end of a match, and may be part of the match. so whenever we hit a symbol, we
   set the match start (mstart) to the current offset
   if we find a symbol again, and we have a match, we have the start of the match (mstart) and the end of the match at the current position (iter)

****************************************************************************** */
	gboolean didindentrun=FALSE;
	gboolean zerolengthmatch;
	do {
		gunichar uc;
		loop++;
		zerolengthmatch=FALSE;
#ifdef HL_PROFILING
		hl_profiling.numloops++;
#endif
		if (G_UNLIKELY(last_character_run)) {
			uc = '\0';
		} else {
			uc = gtk_text_iter_get_char(&iter);
			if (G_UNLIKELY(uc > 128)) {
				/* multibyte characters cannot be matched by the engine. character
				   1 in ascii is "SOH (start of heading)". we need this to support a
				   pattern like [^#]* .  */
				uc = 1;
			}
		}
		if (G_UNLIKELY(!didindentrun && (pos == 0 || pos == 1) && g_array_index(btv->bflang->st->contexts, Tcontext, scanning.context).indent_detection && gtk_text_iter_starts_line(&iter))) {
			/* if we are in a context that has indenting detection, and we are on the start of a line, we 
			use ASCII character 2 STX (start of text) to enter the state for indenting */
			pos = get_tablerow(btv->bflang->st,scanning.context,pos).row['\x02'];
			didindentrun=TRUE;
		} else{
			didindentrun=FALSE;
		}
		
		DBG_SCANNING("scanning offset %d pos %d '%c'=%d ", gtk_text_iter_get_offset(&iter), pos, uc, uc);
		newpos = get_tablerow(btv->bflang->st,scanning.context,pos).row[uc];
		if (G_UNLIKELY(g_array_index(btv->bflang->st->contexts, Tcontext, scanning.context).dump_dfa_run)) {
			if (g_array_index(btv->bflang->st->contexts, Tcontext, scanning.context).indent_detection && gtk_text_iter_starts_line(&iter)) {
				g_print("context %d offset %4d starts line, set pos %4d for indent detection\n",scanning.context, gtk_text_iter_get_offset(&iter),pos);
			}
			g_print("context %d offset %4d '",scanning.context, gtk_text_iter_get_offset(&iter));
			print_character_escaped(uc);
			g_print("' in %4d makes %4d", pos,newpos);
			if (newpos == 0 && get_tablerow(btv->bflang->st,scanning.context,pos).match) {
				g_print(" --> a symbol or the pattern ends on a symbol, the previous was a match (%s)",
					g_array_index(btv->bflang->st->matches, Tpattern,get_tablerow(btv->bflang->st,scanning.context,pos).match).pattern);
			} else if (newpos == 0) {
				g_print(" --> restart scanning (found a symbol, no match?)");
			} else if (newpos == 1 && pos != 1) {
				g_print(" --> nothing matches, go to identstate");
			} else if (newpos == 1 && pos == 1) {
				g_print(" .....identstate");
			}
			g_print("\n");
		}
		DBG_SCANNING("(context=%d).. got newpos %d %s\n", scanning.context, newpos, (newpos==0?" -> symbol or pattern itself ends on symbol":""));
		if (G_UNLIKELY(newpos == 0 || uc == '\0')) {
			if (G_UNLIKELY(get_tablerow(btv->bflang->st,scanning.context,pos).match)) {
				/* if the newpos goes to state 0, and pos indicates a match, we have a match!
				everything that needs to be done for a found match is handled in the function found_match()
				which returns a new context if needed
				*/
				Tmatch match;
				guint oldcontext = scanning.context;
				match.patternum = get_tablerow(btv->bflang->st,scanning.context,pos).match;
				match.start = mstart;
				match.end = iter;
				DBG_SCANNING("we have a match from pos %d to %d\n", gtk_text_iter_get_offset(&match.start),
							 gtk_text_iter_get_offset(&match.end));
				scanning.context = found_match(btv, &match, &scanning);
				DBG_SCANNING("after match context=%d\n", scanning.context);
				
				/* because of indent detection we can find a zero length match */
				zerolengthmatch =  gtk_text_iter_equal(&mstart, &iter);
				
				/* if this indicates an identifier, we call found_identifier() */
				if (G_UNLIKELY(scanning.identmode == 2 && !gtk_text_iter_in_range(&itcursor, &mstart, &iter)
								 && !gtk_text_iter_equal(&itcursor, &mstart)
								 && !gtk_text_iter_equal(&itcursor, &iter))) {
					found_identifier(btv, &mstart, &iter, oldcontext, scanning.identaction);
					scanning.identmode = 0;
				}
			} else {

				/* if there is no match, but the cache does have a match, we have to clean it up */
				if (G_UNLIKELY(uc == '\0' && scanning.nextfound &&
					scanning.nextfound->charoffset_o <= gtk_text_iter_get_offset(&iter))) {
					guint invalidoffset;
					/* scanning->nextfound is invalid! remove from cache */
					invalidoffset = remove_invalid_cache(btv, gtk_text_iter_get_offset(&iter), &scanning);
					if (enlarge_scanning_region(btv, &scanning, invalidoffset))
						last_character_run = FALSE;
				}

				/* if we are in identmode, we might have to call found_identifier() */
				if (G_UNLIKELY
					(scanning.identmode == 1 && pos == 1)) {
					/* ignore if the cursor is within the range, because it could be that the user is still typing the name */
					if (G_LIKELY(!gtk_text_iter_in_range(&itcursor, &mstart, &iter)
								 && !gtk_text_iter_equal(&itcursor, &mstart)
								 && !gtk_text_iter_equal(&itcursor, &iter))) {
						found_identifier(btv, &mstart, &iter, scanning.context, scanning.identaction);
						scanning.identmode = 0;
					}
				}
				DBG_SCANNING("no match, but do set mstart to offset %d and set newpos=0\n",gtk_text_iter_get_offset(&iter));
			}

			/* see if nextfound has a valid context and block stack, if not we enlarge the scanning area */
			if (G_UNLIKELY(last_character_run && scanning.nextfound && !nextcache_valid(&scanning))) {
				guint invalidoffset;
				DBG_SCANNING("last_character_run, but nextfound %p is INVALID!\n", scanning.nextfound);
				invalidoffset = remove_invalid_cache(btv, 0, &scanning);
				enlarge_scanning_region(btv, &scanning, invalidoffset);
				/* TODO: do we need to rescan this position with the real uc instead of uc='\0' ?? */
			}

			/* normally after we return to newpos 0, we restart the scanning at the current character (it might be the first character of a new match)
			but not if this is not the first character of a new match (indicated if mstart is at the same position as iter) 
			this function has a problem with indenting detection, because it can find a match of zero length */
			if (G_LIKELY(((gtk_text_iter_equal(&mstart, &iter)) && !zerolengthmatch) && !last_character_run)) {
				DBG_SCANNING("did found a match, but don't rescan current character if it starts a new match, because iter is at mstart (pos=%d, newpos=%d)\n",pos,newpos);
				gtk_text_iter_forward_char(&iter);
#ifdef HL_PROFILING
				hl_profiling.numchars++;
#endif
			}
			mstart = iter;
			DBG_SCANNING("mstart is set to offset %d, newpos=0\n",gtk_text_iter_get_offset(&mstart));
			newpos = 0;
		} else if (G_LIKELY(!last_character_run)) {
			gtk_text_iter_forward_char(&iter);
#ifdef HL_PROFILING
			hl_profiling.numchars++;
#endif
		}
		pos = newpos;
		end_of_region = gtk_text_iter_equal(&iter, &scanning.end);
		if (G_UNLIKELY(end_of_region || last_character_run)) {
			last_character_run = !last_character_run;
		}
		continue_loop = (!end_of_region || last_character_run);
		/*DBG_SCANNING("continue_loop=%d, end_of_region=%d, last_character_run=%d\n",continue_loop,end_of_region,last_character_run); */
	} while (continue_loop
			 && (loop % loops_per_timer != 0
				 || g_timer_elapsed(scanning.timer, NULL) < MAX_CONTINUOUS_SCANNING_INTERVAL));
	DBG_SCANNING
		("scanned from %d to position %d, (end=%d) which took %f microseconds, loops_per_timer=%d\n",
		 gtk_text_iter_get_offset(&scanning.start), gtk_text_iter_get_offset(&iter),
		 gtk_text_iter_get_offset(&scanning.end), g_timer_elapsed(scanning.timer, NULL), loops_per_timer);
	/* TODO: if we end the scan within a context that has a tag, we have to apply the contexttag */
	if (!gtk_text_iter_is_end(&scanning.end) && scanning.curfcontext) {
		if (g_array_index(btv->bflang->st->contexts, Tcontext, scanning.curfcontext->context).contexttag) {
			GtkTextIter iter2;
			gtk_text_buffer_get_iter_at_offset(btv->buffer, &iter2, scanning.curfcontext->start_o);
			gtk_text_buffer_apply_tag(btv->buffer,
									  g_array_index(btv->bflang->st->contexts, Tcontext,
													scanning.curfcontext->context).contexttag, &iter, &iter2);
		}
	}
#ifdef MARKREGION
	DBG_MARKREGION("bftextview2_run_scanner, region done until %d, mark needscanning from %d:%d\n",gtk_text_iter_get_offset(&iter), gtk_text_iter_get_offset(&iter), gtk_text_iter_get_offset(&scanning.end));
	markregion_region_done(&btv->scanning, gtk_text_iter_get_offset(&iter));
	if (gtk_text_iter_compare(&iter, &scanning.end) < 0) {
		markregion_nochange(&btv->scanning, gtk_text_iter_get_offset(&iter), gtk_text_iter_get_offset(&scanning.end));
	}
#endif
	finished = gtk_text_iter_is_end(&iter);

#ifdef HL_PROFILING
	stage4 = g_timer_elapsed(scanning.timer, NULL);
	hl_profiling.total_runs++;
	hl_profiling.total_chars += hl_profiling.numchars;
	hl_profiling.total_time_ms += (gint) (1000.0 * stage4);
	g_print
		("scanning run %d (%d ms): %d, %d, %d, %d; from %d-%d, loops=%d,chars=%d,blocks %d/%d (%d) contexts %d/%d (%d) scancache %d\n",
		 hl_profiling.total_runs, (gint) (1000.0 * stage4)
		 , (gint) (1000.0 * stage1)
		 , (gint) (1000.0 * stage2 - stage1)
		 , (gint) (1000.0 * stage3 - stage2)
		 , (gint) (1000.0 * stage4 - stage3)
		 , startpos, gtk_text_iter_get_offset(&iter)
		 , hl_profiling.numloops, hl_profiling.numchars, hl_profiling.numblockstart, hl_profiling.numblockend,
		 hl_profiling.longest_blockstack, hl_profiling.numcontextstart, hl_profiling.numcontextend,
		 hl_profiling.longest_contextstack, g_sequence_get_length(btv->scancache.foundcaches)
		);
	g_print("memory scancache %d(%dKb+%dKb) found %d(%dKb) fcontext %d(%dKb) = %dKb\n",
			hl_profiling.found_refcount, (gint) (hl_profiling.found_refcount * sizeof(Tfound) / 1024.0),
			(gint) (hl_profiling.found_refcount * 40 / 1024.0)
			, hl_profiling.fblock_refcount,
			(gint) (hl_profiling.fblock_refcount * sizeof(Tfoundblock) / 1024.0)
			, hl_profiling.fcontext_refcount,
			(gint) (hl_profiling.fcontext_refcount * sizeof(Tfoundcontext) / 1024.0)
			,
			(gint) ((hl_profiling.found_refcount * sizeof(Tfound) +
					 hl_profiling.found_refcount * 5 * sizeof(gpointer) +
					 hl_profiling.fblock_refcount * sizeof(Tfoundblock) +
					 hl_profiling.fcontext_refcount * sizeof(Tfoundcontext)) / 1024.0)
		);
	g_print("average %d chars/s %d chars/run\n",
			(guint) (1000.0 * hl_profiling.total_chars / hl_profiling.total_time_ms)
			, (guint) (1.0 * hl_profiling.total_chars / hl_profiling.total_runs)
		);
#endif
	/* tune the loops_per_timer, try to have 10 timer checks per loop, so we have around 10% deviation from the set interval */
	if (!end_of_region)
		loops_per_timer = MAX(loop / NUM_TIMER_CHECKS_PER_RUN, 200);

#ifdef DEVELOPMENT
	if (finished)
		scancache_check_integrity(btv, scanning.timer);
#endif

	g_timer_destroy(scanning.timer);

#ifdef VALGRIND_PROFILING
	CALLGRIND_STOP_INSTRUMENTATION;
#endif							/* VALGRIND_PROFILING */

#ifdef DUMP_SCANCACHE
	dump_scancache(btv);
#endif
#ifdef DUMP_HIGHLIGHTING
	dump_highlighting(btv);
#endif

	DBG_MSG("cleaned scanning run, finished this run\n");
	return !finished;
}

static GQueue *
get_contextstack_for_found(Tfound *found)
{
	GQueue *retqueue = g_queue_new();
	if (found) {
		Tfoundcontext *tmpfcontext = found->fcontext;
		gint changecounter = found->numcontextchange;
		while (tmpfcontext) {
			if (changecounter >= 0) {
				gint context = tmpfcontext->context;
				g_queue_push_tail(retqueue, GINT_TO_POINTER(context));
			} else {
				changecounter++;
			}
			tmpfcontext = (Tfoundcontext *) tmpfcontext->parentfcontext;
		}
	}
	return retqueue;
}

GQueue *get_contextstack_at_position(BluefishTextView * btv, GtkTextIter * position)
{
	Tfound *found;
	found = get_foundcache_at_offset(btv, gtk_text_iter_get_offset(position), NULL);
	return get_contextstack_for_found(found);
}

void
scan_for_autocomp_prefix(BluefishTextView * btv, GtkTextIter * mstart, GtkTextIter * cursorpos,
						 gint * contextnum)
{
	GtkTextIter iter;
	guint16 pos, newpos;
	GQueue *contextstack;
	/* get the current context */
	iter = *mstart;
	Tfound *found;
	found = get_foundcache_at_offset(btv, gtk_text_iter_get_offset(&iter), NULL);
	contextstack = get_contextstack_for_found(found);
	
	*contextnum = g_queue_get_length(contextstack) ? GPOINTER_TO_INT(g_queue_peek_head(contextstack)) : 1;
	pos = 0;
	DBG_AUTOCOMP("start scanning at offset %d with context %d and position %d\n",
				 gtk_text_iter_get_offset(&iter), *contextnum, pos);
	while (!gtk_text_iter_equal(&iter, cursorpos)) {
		gunichar uc;
		uc = gtk_text_iter_get_char(&iter);
		if (G_UNLIKELY(uc > 128)) {
			/* multibyte characters cannot be matched by the engine. character
			   1 in ascii is "SOH (start of heading)". we need this to support a
			   pattern like [^#]* .  */
			uc = 1;
		}
		DBG_AUTOCOMP("scanning %c\n", uc);
		newpos = get_tablerow(btv->bflang->st,*contextnum,pos).row[uc];
		if (G_UNLIKELY(newpos == 0 || uc == '\0')) {
			DBG_AUTOCOMP("newpos=%d...\n", newpos);
			if (G_UNLIKELY(get_tablerow(btv->bflang->st,*contextnum,pos).match)) {
				if (g_array_index
					(btv->bflang->st->matches, Tpattern,
					 get_tablerow(btv->bflang->st,*contextnum,pos).match).nextcontext < 0) {
					gint num = g_array_index(btv->bflang->st->matches, Tpattern,
											 get_tablerow(btv->bflang->st,*contextnum,pos).match).nextcontext;
					while (num != 0) {
						g_queue_pop_head(contextstack);
						num++;
					}
					*contextnum =
						g_queue_get_length(contextstack) ? GPOINTER_TO_INT(g_queue_peek_head(contextstack)) :
						1;
				} else
					if (g_array_index
						(btv->bflang->st->matches, Tpattern,
						 get_tablerow(btv->bflang->st,*contextnum,pos).match).nextcontext > 0) {
					DBG_AUTOCOMP("previous pos=%d had a match with a context change!\n", pos);
					*contextnum =
						g_array_index(btv->bflang->st->matches, Tpattern,
									  get_tablerow(btv->bflang->st,*contextnum,pos).match).nextcontext;
					g_queue_push_head(contextstack, GINT_TO_POINTER(*contextnum));
				}
			}
			if (G_LIKELY(gtk_text_iter_equal(mstart, &iter))) {
				gtk_text_iter_forward_char(&iter);
			}
			*mstart = iter;
			newpos = 0;
		} else {
			gtk_text_iter_forward_char(&iter);
		}
		pos = newpos;
	}
	g_queue_free(contextstack);
	DBG_AUTOCOMP("scan_for_autocomp_prefix, return mstart at %d, cursor at %d, context %d\n",
				 gtk_text_iter_get_offset(mstart), gtk_text_iter_get_offset(cursorpos), *contextnum);
}

guint
scan_for_identifier_at_position(BluefishTextView * btv, GtkTextIter * mstart, GtkTextIter * position, gint * contextnum, GtkTextIter *so, GtkTextIter *eo)
{
	GtkTextIter iter, end;
	guint16 pos, newpos;
	gboolean retthismatch = FALSE;
	GQueue *contextstack;
	/* get the current context */
	iter = *mstart;
	Tfound *found;
	
	found = get_foundcache_at_offset(btv, gtk_text_iter_get_offset(&iter), NULL);
	contextstack = get_contextstack_for_found(found);
	*contextnum = g_queue_get_length(contextstack) ? GPOINTER_TO_INT(g_queue_peek_head(contextstack)) : 1;
	pos = 0;

	gtk_text_buffer_get_end_iter(gtk_text_view_get_buffer(GTK_TEXT_VIEW(btv)), &end);
	DBG_TOOLTIP("start scanning at offset %d with context %d and position %d\n",
				gtk_text_iter_get_offset(&iter), *contextnum, pos);
	while (!gtk_text_iter_equal(&iter, &end)) {
		gunichar uc;
		uc = gtk_text_iter_get_char(&iter);
		if (G_UNLIKELY(uc > 128)) {
			newpos = 0;
		} else {
			DBG_TOOLTIP("scanning %c\n", uc);
			newpos = get_tablerow(btv->bflang->st,*contextnum,pos).row[uc];
		}
		if (G_UNLIKELY(newpos == 0 || uc == '\0')) {
			gint match = get_tablerow(btv->bflang->st,*contextnum,pos).match;
			DBG_TOOLTIP("newpos=%d...\n", newpos);
			if (G_UNLIKELY(match)) {
				DBG_MSG("found match %d, retthismatch=%d\n",match, retthismatch);
				if (retthismatch) {
					*position = iter;
					g_queue_free(contextstack);
					DBG_TOOLTIP("return TRUE, mstart %d position %d\n", gtk_text_iter_get_offset(mstart),
								gtk_text_iter_get_offset(position));
					if (so && eo) {
						/* fill the start and end positions of the match */
						*eo = iter;
						*so = *mstart;
					}
					return match;
				}
				if (g_array_index(btv->bflang->st->matches, Tpattern,match).nextcontext < 0) {
					gint num = g_array_index(btv->bflang->st->matches, Tpattern,match).nextcontext;
					while (num != 0) {
						g_queue_pop_head(contextstack);
						num++;
					}
					*contextnum =
						g_queue_get_length(contextstack) ? GPOINTER_TO_INT(g_queue_peek_head(contextstack)) :
						1;
					DBG_TOOLTIP("previous pos=%d had a match that popped the context to %d!\n", pos,
								*contextnum);
				} else
					if (g_array_index
						(btv->bflang->st->matches, Tpattern,match).nextcontext > 0) {
					*contextnum =
						g_array_index(btv->bflang->st->matches, Tpattern,match).nextcontext;
					DBG_TOOLTIP("previous pos=%d had a match that pushed the context to %d!\n", pos,
								*contextnum);
					g_queue_push_head(contextstack, GINT_TO_POINTER(*contextnum));
				}
			} else if (retthismatch) {
				g_queue_free(contextstack);
				return FALSE;
			}
			if (gtk_text_iter_equal(mstart, &iter)) {
				gtk_text_iter_forward_char(&iter);
			}
			*mstart = iter;
			newpos = 0;
		} else {
			gtk_text_iter_forward_char(&iter);
		}
		pos = newpos;
		if (gtk_text_iter_equal(&iter, position)) {
			DBG_TOOLTIP("at cursor position..., scanning in context %d, pos %d\n",
						*contextnum, pos);
			if (gtk_text_iter_equal(&iter, mstart)
				|| (pos == 1)) {
				g_queue_free(contextstack);
				return FALSE;
			}
			retthismatch = TRUE;
		}
	}
	g_queue_free(contextstack);
	return FALSE;
}

#ifdef DEVELOPMENT

static void
scancache_check_integrity(BluefishTextView * btv, GTimer *timer) {
	GQueue contexts;
	GQueue blocks;
	GQueue indents;
	GSequenceIter *siter;
	gfloat start;
	guint32 prevfound_o=0;
	guint previndentlevel=NO_INDENT_FOUND;

	start = g_timer_elapsed(timer, NULL);
	g_queue_init(&contexts);
	g_queue_init(&blocks);
	g_queue_init(&indents);
	siter = g_sequence_get_begin_iter(btv->scancache.foundcaches);
	while (siter && !g_sequence_iter_is_end(siter)) {
		Tfound *found = g_sequence_get(siter);
		if (!found)
			break;
		if (found->charoffset_o < 0) {
			g_warning("scancache_check_integrity, found %p has offset < 0\n", found);
			dump_scancache(btv);
			g_assert_not_reached();
		}
		if (found->charoffset_o < prevfound_o) {
			g_warning("scancache_check_integrity, found(%p) has offset %d, the previous found had offset %d, not ordered correctly?!?!!\n",found,found->charoffset_o, prevfound_o);
			dump_scancache(btv);
			g_assert_not_reached();
		} else if (found->charoffset_o == prevfound_o && prevfound_o != 0 && (previndentlevel == NO_INDENT_FOUND && found->indentlevel == NO_INDENT_FOUND)) {
			/* only indentlevel detection can be at the same position as another found, so if they are both not set (for
			the previous and the current found) we found a problem */
			g_warning("scancache_check_integrity, previous found and the next found have offset %d, duplicate!!\n",found->charoffset_o);
			dump_scancache(btv);
			g_assert_not_reached();
		}

		if (found->numcontextchange > 0) {
			/* push context */
			if (found->fcontext->parentfcontext != g_queue_peek_head(&contexts)) {
				if (found->fcontext->parentfcontext == NULL) {
					g_warning("scancache_check_integrity, pushing context at %d:%d on top of non-NULL stack, but parent contexts is NULL!? found at %d\n"
									,found->fcontext->start_o, found->fcontext->end_o,found->charoffset_o);
					dump_scancache(btv);
					g_assert_not_reached();
				} else {
					g_warning("scancache_check_integrity, pushing context at %d:%d, parent contexts at %d:%d do not match! found at %d\n"
									,found->fcontext->start_o, found->fcontext->end_o
									,((Tfoundcontext *)found->fcontext->parentfcontext)->start_o
									,((Tfoundcontext *)found->fcontext->parentfcontext)->end_o,found->charoffset_o);
					dump_scancache(btv);
					g_assert_not_reached();
				}
			}
			g_queue_push_head(&contexts, found->fcontext);

			if (found->fcontext->start_o < prevfound_o || found->fcontext->start_o > found->fcontext->end_o || found->fcontext->end_o < found->charoffset_o) {
					g_warning("scancache_check_integrity, context is at %d:%d, but prevoffset is at %d and charoffset_o is at %d\n"
									,found->fcontext->start_o, found->fcontext->end_o,prevfound_o, found->charoffset_o);
					dump_scancache(btv);
					g_assert_not_reached();
			}

		} else {
			gint i;
			/* check the current context */
			if (found->fcontext != g_queue_peek_head(&contexts)) {
				g_warning("scancache_check_integrity, contexts don't match, found(%p) at %d\n",found,found->charoffset_o);
				dump_scancache(btv);
				g_assert_not_reached();
			}
			i = found->numcontextchange;
			while (i < 0) {
				g_queue_pop_head(&contexts);
				i++;
			}
		}

		if (found->numblockchange > 0) {
			if (found->fblock->parentfblock != g_queue_peek_head(&blocks)) {
				g_warning("scancache_check_integrity, pushing block, parent blocks do not match, found(%p) at %d\n",found,found->charoffset_o);
				dump_scancache(btv);
				g_assert_not_reached();
			}
			g_queue_push_head(&blocks, found->fblock);

			if (found->fblock->start1_o < prevfound_o
					|| found->fblock->end1_o < found->charoffset_o
					|| found->fblock->end1_o < found->fblock->start1_o
					|| found->fblock->start2_o < found->fblock->end1_o
					|| found->fblock->end2_o < found->fblock->start2_o) {
				g_warning("scancache_check_integrity, block is at %d:%d-%d:%d, prevfound_o at %d and charoffset_o at %d\n",
								found->fblock->start1_o,found->fblock->end1_o,found->fblock->start2_o,found->fblock->end2_o,
								prevfound_o,found->charoffset_o);
				dump_scancache(btv);
				g_assert_not_reached();
			}

		} else {
			gint i;
			/* check the current context */
			if (found->fblock != g_queue_peek_head(&blocks)) {
				g_warning("blocks don't match, found(%p) at %d\n",found,found->charoffset_o);
				dump_scancache(btv);
				g_assert_not_reached();
			}
			i = found->numblockchange;
			while (i < 0) {
				g_queue_pop_head(&blocks);
				i++;
			}
		}
		prevfound_o = found->charoffset_o;
		previndentlevel = found->indentlevel;
		siter = g_sequence_iter_next(siter);
	}
	g_queue_clear(&contexts);
	g_queue_clear(&blocks);
	g_print("scancache integrity check done in %3f ms.\n", 1000.0 * (g_timer_elapsed(timer, NULL) - start));
}
#endif /* DEVELOPMENT */

void
cleanup_scanner(BluefishTextView * btv)
{
	GtkTextIter begin, end;
	GSequenceIter *sit1, *sit2;

	gtk_text_buffer_get_bounds(btv->buffer, &begin, &end);
	gtk_text_buffer_remove_all_tags(btv->buffer, &begin, &end);
#ifdef MARKREGION
	markregion_region_done(&btv->scanning, BF_POSITION_UNDEFINED);
#ifdef HAVE_LIBENCHANT
	markregion_region_done(&btv->spellcheck, BF_POSITION_UNDEFINED);
#endif
#endif

	g_sequence_foreach(btv->scancache.foundcaches, found_free_lcb, btv);
	sit1 = g_sequence_get_begin_iter(btv->scancache.foundcaches);
	if (sit1 && !g_sequence_iter_is_end(sit1)) {
		sit2 = g_sequence_get_end_iter(btv->scancache.foundcaches);
		/*g_sequence_foreach_range(sit1,sit2,foundstack_free_lcb,btv); */
		g_sequence_remove_range(sit1, sit2);
	} else {
		/*DBG_SCANNING("cleanup_scanner, no sit1, no cleanup ??\n"); */
	}
#ifdef HL_PROFILING
	g_print("cleanup_scanner, memory scancache %d(%dKb+%dKb) found %d(%dKb) fcontext %d(%dKb) = %dKb\n",
			hl_profiling.found_refcount, (gint) (hl_profiling.found_refcount * sizeof(Tfound) / 1024.0),
			(gint) (hl_profiling.found_refcount * 40 / 1024.0)
			, hl_profiling.fblock_refcount,
			(gint) (hl_profiling.fblock_refcount * sizeof(Tfoundblock) / 1024.0)
			, hl_profiling.fcontext_refcount,
			(gint) (hl_profiling.fcontext_refcount * sizeof(Tfoundcontext) / 1024.0)
			,
			(gint) ((hl_profiling.found_refcount * sizeof(Tfound) +
					 hl_profiling.found_refcount * 5 * sizeof(gpointer) +
					 hl_profiling.fblock_refcount * sizeof(Tfoundblock) +
					 hl_profiling.fcontext_refcount * sizeof(Tfoundcontext)) / 1024.0)
		);

#endif
	bftextview2_identifier_hash_remove_doc(DOCUMENT(btv->doc)->bfwin, btv->doc);

}

void
scancache_destroy(BluefishTextView * btv)
{
	g_sequence_foreach(btv->scancache.foundcaches, found_free_lcb, btv);
	g_sequence_free(btv->scancache.foundcaches);
	btv->scancache.foundcaches = NULL;
}
