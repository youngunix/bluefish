#include <gtk/gtk.h>
#include <string.h>
#include <gdk/gdkkeysyms.h>

/* #define DEBUG */

#include "fref.h"
#include "rcfile.h" /* array_from_arglist() */
#include "stringlist.h"
#include "bluefish.h"
#include "document.h"
#include "bf_lib.h"
#include "gtk_easy.h"

typedef struct {
	GtkWidget *tree;
	GtkTreeStore *store;
/*	GtkWidget *info_window;*/
	GCompletion *autocomplete;
	GtkListStore *autostore;
	GtkWidget *auto_list;
	GtkTooltips *argtips;
	GtkWidget *infocheck;	
	GtkWidget *infoview;
	GtkWidget *infoscroll;	
} Tfref_data;

typedef struct {
  gchar *name;
  gchar *description; 
} Tfref_name_data;

static Tfref_data fref_data = { NULL, NULL, NULL, NULL, NULL, NULL };

/* CONFIG FILE PARSER */

static GMarkupParser FRParser = {
	fref_loader_start_element,
	fref_loader_end_element,
	fref_loader_text,
	NULL,
	fref_loader_error
};

/* AUXILIARY FILE PARSER */

static GMarkupParser FRNameParser = {
	fref_name_loader_start_element,
	NULL,
	NULL,
	NULL,
	fref_loader_error
};

void fref_name_loader_start_element(GMarkupParseContext * context,
							   const gchar * element_name,
							   const gchar ** attribute_names,
							   const gchar ** attribute_values,
							   gpointer user_data, GError ** error)
{
	GHashTable *attrs;
 int i;
 Tfref_name_data *data;
 gchar *tmps;
 
 if (user_data==NULL) return;
 data = (Tfref_name_data*)user_data;
 
 	attrs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	i = 0;
	while (attribute_names[i] != NULL) {
		g_hash_table_insert(attrs, (gpointer) g_strdup(attribute_names[i]),
							(gpointer) g_strdup(attribute_values[i]));
		i++;
	}
	 	
	if (strcmp(element_name, "ref") == 0) {
	   tmps = g_hash_table_lookup(attrs, "name");
	   if (tmps!=NULL)
	      data->name = g_strdup(tmps);
	   tmps = g_hash_table_lookup(attrs, "description");   
	   if (tmps!=NULL)
	      data->description = g_strdup(tmps); 
	}
		
}							   


gchar *fref_xml_get_refname(gchar *filename)
{
	GMarkupParseContext *ctx;
	gchar *config;
	gsize len;
	Tfref_name_data *aux;
	gchar *tmps;

 if (filename==NULL) return NULL; 
 
	aux = g_new0(Tfref_name_data, 1);
	ctx = g_markup_parse_context_new(&FRNameParser, (GMarkupParseFlags) 0,
									 (gpointer) aux, NULL);
	if (ctx == NULL)
		return NULL;
	if (!g_file_get_contents(filename, &config, &len, NULL))
		/* I think we need to free ctx before returning NULL, don't we?*/
		return NULL;
	if (!g_markup_parse_context_parse(ctx, config, len, NULL))
		/* and also here ?? free ctx first? */
		return NULL;
	g_markup_parse_context_free(ctx);
	tmps = aux->name;
	g_free(aux);
	g_free(config);
	return tmps;
}
							  


void fref_loader_start_element(GMarkupParseContext * context,
							   const gchar * element_name,
							   const gchar ** attribute_names,
							   const gchar ** attribute_values,
							   gpointer user_data, GError ** error)
{
	FRParseAux *aux;
	FRInfo *info;
	FRAttrInfo *tmpa;
	FRParamInfo *tmpp;
	GHashTable *attrs;
	int i;
	gpointer pomstr;
	GtkTreeIter iter;

	if (user_data == NULL)
		return;
	aux = (FRParseAux *) user_data;
	attrs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	i = 0;
	while (attribute_names[i] != NULL) {
		g_hash_table_insert(attrs, (gpointer) g_strdup(attribute_names[i]),
							(gpointer) g_strdup(attribute_values[i]));
		i++;
	}

	switch (aux->state) {
	
	case FR_LOADER_STATE_NONE:
		if (strcmp(element_name, "group") == 0) {
		 if (aux->nest_level < MAX_NEST_LEVEL)
		 {
     gtk_tree_store_append(aux->store, &iter, &aux->parent);
     /* you set the value in the tree with g_strdup(), so you allocate new memory for it, 
     but you can never free that memory. Do you really need to use g_strdup() here ? */
     gtk_tree_store_set(aux->store, &iter, STR_COLUMN, 
 			                   g_strdup(g_hash_table_lookup(attrs, "name")),
 			                   FILE_COLUMN,NULL,PTR_COLUMN,NULL,-1);
     aux->grp_parent[aux->nest_level] = aux->parent;							             
     aux->parent = iter;
    } else g_warning("Maximum nesting level reached!");       
    (aux->nest_level)++;
 
		} else if (strcmp(element_name, "tag") == 0) {
			aux->state = FR_LOADER_STATE_TAG;
			aux->pstate = FR_LOADER_STATE_NONE;
			info = g_new0(FRInfo, 1);
			info->type = FR_TYPE_TAG;
			info->name = g_strdup(g_hash_table_lookup(attrs, "name"));
			gtk_tree_store_append(aux->store, &iter, &aux->parent);
			gtk_tree_store_set(aux->store, &iter, STR_COLUMN, info->name,
							   PTR_COLUMN, info, FILE_COLUMN, NULL, -1);
			aux->act_info = info;
		} else if (strcmp(element_name, "function") == 0) {
			aux->state = FR_LOADER_STATE_FUNC;
			aux->pstate = FR_LOADER_STATE_NONE;
			info = g_new0(FRInfo, 1);
			info->type = FR_TYPE_FUNCTION;
			info->name = g_strdup(g_hash_table_lookup(attrs, "name"));
			gtk_tree_store_append(aux->store, &iter, &aux->parent);
			gtk_tree_store_set(aux->store, &iter, STR_COLUMN, info->name,
							   PTR_COLUMN, info, -1);
			aux->act_info = info;
		} else if (strcmp(element_name, "class") == 0) {
			aux->state = FR_LOADER_STATE_CLASS;
			aux->pstate = FR_LOADER_STATE_NONE;
			info = g_new0(FRInfo, 1);
			info->type = FR_TYPE_CLASS;
			info->name = g_strdup(g_hash_table_lookup(attrs, "name"));
			gtk_tree_store_append(aux->store, &iter, &aux->parent);
			gtk_tree_store_set(aux->store, &iter, STR_COLUMN, info->name,
							   PTR_COLUMN, info, -1);
			aux->act_info = info;
		} else if (strcmp(element_name, "ref") == 0) {
		} else
			g_warning("FREF Config Error: Unknown element");
		break;					/* state NONE */
		
				
	case FR_LOADER_STATE_TAG:
		if (strcmp(element_name, "description") == 0) {
			aux->state = FR_LOADER_STATE_DESCR;
			aux->pstate = FR_LOADER_STATE_TAG;
		} else if (strcmp(element_name, "tip") == 0) {
			aux->state = FR_LOADER_STATE_TIP;
			aux->pstate = FR_LOADER_STATE_TAG;
		} else if (strcmp(element_name, "attribute") == 0) {
			aux->state = FR_LOADER_STATE_ATTR;
			aux->pstate = FR_LOADER_STATE_TAG;
			tmpa = g_new0(FRAttrInfo, 1);
			tmpa->name = g_strdup(g_hash_table_lookup(attrs, "name"));
			tmpa->title = g_strdup(g_hash_table_lookup(attrs, "title"));
			pomstr = g_hash_table_lookup(attrs, "required");
			if (pomstr != NULL
				&& (g_strcasecmp(pomstr, "y") == 0
					|| g_strcasecmp(pomstr, "1") == 0))
				tmpa->required = TRUE;
			else
				tmpa->required = FALSE;
			pomstr = g_hash_table_lookup(attrs, "vallist");
			if (pomstr != NULL
				&& (g_strcasecmp(pomstr, "y") == 0
					|| g_strcasecmp(pomstr, "1") == 0))
				tmpa->has_list = TRUE;
			else
				tmpa->has_list = FALSE;
			tmpa->def_value =
				g_strdup(g_hash_table_lookup(attrs, "default"));
			aux->act_attr = tmpa;
			aux->act_info->attributes =
				g_list_append(aux->act_info->attributes, tmpa);
		} else if (strcmp(element_name, "dialog") == 0) {
			aux->state = FR_LOADER_STATE_DIALOG;
			aux->pstate = FR_LOADER_STATE_TAG;
			aux->act_info->dialog_title =
				g_strdup(g_hash_table_lookup(attrs, "title"));
		} else if (strcmp(element_name, "info") == 0) {
			aux->state = FR_LOADER_STATE_INFO;
			aux->pstate = FR_LOADER_STATE_TAG;
			aux->act_info->info_title =
				g_strdup(g_hash_table_lookup(attrs, "title"));
		} else if (strcmp(element_name, "insert") == 0) {
			aux->state = FR_LOADER_STATE_INSERT;
			aux->pstate = FR_LOADER_STATE_TAG;
		} else
			g_warning("FREF Config Error: Unknown tag (%s)", element_name);
		break;					/* state TAG */
	case FR_LOADER_STATE_FUNC:
		if (strcmp(element_name, "description") == 0) {
			aux->state = FR_LOADER_STATE_DESCR;
			aux->pstate = FR_LOADER_STATE_FUNC;
		} else if (strcmp(element_name, "tip") == 0) {
			aux->state = FR_LOADER_STATE_TIP;
			aux->pstate = FR_LOADER_STATE_FUNC;
		} else if (strcmp(element_name, "param") == 0) {
			aux->state = FR_LOADER_STATE_PARAM;
			aux->pstate = FR_LOADER_STATE_FUNC;
			tmpp = g_new0(FRParamInfo, 1);
			tmpp->name = g_strdup(g_hash_table_lookup(attrs, "name"));
			tmpp->title = g_strdup(g_hash_table_lookup(attrs, "title"));
			pomstr = g_hash_table_lookup(attrs, "required");
			if (pomstr != NULL
				&& (g_strcasecmp(pomstr, "y") == 0
					|| g_strcasecmp(pomstr, "1") == 0))
				tmpp->required = TRUE;
			else
				tmpp->required = FALSE;
			pomstr = g_hash_table_lookup(attrs, "vallist");
			if (pomstr != NULL
				&& (g_strcasecmp(pomstr, "y") == 0
					|| g_strcasecmp(pomstr, "1") == 0))
				tmpp->has_list = TRUE;
			else
				tmpp->has_list = FALSE;
			tmpp->def_value =
				g_strdup(g_hash_table_lookup(attrs, "default"));
			tmpp->type = g_strdup(g_hash_table_lookup(attrs, "type"));
			aux->act_param = tmpp;
			aux->act_info->params =
				g_list_append(aux->act_info->params, tmpp);
		} else if (strcmp(element_name, "return") == 0) {
			aux->state = FR_LOADER_STATE_RETURN;
			aux->pstate = FR_LOADER_STATE_FUNC;
			aux->act_info->return_type =
				g_strdup(g_hash_table_lookup(attrs, "type"));
		} else if (strcmp(element_name, "dialog") == 0) {
			aux->state = FR_LOADER_STATE_DIALOG;
			aux->pstate = FR_LOADER_STATE_FUNC;
			aux->act_info->dialog_title =
				g_strdup(g_hash_table_lookup(attrs, "title"));
		} else if (strcmp(element_name, "info") == 0) {
			aux->state = FR_LOADER_STATE_INFO;
			aux->pstate = FR_LOADER_STATE_FUNC;
			aux->act_info->info_title =
				g_strdup(g_hash_table_lookup(attrs, "title"));
		} else if (strcmp(element_name, "insert") == 0) {
			aux->state = FR_LOADER_STATE_INSERT;
			aux->pstate = FR_LOADER_STATE_FUNC;
		} else
			g_warning("FREF Config Error: Unknown tag (%s)", element_name);
		break;					/* state FUNC */
	case FR_LOADER_STATE_ATTR:
		if (strcmp(element_name, "vallist") == 0) {
			aux->state = FR_LOADER_STATE_VALLIST;
			aux->vstate = FR_LOADER_STATE_ATTR;
		} else
			g_warning("FREF Config Error: Unknown tag (%s)", element_name);
		break;					/* state ATTR */
	case FR_LOADER_STATE_PARAM:
		if (strcmp(element_name, "vallist") == 0) {
			aux->state = FR_LOADER_STATE_VALLIST;
			aux->vstate = FR_LOADER_STATE_PARAM;
		} else
			g_warning("FREF Config Error: Unknown tag (%s)", element_name);
		break;					/* state PARAM */

	}							/* switch */
	/* you destroy the hashtable, but you don't free the content of the hashtable, 
	I guess we need to do that ourselves.. */
	g_hash_table_destroy(attrs);
}

