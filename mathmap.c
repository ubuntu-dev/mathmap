/* The GIMP -- an image manipulation program
 * Copyright (C) 1995 Spencer Kimball and Peter Mattis
 *
 * MathMap plug-in --- generate an image by means of a mathematical expression
 * Copyright (C) 1997-2002 Mark Probst
 * schani@complang.tuwien.ac.at
 *
 * Plug-In structure based on:
 *   Whirl plug-in --- distort an image into a whirlpool
 *   Copyright (C) 1997 Federico Mena Quintero
 *   federico@nuclecu.unam.mx
 *
 * Version 0.14
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
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#define MATHMAP_MANUAL_URL    "http://www.complang.tuwien.ac.at/schani/mathmap/manual.html"

#include <sys/param.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include <gtk/gtk.h>
#include <libgimp/gimp.h>
#include <libgimp/gimpintl.h>
#include <libgimp/gimpcolorbutton.h>

#include "lispreader.h"
#include "exprtree.h"
#include "builtins.h"
#include "postfix.h"
#include "tags.h"
#include "scanner.h"
#include "vars.h"
#include "userval.h"
#include "internals.h"
#include "macros.h"
#include "jump.h"
#include "mathmap.h"
#include "noise.h"
#ifdef USE_CGEN
#include "cgen.h"
#endif
#include "mathmap_common.h"

/***** Magic numbers *****/
#define PREVIEW_SIZE 128
#define SCALE_WIDTH  200
#define ENTRY_WIDTH  60

/* Even more stuff from Quartics plugins */
#define CHECK_SIZE  8
#define CHECK_DARK  ((int) (1.0 / 3.0 * 255))
#define CHECK_LIGHT ((int) (2.0 / 3.0 * 255))   


#define DEFAULT_EXPRESSION      "origValXY(x+sin(y*10)*3,y+sin(x*10)*3)"
#define DEFAULT_NUMBER_FRAMES   10

#define FLAG_ANTIALIASING       1
#define FLAG_SUPERSAMPLING      2
#define FLAG_ANIMATION          4
#define FLAG_PERIODIC           8

#define MAX_EXPRESSION_LENGTH   8192

#define NUM_GRADIENT_SAMPLES    1024

/***** Types *****/

typedef struct {
    gint flags;
    gint frames;
    gfloat param_t;
    gchar expression[MAX_EXPRESSION_LENGTH];
} mathmap_vals_t;

typedef struct {
    GtkWidget *preview;
    guchar *wimage;
    gint run;
} mathmap_interface_t;

typedef struct {
    GimpDrawable *drawable;
    gint bpp;
    gint row;
    gint col;
    GimpTile *tile;
    guchar *fast_image_source;
    int used;
} input_drawable_t;

/***** Prototypes *****/

static void query (void);
static void run (char       *name,
		 int         nparams,
		 GimpParam  *param,
		 int        *nreturn_vals,
		 GimpParam **return_vals);

static void expression_copy (gchar *dest, gchar *src);

static void do_mathmap (int frame_num, float t);
static gint32 mathmap_layer_copy (gint32 layerID);

extern int yyparse (void);

static void build_fast_image_source (input_drawable_t *drawable);

static void update_userval_table (void);

static void update_gradient (void);
static gint mathmap_dialog (int);
static void dialog_update_preview (void);
static void dialog_scale_update (GtkAdjustment *adjustment, gint *value);
static void dialog_t_update (GtkAdjustment *adjustment, gfloat *value);
static void dialog_text_changed (void);
static void dialog_text_update (void);
static void dialog_antialiasing_update (GtkWidget *widget, gpointer data);
static void dialog_supersampling_update (GtkWidget *widget, gpointer data);
static void dialog_auto_preview_update (GtkWidget *widget, gpointer data);
static void dialog_fast_preview_update (GtkWidget *widget, gpointer data);
static void dialog_edge_behaviour_update (GtkWidget *widget, gpointer data);
static void dialog_edge_color_changed (GtkWidget *color_well, gpointer data);
static void dialog_animation_update (GtkWidget *widget, gpointer data);
static void dialog_periodic_update (GtkWidget *widget, gpointer data);
static void dialog_preview_callback (GtkWidget *widget, gpointer data);
static void dialog_close_callback (GtkWidget *widget, gpointer data);
static void dialog_ok_callback (GtkWidget *widget, gpointer data);
static void dialog_cancel_callback (GtkWidget *widget, gpointer data);
static void dialog_help_callback (GtkWidget *widget, gpointer data);
static void dialog_about_callback (GtkWidget *widget, gpointer data);
static void dialog_tree_changed (GtkTree *tree);

/***** Variables *****/

GimpPlugInInfo PLUG_IN_INFO = {
	NULL,   /* init_proc */
	NULL,   /* quit_proc */
	query,  /* query_proc */
	run     /* run_proc */
}; /* PLUG_IN_INFO */


static mathmap_vals_t mmvals = {
	FLAG_ANTIALIASING | FLAG_PERIODIC, /* flags */
	DEFAULT_NUMBER_FRAMES,	/* frames */
	0.0,			/* t */
	DEFAULT_EXPRESSION	/* expression */
}; /* mmvals */

static mathmap_interface_t wint = {
	NULL,  /* preview */
	NULL,  /* wimage */
	FALSE  /* run */
}; /* wint */

#define MAX_INPUT_DRAWABLES 64

static GimpRunModeType run_mode;
static gint32 image_id;
static gint32 layer_id;
static input_drawable_t input_drawables[MAX_INPUT_DRAWABLES];
static GimpDrawable *output_drawable;

static gint tile_width, tile_height;
gint sel_x1, sel_y1, sel_x2, sel_y2;
gint sel_width, sel_height;
gint preview_width, preview_height;

GtkWidget *expression_entry = 0,
    *frame_table,
    *t_table,
    *edge_color_well,
    *uservalues_scrolled_window,
    *uservalues_table;
GtkColorSelectionDialog *color_selection_dialog;

int img_width, img_height;
int previewing = 0, auto_preview = 1, fast_preview = 1;
int expression_changed = 1;
int num_gradient_samples = NUM_GRADIENT_SAMPLES;
tuple_t gradient_samples[NUM_GRADIENT_SAMPLES];
int output_bpp;
int edge_behaviour_mode = EDGE_BEHAVIOUR_COLOR;
guchar edge_color[4] = { 0, 0, 0, 0 };

mathmap_t *mathmap = 0;
mathmap_invocation_t *invocation = 0;

/***** Functions *****/

/*****/

static void
expression_copy (gchar *dest, gchar *src)
{
    strncpy(dest, src, MAX_EXPRESSION_LENGTH);
    dest[MAX_EXPRESSION_LENGTH - 1] = 0;
}


/*****/

MAIN()


/*****/

static lisp_object_t*
read_rc_file (void)
{
    static lisp_object_t *obj = 0;

    gchar *filename;
    FILE *file;
    lisp_stream_t stream;

    if (obj != 0)
	return obj;

    filename = gimp_personal_rc_file("mathmaprc");
    file = fopen(filename, "r");
    g_free(filename);
    if (file == 0)
    {
	filename = g_strconcat(gimp_data_directory(), G_DIR_SEPARATOR_S, "mathmaprc", NULL);
	file = fopen(filename, "r");
	g_free(filename);

	if (file == 0)
	    return 0;
    }

    obj = lisp_read(lisp_stream_init_file(&stream, file));
    fclose(file);

    return obj;
}

