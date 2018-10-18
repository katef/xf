/*
 * Copyright 2018 Katherine Flavel
 *
 * See LICENCE for the full copyright terms.
 */

#define _XOPEN_SOURCE 500

#include <sys/types.h>

#include <assert.h>
#include <stdbool.h>
#include <limits.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>

#include <unistd.h>
#include <pthread.h>

#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>

#include <cairo.h>
#include <cairo-pdf.h>
#include <cairo-svg.h>
#include <cairo-xcb.h>

#include <pango/pangocairo.h>

#include <flex.h>

#define MAX_LINE_LEN 8192

enum format {
	FMT_PDF,
	FMT_PNG,
	FMT_SVG,
	FMT_XCB,
};

enum op_type {
	OP_OPEN,
	OP_CLOSE,
	OP_CA,
	OP_BG,
	OP_FG,
	OP_FONT,
	OP_DIR,
	OP_WRAP,
	OP_ELLIPSIZE,
	OP_JUSTIFY_CONTENT,
	OP_ALIGN_ITEMS,
	OP_ALIGN_SELF,
	OP_SHRINK,
	OP_ORDER,
	OP_GROW,
	OP_BASIS,
	OP_IMG,
	OP_RULE,
	OP_MARKUP,
	OP_TEXT
};

/* TODO: moveto, drawing lines, images etc */
enum act_type {
	ACT_IMG,
	ACT_RULE,
	ACT_TEXT
};

struct rgba {
	float r;
	float g;
	float b;
	float a;
};

struct geom {
	double x;
	double y;
	double w;
	double h;
};

struct outline {
	double t;
	double r;
	double b;
	double l;
	double w;
	double h;
};

struct act {
	enum act_type type;

	struct flex_item *item;
	struct geom f;
	struct outline m;
	struct outline p;
	PangoColor bg;
	const char *ca_name;

	union {
		struct act_img {
			cairo_surface_t *img;
		} img;

		struct act_rule {
			PangoColor fg;
		} rule;

		struct act_text {
			PangoFontDescription *desc;
			PangoEllipsizeMode e;
			PangoColor fg;
			void (*f)(PangoLayout *, const char *, int);
			const char *s;
		} text;
	} u;
};

struct eval_ctx {
	struct act *b;
	size_t n;
	struct flex_item *root;
};

struct ui_ctx {
	xcb_connection_t *xcb;
	cairo_t *cr;

	/* input */
	const struct act *b;
	const size_t *n;
};

static enum format
ext(const char *file)
{
	const char *p;
	size_t i;

	struct {
		const char *ext;
		enum format fmt;
	} a[] = {
		{ "pdf", FMT_PDF },
		{ "png", FMT_PNG },
		{ "svg", FMT_SVG }
	};

	assert(file != NULL);

	p = strrchr(file, '.');
	if (p == NULL) {
		fprintf(stderr, "%s: file extension not found\n", file);
		exit(1);
	}

	p++;

	for (i = 0; i < sizeof a / sizeof *a; i++) {
		if (0 == strcmp(a[i].ext, p)) {
			return a[i].fmt;
		}
	}

	fprintf(stderr, "unrecognised file extension. supported formats are:");

	for (i = 0; i < sizeof a / sizeof *a; i++) {
		fprintf(stderr, " %s", a[i].ext);
	}

	fprintf(stderr, "\n");

	exit(1);
}

static float
flex_strtof(const char *s, float min, float max)
{
	float x;
	char *e;

	assert(s != NULL);

	errno = 0;
	x = strtof(s, &e);
	if (s[0] == '\0' || *e != '\0') {
		fprintf(stderr, "%s: invalid float\n", s);
	}

	if (x == HUGE_VALF && errno != 0) {
		perror(s);
		exit(1);
	}

	if (x < min || x > max) {
		fprintf(stderr, "%s: out of range\n", s);
		exit(1);
	}

	return x;
}

static int
flex_strtoi(const char *s, int min, int max)
{
	long l;
	char *e;

	assert(s != NULL);

	errno = 0;
	l = strtol(s, &e, 10);
	if (s[0] == '\0' || *e != '\0') {
		fprintf(stderr, "%s: invalid integer\n", s);
	}

	if ((l == LONG_MIN || l == LONG_MAX) && errno == ERANGE) {
		perror(s);
		exit(1);
	}

	if (l < INT_MIN || l > INT_MAX || l < min || l > max) {
		fprintf(stderr, "%s: out of range\n", s);
		exit(1);
	}

	return (int) l;
}

