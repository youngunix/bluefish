/* Bluefish HTML Editor
 * bftextview2_langmgr.c
 *
 * Copyright (C) 2008 Olivier Sessink
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

#include <libxml/xmlreader.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>

#include "bftextview2.h"
#include "bftextview2_langmgr.h"
#include "bftextview2_patcompile.h"

#ifdef IN_BLUEFISH
#include "bluefish.h"
#include "document.h"
#include "stringlist.h"
#endif

typedef struct {
	GHashTable *patterns;
	GHashTable *contexts;
	GHashTable *setoptions;
	Tscantable *st; /* while not finished */
	Tbflang *bflang;
} Tbflangparsing;

typedef struct {
	GHashTable *bflang_lookup;
	GList *bflang_list;
	GtkTextTagTable *tagtable; /* a GtkTextTagTable uses a hashtable internally, so lookups by name are fast */
	gboolean load_reference;
	GHashTable *configured_styles; /* key: a NULL terminated char **array with first value the language name,
											second value the highlight name, third NULL
											and as value a gchar * with the name of the GtkTextTag to use */
} Tlangmgr;

static Tlangmgr langmgr = {NULL,NULL,NULL};

/* utils */

/* these hash functions hash the first 2 strings in a gchar ** */
gboolean arr2_equal (gconstpointer v1,gconstpointer v2)
{
	const gchar **arr1 = (const gchar **)v1;
	const gchar **arr2 = (const gchar **)v2;

	return ((strcmp ((char *)arr1[0], (char *)arr2[0]) == 0 )&&
			(strcmp ((char *)arr1[1], (char *)arr2[1]) == 0));
}

guint arr2_hash(gconstpointer v)
{
  /* modified after g_str_hash */
  	const signed char **tmp = (const signed char **)v;
	const signed char *p;
	guint32 h = *tmp[0];
	if (h) {
		gshort i=0;
		while (i<2) {
			p = tmp[i];
			for (p += 1; *p != '\0'; p++)
				h = (h << 5) - h + *p;
			i++;
		}
	}
	return h;
}
/* langmgr code */

static void skip_to_end_tag(xmlTextReaderPtr reader, int depth) {
	while (xmlTextReaderRead(reader)==1) {
		if (xmlTextReaderNodeType(reader)==XML_READER_TYPE_END_ELEMENT && depth == xmlTextReaderDepth(reader)) {
			DBG_PARSING("found node %s type %d with depth %d\n", xmlTextReaderName(reader),xmlTextReaderNodeType(reader), xmlTextReaderDepth(reader));
			break;
		}
	}
}

static void langmrg_create_style(const gchar *name, const gchar *fgcolor, const gchar *bgcolor, gchar *bold, gchar *italic) {
	GtkTextTag *tag;
	gboolean newtag=FALSE;
	if (!name || name[0]=='\0') return;

	tag = gtk_text_tag_table_lookup(langmgr.tagtable,name);
	if (!tag) {
		tag = gtk_text_tag_new(name);
		newtag=TRUE;
	}

	if (fgcolor && fgcolor[0]!='\0') {
		g_object_set(tag, "foreground", fgcolor,"foreground-set", TRUE, NULL);
	} else if (newtag){
		g_object_set(tag, "foreground-set", FALSE, NULL);
	}
	if (bgcolor && bgcolor[0]!='\0') {
		g_object_set(tag, "background", bgcolor, "background-set",TRUE, NULL);
	} else if (newtag) {
		g_object_set(tag, "background-set", FALSE, NULL);
	}
	if (bold && (bold[0]=='1'||bold[0]=='2')) {
		g_object_set(tag, "weight", PANGO_WEIGHT_BOLD, NULL);
	} else if (newtag) {
		g_object_set(tag, "weight", PANGO_WEIGHT_NORMAL, NULL);
	}
	if (italic && (italic[0] == '1'||italic[0] == '2')) {
		g_object_set(tag, "style", PANGO_STYLE_ITALIC, NULL);
	} else if (newtag) {
		g_object_set(tag, "style", PANGO_STYLE_NORMAL, NULL);
	}
/*
	if (fgcolor && fgcolor[0]!='\0') g_object_set(tag, "foreground", fgcolor, NULL);
	if (bgcolor && bgcolor[0]!='\0') g_object_set(tag, "background", bgcolor, NULL);
	if (bold) g_object_set(tag, "weight", PANGO_WEIGHT_BOLD, NULL);
	if (italic) g_object_set(tag, "style", PANGO_STYLE_ITALIC, NULL);
	*/
	if (newtag) {
		gtk_text_tag_table_add(langmgr.tagtable, tag);
		g_object_unref(tag);
	}
}

void langmgr_reload_user_styles(GList *user_styles) {
	GList *tmplist;
	for (tmplist = g_list_first(user_styles);tmplist;tmplist=tmplist->next) {
		gchar **arr = (gchar **)tmplist->data;
		if (count_array(arr)==5) 
			langmrg_create_style(arr[0], arr[1], arr[2], arr[3], arr[4]);
	}
}

void langmgr_reload_user_highlights(void) {
	GList *tmplist;
	g_hash_table_remove_all(langmgr.configured_styles);
	for (tmplist = g_list_first(main_v->props.highlight_styles);tmplist;tmplist=tmplist->next) {
		gchar **arr2, **arr = (gchar **)tmplist->data;
		if (arr[0] && arr[1] && arr[3]) {
			arr2 = array_from_arglist(arr[0], arr[1], NULL);
			g_print("set style %s for highlight %s:%s\n",arr[2],arr2[0],arr2[1]);
			g_hash_table_insert(langmgr.configured_styles,arr2,g_strdup(arr[2]));
		}
	}
	
	tmplist = g_list_first(langmgr.bflang_list);
	while (tmplist) {
		Tbflang *bflang=tmplist->data;
		if (bflang->st) {
			g_print("langmgr_reload_user_highlights, bflang name %s\n",bflang->name);
			bftextview2_scantable_rematch_highlights(bflang->st, bflang->name);
		}
		tmplist = g_list_next(tmplist);
	}
}

