/*
 * Copyright © 2008 Kristian Høgsberg
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <pty.h>
#include <ctype.h>
#include <cairo.h>
#include <glib.h>

#include <X11/keysym.h>

#include "wayland-util.h"
#include "wayland-client.h"
#include "wayland-glib.h"

#include "window.h"

static int option_fullscreen;

#define MOD_SHIFT	0x01
#define MOD_ALT		0x02
#define MOD_CTRL	0x04

#define ATTRMASK_BOLD		0x01
#define ATTRMASK_UNDERLINE	0x02
#define ATTRMASK_BLINK		0x04
#define ATTRMASK_INVERSE	0x08

/* Buffer sizes */
#define MAX_RESPONSE		11
#define MAX_ESCAPE		64

/* Terminal modes */
#define MODE_SHOW_CURSOR	0x00000001
#define MODE_INVERSE		0x00000002
#define MODE_AUTOWRAP		0x00000004
#define MODE_AUTOREPEAT		0x00000008
#define MODE_LF_NEWLINE		0x00000010
#define MODE_IRM		0x00000020
#define MODE_DELETE_SENDS_DEL	0x00000040
#define MODE_ALT_SENDS_ESC	0x00000080

union utf8_char {
	unsigned char byte[4];
	uint32_t ch;
};

enum utf8_state {
	utf8state_start,
	utf8state_accept,
	utf8state_reject,
	utf8state_expect3,
	utf8state_expect2,
	utf8state_expect1
};

struct utf8_state_machine {
	enum utf8_state state;
	int len;
	union utf8_char s;
};

static void
init_state_machine(struct utf8_state_machine *machine)
{
	machine->state = utf8state_start;
	machine->len = 0;
	machine->s.ch = 0;
}

static enum utf8_state
utf8_next_char(struct utf8_state_machine *machine, char c)
{
	switch(machine->state) {
	case utf8state_start:
	case utf8state_accept:
	case utf8state_reject:
		machine->s.ch = 0;
		machine->len = 0;
		if(c == 0xC0 || c == 0xC1) {
			/* overlong encoding, reject */
			machine->state = utf8state_reject;
		} else if((c & 0x80) == 0) {
			/* single byte, accept */
			machine->s.byte[machine->len++] = c;
			machine->state = utf8state_accept;
		} else if((c & 0xC0) == 0x80) {
			/* parser out of sync, ignore byte */
			machine->state = utf8state_start;
		} else if((c & 0xE0) == 0xC0) {
			/* start of two byte sequence */
			machine->s.byte[machine->len++] = c;
			machine->state = utf8state_expect1;
		} else if((c & 0xF0) == 0xE0) {
			/* start of three byte sequence */
			machine->s.byte[machine->len++] = c;
			machine->state = utf8state_expect2;
		} else if((c & 0xF8) == 0xF0) {
			/* start of four byte sequence */
			machine->s.byte[machine->len++] = c;
			machine->state = utf8state_expect3;
		} else {
			/* overlong encoding, reject */
			machine->state = utf8state_reject;
		}
		break;
	case utf8state_expect3:
		machine->s.byte[machine->len++] = c;
		if((c & 0xC0) == 0x80) {
			/* all good, continue */
			machine->state = utf8state_expect2;
		} else {
			/* missing extra byte, reject */
			machine->state = utf8state_reject;
		}
		break;
	case utf8state_expect2:
		machine->s.byte[machine->len++] = c;
		if((c & 0xC0) == 0x80) {
			/* all good, continue */
			machine->state = utf8state_expect1;
		} else {
			/* missing extra byte, reject */
			machine->state = utf8state_reject;
		}
		break;
	case utf8state_expect1:
		machine->s.byte[machine->len++] = c;
		if((c & 0xC0) == 0x80) {
			/* all good, accept */
			machine->state = utf8state_accept;
		} else {
			/* missing extra byte, reject */
			machine->state = utf8state_reject;
		}
		break;
	default:
		machine->state = utf8state_reject;
		break;
	}
	
	return machine->state;
}

struct char_sub {
	union utf8_char match;
	union utf8_char replace;
};
/* Set last char_sub match to NULL char */
typedef struct char_sub *character_set;

struct char_sub CS_US[] = {
	{{{0, }}, {{0, }}}
};
static struct char_sub CS_UK[] = {
	{{{'#', 0, }}, {{0xC2, 0xA3, 0, }}},
	{{{0, }}, {{0, }}}
};
static struct char_sub CS_SPECIAL[] = {
	{{{'`', 0, }}, {{0xE2, 0x99, 0xA6, 0}}}, /* diamond */
	{{{'a', 0, }}, {{0xE2, 0x96, 0x92, 0}}}, /* 50% cell */
	{{{'b', 0, }}, {{0xE2, 0x90, 0x89, 0}}}, /* HT */
	{{{'c', 0, }}, {{0xE2, 0x90, 0x8C, 0}}}, /* FF */
	{{{'d', 0, }}, {{0xE2, 0x90, 0x8D, 0}}}, /* CR */
	{{{'e', 0, }}, {{0xE2, 0x90, 0x8A, 0}}}, /* LF */
	{{{'f', 0, }}, {{0xC2, 0xB0, 0, }}}, /* Degree */
	{{{'g', 0, }}, {{0xC2, 0xB1, 0, }}}, /* Plus/Minus */
	{{{'h', 0, }}, {{0xE2, 0x90, 0xA4, 0}}}, /* NL */
	{{{'i', 0, }}, {{0xE2, 0x90, 0x8B, 0}}}, /* VT */
	{{{'j', 0, }}, {{0xE2, 0x94, 0x98, 0}}}, /* CN_RB */
	{{{'k', 0, }}, {{0xE2, 0x94, 0x90, 0}}}, /* CN_RT */
	{{{'l', 0, }}, {{0xE2, 0x94, 0x8C, 0}}}, /* CN_LT */
	{{{'m', 0, }}, {{0xE2, 0x94, 0x94, 0}}}, /* CN_RB */
	{{{'n', 0, }}, {{0xE2, 0x94, 0xBC, 0}}}, /* CROSS */
	{{{'o', 0, }}, {{0xE2, 0x94, 0x80, 0}}}, /* H */
	{{{'p', 0, }}, {{0xE2, 0x94, 0x80, 0}}}, /* H */
	{{{'q', 0, }}, {{0xE2, 0x94, 0x80, 0}}}, /* H */
	{{{'r', 0, }}, {{0xE2, 0x94, 0x80, 0}}}, /* H */
	{{{'s', 0, }}, {{0xE2, 0x94, 0x80, 0}}}, /* H */
	{{{'t', 0, }}, {{0xE2, 0x94, 0x9C, 0}}}, /* TR */
	{{{'u', 0, }}, {{0xE2, 0x94, 0xA4, 0}}}, /* TL */
	{{{'v', 0, }}, {{0xE2, 0x94, 0xB4, 0}}}, /* TU */
	{{{'w', 0, }}, {{0xE2, 0x94, 0xAC, 0}}}, /* TD */
	{{{'x', 0, }}, {{0xE2, 0x94, 0x82, 0}}}, /* V */
	{{{'y', 0, }}, {{0xE2, 0x89, 0xA4, 0}}}, /* LE */
	{{{'z', 0, }}, {{0xE2, 0x89, 0xA5, 0}}}, /* GE */
	{{{'{', 0, }}, {{0xCF, 0x80, 0, }}}, /* PI */
	{{{'|', 0, }}, {{0xE2, 0x89, 0xA0, 0}}}, /* NEQ */
	{{{'}', 0, }}, {{0xC2, 0xA3, 0, }}}, /* POUND */
	{{{'~', 0, }}, {{0xE2, 0x8B, 0x85, 0}}}, /* DOT */
	{{{0, }}, {{0, }}}
};

static void
apply_char_set(character_set cs, union utf8_char *utf8)
{
	int i = 0;
	
	while (cs[i].match.byte[0]) {
		if ((*utf8).ch == cs[i].match.ch) {
			*utf8 = cs[i].replace;
			break;
		}
		i++;
	}
}

