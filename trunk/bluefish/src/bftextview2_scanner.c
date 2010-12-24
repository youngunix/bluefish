/* Bluefish HTML Editor
 * bftextview2_scanner.c
 *
 * Copyright (C) 2008,2009,2010 Olivier Sessink
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

/*#define HL_PROFILING*/
/*#define MINIMAL_REFCOUNTING*/
/*#define VALGRIND_PROFILING*/

#ifdef VALGRIND_PROFILING
#include <valgrind/callgrind.h>
#endif

#ifdef HL_PROFILING
#include <unistd.h>
#endif
/* for the design docs see bftextview2.h */
#include "bluefish.h"
#include "bftextview2_scanner.h"
#include "bftextview2_identifier.h"
/* use 
G_SLICE=always-malloc valgrind --tool=memcheck --leak-check=full --num-callers=32 --freelist-vol=100000000 src/bluefish-unstable
to memory-debug this code
*/
#ifdef VALGRIND_PROFILING
#define MAX_CONTINUOUS_SCANNING_INTERVAL 1.0 /* float in seconds */
#else
#define MAX_CONTINUOUS_SCANNING_INTERVAL 0.1 /* float in seconds */
#endif

typedef struct {
	GtkTextIter start;
	GtkTextIter end;
	guint16 patternum;
} Tmatch;

typedef struct {
	GQueue *contextstack;
	GQueue *blockstack;
	GTimer *timer;
	gint16 context;
	guint8 identmode;
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
	gint fstack_refcount;
	gint queue_count;
	gint num_marks;
	guint total_runs;
	guint total_chars;
	guint total_marks;
	guint total_time_ms;
} Thl_profiling;

Thl_profiling hl_profiling = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
#endif

guint loops_per_timer=1000; /* a tunable to avoid asking time too often. this is auto-tuned. */ 

Tfoundstack *get_stackcache_next(BluefishTextView * btv, GSequenceIter ** siter) {
	DBG_MSG("get_stackcache_next, *siter=%p\n",*siter);
	*siter = g_sequence_iter_next(*siter);
	if (*siter && !g_sequence_iter_is_end(*siter)) {
		return g_sequence_get(*siter);
	}
	return NULL;
}

Tfoundstack *get_stackcache_first(BluefishTextView * btv, GSequenceIter ** retsiter) {
	*retsiter = g_sequence_get_begin_iter(btv->scancache.stackcaches);
	if (*retsiter && !g_sequence_iter_is_end(*retsiter)) {
		return g_sequence_get(*retsiter);
	}
	return NULL;
}

static gint stackcache_compare_charoffset_o(gconstpointer a,gconstpointer b,gpointer user_data) {
	return ((Tfoundstack *)a)->charoffset_o - ((Tfoundstack *)b)->charoffset_o;
}

Tfoundstack *get_stackcache_at_offset(BluefishTextView * btv, guint offset, GSequenceIter ** retsiter) {
	GSequenceIter* siter;
	Tfoundstack fakefstack;
	Tfoundstack *fstack=NULL;
	fakefstack.charoffset_o = offset;
	siter = g_sequence_search(btv->scancache.stackcaches,&fakefstack,stackcache_compare_charoffset_o,NULL);
	if (!g_sequence_iter_is_begin(siter)) {
		/* now get the previous position, and get the stack at that position */
		DBG_SCANCACHE("search for offset %d returned iter-position %d (cache length %d)\n",offset,g_sequence_iter_get_position(siter),g_sequence_get_length(btv->scancache.stackcaches));
		siter = g_sequence_iter_prev(siter);
		if (siter && !g_sequence_iter_is_end(siter)) {
			fstack = g_sequence_get(siter);
			if (retsiter)
				*retsiter = siter;
			DBG_SCANCACHE("found nearest stack %p with charoffset_o %d\n",fstack,fstack->charoffset_o);
		} else {
			DBG_SCANCACHE("no siter no stack\n");
		}
	} else if (!g_sequence_iter_is_end(siter)){
		DBG_SCANCACHE("got begin siter\n");
		fstack = g_sequence_get(siter); /* get the first fstack */
		if (retsiter)
			*retsiter = siter;
	}
	return fstack;
}

void foundstack_free_lcb(gpointer data, gpointer btv);

/** 
 * stackcache_update_offsets
 * 
 * startpos is the lowest position 
 *  so on insert it is the point _after_ which the insert will be (and offset is a positive number) 
 *  on delete it is the point _after_ which the delete area starts (and offset is a negative number)
*/
void stackcache_update_offsets(BluefishTextView * btv, guint startpos, gint offset) {
	Tfoundstack *fstack;
	GSequenceIter *siter;
	if (offset==0)
		return;
	DBG_SCANCACHE("stackcache_update_offsets, update offset %d starting at startpos %d\n",offset,startpos);
	fstack = get_stackcache_at_offset(btv, startpos, &siter);
	if (offset < 0) {
		while (fstack && fstack->charoffset_o < startpos-offset) {
			if (fstack->charoffset_o > startpos) {
				GSequenceIter *tmpsiter = siter;
				Tfoundstack *tmpfstack=fstack;
				fstack = get_stackcache_next(btv, &siter);
				DBG_SCANCACHE("stackcache_update_offsets, remove fstack %p with charoffset_o=%d\n",fstack,fstack->charoffset_o);
				g_sequence_remove(tmpsiter);
				foundstack_free_lcb(tmpfstack, btv);
			} else {
				fstack = get_stackcache_next(btv, &siter);
			}
		}		
	}
	if (fstack) {
		GList *tmplist;
		DBG_SCANCACHE("stackcache_update_offsets, handle first fstack %p on offset %d complete stack\n",fstack, fstack->charoffset_o);
		/* for the first fstack, we have to update the end-offsets for all contexts/blocks on the stack */
		for (tmplist=fstack->contextstack->head;tmplist;tmplist=tmplist->next) {
			Tfoundcontext *fcontext=tmplist->data;
			if (fcontext->end_o != BF2_OFFSET_UNDEFINED)
				fcontext->end_o += offset;
		}
		for (tmplist=fstack->blockstack->head;tmplist;tmplist=tmplist->next) {
			Tfoundblock *fblock=tmplist->data;
			if (fblock->start2_o != BF2_OFFSET_UNDEFINED)
				fblock->start2_o += offset;
			if (fblock->end2_o != BF2_OFFSET_UNDEFINED)
				fblock->end2_o += offset;
		}
		DBG_SCANCACHE("stackcache_update_offsets, handled first fstack %p, requesting next\n",fstack);
		/*this offset is *before* 'position' fstack->charoffset_o += offset;*/
		fstack = get_stackcache_next(btv, &siter);
	}
	/* DO WE actually have to update them all??? sometimes they will be deleted anyway.. can we do this smart?? */
	while (fstack) {
		DBG_SCANCACHE("stackcache_update_offsets, about to update fstack %p with charoffset %d\n",fstack,fstack->charoffset_o);
		/* for all further fstacks, we only handle the pushedblock and pushedcontext */
		if (fstack->pushedcontext) {
			if (fstack->pushedcontext->start_o != BF2_OFFSET_UNDEFINED)
				fstack->pushedcontext->start_o += offset;
			if (fstack->pushedcontext->end_o != BF2_OFFSET_UNDEFINED)
				fstack->pushedcontext->end_o += offset;
		}
		if (fstack->pushedblock) {
			if (fstack->pushedblock->start1_o != BF2_OFFSET_UNDEFINED)
				fstack->pushedblock->start1_o += offset;
			if (fstack->pushedblock->end1_o != BF2_OFFSET_UNDEFINED)
				fstack->pushedblock->end1_o += offset;
			if (fstack->pushedblock->start2_o != BF2_OFFSET_UNDEFINED)
				fstack->pushedblock->start2_o += offset;
			if (fstack->pushedblock->end2_o != BF2_OFFSET_UNDEFINED)
				fstack->pushedblock->end2_o += offset;
		}
		/*g_print("startpos=%d, offset=%d, fstack=%p, update charoffset_o from %d to %d\n",startpos,offset, fstack,fstack->charoffset_o,fstack->charoffset_o+offset);*/
		fstack->charoffset_o += offset;
		fstack = get_stackcache_next(btv, &siter);
	}
}

