/* SPDX-License-Identifier: GPL-2.0 */

/*
 * color.c - colourised output
 *
 * Copyright (c) 2021		Andrew Clayton <andrew@digital-domain.net>
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>

#include "color.h"

#define MAX_COLOR_NAME		32

static const struct color {
	const char *color;
	const char *code;
} colors[] = {
	{ "HI_YELLOW",		TC_HI_YELLOW		},
	{ "HI_GREEN",		TC_HI_GREEN		},
	{ "HI_RED",		TC_HI_RED		},
	{ "HI_BLUE",		TC_HI_BLUE		},
	{ "GREEN",		TC_GREEN		},
	{ "RED",		TC_RED			},
	{ "BLUE",		TC_BLUE			},
	{ "CHARC",		TC_CHARC		},
	{ "TANG",		TC_TANG			},

	{ "BOLD",		TC_BOLD			},
	{ "RST",		TC_RST			},

	{ "MSG_INFO",		TC_HI_BLUE		},
	{ "MSG_WARN",		TC_HI_YELLOW		},
	{ "MSG_ERR",		TC_HI_RED		},

	{ "INFO",		TC_BLUE			},
	{ "CONFIRM",		TC_CHARC		},
	{ "WARNING",		TC_HI_YELLOW		},
	{ "SUCCESS",		TC_HI_GREEN		},
	{ "ERROR",		TC_HI_RED		},

	{ "STRUE",		TC_HI_GREEN		},
	{ "SFALSE",		TC_HI_RED		},

	{}
};

static const char *lookup(const char *color)
{
	const struct color *ptr = colors;

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

extern bool NO_COLOR;
static char *parser(const char *buf)
{
	char *new = malloc(ALLOC_SZ);
	char *ptr = new;
	char *sptr = new;
	char color[MAX_COLOR_NAME];
	char *cptr = color;
	bool in_color = false;
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
		if (!NO_COLOR)
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

void printc_xtra(FILE *fp, enum msg_type type, const char *fmt, ...)
{
	va_list ap;
	char *buf = NULL;
	char *cstr;
	char *str;
	const char *tstr;
	int len;

	switch (type) {
	case MT_ERROR:
		tstr = "[#ERROR#ERROR#RST#] ";
		break;
	case MT_WARNING:
		tstr = "[#WARNING#WARNING#RST#] ";
		break;
	case MT_INFO:
		tstr = "[#INFO#INFO#RST#] ";
		break;
	case MT_CONFIRM:
		tstr = "[#CONFIRM#CONFIRMATION#RST#] ";
		break;
	case MT_SUCCESS:
		tstr = "[#SUCCESS#OK#RST#] ";
		break;
	default:
		tstr = "";
	}

	str = malloc(strlen(fmt) + strlen(tstr) + 1);
	memcpy(str, tstr, strlen(tstr));
	memcpy(str + strlen(tstr), fmt, strlen(fmt) + 1);

	va_start(ap, fmt);
	len = vasprintf(&buf, str, ap);
	va_end(ap);
	if (len == -1)
		goto out_free;

	cstr = parser(buf);
	fprintf(fp, "%s", cstr);

	free(cstr);

out_free:
	free(str);
	free(buf);
}

void printc(const char *fmt, ...)
{
	va_list ap;
	char *buf = NULL;
	char *cstr;
	int len;

	va_start(ap, fmt);
	len = vasprintf(&buf, fmt, ap);
	va_end(ap);
	if (len == -1)
		goto out_free;

	cstr = parser(buf);
	printf("%s", cstr);

	free(cstr);

out_free:
	free(buf);
}