/* frame coordinates contain the padding but not the margins */
static struct geom
flex_item_get_frame(struct flex_item *item)
{
	struct geom f;

	assert(item != NULL);

	f.x = flex_item_get_frame_x(item);
	f.y = flex_item_get_frame_y(item);
	f.w = flex_item_get_frame_width(item);
	f.h = flex_item_get_frame_height(item);

	return f;
}

static struct outline
flex_item_get_margin(struct flex_item *item)
{
	struct outline o;

	o.t = flex_item_get_margin_top(item);
	o.r = flex_item_get_margin_right(item);
	o.b = flex_item_get_margin_bottom(item);
	o.l = flex_item_get_margin_left(item);
	o.w = o.l + o.r;
	o.h = o.t + o.b;

	return o;
}

static struct outline
flex_item_get_padding(struct flex_item *item)
{
	struct outline o;

	o.t = flex_item_get_padding_top(item);
	o.r = flex_item_get_padding_right(item);
	o.b = flex_item_get_padding_bottom(item);
	o.l = flex_item_get_padding_left(item);
	o.w = o.l + o.r;
	o.h = o.t + o.b;

	return o;
}

static bool
inside(const struct geom *g, double x, double y)
{
	assert(g != NULL);

	if (x < g->x || x > g->x + g->w) {
		return false;
	}

	if (y < g->y || y > g->y + g->h) {
		return false;
	}

	return true;
}

static PangoEllipsizeMode
ellipsize_name(const char *name)
{
	size_t i;

	struct {
		const char *name;
		PangoEllipsizeMode e;
	} a[] = {
		{ "none",   PANGO_ELLIPSIZE_NONE   },
		{ "start",  PANGO_ELLIPSIZE_START  },
		{ "middle", PANGO_ELLIPSIZE_MIDDLE },
		{ "end",    PANGO_ELLIPSIZE_END    }
	};

	assert(name != NULL);

	for (i = 0; i < sizeof a / sizeof *a; i++) {
		if (0 == strcmp(a[i].name, name)) {
			return a[i].e;
		}
	}

	fprintf(stderr, "%s: unrecognised ellipsize mode\n", name);
	exit(1);
}

static flex_direction
dir_name(const char *name)
{
	size_t i;

	struct {
		const char *name;
		flex_direction dir;
	} a[] = {
		{ "row",     FLEX_DIRECTION_ROW            },
		{ "row-rev", FLEX_DIRECTION_ROW_REVERSE    },
		{ "col",     FLEX_DIRECTION_COLUMN         },
		{ "col-rev", FLEX_DIRECTION_COLUMN_REVERSE }
	};

	assert(name != NULL);

	for (i = 0; i < sizeof a / sizeof *a; i++) {
		if (0 == strcmp(a[i].name, name)) {
			return a[i].dir;
		}
	}

	fprintf(stderr, "%s: unrecognised direction\n", name);
	exit(1);
}

static flex_wrap
wrap_name(const char *name)
{
	size_t i;

	struct {
		const char *name;
		flex_wrap wrap;
	} a[] = {
		{ "no-wrap",  FLEX_WRAP_NO_WRAP      },
		{ "wrap",     FLEX_WRAP_WRAP         },
		{ "wrap-rev", FLEX_WRAP_WRAP_REVERSE }
	};

	assert(name != NULL);

	for (i = 0; i < sizeof a / sizeof *a; i++) {
		if (0 == strcmp(a[i].name, name)) {
			return a[i].wrap;
		}
	}

	fprintf(stderr, "%s: unrecognised wrap\n", name);
	exit(1);
}

static flex_align
justify_content_name(const char *name)
{
	size_t i;

	struct {
		const char *name;
		flex_align align;
	} a[] = {
		{ "start",         FLEX_ALIGN_START         },
		{ "end",           FLEX_ALIGN_END           },
		{ "center",        FLEX_ALIGN_CENTER        },
		{ "space-between", FLEX_ALIGN_SPACE_BETWEEN },
		{ "space-around",  FLEX_ALIGN_SPACE_AROUND  },
		{ "space-evenly",  FLEX_ALIGN_SPACE_EVENLY  }
	};

	assert(name != NULL);

	for (i = 0; i < sizeof a / sizeof *a; i++) {
		if (0 == strcmp(a[i].name, name)) {
			return a[i].align;
		}
	}

	fprintf(stderr, "%s: unrecognised justify-content\n", name);
	exit(1);
}