struct key_map {
	int sym;
	int num;
	char escape;
	char code;
};
/* Set last key_sub sym to NULL */
typedef struct key_map *keyboard_mode;

static struct key_map KM_NORMAL[] = {
	{XK_Left,  1, '[', 'D'},
	{XK_Right, 1, '[', 'C'},
	{XK_Up,    1, '[', 'A'},
	{XK_Down,  1, '[', 'B'},
	{XK_Home,  1, '[', 'H'},
	{XK_End,   1, '[', 'F'},
	{0, 0, 0, 0}
};
static struct key_map KM_APPLICATION[] = {
	{XK_Left,          1, 'O', 'D'},
	{XK_Right,         1, 'O', 'C'},
	{XK_Up,            1, 'O', 'A'},
	{XK_Down,          1, 'O', 'B'},
	{XK_Home,          1, 'O', 'H'},
	{XK_End,           1, 'O', 'F'},
	{XK_KP_Enter,      1, 'O', 'M'},
	{XK_KP_Multiply,   1, 'O', 'j'},
	{XK_KP_Add,        1, 'O', 'k'},
	{XK_KP_Separator,  1, 'O', 'l'},
	{XK_KP_Subtract,   1, 'O', 'm'},
	{XK_KP_Divide,     1, 'O', 'o'},
	{0, 0, 0, 0}
};

static int
function_key_response(char escape, int num, uint32_t modifiers,
		      char code, char *response)
{
	int mod_num = 0;
	int len;

	if (modifiers & WINDOW_MODIFIER_SHIFT) mod_num   |= 1;
	if (modifiers & WINDOW_MODIFIER_ALT) mod_num     |= 2;
	if (modifiers & WINDOW_MODIFIER_CONTROL) mod_num |= 4;

	if (mod_num != 0)
		len = snprintf(response, MAX_RESPONSE, "\e[%d;%d%c",
			       num, mod_num + 1, code);
	else if (code != '~')
		len = snprintf(response, MAX_RESPONSE, "\e%c%c",
			       escape, code);
	else
		len = snprintf(response, MAX_RESPONSE, "\e%c%d%c",
			       escape, num, code);

	if (len >= MAX_RESPONSE)	return MAX_RESPONSE - 1;
	else				return len;
}

/* returns the number of bytes written into response,
 * which must have room for MAX_RESPONSE bytes */
static int
apply_key_map(keyboard_mode mode, int sym, uint32_t modifiers, char *response)
{
	struct key_map map;
	int len = 0;
	int i = 0;
	
	while (mode[i].sym) {
		map = mode[i++];
		if (sym == map.sym) {
			len = function_key_response(map.escape, map.num,
						    modifiers, map.code,
						    response);
			break;
		}
	}
	
	return len;
}

struct terminal_color { double r, g, b, a; };
struct attr {
	unsigned char fg, bg;
	char a;        /* attributes format:
	                * 76543210
			*     ilub */
	char r;        /* reserved */
};
struct color_scheme {
	struct terminal_color palette[16];
	struct terminal_color border;
	struct attr default_attr;
};

static void
attr_init(struct attr *data_attr, struct attr attr, int n)
{
	int i;
	for (i = 0; i < n; i++) {
		data_attr[i] = attr;
	}
}

struct terminal {
	struct window *window;
	struct display *display;
	union utf8_char *data;
	char *tab_ruler;
	struct attr *data_attr;
	struct attr curr_attr;
	uint32_t mode;
	char origin_mode;
	char saved_origin_mode;
	struct attr saved_attr;
	union utf8_char last_char;
	int margin_top, margin_bottom;
	character_set cs, g0, g1;
	character_set saved_cs, saved_g0, saved_g1;
	keyboard_mode key_mode;
	int data_pitch, attr_pitch;  /* The width in bytes of a line */
	int width, height, start, row, column;
	int saved_row, saved_column;
	int fd, master;
	GIOChannel *channel;
	uint32_t modifiers;
	char escape[MAX_ESCAPE];
	int escape_length;
	int state;
	int qmark_flag;
	struct utf8_state_machine state_machine;
	int margin;
	int fullscreen;
	int focused;
	struct color_scheme *color_scheme;
	struct terminal_color color_table[256];
	cairo_font_extents_t extents;
	cairo_font_face_t *font_normal, *font_bold;
};

/* Create default tab stops, every 8 characters */
static void
terminal_init_tabs(struct terminal *terminal)
{
	int i = 0;
	
	while (i < terminal->width) {
		if (i % 8 == 0)
			terminal->tab_ruler[i] = 1;
		else
			terminal->tab_ruler[i] = 0;
		i++;
	}
}

static void
terminal_init(struct terminal *terminal)
{
	terminal->curr_attr = terminal->color_scheme->default_attr;
	terminal->origin_mode = 0;
	terminal->mode = MODE_SHOW_CURSOR |
			 MODE_AUTOREPEAT |
			 MODE_ALT_SENDS_ESC |
			 MODE_AUTOWRAP;

	terminal->row = 0;
	terminal->column = 0;

	terminal->g0 = CS_US;
	terminal->g1 = CS_US;
	terminal->cs = terminal->g0;
	terminal->key_mode = KM_NORMAL;

	terminal->saved_g0 = terminal->g0;
	terminal->saved_g1 = terminal->g1;
	terminal->saved_cs = terminal->cs;

	terminal->saved_attr = terminal->curr_attr;
	terminal->saved_origin_mode = terminal->origin_mode;
	terminal->saved_row = terminal->row;
	terminal->saved_column = terminal->column;

	if (terminal->tab_ruler != NULL) terminal_init_tabs(terminal);
}

static void
init_color_table(struct terminal *terminal)
{
	int c, r;
	struct terminal_color *color_table = terminal->color_table;

	for (c = 0; c < 256; c ++) {
		if (c < 16) {
			color_table[c] = terminal->color_scheme->palette[c];
		} else if (c < 232) {
			r = c - 16;
			color_table[c].b = ((double)(r % 6) / 6.0); r /= 6;
			color_table[c].g = ((double)(r % 6) / 6.0); r /= 6;
			color_table[c].r = ((double)(r % 6) / 6.0);
			color_table[c].a = 1.0;
		} else {
			r = (c - 232) * 10 + 8;
			color_table[c].r = ((double) r) / 256.0;
			color_table[c].g = color_table[c].r;
			color_table[c].b = color_table[c].r;
			color_table[c].a = 1.0;
		}
	}
}

static union utf8_char *
terminal_get_row(struct terminal *terminal, int row)
{
	int index;

	index = (row + terminal->start) % terminal->height;

	return &terminal->data[index * terminal->width];
}

static struct attr*
terminal_get_attr_row(struct terminal *terminal, int row) {
	int index;

	index = (row + terminal->start) % terminal->height;

	return &terminal->data_attr[index * terminal->width];
}

static struct attr
terminal_get_attr(struct terminal *terminal, int row, int col) {
	return terminal_get_attr_row(terminal, row)[col];
}

static void
terminal_scroll_buffer(struct terminal *terminal, int d)
{
	int i;

	d = d % (terminal->height + 1);
	terminal->start = (terminal->start + d) % terminal->height;
	if (terminal->start < 0) terminal->start = terminal->height + terminal->start;
	if(d < 0) {
		d = 0 - d;
		for(i = 0; i < d; i++) {
			memset(terminal_get_row(terminal, i), 0, terminal->data_pitch);
			attr_init(terminal_get_attr_row(terminal, i),
			    terminal->curr_attr, terminal->width);
		}
	} else {
		for(i = terminal->height - d; i < terminal->height; i++) {
			memset(terminal_get_row(terminal, i), 0, terminal->data_pitch);
			attr_init(terminal_get_attr_row(terminal, i),
			    terminal->curr_attr, terminal->width);
		}
	}
}