void fref_loader_end_element(GMarkupParseContext * context,
							 const gchar * element_name,
							 gpointer user_data, GError ** error)
{
	FRParseAux *aux;

	if (user_data == NULL)
		return;
	aux = (FRParseAux *) user_data;
	switch (aux->state) {
	case FR_LOADER_STATE_TAG:
		if (strcmp(element_name, "tag") == 0) {
			aux->act_info = NULL;
			aux->pstate = FR_LOADER_STATE_NONE;
		}
		break;					/* tag */
	case FR_LOADER_STATE_FUNC:
		if (strcmp(element_name, "function") == 0) {
			aux->act_info = NULL;
			aux->pstate = FR_LOADER_STATE_NONE;
		}
		break;					/* function */
	case FR_LOADER_STATE_ATTR:
		if (strcmp(element_name, "attribute") == 0) {
			aux->act_attr = NULL;
			aux->pstate = FR_LOADER_STATE_TAG;
		}
		break;					/* attribute */
	case FR_LOADER_STATE_PARAM:
		if (strcmp(element_name, "param") == 0) {
			aux->act_param = NULL;
			aux->pstate = FR_LOADER_STATE_FUNC;
		}
		break;					/* param */
	}							/* switch */
	
	if (aux->state != FR_LOADER_STATE_VALLIST)
		aux->state = aux->pstate;
	else
		aux->state = aux->vstate;
		
if (strcmp(element_name, "group") == 0) {		
 if (aux->nest_level>0)
	  {
 	    (aux->nest_level)--;	    
	    if (aux->nest_level < MAX_NEST_LEVEL) 
	       aux->parent = aux->grp_parent[aux->nest_level];
	  }  
  }
  
}

void fref_loader_text(GMarkupParseContext * context, const gchar * _text,
					  gsize _text_len, gpointer user_data, GError ** error)
{
	FRParseAux *aux;
	gchar *text;
	gint text_len;

	if (user_data == NULL && _text == NULL)
		return;
	/* remove white spaces from the begining and the end */
	text = g_strdup(_text);
	text = g_strstrip(text);
	text_len = strlen(text);
	
	aux = (FRParseAux *) user_data;
	switch (aux->state) {
	case FR_LOADER_STATE_DESCR:
		aux->act_info->description = g_strndup(text, text_len);
		break;
	case FR_LOADER_STATE_TIP:
		aux->act_info->tip = g_strndup(text, text_len);
		aux->autoitems = g_list_append(aux->autoitems, aux->act_info->tip);
		break;
	case FR_LOADER_STATE_ATTR:
		aux->act_attr->description = g_strndup(text, text_len);
		break;
	case FR_LOADER_STATE_PARAM:
		aux->act_param->description = g_strndup(text, text_len);
		break;
	case FR_LOADER_STATE_RETURN:
		aux->act_info->return_description = g_strndup(text, text_len);
		break;
	case FR_LOADER_STATE_VALLIST:
		if (aux->vstate == FR_LOADER_STATE_ATTR)
			aux->act_attr->values = g_strndup(text, text_len);
		else if (aux->vstate == FR_LOADER_STATE_PARAM)
			aux->act_param->values = g_strndup(text, text_len);
		else
			g_warning("FREF Config Error: cannot assign vallist");
		break;
	case FR_LOADER_STATE_DIALOG:
		aux->act_info->dialog_text = g_strndup(text, text_len);
		break;
	case FR_LOADER_STATE_INFO:
		aux->act_info->info_text = g_strndup(text, text_len);
		break;
	case FR_LOADER_STATE_INSERT:
		aux->act_info->insert_text = g_strndup(text, text_len);
		break;
	}							/* switch */
	g_free(text);
}

void fref_loader_error(GMarkupParseContext * context, GError * error,
					   gpointer user_data)
{
	if (error != NULL)
		g_warning("FREF Config Error: %s", error->message);
	else
		g_warning("FREF Config Error: Unknown Error");
}

void fref_loader_load_ref_xml(gchar * filename, GtkWidget * tree,
							  GtkTreeStore * store, GtkTreeIter * parent)
{
	GMarkupParseContext *ctx;
	gchar *config;
	gsize len;
	FRParseAux *aux;

 if (filename==NULL) return; 
	aux = g_new0(FRParseAux, 1);
	aux->tree = tree;
	aux->store = store;
	aux->state = FR_LOADER_STATE_NONE;
	aux->parent = *parent;
	aux->nest_level = 0;
	aux->autoitems = NULL;
	
	ctx = g_markup_parse_context_new(&FRParser, (GMarkupParseFlags) 0,
									 (gpointer) aux, NULL);
	if (ctx == NULL)
		return;
	if (!g_file_get_contents(filename, &config, &len, NULL))
		return;
	if (!g_markup_parse_context_parse(ctx, config, len, NULL))
		return;
	g_markup_parse_context_free(ctx);
	if (aux->autoitems != NULL)
		g_completion_add_items(fref_data.autocomplete, aux->autoitems);
	g_free(config);
	/* you free aux here, but during the parsing aux probably got many data attached 
	to it that is not freed yet, shouldn't you check the data and free that if it is there? 
	*/
	g_free(aux);
}

void fref_loader_unload_ref(GtkWidget * tree, GtkTreeStore * store,
							GtkTreeIter * position)
{
	GValue *val;
	FRInfo *entry;
	GtkTreeIter iter;
	GtkTreePath *path;

 path = gtk_tree_model_get_path(GTK_TREE_MODEL(store),position);
 if (gtk_tree_path_get_depth(path)>1) return;
 
	while (gtk_tree_model_iter_nth_child
		   (GTK_TREE_MODEL(store), &iter, position, 0)) {
		val = g_new0(GValue, 1);
		gtk_tree_model_get_value(GTK_TREE_MODEL(store), &iter, 1, val);
		if (G_IS_VALUE(val) && g_value_peek_pointer(val) != NULL) {
			entry = (FRInfo *) g_value_peek_pointer(val);
			if (entry != NULL) {
				fref_free_info(entry);
			}
		}
		gtk_tree_store_remove(store, &iter);
		g_free(val);
	}							/* while */
}