#define foundblock_ref(fblock) ((Tfoundblock *)fblock)->refcount++

static void foundblock_unref(Tfoundblock *fblock, GtkTextBuffer *buffer) {
	if (G_UNLIKELY(fblock->refcount <= 0)) 
		g_warning("fblock %p is unref'ed but should not exist anymore!\n",fblock);
	DBG_FBLOCKREFCOUNT("unref fblock %p;",fblock);
	fblock->refcount--;
	DBG_FBLOCKREFCOUNT(" refcount is %d\n",fblock->refcount);
	if (fblock->refcount == 0) {
		/* remove marks */
		DBG_FBLOCKREFCOUNT("UNREF start cleanup foundblock %p\n",fblock);
		g_slice_free(Tfoundblock,fblock);
#ifdef HL_PROFILING
		hl_profiling.fblock_refcount--;
#endif
	}
}
#ifndef MINIMAL_REFCOUNTING
static void foundblock_foreach_unref_lcb(gpointer data,gpointer user_data) {
	if (G_LIKELY(data))
		foundblock_unref(data,gtk_text_view_get_buffer(user_data));
}

static void foundblock_foreach_ref_lcb(gpointer data,gpointer user_data) {
	if (G_LIKELY(data)) {
		foundblock_ref(data);
		DBG_FBLOCKREFCOUNT("foreach ref fblock %p; refcount is %d\n",((Tfoundblock *)data),((Tfoundblock *)data)->refcount);
	}
}
#endif

#define foundcontext_ref(fcontext) ((Tfoundcontext *)fcontext)->refcount++;

static void foundcontext_unref(Tfoundcontext *fcontext, GtkTextBuffer *buffer) {
	if (fcontext->refcount <= 0) 
		g_warning("fcontext %p is unref'ed but should not exist anymore!\n",fcontext);
	fcontext->refcount--;
	DBG_FCONTEXTREFCOUNT("unref: refcount for fcontext %p is %d\n",fcontext,fcontext->refcount);
	if (fcontext->refcount == 0) {
		/* remove marks */
		DBG_FCONTEXTREFCOUNT("unref: start cleanup foundcontext %p\n",fcontext);
		g_slice_free(Tfoundcontext,fcontext);
#ifdef HL_PROFILING
		hl_profiling.fcontext_refcount--;
#endif
	}
}
#ifndef MINIMAL_REFCOUNTING
static void foundcontext_foreach_unref_lcb(gpointer data,gpointer user_data) {
	if (G_LIKELY(data))
		foundcontext_unref(data,gtk_text_view_get_buffer(user_data));
}

static void foundcontext_foreach_ref_lcb(gpointer data,gpointer user_data) {
	if (G_LIKELY(data)) {
		foundcontext_ref(data);
		DBG_FCONTEXTREFCOUNT("foreach ref: refcount for fcontext %p is %d\n",data,((Tfoundcontext *)data)->refcount);
	}
}
#endif /* MINIMAL_REFCOUNTING */

void foundstack_free_lcb(gpointer data, gpointer btv) {
	Tfoundstack *fstack = data;
#ifndef MINIMAL_REFCOUNTING
	/* unref all contexts and blocks */
	DBG_FBLOCKREFCOUNT("foundstack_free_lcb, removing fstack %p, calling foundblock_foreach_unref_lcb\n",fstack);
	g_queue_foreach(fstack->blockstack,foundblock_foreach_unref_lcb,btv);
	DBG_FCONTEXTREFCOUNT("foundstack_free_lcb, calling foreach unref for contextstack with len %d\n",g_queue_get_length(fstack->contextstack));
	g_queue_foreach(fstack->contextstack,foundcontext_foreach_unref_lcb,btv);
	
	if (fstack->poppedblock)
		foundblock_unref(fstack->poppedblock, gtk_text_view_get_buffer(GTK_TEXT_VIEW(btv)));
	if (fstack->poppedcontext) {
		DBG_FCONTEXTREFCOUNT("foundstack_free_lcb, calling unref for poppedcontext on %p\n",fstack->poppedcontext);
		foundcontext_unref(fstack->poppedcontext, gtk_text_view_get_buffer(GTK_TEXT_VIEW(btv)));
	}
#endif
	if (fstack->pushedblock) {
		DBG_FCONTEXTREFCOUNT("foundstack_free_lcb, calling unref for pushedblock on %p\n",fstack->pushedblock);
		foundblock_unref(fstack->pushedblock, gtk_text_view_get_buffer(GTK_TEXT_VIEW(btv)));
	}
	if (fstack->pushedcontext) {
		DBG_FCONTEXTREFCOUNT("foundstack_free_lcb, calling unref for pushedcontext on %p\n",fstack->pushedcontext);
		foundcontext_unref(fstack->pushedcontext, gtk_text_view_get_buffer(GTK_TEXT_VIEW(btv)));
	}
#ifdef HL_PROFILING
	hl_profiling.fstack_refcount--;
	hl_profiling.queue_count -= (g_queue_get_length(fstack->blockstack) + g_queue_get_length(fstack->contextstack));
#endif
	g_queue_free(fstack->blockstack);
	g_queue_free(fstack->contextstack);
	g_slice_free(Tfoundstack,fstack);
}

