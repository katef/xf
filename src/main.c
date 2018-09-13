/*
 * Copyright 2018 Katherine Flavel
 *
 * See LICENCE for the full copyright terms.
 */

#define _XOPEN_SOURCE 500

#include <sys/types.h>

#include <assert.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <math.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include <cairo.h>
#include <cairo-xlib.h>

#include <pango/pangocairo.h>

#include <flex.h>

/* TODO: moveto, drawing lines, images etc */
enum act_type {
	ACT_TEXT,
	ACT_RULE
};

struct act {
	enum act_type type;

	struct flex_item *item;

	union {
		struct act_rule {
			PangoColor fg;
			PangoColor bg;
		} hr;

		struct act_text {
			PangoColor fg;
			PangoColor bg;
			PangoLayout *layout; /* copy, needs destroying */
		} text;
	} u;
};

Drawable
create_win(Display *display, int screen, int width, int height)
{
	Drawable win;
	Atom type;
	unsigned int desktop;
	pid_t cur_pid;
	static char host_name[HOST_NAME_MAX];
	char *hn = host_name;
	XTextProperty txt_prop;

	assert(display != NULL);

	/* TODO: switch to xcb instead */

	win = XCreateSimpleWindow(display,
		DefaultRootWindow(display),
		0, 0, width, height, 0, 0, 0);

	type = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DOCK", False);
	XChangeProperty(display, win,
		XInternAtom(display, "_NET_WM_WINDOW_TYPE", False),
		XInternAtom(display, "ATOM", False),
		32, PropModeReplace,
		(unsigned char *) &type, 1);

	type = XInternAtom(display, "_NET_WM_STATE_ABOVE", False);
	XChangeProperty(display, win,
		XInternAtom(display, "_NET_WM_STATE", False),
		XInternAtom(display, "ATOM", False),
		32, PropModeReplace,
		(unsigned char *) &type, 1);

	type = XInternAtom(display, "_NET_WM_STATE_STICKY", False);
	XChangeProperty(display, win,
		XInternAtom(display, "_NET_WM_STATE", False),
		XInternAtom(display, "ATOM", False),
		32, PropModeAppend,
		(unsigned char *) &type, 1);

	desktop = 0xffffffff;
	XChangeProperty(display, win,
		XInternAtom(display, "_NET_WM_DESKTOP", False),
		XInternAtom(display, "CARDINAL", False),
		32, PropModeReplace,
		(unsigned char *) &desktop, 1);

	cur_pid = getpid();

	gethostname(host_name, HOST_NAME_MAX);

	XStringListToTextProperty(&hn, 1, &txt_prop);
	XSetWMClientMachine(display, win, &txt_prop);
	XFree(txt_prop.value);

	XChangeProperty(display, win,
		XInternAtom(display, "_NET_WM_PID", False),
		XInternAtom(display, "CARDINAL", False),
		32, PropModeReplace,
		(unsigned char *) &cur_pid, 1);

	return win;
}

void
close_surface(cairo_surface_t *sfc)
{
	Display *dsp = cairo_xlib_surface_get_display(sfc);

	cairo_surface_destroy(sfc);
	XCloseDisplay(dsp);
}

static PangoColor
op_color(const char *s)
{
	PangoColor c;

	assert(s != NULL);

	/* CSS spec color names */
	if (!pango_color_parse(&c, s)) {
		perror("pango_color_parse");
	}

	return c;
}

static void
op_font(cairo_t *cr, PangoLayout *layout, PangoFontDescription **desc, const char *s)
{
	assert(cr != NULL);
	assert(layout != NULL);
	assert(desc != NULL);
	assert(s != NULL);

	if (*desc != NULL) {
		pango_font_description_free(*desc);
	}

	*desc = pango_font_description_from_string(s);

	pango_layout_set_font_description(layout, *desc);
}

