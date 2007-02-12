/* Bluefish HTML Editor
 * infb_gui.c - plugin for information browser
 *
 * Copyright (C) 2007 Oskar Świda
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
 
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h> 
#include <string.h>
#include <time.h>
#include "../dialog_utils.h"
#include "infbrowser.h"
#include "infb_gui.h"
#include "infb_text.h"
#include "infb_load.h"
#include "infb_dtd.h"
#include "infb_wizard.h"


/*
*		Mouse motion and link discovery
*/
static gboolean infb_motion_notify_event (GtkWidget  *text_view,  GdkEventMotion *event,gpointer user_data) {
  gint x, y;
  GSList *tags = NULL, *tagp = NULL;
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  gboolean hovering = FALSE;
  Tbfwin *bfwin = BFWIN(user_data);
  Tinfbwin *win;
  gpointer auxp; 

	auxp = g_hash_table_lookup(infb_v.windows,bfwin);
	if (!auxp) return FALSE;
	win = (Tinfbwin*)auxp;
	
 	gtk_text_view_window_to_buffer_coords (GTK_TEXT_VIEW (text_view),GTK_TEXT_WINDOW_WIDGET,
                                         event->x, event->y, &x, &y);
 	buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW(text_view));
 	gtk_text_view_get_iter_at_location (GTK_TEXT_VIEW(text_view), &iter, x, y);
 	tags = gtk_text_iter_get_tags (&iter);
 
	 for (tagp = tags;  tagp != NULL;  tagp = tagp->next)
    {    	
      GtkTextTag *tag = tagp->data;
      gpointer type = g_object_get_data (G_OBJECT (tag), "type");
      if (type == &infb_v.nt_fileref || type == &infb_v.nt_node || type == &infb_v.nt_group) 
        {
          hovering = TRUE;
          
          break;
        }
    }
  if (hovering != win->hovering_over_link)
    {
      win->hovering_over_link = hovering;
      if (win->hovering_over_link)
        gdk_window_set_cursor (gtk_text_view_get_window (GTK_TEXT_VIEW(text_view), GTK_TEXT_WINDOW_TEXT), win->hand_cursor);
      else
        gdk_window_set_cursor (gtk_text_view_get_window (GTK_TEXT_VIEW(text_view), GTK_TEXT_WINDOW_TEXT), win->regular_cursor);
    }
  if (tags) g_slist_free (tags);
  gdk_window_get_pointer (text_view->window, NULL, NULL, NULL);
  return FALSE;
}