static flex_align
align_name(const char *name)
{
	size_t i;

	struct {
		const char *name;
		flex_align align;
	} a[] = {
		{ "auto",     FLEX_ALIGN_AUTO     },
		{ "start",    FLEX_ALIGN_START    },
		{ "end",      FLEX_ALIGN_END      },
		{ "center",   FLEX_ALIGN_CENTER   },
//		{ "baseline", FLEX_ALIGN_BASELINE }, // not implemented
		{ "stretch",  FLEX_ALIGN_STRETCH  }
	};

	assert(name != NULL);

	for (i = 0; i < sizeof a / sizeof *a; i++) {
		if (0 == strcmp(a[i].name, name)) {
			return a[i].align;
		}
	}

	fprintf(stderr, "%s: unrecognised align-self\n", name);
	exit(1);
}

static enum op_type
op_name(const char *name)
{
	size_t i;

	/* "flow" and "flex" are shorthand for compositions of other items */

	static const struct {
		const char *name;
		enum op_type op;
	} a[] = {
		{ "ca",     OP_CA     },
		{ "bg",     OP_BG     },
		{ "fg",     OP_FG     },
		{ "font",   OP_FONT   },
		{ "dir",    OP_DIR    },
		{ "wrap",   OP_WRAP   },
		{ "ellipsize",       OP_ELLIPSIZE       },
		{ "justify-content", OP_JUSTIFY_CONTENT },
		{ "align-items",     OP_ALIGN_ITEMS     },
		{ "align-self",      OP_ALIGN_SELF      },
		{ "grow",   OP_GROW   },
		{ "shrink", OP_SHRINK },
		{ "order",  OP_ORDER  },
		{ "basis",  OP_BASIS  },
		{ "img",    OP_IMG    },
		{ "rule",   OP_RULE   },
		{ "markup", OP_MARKUP },
		{ "text",   OP_TEXT   }
	};

	assert(name != NULL);

	for (i = 0; i < sizeof a / sizeof *a; i++) {
		if (0 == strcmp(a[i].name, name)) {
			return a[i].op;
		}
	}

	fprintf(stderr, "^%s{}: unrecognised command\n", name);
	exit(1);
}

static void
print_modifiers(uint32_t mask)
{
	size_t i;

	static const char *a[] = {
		"Shift", "Lock", "Ctrl", "Alt",
		"Mod2", "Mod3", "Mod4", "Mod5",
		"Button1", "Button2", "Button3", "Button4", "Button5"
	};

	for (i = 0; mask; mask >>= 1, i++) {
		if (mask & 1) {
			printf(" %s", a[i]);
		}
	}
}

static xcb_screen_t *
screen_of_display(xcb_connection_t *xcb, int screen)
{
	xcb_screen_iterator_t it;

	assert(xcb != NULL);

	it = xcb_setup_roots_iterator(xcb_get_setup(xcb));
	for ( ; it.rem; --screen, xcb_screen_next(&it)) {
		if (screen == 0) {
			return it.data;
		}
	}

	return NULL;
}

static xcb_visualtype_t *
visual_of_screen(xcb_connection_t *xcb, xcb_screen_t *screen, xcb_visualid_t visual_id)
{
	xcb_depth_iterator_t dit;
	xcb_visualtype_iterator_t vit;

	assert(xcb != NULL);
	assert(screen != NULL);

	dit = xcb_screen_allowed_depths_iterator(screen);
	for ( ; dit.rem; xcb_depth_next(&dit)) {
		vit = xcb_depth_visuals_iterator(dit.data);
		for ( ; vit.rem; xcb_visualtype_next(&vit)) {
			if (vit.data->visual_id == visual_id) {
				return vit.data;
			}
		}
	}

	return NULL;
}

static xcb_window_t
win_create(xcb_connection_t *xcb, xcb_ewmh_connection_t *ewmh,
	xcb_screen_t *screen, int width, int height,
	const char *title)
{
	xcb_window_t win;

	assert(xcb != NULL);
	assert(ewmh != NULL);
	assert(screen != NULL);

	uint32_t mask;
	uint32_t valwin[1];

	/* TODO: could set window border colour */
	mask = XCB_CW_EVENT_MASK;
	valwin[0]
		= XCB_EVENT_MASK_KEY_PRESS
		| XCB_EVENT_MASK_EXPOSURE
		| XCB_EVENT_MASK_BUTTON_PRESS;

	win = xcb_generate_id(xcb);

	xcb_create_window(xcb,
		XCB_COPY_FROM_PARENT,
		win,
		screen->root,
		0, 0, width, height,
		0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT,
		screen->root_visual,
		mask, valwin);

	xcb_ewmh_set_wm_name        (ewmh, win, strlen(title), title);
	xcb_ewmh_set_wm_visible_name(ewmh, win, strlen(title), title);
	xcb_ewmh_set_wm_icon_name   (ewmh, win, strlen(title), title);

	xcb_ewmh_set_wm_window_type (ewmh, win, 1, &ewmh->_NET_WM_WINDOW_TYPE_DOCK);
	xcb_ewmh_set_wm_state       (ewmh, win, 1, &ewmh->_NET_WM_STATE_ABOVE);

	xcb_ewmh_set_wm_pid(ewmh, win, getpid());
	xcb_ewmh_set_wm_desktop(ewmh, win, 0xffffffff);

	/* TODO: _NET_WM_STRUT_PARTIAL and friends */

	xcb_map_window(xcb, win);

	return win;
}