static inline void add_to_scancache(BluefishTextView * btv,GtkTextBuffer *buffer,Tscanning *scanning, guint charoffset_o, Tfoundblock *fblock, Tfoundcontext *fcontext) {
	Tfoundstack *fstack;

	fstack = g_slice_new0(Tfoundstack);
#ifdef HL_PROFILING
	hl_profiling.fstack_refcount++;
	hl_profiling.queue_count += g_queue_get_length(scanning->contextstack) + g_queue_get_length(scanning->blockstack);
#endif
	fstack->contextstack = g_queue_copy(scanning->contextstack);
	fstack->blockstack = g_queue_copy(scanning->blockstack);
	DBG_FBLOCKREFCOUNT("creating new fstack %p with fblock=%p\n",fstack,fblock);
#ifndef MINIMAL_REFCOUNTING
	g_queue_foreach(fstack->blockstack,foundblock_foreach_ref_lcb,NULL);
	g_queue_foreach(fstack->contextstack,foundcontext_foreach_ref_lcb,NULL);
#endif

	if (fblock) {
#ifndef MINIMAL_REFCOUNTING
		foundblock_ref(fblock);
#endif
		DBG_FBLOCKREFCOUNT("ref fblock %p for pushed/popped, after ref refcount=%d\n",fblock,fblock->refcount);
		if (fblock == g_queue_peek_head(fstack->blockstack)) {
			fstack->pushedblock = fblock;
#ifdef MINIMAL_REFCOUNTING
			foundblock_ref(fblock);
#endif		
		} else {
			fstack->poppedblock = fblock;
		}
	}
	if (fcontext) {
#ifndef MINIMAL_REFCOUNTING
		foundcontext_ref(fcontext);
#endif
		if (fcontext == g_queue_peek_head(fstack->contextstack)) {
			fstack->pushedcontext = fcontext;
#ifdef MINIMAL_REFCOUNTING /* with minimal refcounting we *do* refcount a pushedcontext, not a poppedcontext */
			foundcontext_ref(fcontext);
			DBG_FCONTEXTREFCOUNT("ref pushedcontext %p, refcount=%d\n",fcontext, fcontext->refcount);
#endif		
		} else
			fstack->poppedcontext = fcontext;
	}
	fstack->charoffset_o = charoffset_o;
	DBG_SCANCACHE("add_to_scancache, put fstack %p in the cache at charoffset_o %d with pushedblock %p poppedblock %p\n",fstack,fstack->charoffset_o,fstack->pushedblock,fstack->poppedblock);
	g_sequence_insert_sorted(btv->scancache.stackcaches,fstack,stackcache_compare_charoffset_o,NULL);
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

static inline Tfoundblock *found_start_of_block(BluefishTextView * btv,GtkTextBuffer *buffer, const Tmatch match, Tscanning *scanning) {
	if (G_UNLIKELY(scanning->blockstack->length > 100)) {
		/* if a file has thousands of blockstarts this results in thousands of Tfoundblock structures, but 
		worse: also thousands of copies of the blockstack in the scancache --> 1000 * 0.5 * 1000 queue elements.
		to avoid this we return NULL here if the blockstack is > 100. If we return NULL here
		there will be no addition to the scancache either.  */
		DBG_BLOCKMATCH("blockstack length > 100 ** IGNORING BLOCK **\n");
		return NULL;
	} else {
		Tfoundblock *fblock;
		DBG_BLOCKMATCH("put block for pattern %d (%s) on blockstack\n",match.patternum,g_array_index(btv->bflang->st->matches,Tpattern,match.patternum).pattern);
#ifdef HL_PROFILING
		hl_profiling.numblockstart++;
#endif
		fblock = g_slice_new0(Tfoundblock);
#ifdef MINIMAL_REFCOUNTING
		fblock->refcount=1;
#else
		fblock->refcount=2; /* one ref returned to the calling function, one ref for scanning->blockstack */
#endif
		DBG_FBLOCKREFCOUNT("created new fblock with refcount %d at %p\n",fblock->refcount, fblock);
#ifdef HL_PROFILING
		hl_profiling.fblock_refcount++;
#endif
		/* TODO: My latest callgrind runs show that creating GtkTextMarks	is one of the slowest parts of the engine.
		having two marks for the start and two marks for the end leaves room for improvement. What if we would
		just store the start of the start in a GtkTextMark and the offset to the end of the start as integer? (same for the end)
		The only function that really needs to work in a very different way is bftextview2_get_block_at_iter() 
		*/
		fblock->start1_o = gtk_text_iter_get_offset(&match.start);
		fblock->end1_o = gtk_text_iter_get_offset(&match.end);
		/*g_print("found blockstart with start_1 %d end1 %d\n",fblock->start1_o,fblock->end1_o);*/
		fblock->start2_o = BF2_OFFSET_UNDEFINED;
		fblock->end2_o = BF2_OFFSET_UNDEFINED;
		fblock->patternum = match.patternum;
#ifdef HL_PROFILING
		hl_profiling.queue_count++;
#endif
		g_queue_push_head(scanning->blockstack,fblock);
		/*print_blockstack(btv,scanning);*/
		return fblock;
	}	
}

static inline Tfoundblock *found_end_of_block(BluefishTextView * btv,GtkTextBuffer *buffer, const Tmatch match, Tscanning *scanning, Tpattern *pat) {
	Tfoundblock *fblock=NULL;
	DBG_BLOCKMATCH("found end of block with blockstartpattern %d\n",pat->blockstartpattern);
#ifdef HL_PROFILING
	hl_profiling.numblockend++;
#endif
	do {
#ifndef MINIMAL_REFCOUNTING
		if (fblock) {
			foundblock_unref(fblock, buffer);
		}
#endif
		fblock = g_queue_pop_head(scanning->blockstack);
#ifdef HL_PROFILING
		hl_profiling.queue_count--;
#endif
		if (G_LIKELY(fblock)) {
			/* we should unref the fblock here, because it is popped from scanning->blockstack, but we want to add a reference too
			because we return a reference to the calling function */
			DBG_BLOCKMATCH("popped block for pattern %d (%s) from blockstack\n",fblock->patternum, g_array_index(btv->bflang->st->matches,Tpattern,fblock->patternum).pattern);
		}
		/* if patternum == -1 this means we should end the last started block
		else we pop until we have the right startpattern */
	} while (fblock && fblock->patternum != pat->blockstartpattern && pat->blockstartpattern != -1);
	/*print_blockstack(btv,scanning);*/
	if (G_LIKELY(fblock)) {
		GtkTextIter iter;
		DBG_BLOCKMATCH("found the matching start of the block\n");
		/* TODO: see comments in start_of_block how to reduce the number of GtkTextMark's */
		fblock->start2_o = gtk_text_iter_get_offset(&match.start);
		fblock->end2_o = gtk_text_iter_get_offset(&match.end);
		gtk_text_buffer_get_iter_at_offset(buffer,&iter,fblock->end1_o);
		if (pat->blocktag) {
			gtk_text_buffer_apply_tag(buffer,pat->blocktag, &iter, &match.start);
		}
		if ((gtk_text_iter_get_line(&iter)+1) < gtk_text_iter_get_line(&match.start)) {
			fblock->foldable = TRUE;
		}
#ifdef MINIMAL_REFCOUNTING
		fblock->refcount++;
#endif		
		return fblock; /* this fblock has a reference, see comment above */
	} else {
		DBG_BLOCKMATCH("no matching start-of-block found\n");
	}
	return NULL;
}

static inline Tfoundcontext *found_context_change(BluefishTextView * btv,GtkTextBuffer *buffer, const Tmatch match, Tscanning *scanning, Tpattern *pat) {
	Tfoundcontext *fcontext=NULL;
	/* check if we change up or down the stack */
	if (pat->nextcontext < 0) {
		gint num = -1 * pat->nextcontext;
#ifdef HL_PROFILING
		hl_profiling.numcontextend++;
#endif
		/* pop, but don't pop if there is nothing to pop (because of an error in the language file) */
		while (num > 0 && scanning->contextstack->head) {
#ifndef MINIMAL_REFCOUNTING
			if (fcontext) {
				DBG_FCONTEXTREFCOUNT("pop multiple, refcount not returnedto calling function, unref!\n");
				foundcontext_unref(fcontext, buffer);
			} 
#endif
#ifdef HL_PROFILING
			hl_profiling.queue_count--;
#endif
			fcontext = g_queue_pop_head(scanning->contextstack);
			DBG_SCANNING("popped %p, stack len now %d\n",fcontext,g_queue_get_length(scanning->contextstack));
			DBG_SCANNING("found_context_change, popped context %d from the stack, stack len %d\n",fcontext->context,g_queue_get_length(scanning->contextstack));
			fcontext->end_o = gtk_text_iter_get_offset(&match.start);
			if (g_array_index(btv->bflang->st->contexts,Tcontext,fcontext->context).contexttag) {
				GtkTextIter iter;
				gtk_text_buffer_get_iter_at_offset(buffer,&iter,fcontext->start_o);
				gtk_text_buffer_apply_tag(buffer,g_array_index(btv->bflang->st->contexts,Tcontext,fcontext->context).contexttag, &iter, &match.start);
			}
			num--;
		}
#ifdef MINIMAL_REFCOUNTING
		fcontext->refcount++;
#endif		
		return fcontext; /* this functions returns a reference to the calling function */
	} else {
#ifdef HL_PROFILING
		hl_profiling.numcontextstart++;
#endif
		fcontext = g_slice_new0(Tfoundcontext);
#ifdef MINIMAL_REFCOUNTING
		fcontext->refcount=1; /* one for the calling function */
#else
		fcontext->refcount=2; /* one reference for the calling function, once reference forscanning->contextstack  */
#endif
#ifdef HL_PROFILING
		hl_profiling.fcontext_refcount++;
#endif
		fcontext->start_o = gtk_text_iter_get_offset(&match.end);
		fcontext->end_o = BF2_OFFSET_UNDEFINED;
		fcontext->context = pat->nextcontext;
#ifdef HL_PROFILING
		hl_profiling.queue_count++;
#endif
		g_queue_push_head(scanning->contextstack, fcontext);
	
		DBG_FCONTEXTREFCOUNT("refcount for NEW fcontext %p is %d\n",fcontext,fcontext->refcount);
		DBG_SCANNING("found_context_change, pushed nextcontext %d onto the stack, stack len %d\n",pat->nextcontext,g_queue_get_length(scanning->contextstack));
		return fcontext;
	}
}

static inline int found_match(BluefishTextView * btv, const Tmatch match, Tscanning *scanning)
{
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(btv));
	Tfoundblock *fblock=NULL;
	Tfoundcontext *fcontext=NULL;
	Tpattern pat = g_array_index(btv->bflang->st->matches,Tpattern, match.patternum);
	DBG_SCANNING("found_match for pattern %d %s at charoffset %d, starts_block=%d,ends_block=%d, nextcontext=%d (current=%d)\n",match.patternum,pat.pattern, gtk_text_iter_get_offset(&match.start),pat.starts_block,pat.ends_block,pat.nextcontext,scanning->context);
/*	DBG_MSG("pattern no. %d (%s) matches (%d:%d) --> nextcontext=%d\n", match.patternum, scantable.matches[match.patternum].message,
			gtk_text_iter_get_offset(&match.start), gtk_text_iter_get_offset(&match.end), scantable.matches[match.patternum].nextcontext);*/
#ifdef IDENTSTORING
	scanning->identmode = pat.identmode;
#endif /* IDENTSTORING */

	if (pat.selftag) {
		DBG_SCANNING("apply tag %p from %d to %d\n",pat.selftag,gtk_text_iter_get_offset(&match.start),gtk_text_iter_get_offset(&match.end));
		gtk_text_buffer_apply_tag(buffer,pat.selftag, &match.start, &match.end);
	}

	if (pat.starts_block) {
		fblock = found_start_of_block(btv, buffer, match, scanning);
	}
	if (pat.ends_block) {
		fblock = found_end_of_block(btv, buffer, match, scanning, &pat);
	}

	if (pat.nextcontext != 0 && pat.nextcontext != scanning->context) {
		fcontext = found_context_change(btv, buffer, match, scanning, &pat);
	}
	if (fblock || fcontext) {
		add_to_scancache(btv,buffer,scanning, gtk_text_iter_get_offset(&match.end), fblock,fcontext);
/*#ifndef MINIMAL_REFCOUNTING*/

		/* both found_start_of_block and found_end_of_block 
		return a fblock with a refcount for the calling function, so we should unref once
		we are done */
		if (fblock) {
			foundblock_unref(fblock,buffer);
		}
		if (fcontext) {
			DBG_FCONTEXTREFCOUNT("found_match, unref the calling function refcount for fcontext %p\n",fcontext);
			foundcontext_unref(fcontext, buffer); 
		}
/*#endif*/
	}
	if (pat.nextcontext < 0) {
		if (g_queue_get_length(scanning->contextstack)) {
			fcontext = g_queue_peek_head(scanning->contextstack);
			DBG_SCANNING("new context %d\n",fcontext->context);
			return fcontext->context;
		}
		DBG_SCANNING("return context 0\n");
		return 0;
	}
#ifdef HL_PROFILING
	if (g_queue_get_length(scanning->blockstack) > hl_profiling.longest_blockstack) 
		hl_profiling.longest_blockstack = g_queue_get_length(scanning->blockstack);
	if (g_queue_get_length(scanning->contextstack) > hl_profiling.longest_contextstack) 
		hl_profiling.longest_contextstack = g_queue_get_length(scanning->contextstack);

#endif
	if (pat.nextcontext == 0)
		return scanning->context;
	else
		return pat.nextcontext;
}