static struct flex_item *
op_rule(struct act *act,
	PangoColor *fg, PangoColor *bg,
	PangoLayout *layout,
	double margin, double padding)
{
	struct flex_item *item;
	int height;

	assert(act != NULL);
	assert(fg != NULL);
	assert(bg != NULL);
	assert(layout != NULL);

	act->type = ACT_RULE;

	act->u.hr.fg = *fg;
	act->u.hr.bg = *bg;

	item = flex_item_new();

	/* XXX: i don't know why this doesn't work; ascent and descent come out at 1 */
	if (0) {
		const PangoFontDescription *desc;
		PangoFontMetrics *metrics;
		PangoContext *context;
		int ascent, descent;

		context = pango_layout_get_context(layout);

		desc = pango_layout_get_font_description(layout);

		/* TODO: default to current language tag */
		metrics = pango_context_get_metrics(context, desc, NULL);

		ascent  = pango_font_metrics_get_ascent(metrics)  / PANGO_SCALE;
		descent = pango_font_metrics_get_descent(metrics) / PANGO_SCALE;
		fprintf(stderr, "%d/%d\n", ascent, descent);

		height = ascent + descent;
	} else {
		PangoRectangle logical_rect;

		pango_layout_set_text(layout, "M", 1); /* XXX: hacky */
		pango_layout_get_pixel_extents(layout, NULL, &logical_rect);

		height = logical_rect.height;
	}

	flex_item_set_width(item,  height + (padding * 2));
	flex_item_set_height(item, height + (padding * 2));

	return item;
}

static struct flex_item *
op_text(struct act *act, PangoLayout *layout, const char *s,
	PangoColor *fg, PangoColor *bg,
	double margin, double padding,
	void (*f)(PangoLayout *, const char *, int))
{
	struct flex_item *item;
	int width, height;

	assert(act != NULL);
	assert(fg != NULL);
	assert(bg != NULL);
	assert(layout != NULL);
	assert(s != NULL);
	assert(f != NULL);

	f(layout, s, -1);

	act->type = ACT_TEXT;

	act->u.text.fg = *fg;
	act->u.text.bg = *bg;
	act->u.text.layout = pango_layout_copy(layout);

	item = flex_item_new();

	pango_layout_get_pixel_size(act->u.text.layout, &width, &height);

	/* TODO: force min-height here? or leave to flexbox layout */
	/* TODO: unless we're in ellipsis mode */

	flex_item_set_width(item,  width  + (padding * 2));
	flex_item_set_height(item, height + (padding * 2));

	return item;
}

static void
act_rule(cairo_t *cr, struct flex_item *item, const struct act_rule *hr)
{
	double x, y, w, h;
	double ml, mr, mt, mb, mw, mh;
	double pl, pr, pt, pb, pw, ph;

	assert(cr != NULL);
	assert(item != NULL);
	assert(hr != NULL);

	/* these contain the padding but not the margins */
	x = flex_item_get_frame_x(item);
	y = flex_item_get_frame_y(item);
	w = flex_item_get_frame_width(item);
	h = flex_item_get_frame_height(item);

	ml = flex_item_get_margin_left(item);
	mr = flex_item_get_margin_right(item);
	mt = flex_item_get_margin_top(item);
	mb = flex_item_get_margin_bottom(item);
	mw = ml + mr;
	mh = mt + mb;

	pl = flex_item_get_padding_left(item);
	pr = flex_item_get_padding_right(item);
	pt = flex_item_get_padding_top(item);
	pb = flex_item_get_padding_bottom(item);
	pw = pl + pr;
	ph = pt + pb;

/*
	cairo_set_source_rgba(cr, 0.4, 0.4, 0.4, 0.5);
	cairo_rectangle(cr, x - ml, y - mt, w + mw, h + mh);
	cairo_fill(cr);
*/

	cairo_set_source_rgba(cr, hr->bg.red, hr->bg.green, hr->bg.blue, 1.0);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);

/*
	cairo_set_source_rgba(cr, 0.0, 0.4, 0.4, 0.5);
	cairo_rectangle(cr, x + pl, y + pt, w - pw, h - ph);
	cairo_fill(cr);
*/

	/* TODO: line style, dashed etc */
	/* TODO: automatic horizontal/vertical rule */

	cairo_set_source_rgba(cr, hr->fg.red, hr->fg.green, hr->fg.blue, 1.0);
	cairo_move_to(cr, x + pl, y + pt + ((h - ph) / 2.0));
	cairo_rel_line_to(cr, w - pw, 0.0);
	cairo_stroke(cr);
}

