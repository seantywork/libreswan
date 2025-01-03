/* Libreswan ISAKMP Quirks handling
 * Copyright (C) 2003 Michael Richardson <mcr@xelerance.com>
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

#ifndef _QUIRKS_H_
#define _QUIRKS_H_

#include "vendorid.h"	/* for enum known_vendorid */

struct ike_sa;
struct msg_digest;

/*
 * PAUL: Why are the quirks in the message digest and not in the state?
 *       EECK: they are in both! Candidate to phase out completely
 *
 * message digest quirks are merged into state quirks by merge_quirks.
 */

/**
 * Where to store various quirks (usually encountered during interop)
 *
 */
struct isakmp_quirks {
	bool xauth_ack_msgid;	/**< Whether to reset the msgid after an
				 * xauth set, such as for SSH Sentinel.
				 */
	bool modecfg_pull_mode;	/* if the client should request peers IP */
	enum known_vendorid qnat_traversal_vid;	/**< which NAT-type vendor IDs we got */
	bool xauth_vid;	/**< if the client has XAUTH */
};

extern void merge_quirks(struct ike_sa *ike,
			 const struct msg_digest *md);

#endif