static gboolean bftextview2_find_region2scan(BluefishTextView * btv, GtkTextBuffer *buffer, GtkTextIter *start, GtkTextIter *end) {
	/* first find a region that needs scanning */
	gtk_text_buffer_get_start_iter(buffer, start);
	if (!gtk_text_iter_begins_tag(start,btv->needscanning) ) {
		if (!gtk_text_iter_forward_to_tag_toggle(start,btv->needscanning)) {
			/* nothing to scan */
			DBG_DELAYSCANNING("nothing to scan..\n");
			return FALSE;
		}
	}
	DBG_SCANNING("first position that toggles needscanning is at %d\n",gtk_text_iter_get_offset(start));
	/* find the end of the region */
	*end = *start;
	gtk_text_iter_forward_char(end);
	if (!gtk_text_iter_ends_tag(end,btv->needscanning)) {
		if (!gtk_text_iter_forward_to_tag_toggle(end,btv->needscanning)) {
			DBG_MSG("BUG: we should never get here\n");
			return FALSE;
		}
	}
	/* now move start to the beginning of the line and end to the end of the line */
	gtk_text_iter_set_line_offset(start,0);
	DBG_SCANNING("set startposition to beginning of line, offset is now %d\n",gtk_text_iter_get_offset(start));
	gtk_text_iter_forward_to_line_end(end);
	gtk_text_iter_forward_char(end);
	return TRUE;
}