void fref_loader_unload_all(GtkWidget * tree, GtkTreeStore * store)
{
	GValue *val;
	GtkTreeIter iter;

	while (gtk_tree_model_iter_nth_child
		   (GTK_TREE_MODEL(store), &iter, NULL, 0)) {
		fref_loader_unload_ref(tree, store, &iter);
		val = g_new0(GValue, 1);
		gtk_tree_model_get_value(GTK_TREE_MODEL(store), &iter, 2, val);
		if (G_IS_VALUE(val) && g_value_peek_pointer(val) != NULL) {
			g_free(g_value_peek_pointer(val));
		}
		gtk_tree_store_remove(store, &iter);
		g_free(val);
	}							/* while */
}



void fref_free_info(FRInfo * info)
{
	FRAttrInfo *tmpa;
	FRParamInfo *tmpp;
	GList *lst;

	if (info->name)
		g_free(info->name);
	if (info->description)
		g_free(info->description);
	if (info->tip)
		g_free(info->tip);
	if (info->return_type)
		g_free(info->return_type);
	if (info->return_description)
		g_free(info->return_description);
	if (info->info_text)
		g_free(info->info_text);
	if (info->info_title)
		g_free(info->info_title);
	if (info->dialog_text)
		g_free(info->dialog_text);
	if (info->dialog_title)
		g_free(info->dialog_title);
	if (info->insert_text)
		g_free(info->insert_text);
	if (info->attributes) {
		lst = g_list_first(info->attributes);
		while (lst) {
			tmpa = (FRAttrInfo *) lst->data;
			if (tmpa->name)
				g_free(tmpa->name);
			if (tmpa->title)
				g_free(tmpa->title);
			if (tmpa->description)
				g_free(tmpa->description);
			if (tmpa->def_value)
				g_free(tmpa->def_value);
			if (tmpa->values)
				g_free(tmpa->values);
			lst = g_list_next(lst);
		}
	}							/* attributes */
	if (info->params) {
		lst = g_list_first(info->params);
		while (lst) {
			tmpp = (FRParamInfo *) lst->data;
			if (tmpp->name)
				g_free(tmpp->name);
			if (tmpp->title)
				g_free(tmpp->title);
			if (tmpp->description)
				g_free(tmpp->description);
			if (tmpp->def_value)
				g_free(tmpp->def_value);
			if (tmpp->type)
				g_free(tmpp->type);
			if (tmpp->values)
				g_free(tmpp->values);
			lst = g_list_next(lst);
		}
	}


	/* params */
	/* methods */
}



/* END OF CONFIG PARSER */

/* GUI */
static void fill_toplevels(gboolean empty_first) {
	GList *reflist;
	
	if (empty_first) {
		gtk_tree_store_clear(fref_data.store);
	}
	/* prepare first nodes - read from configuration data */
	reflist = g_list_first(main_v->props.reference_files);
	while (reflist) {
		gchar **tmparray = reflist->data;
		if (count_array(tmparray) == 2) {
			if (file_exists_and_readable(tmparray[1])) {
				GtkTreeIter iter;
				GtkTreeIter iter2;
				gtk_tree_store_append(fref_data.store, &iter, NULL);
				gtk_tree_store_set(fref_data.store, &iter, STR_COLUMN,
							   tmparray[0], PTR_COLUMN, NULL, FILE_COLUMN,
							   tmparray[1], -1);
				/* dummy node for expander display */
				gtk_tree_store_append(fref_data.store, &iter2, &iter);
			}
		}
		reflist = g_list_next(reflist);
	}
}

GtkWidget *fref_init() {
	GtkWidget *scroll,*box,*pane,*box2,*btn1,*btn2;
	GtkCellRenderer *cell;
	GtkTreeViewColumn *column;

	pane = gtk_vpaned_new();
	box = gtk_vbox_new(FALSE,1);
	box2 = gtk_hbox_new(FALSE,1);
	
	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,GTK_POLICY_AUTOMATIC);
	fref_data.infoscroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(fref_data.infoscroll), GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
	
	fref_data.store =
		gtk_tree_store_new(N_COLUMNS, G_TYPE_STRING, G_TYPE_POINTER,
						   G_TYPE_STRING);
	fref_data.tree =
		gtk_tree_view_new_with_model(GTK_TREE_MODEL(fref_data.store));
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(fref_data.tree), FALSE);
	cell = gtk_cell_renderer_text_new();
	column =
		gtk_tree_view_column_new_with_attributes("", cell, "text",
												 STR_COLUMN, NULL);
	gtk_tree_view_append_column(GTK_TREE_VIEW(fref_data.tree), column);

	gtk_container_add(GTK_CONTAINER(scroll), fref_data.tree);
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(fref_data.tree), FALSE);	

	fill_toplevels(FALSE);

	/* autocompletion */
	fref_data.autocomplete = g_completion_new(NULL);

	g_signal_connect(G_OBJECT(fref_data.tree), "row-collapsed",
					 G_CALLBACK(frefcb_row_collapsed), fref_data.store);
	g_signal_connect(G_OBJECT(fref_data.tree), "row-expanded",
					 G_CALLBACK(frefcb_row_expanded), fref_data.store);
	g_signal_connect(G_OBJECT(fref_data.tree), "button-press-event",
					 G_CALLBACK(frefcb_event_mouseclick), fref_data.tree);
	g_signal_connect(G_OBJECT(fref_data.tree), "key-press-event",
					 G_CALLBACK(frefcb_event_keypress), fref_data.tree);

	gtk_widget_show(fref_data.tree);
	gtk_widget_show(scroll);

	fref_data.argtips = gtk_tooltips_new();

	fref_data.infoview = gtk_label_new(NULL);
	gtk_label_set_line_wrap(GTK_LABEL(fref_data.infoview),TRUE);
	gtk_label_set_use_markup(GTK_LABEL(fref_data.infoview),TRUE); 	
	gtk_misc_set_alignment(GTK_MISC(fref_data.infoview), 0.0, 0.0);
	gtk_misc_set_padding(GTK_MISC(fref_data.infoview), 5, 5);

	g_signal_connect(G_OBJECT(fref_data.tree), "cursor-changed",G_CALLBACK(frefcb_cursor_changed), NULL);

	fref_data.infocheck = gtk_check_button_new_with_label (_("Show info window"));
	gtk_widget_show(fref_data.infocheck);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fref_data.infocheck),TRUE);
	g_signal_connect(G_OBJECT(fref_data.infocheck), "toggled",G_CALLBACK(frefcb_infocheck_toggled),NULL);
	btn1 = gtk_button_new_with_label("D");					 
	btn2 = gtk_button_new_with_label("I");					 
	gtk_tooltips_set_tip(fref_data.argtips,btn1,_("Dialog"),"");					 
	gtk_tooltips_set_tip(fref_data.argtips,btn2,_("Info"),"");					  
	g_signal_connect(G_OBJECT(btn1), "clicked",G_CALLBACK(frefcb_info_dialog),NULL);
	g_signal_connect(G_OBJECT(btn2), "clicked",G_CALLBACK(frefcb_full_info),NULL);
	
	gtk_box_pack_start(GTK_BOX(box2),fref_data.infocheck,TRUE,TRUE,0);	 				 
	gtk_box_pack_start(GTK_BOX(box2),btn1,FALSE,TRUE,0);	 				 	
	gtk_box_pack_start(GTK_BOX(box2),btn2,FALSE,TRUE,0);	 				

	gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(fref_data.infoscroll), fref_data.infoview);
	gtk_box_pack_start(GTK_BOX(box),scroll,TRUE,TRUE,0);
	gtk_box_pack_start(GTK_BOX(box),box2,FALSE,TRUE,0);
	gtk_paned_pack1(GTK_PANED(pane),box,TRUE,FALSE);  
	gtk_paned_pack2(GTK_PANED(pane),fref_data.infoscroll,TRUE,TRUE);
	gtk_widget_show_all(pane); 
	return pane;
}

void fref_cleanup() {
	fref_loader_unload_all(fref_data.tree, fref_data.store);
	g_object_unref(G_OBJECT(fref_data.store));
	fref_data.tree = NULL;
	fref_data.store = NULL;
/*	fref_data.info_window = NULL;*/
	g_completion_free(fref_data.autocomplete);
	fref_data.argtips = NULL;
}