static gchar *langmgr_lookup_style_for_highlight(const gchar *lang, const gchar *highlight) {
	const gchar *arr[] = {lang, highlight, NULL};
	return g_hash_table_lookup(langmgr.configured_styles, arr);
}

GtkTextTag *langmrg_lookup_tag_highlight(const gchar *lang, const gchar *highlight) {
	GtkTextTag *tag=NULL;
	if (lang && highlight) {
		gchar *style = langmgr_lookup_style_for_highlight(lang,highlight);
		if (style)
			tag = gtk_text_tag_table_lookup(langmgr.tagtable,style);
	}
	/*g_print("found tag %p for lang %s highlight %s\n",tag,lang,highlight);*/
	return tag;
}
#ifdef IN_BLUEFISH
static void foreachdoc_lcb(Tdocument *doc, gpointer data) {
	if (BLUEFISH_TEXT_VIEW(doc->view)->bflang == data) {
		bluefish_text_view_rescan(BLUEFISH_TEXT_VIEW(doc->view));
	}
}

#endif
/* this is called in the mainloop again */
static gboolean build_lang_finished_lcb(gpointer data)
{
	Tbflangparsing *bfparser=data;
	bfparser->bflang->st = bfparser->st;
	bfparser->bflang->parsing=FALSE;
	g_print("build_lang_finished_lcb..\n");
	/* now walk and rescan all documents that use this bflang */
#ifdef IN_BLUEFISH
	alldocs_foreach(foreachdoc_lcb, bfparser->bflang);
#else
	testapp_rescan_bflang(bfparser->bflang);
#endif

	/* cleanup the parsing structure */
	g_hash_table_destroy(bfparser->patterns);
	g_hash_table_destroy(bfparser->contexts);
	g_hash_table_destroy(bfparser->setoptions);
	g_slice_free(Tbflangparsing,bfparser);


	return FALSE;
}

static gboolean set_string_if_attribute_name(xmlTextReaderPtr reader, xmlChar *aname, xmlChar *searchname, gchar **string) {
	xmlChar *avalue=NULL;
	if (xmlStrEqual(aname,searchname)) {
		avalue = xmlTextReaderValue(reader);
		*string = g_strdup((gchar *)avalue);
		xmlFree(avalue);
		return TRUE;
	}
	return FALSE;
}

static gboolean set_integer_if_attribute_name(xmlTextReaderPtr reader, xmlChar *aname, xmlChar *searchname, gint *integer) {
	gchar *tmp=NULL;
	if (set_string_if_attribute_name(reader, aname, searchname, &tmp)) {
		if (tmp && tmp[0] != '\0')
			*integer = (gint)g_ascii_strtoll(tmp,NULL,10);
		g_free(tmp);
		return TRUE;
	}
	return FALSE;
}

static gboolean set_boolean_if_attribute_name(xmlTextReaderPtr reader, xmlChar *aname, xmlChar *searchname, gboolean *bool) {
	gchar *tmp=NULL;
	if (set_string_if_attribute_name(reader, aname, searchname, &tmp)) {
		if (tmp) {
			*bool = (tmp[0] == '1');
			g_free(tmp);
		}
		return TRUE;
	}
	return FALSE;
}