static void
register_lisp_obj (lisp_object_t *obj, char *symbol_prefix, char *menu_prefix)
{
    int symbol_prefix_len = strlen(symbol_prefix);
    int menu_prefix_len = strlen(menu_prefix);

    for (; lisp_type(obj) != LISP_TYPE_NIL; obj = lisp_cdr(obj))
    {
	lisp_object_t *vars[2];
	int is_group = 0;
	lisp_object_t *name_obj, *data;
	char *symbol, *menu;
	int i;
	int name_len;

	assert(lisp_type(obj) == LISP_TYPE_CONS);

	if (lisp_match_string("(group #?(string) . #?(list))", lisp_car(obj), vars))
	    is_group = 1;
	else if (lisp_match_string("(expression #?(string) #?(string))", lisp_car(obj), vars))
	    is_group = 0;
	else
	    assert(0);

	name_obj = vars[0];
	data = vars[1];

	name_len = strlen(lisp_string(name_obj));

	symbol = g_malloc(symbol_prefix_len + name_len + 2);
	strcpy(symbol, symbol_prefix);
	strcat(symbol, "_");
	strcat(symbol, lisp_string(name_obj));

	menu = g_malloc(menu_prefix_len + name_len + 2);
	strcpy(menu, menu_prefix);
	strcat(menu, "/");
	strcat(menu, lisp_string(name_obj));

	for (i = symbol_prefix_len + 1; i < symbol_prefix_len + 1 + name_len; ++i)
	    if (symbol[i] == ' ')
		symbol[i] = '_';
	    else
		symbol[i] = tolower(symbol[i]);

	if (is_group)
	    register_lisp_obj(data, symbol, menu);
	else
	{
	    static GimpParamDef args[] = {
		{ GIMP_PDB_INT32,      "run_mode",         "Interactive, non-interactive" },
		{ GIMP_PDB_IMAGE,      "image",            "Input image" },
		{ GIMP_PDB_DRAWABLE,   "drawable",         "Input drawable" },
		{ GIMP_PDB_INT32,      "flags",            "1: Antialiasing 2: Supersampling 4: Animate 8: Periodic" },
		{ GIMP_PDB_INT32,      "frames",           "Number of frames" },
		{ GIMP_PDB_FLOAT,      "param_t",          "The parameter t (if not animating)" },
		{ GIMP_PDB_STRING,     "expression",       "The expression" }
	    };
	    static GimpParamDef *return_vals  = NULL;
	    static int nargs = sizeof(args) / sizeof(args[0]);
	    static int nreturn_vals = 0;

	    fprintf(stderr, "registering %s (%s)\n", symbol, menu);

	    gimp_install_procedure(symbol,
				   "Generate an image using a mathematical expression.",
				   "Generates an image by means of a mathematical expression. The expression "
				   "can also refer to the data of an original image. Thus, arbitrary "
				   "distortions can be constructed. Even animations can be generated.",
				   "Mark Probst",
				   "Mark Probst",
				   MATHMAP_DATE ", " MATHMAP_VERSION,
				   menu,
				   "RGB*, GRAY*",
				   GIMP_PLUGIN,
				   nargs,
				   nreturn_vals,
				   args,
				   return_vals);
	}

	g_free(menu);
	g_free(symbol);
    }
}

static void
register_examples (void)
{
    lisp_object_t *obj = read_rc_file();

    if (obj == 0)
	return;

    register_lisp_obj(obj, "mathmap", "<Image>/Filters/Generic/MathMap");
    lisp_free(obj);
}

static char*
expression_for_symbol (char *symbol, lisp_object_t *obj)
{
    for (; lisp_type(obj) != LISP_TYPE_NIL; obj = lisp_cdr(obj))
    {
	lisp_object_t *vars[2];
	int is_group = 0;
	char *name;
	int i;
	int name_len;

	assert(lisp_type(obj) == LISP_TYPE_CONS);

	if (lisp_match_string("(group #?(string) . #?(list))", lisp_car(obj), vars))
	    is_group = 1;
	else if (lisp_match_string("(expression #?(string) #?(string))", lisp_car(obj), vars))
	    is_group = 0;
	else
	    assert(0);

	name = lisp_string(vars[0]);
	name_len = strlen(name);

	if (name_len > strlen(symbol))
	    continue;
	if ((!is_group && name_len != strlen(symbol))
	    || (is_group && name_len == strlen(symbol)))
	    continue;
	if (is_group && symbol[name_len] != '_')
	    continue;

	for (i = 0; i < name_len; ++i)
	    if ((name[i] == ' ' && symbol[i] != '_')
		|| (name[i] != ' ' && symbol[i] != tolower(name[i])))
		break;

	if (i == name_len)
	{
	    if (is_group)
	    {
		char *exp = expression_for_symbol(symbol + name_len + 1, vars[1]);

		if (exp != 0)
		    return exp;
	    }
	    else
		return lisp_string(vars[1]);
	}
    }

    return 0;
}

static void
query(void)
{
    static GimpParamDef args[] = {
	{ GIMP_PDB_INT32,      "run_mode",         "Interactive, non-interactive" },
	{ GIMP_PDB_IMAGE,      "image",            "Input image" },
	{ GIMP_PDB_DRAWABLE,   "drawable",         "Input drawable" },
	{ GIMP_PDB_INT32,      "flags",            "1: Antialiasing 2: Supersampling 4: Animate 8: Periodic" },
	{ GIMP_PDB_INT32,      "frames",           "Number of frames" },
	{ GIMP_PDB_FLOAT,      "param_t",          "The parameter t (if not animating)" },
	{ GIMP_PDB_STRING,     "expression",       "MathMap expression" }
    };
    static GimpParamDef *return_vals  = NULL;
    static int nargs = sizeof(args) / sizeof(args[0]);
    static int nreturn_vals = 0;

    gimp_install_procedure("plug_in_mathmap",
			   "Generate an image using a mathematical expression.",
			   "Generates an image by means of a mathematical expression. The expression "
			   "can also refer to the data of an original image. Thus, arbitrary "
			   "distortions can be constructed. Even animations can be generated.",
			   "Mark Probst",
			   "Mark Probst",
			   MATHMAP_DATE ", " MATHMAP_VERSION,
			   "<Image>/Filters/Generic/MathMap/MathMap",
			   "RGB*, GRAY*",
			   GIMP_PLUGIN,
			   nargs,
			   nreturn_vals,
			   args,
			   return_vals);

    register_examples();
}

/*****/