/*------------ PREPARE INFO -----------------------*/
gchar *fref_prepare_info(FRInfo * entry, gint infotype,gboolean use_colors)
{
	gchar *ret, *tofree;
	GList *lst;
	FRAttrInfo *tmpa;
	FRParamInfo *tmpp;

	ret = g_strdup("");
	switch (entry->type) {
	case FR_TYPE_TAG:
		   switch (infotype) {
		     case FR_INFO_TITLE:
		            		tofree = ret;
		            		if (use_colors)
		                  ret = g_strconcat(ret, "<span size=\"medium\" foreground=\"#FFFFFF\" >  TAG: <b>",entry->name, "</b></span>", NULL);
		               else
		                  ret = g_strconcat(ret, "<span size=\"medium\">  TAG: <b>",entry->name, "</b></span>", NULL);   
		               g_free(tofree);
		          break;
		     case FR_INFO_DESC:
		            		if (entry->description != NULL) {
                 			tofree = ret;
                 			if (use_colors)
                 			   ret = g_strconcat(ret, "<span size=\"small\"  foreground=\"#FFFFFF\" >       ",entry->description, "</span>", NULL);
                 			else
                 			   ret = g_strconcat(ret, "<span size=\"small\">       ",entry->description, "</span>", NULL);   
              			g_free(tofree);
                		}
		          break;     
		     case FR_INFO_NOTES:
		              if (entry->info_text != NULL) {
                 			tofree = ret;
                 			if (use_colors)
                  		   ret = g_strconcat(ret,"<span size=\"medium\" foreground=\"",FR_COL_4,"\" ><b>NOTES:</b> \n",entry->info_text,"</span>", NULL);
                  		else
                  		   ret = g_strconcat(ret,"<span size=\"medium\"><b>NOTES:</b> \n",entry->info_text,"</span>", NULL);   
			                g_free(tofree);
                	}				     
		          break;     
		     case FR_INFO_ATTRS:
		          lst = g_list_first(entry->attributes);
		          while (lst) {
			                      tmpa = (FRAttrInfo *) lst->data;
			                      tofree = ret;
			                      if (use_colors)
			                         ret = g_strconcat(ret, "<span size=\"small\" >          <b><i>",tmpa->name, "</i></b></span> - <span size=\"small\" foreground=\"",FR_COL_3,"\" >",tmpa->description, "</span>\n", NULL);
			                      else
			                         ret = g_strconcat(ret, "<span size=\"small\" >          <b><i>",tmpa->name, "</i></b></span> - <span size=\"small\">",tmpa->description, "</span>\n", NULL);   
			                      g_free(tofree);
			                      lst = g_list_next(lst);
		          }
		          break;     
		   } /* switch infotype */
		break; /* TAG */
	case FR_TYPE_FUNCTION:
	  switch (infotype)
	  {
	    case FR_INFO_TITLE:
		            		tofree = ret;
		            		if (use_colors)
		                  ret = g_strconcat(ret, "<span size=\"medium\" foreground=\"#FFFFFF\" >  FUNCTION: <b>",entry->name, "</b></span>", NULL);
		               else
		                  ret = g_strconcat(ret, "<span size=\"medium\">  FUNCTION: <b>",entry->name, "</b></span>", NULL);   
		               g_free(tofree);	    
	         break;
	    case FR_INFO_DESC:
		            		if (entry->description != NULL) {
                 			tofree = ret;
                 			if (use_colors)
                 			   ret = g_strconcat(ret, "<span size=\"small\" foreground=\"#FFFFFF\" >       <i>",entry->description, "</i></span>\n", NULL);
                 			else
                 			   ret = g_strconcat(ret, "<span size=\"small\">       <i>",entry->description, "</i></span>\n", NULL);   
              			g_free(tofree);
                		}
		               if (entry->return_type != NULL) {
			                 tofree = ret;
			                 if (use_colors)
			                    ret = g_strconcat(ret,"<span size=\"medium\" foreground=\"",FR_COL_4,"\">       <b>RETURNS: ",entry->return_type, "</b></span>\n", NULL);
			                 else
			                    ret = g_strconcat(ret,"<span size=\"medium\">       <b>RETURNS: ",entry->return_type, "</b></span>\n", NULL);   
			                 g_free(tofree);
		               }
		               if (entry->return_description != NULL) {
			                 tofree = ret;
			                 if (use_colors)
			                    ret = g_strconcat(ret, "<span size=\"small\" foreground=\"",FR_COL_4,"\">       ",entry->return_description, "</span>\n", NULL);
			                 else
			                    ret = g_strconcat(ret, "<span size=\"small\">       ",entry->return_description, "</span>\n", NULL);   
			                 g_free(tofree);
		               }    			    
	         break;     
		     case FR_INFO_NOTES:
		              if (entry->info_text != NULL) {
                 			tofree = ret;
                 			if (use_colors)
                  		   ret = g_strconcat(ret,"<span size=\"medium\" foreground=\"",FR_COL_4,"\"><b>NOTES:</b> \n",entry->info_text,"</span>", NULL);
                  		else
                  		   ret = g_strconcat(ret,"<span size=\"medium\"><b>NOTES:</b> \n",entry->info_text,"</span>", NULL);   
			                g_free(tofree);
                	}				     
		          break;     	         
		    case FR_INFO_ATTRS:
		                  lst = g_list_first(entry->params);
		                  while (lst) {
			                               tmpp = (FRParamInfo *) lst->data;
			                               tofree = ret;
			                               if (tmpp->description!=NULL && 
			                                   tmpp->type!=NULL)
			                               {    
			                                  if (use_colors) 
			                                     ret = g_strconcat(ret, "<span size=\"small\">          <b><i>",tmpp->name, "(", tmpp->type,")</i></b></span> - <span size=\"small\" foreground=\"",FR_COL_3,"\">",tmpp->description, "</span>\n", NULL);
			                                  else
			                                     ret = g_strconcat(ret, "<span size=\"small\">          <b><i>",tmpp->name, "(", tmpp->type,")</i></b></span> - <span size=\"small\">",tmpp->description, "</span>\n", NULL);   
			                               }      
			                               else
			                                  if (tmpp->type!=NULL)
			                                  {
			                                        ret = g_strconcat(ret, "<span size=\"small\">          <b><i>",tmpp->name, "(", tmpp->type,")</i></b></span>\n", NULL);  
		                                   }    
			                                  else
			                                  {
			                                     ret = g_strconcat(ret, "<span size=\"small\">          <b><i>",tmpp->name, "</i></b></span>\n", NULL);        
			                                  }   
			                               g_free(tofree);
			                               lst = g_list_next(lst);
		                  }
		          break;      
	  } /* switch infotype */
 	break;
	}
	return ret;
}

/*------------ SHOW INFO -----------------------*/
static void info_window_close_lcb(GtkWidget *widget, gpointer data) {
	gtk_widget_destroy(data);
}

void fref_show_info(FRInfo * entry, gboolean modal, GtkWidget * parent)
{
	GtkWidget *label_t, *frame, *scroll, *view, *btn1, *btn2, *btn3, *vbox,
		*hbox,*fourbox,*label_d,*label_a,*label_n,*ev_t,*ev_d,*ev_a,*ev_n;
	GtkWidget *info_window;
/*	int x, y, w, h;*/
	GdkColor col1,col2,col4;

 	gdk_color_parse(FR_COL_1,&col1); 
 	gdk_color_parse(FR_COL_2,&col2); 
 	gdk_color_parse(FR_COL_4,&col4); 
 	
 	label_t = gtk_label_new(fref_prepare_info(entry,FR_INFO_TITLE,TRUE));
 	label_d = gtk_label_new(fref_prepare_info(entry,FR_INFO_DESC,TRUE));
 	label_a = gtk_label_new(fref_prepare_info(entry,FR_INFO_ATTRS,TRUE));
 	label_n = gtk_label_new(fref_prepare_info(entry,FR_INFO_NOTES,TRUE));
 	gtk_label_set_use_markup(GTK_LABEL(label_t),TRUE); 
 	gtk_label_set_use_markup(GTK_LABEL(label_d),TRUE);
 	gtk_label_set_use_markup(GTK_LABEL(label_a),TRUE); 	
 	gtk_label_set_use_markup(GTK_LABEL(label_n),TRUE); 	
	gtk_misc_set_alignment(GTK_MISC(label_t), 0.0, 0.0);
	gtk_misc_set_padding(GTK_MISC(label_t), 5, 5);
	gtk_label_set_line_wrap(GTK_LABEL(label_t), TRUE); 	
	gtk_misc_set_alignment(GTK_MISC(label_d), 0.0, 0.0);
	gtk_misc_set_padding(GTK_MISC(label_d), 5, 5);
	gtk_label_set_line_wrap(GTK_LABEL(label_d), TRUE); 	
	gtk_misc_set_alignment(GTK_MISC(label_a), 0.0, 0.0);
	gtk_misc_set_padding(GTK_MISC(label_a), 5, 5);
	gtk_label_set_line_wrap(GTK_LABEL(label_a), TRUE); 	
	gtk_misc_set_alignment(GTK_MISC(label_n), 0.0, 0.0);
	gtk_misc_set_padding(GTK_MISC(label_n), 5, 5);
	gtk_label_set_line_wrap(GTK_LABEL(label_n), TRUE); 	
	ev_t = gtk_event_box_new();
  	gtk_widget_modify_bg(GTK_WIDGET(ev_t),GTK_STATE_NORMAL,&col1);    
	gtk_container_add(GTK_CONTAINER(ev_t),label_t);
	ev_d = gtk_event_box_new();
	gtk_widget_modify_bg(GTK_WIDGET(ev_d),GTK_STATE_NORMAL,&col2);    
	gtk_container_add(GTK_CONTAINER(ev_d),label_d);
	ev_a = gtk_event_box_new();
	gtk_widget_modify_bg(GTK_WIDGET(ev_a),GTK_STATE_NORMAL,&col4);    
	gtk_container_add(GTK_CONTAINER(ev_a),label_a);
	ev_n = gtk_event_box_new();
	gtk_widget_modify_bg(GTK_WIDGET(ev_n),GTK_STATE_NORMAL,&col1);    
	gtk_container_add(GTK_CONTAINER(ev_n),label_n);
 	
	scroll = gtk_scrolled_window_new(NULL, NULL);
	btn2 = NULL;
	btn3 = NULL;
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
								   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	view = gtk_viewport_new(GTK_ADJUSTMENT
						 (gtk_adjustment_new(0, 0, 1024, 1, 100, 200)),
						 GTK_ADJUSTMENT(gtk_adjustment_new
										(0, 0, 768, 1, 100, 200)));
	info_window = window_full2(_("Info"), GTK_WIN_POS_NONE
			, 0, G_CALLBACK(info_window_close_lcb)
			, NULL,TRUE,FALSE);

/*	info_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);*/
	if (modal) {
		gtk_window_set_modal(GTK_WINDOW(info_window), TRUE);
	}
	
	fourbox = gtk_vbox_new(FALSE, 0);
	
	frame = gtk_frame_new(NULL);

	hbox = gtk_hbox_new(TRUE, 5);	
	
	gtk_container_add(GTK_CONTAINER(view), GTK_WIDGET(fourbox)); 
	gtk_box_pack_start(GTK_BOX(fourbox),ev_t,TRUE,TRUE,0);
	gtk_box_pack_start(GTK_BOX(fourbox),ev_d,TRUE,TRUE,0);
	gtk_box_pack_start(GTK_BOX(fourbox),ev_a,TRUE,TRUE,0);
	gtk_box_pack_start(GTK_BOX(fourbox),ev_n,TRUE,TRUE,0);
	
	btn1 = gtk_button_new_with_label(_("Close"));
	gtk_box_pack_start(GTK_BOX(hbox), btn1, TRUE, TRUE, 5);

	if (!modal) {
		btn2 = gtk_button_new_with_label(_("Dialog"));
		btn3 = gtk_button_new_with_label(_("Insert"));
		gtk_box_pack_start(GTK_BOX(hbox), btn2, TRUE, TRUE, 5);
		gtk_box_pack_start(GTK_BOX(hbox), btn3, TRUE, TRUE, 5);
	}

	gtk_container_add(GTK_CONTAINER(scroll), GTK_WIDGET(view));
	gtk_container_add(GTK_CONTAINER(info_window),
					  GTK_WIDGET(frame));

	vbox = gtk_vbox_new(FALSE, 1);
	gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 5);

	gtk_container_add(GTK_CONTAINER(frame), GTK_WIDGET(vbox));
	gtk_container_set_border_width(GTK_CONTAINER(frame), 0);
	gtk_frame_set_label_widget(GTK_FRAME(frame), NULL);
	
	gtk_widget_set_size_request(info_window, 400, 300);
	gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);

	g_signal_connect(G_OBJECT(info_window), "key-press-event",
					 G_CALLBACK(frefcb_info_keypress),
					 info_window);
