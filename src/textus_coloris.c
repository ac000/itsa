/*
 * textus_coloris.c - colourised console output
 */

/*
 * Copyright (c) 2021 Andrew Clayton
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>

#include "textus_coloris.h"

static __thread const struct tc_coloris *tc_colors;

static const char *lookup(const char *color)
{
	const struct tc_coloris *ptr = tc_colors;

	for ( ; ptr->color != NULL; ptr++) {
		if (strcmp(color, ptr->color) == 0)
			return ptr->code;
	}

	return NULL;
}

#define ALLOC_SZ	64
static void *srealloc(char **base, size_t extra, char **ptr, size_t *alloc,
		      char **sptr)
{
	size_t len = *ptr - *base;
	size_t slen = *sptr - *base;

	if (len + extra < *alloc)
		return *base;

	*base = realloc(*base, *alloc + ALLOC_SZ);
	*alloc += ALLOC_SZ;

	*ptr = *base + len;
	*sptr = *base + slen;

	return *base;
}

#define MAX_COLOR_NAME		32
static char *parser(const char *buf)
{
	char *new = malloc(ALLOC_SZ);
	char *ptr = new;
	char *sptr = new;
	char color[MAX_COLOR_NAME];
	char *cptr = color;
	bool in_color = false;
	bool use_color = getenv("NO_COLOR") ? false : true;
	size_t alloc = ALLOC_SZ;

	while (*buf) {
		const char *code = "\0";

		*ptr = *buf;

		if (*buf == '#' && !in_color) {
			sptr = ptr;
			cptr = color;
			in_color = true;
			goto next;
		} else if (in_color && *buf != '#') {
			*cptr = *buf;
			cptr++;
			goto next;
		} else if (!in_color) {
			goto next;
		}

		/*
		 * in_color == true and we have a trailing '#', see if
		 * it's a colour code.
		 */
		in_color = false;
		*cptr = '\0';
		if (use_color)
			code = lookup(color);

		if (code && *code == '\0') {
			ptr = sptr - 1;
		} else if (code) {
			size_t clen = strlen(code);

			ptr = sptr;
			new = srealloc(&new, clen, &ptr, &alloc, &sptr);
			memcpy(ptr, code, clen);
			ptr += clen - 1;
		} else {
			/*
			 * Handle cases like
			 *
			 *   #XXXX##text...
			 *
			 * Note the extra '#' which should appear in the
			 * output string.
			 */
			buf--;
			ptr--;
		}

next:
		new = srealloc(&new, 1, &ptr, &alloc, &sptr);
		buf++;
		ptr++;
	}
	*ptr = '\0';

	return new;
}

static char *cstring(const char *fmt, va_list args)
{
	va_list args2;
	char *buf = NULL;
	char *cstr;
	size_t buf_sz = 0;
	int len;

	/*
	 * Need to take a copy of args as it will be in an undefined
	 * state after the first call to vsnprintf(3).
	 *
	 * We could just use vasprintf(3) but I would like to not have
	 * to define _GNU_SOURCE in this particular instance.
	 */
	va_copy(args2, args);

	/* Determine the required size of 'buf; */
	len = vsnprintf(buf, buf_sz, fmt, args);
	if (len < 0)
		return NULL;

	buf_sz = (size_t)len + 1;
	buf = malloc(buf_sz);
	if (!buf)
		return NULL;

	len = vsnprintf(buf, buf_sz, fmt, args2);
	if (len < 0) {
		free(buf);
		return NULL;
	}

	cstr = parser(buf);
	free(buf);

	return cstr;
}

/*
 * Colourise a string.
 *
 * Returns a pointer to a colourised version of the given
 * string. User should free(2) the returned pointer.
 */
char *tc_cstringv(const char *fmt, va_list args)
{
	return cstring(fmt, args);
}

/*
 * Colourise a string.
 *
 * Returns a pointer to a colourised version of the given
 * string. User should free(2) the returned pointer.
 */
char *tc_cstring(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	va_end(args);

	return cstring(fmt, args);
}

/*
 * Print a colourised version of the given string to the
 * specified output stream.
 *
 * Like the v*printf() functions.
 */
int tc_printv(FILE *fp, const char *fmt, va_list args)
{
	char *cstr;
	int len;

	cstr = cstring(fmt, args);
	if (!cstr)
		return -1;

	len = fprintf(fp, "%s", cstr);
	free(cstr);

	return len;
}

/*
 * Print a colourised version of the given string to the
 * specified output stream.
 */
int tc_print(FILE *fp, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	va_end(args);

	return tc_printv(fp, fmt, args);
}

/*
 * Set the colour map.
 */
void tc_set_colors(const struct tc_coloris *colors)
{
	tc_colors = colors;
}