static PangoColor
op_color(const char *s)
{
	PangoColor c;

	assert(s != NULL);

	/* TODO: disallow #rrrgggbbb or #rrrrggggbbbb */
	/* TODO: allow #rrggbbaa for alpha */

	/* CSS spec color names */
	if (!pango_color_parse(&c, s)) {
		perror("pango_color_parse");
	}

	return c;
}

static void
op_font(PangoFontDescription **desc, const char *s)
{
	assert(desc != NULL);
	assert(s != NULL);

	if (*desc != NULL) {
		pango_font_description_free(*desc);
	}

	*desc = pango_font_description_from_string(s);
}

static struct flex_item *
op_img(struct act *act, const char *file,
	double margin, double padding)
{
	struct flex_item *item;
	int width, height;
	cairo_surface_t *img;

	assert(act != NULL);
	assert(file != NULL);

	switch (ext(file)) {
	case FMT_PNG:
		img = cairo_image_surface_create_from_png(file);
		break;

/*
	case FMT_SVG:
		img = cairo_svg_surface_create(file, TODO);
		break;
*/

	default:
		fprintf(stderr, "%s: unsupported file extension\n", file);
		exit(1);
	}

	/* TODO: s/^~/$HOME/ */

	act->type = ACT_IMG;

	act->u.img.img = img;

	item = flex_item_new();

	width  = cairo_image_surface_get_width(img);
	height = cairo_image_surface_get_height(img);

	/* TODO: force min-height here? or leave to flexbox layout */

	flex_item_set_width(item,  width  + (padding * 2));
	flex_item_set_height(item, height + (padding * 2));

	return item;
}

static struct flex_item *
op_rule(struct act *act,
	PangoFontDescription *desc, PangoColor *fg,
	double margin, double padding)
{
	struct flex_item *item;
	int height;

	assert(act != NULL);
	assert(desc != NULL);
	assert(fg != NULL);

	act->type = ACT_RULE;

	act->u.rule.fg   = *fg;

	item = flex_item_new();

	{
		PangoContext *context;
		PangoLayout *layout;
		PangoFontMap *fontmap;
		PangoFontMetrics *metrics;
		int ascent, descent;

		/* XXX: error handling */
		fontmap = pango_cairo_font_map_get_default();
		context = pango_font_map_create_context(fontmap);
		layout = pango_layout_new(context);

		/* TODO: centralise with ACT_RULE */
		pango_context_set_base_gravity(context, PANGO_GRAVITY_SOUTH);
		pango_layout_set_single_paragraph_mode(layout, true);
		pango_layout_set_font_description(layout, desc);
		pango_layout_set_ellipsize(layout, act->u.text.e);

		/* TODO: centralise with ACT_RULE */
		/* TODO: default to current language tag */
		metrics = pango_context_get_metrics(context, desc, NULL);

		ascent  = pango_font_metrics_get_ascent(metrics)  / PANGO_SCALE;
		descent = pango_font_metrics_get_descent(metrics) / PANGO_SCALE;

		height = ascent + descent;

		/* TODO: destroy stuff */
		/* this layout is then discarded because the final width and height of the text
		 * depend on the flex layout. Here we're providing the ideal size. */

		g_object_unref(context);
		g_object_unref(fontmap);
	}

	flex_item_set_width(item,  height + (padding * 2));
	flex_item_set_height(item, height + (padding * 2));

	return item;
}