/*	g_signal_connect(G_OBJECT(info_window), "focus-out-event",
					 G_CALLBACK(frefcb_info_lost_focus),
					 info_window);*/
	g_signal_connect(G_OBJECT(btn1), "clicked",
					 G_CALLBACK(frefcb_info_close), info_window);
	if (!modal) {
		g_signal_connect(G_OBJECT(btn2), "clicked",
						 G_CALLBACK(frefcb_info_dialog),
						 info_window);
		g_signal_connect(G_OBJECT(btn3), "clicked",
						 G_CALLBACK(frefcb_info_insert),
						 info_window);
	}
/*	gtk_window_set_decorated(GTK_WINDOW(info_window), FALSE);*/
/* let the window manager do it's work */
/*	
	if (!modal)
		gtk_window_set_position(GTK_WINDOW(info_window),
								GTK_WIN_POS_CENTER_ALWAYS);
	else if (parent != NULL) {
		gtk_window_get_position(GTK_WINDOW(parent), &x, &y);
		gtk_window_get_size(GTK_WINDOW(parent), &w, &h);
		gtk_window_move(GTK_WINDOW(info_window), x + w + 10, y);
	}*/
	gtk_widget_show_all(info_window);
/*	gtk_widget_grab_focus(info_window);*/
}

GtkWidget *fref_prepare_dialog(FRInfo * entry)
{
	GtkWidget *dialog;
	GtkWidget *vbox;
	GtkWidget *table;
	GtkWidget *label;
	GtkWidget *input;
	GtkWidget *combo;
	GtkWidget *dialog_action_area;
	GtkWidget *cancelbutton;
	GtkWidget *okbutton;
	GtkWidget *infobutton;
	GtkWidget *scroll;
	GtkRequisition req,req2;
	FRAttrInfo *attr = NULL;
	FRParamInfo *par = NULL;
	GList *list = NULL;
	gint itnum,w,h;

	DEBUG_MSG("fref_prepare_dialog, started\n");
	dialog = gtk_dialog_new();
	if (entry->dialog_title != NULL)
		gtk_window_set_title(GTK_WINDOW(dialog), entry->dialog_title);
	vbox = GTK_DIALOG(dialog)->vbox;
	
	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,GTK_POLICY_AUTOMATIC);
	gtk_widget_show(scroll);
	
	gtk_widget_show(vbox);

	switch (entry->type) {
	case FR_TYPE_TAG:
		list = entry->attributes;
		break;
	case FR_TYPE_FUNCTION:
		list = entry->params;
		break;
	case FR_TYPE_CLASS:
		list = NULL;
		break;
	}
	if (list == NULL) {
		gtk_widget_destroy(dialog);
		DEBUG_MSG("fref_prepare_dialog, list==NULL, aborting..\n");
		return NULL;
	}

	table = gtk_table_new(g_list_length(list), 2, FALSE);

	
	gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);
	gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll),table);

	switch (entry->type) {
	case FR_TYPE_TAG:
		{
			list = g_list_first(list);
			attr = (FRAttrInfo *) g_list_nth_data(list, 0);
			itnum = 0;
			while (itnum < g_list_length(list)) {
				if (attr->title != NULL) {
					label = gtk_label_new("");
					if (attr->required) {
						gchar *tofree = g_strconcat("<span color='#FF0000'>",attr->title, "</span>",NULL);
						gtk_label_set_markup(GTK_LABEL(label),tofree);
						g_free(tofree);
					} else {
						gtk_label_set_text(GTK_LABEL(label), attr->title);
					}
				} else {
					label = gtk_label_new("");
					if (attr->required) {
						gchar *tofree = g_strconcat("<span color='#FF0000'>",attr->name, "</span>",NULL);
						gtk_label_set_markup(GTK_LABEL(label),tofree);
						g_free(tofree);
					}else {
						gtk_label_set_text(GTK_LABEL(label), attr->name);
					}
				}
				gtk_widget_show(label);
				gtk_table_attach(GTK_TABLE(table), label, 0, 1, itnum,
								 itnum + 1, (GtkAttachOptions) (GTK_FILL),
								 (GtkAttachOptions) (0), 5, 6);
				gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_LEFT);
				gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
				if (attr->has_list) {
					combo = gtk_combo_new();
					gtk_widget_show(combo);
					gtk_table_attach(GTK_TABLE(table), combo, 1, 2, itnum,
									 itnum + 1,
									 (GtkAttachOptions) (GTK_EXPAND |
														 GTK_FILL),
									 (GtkAttachOptions) (0), 5, 5);
					gtk_combo_set_value_in_list(GTK_COMBO(combo), TRUE,
												TRUE);
					gtk_combo_set_popdown_strings(GTK_COMBO(combo),
												  fref_string_to_list
												  (attr->values, ","));
					if (attr->def_value != NULL)
						gtk_entry_set_text(GTK_ENTRY
										   (GTK_COMBO(combo)->entry),
										   attr->def_value);
					attr->dlg_item = combo;
					gtk_tooltips_set_tip(fref_data.argtips,GTK_COMBO(combo)->entry,
					                     attr->description,"");
				} else {
					input = gtk_entry_new();
					if (attr->def_value != NULL)
						gtk_entry_set_text(GTK_ENTRY(input),
										   attr->def_value);
					gtk_widget_show(input);
					gtk_table_attach(GTK_TABLE(table), input, 1, 2, itnum,
									 itnum + 1,
									 (GtkAttachOptions) (GTK_EXPAND |
														 GTK_FILL),
									 (GtkAttachOptions) (0), 5, 5);
					attr->dlg_item = input;
					gtk_tooltips_set_tip(fref_data.argtips,input,
					                     attr->description,"");					
				}
				itnum++;
				attr = (FRAttrInfo *) g_list_nth_data(list, itnum);
			}
		};
		break;
	case FR_TYPE_FUNCTION:
		{
			list = g_list_first(list);
			par = (FRParamInfo *) g_list_nth_data(list, 0);
			itnum = 0;
			while (itnum < g_list_length(list)) {
				if (par->title != NULL) {
					label = gtk_label_new("");
					if (par->required) {
						gchar *tofree = NULL;
						if (par->type!=NULL)
						   tofree = g_strconcat("<span color='#FF0000'>",par->title, " (", par->type,") ", "</span>", NULL);
						else
						   tofree = g_strconcat("<span color='#FF0000'>",par->title, " </span>", NULL);   
						gtk_label_set_markup(GTK_LABEL(label),tofree);
						g_free(tofree); 
					} else {
						gchar *tofree = NULL;
						if (par->type!=NULL)
						   tofree = g_strconcat(par->title, " (",par->type, ") ",NULL);
						else
						   tofree = g_strconcat(par->title, " ",NULL);   
						gtk_label_set_text(GTK_LABEL(label),tofree);
						g_free(tofree); 
					}
				} else {
					label = gtk_label_new("");
					if (par->required) {
						gchar *tofree = NULL;
						if (par->type!=NULL)
						   tofree = g_strconcat("<span color='#FF0000'>",par->name, " (", par->type,") ", "</span>", NULL);
						else
						   tofree = g_strconcat("<span color='#FF0000'>",par->name, " </span>", NULL);   
						gtk_label_set_markup(GTK_LABEL(label),tofree);
						g_free(tofree); 
					} else {
						gchar *tofree = NULL;
						if (par->type!=NULL)
						   tofree = g_strconcat(par->name, " (",par->type, ") ",NULL);
						else
						   tofree = g_strconcat(par->name, " ",NULL);   
						gtk_label_set_text(GTK_LABEL(label),tofree);
						g_free(tofree); 
					}
				}


				gtk_widget_show(label);
				gtk_table_attach(GTK_TABLE(table), label, 0, 1, itnum,
								 itnum + 1, (GtkAttachOptions) (GTK_FILL),
								 (GtkAttachOptions) (0), 5, 6);
				gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_LEFT);
				gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
				if (par->has_list) {
					combo = gtk_combo_new();
					gtk_widget_show(combo);
					gtk_table_attach(GTK_TABLE(table), combo, 1, 2, itnum,
									 itnum + 1,
									 (GtkAttachOptions) (GTK_EXPAND | GTK_FILL),
									 (GtkAttachOptions) (0), 5, 5);
					gtk_combo_set_value_in_list(GTK_COMBO(combo), TRUE,TRUE);
					gtk_combo_set_popdown_strings(GTK_COMBO(combo),
												  fref_string_to_list(par->values,","));
					if (par->def_value != NULL)
						gtk_entry_set_text(GTK_ENTRY
										   (GTK_COMBO(combo)->entry),par->def_value);
					par->dlg_item = combo;
					gtk_tooltips_set_tip(fref_data.argtips,GTK_COMBO(combo)->entry,
					                     par->description,"");					
				} else {
					input = gtk_entry_new();
					if (par->def_value != NULL)
						gtk_entry_set_text(GTK_ENTRY(input),par->def_value);
					gtk_widget_show(input);
					gtk_table_attach(GTK_TABLE(table), input, 1, 2, itnum,
									 itnum + 1,
									 (GtkAttachOptions) (GTK_EXPAND | GTK_FILL),
									 (GtkAttachOptions) (0), 5, 5);
					par->dlg_item = input;
					if (par->description!=NULL)
					   gtk_tooltips_set_tip(fref_data.argtips,input,par->description,"");					
				}
				itnum++;
				par = (FRParamInfo *) g_list_nth_data(list, itnum);
			}
		};
		break;

	}							/* switch */

	dialog_action_area = GTK_DIALOG(dialog)->action_area;
	gtk_widget_show(dialog_action_area);
	gtk_button_box_set_layout(GTK_BUTTON_BOX(dialog_action_area),
							  GTK_BUTTONBOX_END);


	infobutton = gtk_button_new_with_label(_("Info"));
	gtk_widget_show(infobutton);
	gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->action_area),
					   infobutton, FALSE, FALSE, 2);
	g_signal_connect(G_OBJECT(infobutton), "clicked",
					 G_CALLBACK(frefcb_info_show), fref_data.tree);

	cancelbutton = gtk_button_new_from_stock("gtk-cancel");
	gtk_widget_show(cancelbutton);
	gtk_dialog_add_action_widget(GTK_DIALOG(dialog), cancelbutton,
								 GTK_RESPONSE_CANCEL);

	okbutton = gtk_button_new_from_stock("gtk-ok");
	gtk_widget_show(okbutton);
	gtk_dialog_add_action_widget(GTK_DIALOG(dialog), okbutton,
								 GTK_RESPONSE_OK);
   gtk_tooltips_enable(fref_data.argtips);
   
  gtk_widget_show(table); 
  gtk_window_get_size(GTK_WINDOW(dialog),&w,&h); 
  gtk_widget_size_request(table,&req);
  gtk_widget_size_request(dialog_action_area,&req2);  
  gtk_window_resize(GTK_WINDOW(dialog),req.width+10,MIN(400,req.height+req2.height+20));
	return dialog;
}