static void foundblock_foreach_clear_end_lcb(gpointer data,gpointer user_data) {
	if (G_LIKELY(data)) {
		((Tfoundblock *)data)->start2_o = BF2_OFFSET_UNDEFINED;
		((Tfoundblock *)data)->end2_o = BF2_OFFSET_UNDEFINED;
		((Tfoundblock *)data)->foldable = FALSE;
	}
}

static void foundcontext_foreach_clear_end_lcb(gpointer data,gpointer user_data) {
	if (G_LIKELY(data))
		((Tfoundcontext *)data)->end_o = BF2_OFFSET_UNDEFINED;
}

static void reconstruct_stack(BluefishTextView * btv, GtkTextBuffer *buffer, GtkTextIter *position, Tscanning *scanning) {
	Tfoundstack *fstack;
	DBG_SCANNING("reconstruct_stack at position %d\n",gtk_text_iter_get_offset(position));
	fstack = get_stackcache_at_offset(btv, gtk_text_iter_get_offset(position), NULL);
	DBG_SCANCACHE("reconstruct_stack, got fstack %p with charoffset_o=%d to reconstruct stack at position %d\n",fstack,fstack->charoffset_o,gtk_text_iter_get_offset(position));
	if (G_LIKELY(fstack)) {
		Tfoundcontext *fcontext;
#ifdef HL_PROFILING
		hl_profiling.queue_count += g_queue_get_length(fstack->contextstack) + g_queue_get_length(fstack->blockstack);
#endif
		scanning->contextstack = g_queue_copy(fstack->contextstack);
		fcontext = g_queue_peek_head(scanning->contextstack);
		if (fcontext)
			scanning->context = fcontext->context;
		scanning->blockstack = g_queue_copy(fstack->blockstack);
#ifndef MINIMAL_REFCOUNTING
		g_queue_foreach(scanning->blockstack,foundblock_foreach_ref_lcb,NULL);
		g_queue_foreach(scanning->contextstack,foundcontext_foreach_ref_lcb,NULL);
#endif
		g_queue_foreach(scanning->blockstack,foundblock_foreach_clear_end_lcb,buffer);
		g_queue_foreach(scanning->contextstack,foundcontext_foreach_clear_end_lcb,buffer);

		DBG_SCANNING("stack from the cache, contextstack has len %d, blockstack has len %d, context=%d\n",g_queue_get_length(scanning->contextstack),g_queue_get_length(scanning->blockstack),scanning->context);
	} else {
		DBG_SCANNING("empty stack\n");
		scanning->contextstack = g_queue_new();
		scanning->blockstack = g_queue_new();
	}
}

/*
static void remove_old_matches_at_iter(BluefishTextView *btv, GtkTextBuffer *buffer, GtkTextIter *iter) {
	GSList *toggles, *tmplist;
	/ * remove any toggled tags * /
	toggles = tmplist = gtk_text_iter_get_toggled_tags(iter,FALSE);
	while (tmplist) {
		GtkTextIter tmpit;
		tmpit = *iter;
		gtk_text_iter_forward_to_tag_toggle(&tmpit,tmplist->data);
		DBG_MSG("%s:%d, removing tag %p from %d to %d\n",__FILE__,__LINE__,tmplist->data,gtk_text_iter_get_offset(iter),gtk_text_iter_get_offset(&tmpit));
		gtk_text_buffer_remove_tag(buffer,tmplist->data,iter,&tmpit);
		tmplist = g_slist_next(tmplist);
	}
	g_slist_free(toggles);
	/ * TODO see if there are any old blockstack or context changes * /

}*/
static void remove_old_highlighting(BluefishTextView *btv, GtkTextBuffer *buffer, GtkTextIter *start, GtkTextIter *end) {
	GList *tmplist = g_list_first(btv->bflang->tags);
	while (tmplist) {
		gtk_text_buffer_remove_tag(buffer, (GtkTextTag *)tmplist->data, start, end);
		tmplist = g_list_next(tmplist);
	}
}

static void remove_old_scan_results(BluefishTextView *btv, GtkTextBuffer *buffer, GtkTextIter *fromhere) {
	GtkTextIter end;
	GSequenceIter *sit1,*sit2;
	Tfoundstack fakefstack;

	gtk_text_buffer_get_end_iter(buffer,&end);
	DBG_SCANCACHE("remove_old_scan_results: remove tags from charoffset %d to %d\n",gtk_text_iter_get_offset(fromhere),gtk_text_iter_get_offset(&end));
	remove_old_highlighting(btv, buffer, fromhere, &end);
	fakefstack.charoffset_o = gtk_text_iter_get_offset(fromhere);
	sit1 = g_sequence_search(btv->scancache.stackcaches,&fakefstack,stackcache_compare_charoffset_o,NULL);
	if (sit1 && !g_sequence_iter_is_end(sit1)) {
		sit2 = g_sequence_get_end_iter(btv->scancache.stackcaches);
		DBG_SCANCACHE("sit1=%p, sit2=%p\n",sit1,sit2);
		DBG_SCANCACHE("remove_old_scan_results: remove stackcache entries %d to %d\n",g_sequence_iter_get_position(sit1),g_sequence_iter_get_position(sit2));
		g_sequence_foreach_range(sit1,sit2,foundstack_free_lcb,btv);
		g_sequence_remove_range(sit1,sit2);
	} else{
		DBG_SCANCACHE("no sit1, no cleanup ??\n");
	}
}