static struct flex_item *
op_text(struct act *act, const char *s,
	PangoEllipsizeMode e, PangoFontDescription *desc, PangoColor *fg,
	double margin, double padding,
	void (*f)(PangoLayout *, const char *, int))
{
	struct flex_item *item;
	int width, height;

	assert(act != NULL);
	assert(s != NULL);
	assert(desc != NULL);
	assert(fg != NULL);
	assert(f != NULL);

	act->type = ACT_TEXT;

	act->u.text.desc = pango_font_description_copy(desc);
	act->u.text.e    = e;
	act->u.text.fg   = *fg;
	act->u.text.f    = f;
	act->u.text.s    = strdup(s); /* XXX: because *p = '\0' is overwritten later */

	if (act->u.text.s == NULL) {
		perror("strdup");
		exit(1);
	}

	item = flex_item_new();

	/* TODO: force min-height here? or leave to flexbox layout */
	/* TODO: unless we're in ellipsis mode */

	{
		PangoContext *context;
		PangoLayout *layout;
		PangoFontMap *fontmap;

		/* XXX: error handling */
		fontmap = pango_cairo_font_map_get_default();
		context = pango_font_map_create_context(fontmap);
		layout = pango_layout_new(context);

		/* TODO: centralise with ACT_TEXT */
		pango_context_set_base_gravity(context, PANGO_GRAVITY_SOUTH);
		pango_layout_set_single_paragraph_mode(layout, true);
		pango_layout_set_font_description(layout, act->u.text.desc);
		pango_layout_set_ellipsize(layout, act->u.text.e);

		act->u.text.f(layout, act->u.text.s, -1);

		pango_layout_get_pixel_size(layout, &width, &height);

		/* TODO: destroy stuff */
		/* this layout is then discarded because the final width and height of the text
		 * depend on the flex layout. Here we're providing the ideal size. */

		g_object_unref(context);
		g_object_unref(fontmap);
	}

	flex_item_set_width(item,  width  + (padding * 2));
	flex_item_set_height(item, height + (padding * 2));

	return item;
}

static void
paint(cairo_t *cr, const struct act *b, size_t n)
{
	unsigned i;

	for (i = 0; i < n; i++) {
		const struct geom    *f = &b[i].f;
		const struct outline *m = &b[i].m;
		const struct outline *p = &b[i].p;

		(void) m;

/*
		cairo_set_source_rgba(cr, 0.4, 0.4, 1.0, 0.5);
		cairo_rectangle(cr, f->x - m->l, f->y - m->t, f->w + m->w, f->h + m->h);
		cairo_fill(cr);
*/

		cairo_set_source_rgba(cr, b[i].bg.red, b[i].bg.green, b[i].bg.blue, 1.0);
		cairo_rectangle(cr, f->x, f->y, f->w, f->h);
		cairo_fill(cr);

/*
		cairo_set_source_rgba(cr, 1.0, 0.4, 0.4, 0.5);
		cairo_rectangle(cr, f->x + p->l, f->y + p->t, f->w - p->w, f->h - p->h);
		cairo_fill(cr);
*/

		switch (b[i].type) {
		case ACT_IMG:
			assert(b[i].u.img.img != NULL);

			cairo_set_source_surface(cr, b[i].u.img.img, f->x + p->l, f->y + p->t);
			cairo_paint(cr);
			break;

		case ACT_RULE: {
			/* TODO: line style, dashed etc */
			/* TODO: automatic horizontal/vertical rule */

			cairo_set_source_rgba(cr, b[i].u.rule.fg.red, b[i].u.rule.fg.green, b[i].u.rule.fg.blue, 1.0);
			cairo_move_to(cr, f->x + p->l, f->y + p->t + ((f->h - p->h) / 2.0));
			cairo_rel_line_to(cr, f->w - p->w, 0.0);
			cairo_stroke(cr);
			break;
		}

		case ACT_TEXT: {
			PangoLayout *layout;
			PangoContext *context;

			assert(b[i].u.text.desc != NULL);
			assert(b[i].u.text.f != NULL);
			assert(b[i].u.text.s != NULL);

			layout = pango_cairo_create_layout(cr);

			/* pango_layout_set_width(layout, f->w * PANGO_SCALE); */
			pango_layout_set_height(layout, f->h * PANGO_SCALE);

			pango_layout_set_single_paragraph_mode(layout, true);

			context = pango_layout_get_context(layout);
			pango_context_set_base_gravity(context, PANGO_GRAVITY_SOUTH);

			pango_layout_set_font_description(layout, b[i].u.text.desc);

/*
			int baseline = pango_layout_get_baseline(b[i].u.text.layout);

			cairo_set_source_rgba(cr, b[i].u.text.fg.red * 0.2, b[i].u.text.fg.green * 0.2, b[i].u.text.fg.blue * 0.2, 0.8);
			cairo_move_to(cr, f->x + p->l, f->y + p->t + (double) baseline / PANGO_SCALE);
			cairo_rel_line_to(cr, f->w - p->w, 0.0);
			cairo_stroke(cr);
*/

			pango_layout_set_ellipsize(layout, b[i].u.text.e);

			b[i].u.text.f(layout, b[i].u.text.s, -1);

			cairo_move_to(cr, f->x + p->l, f->y + p->t);
			cairo_set_source_rgba(cr, b[i].u.text.fg.red, b[i].u.text.fg.green, b[i].u.text.fg.blue, 1.0);
			pango_cairo_show_layout(cr, layout);

			g_object_unref(layout);

			break;
		}

		default:
			assert(!"unreached");
			break;
		}
	}
}

