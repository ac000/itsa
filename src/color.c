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
#include "textus_coloris.h"

#define TC_HI_YELLOW		"\e[38;5;11m"
#define TC_HI_GREEN		"\e[38;5;10m"
#define TC_HI_RED		"\e[38;5;9m"
#define TC_HI_BLUE		"\e[38;5;33m"
#define TC_GREEN		"\e[38;5;40m"
#define TC_RED			"\e[38;5;160m"
#define TC_BLUE			"\e[38;5;75m"
#define TC_CHARC		"\e[38;5;8m"
#define TC_TANG			"\e[38;5;220m"

#define TC_BOLD			"\e[1m"
#define TC_RST			"\e[0m"

static const struct tc_coloris colors[] = {
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

static const struct {
	const char *str;
} msg_types[] = {
	[MT_ERROR]	= { "[#ERROR#ERROR#RST#]"		},
	[MT_WARNING]	= { "[#WARNING#WARNING#RST#]"		},
	[MT_INFO]	= { "[#INFO#INFO#RST#]"			},
	[MT_CONFIRM]	= { "[#CONFIRM#CONFIRMATION#RST#]"	},
	[MT_SUCCESS]	= { "[#SUCCESS#OK#RST#]"		},
};

void printc_xtra(FILE *fp, enum msg_type type, const char *fmt, ...)
{
	va_list args;
	char *str;
	int len;

	len = asprintf(&str, "%s %s", msg_types[type].str, fmt);
	if (len == -1)
		return;

	va_start(args, fmt);
	tc_printv(fp, str, args);
	va_end(args);

	free(str);
}

void set_colors(void)
{
	const char *color = getenv("ITSA_COLOR");
	enum tc_coloris_mode mode = TC_COLORIS_MODE_AUTO;

	if (!color)
		goto out_set;

	switch (*color) {
	case 't':
	case 'T':
	case 'y':
	case 'Y':
		mode = TC_COLORIS_MODE_ON;
		break;
	case 'f':
	case 'F':
	case 'n':
	case 'N':
		mode = TC_COLORIS_MODE_OFF;
		break;
	}

out_set:
	tc_set_colors(colors, mode);
}