/* if visible_end is set (not NULL) we will scan only the visible area and nothing else.
this can be used to delay scanning everything until the editor is idle for several milliseconds */
gboolean bftextview2_run_scanner(BluefishTextView * btv, GtkTextIter *visible_end)
{
	GtkTextBuffer *buffer;
	GtkTextIter start, end, iter, orig_end;
	GtkTextIter mstart;
	/*GArray *matchstack;*/
	Tscanning scanning;
	guint pos = 0, newpos;
	gboolean normal_run=TRUE, last_character_run=FALSE;
	gint loop=0;
#ifdef IDENTSTORING
	GtkTextIter itcursor;
#endif
#ifdef HL_PROFILING
	guint startpos;
	gdouble stage1=0;
	gdouble stage2;
	gdouble stage3;
	gdouble stage4;
	hl_profiling.longest_contextstack=0;
	hl_profiling.longest_blockstack=0;
	hl_profiling.numcontextstart=0;
	hl_profiling.numcontextend=0;
	hl_profiling.numblockstart=0;
	hl_profiling.numblockend=0;
	hl_profiling.numchars=0;
	hl_profiling.numloops=0;
#endif

	scanning.context = 1;
#ifdef IDENTSTORING
	scanning.identmode = 0;
#endif /* IDENTSTORING */

	DBG_MSG("bftextview2_run_scanner for btv %p..\n",btv);
	if (!btv->bflang->st) {
		DBG_MSG("no scantable, nothing to scan, returning...\n");
		return FALSE;
	}
#ifdef VALGRIND_PROFILING
	CALLGRIND_START_INSTRUMENTATION;
#endif /* VALGRIND_PROFILING */	
	buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(btv));

	if (!bftextview2_find_region2scan(btv, buffer, &start, &end)) {
		DBG_MSG("nothing to scan here.. return FALSE\n");
#ifdef VALGRIND_PROFILING
		CALLGRIND_STOP_INSTRUMENTATION;
#endif /* VALGRIND_PROFILING */
		return FALSE;
	}
	/* start timer */
	scanning.timer = g_timer_new();

	orig_end = end;
	if (visible_end) {
		/* check such that we only scan up to vend */
		if (gtk_text_iter_compare(&start,visible_end)>0) {
			DBG_DELAYSCANNING("start of region that needs scanning is beyond visible_end, return TRUE\n");
			g_timer_destroy(scanning.timer);
#ifdef VALGRIND_PROFILING
			CALLGRIND_STOP_INSTRUMENTATION;
#endif /* VALGRIND_PROFILING */
			return TRUE;
		}
		if (gtk_text_iter_compare(&end,visible_end)>0) {
			DBG_DELAYSCANNING("end of region that needs scanning (%d) is beyond visible_end (%d), reset end\n",gtk_text_iter_get_offset(&end),gtk_text_iter_get_offset(visible_end));
			end = *visible_end;
		}

	}

	DBG_SCANNING("scanning from %d to %d\n",gtk_text_iter_get_offset(&start),gtk_text_iter_get_offset(&end));
#ifdef HL_PROFILING
	startpos = gtk_text_iter_get_offset(&start);
	stage1 = g_timer_elapsed(scanning.timer,NULL);
#endif
	iter = mstart = start;
	if (gtk_text_iter_is_start(&start)) {
		scanning.contextstack = g_queue_new();
		scanning.blockstack = g_queue_new();
		/*siter = g_sequence_iter_first(btv->scancache.stackcaches);*/
	} else {
		/* reconstruct the context stack and the block stack */
		reconstruct_stack(btv, buffer, &iter, &scanning);
		pos = g_array_index(btv->bflang->st->contexts,Tcontext,scanning.context).startstate;
		DBG_SCANNING("reconstructed stacks, context=%d, startstate=%d\n",scanning.context,pos);
	}
#ifdef HL_PROFILING
	stage2= g_timer_elapsed(scanning.timer,NULL);
#endif
	/* TODO: when rescanning text that has been scanned before we need to remove
	invalid tags and blocks. right now we remove all, but most are likely
	still valid. 
	This function takes a lot of time!!!!!!!!!! */
	if (btv->needremovetags) {
		remove_old_scan_results(btv, buffer, &start);
		btv->needremovetags = FALSE;
	}
	/* because we remove all to the end we have to rescan to the end (I know this
	is stupid, should become smarter in the future )*/
#ifdef HL_PROFILING
	stage3 = g_timer_elapsed(scanning.timer,NULL);
#endif
	if (!visible_end)
		gtk_text_iter_forward_to_end(&end);
	else
		end = *visible_end;
#ifdef IDENTSTORING
	gtk_text_buffer_get_iter_at_mark(buffer, &itcursor, gtk_text_buffer_get_insert(buffer));
#endif
	do {
		gunichar uc;
		loop++;
#ifdef HL_PROFILING
		hl_profiling.numloops++;
#endif
		if (G_UNLIKELY(last_character_run)) {
			uc = '\0';
		} else {
			uc = gtk_text_iter_get_char(&iter);
			if (uc > 128) {
				/* multibyte characters cannot be matched by the engine. character
				1 in ascii is "SOH (start of heading)". we need this to support a
				pattern like [^#]* .  */
				uc = 1;
			}
		}
		DBG_SCANNING("scanning %d %c in pos %d..",gtk_text_iter_get_offset(&iter),uc,pos);
		newpos = g_array_index(btv->bflang->st->table, Ttablerow, pos).row[uc];
		DBG_SCANNING(" got newpos %d\n",newpos);
		if (G_UNLIKELY(newpos == 0 || uc == '\0')) {
			if (G_UNLIKELY(g_array_index(btv->bflang->st->table,Ttablerow, pos).match)) {
				Tmatch match;
				match.patternum = g_array_index(btv->bflang->st->table,Ttablerow, pos).match;
				match.start = mstart;
				match.end = iter;
				DBG_SCANNING("we have a match from pos %d to %d\n", gtk_text_iter_get_offset(&match.start),gtk_text_iter_get_offset(&match.end));
				scanning.context = found_match(btv, match,&scanning);
				DBG_SCANNING("after match context=%d\n",scanning.context);
			}
#ifdef IDENTSTORING
			else if (G_UNLIKELY(scanning.identmode != 0 && pos == g_array_index(btv->bflang->st->contexts,Tcontext,scanning.context).identstate)) {
				/* ignore if the cursor is within the range, because it could be that the user is still typing the name */
				if (G_LIKELY(!gtk_text_iter_in_range(&itcursor, &mstart, &iter) 
								&& !gtk_text_iter_equal(&itcursor, &mstart) 
								&& !gtk_text_iter_equal(&itcursor, &iter))) {
					found_identifier(btv, &mstart, &iter, scanning.context, scanning.identmode);
					scanning.identmode = 0;
				}
			}
#endif /* IDENTSTORING */
			if (G_LIKELY(gtk_text_iter_equal(&mstart,&iter) && !last_character_run)) {
				gtk_text_iter_forward_char(&iter);
#ifdef HL_PROFILING
				hl_profiling.numchars++;
#endif
			}
			mstart = iter;
			newpos = g_array_index(btv->bflang->st->contexts,Tcontext,scanning.context).startstate;
		} else if (G_LIKELY(!last_character_run)){
			gtk_text_iter_forward_char(&iter);
#ifdef HL_PROFILING
			hl_profiling.numchars++;
#endif
		}
		pos = newpos;
		normal_run = !gtk_text_iter_equal(&iter, &end);
		if (G_UNLIKELY(!normal_run)) {
			/* only if last_character_run is FALSE and normal_run is FALSE we set last_character run to TRUE */
			last_character_run = 1 - last_character_run;
		}
	} while ((normal_run || last_character_run) && (loop%loops_per_timer!=0 || g_timer_elapsed(scanning.timer,NULL)<MAX_CONTINUOUS_SCANNING_INTERVAL));
	DBG_SCANNING("scanned from %d to position %d, (end=%d, orig_end=%d) which took %f microseconds, loops_per_timer=%d\n",gtk_text_iter_get_offset(&start),gtk_text_iter_get_offset(&iter),gtk_text_iter_get_offset(&end),gtk_text_iter_get_offset(&orig_end),g_timer_elapsed(scanning.timer,NULL),loops_per_timer);
	gtk_text_buffer_remove_tag(buffer, btv->needscanning, &start , &iter);
	
	/* because we do not yet have an algorithm to find out where our previous scanning runs are still valid
	we have to re-scan all the text up to the end */
	gtk_text_buffer_apply_tag(buffer,btv->needscanning,&iter,&orig_end);
	/*g_array_free(matchstack,TRUE);*/