static void process_header(xmlTextReaderPtr reader, Tbflang *bflang) {
	while (xmlTextReaderRead(reader)==1) {
		xmlChar *name=xmlTextReaderName(reader);
		if (xmlStrEqual(name,(xmlChar *)"mime")) {
			while (xmlTextReaderMoveToNextAttribute(reader)) {
				gchar *mimetype=NULL;
				xmlChar *aname = xmlTextReaderName(reader);
				set_string_if_attribute_name(reader,aname,(xmlChar *)"type", &mimetype);
				if (mimetype)
					bflang->mimetypes = g_list_prepend(bflang->mimetypes, mimetype);
				xmlFree(aname);
			}
		} else if (xmlStrEqual(name,(xmlChar *)"option")) {
			gchar *optionname=NULL;
			gboolean defaultval=FALSE;
			while (xmlTextReaderMoveToNextAttribute(reader)) {
				xmlChar *aname = xmlTextReaderName(reader);
				set_string_if_attribute_name(reader,aname,(xmlChar *)"name", &optionname);
				set_boolean_if_attribute_name(reader,aname,(xmlChar *)"default", &defaultval);
				xmlFree(aname);
			}
			if (optionname) {
				bflang->langoptions = g_list_prepend(bflang->langoptions, optionname);
				if (defaultval) {
					bflang->setoptions = g_list_prepend(bflang->setoptions, optionname);
				}
			}
		} else if (xmlStrEqual(name,(xmlChar *)"highlight")) {
			gchar *name=NULL, *style=NULL, *fgcolor=NULL, *bgcolor=NULL, *italic=NULL,*bold=NULL;
			while (xmlTextReaderMoveToNextAttribute(reader)) {
				xmlChar *aname = xmlTextReaderName(reader);
				set_string_if_attribute_name(reader,aname,(xmlChar *)"name", &name);
				set_string_if_attribute_name(reader,aname,(xmlChar *)"style", &style);
				set_string_if_attribute_name(reader,aname,(xmlChar *)"fgcolor", &fgcolor);
				set_string_if_attribute_name(reader,aname,(xmlChar *)"bgcolor", &bgcolor);
				set_string_if_attribute_name(reader,aname,(xmlChar *)"italic", &italic);
				set_string_if_attribute_name(reader,aname,(xmlChar *)"bold", &bold);
				xmlFree(aname);
			}
			if (name) {
				gchar *use_textstyle = langmgr_lookup_style_for_highlight(bflang->name, name);
				if (use_textstyle) { /* we have a user-configured textstyle for this highlight */
					if (!gtk_text_tag_table_lookup(langmgr.tagtable,use_textstyle)) {
						/* the user-set style does not exist, create the user-set style with the provided style as content */
						gchar **arr;
						langmrg_create_style(use_textstyle, fgcolor, bgcolor, bold, italic);
						arr = array_from_arglist(use_textstyle, fgcolor?fgcolor:"", bgcolor?bgcolor:"", bold?bold:"0", italic?italic:"0",NULL);
						main_v->props.textstyles = g_list_prepend(main_v->props.textstyles, arr);
					}
				} else if (style) { /* no textstyle was configured, use the provided style */
					gchar **arr;
					if (!gtk_text_tag_table_lookup(langmgr.tagtable,style)) {
						langmrg_create_style(style, fgcolor, bgcolor, bold, italic);
						arr = array_from_arglist(style, fgcolor?fgcolor:"", bgcolor?bgcolor:"", bold?bold:"0", italic?italic:"0",NULL);
						main_v->props.textstyles = g_list_prepend(main_v->props.textstyles, arr);
					}
					arr = array_from_arglist(bflang->name,name,NULL);
					g_hash_table_insert(langmgr.configured_styles,arr,g_strdup(style));
					arr = array_from_arglist(bflang->name,name,style,NULL);
					main_v->props.highlight_styles = g_list_prepend(main_v->props.highlight_styles, arr);
				}
			}
			g_free(name);
			g_free(style);
			g_free(fgcolor);
			g_free(bgcolor);
			g_free(italic);
			g_free(bold);
		} else if (xmlStrEqual(name,(xmlChar *)"header")) {
			xmlFree(name);
			break;
		}
		xmlFree(name);
	}
}
/* declaration needed for recursive calling */
static gint16 process_scanning_context(xmlTextReaderPtr reader, Tbflangparsing *bfparser, GQueue *contextstack);