/*
*			Mouse release: link activation
*/
gboolean  infb_button_release_event(GtkWidget  *widget,GdkEventButton *event, gpointer  user_data) {
   gint x,y;
	GtkTextBuffer *buffer;
	GtkTextIter start,end,iter;
	GSList *tags = NULL, *tagp = NULL;
	GtkTextTag *tag;
	gpointer aux;
	xmlDocPtr doc;
	xmlNodePtr auxnode;	   
	xmlChar *text;
	Tbfwin *bfwin = BFWIN(user_data);
   
   if (event->button == 1) {
   
   	buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (widget));
   	gtk_text_buffer_get_selection_bounds (buffer, &start, &end);
  		if (gtk_text_iter_get_offset (&start) != gtk_text_iter_get_offset (&end))  return FALSE;
	   gtk_text_view_window_to_buffer_coords (GTK_TEXT_VIEW (widget),GTK_TEXT_WINDOW_WIDGET,event->x, event->y, &x, &y);
	   gtk_text_view_get_iter_at_location (GTK_TEXT_VIEW (widget), &iter, x, y);
	   tags = gtk_text_iter_get_tags (&iter);
  		for (tagp = tags;  tagp != NULL;  tagp = tagp->next)
    	{
      	tag = tagp->data;
      	aux = g_object_get_data(G_OBJECT(tag),"type");
      	if ( aux && aux == &infb_v.nt_fileref) { /* fileref */
      		infb_v.currentNode = NULL;
      		if (infb_v.currentDoc!=NULL && infb_v.currentDoc!=infb_v.homeDoc)
      			xmlFreeDoc(infb_v.currentDoc);
      		aux = g_object_get_data(G_OBJECT(tag),"loaded");
      		if ( aux ) {
      			infb_v.currentDoc = aux;
					infb_fill_doc(bfwin,NULL);      				
      		}
      		else {	
      			aux = g_object_get_data(G_OBJECT(tag),"file");
      			if ( aux ) {
      				doc = xmlParseFile(aux);
      				if ( doc ) {
      					g_object_set_data (G_OBJECT (tag), "loaded", doc);
      					/* check for conversion */
      					auxnode = xmlDocGetRootElement(doc);
      					if (xmlStrcmp(auxnode->name,BAD_CAST "ref")==0) {
      						text = xmlGetProp(auxnode,BAD_CAST "type");
      						if (text) {
      							if (xmlStrcmp(text,BAD_CAST "dtd")==0) {
      								xmlFree(text);
      								text = xmlGetProp(auxnode,BAD_CAST "uri");
      								if (text) {
      									const gchar *buttons[] = { GTK_STOCK_NO, GTK_STOCK_YES, NULL };
      									gint res;      									 
      									if (g_str_has_prefix ((gchar*)text,"http://")) { 
      										res = message_dialog_new_multi(bfwin->main_window,GTK_MESSAGE_QUESTION,
      												buttons,_("Remote DTD file"),
      												_("It seems this is remote DTD.\nIt can take some time to load.\nContinue?"));
      										if ( res == 0 ) {
      											g_object_set_data (G_OBJECT (tag), "loaded", NULL);
      											if (tags) g_slist_free (tags);      												
      										 	return FALSE;
      										} 			
      										infb_convert_dtd(doc);
      									} 
											else				
      										infb_convert_dtd(doc);
      									xmlFree(text);
      								}	
      							} else xmlFree(text);
      						}
      					}
      					infb_v.currentNode = auxnode;
		      			infb_v.currentDoc = doc;
							infb_fill_doc(bfwin,NULL);      				
      				}				
      			}
      		}
      		break;	
      	} /* fileref */
      	else if ( aux == &infb_v.nt_group ) { /* group */
				aux = g_object_get_data(G_OBJECT(tag),"node");
				if ( aux ) {
					infb_v.currentNode = NULL;
					auxnode = (xmlNodePtr)aux;
					text = xmlGetProp(auxnode,BAD_CAST "expanded");
					if ( !text ) {
						xmlSetProp(auxnode,BAD_CAST "expanded",BAD_CAST "0");
						text = xmlGetProp(auxnode,BAD_CAST "expanded");
					}
					if ( xmlStrcmp(text,BAD_CAST "1") == 0 )
						xmlSetProp(auxnode,BAD_CAST "expanded",BAD_CAST "0");
					else
						xmlSetProp(auxnode,BAD_CAST "expanded",BAD_CAST "1");
					/*offset = gtk_text_iter_get_line(&iter);*/
					infb_fill_doc( bfwin,NULL );
					/*gtk_text_buffer_get_iter_at_line (buffer, &iter, offset);
					gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(gui->docview),&iter,0.0,FALSE,0.0,0.0);
					gtk_widget_grab_focus(gui->docview);*/
					xmlFree(text);
				}
      	} /* group */
      	else if ( aux == &infb_v.nt_node ) { /* node */
				aux = g_object_get_data(G_OBJECT(tag),"node");
				if ( aux ) {
					infb_v.currentNode = (xmlNodePtr)aux;
					infb_fill_doc( bfwin, (xmlNodePtr)aux );
				}
			} /* node */
			
	   }
		if (tags) g_slist_free (tags);		
   }
	return FALSE;
}  

static void infb_idx_clicked(GtkButton *button, gpointer data) {
	if ( !infb_v.currentDoc || !data) return;
	infb_fill_doc(BFWIN(data),NULL);
}

static void infb_midx_clicked(GtkButton *button, gpointer data) {
	if ( !data ) return;
	if (infb_v.currentDoc!=NULL && infb_v.currentDoc!=infb_v.homeDoc)
		xmlFreeDoc(infb_v.currentDoc);
	infb_v.currentDoc = infb_v.homeDoc;
	infb_v.currentNode = NULL;
	infb_fill_doc(BFWIN(data),NULL);
}

static void infb_save_clicked(GtkButton *button, gpointer data) {
	gchar *pstr;
	gchar *userdir = g_strconcat(g_get_home_dir(), "/."PACKAGE"/", NULL);
	FILE *f;
	xmlBufferPtr buff;
	xmlChar *text; 
	
	if ( !data ) return;
	if (infb_v.currentNode!=NULL) {
		if ( xmlStrcmp(infb_v.currentNode->name,BAD_CAST "element")==0 ||
			  xmlStrcmp(infb_v.currentNode->name,BAD_CAST "ref")==0 )
			text = xmlGetProp(infb_v.currentNode,BAD_CAST "name");
		else 	if ( xmlStrcmp(infb_v.currentNode->name,BAD_CAST "note")==0 ||
					  xmlStrcmp(infb_v.currentNode->name,BAD_CAST "search_result")==0	  )
			text = xmlGetProp(infb_v.currentNode,BAD_CAST "title");
		else 
			text = xmlStrdup(BAD_CAST "unknown");	
		pstr = g_strdup_printf("%s/bfrag_%s_%ld",userdir,
										(gchar*)text,(long int)time(NULL));
		xmlFree(text);									
		f = fopen(pstr,"w");
		buff = xmlBufferCreate();
		xmlNodeDump(buff,infb_v.currentDoc,infb_v.currentNode,1,1);
		xmlBufferDump(f,buff);
		xmlBufferFree(buff);
		fclose(f);
		g_free(pstr);
		infb_load_fragments((Tinfbwin*)data);
		message_dialog_new(((Tinfbwin*)data)->bfwin->main_window,GTK_MESSAGE_INFO,GTK_BUTTONS_CLOSE,_("Fragment saved"),"");
	}
	g_free(userdir);
}



