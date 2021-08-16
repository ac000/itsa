/* SPDX-License-Identifier: GPL-2.0 */

/*
 * color.h - Color output
 *
 * Copyright (c) 2021		Andrew Clayton <andrew@digital-domain.net>
 */

#ifndef _COLOR_H_
#define _COLOR_H_

#include <stdio.h>

#include "textus_coloris.h"

enum msg_type {
	MT_ERROR = 0,
	MT_WARNING,
	MT_INFO,
	MT_CONFIRM,
	MT_SUCCESS,
};

extern void printc_xtra(FILE *fp, enum msg_type type, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
extern void set_colors(void);

#define printec(fmt, ...) printc_xtra(stderr, MT_ERROR, fmt, ##__VA_ARGS__)
#define printwc(fmt, ...) printc_xtra(stdout, MT_WARNING, fmt, ##__VA_ARGS__)
#define printcc(fmt, ...) printc_xtra(stdout, MT_CONFIRM, fmt, ##__VA_ARGS__)
#define printsc(fmt, ...) printc_xtra(stdout, MT_SUCCESS, fmt, ##__VA_ARGS__)
#define printic(fmt, ...) printc_xtra(stdout, MT_INFO, fmt, ##__VA_ARGS__)

#define printc(fmt, ...) tc_print(stdout, fmt, ##__VA_ARGS__)

#endif /* _COLOR_H_ */