static void
terminal_scroll_window(struct terminal *terminal, int d)
{
	int i;
	int window_height;
	int from_row, to_row;
	
	// scrolling range is inclusive
	window_height = terminal->margin_bottom - terminal->margin_top + 1;
	d = d % (window_height + 1);
	if(d < 0) {
		d = 0 - d;
		to_row = terminal->margin_bottom;
		from_row = terminal->margin_bottom - d;
		
		for (i = 0; i < (window_height - d); i++) {
			memcpy(terminal_get_row(terminal, to_row - i),
			       terminal_get_row(terminal, from_row - i),
			       terminal->data_pitch);
			memcpy(terminal_get_attr_row(terminal, to_row - i),
			       terminal_get_attr_row(terminal, from_row - i),
			       terminal->attr_pitch);
		}
		for (i = terminal->margin_top; i < (terminal->margin_top + d); i++) {
			memset(terminal_get_row(terminal, i), 0, terminal->data_pitch);
			attr_init(terminal_get_attr_row(terminal, i),
				terminal->curr_attr, terminal->width);
		}
	} else {
		to_row = terminal->margin_top;
		from_row = terminal->margin_top + d;
		
		for (i = 0; i < (window_height - d); i++) {
			memcpy(terminal_get_row(terminal, to_row + i),
			       terminal_get_row(terminal, from_row + i),
			       terminal->data_pitch);
			memcpy(terminal_get_attr_row(terminal, to_row + i),
			       terminal_get_attr_row(terminal, from_row + i),
			       terminal->attr_pitch);
		}
		for (i = terminal->margin_bottom - d + 1; i <= terminal->margin_bottom; i++) {
			memset(terminal_get_row(terminal, i), 0, terminal->data_pitch);
			attr_init(terminal_get_attr_row(terminal, i),
				terminal->curr_attr, terminal->width);
		}
	}
}

static void
terminal_scroll(struct terminal *terminal, int d)
{
	if(terminal->margin_top == 0 && terminal->margin_bottom == terminal->height - 1)
		terminal_scroll_buffer(terminal, d);
	else
		terminal_scroll_window(terminal, d);
}

static void
terminal_shift_line(struct terminal *terminal, int d)
{
	union utf8_char *row;
	struct attr *attr_row, attr;
	
	row = terminal_get_row(terminal, terminal->row);
	attr_row = terminal_get_attr_row(terminal, terminal->row);

	if ((terminal->width + d) <= terminal->column)
		d = terminal->column + 1 - terminal->width;
	if ((terminal->column + d) >= terminal->width)
		d = terminal->width - terminal->column - 1;
	
	if (d < 0) {
		d = 0 - d;
		memmove(&row[terminal->column],
		        &row[terminal->column + d],
			(terminal->width - terminal->column - d) * sizeof(union utf8_char));
		attr = attr_row[terminal->width - 1];
		memmove(&attr_row[terminal->column], &attr_row[terminal->column + d],
		        (terminal->width - terminal->column - d) * sizeof(struct attr));
		memset(&row[terminal->width - d], 0, d * sizeof(union utf8_char));
		attr_init(&attr_row[terminal->width - d], terminal->curr_attr, d);
	} else {
		memmove(&row[terminal->column + d], &row[terminal->column],
			(terminal->width - terminal->column - d) * sizeof(union utf8_char));
		memmove(&attr_row[terminal->column + d], &attr_row[terminal->column],
			(terminal->width - terminal->column - d) * sizeof(struct attr));
		memset(&row[terminal->column], 0, d * sizeof(union utf8_char));
		attr_init(&attr_row[terminal->column], terminal->curr_attr, d);
	}
}

static void
terminal_resize(struct terminal *terminal, int width, int height)
{
	size_t size;
	union utf8_char *data;
	struct attr *data_attr;
	char *tab_ruler;
	int data_pitch, attr_pitch;
	int i, l, total_rows, start;
	struct rectangle rectangle;
	struct winsize ws;

	if (terminal->width == width && terminal->height == height)
		return;

	data_pitch = width * sizeof(union utf8_char);
	size = data_pitch * height;
	data = malloc(size);
	attr_pitch = width * sizeof(struct attr);
	data_attr = malloc(attr_pitch * height);
	tab_ruler = malloc(width);
	memset(data, 0, size);
	memset(tab_ruler, 0, width);
	attr_init(data_attr, terminal->curr_attr, width * height);
	if (terminal->data && terminal->data_attr) {
		if (width > terminal->width)
			l = terminal->width;
		else
			l = width;

		if (terminal->height > height) {
			total_rows = height;
			start = terminal->height - height;
		} else {
			total_rows = terminal->height;
			start = 0;
		}

		for (i = 0; i < total_rows; i++) {
			memcpy(&data[width * i],
			       terminal_get_row(terminal, i),
			       l * sizeof(union utf8_char));
			memcpy(&data_attr[width * i],
			       terminal_get_attr_row(terminal, i),
			       l * sizeof(struct attr));
		}

		free(terminal->data);
		free(terminal->data_attr);
		free(terminal->tab_ruler);
	}

	terminal->data_pitch = data_pitch;
	terminal->attr_pitch = attr_pitch;
	terminal->margin_bottom =
		height - (terminal->height - terminal->margin_bottom);
	terminal->width = width;
	terminal->height = height;
	terminal->data = data;
	terminal->data_attr = data_attr;
	terminal->tab_ruler = tab_ruler;
	terminal_init_tabs(terminal);

	if (terminal->row >= terminal->height)
		terminal->row = terminal->height - 1;
	if (terminal->column >= terminal->width)
		terminal->column = terminal->width - 1;
	terminal->start = 0;
	
	if (!terminal->fullscreen) {
		rectangle.width = terminal->width *
			terminal->extents.max_x_advance + 2 * terminal->margin;
		rectangle.height = terminal->height *
			terminal->extents.height + 2 * terminal->margin;
		window_set_child_size(terminal->window, &rectangle);
	}

	/* Update the window size */
	ws.ws_row = terminal->height;
	ws.ws_col = terminal->width;
	window_get_child_rectangle(terminal->window, &rectangle);
	ws.ws_xpixel = rectangle.width;
	ws.ws_ypixel = rectangle.height;
	ioctl(terminal->master, TIOCSWINSZ, &ws);
}

struct color_scheme DEFAULT_COLORS = {
	{
		{0,    0,    0,    1}, /* black */
		{0.66, 0,    0,    1}, /* red */
		{0  ,  0.66, 0,    1}, /* green */
		{0.66, 0.33, 0,    1}, /* orange (nicer than muddy yellow) */
		{0  ,  0  ,  0.66, 1}, /* blue */
		{0.66, 0  ,  0.66, 1}, /* magenta */
		{0,    0.66, 0.66, 1}, /* cyan */
		{0.66, 0.66, 0.66, 1}, /* light grey */
		{0.22, 0.33, 0.33, 1}, /* dark grey */
		{1,    0.33, 0.33, 1}, /* high red */
		{0.33, 1,    0.33, 1}, /* high green */
		{1,    1,    0.33, 1}, /* high yellow */
		{0.33, 0.33, 1,    1}, /* high blue */
		{1,    0.33, 1,    1}, /* high magenta */
		{0.33, 1,    1,    1}, /* high cyan */
		{1,    1,    1,    1}  /* white */
	},
	{0, 0, 0, 1},                  /* black border */
	{7, 0, 0, }                    /* bg:black (0), fg:light gray (7)  */
};