#ifdef HL_PROFILING
	stage4 = g_timer_elapsed(scanning.timer,NULL);
	hl_profiling.total_runs++;
	hl_profiling.total_marks += hl_profiling.num_marks;
	hl_profiling.total_chars += hl_profiling.numchars;
	hl_profiling.total_time_ms += (gint)(1000.0*stage4); 
	g_print("scanning run %d (%d ms): %d, %d, %d, %d; from %d-%d, loops=%d,chars=%d,blocks %d/%d (%d) contexts %d/%d (%d) scancache %d, marks %d\n"
		,hl_profiling.total_runs
		,(gint)(1000.0*stage4)
		,(gint)(1000.0*stage1)
		,(gint)(1000.0*stage2-stage1)
		,(gint)(1000.0*stage3-stage2)
		,(gint)(1000.0*stage4-stage3)
		,startpos,gtk_text_iter_get_offset(&iter)
		,hl_profiling.numloops,hl_profiling.numchars
		,hl_profiling.numblockstart,hl_profiling.numblockend,hl_profiling.longest_blockstack
		,hl_profiling.numcontextstart,hl_profiling.numcontextend,hl_profiling.longest_contextstack
		,g_sequence_get_length(btv->scancache.stackcaches)
		,hl_profiling.num_marks
		);
	g_print("memory scancache %d(%dKb+%dKb) fstack %d(%dKb) fcontext %d(%dKb) queue %d(%dKb)\n"
		,hl_profiling.fstack_refcount,(gint)(hl_profiling.fstack_refcount*sizeof(Tfoundstack)/1024.0),(gint)(hl_profiling.fstack_refcount*40/1024.0)
		,hl_profiling.fblock_refcount,(gint)(hl_profiling.fblock_refcount*sizeof(Tfoundblock)/1024.0)
		,hl_profiling.fcontext_refcount,(gint)(hl_profiling.fcontext_refcount*sizeof(Tfoundcontext)/1024.0)
		,hl_profiling.queue_count,(gint)(hl_profiling.queue_count*sizeof(GList)/1024.0)
		);
	g_print("average %d chars/s %d chars/run, %d marks/s, %d marks/run\n"
			,(guint)(1000.0*hl_profiling.total_chars / hl_profiling.total_time_ms )
			,(guint)(1.0*hl_profiling.total_chars / hl_profiling.total_runs )
			,(guint)(1000.0*hl_profiling.total_marks / hl_profiling.total_time_ms )
			,(guint)(1.0*hl_profiling.total_marks / hl_profiling.total_runs )
			);
#endif
	/* tune the loops_per_timer, try to have 10 timer checks per loop, so we have around 10% deviation from the set interval */
	if (normal_run)
		loops_per_timer = MAX(loop/10,200);
	g_timer_destroy(scanning.timer);
#ifndef MINIMAL_REFCOUNTING
	g_queue_foreach(scanning.blockstack,foundblock_foreach_unref_lcb,btv);
	g_queue_foreach(scanning.contextstack,foundcontext_foreach_unref_lcb,btv);
#endif
#ifdef HL_PROFILING
	hl_profiling.queue_count -= (g_queue_get_length(scanning.contextstack)+g_queue_get_length(scanning.blockstack));
#endif
	g_queue_free(scanning.contextstack);
	g_queue_free(scanning.blockstack);
#ifdef VALGRIND_PROFILING
	CALLGRIND_STOP_INSTRUMENTATION;
#endif /* VALGRIND_PROFILING */
	DBG_MSG("cleaned scanning run, finished this run\n");
	return !gtk_text_iter_is_end(&iter); 
}

GQueue *get_contextstack_at_position(BluefishTextView * btv, GtkTextIter *position) {
	Tfoundstack *fstack;
	GQueue *retqueue = g_queue_new();
	fstack = get_stackcache_at_offset(btv, gtk_text_iter_get_offset(position), NULL);
	if (fstack) {
		GList *tmplist;

		tmplist = fstack->contextstack->head;
		while (tmplist) {
			gint cont = ((Tfoundcontext *)tmplist->data)->context;
			g_queue_push_tail(retqueue, GINT_TO_POINTER(cont));
			tmplist = g_list_next(tmplist);
		}
	}
	return retqueue;
}

void scan_for_autocomp_prefix(BluefishTextView *btv,GtkTextIter *mstart,GtkTextIter *cursorpos,gint *contextnum) {
	GtkTextIter iter;
	guint16 pos,newpos;
	GQueue *contextstack;
	/* get the current context */
	iter = *mstart;

	contextstack = get_contextstack_at_position(btv, &iter);
	*contextnum = g_queue_get_length(contextstack) ? GPOINTER_TO_INT(g_queue_peek_head(contextstack)): 1;
	pos = g_array_index(btv->bflang->st->contexts,Tcontext, *contextnum).startstate;
	DBG_AUTOCOMP("start scanning at offset %d with context %d and position %d\n",gtk_text_iter_get_offset(&iter),*contextnum,pos);
	while (!gtk_text_iter_equal(&iter, cursorpos)) {
		gunichar uc;
		uc = gtk_text_iter_get_char(&iter);
		if (G_UNLIKELY(uc > 128)) {
				/* multibyte characters cannot be matched by the engine. character
				1 in ascii is "SOH (start of heading)". we need this to support a
				pattern like [^#]* .  */
			uc = 1;
		}
		DBG_AUTOCOMP("scanning %c\n",uc);
		newpos = g_array_index(btv->bflang->st->table, Ttablerow, pos).row[uc];
		if (G_UNLIKELY(newpos == 0 || uc == '\0')) {
			DBG_AUTOCOMP("newpos=%d...\n",newpos);
			if (G_UNLIKELY(g_array_index(btv->bflang->st->table,Ttablerow, pos).match)) {
				if (g_array_index(btv->bflang->st->matches,Tpattern, g_array_index(btv->bflang->st->table,Ttablerow, pos).match).nextcontext < 0) {
					gint num  = g_array_index(btv->bflang->st->matches,Tpattern, g_array_index(btv->bflang->st->table,Ttablerow, pos).match).nextcontext;
					while (num != 0) {
						g_queue_pop_head(contextstack);
						num++;
					}
					*contextnum = g_queue_get_length(contextstack) ? GPOINTER_TO_INT(g_queue_peek_head(contextstack)): 1;
				} else if (g_array_index(btv->bflang->st->matches,Tpattern, g_array_index(btv->bflang->st->table,Ttablerow, pos).match).nextcontext > 0) {
					DBG_AUTOCOMP("previous pos=%d had a match with a context change!\n",pos);
					*contextnum = g_array_index(btv->bflang->st->matches,Tpattern, g_array_index(btv->bflang->st->table,Ttablerow, pos).match).nextcontext;
					g_queue_push_head(contextstack, GINT_TO_POINTER(*contextnum));
				}
				DBG_AUTOCOMP("found match %d, new context is %d\n",g_array_index(btv->bflang->st->table,Ttablerow, pos).match,*contextnum);
			}
			if (G_LIKELY(gtk_text_iter_equal(mstart,&iter))) {
				gtk_text_iter_forward_char(&iter);
			}
			*mstart = iter;
			newpos = g_array_index(btv->bflang->st->contexts,Tcontext, *contextnum).startstate;
		} else {
			gtk_text_iter_forward_char(&iter);
		}
		pos = newpos;
	}
	g_queue_free(contextstack);
	DBG_AUTOCOMP("scan_for_autocomp_prefix, return mstart at %d, cursor at %d, context %d\n",gtk_text_iter_get_offset(mstart),gtk_text_iter_get_offset(cursorpos),*contextnum);
}