static char *
parse_op(char *p, enum op_type *op, char **arg)
{
	assert(p != NULL);
	assert(op != NULL);
	assert(arg != NULL && *arg != NULL);

	/*
	 * TODO: otf feature for tnum
	 * TODO: push/pop for heirachical flexbox model
	 * container, item
	 */

	switch (*p) {
	case '\0':
		return NULL;

	case '\n':
		*p = '\0';
		*op = OP_TEXT;
		break;

	case '{':
	case '}':
		if (*arg < p) {
			*op = OP_TEXT;
			break;
		}

		*op = *p == '{' ? OP_OPEN : OP_CLOSE;

		p++;
		break;

	case '^':
		if (*arg < p) {
			*op = OP_TEXT;
			break;
		}

		p++;

		const char *tmp;

		tmp = p;
		p += strcspn(p, "{");
		if (*p == '\0') { fprintf(stderr, "syntax error\n"); exit(1); }
		*p = '\0';

		*op = op_name(tmp);

		p++;

		/* TODO: check arity, consider perhaps multiple arguments */
		*arg = p;
		p += strcspn(p, "}");
		if (*p == '\0') { fprintf(stderr, "syntax error\n"); exit(1); }
		*p = '\0';

		p++;
		break;

	case '\f':
	case '\t':
	case '\v':
		*p = ' ';
	default:
		p++;
		return parse_op(p, op, arg);
	}

	return p;
}