static void
terminal_draw_contents(struct terminal *terminal)
{
	struct rectangle rectangle;
	cairo_t *cr;
	cairo_font_extents_t extents;
	int top_margin, side_margin;
	int row, col;
	struct attr attr;
	union utf8_char *p_row;
	struct utf8_chars {
		union utf8_char c;
		char null;
	} toShow;
	int foreground, background, bold, underline, tmp;
	int text_x, text_y;
	cairo_surface_t *surface;
	double d;

	toShow.null = 0;

	window_get_child_rectangle(terminal->window, &rectangle);

	surface = display_create_surface(terminal->display, &rectangle);
	cr = cairo_create(surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr,
			      terminal->color_scheme->border.r,
			      terminal->color_scheme->border.g,
			      terminal->color_scheme->border.b,
			      terminal->color_scheme->border.a);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	cairo_set_font_face(cr, terminal->font_normal);
	cairo_set_font_size(cr, 14);

	cairo_font_extents(cr, &extents);
	side_margin = (rectangle.width - terminal->width * extents.max_x_advance) / 2;
	top_margin = (rectangle.height - terminal->height * extents.height) / 2;

	cairo_set_line_width(cr, 1.0);

	for (row = 0; row < terminal->height; row++) {
		p_row = terminal_get_row(terminal, row);
		for (col = 0; col < terminal->width; col++) {
			/* get the attributes for this character cell */
			attr = terminal_get_attr(terminal, row, col);
			if ((attr.a & ATTRMASK_INVERSE) ||
				((terminal->mode & MODE_SHOW_CURSOR) &&
				terminal->focused && terminal->row == row &&
				terminal->column == col))
			{
				foreground = attr.bg;
				background = attr.fg;
				if (attr.a & ATTRMASK_BOLD) {
					if (foreground <= 16) foreground |= 0x08;
					if (background <= 16) background &= 0x07;
				}
			} else {
				foreground = attr.fg;
				background = attr.bg;
			}
			if (terminal->mode & MODE_INVERSE) {
				tmp = foreground;
				foreground = background;
				background = tmp;
				if (attr.a & ATTRMASK_BOLD) {
					if (foreground <= 16) foreground |= 0x08;
					if (background <= 16) background &= 0x07;
				}
			}
			bold = attr.a & (ATTRMASK_BOLD | ATTRMASK_BLINK);
			underline = attr.a & ATTRMASK_UNDERLINE;

			/* paint the background */
			cairo_set_source_rgba(cr,
					      terminal->color_table[background].r,
					      terminal->color_table[background].g,
					      terminal->color_table[background].b,
					      terminal->color_table[background].a);
			cairo_move_to(cr, side_margin + (col * extents.max_x_advance),
			      top_margin + (row * extents.height));
			cairo_rel_line_to(cr, extents.max_x_advance, 0);
			cairo_rel_line_to(cr, 0, extents.height);
			cairo_rel_line_to(cr, -extents.max_x_advance, 0);
			cairo_close_path(cr);
			cairo_fill(cr);

			/* paint the foreground */
			if (bold)
				cairo_set_font_face(cr, terminal->font_bold);
			else
				cairo_set_font_face(cr, terminal->font_normal);
			cairo_set_source_rgba(cr,
					      terminal->color_table[foreground].r,
					      terminal->color_table[foreground].g,
					      terminal->color_table[foreground].b,
					      terminal->color_table[foreground].a);

			text_x = side_margin + col * extents.max_x_advance;
			text_y = top_margin + extents.ascent + row * extents.height;
			if (underline) {
				cairo_move_to(cr, text_x, (double)text_y + 1.5);
				cairo_line_to(cr, text_x + extents.max_x_advance, (double) text_y + 1.5);
				cairo_stroke(cr);
			}
			cairo_move_to(cr, text_x, text_y);
			
			toShow.c = p_row[col];
			cairo_show_text(cr, (char *) toShow.c.byte);
		}
	}

	if ((terminal->mode & MODE_SHOW_CURSOR) && !terminal->focused) {
		d = 0.5;

		cairo_set_line_width(cr, 1);
		cairo_move_to(cr, side_margin + terminal->column * extents.max_x_advance + d,
			      top_margin + terminal->row * extents.height + d);
		cairo_rel_line_to(cr, extents.max_x_advance - 2 * d, 0);
		cairo_rel_line_to(cr, 0, extents.height - 2 * d);
		cairo_rel_line_to(cr, -extents.max_x_advance + 2 * d, 0);
		cairo_close_path(cr);

		cairo_stroke(cr);
	}

	cairo_destroy(cr);

	window_copy_surface(terminal->window,
			    &rectangle,
			    surface);

	cairo_surface_destroy(surface);
}

static void
terminal_draw(struct terminal *terminal)
{
	struct rectangle rectangle;
	int32_t width, height;

	window_get_child_rectangle(terminal->window, &rectangle);

	width = (rectangle.width - 2 * terminal->margin) /
		(int32_t) terminal->extents.max_x_advance;
	height = (rectangle.height - 2 * terminal->margin) /
		(int32_t) terminal->extents.height;
	terminal_resize(terminal, width, height);

	window_draw(terminal->window);
	terminal_draw_contents(terminal);
	window_flush(terminal->window);
}

static void
redraw_handler(struct window *window, void *data)
{
	struct terminal *terminal = data;

	terminal_draw(terminal);
}

#define STATE_NORMAL 0
#define STATE_ESCAPE 1
#define STATE_ESCAPE_SPECIAL 2
#define STATE_ESCAPE_CSI  3

static void
terminal_data(struct terminal *terminal, const char *data, size_t length);

static void
handle_char(struct terminal *terminal, union utf8_char utf8);

static void
handle_sgr(struct terminal *terminal, int code);

static void
handle_term_parameter(struct terminal *terminal, int code, int sr)
{
	int i;

	if (terminal->qmark_flag) {
		switch(code) {
		case 1:  /* DECCKM */
			if (sr)	terminal->key_mode = KM_APPLICATION;
			else	terminal->key_mode = KM_NORMAL;
			break;
		case 2:  /* DECANM */
			/* No VT52 support yet */
			terminal->g0 = CS_US;
			terminal->g1 = CS_US;
			terminal->cs = terminal->g0;
			break;
		case 3:  /* DECCOLM */
			if (sr)
				terminal_resize(terminal, 132, 24);
			else
				terminal_resize(terminal, 80, 24);
			
			/* set columns, but also home cursor and clear screen */
			terminal->row = 0; terminal->column = 0;
			for (i = 0; i < terminal->height; i++) {
				memset(terminal_get_row(terminal, i),
				    0, terminal->data_pitch);
				attr_init(terminal_get_attr_row(terminal, i),
				    terminal->curr_attr, terminal->width);
			}
			break;
		case 5:  /* DECSCNM */
			if (sr)	terminal->mode |=  MODE_INVERSE;
			else	terminal->mode &= ~MODE_INVERSE;
			break;
		case 6:  /* DECOM */
			terminal->origin_mode = sr;
			if (terminal->origin_mode)
				terminal->row = terminal->margin_top;
			else
				terminal->row = 0;
			terminal->column = 0;
			break;
		case 7:  /* DECAWM */
			if (sr)	terminal->mode |=  MODE_AUTOWRAP;
			else	terminal->mode &= ~MODE_AUTOWRAP;
			break;
		case 8:  /* DECARM */
			if (sr)	terminal->mode |=  MODE_AUTOREPEAT;
			else	terminal->mode &= ~MODE_AUTOREPEAT;
			break;
		case 25:
			if (sr)	terminal->mode |=  MODE_SHOW_CURSOR;
			else	terminal->mode &= ~MODE_SHOW_CURSOR;
			break;
		case 1037:   /* deleteSendsDel */
			if (sr)	terminal->mode |=  MODE_DELETE_SENDS_DEL;
			else	terminal->mode &= ~MODE_DELETE_SENDS_DEL;
			break;
		case 1039:   /* altSendsEscape */
			if (sr)	terminal->mode |=  MODE_ALT_SENDS_ESC;
			else	terminal->mode &= ~MODE_ALT_SENDS_ESC;
			break;
		default:
			fprintf(stderr, "Unknown parameter: ?%d\n", code);
			break;
		}
	} else {
		switch(code) {
		case 4:  /* IRM */
			if (sr)	terminal->mode |=  MODE_IRM;
			else	terminal->mode &= ~MODE_IRM;
			break;
		case 20: /* LNM */
			if (sr)	terminal->mode |=  MODE_LF_NEWLINE;
			else	terminal->mode &= ~MODE_LF_NEWLINE;
			break;
		default:
			fprintf(stderr, "Unknown parameter: %d\n", code);
			break;
		}
	}
}