gchar *fref_prepare_text(FRInfo * entry, GtkWidget * dialog)
{
	/* Here attribute/param names have to be replaced by values from dialog */
	gchar *p, *prev, *stringdup, *tofree;
	gchar *tmp, *dest, *tmp3 = NULL;
	GList *lst;
	gint d1, d2;
	FRAttrInfo *tmpa;
	FRParamInfo *tmpp;
	const gchar *converted = NULL, *tmp2 = NULL;

	dest = g_strdup("");
	stringdup = g_strdup(entry->dialog_text);
	prev = stringdup;
	p = strchr(prev, '%');
	while (p) {
		tmp = dest;
		*p = '\0';
		p++;
		if (*p == '%') {
			converted = "%";
		} else {

			switch (entry->type) {
			case FR_TYPE_TAG:
				d1 = g_ascii_digit_value(*p);
				if (d1 != -1) {
					d2 = g_ascii_digit_value(*(p + 1));
					if (d2 != -1)
						d1 = d1 * 10 + d2;
					lst = g_list_nth(entry->attributes, d1);
					if (lst != NULL) {
						tmpa = (FRAttrInfo *) lst->data;
						if (tmpa->dlg_item) {
							if (GTK_IS_COMBO(tmpa->dlg_item))
								converted = gtk_entry_get_text(GTK_ENTRY(GTK_COMBO(tmpa->dlg_item)->entry));
							else
								converted = gtk_entry_get_text(GTK_ENTRY(tmpa->dlg_item));
						} else
							converted = "";
					} else
						converted = "";
				} else if (*p == '_') {	/* non empty attributes */
					lst = g_list_first(entry->attributes);
					tmp3 = g_strdup("");
					while (lst != NULL) {
						tmpa = (FRAttrInfo *) lst->data;
						if (tmpa->dlg_item) {
							if (GTK_IS_COMBO(tmpa->dlg_item))
								tmp2 = gtk_entry_get_text(GTK_ENTRY(GTK_COMBO(tmpa->dlg_item)->entry));
							else
								tmp2 = gtk_entry_get_text(GTK_ENTRY(tmpa->dlg_item));
							if (strcmp(tmp2, "") != 0) {
							  tofree = tmp3; 
								tmp3 =	g_strconcat(tmp3, " ", tmpa->name,	"=\"", tmp2, "\"", NULL);
								g_free(tofree);
							}
						}
						lst = g_list_next(lst);
					}			/* while */
					converted = tmp3;
				} else if (*p == '~') {	/* non empty attributes and required  */
					lst = g_list_first(entry->attributes);
					tmp3 = g_strdup("");
					while (lst != NULL) {
						tmpa = (FRAttrInfo *) lst->data;
						if (tmpa->dlg_item) {
							if (GTK_IS_COMBO(tmpa->dlg_item))
								tmp2 = gtk_entry_get_text(GTK_ENTRY(GTK_COMBO(tmpa->dlg_item)->entry));
							else
								tmp2 = gtk_entry_get_text(GTK_ENTRY(tmpa->dlg_item));
							if (strcmp(tmp2, "") != 0 || tmpa->required) {
							  tofree = tmp3;
								tmp3 = g_strconcat(tmp3, " ", tmpa->name,	"=\"", tmp2, "\"", NULL);
								g_free(tofree); 
							}
						}
						lst = g_list_next(lst);
					}			/* while */
					converted = tmp3;
				}				/* required and non-empty */
				break;
			case FR_TYPE_FUNCTION:
				d1 = g_ascii_digit_value(*p);
				if (d1 != -1) {
					d2 = g_ascii_digit_value(*(p + 1));
					if (d2 != -1)
						d1 = d1 * 10 + d2;
					lst = g_list_nth(entry->params, d1);
					if (lst != NULL) {
						tmpp = (FRParamInfo *) lst->data;
						if (tmpp->dlg_item) {
							if (GTK_IS_COMBO(tmpp->dlg_item))
								converted =	gtk_entry_get_text(GTK_ENTRY(GTK_COMBO(tmpp->dlg_item)->entry));
							else
								converted =	gtk_entry_get_text(GTK_ENTRY(tmpp->dlg_item));
						} else
							converted = "";
					} else
						converted = "";
				}
				break;

			}					/* switch */
		}
		dest = g_strconcat(dest, prev, converted, NULL);
		g_free(tmp); /* here I free the dest (tmp=dest) */
		g_free(tmp3);
		prev = ++p;
		p = strchr(p, '%');
	}
	tmp = dest;
	dest = g_strconcat(dest, prev, NULL);
	g_free(tmp);
	g_free(stringdup);
	return dest;
}

GList *fref_string_to_list(gchar * string, gchar * delimiter)
{
	GList *lst;
	gchar **arr;
	gint i;

	lst = NULL;
	arr = g_strsplit(string, delimiter, 0);
	i = 0;
	while (arr[i] != NULL) {
		lst = g_list_append(lst, arr[i]);
		i++;
	}
	return lst;
}


/* CALLBACKS */

void frefcb_row_expanded(GtkTreeView * treeview, GtkTreeIter * arg1,
						 GtkTreePath * arg2, gpointer user_data)
{
	GValue *val;
	GtkTreeIter iter;

	val = g_new0(GValue, 1);
	gtk_tree_model_get_value(GTK_TREE_MODEL(user_data), arg1, 2, val);
	if (G_IS_VALUE(val) && g_value_peek_pointer(val)!=NULL) {
		fref_loader_load_ref_xml((gchar *) g_value_peek_pointer(val),
								 GTK_WIDGET(treeview),
								 GTK_TREE_STORE(user_data), arg1);
	}

	/* remove dummy */
	
	if (g_value_peek_pointer(val)!=NULL &&
	    gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(user_data), &iter, arg1, 0)) {
		gtk_tree_store_remove(GTK_TREE_STORE(user_data), &iter);
	}
	g_free(val);
}

void frefcb_row_collapsed(GtkTreeView * treeview, GtkTreeIter * arg1,
						  GtkTreePath * arg2, gpointer user_data)
{
	GtkTreeIter iter;
	GValue *val;


	val = g_new0(GValue, 1);
	gtk_tree_model_get_value(GTK_TREE_MODEL(user_data), arg1, 2, val);
	if (G_IS_VALUE(val) && g_value_peek_pointer(val)!=NULL) {
	  	fref_loader_unload_ref(GTK_WIDGET(treeview), GTK_TREE_STORE(user_data),arg1);
	 	 /* dummy node for expander display */
 	   gtk_tree_store_append(GTK_TREE_STORE(user_data), &iter, arg1);
 	}   
}

