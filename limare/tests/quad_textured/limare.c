/*
 * Copyright 2011-2012 Luc Verhaegen <libv@skynet.be>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

/*
 * Template file for replaying a dumped stream.
 */

#include <stdlib.h>
#include <stdio.h>

#include <GLES2/gl2.h>

#include "limare.h"
#include "fb.h"
#include "symbols.h"
#include "gp.h"
#include "pp.h"
#include "program.h"
#include "formats.h"

#include "companion.h"

#define WIDTH 400
#define HEIGHT 240

int
main(int argc, char *argv[])
{
	struct limare_state *state;
	int ret;

	float vertices[] = { -0.6, -1, 0.0,
			      0.6, -1, 0.0,
			     -0.6,  1, 0.0,
			      0.6,  1, 0.0 };
	float coords[] = {0, 0, 1, 0, 0, 1, 1, 1};

	const char *vertex_shader_source =
		"attribute vec4 in_vertex;\n"
		"attribute vec2 in_coord;\n"
		"varying vec2 coord;\n"
		"void main()\n"
		"{\n"
		"    gl_Position = in_vertex;\n"
		"    coord = in_coord;\n"
		"}\n";

	const char *fragment_shader_source =
		"precision mediump float;\n"
		"varying vec2 coord;\n"
		"uniform sampler2D in_texture;\n"
		"void main()\n"
		"{\n"
		"    gl_FragColor = texture2D(in_texture, coord);\n"
		"}\n";

	fb_clear();

	state = limare_init();
	if (!state)
		return -1;

	ret = limare_state_setup(state, WIDTH, HEIGHT, 0xFF505050);
	if (ret)
		return ret;

	vertex_shader_attach(state, vertex_shader_source);
	fragment_shader_attach(state, fragment_shader_source);

	ret = limare_link(state);
	if (ret)
		return ret;

	limare_attribute_pointer(state, "in_vertex", 4, 3, vertices);

	limare_attribute_pointer(state, "in_coord", 4, 2, coords);

	limare_texture_attach(state, "in_texture", companion_texture_flat,
			      COMPANION_TEXTURE_WIDTH, COMPANION_TEXTURE_HEIGHT,
			      COMPANION_TEXTURE_FORMAT);

	ret = limare_draw_arrays(state, GL_TRIANGLE_STRIP, 0, 4);
	if (ret)
		return ret;

	ret = limare_flush(state);
	if (ret)
		return ret;

	fb_dump(state->pp->frame_address, 0, state->width, state->height);

	limare_finish();

	return 0;
}