static guint16 process_scanning_element(xmlTextReaderPtr reader, Tbflangparsing *bfparser, gint16 context, GQueue *contextstack
		,gchar *ih_autocomplete_append ,const gchar *ih_highlight
		,gboolean ih_case_insens ,gboolean ih_is_regex ,gboolean ih_autocomplete) {
	guint16 matchnum=0;
	gchar *pattern=NULL, *idref=NULL, *highlight=NULL, *blockstartelement=NULL, *blockhighlight=NULL, *class=NULL, *autocomplete_append=NULL, *autocomplete_string=NULL, *id=NULL;
	gboolean case_insens=FALSE, is_regex=FALSE, starts_block=FALSE, ends_block=FALSE, is_empty, autocomplete=FALSE;
	gint ends_context=0;
	is_empty = xmlTextReaderIsEmptyElement(reader);
	while (xmlTextReaderMoveToNextAttribute(reader)) {
		xmlChar *aname = xmlTextReaderName(reader);
		set_string_if_attribute_name(reader,aname,(xmlChar *)"pattern",&pattern);
		set_string_if_attribute_name(reader,aname,(xmlChar *)"id",&id);
		set_string_if_attribute_name(reader,aname,(xmlChar *)"idref",&idref);
		set_string_if_attribute_name(reader,aname,(xmlChar *)"highlight",&highlight);
		set_string_if_attribute_name(reader,aname,(xmlChar *)"blockhighlight",&blockhighlight);
		set_string_if_attribute_name(reader,aname,(xmlChar *)"blockstartelement",&blockstartelement);
		set_string_if_attribute_name(reader,aname,(xmlChar *)"class",&class);
		set_boolean_if_attribute_name(reader, aname, (xmlChar *)"is_regex", &is_regex);
		set_boolean_if_attribute_name(reader, aname, (xmlChar *)"starts_block", &starts_block);
		set_boolean_if_attribute_name(reader, aname, (xmlChar *)"ends_block", &ends_block);
		set_boolean_if_attribute_name(reader, aname, (xmlChar *)"case_insens", &case_insens);
		set_integer_if_attribute_name(reader, aname, (xmlChar *)"ends_context", &ends_context);
		set_boolean_if_attribute_name(reader, aname, (xmlChar *)"autocomplete", &autocomplete);
		set_string_if_attribute_name(reader,aname,(xmlChar *)"autocomplete_string",&autocomplete_string);
		set_string_if_attribute_name(reader,aname,(xmlChar *)"autocomplete_append",&autocomplete_append);
		xmlFree(aname);
	}
	if (!class || g_hash_table_lookup(bfparser->setoptions,class)) {
		if (idref && idref[0] && !id && !pattern) {
			guint16 matchnum;
			matchnum = GPOINTER_TO_INT(g_hash_table_lookup(bfparser->patterns, idref));
			compile_existing_match(bfparser->st,matchnum, context);
		} else if (pattern && pattern[0]) {
			gchar *reference=NULL;
			guint16 blockstartelementum=0, nextcontext=0;
			DBG_PARSING("pattern %s\n",pattern);
			if (ends_context) {
				/* the nth number in the stack */
				nextcontext=-1*ends_context;/*GPOINTER_TO_INT(g_queue_peek_nth(contextstack,ends_context));*/

			}
			if (blockstartelement) {
				blockstartelementum = GPOINTER_TO_INT(g_hash_table_lookup(bfparser->patterns, blockstartelement));
				DBG_PARSING("got blockstartelementum %d for blockstartelement %s, ends_block=%d\n",blockstartelementum,blockstartelement,ends_block);
			}
			matchnum = add_keyword_to_scanning_table(bfparser->st, pattern, bfparser->bflang->name, highlight?highlight:ih_highlight, blockhighlight,is_regex?is_regex:ih_is_regex,case_insens?case_insens:ih_case_insens, context, nextcontext
					, starts_block, ends_block, blockstartelementum,FALSE,NULL, NULL);
			DBG_PARSING("add matchnum %d to hash table for key %s, starts_block=%d\n",matchnum,pattern,starts_block);
			g_hash_table_insert(bfparser->patterns, g_strdup(id?id:pattern), GINT_TO_POINTER((gint)matchnum));
			/* now check if there is a deeper context */
			if (!is_empty) {
				while (xmlTextReaderRead(reader)==1) {
					xmlChar *name=xmlTextReaderName(reader);
					if (xmlStrEqual(name,(xmlChar *)"context")) {
						DBG_PARSING("in pattern, found countext\n");
						nextcontext = process_scanning_context(reader,bfparser,contextstack);
						match_set_nextcontext(bfparser->st, matchnum, nextcontext);
					} else if (xmlStrEqual(name,(xmlChar *)"reference")) {
						DBG_PARSING("in pattern, found reference\n");
						if (!xmlTextReaderIsEmptyElement(reader)) {
							if (langmgr.load_reference)
								reference = (gchar *)xmlTextReaderReadInnerXml(reader);
							DBG_PARSING("reference=%s\n",reference);
							while (xmlTextReaderRead(reader)==1) {
								xmlFree(name);
								name=xmlTextReaderName(reader);
								if (xmlStrEqual(name,(xmlChar *)"reference")) {
									break;
								}
							}
						}
					} else if (xmlStrEqual(name,(xmlChar *)"element")) {
						DBG_PARSING("in pattern, found pattern --> end -of-pattern\n");
						xmlFree(name);
						break;
					} else {
						DBG_PARSING("parsing element with name %s\n",name);
					}
					xmlFree(name);
				}
			}
			match_autocomplete_reference(bfparser->st,matchnum,autocomplete?autocomplete:ih_autocomplete,autocomplete_string?autocomplete_string:pattern,context,autocomplete_append?autocomplete_append:ih_autocomplete_append,reference);
		}
	}
	/* TODO cleanup! */
	if (pattern) xmlFree(pattern);
	if (id) xmlFree(id);
	if (idref) xmlFree(idref);
	if (autocomplete_string) xmlFree(autocomplete_string);
	if (autocomplete_append) xmlFree(autocomplete_append);
	/*if (highlight) xmlFree(highlight);*/
	if (blockstartelement) xmlFree(blockstartelement);
	/*if (blockhighlight) xmlFree(blockhighlight);*/
	if (class) xmlFree(class);
	return matchnum;
}