static void
run (char *name, int nparams, GimpParam *param, int *nreturn_vals, GimpParam **return_vals)
{
    static GimpParam values[1];

    GimpPDBStatusType status;
    int pwidth, pheight;

    int mutable_expression = 1;

    fprintf(stderr, "started as %s\n", name);

    INIT_LOCALE("mathmap");

    if (strncmp(name, "mathmap_", 8) == 0)
    {
	char *exp = expression_for_symbol(name + 8, read_rc_file());

	fprintf(stderr, "found %s\n", exp);

	if (exp != 0)
	{
	    strcpy(mmvals.expression, exp);
	    mutable_expression = 0;
	}
    }

    status   = GIMP_PDB_SUCCESS;
    run_mode = param[0].data.d_int32;

    image_id = param[1].data.d_int32;
    layer_id = gimp_image_get_active_layer(image_id);

    values[0].type = GIMP_PDB_STATUS;
    values[0].data.d_status = status;

    *nreturn_vals = 1;
    *return_vals = values;

    /* Get the active drawable info */

    input_drawables[0].drawable = gimp_drawable_get(param[2].data.d_drawable);
    input_drawables[0].bpp = gimp_drawable_bpp(input_drawables[0].drawable->id);
    input_drawables[0].row = input_drawables[0].col = -1;
    input_drawables[0].tile = 0;
    input_drawables[0].fast_image_source = 0;
    input_drawables[0].used = 1;

    output_bpp = input_drawables[0].bpp;

    tile_width = gimp_tile_width();
    tile_height = gimp_tile_height();

    img_width = gimp_drawable_width(input_drawables[0].drawable->id);
    img_height = gimp_drawable_height(input_drawables[0].drawable->id);

    gimp_drawable_mask_bounds(input_drawables[0].drawable->id, &sel_x1, &sel_y1, &sel_x2, &sel_y2);

    sel_width = sel_x2 - sel_x1;
    sel_height = sel_y2 - sel_y1;

    /* Calculate preview size */

    if (sel_width > sel_height) {
	pwidth  = MIN(sel_width, PREVIEW_SIZE);
	pheight = sel_height * pwidth / sel_width;
    } else {
	pheight = MIN(sel_height, PREVIEW_SIZE);
	pwidth  = sel_width * pheight / sel_height;
    } /* else */

    preview_width  = MAX(pwidth, 2);  /* Min size is 2 */
    preview_height = MAX(pheight, 2);

    init_builtins();
    init_tags();
    init_macros();
    init_noise();

    /* See how we will run */

    switch (run_mode) {
	case GIMP_RUN_INTERACTIVE:
	    /* Possibly retrieve data */

	    gimp_get_data(name, &mmvals);

	    /* Get information from the dialog */

	    update_gradient();

	    if (!mathmap_dialog(mutable_expression))
		return;

	    break;

	case GIMP_RUN_NONINTERACTIVE:
	    /* Make sure all the arguments are present */

	    if (nparams != 7)
		status = GIMP_PDB_CALLING_ERROR;

	    if (status == GIMP_PDB_SUCCESS)
	    {
		mmvals.flags = param[3].data.d_int32;
		mmvals.frames = param[4].data.d_int32;
		mmvals.param_t = param[5].data.d_float;
		expression_copy(mmvals.expression, param[6].data.d_string);
	    }

	    break;

	case GIMP_RUN_WITH_LAST_VALS:
	    /* Possibly retrieve data */

	    gimp_get_data(name, &mmvals);
	    break;

	default:
	    break;
    } /* switch */

    /* Mathmap the image */

    if ((status == GIMP_PDB_SUCCESS)
	&& (gimp_drawable_is_rgb(input_drawables[0].drawable->id)
	    || gimp_drawable_is_gray(input_drawables[0].drawable->id)))
    {
	int animation_enabled = mmvals.flags & FLAG_ANIMATION;

	update_gradient();

	/* Set the tile cache size */
	gimp_tile_cache_ntiles((input_drawables[0].drawable->width + gimp_tile_width() - 1)
			       / gimp_tile_width());

	/* Run! */

	if (animation_enabled)
	{
	    int frame;

	    gimp_undo_push_group_start(image_id);
	    for (frame = 0; frame < mmvals.frames; ++frame)
	    {
		gint32 layer;
		char layer_name[100];
		float t;

		if (mmvals.flags & FLAG_PERIODIC)
		    t = (double)frame / (double)mmvals.frames;
		else if (mmvals.frames < 2)
		    t = 0.0;
		else
		    t = (double)frame / (double)(mmvals.frames - 1);
		layer = mathmap_layer_copy(layer_id);
		sprintf(layer_name, "Frame %d", frame + 1);
		gimp_layer_set_name(layer, layer_name);
		output_drawable = gimp_drawable_get(layer);
		do_mathmap(frame, t);
		gimp_image_add_layer(image_id, layer, 0);
	    }
	    gimp_undo_push_group_end(image_id);
	}
	else
	{
	    output_drawable = input_drawables[0].drawable;
	    do_mathmap(-1, mmvals.param_t);
	}

	/* If run mode is interactive, flush displays */

	if (run_mode != GIMP_RUN_NONINTERACTIVE)
	    gimp_displays_flush();

	/* Store data */

	if (run_mode == GIMP_RUN_INTERACTIVE)
	    gimp_set_data(name, &mmvals, sizeof(mathmap_vals_t));
    } else if (status == GIMP_PDB_SUCCESS)
	status = GIMP_PDB_EXECUTION_ERROR;

    values[0].data.d_status = status;

    gimp_drawable_detach(input_drawables[0].drawable);
} /* run */

/*****/

static gint32 
mathmap_layer_copy(gint32 layerID)
{
    GimpParam *return_vals;
    int nreturn_vals;
    gint32 nlayer;

    return_vals = gimp_run_procedure ("gimp_layer_copy", 
				      &nreturn_vals,
				      GIMP_PDB_LAYER, layerID,
				      GIMP_PDB_INT32, TRUE,
				      GIMP_PDB_END);
 
    if (return_vals[0].data.d_status == GIMP_PDB_SUCCESS)
	nlayer = return_vals[1].data.d_layer;
    else
	nlayer = -1;
    gimp_destroy_params(return_vals, nreturn_vals);
    return nlayer;
} 

/*****/

static void
update_gradient (void)
{
    gdouble *samples;
    int i;

    samples = gimp_gradients_sample_uniform(num_gradient_samples);

    for (i = 0; i < num_gradient_samples; ++i)
	gradient_samples[i] = color_to_tuple(samples[i * 4 + 0], samples[i * 4 + 1],
					     samples[i * 4 + 2], samples[i * 4 + 3]);
}

/*****/

static int
generate_code (int current_frame, float current_t)
{
    if (expression_changed)
    {
	mathmap_t *new_mathmap;

	if (run_mode == GIMP_RUN_INTERACTIVE && expression_entry != 0)
	    dialog_text_update();

	if (mathmap != 0)
	    unload_mathmap(mathmap);

	new_mathmap = compile_mathmap(mmvals.expression);

	if (new_mathmap == 0)
	{
	    gimp_message(error_string);

	    /* FIXME: free old mathmap/invocation */

	    mathmap = 0;
	    invocation = 0;
	}
	else
	{
	    mathmap_invocation_t *new_invocation;

	    new_invocation = invoke_mathmap(new_mathmap, invocation, sel_width, sel_height);
	    assert(new_invocation != 0);

	    new_invocation->output_bpp = output_bpp;
	    new_invocation->origin_x = sel_x1;
	    new_invocation->origin_y = sel_y1;

	    if (invocation != 0)
	    {
		free_invocation(invocation);
		free_mathmap(mathmap);
	    }

	    /* FIXME: free old mathmap/invocation */

	    mathmap = new_mathmap;
	    invocation = new_invocation;

	    init_invocation(invocation);

	    update_userval_table();

	    expression_changed = 0;
	}
    }

    if (invocation != 0)
    {
	invocation->antialiasing = mmvals.flags & FLAG_ANTIALIASING;
	invocation->supersampling = mmvals.flags & FLAG_SUPERSAMPLING;

	invocation->current_frame = current_frame;
	invocation->current_t = current_t;

	invocation->edge_behaviour = edge_behaviour_mode;
	memcpy(invocation->edge_color, edge_color, sizeof(edge_color));

	update_image_internals(invocation);
    }

    return invocation != 0;
}

/*****/

/*****/

static void
unref_tiles (void)
{
    int i;

    for (i = 0; i < MAX_INPUT_DRAWABLES; ++i)
	if (input_drawables[i].used != 0 && input_drawables[i].tile != 0)
	{
	    gimp_tile_unref(input_drawables[i].tile, FALSE);
	    input_drawables[i].tile = 0;
	}
}

/*****/

int
alloc_input_drawable (GimpDrawable *drawable)
{
    int i;

    for (i = 0; i < MAX_INPUT_DRAWABLES; ++i)
	if (!input_drawables[i].used)
	    break;
    if (i == MAX_INPUT_DRAWABLES)
	return -1;

    input_drawables[i].drawable = drawable;
    input_drawables[i].bpp = gimp_drawable_bpp(drawable->id);
    input_drawables[i].row = -1;
    input_drawables[i].col = -1;
    input_drawables[i].tile = 0;
    input_drawables[i].fast_image_source = 0;
    input_drawables[i].used = 1;

    return i;
}

void
free_input_drawable (int index)
{
    assert(input_drawables[index].used);
    if (input_drawables[index].tile != 0)
    {
	gimp_tile_unref(input_drawables[index].tile, FALSE);
	input_drawables[index].tile = 0;
    }
    if (input_drawables[index].fast_image_source != 0)
    {
	g_free(input_drawables[index].fast_image_source);
	input_drawables[index].fast_image_source = 0;
    }
    input_drawables[index].drawable = 0;
    input_drawables[index].used = 0;
}

GimpDrawable*
get_input_drawable (int index)
{
    assert(input_drawables[index].used);

    return input_drawables[index].drawable;
}