static void
handle_escape(struct terminal *terminal)
{
	union utf8_char *row;
	struct attr *attr_row;
	char *p;
	int i, count, x, y, top, bottom;
	int args[10], set[10] = { 0, };
	char response[MAX_RESPONSE] = {0, };

	terminal->escape[terminal->escape_length++] = '\0';
	i = 0;
	p = &terminal->escape[2];
	while ((isdigit(*p) || *p == ';') && i < 10) {
		if (*p == ';') {
			if (!set[i]) {
				args[i] = 0;
				set[i] = 1;
			}
			p++;
			i++;
		} else {
			args[i] = strtol(p, &p, 10);
			set[i] = 1;
		}
	}
	
	switch (*p) {
	case '@':    /* ICH */
		count = set[0] ? args[0] : 1;
		if (count == 0) count = 1;
		terminal_shift_line(terminal, count);
		break;
	case 'A':    /* CUU */
		count = set[0] ? args[0] : 1;
		if (count == 0) count = 1;
		if (terminal->row - count >= terminal->margin_top)
			terminal->row -= count;
		else
			terminal->row = terminal->margin_top;
		break;
	case 'B':    /* CUD */
		count = set[0] ? args[0] : 1;
		if (count == 0) count = 1;
		if (terminal->row + count <= terminal->margin_bottom)
			terminal->row += count;
		else
			terminal->row = terminal->margin_bottom;
		break;
	case 'C':    /* CUF */
		count = set[0] ? args[0] : 1;
		if (count == 0) count = 1;
		if ((terminal->column + count) < terminal->width)
			terminal->column += count;
		else
			terminal->column = terminal->width - 1;
		break;
	case 'D':    /* CUB */
		count = set[0] ? args[0] : 1;
		if (count == 0) count = 1;
		if ((terminal->column - count) >= 0)
			terminal->column -= count;
		else
			terminal->column = 0;
		break;
	case 'E':    /* CNL */
		count = set[0] ? args[0] : 1;
		if (terminal->row + count <= terminal->margin_bottom)
			terminal->row += count;
		else
			terminal->row = terminal->margin_bottom;
		terminal->column = 0;
		break;
	case 'F':    /* CPL */
		count = set[0] ? args[0] : 1;
		if (terminal->row - count >= terminal->margin_top)
			terminal->row -= count;
		else
			terminal->row = terminal->margin_top;
		terminal->column = 0;
		break;
	case 'G':    /* CHA */
		y = set[0] ? args[0] : 1;
		y = y <= 0 ? 1 : y > terminal->width ? terminal->width : y;
		
		terminal->column = y - 1;
		break;
	case 'f':    /* HVP */
	case 'H':    /* CUP */
		x = (set[1] ? args[1] : 1) - 1;
		x = x < 0 ? 0 :
		    (x >= terminal->width ? terminal->width - 1 : x);
		
		y = (set[0] ? args[0] : 1) - 1;
		if (terminal->origin_mode) {
			y += terminal->margin_top;
			y = y < terminal->margin_top ? terminal->margin_top :
			    (y > terminal->margin_bottom ? terminal->margin_bottom : y);
		} else {
			y = y < 0 ? 0 :
			    (y >= terminal->height ? terminal->height - 1 : y);
		}
		
		terminal->row = y;
		terminal->column = x;
		break;
	case 'I':    /* CHT */
		count = set[0] ? args[0] : 1;
		if (count == 0) count = 1;
		while (count > 0 && terminal->column < terminal->width) {
			if (terminal->tab_ruler[terminal->column]) count--;
			terminal->column++;
		}
		terminal->column--;
		break;
	case 'J':    /* ED */
		row = terminal_get_row(terminal, terminal->row);
		attr_row = terminal_get_attr_row(terminal, terminal->row);
		if (!set[0] || args[0] == 0 || args[0] > 2) {
			memset(&row[terminal->column],
			       0, (terminal->width - terminal->column) * sizeof(union utf8_char));
			attr_init(&attr_row[terminal->column],
			       terminal->curr_attr, terminal->width - terminal->column);
			for (i = terminal->row + 1; i < terminal->height; i++) {
				memset(terminal_get_row(terminal, i),
				    0, terminal->data_pitch);
				attr_init(terminal_get_attr_row(terminal, i),
				    terminal->curr_attr, terminal->width);
			}
		} else if (args[0] == 1) {
			memset(row, 0, (terminal->column+1) * sizeof(union utf8_char));
			attr_init(attr_row, terminal->curr_attr, terminal->column+1);
			for (i = 0; i < terminal->row; i++) {
				memset(terminal_get_row(terminal, i),
				    0, terminal->data_pitch);
				attr_init(terminal_get_attr_row(terminal, i),
				    terminal->curr_attr, terminal->width);
			}
		} else if (args[0] == 2) {
			for (i = 0; i < terminal->height; i++) {
				memset(terminal_get_row(terminal, i),
				    0, terminal->data_pitch);
				attr_init(terminal_get_attr_row(terminal, i),
				    terminal->curr_attr, terminal->width);
			}
		}
		break;
	case 'K':    /* EL */
		row = terminal_get_row(terminal, terminal->row);
		attr_row = terminal_get_attr_row(terminal, terminal->row);
		if (!set[0] || args[0] == 0 || args[0] > 2) {
			memset(&row[terminal->column], 0,
			    (terminal->width - terminal->column) * sizeof(union utf8_char));
			attr_init(&attr_row[terminal->column], terminal->curr_attr,
			    terminal->width - terminal->column);
		} else if (args[0] == 1) {
			memset(row, 0, (terminal->column+1) * sizeof(union utf8_char));
			attr_init(attr_row, terminal->curr_attr, terminal->column+1);
		} else if (args[0] == 2) {
			memset(row, 0, terminal->data_pitch);
			attr_init(attr_row, terminal->curr_attr, terminal->width);
		}
		break;
	case 'L':    /* IL */
		count = set[0] ? args[0] : 1;
		if (count == 0) count = 1;
		if (terminal->row >= terminal->margin_top &&
			terminal->row < terminal->margin_bottom)
		{
			top = terminal->margin_top;
			terminal->margin_top = terminal->row;
			terminal_scroll(terminal, 0 - count);
			terminal->margin_top = top;
		} else if (terminal->row == terminal->margin_bottom) {
			memset(terminal_get_row(terminal, terminal->row),
			       0, terminal->data_pitch);
			attr_init(terminal_get_attr_row(terminal, terminal->row),
				terminal->curr_attr, terminal->width);
		}
		break;
	case 'M':    /* DL */
		count = set[0] ? args[0] : 1;
		if (count == 0) count = 1;
		if (terminal->row >= terminal->margin_top &&
			terminal->row < terminal->margin_bottom)
		{
			top = terminal->margin_top;
			terminal->margin_top = terminal->row;
			terminal_scroll(terminal, count);
			terminal->margin_top = top;
		} else if (terminal->row == terminal->margin_bottom) {
			memset(terminal_get_row(terminal, terminal->row),
			       0, terminal->data_pitch);
		}
		break;
	case 'P':    /* DCH */
		count = set[0] ? args[0] : 1;
		if (count == 0) count = 1;
		terminal_shift_line(terminal, 0 - count);
		break;
	case 'S':    /* SU */
		terminal_scroll(terminal, set[0] ? args[0] : 1);
		break;
	case 'T':    /* SD */
		terminal_scroll(terminal, 0 - (set[0] ? args[0] : 1));
		break;
	case 'X':    /* ECH */
		count = set[0] ? args[0] : 1;
		if (count == 0) count = 1;
		if ((terminal->column + count) > terminal->width)
			count = terminal->width - terminal->column;
		row = terminal_get_row(terminal, terminal->row);
		attr_row = terminal_get_attr_row(terminal, terminal->row);
		memset(&row[terminal->column], 0, count * sizeof(union utf8_char));
		attr_init(&attr_row[terminal->column], terminal->curr_attr, count);
		break;
	case 'Z':    /* CBT */
		count = set[0] ? args[0] : 1;
		if (count == 0) count = 1;
		while (count > 0 && terminal->column >= 0) {
			if (terminal->tab_ruler[terminal->column]) count--;
			terminal->column--;
		}
		terminal->column++;
		break;
	case '`':    /* HPA */
		y = set[0] ? args[0] : 1;
		y = y <= 0 ? 1 : y > terminal->width ? terminal->width : y;
		
		terminal->column = y - 1;
		break;
	case 'b':    /* REP */
		count = set[0] ? args[0] : 1;
		if (count == 0) count = 1;
		if (terminal->last_char.byte[0])
			for (i = 0; i < count; i++)
				handle_char(terminal, terminal->last_char);
		terminal->last_char.byte[0] = 0;
		break;
	case 'c':    /* Primary DA */
		write(terminal->master, "\e[?6c", 5);
		break;
	case 'd':    /* VPA */
		x = set[0] ? args[0] : 1;
		x = x <= 0 ? 1 : x > terminal->height ? terminal->height : x;
		
		terminal->row = x - 1;
		break;
	case 'g':    /* TBC */
		if (!set[0] || args[0] == 0) {
			terminal->tab_ruler[terminal->column] = 0;
		} else if (args[0] == 3) {
			memset(terminal->tab_ruler, 0, terminal->width);
		}
		break;
	case 'h':    /* SM */
		for(i = 0; i < 10 && set[i]; i++) {
			handle_term_parameter(terminal, args[i], 1);
		}
		break;
	case 'l':    /* RM */
		for(i = 0; i < 10 && set[i]; i++) {
			handle_term_parameter(terminal, args[i], 0);
		}
		break;
	case 'm':    /* SGR */
		if (set[0] && set[1] && set[2] && args[1] == 5) {
			if (args[0] == 38) {
				handle_sgr(terminal, args[2] + 256);
				break;
			} else if (args[0] == 48) {
				handle_sgr(terminal, args[2] + 512);
				break;
			}
		}
		for(i = 0; i < 10; i++) {
			if(set[i]) {
				handle_sgr(terminal, args[i]);
			} else if(i == 0) {
				handle_sgr(terminal, 0);
				break;
			} else {
				break;
			}
		}
		break;
	case 'n':    /* DSR */
		i = set[0] ? args[0] : 0;
		if (i == 0 || i == 5) {
			write(terminal->master, "\e[0n", 4);
		} else if (i == 6) {
			snprintf(response, MAX_RESPONSE, "\e[%d;%dR",
			         terminal->origin_mode ?
				     terminal->row+terminal->margin_top : terminal->row+1,
				 terminal->column+1);
			write(terminal->master, response, strlen(response));
		}
 		break;
	case 'r':
		if(!set[0]) {
			terminal->margin_top = 0;
			terminal->margin_bottom = terminal->height-1;
			terminal->row = 0;
			terminal->column = 0;
		} else {
			top = (set[0] ? args[0] : 1) - 1;
			top = top < 0 ? 0 :
			      (top >= terminal->height ? terminal->height - 1 : top);
			bottom = (set[1] ? args[1] : 1) - 1;
			bottom = bottom < 0 ? 0 :
			         (bottom >= terminal->height ? terminal->height - 1 : bottom);
			if(bottom > top) {
				terminal->margin_top = top;
				terminal->margin_bottom = bottom;
			} else {
				terminal->margin_top = 0;
				terminal->margin_bottom = terminal->height-1;
			}
			if(terminal->origin_mode)
				terminal->row = terminal->margin_top;
			else
				terminal->row = 0;
			terminal->column = 0;
		}
		break;
	case 's':
		terminal->saved_row = terminal->row;
		terminal->saved_column = terminal->column;
		break;
	case 'u':
		terminal->row = terminal->saved_row;
		terminal->column = terminal->saved_column;
		break;
	default:
		fprintf(stderr, "Unknown CSI escape: %c\n", *p);
		break;
	}	
}