/*
static void infb_add_clicked(GtkButton *button, gpointer data) {
	Tbfwin *bfwin;
	if ( !data ) return;
	bfwin = BFWIN(data);
	infb_add_item(bfwin);
}

static void infb_del_clicked(GtkButton *button, gpointer data) {
	Tbfwin *bfwin;
	if ( !data ) return;
	bfwin = BFWIN(data);
	infb_del_item(bfwin);
}
*/

gboolean infb_search_keypress (GtkWidget *widget,GdkEventKey *event,Tbfwin *bfwin) {
	gchar *txt,*str;
	xmlXPathObjectPtr result;	
	gint i;
	xmlNodePtr node,node2;
	gboolean found = FALSE;
	
	if ( event->keyval != GDK_Return )	return FALSE;
	if (infb_v.currentDoc == NULL ) return FALSE; 
	txt = (gchar*)gtk_entry_get_text(GTK_ENTRY(widget));
	if ( txt && strcmp(txt,"")!=0) {
	
		node = xmlNewDocNode(infb_v.currentDoc,NULL,BAD_CAST "search_result",NULL);
		str = g_strconcat("Search: ",txt,NULL);
		xmlNewProp(node,BAD_CAST "title",BAD_CAST str);
		g_free(str);

		switch (infb_v.currentType) {
			default: /* fref2 */
				str = g_strconcat("/descendant::element[contains(@name,\"",txt,"\")]",NULL);
				result = getnodeset(infb_v.currentDoc,BAD_CAST str,NULL);
				g_free(str);
				if (result) {
					found = TRUE;
					for(i=0;i<result->nodesetval->nodeNr;i++) {						
						node2 = xmlDocCopyNode(result->nodesetval->nodeTab[i],infb_v.currentDoc,1);
						xmlAddChild(node,node2);
					}
				}
				str = g_strconcat("/descendant::note[contains(@title,\"",txt,"\") and local-name(..)!=\"element\"]",NULL);
				result = getnodeset(infb_v.currentDoc,BAD_CAST str,NULL);
				g_free(str);
				if (result) {
					found = TRUE;
					for(i=0;i<result->nodesetval->nodeNr;i++) {
						node2 = xmlDocCopyNode(result->nodesetval->nodeTab[i],infb_v.currentDoc,1);
						xmlAddChild(node,node2);
					}
				}				
								
			break;
		} /* switch */
		
		if ( found ) {		
			node2 = xmlDocGetRootElement(infb_v.currentDoc);
			xmlAddChild(node2,node);	
			infb_fill_doc(bfwin,node);
		}	
		else {
			message_dialog_new(bfwin->main_window,GTK_MESSAGE_INFO,GTK_BUTTONS_CLOSE,_("Nothing found"),"");
			xmlFreeNode(node);
		}	 
	}
	return FALSE;
} 