/*****/

static void
do_mathmap (int frame_num, float current_t)
{
    GimpPixelRgn dest_rgn;
    gpointer pr;
    gint progress, max_progress;
    guchar *dest_row;
    guchar *dest;
    gint row, col;
    int i;
    gchar progress_info[30];

    assert(invocation != 0);

    previewing = 0;

    invocation->output_bpp = gimp_drawable_bpp(output_drawable->id);

    if (generate_code(frame_num, current_t))
    {
	/* Initialize pixel region */

	gimp_pixel_rgn_init(&dest_rgn, output_drawable, sel_x1, sel_y1, sel_width, sel_height,
			    TRUE, TRUE);

	progress = 0;
	max_progress = sel_width * sel_height;

	if (frame_num >= 0)
	    sprintf(progress_info, _("Mathmapping frame %d..."), frame_num + 1);
	else
	    strcpy(progress_info, _("Mathmapping..."));
	gimp_progress_init(progress_info);

	for (pr = gimp_pixel_rgns_register(1, &dest_rgn);
	     pr != NULL; pr = gimp_pixel_rgns_process(pr))
	{
	    if (invocation->supersampling)
	    {
		unsigned char *line1,
		    *line2,
		    *line3;

		dest_row = dest_rgn.data;

		line1 = (unsigned char*)malloc((sel_width + 1) * output_bpp);
		line2 = (unsigned char*)malloc(sel_width * output_bpp);
		line3 = (unsigned char*)malloc((sel_width + 1) * output_bpp);

		for (col = 0; col <= dest_rgn.w; ++col)
		{
		    invocation->current_x = col + dest_rgn.x - sel_x1 - invocation->middle_x;
		    invocation->current_y = -(0.0 + dest_rgn.y - sel_y1 - invocation->middle_y);
		    calc_ra(invocation);
		    update_pixel_internals(invocation);
		    write_tuple_to_pixel(call_invocation(invocation), line1 + col * output_bpp, output_bpp);
		}

		for (row = 0; row < dest_rgn.h; ++row)
		{
		    dest = dest_row;

		    for (col = 0; col < dest_rgn.w; ++col)
		    {
			invocation->current_x = col + dest_rgn.x - sel_x1 + 0.5 - invocation->middle_x;
			invocation->current_y = -(row + dest_rgn.y - sel_y1 + 0.5 - invocation->middle_y);
			calc_ra(invocation);
			update_pixel_internals(invocation);
			write_tuple_to_pixel(call_invocation(invocation), line2 + col * output_bpp, output_bpp);
		    }
		    for (col = 0; col <= dest_rgn.w; ++col)
		    {
			invocation->current_x = col + dest_rgn.x - sel_x1 - invocation->middle_x;
			invocation->current_y = -(row + dest_rgn.y - sel_y1 + 1.0 - invocation->middle_y);
			calc_ra(invocation);
			update_pixel_internals(invocation);
			write_tuple_to_pixel(call_invocation(invocation), line3 + col * output_bpp, output_bpp);
		    }
	    
		    for (col = 0; col < dest_rgn.w; ++col)
		    {
			for (i = 0; i < output_bpp; ++i)
			    dest[i] = (line1[col*output_bpp+i]
				       + line1[(col+1)*output_bpp+i]
				       + 2*line2[col*output_bpp+i]
				       + line3[col*output_bpp+i]
				       + line3[(col+1)*output_bpp+i]) / 6;
			dest += output_bpp;
		    }

		    memcpy(line1, line3, (sel_width + 1) * output_bpp);

		    dest_row += dest_rgn.rowstride;
		}
	    }
	    else
	    {
		dest_row = dest_rgn.data;

		for (row = dest_rgn.y; row < (dest_rgn.y + dest_rgn.h); row++)
		{
		    dest = dest_row;

		    for (col = dest_rgn.x; col < (dest_rgn.x + dest_rgn.w); col++)
		    {
			invocation->current_x = col - sel_x1 - invocation->middle_x;
			invocation->current_y = -(row - sel_y1 - invocation->middle_y);
			calc_ra(invocation);
			update_pixel_internals(invocation);
			write_tuple_to_pixel(call_invocation(invocation), dest, output_bpp);
			dest += output_bpp;
		    }
		
		    dest_row += dest_rgn.rowstride;
		}
	    }

	    /* Update progress */
	    progress += dest_rgn.w * dest_rgn.h;
	    gimp_progress_update((double) progress / max_progress);
	}

	unref_tiles();

	gimp_drawable_flush(output_drawable);
	gimp_drawable_merge_shadow(output_drawable->id, TRUE);
	gimp_drawable_update(output_drawable->id, sel_x1, sel_y1, sel_width, sel_height);
    }
} /* mathmap */

/*****/

void
mathmap_get_pixel (mathmap_invocation_t *invocation, int drawable_index, int frame, int x, int y, guchar *pixel)
{
    gint newcol, newrow;
    gint newcoloff, newrowoff;
    guchar *p;
    int i;
    input_drawable_t *drawable;

    if (drawable_index < 0 || drawable_index >= MAX_INPUT_DRAWABLES || !input_drawables[drawable_index].used
	|| x < 0 || x >= img_width
	|| y < 0 || y >= img_height)
    {
	for (i = 0; i < 4; ++i)
	    pixel[i] = edge_color[i];
	return;
    }

    drawable = &input_drawables[drawable_index];

    newcol = x / tile_width;
    newcoloff = x % tile_width;
    newrow = y / tile_height;
    newrowoff = y % tile_height;

    if (drawable->col != newcol || drawable->row != newrow || drawable->tile == NULL)
    {
	if (drawable->tile != NULL)
	    gimp_tile_unref(drawable->tile, FALSE);

	drawable->tile = gimp_drawable_get_tile(drawable->drawable, FALSE, newrow, newcol);
	assert(drawable->tile != 0);
	gimp_tile_ref(drawable->tile);

	drawable->col = newcol;
	drawable->row = newrow;
    }

    p = drawable->tile->data + drawable->tile->bpp * (drawable->tile->ewidth * newrowoff + newcoloff);

    if (drawable->bpp == 1 || drawable->bpp == 2)
	pixel[0] = pixel[1] = pixel[2] = p[0];
    else if (drawable->bpp == 3 || drawable->bpp == 4)
	for (i = 0; i < 3; ++i)
	    pixel[i] = p[i];
    else
	assert(0);

    if (drawable->bpp == 1 || drawable->bpp == 3)
	pixel[3] = 255;
    else
	pixel[3] = p[drawable->bpp - 1];
}

void
mathmap_get_fast_pixel (mathmap_invocation_t *invocation, int drawable_index, int x, int y, guchar *pixel)
{
    input_drawable_t *drawable;

    if (drawable_index < 0 || drawable_index >= MAX_INPUT_DRAWABLES || !input_drawables[drawable_index].used
	|| x < 0 || x >= preview_width
	|| y < 0 || y >= preview_height)
    {
	int i;

	for (i = 0; i < output_bpp; ++i)
	    pixel[i] = edge_color[i];
	return;
    }

    drawable = &input_drawables[drawable_index];

    if (drawable->fast_image_source == 0)
	build_fast_image_source(drawable);

    memcpy(pixel, drawable->fast_image_source + (x + y * preview_width) * 4, 4);
}

/*****/

static void
build_fast_image_source (input_drawable_t *drawable)
{
    guchar *p;
    int x, y;

    assert(drawable->fast_image_source == 0);

    p = drawable->fast_image_source = g_malloc(preview_width * preview_height * 4);

    for (y = 0; y < preview_height; ++y)
    {
	for (x = 0; x < preview_width; ++x)
	{
	    mathmap_get_pixel(invocation,
			      drawable - input_drawables, 0,
			      sel_x1 + x * sel_width / preview_width,
			      sel_y1 + y * sel_height / preview_height, p);
	    p += 4;
	}
    }
}

/*****/

