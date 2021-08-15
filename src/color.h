/* SPDX-License-Identifier: GPL-2.0 */

/*
 * color.h - Color output
 *
 * Copyright (c) 2021		Andrew Clayton <andrew@digital-domain.net>
 */

#ifndef _COLOR_H_
#define _COLOR_H_

#include <stdio.h>

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

enum msg_type {
	MT_ERROR = 0,
	MT_WARNING,
	MT_INFO,
	MT_CONFIRM,
	MT_SUCCESS,
};

#define __fmt_printf(n, m)	__attribute__((format(printf, n, m)))
/* forward declaration */
void printc_xtra(FILE *fp, enum msg_type type, const char *fmt, ...)
	__fmt_printf(3, 4);

#define printec(fmt, ...) printc_xtra(stderr, MT_ERROR, fmt, ##__VA_ARGS__)
#define printwc(fmt, ...) printc_xtra(stdout, MT_WARNING, fmt, ##__VA_ARGS__)
#define printcc(fmt, ...) printc_xtra(stdout, MT_CONFIRM, fmt, ##__VA_ARGS__)
#define printsc(fmt, ...) printc_xtra(stdout, MT_SUCCESS, fmt, ##__VA_ARGS__)
#define printic(fmt, ...) printc_xtra(stdout, MT_INFO, fmt, ##__VA_ARGS__)

extern void printc(const char *fmt, ...) __fmt_printf(1, 2);

#endif /* _COLOR_H_ */