static guint16 process_scanning_tag(xmlTextReaderPtr reader, Tbflangparsing *bfparser, guint16 context, GQueue *contextstack
		,gchar *ih_autocomplete_append ,gchar *ih_highlight,gchar *ih_attrib_autocomplete_append ,gchar *ih_attribhighlight) {
	gchar *tag=NULL, *idref=NULL, *highlight=NULL, *attributes=NULL, *attribhighlight=NULL,*class=NULL, *autocomplete_append=NULL,*attrib_autocomplete_append=NULL,*id=NULL;
	guint16 matchnum=0,innercontext=context;
	gboolean is_empty;
	DBG_PARSING("processing tag...\n");
	is_empty = xmlTextReaderIsEmptyElement(reader);
	while (xmlTextReaderMoveToNextAttribute(reader)) {
		xmlChar *aname = xmlTextReaderName(reader);
		set_string_if_attribute_name(reader,aname,(xmlChar *)"id",&id);
		set_string_if_attribute_name(reader,aname,(xmlChar *)"idref",&idref);
		set_string_if_attribute_name(reader,aname,(xmlChar *)"name",&tag);
		set_string_if_attribute_name(reader,aname,(xmlChar *)"highlight",&highlight);
		set_string_if_attribute_name(reader,aname,(xmlChar *)"class",&class);
		set_string_if_attribute_name(reader,aname,(xmlChar *)"autocomplete_append",&autocomplete_append);
		set_string_if_attribute_name(reader,aname,(xmlChar *)"attribhighlight",&attribhighlight);
		set_string_if_attribute_name(reader,aname, (xmlChar *)"attributes", &attributes);
		set_string_if_attribute_name(reader,aname, (xmlChar *)"attrib_autocomplete_append", &attrib_autocomplete_append);
		xmlFree(aname);
	}
	if (!class || g_hash_table_lookup(bfparser->setoptions,class)) {
		if (idref && idref[0] && !tag) {
			guint16 matchnum = GPOINTER_TO_INT(g_hash_table_lookup(bfparser->patterns, idref));
			DBG_PARSING("lookup tag with id %s returned matchnum %d\n",id,matchnum);
			if (matchnum)
				compile_existing_match(bfparser->st,matchnum, context);
		} else if (tag && tag[0]) {
			const gchar *stringhighlight="string";
			guint16 contexttag, starttagmatch, endtagmatch;
			gint contextstring;
			gchar *tmp, *reference=NULL;

			contexttag = new_context(bfparser->st, bfparser->bflang->name, ">\"=' \t\n\r", NULL, FALSE);
			tmp = g_strconcat("<",tag,NULL);
			matchnum = add_keyword_to_scanning_table(bfparser->st, tmp,bfparser->bflang->name,highlight?highlight:ih_highlight,NULL, FALSE, FALSE, context, contexttag, TRUE, FALSE, 0, FALSE,NULL,NULL);
			DBG_PARSING("insert tag %s into hash table with matchnum %d\n",id?id:tmp,matchnum);
			g_hash_table_insert(bfparser->patterns, g_strdup(id?id:tmp), GINT_TO_POINTER((gint)matchnum));
			g_free(tmp);
			add_keyword_to_scanning_table(bfparser->st, "/>", bfparser->bflang->name, highlight?highlight:ih_highlight, NULL, FALSE, FALSE, contexttag, -1, FALSE, FALSE, 0, FALSE,NULL,NULL);
			starttagmatch = add_keyword_to_scanning_table(bfparser->st, ">", bfparser->bflang->name, highlight?highlight:ih_highlight, NULL, FALSE, FALSE, contexttag, -1, FALSE, FALSE, 0, FALSE,NULL,NULL);
			if (attributes) {
				gchar **arr, **tmp2;
				arr = g_strsplit(attributes,",",-1);
				tmp2 = arr;
				while (*tmp2) {
					add_keyword_to_scanning_table(bfparser->st, *tmp2,bfparser->bflang->name,attribhighlight?attribhighlight:ih_attribhighlight,NULL, FALSE, FALSE, contexttag, 0, FALSE, FALSE, 0, TRUE,attrib_autocomplete_append?attrib_autocomplete_append:ih_attrib_autocomplete_append,NULL);
					tmp2++;
				}
				g_strfreev(arr);
				contextstring = GPOINTER_TO_INT(g_hash_table_lookup(bfparser->contexts, "__internal_tag__"));
				if (!contextstring) {
					contextstring = new_context(bfparser->st, bfparser->bflang->name, "\"=' \t\n\r", stringhighlight, FALSE);
					add_keyword_to_scanning_table(bfparser->st, "\"",bfparser->bflang->name,stringhighlight,stringhighlight, FALSE, FALSE, contextstring, -1, FALSE, FALSE, 0, FALSE,NULL,NULL);
					g_hash_table_insert(bfparser->contexts, g_strdup("__internal_tag__"), GINT_TO_POINTER(contextstring));
				}
				add_keyword_to_scanning_table(bfparser->st, "\"", bfparser->bflang->name, stringhighlight,NULL,FALSE, FALSE, contexttag, contextstring, FALSE, FALSE, 0, FALSE,NULL,NULL);
			}

			if (!is_empty) {
				while (xmlTextReaderRead(reader)==1) {
					xmlChar *name=xmlTextReaderName(reader);
					if (xmlStrEqual(name,(xmlChar *)"reference")) {
						if (!xmlTextReaderIsEmptyElement(reader)) {
							if (langmgr.load_reference)
								reference = (gchar *)xmlTextReaderReadInnerXml(reader);
							DBG_PARSING("reference=%s\n",reference);
							while (xmlTextReaderRead(reader)==1) {
								xmlFree(name);
								name=xmlTextReaderName(reader);
								if (xmlStrEqual(name,(xmlChar *)"reference")) {
									break;
								}
							}
						}
					} else if (xmlStrEqual(name,(xmlChar *)"context")) {
						innercontext = process_scanning_context(reader,bfparser,contextstack);
						match_set_nextcontext(bfparser->st, starttagmatch, innercontext);
					} else if (xmlStrEqual(name,(xmlChar *)"tag")) {
						xmlFree(name);
						break;
					}
					xmlFree(name);
				}
			}
			tmp = g_strconcat("<",tag,NULL);
			match_autocomplete_reference(bfparser->st,matchnum,TRUE,tmp,context,autocomplete_append?autocomplete_append:ih_autocomplete_append,reference);
			g_free(tmp);
			tmp = g_strconcat("</",tag,">",NULL);
			endtagmatch = add_keyword_to_scanning_table(bfparser->st, tmp, bfparser->bflang->name,highlight?highlight:ih_highlight,NULL, FALSE, FALSE, innercontext, (innercontext==context)?0:-2, FALSE, TRUE, matchnum, TRUE,NULL,NULL);
			g_hash_table_insert(bfparser->patterns, g_strdup(tmp), GINT_TO_POINTER((gint)endtagmatch));
			g_free(tmp);
		}
	}
	g_free(id);
	g_free(idref);
	g_free(tag);
	/*g_free(highlight);*/
	g_free(class);
	g_free(autocomplete_append);
	g_free(attrib_autocomplete_append);
	/*g_free(attribhighlight);*/
	g_free(attributes);

	return matchnum;
}