static void
act_text(cairo_t *cr, struct flex_item *item, const struct act_text *text)
{
	double x, y, w, h;
	double ml, mr, mt, mb, mw, mh;
	double pl, pr, pt, pb, pw, ph;

	assert(cr != NULL);
	assert(item != NULL);
	assert(text != NULL);

	/* these contain the padding but not the margins */
	x = flex_item_get_frame_x(item);
	y = flex_item_get_frame_y(item);
	w = flex_item_get_frame_width(item);
	h = flex_item_get_frame_height(item);

	ml = flex_item_get_margin_left(item);
	mr = flex_item_get_margin_right(item);
	mt = flex_item_get_margin_top(item);
	mb = flex_item_get_margin_bottom(item);
	mw = ml + mr;
	mh = mt + mb;

	pl = flex_item_get_padding_left(item);
	pr = flex_item_get_padding_right(item);
	pt = flex_item_get_padding_top(item);
	pb = flex_item_get_padding_bottom(item);
	pw = pl + pr;
	ph = pt + pb;

/*
	cairo_set_source_rgba(cr, 0.4, 0.4, 0.4, 0.5);
	cairo_rectangle(cr, x - ml, y - mt, w + mw, h + mh);
	cairo_fill(cr);
*/

	cairo_set_source_rgba(cr, text->bg.red, text->bg.green, text->bg.blue, 1.0);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);

/*
	cairo_set_source_rgba(cr, 0.0, 0.4, 0.4, 0.5);
	cairo_rectangle(cr, x + pl, y + pt, w - pw, h - ph);
	cairo_fill(cr);
*/

/*
	int baseline = pango_layout_get_baseline(text->layout);

	cairo_set_source_rgba(cr, text->fg.red * 0.2, text->fg.green * 0.2, text->fg.blue * 0.2, 0.8);
	cairo_move_to(cr, x + pl, y + pt + (double) baseline / PANGO_SCALE);
	cairo_rel_line_to(cr, w - pw, 0.0);
	cairo_stroke(cr);
*/

	cairo_move_to(cr, x + pl, y + pt);

	cairo_set_source_rgba(cr, text->fg.red, text->fg.green, text->fg.blue, 1.0);

	pango_cairo_show_layout(cr, text->layout);
}