static GtkWidget*
tree_from_lisp_object (GtkWidget *root_item, lisp_object_t *obj)
{
    GtkWidget *tree = gtk_tree_new();

    if (root_item != 0)
	gtk_tree_item_set_subtree(GTK_TREE_ITEM(root_item), tree);

    for (; lisp_type(obj) != LISP_TYPE_NIL; obj = lisp_cdr(obj))
    {
	lisp_object_t *vars[2];
	GtkWidget *item = 0;

	assert(lisp_type(obj) == LISP_TYPE_CONS);

	if (lisp_match_string("(group #?(string) . #?(list))", lisp_car(obj), vars))
	{
	    item = gtk_tree_item_new_with_label(lisp_string(vars[0]));
	    gtk_tree_append(GTK_TREE(tree), item);
	    gtk_widget_show(item);
	    tree_from_lisp_object(item, vars[1]);
	}
	else if (lisp_match_string("(expression #?(string) #?(string))", lisp_car(obj), vars))
	{
	    item = gtk_tree_item_new_with_label(lisp_string(vars[0]));
	    gtk_tree_append(GTK_TREE(tree), item);
	    gtk_widget_show(item);
	    gtk_object_set_user_data(GTK_OBJECT(item),
				     strcpy((char*)malloc(strlen(lisp_string(vars[1])) + 1),
					    lisp_string(vars[1])));
	}
	else
	    assert(0);
    }

    gtk_widget_show(tree);

    if (root_item != 0)
	gtk_tree_item_expand(GTK_TREE_ITEM(root_item));

    return tree;
}

static GtkWidget*
read_tree_from_rc (void)
{
    GtkWidget *tree;
    lisp_object_t *obj = read_rc_file();

    if (obj == 0)
    {
	tree = gtk_tree_new();
	gtk_widget_show(tree);
	return tree;
    }

    tree = tree_from_lisp_object(0, obj);
    lisp_free(obj);

    return tree;
}

/*****/

static void
update_userval_table (void)
{
    if (uservalues_table != 0)
    {
	gtk_container_remove(GTK_CONTAINER(GTK_BIN(uservalues_scrolled_window)->child), uservalues_table);
	uservalues_table = 0;
    }

    uservalues_table = make_userval_table(mathmap->userval_infos, invocation->uservals);

    if (uservalues_table != 0)
    {
#if GTK_MAJOR_VERSION < 1 || (GTK_MAJOR_VERSION == 1 && GTK_MINOR_VERSION < 1)
	gtk_container_add(GTK_CONTAINER(uservalues_scrolled_window), uservalues_table);
#else
	gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(uservalues_scrolled_window), uservalues_table);
#endif
    }
}


/*****/

