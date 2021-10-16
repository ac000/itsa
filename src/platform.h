/* SPDX-License-Identifier: LGPL-2.1 */

/*
 * platform.h - Platform dependant stuff
 *
 * Copyright (c) 2021		Andrew Clayton <andrew@digital-domain.net>
 */

#ifndef _PLATFORM_H_
#define _PLATFORM_H_

#include <fcntl.h>

#if defined(__FreeBSD__) && !defined(O_PATH)
#define O_PATH	0
#endif

#endif /* _PLATFORM_H_ */