void infb_sidepanel_initgui(Tbfwin *bfwin) {
	Tinfbwin *win;
	GtkWidget *scrolwin, *hbox, *vbox;

	win = g_new0(Tinfbwin,1);
	win->bfwin = bfwin;
	g_hash_table_insert(infb_v.windows,bfwin,win);
	win->hand_cursor = gdk_cursor_new (GDK_HAND2);
	win->regular_cursor = gdk_cursor_new (GDK_XTERM);
	win->hovering_over_link = FALSE;		

    vbox = gtk_vbox_new(FALSE, 1);		

	hbox = gtk_toolbar_new();
	gtk_toolbar_set_icon_size(GTK_TOOLBAR(hbox),GTK_ICON_SIZE_MENU);
	gtk_toolbar_set_style(GTK_TOOLBAR(hbox),GTK_TOOLBAR_ICONS);
	
	win->btn_home = gtk_tool_button_new(gtk_image_new_from_stock(GTK_STOCK_HOME,GTK_ICON_SIZE_MENU),"");
	g_signal_connect(G_OBJECT(win->btn_home),"clicked",G_CALLBACK(infb_midx_clicked),bfwin);
	gtk_tool_item_set_tooltip(GTK_TOOL_ITEM(win->btn_home),main_v->tooltips,_("Documentation index"),"");
	gtk_toolbar_insert(GTK_TOOLBAR(hbox),win->btn_home,0);

	win->btn_up = gtk_tool_button_new(gtk_image_new_from_stock(GTK_STOCK_GO_UP,GTK_ICON_SIZE_MENU),"");
	g_signal_connect(G_OBJECT(win->btn_up),"clicked",G_CALLBACK(infb_idx_clicked),bfwin);
	gtk_tool_item_set_tooltip(GTK_TOOL_ITEM(win->btn_up),main_v->tooltips,_("Up to document index"),"");
	gtk_toolbar_insert(GTK_TOOLBAR(hbox),win->btn_up,1);
	
	win->saved = gtk_menu_tool_button_new(gtk_image_new_from_stock(GTK_STOCK_FLOPPY,GTK_ICON_SIZE_MENU),"");
	g_signal_connect(G_OBJECT(win->saved),"clicked",G_CALLBACK(infb_save_clicked),win);
	gtk_tool_item_set_tooltip(GTK_TOOL_ITEM(win->saved),main_v->tooltips,_("Save current view"),"");
	gtk_menu_tool_button_set_arrow_tooltip(GTK_MENU_TOOL_BUTTON(win->saved),main_v->tooltips,_("Go to sekected fragment"),"");
	gtk_toolbar_insert(GTK_TOOLBAR(hbox),win->saved,2);


		
	/*win->btn_add = gtk_button_new();
	gtk_container_add(GTK_CONTAINER(win->btn_add), gtk_image_new_from_stock(GTK_STOCK_NEW,GTK_ICON_SIZE_MENU));
	g_signal_connect (win->btn_add, "clicked",G_CALLBACK (infb_add_clicked), bfwin);
	gtk_tooltips_set_tip(main_v->tooltips, win->btn_add, _("Add document"), "");
	gtk_box_pack_end(GTK_BOX(box3), win->btn_add, FALSE, FALSE, 2);	
	win->btn_del = gtk_button_new();
	gtk_container_add(GTK_CONTAINER(win->btn_del), gtk_image_new_from_stock(GTK_STOCK_DELETE,GTK_ICON_SIZE_MENU));
	g_signal_connect (win->btn_del, "clicked",G_CALLBACK (infb_del_clicked), bfwin);
	gtk_tooltips_set_tip(main_v->tooltips, win->btn_del, _("Remove document"), "");
	gtk_box_pack_end(GTK_BOX(box3), win->btn_del, FALSE, FALSE, 2);*/	

	gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 2);

	win->view = gtk_text_view_new();
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(win->view),GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_editable(GTK_TEXT_VIEW(win->view),FALSE);
	gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(win->view),FALSE);
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(win->view),8);
	gtk_text_view_set_right_margin(GTK_TEXT_VIEW(win->view),8);
	g_signal_connect (win->view, "motion-notify-event",G_CALLBACK (infb_motion_notify_event), bfwin);
	g_signal_connect (win->view, "button-release-event",G_CALLBACK (infb_button_release_event), bfwin);
	
	scrolwin = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolwin), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scrolwin), GTK_SHADOW_IN);
	gtk_container_add(GTK_CONTAINER(scrolwin), win->view);	
	gtk_box_pack_start(GTK_BOX(vbox), scrolwin, TRUE, TRUE, 4);

    hbox = gtk_hbox_new(FALSE, 1);
	win->sentry = gtk_entry_new();	
	gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new(_("Find")), FALSE, TRUE, 2);
	gtk_box_pack_start(GTK_BOX(hbox), win->sentry, TRUE, TRUE, 2);
	g_signal_connect(win->sentry, "key-press-event",G_CALLBACK(infb_search_keypress), bfwin);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 2);
	
	gtk_notebook_insert_page_menu(GTK_NOTEBOOK(bfwin->leftpanel_notebook),vbox,
									gtk_image_new_from_stock(GTK_STOCK_INFO,GTK_ICON_SIZE_LARGE_TOOLBAR),
									gtk_label_new(_("Info Browser")),-1);

	infb_load();
	infb_load_fragments(win);
	infb_v.currentDoc = infb_v.homeDoc;
	infb_fill_doc(bfwin,NULL);		
}

void infb_sidepanel_destroygui(Tbfwin *bfwin) {

}