static gint
mathmap_dialog (int mutable_expression)
{
    static int edge_behaviour_color = EDGE_BEHAVIOUR_COLOR;
    static int edge_behaviour_wrap = EDGE_BEHAVIOUR_WRAP;
    static int edge_behaviour_reflect = EDGE_BEHAVIOUR_REFLECT;

    GtkWidget *dialog;
    GtkWidget *top_table, *middle_table;
    GtkWidget *vbox;
    GtkWidget *frame;
    GtkWidget *table;
    GtkWidget *button;
    GtkWidget *label;
    GtkWidget *toggle;
    GtkWidget *alignment;
    GtkWidget *root_tree;
    GtkWidget *scale;
    GtkWidget *vscrollbar;
    GtkWidget *notebook;
    GtkObject *adjustment;
    GSList *edge_group = 0;
    gint        argc,
	position = 0;
    gchar     **argv;
    guchar     *color_cube;

    argc    = 1;
    argv    = g_new(gchar *, 1);
    argv[0] = g_strdup("mathmap");

    gtk_init(&argc, &argv);

    gtk_preview_set_gamma(gimp_gamma());
    gtk_preview_set_install_cmap(gimp_install_cmap());
    color_cube = gimp_color_cube();
    gtk_preview_set_color_cube(color_cube[0], color_cube[1], color_cube[2], color_cube[3]);

    gtk_widget_set_default_visual(gtk_preview_get_visual());
    gtk_widget_set_default_colormap(gtk_preview_get_cmap());

    wint.wimage = g_malloc(preview_width * preview_height * 3 * sizeof(guchar));

    dialog = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "MathMap");
    gtk_window_position(GTK_WINDOW(dialog), GTK_WIN_POS_MOUSE);
    gtk_container_border_width(GTK_CONTAINER(dialog), 0);
    gtk_signal_connect(GTK_OBJECT(dialog), "destroy",
		       (GtkSignalFunc) dialog_close_callback,
		       NULL);

    top_table = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->vbox), top_table, TRUE, TRUE, 0);
    gtk_widget_show(top_table);

    /* Preview */

    vbox = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top_table), vbox, FALSE, FALSE, 0);
    gtk_widget_show(vbox);
    frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_IN);
    gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 0);
    gtk_widget_show(frame);

    wint.preview = gtk_preview_new(GTK_PREVIEW_COLOR);
    gtk_preview_size(GTK_PREVIEW(wint.preview), preview_width, preview_height);
    gtk_container_add(GTK_CONTAINER(frame), wint.preview);
    gtk_widget_show(wint.preview);

    button = gtk_button_new_with_label(_("Preview"));
    gtk_signal_connect(GTK_OBJECT(button), "clicked",
		       (GtkSignalFunc)dialog_preview_callback, 0);
    gtk_box_pack_start(GTK_BOX(vbox), button, FALSE, FALSE, 0);
    gtk_widget_show(button);

    /* Notebook */

    notebook = gtk_notebook_new();
    gtk_notebook_set_tab_pos (GTK_NOTEBOOK (notebook), GTK_POS_TOP);
    gtk_box_pack_start(GTK_BOX(top_table), notebook, TRUE, TRUE, 0);
    gtk_widget_show(notebook);

	/* Settings */

	middle_table = gtk_table_new(3, 2, FALSE);
	gtk_container_border_width(GTK_CONTAINER(middle_table), 6);
	gtk_table_set_col_spacings(GTK_TABLE(middle_table), 4);
	gtk_widget_show(middle_table);

            /* Sampling */

            table = gtk_table_new(2, 1, FALSE);
	    gtk_container_border_width(GTK_CONTAINER(table), 6);
	    gtk_table_set_row_spacings(GTK_TABLE(table), 4);
    
	    frame = gtk_frame_new(NULL);
	    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
	    gtk_container_add(GTK_CONTAINER(frame), table);
	    gtk_table_attach(GTK_TABLE(middle_table), frame, 0, 1, 0, 1, GTK_FILL, 0, 0, 0);

	    gtk_widget_show(table);
	    gtk_widget_show(frame);

                /* Antialiasing */

		toggle = gtk_check_button_new_with_label(_("Antialiasing"));
		gtk_toggle_button_set_state(GTK_TOGGLE_BUTTON(toggle),
					    mmvals.flags & FLAG_ANTIALIASING);
		gtk_table_attach(GTK_TABLE(table), toggle, 0, 1, 0, 1, GTK_FILL, 0, 0, 0);
		gtk_signal_connect(GTK_OBJECT(toggle), "toggled",
				   (GtkSignalFunc)dialog_antialiasing_update, 0);
		gtk_widget_show(toggle);

		/* Supersampling */
	    
		toggle = gtk_check_button_new_with_label(_("Supersampling"));
		gtk_toggle_button_set_state(GTK_TOGGLE_BUTTON(toggle),
					    mmvals.flags & FLAG_SUPERSAMPLING);
		gtk_table_attach(GTK_TABLE(table), toggle, 0, 1, 1, 2, GTK_FILL, 0, 0, 0);
		gtk_signal_connect(GTK_OBJECT(toggle), "toggled",
				   (GtkSignalFunc)dialog_supersampling_update, 0);
		gtk_widget_show(toggle);

	    /* Preview Options */

            table = gtk_table_new(2, 1, FALSE);
	    gtk_container_border_width(GTK_CONTAINER(table), 6);
	    gtk_table_set_row_spacings(GTK_TABLE(table), 4);
    
	    frame = gtk_frame_new(NULL);
	    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
	    gtk_container_add(GTK_CONTAINER(frame), table);
	    gtk_table_attach(GTK_TABLE(middle_table), frame, 0, 1, 1, 2, GTK_FILL, 0, 0, 0);

	    gtk_widget_show(table);
	    gtk_widget_show(frame);

	        /* Auto Preview */

	        toggle = gtk_check_button_new_with_label(_("Auto Preview"));
		gtk_toggle_button_set_state(GTK_TOGGLE_BUTTON(toggle), auto_preview);
		gtk_table_attach(GTK_TABLE(table), toggle, 0, 1, 0, 1, GTK_FILL, 0, 0, 0);
		gtk_signal_connect(GTK_OBJECT(toggle), "toggled",
				   (GtkSignalFunc)dialog_auto_preview_update, 0);
		gtk_widget_show(toggle);

	        /* Fast Preview */

		toggle = gtk_check_button_new_with_label(_("Fast Preview"));
		gtk_toggle_button_set_state(GTK_TOGGLE_BUTTON(toggle), fast_preview);
		gtk_table_attach(GTK_TABLE(table), toggle, 0, 1, 1, 2, GTK_FILL, 0, 0, 0);
		gtk_signal_connect(GTK_OBJECT(toggle), "toggled",
				   (GtkSignalFunc)dialog_fast_preview_update, 0);
		gtk_widget_show(toggle);

	    /* Edge Behaviour */

	    table = gtk_table_new(2, 3, FALSE);
	    gtk_container_border_width(GTK_CONTAINER(table), 6);
	    gtk_table_set_row_spacings(GTK_TABLE(table), 4);

	    frame = gtk_frame_new(_("Edge Behaviour"));
	    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
	    gtk_container_add(GTK_CONTAINER(frame), table);
	    gtk_table_attach(GTK_TABLE(middle_table), frame, 0, 1, 2, 3, GTK_FILL, 0, 0, 0);

	    gtk_widget_show(table);
	    gtk_widget_show(frame);

	        /* Color */

	        toggle = gtk_radio_button_new_with_label(edge_group, _("Color"));
		edge_group = gtk_radio_button_group(GTK_RADIO_BUTTON(toggle));
		gtk_table_attach(GTK_TABLE(table), toggle, 0, 1, 0, 1, GTK_FILL, 0, 0, 0);
		gtk_signal_connect(GTK_OBJECT(toggle), "toggled",
				   (GtkSignalFunc)dialog_edge_behaviour_update, &edge_behaviour_color);
		gtk_widget_show(toggle);

		edge_color_well = gimp_color_button_new(_("Edge Color"), 32, 16, edge_color, 4);
		gtk_signal_connect(GTK_OBJECT(edge_color_well), "color_changed",
				   (GtkSignalFunc)dialog_edge_color_changed, 0);
		gtk_widget_show(edge_color_well);
		gtk_table_attach(GTK_TABLE(table), edge_color_well, 1, 2, 0, 1, GTK_FILL, 0, 0, 0);

	        /* Wrap */

	        toggle = gtk_radio_button_new_with_label(edge_group, _("Wrap"));
		edge_group = gtk_radio_button_group(GTK_RADIO_BUTTON(toggle));
		gtk_table_attach(GTK_TABLE(table), toggle, 0, 1, 1, 2, GTK_FILL, 0, 0, 0);
		gtk_signal_connect(GTK_OBJECT(toggle), "toggled",
				   (GtkSignalFunc)dialog_edge_behaviour_update, &edge_behaviour_wrap);
		gtk_widget_show(toggle);

	        /* Reflect */

	        toggle = gtk_radio_button_new_with_label(edge_group, _("Reflect"));
		edge_group = gtk_radio_button_group(GTK_RADIO_BUTTON(toggle));
		gtk_table_attach(GTK_TABLE(table), toggle, 0, 1, 2, 3, GTK_FILL, 0, 0, 0);
		gtk_signal_connect(GTK_OBJECT(toggle), "toggled",
				   (GtkSignalFunc)dialog_edge_behaviour_update, &edge_behaviour_reflect);
		gtk_widget_show(toggle);

	    /* Animation */
	    
	    table = gtk_table_new(4, 1, FALSE);
	    gtk_container_border_width(GTK_CONTAINER(table), 6);
	    gtk_table_set_row_spacings(GTK_TABLE(table), 4);
	    gtk_table_set_col_spacings(GTK_TABLE(table), 4);
    
	    frame = gtk_frame_new(NULL);
	    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
	    gtk_container_add(GTK_CONTAINER(frame), table);

	    alignment = gtk_alignment_new(0, 0, 0, 0);
	    gtk_container_add(GTK_CONTAINER(alignment), frame);
	    gtk_table_attach(GTK_TABLE(middle_table), alignment, 1, 2, 0, 3, GTK_FILL, 0, 0, 0);

	    gtk_widget_show(table);
	    gtk_widget_show(frame);
	    gtk_widget_show(alignment);

	        /* Animation Toggle */

	        alignment = gtk_alignment_new(0, 0, 0, 0);
		toggle = gtk_check_button_new_with_label(_("Animate"));
		gtk_container_add(GTK_CONTAINER(alignment), toggle);
		gtk_toggle_button_set_state(GTK_TOGGLE_BUTTON(toggle),
					    mmvals.flags & FLAG_ANIMATION);
		gtk_table_attach(GTK_TABLE(table), alignment, 0, 1, 0, 1, GTK_FILL, 0, 0, 0);
		gtk_signal_connect(GTK_OBJECT(toggle), "toggled",
				   (GtkSignalFunc)dialog_animation_update, 0);
		gtk_widget_show(toggle);
		gtk_widget_show(alignment);

		/* Number of Frames */

		frame_table = gtk_table_new(1, 2, FALSE);
		gtk_table_set_col_spacings(GTK_TABLE(frame_table), 4);
		gtk_table_attach(GTK_TABLE(table), frame_table, 0, 1, 1, 2, GTK_FILL, 0, 0, 0);

		label = gtk_label_new(_("Frames"));
		gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
		gtk_table_attach(GTK_TABLE(frame_table), label, 0, 1, 0, 1, GTK_FILL, 0, 0, 0);
		adjustment = gtk_adjustment_new(mmvals.frames, 2, 100, 1.0, 1.0, 0.0);
		scale = gtk_hscale_new(GTK_ADJUSTMENT(adjustment));
		gtk_widget_set_usize(scale, 100, 0);
		gtk_table_attach (GTK_TABLE (frame_table), scale, 1, 2, 0, 1, GTK_FILL, 0, 0, 0);
		gtk_scale_set_value_pos (GTK_SCALE (scale), GTK_POS_TOP);
		gtk_scale_set_digits(GTK_SCALE(scale),0);
		gtk_range_set_update_policy (GTK_RANGE (scale), GTK_UPDATE_DELAYED);
		gtk_signal_connect (GTK_OBJECT (adjustment), "value_changed",
				    (GtkSignalFunc) dialog_scale_update,
				    &mmvals.frames);
		gtk_widget_show(label);
		gtk_widget_show(scale);

		gtk_widget_show(frame_table);
		gtk_widget_set_sensitive(frame_table, mmvals.flags & FLAG_ANIMATION);

		/* Periodic */

	        alignment = gtk_alignment_new(0, 0, 0, 0);
		toggle = gtk_check_button_new_with_label(_("Periodic"));
		gtk_container_add(GTK_CONTAINER(alignment), toggle);
		gtk_toggle_button_set_state(GTK_TOGGLE_BUTTON(toggle), mmvals.flags & FLAG_PERIODIC);
		gtk_table_attach(GTK_TABLE(table), alignment, 0, 1, 2, 3, GTK_FILL, 0, 0, 0);
		gtk_signal_connect(GTK_OBJECT(toggle), "toggled",
				   (GtkSignalFunc)dialog_periodic_update, 0);
		gtk_widget_show(toggle);
		gtk_widget_show(alignment);

		/* t */

		t_table = gtk_table_new(1, 2, FALSE);
		gtk_table_set_col_spacings(GTK_TABLE(t_table), 4);
		gtk_table_attach(GTK_TABLE(table), t_table, 0, 1, 4, 5, GTK_FILL, 0, 0, 0);

		label = gtk_label_new(_("Parameter t"));
		gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
		gtk_table_attach(GTK_TABLE(t_table), label, 0, 1, 0, 1, GTK_FILL, 0, 0, 0);
		adjustment = gtk_adjustment_new(mmvals.param_t, 0.0, 1.0, 0.01, 0.1, 0.0);
		scale = gtk_hscale_new(GTK_ADJUSTMENT(adjustment));
		gtk_widget_set_usize(scale, 100, 0);
		gtk_table_attach (GTK_TABLE (t_table), scale, 1, 2, 0, 1, GTK_FILL, 0, 0, 0);
		gtk_scale_set_value_pos (GTK_SCALE (scale), GTK_POS_TOP);
		gtk_scale_set_digits(GTK_SCALE(scale),2);
		gtk_range_set_update_policy (GTK_RANGE (scale), GTK_UPDATE_CONTINUOUS);
		gtk_signal_connect (GTK_OBJECT (adjustment), "value_changed",
				    (GtkSignalFunc) dialog_t_update,
				    &mmvals.param_t);
		gtk_widget_show(label);
		gtk_widget_show(scale);

		gtk_widget_show(t_table);
		gtk_widget_set_sensitive(t_table, !(mmvals.flags & FLAG_ANIMATION));

	label = gtk_label_new(_("Settings"));
	gtk_widget_show(label);
	gtk_notebook_append_page_menu(GTK_NOTEBOOK(notebook), middle_table, label, label);

        /* Expression */

	if (mutable_expression)
	{
	    table = gtk_hbox_new(FALSE, 0);
	    gtk_widget_show(table);

	    label = gtk_label_new(_("Expression"));
	    gtk_widget_show(label);
	    gtk_notebook_append_page_menu(GTK_NOTEBOOK(notebook), table, label, label);

	    expression_entry = gtk_text_new(NULL, NULL);
	    gtk_signal_connect(GTK_OBJECT(expression_entry), "changed",
			       (GtkSignalFunc)dialog_text_changed,
			       (gpointer)NULL);
	    gtk_text_set_editable(GTK_TEXT(expression_entry), TRUE);
	    gtk_box_pack_start(GTK_BOX(table), expression_entry, TRUE, TRUE, 0);
	    gtk_widget_show(expression_entry);
	    /* gtk_text_freeze(GTK_TEXT(expression_entry)); */
	    gtk_widget_realize(expression_entry);
	    /* gtk_text_thaw(GTK_TEXT(expression_entry)); */
	    gtk_editable_insert_text(GTK_EDITABLE(expression_entry), mmvals.expression,
				     strlen(mmvals.expression), &position);

	    vscrollbar = gtk_vscrollbar_new(GTK_TEXT(expression_entry)->vadj);
	    gtk_box_pack_start(GTK_BOX(table), vscrollbar, FALSE, FALSE, 0);
	    gtk_widget_show (vscrollbar);
	}

	/* User Values */

	uservalues_scrolled_window = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW(uservalues_scrolled_window),
					GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_widget_show(uservalues_scrolled_window);

	uservalues_table = 0;

	label = gtk_label_new(_("User Values"));
	gtk_widget_show(label);
	gtk_notebook_append_page_menu(GTK_NOTEBOOK(notebook), uservalues_scrolled_window, label, label);

	/* Examples */

	if (mutable_expression)
	{
	    table = gtk_scrolled_window_new (NULL, NULL);
	    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW(table),
					    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	    gtk_widget_show (table);

	    root_tree = read_tree_from_rc();
	    gtk_signal_connect(GTK_OBJECT(root_tree), "selection_changed",
			       (GtkSignalFunc)dialog_tree_changed,
			       (gpointer)NULL);
#if GTK_MAJOR_VERSION < 1 || (GTK_MAJOR_VERSION == 1 && GTK_MINOR_VERSION < 1)
	    gtk_container_add(GTK_CONTAINER(table), root_tree);
#else
	    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(table), root_tree);