static void *
eval_main(void *opaque)
{
	struct eval_ctx *ectx = opaque;
	char buf[MAX_LINE_LEN];

	assert(ectx != NULL);

	ectx->n = 0;

	while (fgets(buf, sizeof buf, stdin) != NULL) {
		enum op_type op;
		char *arg;
		char *p;

		struct {
			double margin;
			double padding;
			PangoColor fg;
			PangoColor bg;
			flex_align align_self;
			float grow;
			float shrink;
			float basis;
			int order;
			const char *ca_name;
			PangoFontDescription *desc;
			PangoEllipsizeMode e;
		} state;

		buf[sizeof buf - 1] = 'x';

		if (buf[sizeof buf - 1] == '\0' && buf[sizeof buf - 2] != '\n') {
			fprintf(stderr, "buffer overflow\n");
			exit(1);
		}

		/* evaluator state persists between ops, reset for each line */
		state.margin  = 0;
		state.padding = 0;
		state.fg      = op_color("white");
		state.bg      = op_color("black");
		state.align_self = FLEX_ALIGN_AUTO;
		state.grow    = 0.0;
		state.shrink  = 0.0;
		state.basis   = NAN;
		state.order   = 0;
		state.ca_name = NULL;
		state.desc    = NULL;
		state.e       = PANGO_ELLIPSIZE_NONE;

		op_font(&state.desc, "Sans");

		flex_item_set_align_content(ectx->root, FLEX_ALIGN_CENTER);
		flex_item_set_align_items(ectx->root, FLEX_ALIGN_END);
		flex_item_set_direction(ectx->root, FLEX_DIRECTION_ROW);

		p = buf;

		while (arg = p, p = parse_op(p, &op, &arg), p != NULL) {
			struct flex_item *item;
			char tmp;

			if (op == OP_TEXT) {
				tmp = *p;
				*p = '\0';
			}

			switch (op) {
			case OP_OPEN:
				item = flex_item_new();
				flex_item_set_width(item,  flex_item_get_width(ectx->root));
				flex_item_set_height(item, flex_item_get_height(ectx->root));
				break;

			case OP_CLOSE:
				ectx->root = flex_item_parent(ectx->root);
				if (ectx->root == NULL) {
					fprintf(stderr, "syntax error: unbalanced '}'\n");
					exit(1);
				}
				continue;

			case OP_CA:        state.ca_name = arg;           continue;
			case OP_BG:        state.bg = op_color(arg);      continue;
			case OP_FG:        state.fg = op_color(arg);      continue;
			case OP_FONT:      op_font(&state.desc, arg);     continue;
			case OP_ELLIPSIZE: state.e = ellipsize_name(arg); continue;

			case OP_DIR:
				flex_item_set_direction(ectx->root, dir_name(arg));
				continue;

			case OP_WRAP:
				flex_item_set_wrap(ectx->root, wrap_name(arg));
				continue;

			case OP_JUSTIFY_CONTENT:
				flex_item_set_justify_content(ectx->root, justify_content_name(arg));
				continue;

			case OP_ALIGN_ITEMS:
				flex_item_set_align_items(ectx->root, align_name(arg));
				continue;

			case OP_ALIGN_SELF:
				state.align_self = align_name(arg);
				continue;

			case OP_SHRINK: state.shrink = flex_strtof(arg, 0, INFINITY); continue;
			case OP_ORDER:  state.order  = flex_strtoi(arg, 0, INT_MAX);  continue;
			case OP_GROW:   state.grow   = flex_strtof(arg, 0, INFINITY); continue;
			case OP_BASIS:  state.basis  = flex_strtof(arg, 0, INFINITY); continue; /* TODO: auto, etc */

			case OP_IMG:
				item = op_img(&ectx->b[ectx->n], arg, state.margin, state.padding);
				break;

			case OP_RULE:
				if (state.ca_name != NULL) {
					fprintf(stderr, "^rule{} is a non-clickable area\n");
					exit(1);
				}
				item = op_rule(&ectx->b[ectx->n], state.desc, &state.fg, state.margin, state.padding);
				if (state.grow == 0.0) {
					state.grow = 10.0; /* TODO: something sensible for OP_RULE */
				}
				break;

			/* pango markup: https://developer.gnome.org/pango/stable/PangoMarkupFormat.html */
			case OP_MARKUP:
				item = op_text(&ectx->b[ectx->n], arg,
					state.e, state.desc, &state.fg, state.margin, state.padding,
					pango_layout_set_markup);
				break;

			case OP_TEXT:
				item = op_text(&ectx->b[ectx->n], arg,
					state.e, state.desc, &state.fg, state.margin, state.padding,
					pango_layout_set_text);
				break;

			default:
				assert(!"unreached");
			}

			if (op != OP_OPEN) {
				ectx->b[ectx->n].bg      = state.bg;
				ectx->b[ectx->n].item    = item;
				ectx->b[ectx->n].ca_name = state.ca_name;
				ectx->n++;
			}

			if (!isnan(flex_item_get_width(item))) {
				flex_item_set_grow(item, state.grow);
				flex_item_set_shrink(item, state.shrink);
				state.grow    = 0.0;
				state.shrink  = 0.0;
				state.ca_name = NULL;
				/* TODO: reset align-self too */
			}

			flex_item_set_order(item, state.order);
			flex_item_set_basis(item, state.basis);
			flex_item_set_align_self(item, state.align_self);

			flex_item_set_margin_top(item, state.margin);
			flex_item_set_margin_left(item, state.margin);
			flex_item_set_margin_bottom(item, state.margin);
			flex_item_set_margin_right(item, state.margin);

			flex_item_set_padding_top(item, state.padding);
			flex_item_set_padding_left(item, state.padding);
			flex_item_set_padding_bottom(item, state.padding);
			flex_item_set_padding_right(item, state.padding);

			flex_item_add(ectx->root, item);

			state.order = 0;

			if (op == OP_TEXT) {
				*p = tmp;
			}

			if (op == OP_OPEN) {
				ectx->root = item;
			}
		}

		if (flex_item_parent(ectx->root) != NULL) {
			fprintf(stderr, "syntax error: unbalanced '{'\n");
			exit(1);
		}

		/* TODO: dispatch */

		pango_font_description_free(state.desc);
	}

	return NULL;
}

static void *
ui_main(void *opaque)
{
	struct ui_ctx *uctx = opaque;
	xcb_generic_event_t *e;

	assert(uctx != NULL);
	assert(uctx->b != NULL);
	assert(uctx->n != NULL);

	while (e = xcb_wait_for_event(uctx->xcb), e != NULL) {
		switch (e->response_type & ~0x80) {
		case XCB_KEY_PRESS: {
			xcb_key_press_event_t *press = (xcb_key_press_event_t *) e;
			fprintf(stderr, "key %d\n", press->detail);
			if (press->detail == 24) exit(1);
			break;
		}

		case XCB_EXPOSE: {
			xcb_expose_event_t *expose = (xcb_expose_event_t *) e;

			if (expose->count != 0) {
				break;
			}

			fprintf(stderr, "expose\n");

			paint(uctx->cr, uctx->b, *uctx->n);
			xcb_flush(uctx->xcb);
			break;
		}

		case XCB_BUTTON_PRESS: {
			xcb_button_press_event_t *press = (xcb_button_press_event_t *) e;
			unsigned i;

			for (i = 0; i < *uctx->n; i++) {
				if (uctx->b[i].ca_name == NULL) {
					continue;
				}

				if (!inside(&uctx->b[i].f, press->event_x, press->event_y)) {
					continue;
				}

				fprintf(stderr, "%s %d", uctx->b[i].ca_name, press->detail);
				print_modifiers(press->state);
				fprintf(stderr, "\n");
			}

			break;
		}

		default:
			fprintf(stderr, "unhandled event %d\n", e->response_type & ~0x80);
			break;
		}

		free(e);
	}

	return NULL;
}