static void
handle_non_csi_escape(struct terminal *terminal, char code)
{
	switch(code) {
	case 'M':    /* RI */
		terminal->row -= 1;
		if(terminal->row < terminal->margin_top) {
			terminal->row = terminal->margin_top;
			terminal_scroll(terminal, -1);
		}
		break;
	case 'E':    /* NEL */
		terminal->column = 0;
		// fallthrough
	case 'D':    /* IND */
		terminal->row += 1;
		if(terminal->row > terminal->margin_bottom) {
			terminal->row = terminal->margin_bottom;
			terminal_scroll(terminal, +1);
		}
		break;
	case 'c':    /* RIS */
		terminal_init(terminal);
		break;
	case 'H':    /* HTS */
		terminal->tab_ruler[terminal->column] = 1;
		break;
	case '7':    /* DECSC */
		terminal->saved_row = terminal->row;
		terminal->saved_column = terminal->column;
		terminal->saved_attr = terminal->curr_attr;
		terminal->saved_origin_mode = terminal->origin_mode;
		terminal->saved_cs = terminal->cs;
		terminal->saved_g0 = terminal->g0;
		terminal->saved_g1 = terminal->g1;
		break;
	case '8':    /* DECRC */
		terminal->row = terminal->saved_row;
		terminal->column = terminal->saved_column;
		terminal->curr_attr = terminal->saved_attr;
		terminal->origin_mode = terminal->saved_origin_mode;
		terminal->cs = terminal->saved_cs;
		terminal->g0 = terminal->saved_g0;
		terminal->g1 = terminal->saved_g1;
		break;
	case '=':    /* DECPAM */
		terminal->key_mode = KM_APPLICATION;
		break;
	case '>':    /* DECPNM */
		terminal->key_mode = KM_NORMAL;
		break;
	default:
		fprintf(stderr, "Unknown escape code: %c\n", code);
		break;
	}
}

static void
handle_special_escape(struct terminal *terminal, char special, char code)
{
	int i, numChars;

	if (special == '#') {
		switch(code) {
		case '8':
			/* fill with 'E', no cheap way to do this */
			memset(terminal->data, 0, terminal->data_pitch * terminal->height);
			numChars = terminal->width * terminal->height;
			for(i = 0; i < numChars; i++) {
				terminal->data[i].byte[0] = 'E';
			}
			break;
		default:
			fprintf(stderr, "Unknown HASH escape #%c\n", code);
			break;
		}
	} else if (special == '(' || special == ')') {
		switch(code) {
		case '0':
			if (special == '(')
				terminal->g0 = CS_SPECIAL;
			else
				terminal->g1 = CS_SPECIAL;
			break;
		case 'A':
			if (special == '(')
				terminal->g0 = CS_UK;
			else
				terminal->g1 = CS_UK;
			break;
		case 'B':
			if (special == '(')
				terminal->g0 = CS_US;
			else
				terminal->g1 = CS_US;
			break;
		default:
			fprintf(stderr, "Unknown character set %c\n", code);
			break;
		}
	} else {
		fprintf(stderr, "Unknown special escape %c%c\n", special, code);
	}
}