#endif
	    gtk_tree_set_selection_mode(GTK_TREE(root_tree), GTK_SELECTION_BROWSE);
	    gtk_tree_set_view_lines(GTK_TREE(root_tree), FALSE);
	    gtk_tree_set_view_mode(GTK_TREE(root_tree), FALSE);
	    gtk_widget_show(root_tree);

	    label = gtk_label_new(_("Examples"));
	    gtk_widget_show(label);
	    gtk_notebook_append_page_menu(GTK_NOTEBOOK(notebook), table, label, label);
	}

    /* Buttons */

    gtk_container_border_width(GTK_CONTAINER(GTK_DIALOG(dialog)->action_area), 6);

    button = gtk_button_new_with_label(_("OK"));
    GTK_WIDGET_SET_FLAGS(button, GTK_CAN_DEFAULT);
    gtk_signal_connect(GTK_OBJECT(button), "clicked",
		       (GtkSignalFunc) dialog_ok_callback,
		       dialog);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->action_area), button, TRUE, TRUE, 0);
    gtk_widget_grab_default(button);
    gtk_widget_show(button);

    button = gtk_button_new_with_label(_("Cancel"));
    GTK_WIDGET_SET_FLAGS(button, GTK_CAN_DEFAULT);
    gtk_signal_connect(GTK_OBJECT(button), "clicked",
		       (GtkSignalFunc) dialog_cancel_callback,
		       dialog);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->action_area), button, TRUE, TRUE, 0);
    gtk_widget_show(button);

    button = gtk_button_new_with_label(_("Help"));
    GTK_WIDGET_SET_FLAGS(button, GTK_CAN_DEFAULT);
    gtk_signal_connect(GTK_OBJECT(button), "clicked",
		       (GtkSignalFunc) dialog_help_callback,
		       dialog);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->action_area), button, TRUE, TRUE, 0);
    gtk_widget_show(button);

    button = gtk_button_new_with_label(_("About"));
    GTK_WIDGET_SET_FLAGS(button, GTK_CAN_DEFAULT);
    gtk_signal_connect(GTK_OBJECT(button), "clicked",
		       (GtkSignalFunc) dialog_about_callback,
		       dialog);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->action_area), button, TRUE, TRUE, 0);
    gtk_widget_show(button);

    /* Done */

    if (!mutable_expression)
	dialog_update_preview();

    gtk_widget_show(dialog);

    gtk_main();
    gdk_flush();

    unref_tiles();

    g_free(wint.wimage);

    return wint.run;
} /* mathmap_dialog */


/*****/

void
user_value_changed (void)
{
    if (auto_preview)
	dialog_update_preview();
}