static GtkWidget *togglemenuitem(GSList *group, gchar *name, gboolean selected, GCallback toggledfunc, gpointer toggleddata) {
	GtkWidget *retval;
	retval = gtk_radio_menu_item_new_with_label(group, name);
	gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM(retval), selected);
	g_signal_connect(GTK_OBJECT(retval), "toggled", toggledfunc, toggleddata);
	DEBUG_MSG("togglemenuitem, created %p and set to %d\n",retval,selected);
	return retval;
}

static void fref_ldblclck_changed(GtkWidget *widget, gpointer data) {
	if (GTK_CHECK_MENU_ITEM(widget)->active) {
		DEBUG_MSG("fref_ldblclck_changed, set to %d\n", GPOINTER_TO_INT(data));
		main_v->props.fref_ldoubleclick_action = GPOINTER_TO_INT(data);
	}
}

static void fref_infotype_changed(GtkWidget *widget, gpointer data) {
	if (GTK_CHECK_MENU_ITEM(widget)->active) {
	 	DEBUG_MSG("fref_infotype_changed, set to %d\n", GPOINTER_TO_INT(data));
	 	main_v->props.fref_info_type = GPOINTER_TO_INT(data);
	}
}

static void fref_popup_menu_dialog(GtkWidget *widget, FRInfo *entry) {
	GtkWidget *dialog;
	gint resp;
	gchar *pomstr;
	DEBUG_MSG("starting dialog\n");
	dialog = fref_prepare_dialog(entry);
	if (dialog) {
		resp = gtk_dialog_run(GTK_DIALOG(dialog));
		if (resp == GTK_RESPONSE_OK) {
			pomstr = fref_prepare_text(entry, dialog);
			gtk_widget_destroy(dialog);
			doc_insert_two_strings(main_v->current_document, pomstr,NULL);
			g_free(pomstr);
		} else gtk_widget_destroy(dialog);
	}
}
static void fref_popup_menu_insert(GtkWidget *widget, FRInfo *entry) {
	doc_insert_two_strings(main_v->current_document,entry->insert_text, NULL);
}
static void fref_popup_menu_info(GtkWidget *widget, FRInfo *entry) {
	fref_show_info(entry, FALSE, NULL);
}


static void fref_popup_menu_rescan_lcb(GtkWidget *widget,gpointer user_data) {
	gchar *userdir = g_strconcat(g_get_home_dir(), "/.bluefish/", NULL);
	DEBUG_MSG("fref_popup_menu_rescan_lcb, started\n");
	fref_rescan_dir(PKGDATADIR);
	fref_rescan_dir(userdir);
	g_free(userdir);
	DEBUG_MSG("about to refill toplevels\n");
	fill_toplevels(TRUE);
}

static GtkWidget *fref_popup_menu(FRInfo *entry) {
	GtkWidget *menu, *menu_item;
	DEBUG_MSG("fref_popup_menu, started\n");
	menu = gtk_menu_new();
	if (entry) {
		menu_item = gtk_menu_item_new_with_label(_("Dialog"));
		g_signal_connect(GTK_OBJECT(menu_item), "activate", G_CALLBACK(fref_popup_menu_dialog), entry);
		gtk_menu_append(GTK_MENU(menu), menu_item);
		menu_item = gtk_menu_item_new_with_label(_("Insert"));
		g_signal_connect(GTK_OBJECT(menu_item), "activate", G_CALLBACK(fref_popup_menu_insert), entry);
		gtk_menu_append(GTK_MENU(menu), menu_item);
		menu_item = gtk_menu_item_new_with_label(_("Info"));
		g_signal_connect(GTK_OBJECT(menu_item), "activate", G_CALLBACK(fref_popup_menu_info), entry);
		gtk_menu_append(GTK_MENU(menu), menu_item);
		menu_item = gtk_menu_item_new();
		gtk_menu_append(GTK_MENU(menu), menu_item);
	}
	menu_item = gtk_menu_item_new_with_label(_("Options"));
	gtk_menu_append(GTK_MENU(menu), menu_item);
	{
		GtkWidget *optionsmenu, *ldblclckmenu, *infowinmenu;
		GSList *group=NULL;
		GSList *group2=NULL;
		optionsmenu = gtk_menu_new();
		gtk_menu_item_set_submenu(GTK_MENU_ITEM(menu_item), optionsmenu);
		menu_item = gtk_menu_item_new_with_label(_("Rescan reference files"));
		g_signal_connect(GTK_OBJECT(menu_item), "activate", G_CALLBACK(fref_popup_menu_rescan_lcb), NULL);
		gtk_menu_append(GTK_MENU(optionsmenu), menu_item);
		menu_item = gtk_menu_item_new_with_label(_("Left doubleclick action"));
		gtk_menu_append(GTK_MENU(optionsmenu), menu_item);
		
		ldblclckmenu = gtk_menu_new();
		gtk_menu_item_set_submenu(GTK_MENU_ITEM(menu_item), ldblclckmenu);
		menu_item = togglemenuitem(NULL, _("Insert"), (main_v->props.fref_ldoubleclick_action == FREF_ACTION_INSERT), G_CALLBACK(fref_ldblclck_changed), GINT_TO_POINTER(FREF_ACTION_INSERT));
		group = gtk_radio_menu_item_group(GTK_RADIO_MENU_ITEM(menu_item));
		gtk_menu_append(GTK_MENU(ldblclckmenu), menu_item);
		menu_item = togglemenuitem(group, _("Dialog"), (main_v->props.fref_ldoubleclick_action == FREF_ACTION_DIALOG), G_CALLBACK(fref_ldblclck_changed), GINT_TO_POINTER(FREF_ACTION_DIALOG));
		gtk_menu_append(GTK_MENU(ldblclckmenu), menu_item);
		menu_item = togglemenuitem(group, _("Info"), (main_v->props.fref_ldoubleclick_action == FREF_ACTION_INFO), G_CALLBACK(fref_ldblclck_changed), GINT_TO_POINTER(FREF_ACTION_INFO));
		gtk_menu_append(GTK_MENU(ldblclckmenu), menu_item);

		menu_item = gtk_menu_item_new_with_label(_("Info type"));
		gtk_menu_append(GTK_MENU(optionsmenu), menu_item);
		infowinmenu = gtk_menu_new();
		gtk_menu_item_set_submenu(GTK_MENU_ITEM(menu_item), infowinmenu);
		menu_item = togglemenuitem(NULL, _("Description"), (main_v->props.fref_info_type == FREF_IT_DESC), G_CALLBACK(fref_infotype_changed), GINT_TO_POINTER(FREF_IT_DESC));
		group2 = gtk_radio_menu_item_group(GTK_RADIO_MENU_ITEM(menu_item));
		gtk_menu_append(GTK_MENU(infowinmenu), menu_item);
		menu_item = togglemenuitem(group2, _("Attributes/Parameters"), (main_v->props.fref_info_type == FREF_IT_ATTRS), G_CALLBACK(fref_infotype_changed), GINT_TO_POINTER(FREF_IT_ATTRS));
		gtk_menu_append(GTK_MENU(infowinmenu), menu_item);
		menu_item = togglemenuitem(group2, _("Notes"), (main_v->props.fref_info_type == FREF_IT_NOTES), G_CALLBACK(fref_infotype_changed), GINT_TO_POINTER(FREF_IT_NOTES));
		gtk_menu_append(GTK_MENU(infowinmenu), menu_item);

		
		}
	gtk_widget_show_all(menu);
	g_signal_connect_after(G_OBJECT(menu), "hide", G_CALLBACK(destroy_disposable_menu_hide_cb), menu);
	return menu;
}

static FRInfo *get_current_entry() {
	GtkTreePath *path;
	GtkTreeViewColumn *col;
	gtk_tree_view_get_cursor(GTK_TREE_VIEW(fref_data.tree), &path, &col);
	if (path != NULL) {
		FRInfo *retval=NULL;
		GValue *val;
		GtkTreeIter iter;
		gtk_tree_model_get_iter(gtk_tree_view_get_model(GTK_TREE_VIEW(fref_data.tree)), &iter, path);
		gtk_tree_path_free(path);
		val = g_new0(GValue, 1);
		gtk_tree_model_get_value(gtk_tree_view_get_model(GTK_TREE_VIEW(fref_data.tree)), &iter, 1, val);
		if (G_IS_VALUE(val) && g_value_fits_pointer(val)) {
			retval = (FRInfo *) g_value_peek_pointer(val);
		}
		g_value_unset(val);
		g_free(val);
		return retval;
	}
	return NULL;
}