static void
handle_sgr(struct terminal *terminal, int code)
{
	switch(code) {
	case 0:
		terminal->curr_attr = terminal->color_scheme->default_attr;
		break;
	case 1:
		terminal->curr_attr.a |= ATTRMASK_BOLD;
		if (terminal->curr_attr.fg < 8)
			terminal->curr_attr.fg += 8;
		break;
	case 4:
		terminal->curr_attr.a |= ATTRMASK_UNDERLINE;
		break;
	case 5:
		terminal->curr_attr.a |= ATTRMASK_BLINK;
		break;
	case 2:
	case 21:
	case 22:
		terminal->curr_attr.a &= ~ATTRMASK_BOLD;
		if (terminal->curr_attr.fg < 16 && terminal->curr_attr.fg >= 8)
			terminal->curr_attr.fg -= 8;
		break;
	case 24:
		terminal->curr_attr.a &= ~ATTRMASK_UNDERLINE;
		break;
	case 25:
		terminal->curr_attr.a &= ~ATTRMASK_BLINK;
		break;
	case 7:
	case 26:
		terminal->curr_attr.a |= ATTRMASK_INVERSE;
		break;
	case 27:
		terminal->curr_attr.a &= ~ATTRMASK_INVERSE;
		break;
	case 39:
		terminal->curr_attr.fg = terminal->color_scheme->default_attr.fg;
		break;
	case 49:
		terminal->curr_attr.bg = terminal->color_scheme->default_attr.bg;
		break;
	default:
		if(code >= 30 && code <= 37) {
			terminal->curr_attr.fg = code - 30;
			if (terminal->curr_attr.a & ATTRMASK_BOLD)
				terminal->curr_attr.fg += 8;
		} else if(code >= 40 && code <= 47) {
			terminal->curr_attr.bg = code - 40;
		} else if(code >= 256 && code < 512) {
			terminal->curr_attr.fg = code - 256;
		} else if(code >= 512 && code < 768) {
			terminal->curr_attr.bg = code - 512;
		} else {
			fprintf(stderr, "Unknown SGR code: %d\n", code);
		}
		break;
	}
}

/* Returns 1 if c was special, otherwise 0 */
static int
handle_special_char(struct terminal *terminal, char c)
{
	union utf8_char *row;
	struct attr *attr_row;
	
	row = terminal_get_row(terminal, terminal->row);
	attr_row = terminal_get_attr_row(terminal, terminal->row);
	
	switch(c) {
	case '\r':
		terminal->column = 0;
		break;
	case '\n':
		if (terminal->mode & MODE_LF_NEWLINE) {
			terminal->column = 0;
		}
		/* fallthrough */
	case '\v':
	case '\f':
		terminal->row++;
		if(terminal->row > terminal->margin_bottom) {
			terminal->row = terminal->margin_bottom;
			terminal_scroll(terminal, +1);
		}

		break;
	case '\t':
		while (terminal->column < terminal->width) {
			if (terminal->tab_ruler[terminal->column]) break;
			if (terminal->mode & MODE_IRM)
				terminal_shift_line(terminal, +1);
			row[terminal->column].byte[0] = ' ';
			row[terminal->column].byte[1] = '\0';
			attr_row[terminal->column] = terminal->curr_attr;
			terminal->column++;
		}
		if (terminal->column >= terminal->width) {
			terminal->column = terminal->width - 1;
		}

		break;
	case '\b':
		if (terminal->column >= terminal->width) {
			terminal->column = terminal->width - 2;
		} else if (terminal->column > 0) {
			terminal->column--;
		} else if (terminal->mode & MODE_AUTOWRAP) {
			terminal->column = terminal->width - 1;
			terminal->row -= 1;
			if (terminal->row < terminal->margin_top) {
				terminal->row = terminal->margin_top;
				terminal_scroll(terminal, -1);
			}
		}

		break;
	case '\a':
		/* Bell */
		break;
	case '\x0E': /* SO */
		terminal->cs = terminal->g1;
		break;
	case '\x0F': /* SI */
		terminal->cs = terminal->g0;
		break;
	default:
		return 0;
	}
	
	return 1;
}

static void
handle_char(struct terminal *terminal, union utf8_char utf8)
{
	union utf8_char *row;
	struct attr *attr_row;
	
	if (handle_special_char(terminal, utf8.byte[0])) return;

	apply_char_set(terminal->cs, &utf8);
	
	/* There are a whole lot of non-characters, control codes,
	 * and formatting codes that should probably be ignored,
	 * for example: */
	if (strncmp((char*) utf8.byte, "\xEF\xBB\xBF", 3) == 0) {
		/* BOM, ignore */
		return;
	} 
	
	/* Some of these non-characters should be translated, e.g.: */
	if (utf8.byte[0] < 32) {
		utf8.byte[0] = utf8.byte[0] + 64;
	}
	
	/* handle right margin effects */
	if (terminal->column >= terminal->width) {
		if (terminal->mode & MODE_AUTOWRAP) {
			terminal->column = 0;
			terminal->row += 1;
			if (terminal->row > terminal->margin_bottom) {
				terminal->row = terminal->margin_bottom;
				terminal_scroll(terminal, +1);
			}
		} else {
			terminal->column--;
 		}
 	}
	
	row = terminal_get_row(terminal, terminal->row);
	attr_row = terminal_get_attr_row(terminal, terminal->row);
	
	if (terminal->mode & MODE_IRM)
		terminal_shift_line(terminal, +1);
	row[terminal->column] = utf8;
	attr_row[terminal->column++] = terminal->curr_attr;

	if (utf8.ch != terminal->last_char.ch)
		terminal->last_char = utf8;
}

static void
terminal_data(struct terminal *terminal, const char *data, size_t length)
{
	int i;
	union utf8_char utf8;
	enum utf8_state parser_state;

	for (i = 0; i < length; i++) {
		parser_state =
			utf8_next_char(&terminal->state_machine, data[i]);
		switch(parser_state) {
		case utf8state_accept:
			utf8.ch = terminal->state_machine.s.ch;
			break;
		case utf8state_reject:
			/* the unicode replacement character */
			utf8.byte[0] = 0xEF;
			utf8.byte[1] = 0xBF;
			utf8.byte[2] = 0xBD;
			utf8.byte[3] = 0x00;
			break;
		default:
			continue;
		}

		/* assume escape codes never use non-ASCII characters */
		if (terminal->state == STATE_ESCAPE) {
			terminal->escape[terminal->escape_length++] = utf8.byte[0];
			if (utf8.byte[0] == '[') {
				terminal->state = STATE_ESCAPE_CSI;
				continue;
			} else if (utf8.byte[0] == '#' || utf8.byte[0] == '(' ||
				utf8.byte[0] == ')')
			{
				terminal->state = STATE_ESCAPE_SPECIAL;
				continue;
			} else {
				terminal->state = STATE_NORMAL;
				handle_non_csi_escape(terminal, utf8.byte[0]);
				continue;
			}
		} else if (terminal->state == STATE_ESCAPE_SPECIAL) {
			terminal->escape[terminal->escape_length++] = utf8.byte[0];
			terminal->state = STATE_NORMAL;
			if (isdigit(utf8.byte[0]) || isalpha(utf8.byte[0])) {
				handle_special_escape(terminal, terminal->escape[1],
				                      utf8.byte[0]);
				continue;
			}
		} else if (terminal->state == STATE_ESCAPE_CSI) {
			if (handle_special_char(terminal, utf8.byte[0]) != 0) {
				/* do nothing */
			} else if (utf8.byte[0] == '?') {
				terminal->qmark_flag = 1;
			} else {
				/* Don't overflow the buffer */
				if (terminal->escape_length < MAX_ESCAPE)
					terminal->escape[terminal->escape_length++] = utf8.byte[0];
				if (terminal->escape_length >= MAX_ESCAPE)
					terminal->state = STATE_NORMAL;
			}
			
			if (isalpha(utf8.byte[0]) || utf8.byte[0] == '@' ||
				utf8.byte[0] == '`')
			{
				terminal->state = STATE_NORMAL;
				handle_escape(terminal);
				continue;
			} else {
				continue;
			}
		}

		/* this is valid, because ASCII characters are never used to
		 * introduce a multibyte sequence in UTF-8 */
		if (utf8.byte[0] == '\e') {
			terminal->state = STATE_ESCAPE;
			terminal->escape[0] = '\e';
			terminal->escape_length = 1;
			terminal->qmark_flag = 0;
		} else {
			handle_char(terminal, utf8);
		} /* if */
	} /* for */

	window_schedule_redraw(terminal->window);
}

