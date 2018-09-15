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
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <math.h>

#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>

#include <cairo.h>
#include <cairo-xcb.h>

#include <pango/pangocairo.h>

#include <flex.h>

#define MAX_LINE_LEN 8192

enum op_type {
	OP_BG,
	OP_FG,
	OP_FONT,
	OP_GROW,
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

struct act {
	enum act_type type;

	struct flex_item *item;

	union {
		struct act_img {
			PangoColor bg;
			cairo_surface_t *img;
		} img;

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

static enum op_type
op_name(const char *cmd)
{
	size_t i;

	static const struct {
		const char *cmd;
		enum op_type op;
	} a[] = {
		{ "bg",     OP_BG     },
		{ "fg",     OP_FG     },
		{ "font",   OP_FONT   },
		{ "grow",   OP_GROW   },
		{ "img",    OP_IMG    },
		{ "rule",   OP_RULE   },
		{ "markup", OP_MARKUP },
		{ "text",   OP_TEXT   }
	};

	for (i = 0; i < sizeof a / sizeof *a; i++) {
		if (0 == strcmp(a[i].cmd, cmd)) {
			return a[i].op;
		}
	}

	fprintf(stderr, "unrecognised command ^%s\n", cmd);
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
			printf(a[i]);
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
op_img(struct act *act, const char *file,
	PangoColor *bg,
	double margin, double padding)
{
	struct flex_item *item;
	int width, height;
	cairo_surface_t *img;
	const char *p;

	assert(act != NULL);
	assert(bg != NULL);
	assert(file != NULL);

	p = strrchr(file, '.');
	if (p == NULL) {
		fprintf(stderr, "%s: file extension not found\n", file);
		exit(1);
	}

	p++;

	/* TODO: s/^~/$HOME/ */
	/* TODO: cairo_svg_surface_create(file, ...); */

#ifdef CAIRO_HAS_PNG_FUNCTIONS
	if (0 == strcasecmp(p, "png")) {
		img = cairo_image_surface_create_from_png(file);
	} else
#endif
	{
		fprintf(stderr, "%s: unsupported file extension\n", file);
		exit(1);
	}

	act->type = ACT_IMG;

	act->u.img.bg  = *bg;
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
act_img(cairo_t *cr, struct flex_item *item, const struct act_img *img)
{
	double x, y, w, h;
	double ml, mr, mt, mb, mw, mh;
	double pl, pr, pt, pb, pw, ph;

	assert(cr != NULL);
	assert(item != NULL);
	assert(img != NULL);

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

/*
	cairo_set_source_rgba(cr, img->bg.red, img->bg.green, img->bg.blue, 1.0);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
*/

/*
	cairo_set_source_rgba(cr, 0.0, 0.4, 0.4, 0.5);
	cairo_rectangle(cr, x + pl, y + pt, w - pw, h - ph);
	cairo_fill(cr);
*/

	cairo_set_source_surface(cr, img->img, x + pl, y + pt);

/* XXX */
	cairo_paint(cr);
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

static void
paint(cairo_t *cr, struct flex_item *root, struct act *b, size_t n)
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
		case ACT_IMG:
			act_img(cr, b[i].item, &b[i].u.img);
			break;

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

		/* TODO: open/close flex container */

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

static struct flex_item *
make_item(enum op_type op, const char *arg, struct act *act,
	cairo_t *cr, PangoLayout *layout,
	PangoColor *fg, PangoColor *bg, PangoFontDescription **desc,
	double *grow, double *margin, double *padding)
{
	struct flex_item *item;

	assert(arg != NULL);
	assert(act != NULL);
	assert(layout != NULL);
	assert(fg != NULL);
	assert(bg != NULL);
	assert(desc != NULL);
	assert(grow != NULL);
	assert(margin != NULL);
	assert(padding != NULL);

	switch (op) {
	case OP_BG:     *bg = op_color(arg);             return NULL;
	case OP_FG:     *fg = op_color(arg);             return NULL;
	case OP_FONT:   op_font(cr, layout, desc, arg); return NULL;

	case OP_GROW:
		*grow = strtod(arg, NULL); /* XXX */
		return NULL;

	case OP_IMG:
		item = op_img(act, arg, bg, *margin, *padding);
		break;

	case OP_RULE:
		item = op_rule(act, fg, bg, layout, *margin, *padding);
		if (*grow == 0.0) {
			*grow = 10.0; /* TODO: something sensible for OP_RULE */
		}
		break;

	/* pango markup: https://developer.gnome.org/pango/stable/PangoMarkupFormat.html */
	case OP_MARKUP:
		item = op_text(act, layout, arg, fg, bg, *margin, *padding,
			pango_layout_set_markup);
		break;

	case OP_TEXT:
		item = op_text(act, layout, arg, fg, bg, *margin, *padding,
			pango_layout_set_text);
		break;

	default:
		assert(!"unreached");
	}

	if (!isnan(flex_item_get_width(item))) {
		flex_item_set_grow(item, *grow);
		*grow = 0.0;
	}

	flex_item_set_margin_top(item, *margin);
	flex_item_set_margin_left(item, *margin);
	flex_item_set_margin_bottom(item, *margin);
	flex_item_set_margin_right(item, *margin);

	flex_item_set_padding_top(item, *padding);
	flex_item_set_padding_left(item, *padding);
	flex_item_set_padding_bottom(item, *padding);
	flex_item_set_padding_right(item, *padding);

	return item;
}

int
main(int argc, char **argv)
{
	cairo_surface_t *surface;
	cairo_t *cr;
	PangoLayout *layout;
	PangoFontDescription *desc;
	struct act b[50];
	size_t n;
	struct flex_item *root;
	int height, width;
	double margin, padding;
	xcb_window_t win;
	xcb_connection_t *xcb;
	xcb_ewmh_connection_t ewmh;
	xcb_screen_t *screen;
	xcb_visualtype_t *visual;
	int screen_number;

	margin  = 0;
	padding = 0;

	xcb = xcb_connect(NULL, &screen_number);

	xcb_intern_atom_cookie_t *cookie = xcb_ewmh_init_atoms(xcb, &ewmh);
	xcb_ewmh_init_atoms_replies(&ewmh, cookie, NULL);

	screen = screen_of_display(xcb, screen_number);
	visual = visual_of_screen(xcb, screen, screen->root_visual);

	width  = screen->width_in_pixels;
	height = 20;

	/* TODO: title */
	win = win_create(xcb, &ewmh, screen, width, height, "hello");

	surface = cairo_xcb_surface_create(xcb, win, visual, width, height);
	cairo_xcb_surface_set_size(surface, width, height);

	cr = cairo_create(surface);

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

	char buf[MAX_LINE_LEN];

	PangoColor fg, bg;
	double grow;

	/* evalator state persists between ops (and over lines) */
	fg = op_color("white");
	bg = op_color("black");
	grow = 0.0;

	while (fgets(buf, sizeof buf, stdin) != NULL) {
		enum op_type op;
		char *arg;
		char *p;

		buf[sizeof buf - 1] = 'x';

		if (buf[sizeof buf - 1] == '\0' && buf[sizeof buf - 2] != '\n') {
			fprintf(stderr, "buffer overflow\n");
			exit(1);
		}

		arg = p = buf;

		while (p = parse_op(p, &op, &arg), p != NULL) {
			struct flex_item *item;
			char tmp;

			if (op == OP_TEXT) {
				tmp = *p;
				*p = '\0';
			}

			item = make_item(op, arg, &b[n],
				cr, layout,
				&fg, &bg, &desc,
				&grow, &margin, &padding);

			if (item != NULL) {
				b[n].item = item;
				flex_item_add(root, b[n].item);
				n++;
			}

			if (op == OP_TEXT) {
				*p = tmp;
			}

			arg = p;
		}
	}

	pango_font_description_free(desc);
	g_object_unref(layout);

	xcb_generic_event_t *e;

	while (e = xcb_wait_for_event(xcb), e != NULL) {
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

			paint(cr, root, b, n);
			xcb_flush(xcb);
			break;
		}

		case XCB_BUTTON_PRESS: {
			xcb_button_press_event_t *press = (xcb_button_press_event_t *) e;
			fprintf(stderr, "button %d", press->detail);
			print_modifiers(press->state);
			fprintf(stderr, "\n");
			break;
		}

		default:
			fprintf(stderr, "unhandled event %d\n", e->response_type & ~0x80);
			break;
		}

		free(e);
	}

	flex_item_free(root);
	cairo_surface_destroy(surface);
	xcb_disconnect(xcb);

	return 0;
}