gboolean scan_for_tooltip(BluefishTextView *btv,GtkTextIter *mstart,GtkTextIter *position,gint *contextnum) {
	GtkTextIter iter,end;
	guint16 pos,newpos;
	gboolean retthismatch=FALSE;
	GQueue *contextstack;
	/* get the current context */
	iter = *mstart;

	contextstack = get_contextstack_at_position(btv, &iter);
	*contextnum = g_queue_get_length(contextstack) ? GPOINTER_TO_INT(g_queue_peek_head(contextstack)): 1;
	pos = g_array_index(btv->bflang->st->contexts,Tcontext, *contextnum).startstate;

	gtk_text_buffer_get_end_iter(gtk_text_view_get_buffer(GTK_TEXT_VIEW(btv)),&end);
	DBG_TOOLTIP("start scanning at offset %d with context %d and position %d\n",gtk_text_iter_get_offset(&iter),*contextnum,pos);
	while (!gtk_text_iter_equal(&iter, &end)) {
		gunichar uc;
		uc = gtk_text_iter_get_char(&iter);
		if (G_UNLIKELY(uc > 128)) {
			newpos = 0;
		} else {
			DBG_TOOLTIP("scanning %c\n",uc);
			newpos = g_array_index(btv->bflang->st->table, Ttablerow, pos).row[uc];
		}
		if (G_UNLIKELY(newpos == 0 || uc == '\0')) {
			DBG_TOOLTIP("newpos=%d...\n",newpos);
			if (G_UNLIKELY(g_array_index(btv->bflang->st->table,Ttablerow, pos).match)) {
				DBG_MSG("found match %d, retthismatch=%d\n",g_array_index(btv->bflang->st->table,Ttablerow, pos).match,retthismatch);
				if (retthismatch) {
					*position = iter;
					g_queue_free(contextstack);
					DBG_TOOLTIP("return TRUE, mstart %d position %d\n",gtk_text_iter_get_offset(mstart),gtk_text_iter_get_offset(position));
					return TRUE;
				}
				if (g_array_index(btv->bflang->st->matches,Tpattern, g_array_index(btv->bflang->st->table,Ttablerow, pos).match).nextcontext < 0) {
					gint num  = g_array_index(btv->bflang->st->matches,Tpattern, g_array_index(btv->bflang->st->table,Ttablerow, pos).match).nextcontext;
					while (num != 0) {
						g_queue_pop_head(contextstack);
						num++;
					}
					*contextnum = g_queue_get_length(contextstack) ? GPOINTER_TO_INT(g_queue_peek_head(contextstack)): 1;
					DBG_TOOLTIP("previous pos=%d had a match that popped the context to %d!\n",pos,*contextnum);
				} else if (g_array_index(btv->bflang->st->matches,Tpattern, g_array_index(btv->bflang->st->table,Ttablerow, pos).match).nextcontext > 0) {
					*contextnum = g_array_index(btv->bflang->st->matches,Tpattern, g_array_index(btv->bflang->st->table,Ttablerow, pos).match).nextcontext;
					DBG_TOOLTIP("previous pos=%d had a match that pushed the context to %d!\n",pos,*contextnum);
					g_queue_push_head(contextstack, GINT_TO_POINTER(*contextnum));
				}
			} else if (retthismatch) {
				g_queue_free(contextstack);
				return FALSE;
			}
			if (gtk_text_iter_equal(mstart,&iter)) {
				gtk_text_iter_forward_char(&iter);
			}
			*mstart = iter;
			newpos = g_array_index(btv->bflang->st->contexts,Tcontext, *contextnum).startstate;
		} else {
			gtk_text_iter_forward_char(&iter);
		}
		pos = newpos;
		if (gtk_text_iter_equal(&iter, position)) {
			DBG_TOOLTIP("at cursor position..., scanning in context %d, pos %d (identstate=%d)\n",*contextnum,pos,g_array_index(btv->bflang->st->contexts,Tcontext, *contextnum).identstate);
			if (gtk_text_iter_equal(&iter, mstart) || (pos==g_array_index(btv->bflang->st->contexts,Tcontext, *contextnum).identstate)) {
				g_queue_free(contextstack);
				return FALSE;
			}
			retthismatch = TRUE;
		}
	}
	g_queue_free(contextstack);
	return FALSE;
}

void cleanup_scanner(BluefishTextView *btv) {
	GtkTextIter begin,end;
	GtkTextBuffer *buffer;
	GSequenceIter *sit1,*sit2;

	buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(btv));
	gtk_text_buffer_get_bounds(buffer,&begin,&end);
	gtk_text_buffer_remove_all_tags(buffer,&begin,&end);

	g_sequence_foreach(btv->scancache.stackcaches,foundstack_free_lcb,btv);
	sit1 = g_sequence_get_begin_iter(btv->scancache.stackcaches);
	if (sit1 && !g_sequence_iter_is_end(sit1)) {
		sit2 = g_sequence_get_end_iter(btv->scancache.stackcaches);
		/*g_sequence_foreach_range(sit1,sit2,foundstack_free_lcb,btv);*/
		g_sequence_remove_range(sit1,sit2);
	} else{
		DBG_SCANNING("cleanup_scanner, no sit1, no cleanup ??\n");
	}
#ifdef HL_PROFILING
	g_print("cleanup_scanner, num_marks=%d, fblock_refcount=%d,fcontext_refcount=%d,fstack_refcount=%d\n",hl_profiling.num_marks,hl_profiling.fblock_refcount,hl_profiling.fcontext_refcount,hl_profiling.fstack_refcount);
#endif
#ifdef IDENTSTORING
	bftextview2_identifier_hash_remove_doc(DOCUMENT(btv->doc)->bfwin, btv->doc);
#endif /* IDENTSTORING */

}

void scancache_destroy(BluefishTextView *btv) {
	g_sequence_foreach(btv->scancache.stackcaches,foundstack_free_lcb,btv);
	g_sequence_free(btv->scancache.stackcaches);
	btv->scancache.stackcaches = NULL;
}