int
main(int argc, char **argv)
{
	cairo_surface_t *sfc;
	cairo_t *cr;
	PangoLayout *layout;
	PangoFontDescription *desc;
	struct act b[50];
	unsigned n;
	struct flex_item *root;
	int height, width;
	double margin, padding;
	Display *display;
	Drawable win;
	int screen;

	margin  = 0;
	padding = 0;

	display = XOpenDisplay(NULL);
	if (display == NULL) {
		exit(1);
	}

	screen = DefaultScreen(display);

	width  = DisplayWidth(display, screen);
	height = 20;

	win = create_win(display, screen, width, height);

	XSelectInput(display, win, ButtonPressMask | KeyPressMask);
	XMapWindow(display, win);

	sfc = cairo_xlib_surface_create(display, win, DefaultVisual(display, screen), width, height);
	cairo_xlib_surface_set_size(sfc, width, height);

	cr = cairo_create(sfc);

	cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);

	layout = pango_cairo_create_layout(cr);
	desc = NULL;

	op_font(cr, layout, &desc, "Sans");

	/* pango_layout_set_width(layout, width * PANGO_SCALE); */
	pango_layout_set_height(layout, height * PANGO_SCALE);

	pango_layout_set_single_paragraph_mode(layout, true);

	PangoContext *pctx = pango_layout_get_context(layout);
	pango_context_set_base_gravity(pctx, PANGO_GRAVITY_SOUTH);

	root = flex_item_new();

	flex_item_set_wrap(root, FLEX_WRAP_WRAP);

	flex_item_set_width(root, width);
	flex_item_set_height(root, height);

	/* flex_item_set_justify_content(root, FLEX_ALIGN_SPACE_BETWEEN); */
	flex_item_set_align_content(root, FLEX_ALIGN_CENTER);
	flex_item_set_align_items(root, FLEX_ALIGN_END);
	flex_item_set_direction(root, FLEX_DIRECTION_ROW);

	n = 0;

	{
		enum op_type { OP_BG, OP_FG, OP_FONT, OP_GROW, OP_RULE, OP_MARKUP, OP_TEXT };
		PangoColor fg, bg;
		double grow;
		unsigned i;

		fg = op_color("white");
		bg = op_color("black");
		grow = 0.0;

		/*
		 * TODO: otf feature for tnum
		 * TODO: push/pop for heirachical flexbox model
		 * container, item
		 */
		struct {
			enum op_type type;
			const char *s;
		} a[] = {
			{ OP_FONT,   "Sans 12" },

			{ OP_BG,     "white" },
			{ OP_FG,     "black" },
			{ OP_TEXT,   " 1 " },

			{ OP_BG,     "black" },
			{ OP_FG,     "gray" },
			{ OP_TEXT,   " 2 " },

			{ OP_BG,     "black" },
			{ OP_FG,     "gray" },
			{ OP_TEXT,   " 3 " },

			{ OP_BG,     "black" },
			{ OP_FG,     "gray" },
			{ OP_TEXT,   " 4 " },

			{ OP_BG,     "black" },
			{ OP_FG,     "gray" },
			{ OP_TEXT,   " 5 " },

			{ OP_BG,     "black" },
			{ OP_FG,     "gray" },
			{ OP_TEXT,   " 6 " },

			{ OP_BG,     "black" },
			{ OP_FG,     "gray" },
			{ OP_TEXT,   " 7 " },

			{ OP_BG,     "black" },
			{ OP_FG,     "gray" },
			{ OP_TEXT,   " 8 " },

			{ OP_BG,     "black" },
			{ OP_FG,     "gray" },
			{ OP_TEXT,   " 9 " },

			{ OP_BG,     "black" },
			{ OP_FG,     "white" },
			{ OP_TEXT,   "10/12 hello.c " },

			{ OP_GROW,   "2.0"  },
			{ OP_RULE,   NULL   },
			{ OP_MARKUP, "x" },
			{ OP_GROW,   "1.0"  },
			{ OP_RULE,   NULL   },
			{ OP_MARKUP, "xyz" },
			{ OP_MARKUP, " 17:25 " },
		};

		for (i = 0; i < sizeof a / sizeof *a; i++) {
			struct flex_item *item;

			switch (a[i].type) {
			case OP_BG:     bg = op_color(a[i].s);              continue;
			case OP_FG:     fg = op_color(a[i].s);              continue;
			case OP_FONT:   op_font(cr, layout, &desc, a[i].s); continue;

			case OP_GROW:
				grow = strtod(a[i].s, NULL); /* XXX */
				continue;

			case OP_RULE:
				item = op_rule(&b[n], &fg, &bg, layout, margin, padding);
				if (grow == 0.0) {
					grow = 10; /* TODO: something sensible for OP_RULE */
				}
				break;

			/* pango markup: https://developer.gnome.org/pango/stable/PangoMarkupFormat.html */
			case OP_MARKUP:
				item = op_text(&b[n], layout, a[i].s, &fg, &bg, margin, padding, pango_layout_set_markup);
				break;

			case OP_TEXT:
				item = op_text(&b[n], layout, a[i].s, &fg, &bg, margin, padding, pango_layout_set_text);
				break;

			default:
				assert(!"unreached");
			}

			if (!isnan(flex_item_get_width(item))) {
				flex_item_set_grow(item, grow);
				grow = 0.0;
			}

			flex_item_set_margin_top(item, margin);
			flex_item_set_margin_left(item, margin);
			flex_item_set_margin_bottom(item, margin);
			flex_item_set_margin_right(item, margin);

			flex_item_set_padding_top(item, padding);
			flex_item_set_padding_left(item, padding);
			flex_item_set_padding_bottom(item, padding);
			flex_item_set_padding_right(item, padding);

			b[n].item = item;
			flex_item_add(root, b[n].item);
			n++;
		}
	}

	pango_font_description_free(desc);
	g_object_unref(layout);

	{
		unsigned i;
		double w, h;

		flex_layout(root);

		w = flex_item_get_width(root);
		h = flex_item_get_height(root);

		/* TODO: bg */
		cairo_set_source_rgba(cr, 0.2, 0.3, 0.4, 1.0);
		cairo_rectangle(cr, 0, 0, w, h);
		cairo_fill(cr);

		for (i = 0; i < n; i++) {
			switch (b[i].type) {
			case ACT_RULE:
				act_rule(cr, b[i].item, &b[i].u.hr);
				break;

			case ACT_TEXT:
				act_text(cr, b[i].item, &b[i].u.text);
				break;

			default:
				assert(!"unreached");
				break;
			}
		}
	}

	flex_item_free(root);
	cairo_destroy(cr);

	for (;;) {
		char keybuf[8];
		KeySym key;
		XEvent e;

		XNextEvent(cairo_xlib_surface_get_display(sfc), &e);

		switch (e.type) {
		case ButtonPress:
			fprintf(stderr, "button %d\n", e.xbutton.button);
			continue;

		case KeyPress:
			XLookupString(&e.xkey, keybuf, sizeof(keybuf), &key, NULL);
			fprintf(stderr, "key %lu\n", key);
			if (key == 113) exit(1);
			continue;

		default:
			fprintf(stderr, "unhandled XEevent %d\n", e.type);
			continue;
		}
	}

	close_surface(sfc);

	return 0;
}