static void
dialog_update_preview (void)
{
    double left, right, bottom, top;
    double dx, dy;
    int x, y;
    guchar *p_ul, *p_lr, *p;
    gint check, check_0, check_1; 

    previewing = fast_preview;

    left = sel_x1;
    right = sel_x2 - 1;
    bottom = sel_y2 - 1;
    top = sel_y1;

    dx = (right - left) / (preview_width - 1);
    dy = (bottom - top) / (preview_height - 1);

    p_ul = wint.wimage;
    p_lr = wint.wimage + 3 * (preview_width * preview_height - 1);

    if (generate_code(0, mmvals.param_t))
    {
	update_uservals(mathmap->userval_infos, invocation->uservals);

	for (y = 0; y < preview_height; y++)
	{
	    if ((y / CHECK_SIZE) & 1) {
		check_0 = CHECK_DARK;
		check_1 = CHECK_LIGHT;
	    } else {
		check_0 = CHECK_LIGHT;
		check_1 = CHECK_DARK;
	    }                        
	    for (x = 0; x < preview_width; x++)
	    {
		tuple_t *result;
		float redf, greenf, bluef, alphaf;

		invocation->current_x = x * sel_width / preview_width - invocation->middle_x;
		invocation->current_y = -(y * sel_height / preview_height - invocation->middle_y);

		calc_ra(invocation);
		update_pixel_internals(invocation);

		result = call_invocation(invocation);
		tuple_to_color(result, &redf, &greenf, &bluef, &alphaf);

		if (input_drawables[0].bpp < 2)
		    redf = greenf = bluef = 0.299 * redf + 0.587 * greenf + 0.114 * bluef;

		p_ul[0] = redf * 255;
		p_ul[1] = greenf * 255;
		p_ul[2] = bluef * 255;

		if (output_bpp == 2 || output_bpp == 4)
		{
		    if (((x) / CHECK_SIZE) & 1)
			check = check_0;
		    else
			check = check_1;
		    p_ul[0] = check + (p_ul[0] - check) * alphaf;
		    p_ul[1] = check + (p_ul[1] - check) * alphaf;
		    p_ul[2] = check + (p_ul[2] - check) * alphaf;
		}

		p_ul += 3;
	    }
	}

	p = wint.wimage;

	for (y = 0; y < preview_height; y++)
	{
	    gtk_preview_draw_row(GTK_PREVIEW(wint.preview), p, 0, y, preview_width);

	    p += preview_width * 3;
	}

	gtk_widget_draw(wint.preview, NULL);
	gdk_flush();
    }
} /* dialog_update_preview */


/*****/

static void
dialog_scale_update (GtkAdjustment *adjustment, gint *value)
{
    *value = (gint)adjustment->value;
} /* dialog_scale_update */


/*****/

static void
dialog_t_update (GtkAdjustment *adjustment, gfloat *value)
{
    *value = (gfloat)adjustment->value;

    if (auto_preview)
	dialog_update_preview();
} /* dialog_scale_update */


/*****/

static void
dialog_text_changed (void)
{
    expression_changed = 1;
}

/*****/

static void
dialog_text_update (void)
{
    guint length = gtk_text_get_length(GTK_TEXT(expression_entry));
    char *expression = (char*)malloc(length + 1);
    int i;

    for (i = 0; i < length; ++i)
	expression[i] = GTK_TEXT_INDEX(GTK_TEXT(expression_entry), i);
    expression[i] = '\0';

    expression_copy(mmvals.expression, expression);

    free(expression);
} /* dialog_text_update */

/*****/

static void
dialog_supersampling_update (GtkWidget *widget, gpointer data)
{
    mmvals.flags &= ~FLAG_SUPERSAMPLING;

    if (GTK_TOGGLE_BUTTON(widget)->active)
	mmvals.flags |= FLAG_SUPERSAMPLING;
}

/*****/

static void
dialog_auto_preview_update (GtkWidget *widget, gpointer data)
{
    auto_preview = GTK_TOGGLE_BUTTON(widget)->active;
}

/*****/

static void
dialog_fast_preview_update (GtkWidget *widget, gpointer data)
{
    fast_preview = GTK_TOGGLE_BUTTON(widget)->active;
    if (auto_preview)
	dialog_update_preview();
}

/*****/

static void
dialog_edge_behaviour_update (GtkWidget *widget, gpointer data)
{
    edge_behaviour_mode = *(int*)data;
    if (edge_behaviour_mode == EDGE_BEHAVIOUR_COLOR)
	gtk_widget_set_sensitive(edge_color_well, 1);
    else
	gtk_widget_set_sensitive(edge_color_well, 0);

    if (auto_preview)
	dialog_update_preview();
}

static void
dialog_edge_color_changed (GtkWidget *color_well, gpointer data)
{
    if (auto_preview)
	dialog_update_preview();
}

/*****/

static void
dialog_antialiasing_update (GtkWidget *widget, gpointer data)
{
    mmvals.flags &= ~FLAG_ANTIALIASING;

    if (GTK_TOGGLE_BUTTON(widget)->active)
	mmvals.flags |= FLAG_ANTIALIASING;

    expression_changed = 1;

    if (auto_preview)
	dialog_update_preview();
}

/*****/

static void
dialog_animation_update (GtkWidget *widget, gpointer data)
{
    mmvals.flags &= ~FLAG_ANIMATION;

    if (GTK_TOGGLE_BUTTON(widget)->active)
	mmvals.flags |= FLAG_ANIMATION;

    gtk_widget_set_sensitive(frame_table, mmvals.flags & FLAG_ANIMATION);
    gtk_widget_set_sensitive(t_table, !(mmvals.flags & FLAG_ANIMATION));
}

/*****/

static void
dialog_periodic_update (GtkWidget *widget, gpointer data)
{
    mmvals.flags &= ~FLAG_PERIODIC;

    if (GTK_TOGGLE_BUTTON(widget)->active)
	mmvals.flags |= FLAG_PERIODIC;
}

/*****/

static void
dialog_preview_callback (GtkWidget *widget, gpointer data)
{
    update_gradient();
    dialog_update_preview();
}

/*****/

static void
dialog_close_callback (GtkWidget *widget, gpointer data)
{
    gtk_main_quit();
} /* dialog_close_callback */


/*****/

static void
dialog_ok_callback (GtkWidget *widget, gpointer data)
{
    if (generate_code(0, 0))
    {
	wint.run = TRUE;
	gtk_widget_destroy(GTK_WIDGET(data));
    }
} /* dialog_ok_callback */


/*****/

static void
dialog_cancel_callback (GtkWidget *widget, gpointer data)
{
    gtk_widget_destroy(GTK_WIDGET(data));
} /* dialog_cancel_callback */

/*****/

static void
dialog_help_callback (GtkWidget *widget, gpointer data)
{
    char *proc_blurb, *proc_help, *proc_author, *proc_copyright, *proc_date;
    int nparams, nreturn_vals;
    GimpParamDef *params, *return_vals;
    gint baz;
    GimpPDBProcType proc_type;

    if (gimp_procedural_db_proc_info("extension_web_browser",
				     &proc_blurb, &proc_help, 
				     &proc_author, &proc_copyright, &proc_date,
				     &proc_type, &nparams, &nreturn_vals,
				     &params, &return_vals))
	gimp_run_procedure("extension_web_browser", &baz,
			   GIMP_PDB_INT32, GIMP_RUN_NONINTERACTIVE,
			   GIMP_PDB_STRING, MATHMAP_MANUAL_URL,
			   GIMP_PDB_INT32, 1,
			   GIMP_PDB_END);
    else 
    {
	gchar *message = g_strdup_printf(_("See %s"), MATHMAP_MANUAL_URL);

	gimp_message(message);
	g_free(message);
    }                                            
} /* dialog_help_callback */

/*****/

static void
dialog_about_callback (GtkWidget *widget, gpointer data)
{
    gchar *message = g_strdup_printf("Mathmap %s\n%s",
				     MATHMAP_VERSION,
				     _("written by\n"
				       "Mark Probst <schani@complang.tuwien.ac.at>"));

    gimp_message(message);
    g_free(message);
} /* dialog_about_callback */

/*****/

static void
dialog_tree_changed (GtkTree *tree)
{
    GList *selection;
    GtkWidget *tree_item;

    selection = GTK_TREE_SELECTION(tree);

    if (g_list_length(selection) != 1)
	return;

    tree_item = GTK_WIDGET(selection->data);

    if (gtk_object_get_user_data(GTK_OBJECT(tree_item)) != 0)
    {
	char *expression = (char*)gtk_object_get_user_data(GTK_OBJECT(tree_item));
	gint position = 0;

	tree_item = GTK_WIDGET(selection->data);
	
	gtk_editable_delete_text(GTK_EDITABLE(expression_entry), 0,
				 gtk_text_get_length(GTK_TEXT(expression_entry)));
	gtk_editable_insert_text(GTK_EDITABLE(expression_entry), expression, strlen(expression),
				 &position);

	expression_copy(mmvals.expression, expression);
    }

    if (auto_preview)
	dialog_update_preview();
}
