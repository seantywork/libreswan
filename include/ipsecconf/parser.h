/* Libreswan config file parser
 * This header is only for use by code within libipsecconf.
 *
 * Copyright (C) 2001-2002 Mathieu Lafon - Arkoon Network Security
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/gpl2.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#ifndef _IPSEC_PARSER_H_
#define _IPSEC_PARSER_H_

struct logger;

#include "parser.tab.h"	/* generated by bison */
#include "parser-controls.h"

extern const char *parser_cur_filename(void);
extern int parser_cur_lineno(void);
extern void parser_y_error(char *b, int size, const char *s);
extern void parser_y_init(const char *name, FILE *f );
void parser_y_include(const char *filename, struct logger *logger);

#define THIS_IPSEC_CONF_VERSION 2

#endif /* _IPSEC_PARSER_H_ */