int
main(int argc, char **argv)
{
	cairo_surface_t *surface;
	struct flex_item *root;
	int height, width;
	xcb_window_t win;
	xcb_connection_t *xcb;
	xcb_ewmh_connection_t ewmh;
	xcb_screen_t *screen;
	xcb_visualtype_t *visual;
	int screen_number;
	const char *of;
	enum format format;

	width   = 0;
	height  = 0;

	of = NULL;
	format = FMT_XCB;

	{
		int c;

		while (c = getopt(argc, argv, "w:h:o:"), c != -1) {
			switch (c) {
			case 'h': height = atoi(optarg); break; /* XXX: range */
			case 'w': width  = atoi(optarg); break;

			case 'o':
				of = optarg;
				format = ext(of);
				break;

			case '?':
			default:
				exit(1);
			}
		}

		argc -= optind;
		argv += optind;
	}

	if (height == 0) {
		height = 20; /* XXX: default from discovered height */
	}

	switch (format) {
	case FMT_PDF: surface = cairo_pdf_surface_create(of, width, height); break;
	case FMT_PNG: surface = cairo_image_surface_create(
	                        	CAIRO_FORMAT_ARGB32, width, height);     break;
	case FMT_SVG: surface = cairo_svg_surface_create(of, width, height); break;

	case FMT_XCB:
		xcb = xcb_connect(NULL, &screen_number);

		xcb_intern_atom_cookie_t *cookie = xcb_ewmh_init_atoms(xcb, &ewmh);
		xcb_ewmh_init_atoms_replies(&ewmh, cookie, NULL);

		screen = screen_of_display(xcb, screen_number);
		visual = visual_of_screen(xcb, screen, screen->root_visual);

		if (width == 0) {
			width  = screen->width_in_pixels;
		}

		/* TODO: title */
		win = win_create(xcb, &ewmh, screen, width, height, "hello");

		surface = cairo_xcb_surface_create(xcb, win, visual, width, height);
		cairo_xcb_surface_set_size(surface, width, height);
	}

	root = flex_item_new();

	/* TODO: would be re-set on xcb resize event */
	flex_item_set_width(root, width);
	flex_item_set_height(root, height);

	struct act b[50];
	struct eval_ctx ectx = { b, 0, root };

	{
		int e;

		pthread_t eval_tid;
		e = pthread_create(&eval_tid, NULL, eval_main, &ectx);
		if (e != 0) {
			errno = e;
			perror("pthread_create");
			exit(1);
		}

		e = pthread_join(eval_tid, NULL);
		if (e != 0) {
			errno = e;
			perror("pthread_join");
			exit(1);
		}
	}

/* XXX: the laying out needs to be re-done on an xcb resize event
we only need to layout when painting, and expose is also one of those events
so maybe layout is owned by the painting */
/*
- main thread: layout, ownership of root
- eval thread: populate items
- ui thread: events, paint
still don't know how to wake up ui thread to re-draw. generate fake event?
*/

	flex_layout(root);

	for (unsigned i = 0; i < ectx.n; i++) {
		ectx.b[i].f = flex_item_get_frame(ectx.b[i].item);
		ectx.b[i].m = flex_item_get_margin(ectx.b[i].item);
		ectx.b[i].p = flex_item_get_padding(ectx.b[i].item);

		ectx.b[i].item = NULL;
	}

	flex_item_free(root);

	switch (format) {
	case FMT_PDF:
	case FMT_PNG:
	case FMT_SVG: {
		cairo_t *cr;

		cr = cairo_create(surface);

		cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);

		paint(cr, ectx.b, ectx.n);
		if (format == FMT_PNG) {
			cairo_surface_write_to_png(surface, of);
		}
		cairo_destroy(cr);
		cairo_surface_destroy(surface);
		exit(0);
	}

	case FMT_XCB:
		break;
	}

	cairo_t *cr;

	cr = cairo_create(surface);

	cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);

	struct ui_ctx uctx = { xcb, cr, ectx.b, &ectx.n };

	{
		int e;

		pthread_t ui_tid;
		e = pthread_create(&ui_tid, NULL, ui_main, &uctx);
		if (e != 0) {
			errno = e;
			perror("pthread_create");
			exit(1);
		}

		e = pthread_join(ui_tid, NULL);
		if (e != 0) {
			errno = e;
			perror("pthread_join");
			exit(1);
		}
	}

	cairo_surface_destroy(surface);
	xcb_disconnect(xcb);

	return 0;
}