/* ih stands for InHerit */
static void process_scanning_group(xmlTextReaderPtr reader, Tbflangparsing *bfparser, gint context, GQueue *contextstack
		,gchar *ih_autocomplete_append ,gchar *ih_highlight ,gchar *ih_class ,gchar *ih_attrib_autocomplete_append ,gchar *ih_attribhighlight
		,gboolean ih_case_insens ,gboolean ih_is_regex ,gboolean ih_autocomplete) {
	gchar *autocomplete_append=NULL, *highlight=NULL, *class=NULL, *attrib_autocomplete_append=NULL, *attribhighlight=NULL;
	gboolean case_insens=FALSE, is_regex=FALSE, autocomplete=FALSE;
	gint depth;
	if (xmlTextReaderIsEmptyElement(reader)) {
		return;
	}
	depth = xmlTextReaderDepth(reader);
	while (xmlTextReaderMoveToNextAttribute(reader)) {
		xmlChar *aname = xmlTextReaderName(reader);
		set_string_if_attribute_name(reader,aname,(xmlChar *)"autocomplete_append",&autocomplete_append);
		set_string_if_attribute_name(reader,aname,(xmlChar *)"highlight",&highlight);
		set_string_if_attribute_name(reader,aname,(xmlChar *)"class",&class);
		set_boolean_if_attribute_name(reader,aname,(xmlChar *)"case_insens",&case_insens);
		set_boolean_if_attribute_name(reader,aname,(xmlChar *)"is_regex",&is_regex);
		set_boolean_if_attribute_name(reader,aname,(xmlChar *)"autocomplete",&autocomplete);
		set_string_if_attribute_name(reader,aname,(xmlChar *)"attrib_autocomplete_append",&attrib_autocomplete_append);
		set_string_if_attribute_name(reader,aname,(xmlChar *)"attribhighlight",&attribhighlight);
		xmlFree(aname);
	}

	if (class && GPOINTER_TO_INT(g_hash_table_lookup(bfparser->setoptions,class))!=1){
		DBG_PARSING("group disabled, class=%s, skip to end of group, my depth=%d\n",class,depth);
		skip_to_end_tag(reader, depth);
	} else {
		while (xmlTextReaderRead(reader)==1) {
			xmlChar *name = xmlTextReaderName(reader);
			if (xmlStrEqual(name,(xmlChar *)"element")) {
				process_scanning_element(reader,bfparser,context,contextstack
						,autocomplete_append?autocomplete_append:ih_autocomplete_append
						,highlight?highlight:ih_highlight
						,case_insens?case_insens:ih_case_insens
						,is_regex?is_regex:ih_is_regex
						,autocomplete?autocomplete:ih_autocomplete
						);
			} else if (xmlStrEqual(name,(xmlChar *)"tag")) {
				process_scanning_tag(reader,bfparser,context,contextstack
						,autocomplete_append?autocomplete_append:ih_autocomplete_append
						,highlight?highlight:ih_highlight
						,attrib_autocomplete_append?attrib_autocomplete_append:ih_attrib_autocomplete_append
						,attribhighlight?attribhighlight:ih_attribhighlight
						);
			} else if (xmlStrEqual(name,(xmlChar *)"group")) {
				if (xmlTextReaderDepth(reader)==depth) {
					DBG_PARSING("end-of-group\n");
					xmlFree(name);
					break;
				} else {
					process_scanning_group(reader, bfparser, context, contextstack
							,autocomplete_append?autocomplete_append:ih_autocomplete_append
							,highlight?highlight:ih_highlight
							,class?class:ih_class
							,attrib_autocomplete_append?attrib_autocomplete_append:ih_attrib_autocomplete_append
							,attribhighlight?attribhighlight:ih_attribhighlight
							,case_insens?case_insens:ih_case_insens
							,is_regex?is_regex:ih_is_regex
							,autocomplete?autocomplete:ih_autocomplete
							 );
				}
			} else
				DBG_PARSING("found %s\n",name);
			xmlFree(name);
		}
	}
	g_free(autocomplete_append);
	/*g_free(highlight); stored in the structure */
	g_free(class);
	g_free(attrib_autocomplete_append);
	g_free(attribhighlight);
}

static gint16 process_scanning_context(xmlTextReaderPtr reader, Tbflangparsing *bfparser, GQueue *contextstack) {
	gchar *symbols=NULL, *highlight=NULL, *name=NULL, *idref=NULL;
	gboolean autocomplete_case_insens=FALSE,is_empty;
	gint context;
	is_empty = xmlTextReaderIsEmptyElement(reader);
	while (xmlTextReaderMoveToNextAttribute(reader)) {
		xmlChar *aname = xmlTextReaderName(reader);
		set_string_if_attribute_name(reader,aname,(xmlChar *)"id",&name);
		set_string_if_attribute_name(reader,aname,(xmlChar *)"idref",&idref);
		set_string_if_attribute_name(reader,aname,(xmlChar *)"symbols",&symbols);
		set_string_if_attribute_name(reader,aname,(xmlChar *)"highlight",&highlight);
		set_boolean_if_attribute_name(reader,aname,(xmlChar *)"autocomplete_case_insens",&autocomplete_case_insens);
		xmlFree(aname);
	}
	if (is_empty && idref && !name && !symbols && !highlight && !autocomplete_case_insens) {
		DBG_PARSING("lookup context %s in hash table..\n",name);
		context = GPOINTER_TO_INT(g_hash_table_lookup(bfparser->contexts, idref));
		g_free(idref);
		return context;
	}
	if (!symbols || !name) {
		return 0;
	}
	/* create context */
	DBG_PARSING("create context symbols %s and style %s\n",symbols,style);
	context = new_context(bfparser->st,bfparser->bflang->name,symbols,highlight,autocomplete_case_insens);
	g_queue_push_head(contextstack,GINT_TO_POINTER(context));
	if (name) {
		DBG_PARSING("insert context %s into hash table as %d\n",name,context);
		g_hash_table_insert(bfparser->contexts, g_strdup(name), GINT_TO_POINTER(context));
	}
	g_free(name);
	g_free(symbols);
	/*g_free(highlight); stored in the structure */
	/* now get the children */
	while (xmlTextReaderRead(reader)==1) {
		xmlChar *name = xmlTextReaderName(reader);
		DBG_PARSING("parsing context, found node name %s\n",name);
		if (xmlStrEqual(name,(xmlChar *)"element")) {
			process_scanning_element(reader,bfparser,context,contextstack,NULL,NULL,FALSE,FALSE,FALSE);
		} else if (xmlStrEqual(name,(xmlChar *)"tag")) {
			process_scanning_tag(reader,bfparser,context,contextstack,NULL,NULL,NULL,NULL);
		} else if (xmlStrEqual(name,(xmlChar *)"group")) {
			process_scanning_group(reader,bfparser,context,contextstack,NULL,NULL,NULL,NULL,NULL,FALSE,FALSE,FALSE);
		} else if (xmlStrEqual(name,(xmlChar *)"context")) {
			xmlFree(name);
			DBG_PARSING("parsing context, end-of-context, return context %d\n",context);
			g_queue_pop_head(contextstack);
			return context;
		} else
			DBG_PARSING("parsing context, found %s\n",name);
		xmlFree(name);
	}
	/* can we ever get here ?? */
	g_print("pop context %d\n",GPOINTER_TO_INT(g_queue_peek_head(contextstack)));
	g_queue_pop_head(contextstack);
	return context;
}

