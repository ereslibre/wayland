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
  cairo_surface_t *surface;
  int width, height;
};

static int increase = 1;
static float radius = 0.0;

static void
draw_tux(struct test *test)
{
  cairo_t *cr = cairo_create(test->surface);

  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_paint(cr);

  cairo_surface_t *tux = cairo_image_surface_create_from_png("./tux.png");
  int w = cairo_image_surface_get_width(tux);
  int h = cairo_image_surface_get_height(tux);

  cairo_arc(cr, 140.0, 140.0, radius, 0, 2 * M_PI);
  cairo_clip(cr);
  cairo_new_path(cr);

  cairo_set_source_surface(cr, tux, 0, 50);
  cairo_paint(cr);
  
  cairo_surface_destroy(tux);
}

static void
frame_callback(void *data, uint32_t time)
{
  struct test *test = data;

  if (radius > 100.0) {
    increase = 0;
  } else if (radius < 20.0) {
    increase = 1;
  }

  if (increase == 1) {
    radius += 1.0;
  } else {
    radius -= 1.0;
  }

  draw_tux(test);

  window_damage(test->window, 0, 0, test->width, test->height);
  wl_display_frame_callback(display_get_display(test->display),
                            frame_callback, test);
}

int main(int argc, char *argv[])
{
  struct test test;
  struct display *d;
  int size;

  d = display_create(&argc, &argv, NULL);
  if (d == NULL) {
    fprintf(stderr, "failed to create display: %m\n");
    return -1;
  }

  test.width = 280;
  test.height = 280;
  test.display = d;
  test.window = window_create(d, "test", test.width, test.height);

  window_set_buffer_type(test.window, WINDOW_BUFFER_TYPE_SHM);

  window_set_decoration(test.window, 0);
  window_create_surface(test.window);
  test.surface = window_get_surface(test.window);

  window_flush(test.window);

  window_set_user_data(test.window, &test);
  wl_display_frame_callback(display_get_display(d),
                            frame_callback, &test);
  display_run(d);
  
  return 0;
}
