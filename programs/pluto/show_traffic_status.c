/* routines for state objects, for libreswan
 *
 * Copyright (C) 1997 Angelos D. Keromytis.
 * Copyright (C) 1998-2001, 2013-2017 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2003-2008 Michael C Richardson <mcr@xelerance.com>
 * Copyright (C) 2003-2010 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2008-2009 David McCullough <david_mccullough@securecomputing.com>
 * Copyright (C) 2009, 2012 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2012-2019 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2012 Wes Hardaker <opensource@hardakers.net>
 * Copyright (C) 2012 Bram <bram-bcrafjna-erqzvar@spam.wizbit.be>
 * Copyright (C) 2012-2013 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2013 Tuomo Soini <tis@foobar.fi>
 * Copyright (C) 2013 Matt Rogers <mrogers@redhat.com>
 * Copyright (C) 2013 Florian Weimer <fweimer@redhat.com>
 * Copyright (C) 2015-2019 Andrew Cagney <cagney@gnu.org>
 * Copyright (C) 2015-2018 Antony Antony <antony@phenome.org>
 * Copyright (C) 2015-2019 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2017 Richard Guy Briggs <rgb@tricolour.ca>
 * Copyright (C) 2017 Vukasin Karadzic <vukasin.karadzic@gmail.com>
 * Copyright (C) 2017 Mayank Totale <mtotale@gmail.com>
 * Copyright (C) 2021 Paul Wouters <paul.wouters@aiven.io>
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
 *
 */

#include "defs.h"
#include "connections.h"
#include "state.h"
#include "log.h"
#include "kernel.h"
#include "show.h"

/* note: this mutates *st by calling get_sa_bundle_info */
static void jam_child_sa_traffic(struct jambuf *buf, struct child_sa *child)
{
	if (!pexpect(child != NULL)) {
		return;
	}

	jam_so(buf, child->sa.st_serialno);
	jam_string(buf, ": ");

	const struct connection *c = child->sa.st_connection;
	jam_connection(buf, c);

	if (child->sa.st_xauth_username[0] != '\0') {
		jam(buf, ", username=%s", child->sa.st_xauth_username);
	}

	/* traffic */
	jam(buf, ", type=%s, add_time=%"PRIu64,
	    (child->sa.st_esp.present ? "ESP" : child->sa.st_ah.present ? "AH" : child->sa.st_ipcomp.present ? "IPCOMP" : "UNKNOWN"),
	    child->sa.st_esp.add_time);

	struct ipsec_proto_info *first_ipsec_proto =
		(child->sa.st_esp.present ? &child->sa.st_esp:
		 child->sa.st_ah.present ? &child->sa.st_ah :
		 child->sa.st_ipcomp.present ? &child->sa.st_ipcomp :
		 NULL);
	passert(first_ipsec_proto != NULL);

	if (get_ipsec_traffic(child, first_ipsec_proto, DIRECTION_INBOUND)) {
		jam(buf, ", inBytes=%ju", first_ipsec_proto->inbound.bytes);
	}

	if (get_ipsec_traffic(child, first_ipsec_proto, DIRECTION_OUTBOUND)) {
		jam(buf, ", outBytes=%ju", first_ipsec_proto->outbound.bytes);
		if (c->config->sa_ipsec_max_bytes != 0) {
			jam_humber_uintmax(buf, ", maxBytes=", c->config->sa_ipsec_max_bytes, "B");
		}
	}

	if (child->sa.st_xauth_username[0] == '\0') {
		jam(buf, ", id='");
		jam_id_bytes(buf, &c->remote->host.id, jam_sanitized_bytes);
		jam(buf, "'");
	}

	/*
	 * Only one end can have a lease.
	 */
	FOR_EACH_THING(end,
		       /* "this" gave "that" a lease from "this"
			* address pool. */
		       &c->remote,
		       /* "this" received an internal address from
			* "that"; presumably from "that"'s address
			* pool. */
		       &c->local) {
		if (nr_child_leases(*end) > 0) {
			jam(buf, ", lease=");
			const char *sep = "";
			FOR_EACH_ELEMENT(lease, (*end)->child.lease) {
				if (lease->is_set) {
					jam_string(buf, sep); sep = ",";
					/* XXX: lease should be CIDR */
					ip_subnet s = subnet_from_address(*lease);
					jam_subnet(buf, &s);
				}
			}
		}
	}
}

static void show_state_traffic(struct show *s,
			       enum rc_type rc, struct state *st)
{
	if (IS_IKE_SA(st))
		return; /* ignore non-IPsec states */

	if (!IS_IPSEC_SA_ESTABLISHED(st))
		return; /* ignore non established states */

	/* whack-log-global - no prefix */
	SHOW_JAMBUF(rc, s, buf) {
		/* note: this mutates *st by calling get_sa_bundle_info */
		jam_child_sa_traffic(buf, pexpect_child_sa(st));
	}
}

static int show_newest_state_traffic(struct connection *c,
				     void *arg, struct logger *logger UNUSED)
{
	struct show *s = arg;
	struct state *st = state_by_serialno(c->newest_ipsec_sa);

	if (st == NULL)
		return 0;

	show_state_traffic(s, RC_INFORMATIONAL_TRAFFIC, st);
	return 1;
}

void show_traffic_status(struct show *s, const char *name)
{
	if (name == NULL) {
		struct state **array = sort_states(state_compare_serial, HERE);

		/* now print sorted results */
		if (array != NULL) {
			int i;
			for (i = 0; array[i] != NULL; i++) {
				show_state_traffic(s,
						   RC_INFORMATIONAL_TRAFFIC,
						   array[i]);
			}
			pfree(array);
		}
	} else {
		struct connection *c = conn_by_name(name, true/*strict*/);

		if (c != NULL) {
			/* cast away const sillyness */
			show_newest_state_traffic(c, s, show_logger(s));
		} else {
			/* cast away const sillyness */
			int count = foreach_connection_by_alias(name, show_newest_state_traffic,
								s, show_logger(s));
			if (count == 0) {
				/*
				 * XXX: don't bother implementing
				 * show_rc(...) - this is the only
				 * place where it would be useful.
				 */
				SHOW_JAMBUF(RC_UNKNOWN_NAME, s, buf) {
					jam(buf, "no such connection or aliased connection named \"%s\"", name);
				}
			}
		}
	}
}
