/*
 * Copyright © 2011 Rafael Fernández López <ereslibre@ereslibre.es>
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
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <cairo.h>
#include <glib.h>

#include "wayland-client.h"
#include "wayland-glib.h"
#include "window.h"

struct test {
  struct display *display;
  struct window *window;
  int x, y, width, height;
};

static void
draw_rectangle(cairo_surface_t *surface, int width, int height)
{
  cairo_t *cr = cairo_create(surface);
  cairo_surface_t *tux = cairo_image_surface_create_from_png("./tux.png");
  int w = cairo_image_surface_get_width(tux);
  int h = cairo_image_surface_get_height(tux);
  
  cairo_arc(cr, 128.0, 129.0, 76.8, 0, 2 * M_PI);
  cairo_clip(cr);
  cairo_new_path(cr);

  cairo_scale(cr, 256.0 / w, 256.0 / h);
  
  cairo_set_source_surface(cr, tux, 0, 50);
  cairo_paint(cr);
  
  cairo_surface_destroy(tux);
}

int main(int argc, char *argv[])
{
  cairo_surface_t *s;
	struct test test;
	struct display *d;

	d = display_create(&argc, &argv, NULL);
	if (d == NULL) {
		fprintf(stderr, "failed to create display: %m\n");
		return -1;
	}

  test.x = 100;
  test.y = 100;
	test.width = 256;
	test.height = 256;
	test.display = d;
	test.window = window_create(d, "test", test.width, test.height);

  window_set_decoration(test.window, 0);
  window_draw(test.window);
  s = window_get_surface(test.window);
  if (!s || cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
    return -1;
  }
  
  draw_rectangle(s, test.width, test.height);
  cairo_surface_flush(s);
  cairo_surface_destroy(s);
  window_flush(test.window);

  window_set_user_data(test.window, &test);
  display_run(d);
  
	return 0;
}
