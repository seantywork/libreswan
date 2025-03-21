/* test subnets, for libreswan
 *
 * Copyright (C) 2000  Henry Spencer.
 * Copyright (C) 2018, 2019  Andrew Cagney
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Library General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/lgpl-2.1.txt>.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Library General Public
 * License for more details.
 */

#include <stdio.h>

#include "lswcdefs.h"		/* for elemsof() */
#include "constants.h"		/* for streq() */
#include "ipcheck.h"
#include "ip_cidr.h"

static void check__ttocidr_num(void)
{
	static const struct test {
		int line;
		const struct ip_info *afi;
		const char *in;
		const char *str;
	} tests[] = {
		{ LN, &ipv4_info, "128.0.0.0/0", "128.0.0.0/0", },
		{ LN, &ipv6_info, "8000::/0", "8000::/0", },

		{ LN, &ipv4_info, "128.0.0.0/1", "128.0.0.0/1", },
		{ LN, &ipv6_info, "8000::/1", "8000::/1", },

		{ LN, &ipv4_info, "1.2.255.4/23", "1.2.255.4/23", },
		{ LN, &ipv4_info, "1.2.255.255/24", "1.2.255.255/24", },
		{ LN, &ipv4_info, "1.2.3.255/25", "1.2.3.255/25", },

		{ LN, &ipv6_info, "1:2:3:ffff::/63", "1:2:3:ffff::/63", },
		{ LN, &ipv6_info, "1:2:3:ffff:ffff::/64", "1:2:3:ffff:ffff::/64", },
		{ LN, &ipv6_info, "1:2:3:4:ffff::/65", "1:2:3:4:ffff::/65", },

		{ LN, &ipv4_info, "1.2.3.255/31", "1.2.3.255/31", },
		{ LN, &ipv4_info, "1.2.3.255/32", "1.2.3.255/32", },
		{ LN, &ipv6_info, "1:2:3:4:5:6:7:ffff/127", "1:2:3:4:5:6:7:ffff/127", },
		{ LN, &ipv6_info, "1:2:3:4:5:6:7:ffff/128", "1:2:3:4:5:6:7:ffff/128", },

		/* no mask */
		{ LN, &ipv4_info, "1.2.3.4",  "1.2.3.4/32", },
		{ LN, &ipv6_info, "1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7:8/128", },

		/* empty */
		{ LN, &ipv4_info, "", NULL, },
		{ LN, &ipv6_info, "", NULL, },
		/* mask out-of-bounds */
		{ LN, &ipv4_info, "1.2.3.255/33", NULL, },
		{ LN, &ipv6_info, "1:2:3:4:5:6:7:ffff/129", NULL, },
	};

	for (size_t ti = 0; ti < elemsof(tests); ti++) {
		const struct test *t = &tests[ti];
		PRINT("%s %s ", t->in, t->str != NULL ? t->str : "ERROR");

		ip_cidr tmp, *cidr = &tmp;
		err_t err = ttocidr_num(shunk1(t->in), t->afi, cidr);
		if (err != NULL) {
			if (t->str != NULL) {
				FAIL("numeric_to_cidr() unexpectedly failed: %s", err);
			}
			continue;
		} else if (t->str == NULL) {
			FAIL("numeric_to_cidr() unexpectedly succeeded");
		}

		CHECK_INFO(cidr);
		CHECK_STR2(cidr);
	}
}

static void check_cidr_is()
{
	static const struct test {
		int line;
		const struct ip_info *afi;
		const char *in;
		bool specified;
	} tests[] = {
		/* default route */
		{ LN, &ipv4_info, "0.0.0.0/0", false, },
		{ LN, &ipv6_info, "::/0", false, },

		/* unspecified address */
		{ LN, &ipv4_info, "0.0.0.0/32", false, },
		{ LN, &ipv4_info, "0.0.0.0/24", false, },
		{ LN, &ipv4_info, "0.0.0.0/16", false, },
		{ LN, &ipv6_info, "::/128", false, },
		{ LN, &ipv6_info, "::/64", false, },

		/* something valid */
		{ LN, &ipv4_info, "127.0.0.1/32", true, },
		{ LN, &ipv6_info, "::1/128", true, },
	};

	for (size_t ti = 0; ti < elemsof(tests); ti++) {
		const struct test *t = &tests[ti];
		PRINT("%s %s ", t->in, bool_str(t->specified));

		ip_cidr cidr;
		err_t err = ttocidr_num(shunk1(t->in), t->afi, &cidr);
		if (err != NULL) {
			FAIL("numeric_to_cidr() unexpectedly failed: %s", err);
		}

		bool specified = cidr_is_specified(cidr);
		if (specified != t->specified) {
			FAIL("cidr_is_specified() returned %s, expecting %s",
			     bool_str(specified), bool_str(t->specified));
		}
	}
}

void ip_cidr_check(void)
{
	check__ttocidr_num();
	check_cidr_is();
}