gboolean frefcb_event_mouseclick(GtkWidget * widget,GdkEventButton * event,gpointer user_data)
{
	FRInfo *entry;

 if (widget!=fref_data.tree) return FALSE;
	entry = get_current_entry();
	if (entry == NULL) {
		if (event->button == 3 && event->type == GDK_BUTTON_PRESS) {
			gtk_menu_popup(GTK_MENU(fref_popup_menu(NULL)), NULL, NULL, NULL, NULL, event->button, event->time);     
			return TRUE; 
		} else return FALSE;
	}

	if (event->button == 3 && event->type == GDK_BUTTON_PRESS) {	/* right mouse click */
		gtk_menu_popup(GTK_MENU(fref_popup_menu(entry)), NULL, NULL, NULL, NULL, event->button, event->time);
	} else if (event->button == 1 && event->type == GDK_2BUTTON_PRESS) {	/* double click  */
		switch (main_v->props.fref_ldoubleclick_action) {
			case FREF_ACTION_INSERT:
				fref_popup_menu_insert(NULL, entry);
			break;
			case FREF_ACTION_DIALOG:
				fref_popup_menu_dialog(NULL, entry);
			break;
			case FREF_ACTION_INFO:
				fref_popup_menu_info(NULL, entry);
			break;
			default:
				g_print("weird, fref_doubleclick_action=%d\n",main_v->props.fref_ldoubleclick_action);
				main_v->props.fref_ldoubleclick_action = FREF_ACTION_DIALOG;
			break;
		}
	}
	return FALSE; /* we have handled the event, but the treeview freezes if you return TRUE,	so we return FALSE */
}

gboolean frefcb_event_keypress(GtkWidget * widget, GdkEventKey * event,
							   gpointer user_data)
{
	FRInfo *entry;
	entry = get_current_entry();
	if (entry != NULL) {
		if (g_strcasecmp(gdk_keyval_name(event->keyval), "F1") == 0) {
			fref_show_info(entry, FALSE, NULL);
			return TRUE;
		}
	}
	return FALSE;
}

/*gboolean frefcb_info_lost_focus(GtkWidget * widget, GdkEventFocus * event,
								gpointer user_data)
{
	if (user_data != NULL)
		gtk_widget_destroy(GTK_WIDGET(user_data));
	return TRUE;
}*/

gboolean frefcb_info_keypress(GtkWidget * widget, GdkEventKey * event,
							  gpointer user_data)
{
	if (event->keyval == GDK_Escape && user_data != NULL) {
		gtk_widget_destroy(GTK_WIDGET(user_data));
		return FALSE;
	}
	return TRUE;
}

void frefcb_info_close(GtkButton * button, gpointer user_data)
{
	if (user_data != NULL) {
		gtk_widget_destroy(GTK_WIDGET(user_data));
	}
}

void frefcb_info_dialog(GtkButton * button, gpointer user_data)
{
	FRInfo *entry;
	GtkWidget *dialog;
	gchar *pomstr;
	gint resp;

	if (user_data != NULL) gtk_widget_destroy(GTK_WIDGET(user_data));
	entry = get_current_entry();
	if (entry == NULL) return;

	dialog = fref_prepare_dialog(entry);
	if (dialog) {
		resp = gtk_dialog_run(GTK_DIALOG(dialog));
		if (resp == GTK_RESPONSE_OK) {
			pomstr = fref_prepare_text(entry, dialog);
			gtk_widget_destroy(dialog);
			doc_insert_two_strings(main_v->current_document, pomstr, NULL);
			g_free(pomstr);
		} else gtk_widget_destroy(dialog);
	}
}

void frefcb_full_info(GtkButton *button,gpointer user_data)
{
	FRInfo *entry;

	entry = get_current_entry();
	if (entry == NULL) return;
	fref_show_info(entry, FALSE, NULL);
}

void frefcb_info_insert(GtkButton * button, gpointer user_data)
{
	FRInfo *entry;

	if (user_data != NULL)
		gtk_widget_destroy(GTK_WIDGET(user_data));
	entry = get_current_entry();
	if (entry == NULL || entry->insert_text==NULL) return;
	doc_insert_two_strings(main_v->current_document, entry->insert_text,NULL);

}

void frefcb_autocomplete(GtkWidget * widget, gpointer data)
{
	GtkTextIter pomit, *pomit2;
	gchar *word;
	GList *lst;
	gchar *pref;
	GtkWidget *menu;
	GtkWidget *item;

	gtk_text_buffer_get_iter_at_mark(main_v->current_document->buffer,
									 &pomit,
									 gtk_text_buffer_get_insert(main_v->
																current_document->
																buffer));
	pomit2 = gtk_text_iter_copy(&pomit);
	gtk_text_iter_backward_sentence_start(pomit2);

	word =
		gtk_text_buffer_get_slice(main_v->current_document->buffer, pomit2,
								  &pomit, FALSE);
	if (word == NULL)
		return;

	lst = g_completion_complete(fref_data.autocomplete, word, &pref);
	if (lst == NULL)
		return;
	menu = gtk_menu_new();
	lst = g_list_first(lst);
	while (lst != NULL) {
		item = gtk_menu_item_new_with_label(lst->data);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
		g_signal_connect(G_OBJECT(item), "activate",
						 G_CALLBACK(frefcb_autocomplete_activate),
						 (lst->data) + strlen(word));
		gtk_widget_show(item);
		lst = g_list_next(lst);
	}
	gtk_menu_popup(GTK_MENU(menu), NULL, NULL, fref_ac_position, NULL, 0,
				   gtk_get_current_event_time());
}


void fref_ac_position(GtkMenu * menu, gint * x, gint * y,
					  gboolean * push_in, gpointer user_data)
{
	GtkTextIter pomit;
	GdkRectangle rec;
	gint x1, y1, x2, y2;

	gtk_text_buffer_get_iter_at_mark(main_v->current_document->buffer,
									 &pomit,
									 gtk_text_buffer_get_insert(main_v->
																current_document->
																buffer));
	gtk_text_view_get_iter_location(GTK_TEXT_VIEW
									(main_v->current_document->view),
									&pomit, &rec);
	gdk_window_get_origin(gtk_text_view_get_window
						  (GTK_TEXT_VIEW(main_v->current_document->view),
						   GTK_TEXT_WINDOW_TEXT), &x1, &y1);
	gtk_text_view_buffer_to_window_coords(GTK_TEXT_VIEW
										  (main_v->current_document->view),
										  GTK_TEXT_WINDOW_TEXT, x1, y1,
										  &x2, &y2);
	*x = x2 + rec.x;
	*y = y2 + rec.y + rec.height;
}

void frefcb_autocomplete_activate(GtkMenuItem * menuitem,
								  gpointer user_data)
{
	doc_insert_two_strings(main_v->current_document, (gchar *) user_data,NULL);
}

void frefcb_info_show(GtkButton * button, gpointer user_data)
{
	FRInfo *entry;
	entry = get_current_entry();
	if (entry == NULL) return;

	fref_show_info(entry, TRUE,gtk_widget_get_toplevel(GTK_WIDGET(button)));
}

static gboolean reference_file_known(gchar *path) {
	GList *tmplist = g_list_first(main_v->props.reference_files);
	while (tmplist) {
		gchar **arr = tmplist->data;
		if (count_array(arr)==2 && strcmp(arr[1],path)==0) {
			return TRUE;
		}
		tmplist = g_list_next(tmplist);
	}
	return FALSE;
}

void fref_rescan_dir(const gchar *dir) {
	const gchar *filename;
	GError *error = NULL;
	gchar *tofree;
	GPatternSpec* ps = g_pattern_spec_new("funcref_*.xml");
	GDir* gd = g_dir_open(dir,0,&error);
	filename = g_dir_read_name(gd);
	while (filename) {
		if (g_pattern_match(ps, strlen(filename), filename, NULL)) {
			gchar *path = g_strconcat(dir, filename, NULL);
			DEBUG_MSG("filename %s has a match!\n",filename);
			if (!reference_file_known(path)) {
				tofree = fref_xml_get_refname(path);
				main_v->props.reference_files = g_list_append(main_v->props.reference_files, array_from_arglist(g_strdup(tofree),path,NULL));
				g_free(tofree);
			}
			g_free(path);
		}
		filename = g_dir_read_name(gd);
	}
	g_dir_close(gd);
	g_pattern_spec_free(ps);
}

void frefcb_infocheck_toggled(GtkToggleButton *togglebutton, gpointer user_data) {
	if (gtk_toggle_button_get_active(togglebutton)) gtk_widget_show(fref_data.infoscroll);
	else gtk_widget_hide(fref_data.infoscroll);   
}

void  frefcb_cursor_changed(GtkTreeView *treeview, gpointer user_data) {
	FRInfo *entry;
	gchar *info=NULL;
	GdkRectangle rect;

	entry = get_current_entry();
	if (entry==NULL) return;
	if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(fref_data.infocheck))) {
		if (entry->description!=NULL) {
			switch (main_v->props.fref_info_type) {
			case FREF_IT_DESC:
				info = g_strconcat("<span size=\"small\"><b>Description:</b></span>\n",fref_prepare_info(entry,FR_INFO_DESC,FALSE),NULL);
			break;    
			case FREF_IT_ATTRS:
				switch (entry->type) {
					case FR_TYPE_TAG:
						info = g_strconcat("<span size=\"small\"><b>Attributes:</b></span>\n",fref_prepare_info(entry,FR_INFO_ATTRS,FALSE),NULL);break;
					case FR_TYPE_FUNCTION:
						info = g_strconcat("<span size=\"small\"><b>Parameters:</b></span>\n",fref_prepare_info(entry,FR_INFO_ATTRS,FALSE),NULL);break;
				}  
			break;    
			case FREF_IT_NOTES:
				info = fref_prepare_info(entry,FR_INFO_NOTES,FALSE);
			break;
			default:
				info=g_strdup("Unknown fref_info_type");    	        
			} /* switch */                     
			gtk_tree_view_get_visible_rect(GTK_TREE_VIEW(fref_data.tree),&rect);
			gtk_widget_set_size_request(fref_data.infoview,rect.width,-1);
			gtk_label_set_markup(GTK_LABEL(fref_data.infoview),info);                   
			g_free(info);  
	    } else gtk_label_set_text(GTK_LABEL(fref_data.infoview),"");
	}
}