static gpointer build_lang_thread(gpointer data)
{
	xmlTextReaderPtr reader;
	Tbflang *bflang = data;
	Tbflangparsing *bfparser;
	GList *tmplist;
	
	bfparser = g_slice_new0(Tbflangparsing);
	bfparser->patterns =  g_hash_table_new_full(g_str_hash,g_str_equal,g_free,NULL);
	bfparser->contexts =  g_hash_table_new_full(g_str_hash,g_str_equal,g_free,NULL);
	bfparser->setoptions =  g_hash_table_new_full(g_str_hash,g_str_equal,g_free,NULL);
	bfparser->bflang = bflang;
	for(tmplist = g_list_first(bfparser->bflang->setoptions);tmplist;tmplist=g_list_next(tmplist)) {
		g_hash_table_insert(bfparser->setoptions,g_strdup(tmplist->data),GINT_TO_POINTER(1));
	}
	bfparser->st = scantable_new(bflang->size_table,bflang->size_matches,bflang->size_contexts);

	DBG_PARSING("build_lang_thread %p, started for %s\n",g_thread_self(),bfparser->bflang->filename);
	reader = xmlNewTextReaderFilename(bfparser->bflang->filename);
	if (!reader) {
		g_print("failed to open %s\n",bfparser->bflang->filename);
		/* TODO CLEANUP*/
		return NULL;
	}
	xmlTextReaderSetParserProp(reader,XML_PARSER_SUBST_ENTITIES,TRUE);

	while (xmlTextReaderRead(reader) == 1) {
		xmlChar *name = xmlTextReaderName(reader);
		DBG_PARSING("found %s\n",name);
		if (xmlStrEqual(name,(xmlChar *)"header")) {
			/* actually we can skip detection */
			DBG_PARSING("processing <header>\n");
			process_header(reader,bflang);
		} else if (xmlStrEqual(name,(xmlChar *)"definition")) {
			if (xmlTextReaderIsEmptyElement(reader)) {
				/* empty <definition />, probably text/plain */
				bfparser->st->table->len = 2;
				bfparser->st->matches->len = 2;
				bfparser->st->contexts->len = 2;
				xmlFree(name);
				break;
			}
			DBG_PARSING("processing <definition>\n");
			while (xmlTextReaderRead(reader)==1) {
				xmlChar *name2 = xmlTextReaderName(reader);
				if (xmlStrEqual(name2,(xmlChar *)"context")) {
					GQueue *contextstack = g_queue_new();
					process_scanning_context(reader,bfparser,contextstack);
				} else if (xmlStrEqual(name2,(xmlChar *)"definition")) {
					xmlFree(name2);
					break;
				} else
					DBG_PARSING("found %s\n",name2);
				xmlFree(name2);
			}
		}
		xmlFree(name);
	}
	xmlFreeTextReader(reader);
	/* do some final memory management */
	bfparser->st->table->data = g_realloc(bfparser->st->table->data, bfparser->st->table->len*sizeof(Ttablerow));
	bfparser->st->contexts->data = g_realloc(bfparser->st->contexts->data, bfparser->st->contexts->len*sizeof(Tcontext));
	bfparser->st->matches->data = g_realloc(bfparser->st->matches->data, bfparser->st->matches->len*sizeof(Tpattern));

	DBG_PARSING("build_lang_thread finished bflang=%p\n",bflang);
	print_scantable_stats(bflang->name,bfparser->st);
	/*print_DFA(bfparser->st, '&','Z');*/
	/*print_DFA_subset(bfparser->st, "\\\" ");*/

	/* when done call mainloop */
	g_idle_add_full(G_PRIORITY_LOW,build_lang_finished_lcb, bfparser,NULL);
	return bflang;
}

Tbflang *langmgr_get_bflang_for_mimetype(const gchar *mimetype) {
	Tbflang *bflang;

	bflang = g_hash_table_lookup(langmgr.bflang_lookup,mimetype);
	if (bflang && bflang->filename && !bflang->st && !bflang->parsing) {
		GError *error=NULL;
		GThread* thread;
		bflang->parsing=TRUE;
		DBG_MSG("no scantable in %p, start thread\n",bflang);
		/*thread = g_thread_create(build_lang_thread,bflang,FALSE,&error);*/
		thread = g_thread_create_full(build_lang_thread,bflang,0,FALSE,TRUE,G_THREAD_PRIORITY_LOW,&error);
		
		if (error) {
			DBG_PARSING("start thread, error\n");
		}
	} else {
		DBG_MSG("have scantable, return %p\n",bflang);
	}
	return bflang;
}