static void
key_handler(struct window *window, uint32_t key, uint32_t sym,
	    uint32_t state, uint32_t modifiers, void *data)
{
	struct terminal *terminal = data;
	char ch[MAX_RESPONSE];
	int len = 0;

	switch (sym) {
	case XK_F11:
		if (!state)
			break;
		terminal->fullscreen ^= 1;
		window_set_fullscreen(window, terminal->fullscreen);
		window_schedule_redraw(terminal->window);
		break;

	case XK_BackSpace:
	case XK_Tab:
	case XK_Linefeed:
	case XK_Clear:
	case XK_Pause:
	case XK_Scroll_Lock:
	case XK_Sys_Req:
	case XK_Escape:
		ch[len++] = sym & 0x7f;
		break;

	case XK_Return:
		if (terminal->mode & MODE_LF_NEWLINE) {
			ch[len++] = 0x0D;
			ch[len++] = 0x0A;
		} else {
			ch[len++] = 0x0D;
		}
		break;

	case XK_Shift_L:
	case XK_Shift_R:
	case XK_Control_L:
	case XK_Control_R:
	case XK_Alt_L:
	case XK_Alt_R:
		break;

	case XK_Insert:
		len = function_key_response('[', 2, modifiers, '~', ch);
		break;
	case XK_Delete:
		if (terminal->mode & MODE_DELETE_SENDS_DEL) {
			ch[len++] = '\x04';
		} else {
			len = function_key_response('[', 3, modifiers, '~', ch);
		}
		break;
	case XK_Page_Up:
		len = function_key_response('[', 5, modifiers, '~', ch);
		break;
	case XK_Page_Down:
		len = function_key_response('[', 6, modifiers, '~', ch);
		break;
	case XK_F1:
		len = function_key_response('O', 1, modifiers, 'P', ch);
		break;
	case XK_F2:
		len = function_key_response('O', 1, modifiers, 'Q', ch);
		break;
	case XK_F3:
		len = function_key_response('O', 1, modifiers, 'R', ch);
		break;
	case XK_F4:
		len = function_key_response('O', 1, modifiers, 'S', ch);
		break;
	case XK_F5:
		len = function_key_response('[', 15, modifiers, '~', ch);
		break;
	case XK_F6:
		len = function_key_response('[', 17, modifiers, '~', ch);
		break;
	case XK_F7:
		len = function_key_response('[', 18, modifiers, '~', ch);
		break;
	case XK_F8:
		len = function_key_response('[', 19, modifiers, '~', ch);
		break;
	case XK_F9:
		len = function_key_response('[', 20, modifiers, '~', ch);
		break;
	case XK_F10:
		len = function_key_response('[', 21, modifiers, '~', ch);
		break;
	case XK_F12:
		len = function_key_response('[', 24, modifiers, '~', ch);
		break;
	default:
		/* Handle special keys with alternate mappings */
		len = apply_key_map(terminal->key_mode, sym, modifiers, ch);
		if (len != 0) break;
		
		if (modifiers & WINDOW_MODIFIER_CONTROL) {
			if (sym >= '3' && sym <= '7')
				sym = (sym & 0x1f) + 8;

			if (!((sym >= '!' && sym <= '/') ||
				(sym >= '8' && sym <= '?') ||
				(sym >= '0' && sym <= '2'))) sym = sym & 0x1f;
			else if (sym == '2') sym = 0x00;
			else if (sym == '/') sym = 0x1F;
			else if (sym == '8' || sym == '?') sym = 0x7F;
		} else if ((terminal->mode & MODE_ALT_SENDS_ESC) && 
			(modifiers & WINDOW_MODIFIER_ALT))
		{
			ch[len++] = 0x1b;
		} else if (modifiers & WINDOW_MODIFIER_ALT) {
			sym = sym | 0x80;
		}

		if (sym < 256)
			ch[len++] = sym;
		break;
	}

	if (state && len > 0)
		write(terminal->master, ch, len);
}

static void
keyboard_focus_handler(struct window *window,
		       struct input *device, void *data)
{
	struct terminal *terminal = data;

	terminal->focused = (device != NULL);
	window_schedule_redraw(terminal->window);
}

static struct terminal *
terminal_create(struct display *display, int fullscreen)
{
	struct terminal *terminal;
	cairo_surface_t *surface;
	cairo_t *cr;

	terminal = malloc(sizeof *terminal);
	if (terminal == NULL)
		return terminal;

	memset(terminal, 0, sizeof *terminal);
	terminal->fullscreen = fullscreen;
	terminal->color_scheme = &DEFAULT_COLORS;
	terminal_init(terminal);
	terminal->margin_top = 0;
	terminal->margin_bottom = -1;
	terminal->window = window_create(display, "Wayland Terminal",
					 500, 400);

	init_state_machine(&terminal->state_machine);
	init_color_table(terminal);

	terminal->display = display;
	terminal->margin = 5;

	window_set_fullscreen(terminal->window, terminal->fullscreen);
	window_set_user_data(terminal->window, terminal);
	window_set_redraw_handler(terminal->window, redraw_handler);

	window_set_key_handler(terminal->window, key_handler);
	window_set_keyboard_focus_handler(terminal->window,
					  keyboard_focus_handler);

	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 0, 0);
	cr = cairo_create(surface);
	terminal->font_bold = cairo_toy_font_face_create ("mono",
	                      CAIRO_FONT_SLANT_NORMAL,
	                      CAIRO_FONT_WEIGHT_BOLD);
	cairo_font_face_reference(terminal->font_bold);
	terminal->font_normal = cairo_toy_font_face_create ("mono",
	                        CAIRO_FONT_SLANT_NORMAL,
	                        CAIRO_FONT_WEIGHT_NORMAL);
	cairo_font_face_reference(terminal->font_normal);
	cairo_set_font_face(cr, terminal->font_normal);
	cairo_set_font_size(cr, 14);
	cairo_font_extents(cr, &terminal->extents);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);

	terminal_resize(terminal, 80, 24);
	terminal_draw(terminal);

	return terminal;
}

static gboolean
io_handler(GIOChannel   *source,
	   GIOCondition  condition,
	   gpointer      data)
{
	struct terminal *terminal = data;
	gchar buffer[256];
	gsize bytes_read;
	GError *error = NULL;

	g_io_channel_read_chars(source, buffer, sizeof buffer,
				&bytes_read, &error);

	terminal_data(terminal, buffer, bytes_read);

	return TRUE;
}

static int
terminal_run(struct terminal *terminal, const char *path)
{
	int master;
	pid_t pid;

	pid = forkpty(&master, NULL, NULL, NULL);
	if (pid == 0) {
		setenv("TERM", "xterm-256color", 1);
		setenv("COLORTERM", "xterm-256color", 1);
		if (execl(path, path, NULL)) {
			printf("exec failed: %m\n");
			exit(EXIT_FAILURE);
		}
	} else if (pid < 0) {
		fprintf(stderr, "failed to fork and create pty (%m).\n");
		return -1;
	}

	terminal->master = master;
	terminal->channel = g_io_channel_unix_new(master);
	fcntl(master, F_SETFL, O_NONBLOCK);
	g_io_add_watch(terminal->channel, G_IO_IN,
		       io_handler, terminal);

	return 0;
}

static const GOptionEntry option_entries[] = {
	{ "fullscreen", 'f', 0, G_OPTION_ARG_NONE,
	  &option_fullscreen, "Run in fullscreen mode" },
	{ NULL }
};

int main(int argc, char *argv[])
{
	struct display *d;
	struct terminal *terminal;

	d = display_create(&argc, &argv, option_entries);
	if (d == NULL) {
		fprintf(stderr, "failed to create display: %m\n");
		return -1;
	}

	terminal = terminal_create(d, option_fullscreen);
	if (terminal_run(terminal, "/bin/bash"))
		exit(EXIT_FAILURE);

	display_run(d);

	return 0;
}
