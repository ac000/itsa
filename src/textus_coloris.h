/*
 * textus_coloris.h - colourised console output
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

#ifndef _TEXTUS_COLORIS_H_
#define _TEXTUS_COLORIS_H_

#include <stdio.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tc_coloris {
	const char *color;
	const char *code;
};

#pragma GCC visibility push(default)

extern char *tc_cstringv(const char *fmt, va_list args)
	__attribute__((format(printf, 1, 0)));
extern char *tc_cstring(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));
extern int tc_printv(FILE *fp, const char *fmt, va_list args)
	__attribute__((format(printf, 2, 0)));
extern int tc_print(FILE *fp, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
extern void tc_set_colors(const struct tc_coloris *colors);

#pragma GCC visibility pop

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TEXTUS_COLORIS_H_ */