GtkTextTagTable *langmgr_get_tagtable(void) {
	return langmgr.tagtable;
}

static Tbflang *parse_bflang2_header(const gchar *filename) {
	xmlTextReaderPtr reader;
	Tbflang *bflang=NULL;
	reader = xmlNewTextReaderFilename(filename);
	xmlTextReaderSetParserProp(reader,XML_PARSER_SUBST_ENTITIES,TRUE);
	if (reader != NULL) {
		bflang = g_slice_new0(Tbflang);
		bflang->size_table=4; /* bare minimum sizes */
		bflang->size_matches=2;
		bflang->size_contexts=2;
		bflang->filename = g_strdup(filename);
		while (xmlTextReaderRead(reader) == 1) {
			xmlChar *name = xmlTextReaderName(reader);
			if (xmlStrEqual(name,(xmlChar *)"bflang")) {
				while (xmlTextReaderMoveToNextAttribute(reader)) {
					xmlChar *aname = xmlTextReaderName(reader);
					set_string_if_attribute_name(reader,aname,(xmlChar *)"name",&bflang->name);
					set_integer_if_attribute_name(reader,aname,(xmlChar *)"table",&bflang->size_table);
					set_integer_if_attribute_name(reader,aname,(xmlChar *)"matches",&bflang->size_matches);
					set_integer_if_attribute_name(reader,aname,(xmlChar *)"contexts",&bflang->size_contexts);
					xmlFree(aname);
				}
			} else if (xmlStrEqual(name,(xmlChar *)"header")) {
				process_header(reader,bflang);
				xmlFree(name);
				break;
			}
			xmlFree(name);
		}
		xmlFreeTextReader(reader);
		if (bflang->name == NULL) {
			g_print("Language file %s has no name.. abort..\n",filename);
			return NULL;
		}
	}
	return bflang;
}

GList *langmgr_get_languages(void) {
	return g_list_copy(langmgr.bflang_list);
}

static void register_bflanguage(Tbflang *bflang) {
	if (bflang) {
		GList *tmplist;
		/*g_print("register bflang %s\n",bflang->name);*/
		tmplist = g_list_first(bflang->mimetypes);
		while (tmplist) {
			g_hash_table_insert(langmgr.bflang_lookup, (gchar *)tmplist->data, bflang);
			tmplist = g_list_next(tmplist);
		}
		langmgr.bflang_list = g_list_prepend(langmgr.bflang_list, bflang);
	}
}

static void scan_bflang2files(void) {
	const gchar *filename, *dir=PKGDATADIR"/bflang/";
	GError *error = NULL;
	GPatternSpec *ps = g_pattern_spec_new("*.bflang2");
	GDir *gd = g_dir_open(dir, 0, &error);
	if (!error) {
		filename = g_dir_read_name(gd);
		while (filename) {
			if (g_pattern_match(ps, strlen(filename), filename, NULL)) {
				Tbflang *bflang;
				gchar *path = g_strconcat(dir, "/",filename, NULL);
				bflang = parse_bflang2_header(path);
				register_bflanguage(bflang);
				g_free(path);
			}
			filename = g_dir_read_name(gd);
		}
		g_dir_close(gd);
	}
	g_pattern_spec_free(ps);
}

void langmgr_init() {
	GList *tmplist;
	GtkTextTag *tag;

	langmgr.tagtable = gtk_text_tag_table_new();
	langmgr.bflang_lookup = g_hash_table_new(g_str_hash,g_str_equal);
	langmgr.load_reference = main_v->props.load_reference;
	langmgr.configured_styles = g_hash_table_new(arr2_hash,arr2_equal);

	tag = gtk_text_tag_new("_needscanning_");
	gtk_text_tag_table_add(langmgr.tagtable, tag);
	g_object_unref(tag);
	tag = gtk_text_tag_new("_blockmatch_");
	g_object_set(tag, "background", "red", NULL);
	g_object_set(tag, "foreground", "white", NULL);
	gtk_text_tag_table_add(langmgr.tagtable, tag);
	g_object_unref(tag);
	tag = gtk_text_tag_new("_folded_");
	g_object_set(tag, "editable", FALSE, NULL);
	g_object_set(tag, "invisible", TRUE, NULL);
	gtk_text_tag_table_add(langmgr.tagtable, tag);
	g_object_unref(tag);
	tag = gtk_text_tag_new("_foldheader_");
	g_object_set(tag, "editable", FALSE, NULL);
	g_object_set(tag, "background", "#99FF99", NULL);
	gtk_text_tag_table_add(langmgr.tagtable, tag);
	g_object_unref(tag);

	langmgr_reload_user_styles(main_v->props.textstyles);
	for (tmplist = g_list_first(main_v->props.highlight_styles);tmplist;tmplist=tmplist->next) {
		gchar **arr2, **arr = (gchar **)tmplist->data;
		arr2 = array_from_arglist(arr[0], arr[1], NULL);
		g_print("set style %s for highlight %s:%s\n",arr[2],arr2[0],arr2[1]);
		g_hash_table_insert(langmgr.configured_styles,arr2,g_strdup(arr[2]));
	}
	scan_bflang2files();
	DBG_PARSING("langmgr_init, returning \n");
}
