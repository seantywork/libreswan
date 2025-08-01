/* IKEv2 Configuration Payload, for Libreswan
 *
 * Copyright (C) 2007-2008 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2008-2011 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2008 Antony Antony <antony@xelerance.com>
 * Copyright (C) 2008-2009 David McCullough <david_mccullough@securecomputing.com>
 * Copyright (C) 2010,2012 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2010-2019 Tuomo Soini <tis@foobar.fi
 * Copyright (C) 2012-2019 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2012-2018 Antony Antony <antony@phenome.org>
 * Copyright (C) 2013-2019 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2013 David McCullough <ucdevel@gmail.com>
 * Copyright (C) 2013 Matt Rogers <mrogers@redhat.com>
 * Copyright (C) 2015-2019 Andrew Cagney <cagney@gnu.org>
 * Copyright (C) 2017-2018 Sahana Prasad <sahana.prasad07@gmail.com>
 * Copyright (C) 2017-2018 Vukasin Karadzic <vukasin.karadzic@gmail.com>
 * Copyright (C) 2017 Mayank Totale <mtotale@gmail.com>
 * Copyright (C) 2020 Yulia Kuzovkova <ukuzovkova@gmail.com>
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

#include "ip_info.h"

#include "defs.h"
#include "demux.h"
#include "connections.h"
#include "state.h"
#include "log.h"
#include "addresspool.h"
#include "ikev2_cp.h"
#include "orient.h"		/* for oriented() */

void ldbg_cp(struct logger *logger, const struct connection *cc, const char *fmt, ...)
{
	LDBGP_JAMBUF(DBG_BASE, logger, buf) {
		jam_string(buf, "CP: ");
		va_list ap;
		va_start(ap, fmt);
		jam_va_list(buf, fmt, ap);
		va_end(ap);
		FOR_EACH_THING(lr, LEFT_END, RIGHT_END) {
			const struct end_config *end = cc->end[lr].config;
			jam_string(buf, "; ");
			jam_string(buf, end->leftright);
			jam_string(buf, ":");
			if (end == cc->local->config) {
				jam_string(buf, " local");
			}
			if (end == cc->remote->config) {
				jam_string(buf, " remote");
			}
			if (end->host.modecfg.client) {
				jam_string(buf, " client");
			}
			if (end->host.modecfg.server) {
				jam_string(buf, " server");
			}
			if (end->child.addresspools.len > 0) {
				jam_string(buf, " addresspool");
			}
		}
		jam_string(buf, ";");
		if (cc->config->modecfg.domains != NULL) {
			jam_string(buf, " domains");
		}
		if (cc->config->modecfg.dns.len > 0) {
			jam_string(buf, " dns");
		}
		if (is_opportunistic(cc)) {
			jam_string(buf, " OE");
		}
	}
}

static bool need_v2CP_payload(const struct connection *const cc,
			      bool this_end_behind_nat)
{
	if (cc->local->host.config->modecfg.client &&
	    cc->local->child.config->has_client_address_translation &&
	    this_end_behind_nat) {
		return true;
	}
	if (cc->local->host.config->modecfg.client &&
	    !cc->local->child.config->has_client_address_translation) {
		return true;
	}
	return false;
}

bool need_v2CP_response(const struct connection *const cc,
			bool this_end_behind_nat)
{
	return need_v2CP_payload(cc, this_end_behind_nat);
}

bool send_v2CP_request(const struct connection *const cc,
		       bool this_end_behind_nat)
{
	bool send = (need_v2CP_payload(cc, this_end_behind_nat) ||
		     cc->config->modecfg.domains != NULL ||
		     cc->config->modecfg.dns.len > 0);
	ldbg_cp(cc->logger, cc, "send-v2CP=%s", bool_str(send));
	return send;
}

/* Misleading name, also used for NULL sized type's */
static bool emit_v2CP_attribute_address(uint16_t type, const ip_address *ip,
					const char *story, struct pbs_out *outpbs)
{
	struct pbs_out a_pbs;

	struct ikev2_cp_attribute attr = {
		.type = type,
	};

	if (!pbs_out_struct(outpbs, attr, &ikev2_cp_attribute_desc, &a_pbs)) {
		return false;
	}

	/* could be NULL */
	const struct ip_info *afi = address_type(ip);
	if (afi == NULL) {
		attr.len = 0;
	} else {
		attr.len = address_type(ip)->ip_size;
	}

	if (afi == &ipv6_info) {
		/* RFC hack to append 1-byte IPv6 prefix len */
		attr.len += sizeof(uint8_t);
	}

	if (attr.len > 0) {
		if (!pbs_out_address(&a_pbs, *ip, story)) {
			/* already logged */
			return false;
		}
	}

	if (afi == &ipv6_info) {
		uint8_t ipv6_prefix_len = IKEv2_INTERNAL_IP6_PREFIX_LEN; /*128*/
		if (!pbs_out_thing(&a_pbs, ipv6_prefix_len, "INTERNAL_IP6_PREFIX_LEN")) {
			/* already logged */
			return false;
		}
	}

	close_pbs_out(&a_pbs);
	return true;
}

static bool emit_v2CP_attribute(struct pbs_out *outpbs,
				uint16_t type, shunk_t attrib,
				const char *story)
{
	struct ikev2_cp_attribute attr = {
		.type = type,
		.len = attrib.len,
	};

	struct pbs_out a_pbs;
	if (!pbs_out_struct(outpbs, attr, &ikev2_cp_attribute_desc, &a_pbs)) {
		/* already logged */
		return false; /*fatal*/
	}

	if (attrib.len > 0) {
		if (!pbs_out_hunk(&a_pbs, attrib, story)) {
			/* already logged */
			return false;
		}
	}

	close_pbs_out(&a_pbs);
	return true;
}

/*
 * CHILD is negotiating configuration; hence log against child.
 */

bool emit_v2CP_response(const struct child_sa *child, struct pbs_out *outpbs)
{
	struct connection *c = child->sa.st_connection;
	struct pbs_out cp_pbs;
	struct ikev2_cp cp = {
		.isacp_critical = ISAKMP_PAYLOAD_NONCRITICAL,
		.isacp_type = IKEv2_CP_CFG_REPLY,
	};

	name_buf cpb;
	ldbg_cp(child->sa.logger, c,
		"send %s Configuration Payload",
		str_enum_long(&ikev2_cp_type_names, cp.isacp_type, &cpb));

	if (!pbs_out_struct(outpbs, cp, &ikev2_cp_desc, &cp_pbs))
		return false;

	FOR_EACH_ELEMENT(lease, c->remote->child.lease) {
		if (lease->ip.is_set) {
			const struct ip_info *lease_afi = address_type(lease);
			if (!emit_v2CP_attribute_address(lease_afi->ikev2_internal_address,
							 lease, "Internal IP Address", &cp_pbs)) {
				return false;
			}
		}
	}

	FOR_EACH_ITEM(dns, &c->config->modecfg.dns) {
		const struct ip_info *afi = address_type(dns);
		if (!emit_v2CP_attribute_address(afi->ikev2_internal_dns, dns,
						 "DNS", &cp_pbs)) {
			return false;
		}
	}

	for (const shunk_t *domain = c->config->modecfg.domains;
	     domain != NULL && domain->ptr != NULL; domain++) {
		if (!emit_v2CP_attribute(&cp_pbs,
					 IKEv2_INTERNAL_DNS_DOMAIN,
					 *domain,
					 "IKEv2_INTERNAL_DNS_DOMAIN")) {
			/* already logged */
			return false;
		}
	}

	close_pbs_out(&cp_pbs);
	return true;
}

bool emit_v2CP_request(const struct child_sa *child, struct pbs_out *outpbs)
{
	struct pbs_out cp_pbs;
	struct ikev2_cp cp = {
		.isacp_critical = ISAKMP_PAYLOAD_NONCRITICAL,
		.isacp_type = IKEv2_CP_CFG_REQUEST,
	};

	name_buf cpb;
	ldbg_cp(child->sa.logger, child->sa.st_connection,
		"emit %s Configuration Payload",
		str_enum_long(&ikev2_cp_type_names, cp.isacp_type, &cpb));

	if (!pbs_out_struct(outpbs, cp, &ikev2_cp_desc, &cp_pbs))
		return false;

	struct connection *cc = child->sa.st_connection;
	bool ask_for_ip[IP_VERSION_ROOF] = {0};
	bool ask_for_something = false;

	FOR_EACH_ITEM(proposed, &cc->local->child.selectors.proposed) {
		const struct ip_info *afi = selector_type(proposed);
		ldbg(child->sa.logger, "pool says to ask for %s", afi->ip_name);
		ask_for_ip[afi->ip.version] = true;
		ask_for_something |= ask_for_ip[afi->ip.version];
	}

	if (!ask_for_something) {
		llog_pexpect(child->sa.logger, HERE,
			     "can't figure out which internal address is needed");
		return false;
	}

	FOR_EACH_ELEMENT(afi, ip_families) {
		if (ask_for_ip[afi->ip.version]) {
			if (!emit_v2CP_attribute_address(afi->ikev2_internal_address,
							 NULL, "address", &cp_pbs) ||
			    !emit_v2CP_attribute_address(afi->ikev2_internal_dns,
							 NULL, "DNS", &cp_pbs)) {
				return false;
			}
		}
	}
	if (!emit_v2CP_attribute_address(IKEv2_INTERNAL_DNS_DOMAIN, NULL, "Domain", &cp_pbs)) {
		return false;
	}

	close_pbs_out(&cp_pbs);
	return true;
}

static bool lease_cp_address(struct child_sa *child, const struct ip_info *afi)
{
	struct connection *cc = child->sa.st_connection;
	const struct addresspool *pool = cc->pool[afi->ip.version];
	if (pool == NULL) {
		ldbg_sa(child, "ignoring %s address request, no pool",
			afi->ip_name);
		return true; /*non-fatal*/
	}

	ip_address assigned_address; /* ignore */
	diag_t d = assign_remote_lease(cc, /*xauth-username*/NULL, afi,
				       /*preferred-address*/unset_address,
				       &assigned_address,
				       child->sa.logger);
	if (d != NULL) {
		llog_sa(RC_LOG, child, "leasing %s address failed: %s",
			afi->ip_name, str_diag(d));
		pfree_diag(&d);
		return false; /*fatal*/
	}
	PASSERT(cc->logger, nr_child_leases(cc->remote) > 0); /* used below */
	return true;
}

bool process_v2_IKE_AUTH_request_v2CP_request_payload(struct ike_sa *ike,
						      struct child_sa *child,
						      struct payload_digest *cp_digest)
{
	struct connection *cc = child->sa.st_connection;
	pexpect(ike->sa.st_connection == cc);

	struct ikev2_cp *cp =  &cp_digest->payload.v2cp;
	struct pbs_in *cp_pbs = &cp_digest->pbs;

	ldbg_sa(child, "parsing ISAKMP_NEXT_v2CP payload");

	if (cp->isacp_type != IKEv2_CP_CFG_REQUEST) {
		name_buf cpb;
		llog_sa(RC_LOG, child,
			"ERROR: expected IKEv2_CP_CFG_REQUEST got a %s",
			str_enum_long(&ikev2_cp_type_names, cp->isacp_type, &cpb));
		return false;
	}

	while (pbs_left(cp_pbs) > 0) {

		struct ikev2_cp_attribute cp_attr;
		struct pbs_in cp_attr_pbs;
		diag_t d = pbs_in_struct(cp_pbs, &ikev2_cp_attribute_desc,
					 &cp_attr, sizeof(cp_attr), &cp_attr_pbs);
		if (d != NULL) {
			llog(RC_LOG, child->sa.logger, "ERROR: malformed CP attribute: %s", str_diag(d));
			pfree_diag(&d);
			return false;
		}

		enum ikev2_cp_attribute_type type = cp_attr.type;
		switch (type) {
		case IKEv2_INTERNAL_IP4_ADDRESS:
			if (!lease_cp_address(child, &ipv4_info)) {
				return false;
			}
			break;
		case IKEv2_INTERNAL_IP6_ADDRESS:
			if (!lease_cp_address(child, &ipv6_info)) {
				return false;
			}
			break;

		default:
		{
			name_buf eb;
			ldbg_sa(child, "ignoring attribute %s length %u",
				str_enum_short(&ikev2_cp_attribute_type_names, type, &eb),
				cp_attr.len);
			break;
		}
		}
	}

	if (nr_child_leases(cc->remote) == 0) {
		llog_sa(RC_LOG, child, "ERROR: no valid internal address request");
		return false;
	}

	set_child_has_client(cc, remote, true);

	/* rebuild the SPDs */
	discard_connection_spds(cc);
	PEXPECT(cc->logger, oriented(cc));
	build_connection_spds_from_proposals(cc);

	return true;
}

/*
 * Verify and save the INTERNAL_DNS_DOMAIN.
 *
 * THIS FUNCTION IS WRONG
 *
 * According to RFC 8598 (Split DNS Configuration for the Internet Key
 * Exchange Protocol Version 2 (IKEv2)) the payload contains:
 *
 *   o  Domain Name (0 or more octets) - A Fully Qualified Domain Name
 *      used for Split DNS rules, such as "example.com", in DNS
 *      presentation format and using an Internationalized Domain Names
 *      for Applications (IDNA) A-label [RFC5890].  Implementers need to
 *      be careful that this value is not null terminated.
 *
 * but the below is based on cisco_stringify() which is a hang-over
 * from IKEv1.
 */

static char *broken_dns_stringify(shunk_t str, bool *ok)
{
	char strbuf[500]; /* Cisco maximum unknown - arbitrary choice */
	struct jambuf buf = ARRAY_AS_JAMBUF(strbuf); /* let jambuf deal with overflow */

	/*
	 * detox string
	 */
	for (const char *p = (const void *)str.ptr, *end = p + str.len;
	     p < end && *p != '\0'; p++) {
		char c = *p;
		switch (c) {
		case '\'':
			/*
			 * preserve cisco_stringify() behaviour:
			 *
			 * ' is poison to the way this string will be
			 * used in system() and hence shell.  Remove
			 * any.
			 */
			jam(&buf, "?");
			break;
		case '\n':
		case '\r':
			/*
			 * preserve sanitize_string() behaviour:
			 *
			 * exception is that all vertical space just
			 * becomes white space
			 */
			jam(&buf, " ");
			break;
		default:
			/*
			 * preserve sanitize_string() behaviour:
			 *
			 * XXX: isprint() is wrong as it is affected
			 * by locale - need portable is printable
			 * ascii; is there something hiding in the
			 * x509 sources?
			 */
			if (c != '\\' && isprint(c)) {
				jam_char(&buf, c);
			} else {
				jam(&buf, "\\%03o", c);
			}
			break;
		}
	}
	(*ok) = jambuf_ok(&buf);
	return clone_str(strbuf, "INTERNAL_DNS_NAME");
}

static void ikev2_set_domain(struct pbs_in *cp_a_pbs, struct child_sa *child)
{
	if (child->sa.st_sa_role == SA_RESPONDER) {
		llog_sa(RC_LOG, child, "initiator INTERNAL_DNS_DOMAIN CP ignored");
		return;
	}

	/* must be initiator parsing CP response */
	append_st_cfg_domain(&child->sa, cp_a_pbs, broken_dns_stringify);
}

static diag_t ikev2_set_dns(struct pbs_in *cp_a_pbs, struct child_sa *child,
			    const struct ip_info *af)
{
	struct connection *c = child->sa.st_connection;

	if (is_opportunistic(c)) {
		llog_sa(RC_LOG, child,
			  "ignored INTERNAL_IP%s_DNS CP payload for Opportunistic IPsec",
			  af->n_name);
		return NULL;
	}

	bool responder = (child->sa.st_sa_role == SA_RESPONDER);
	if (responder) {
		llog_sa(RC_LOG, child,
			"initiator INTERNAL_IP%s_DNS CP ignored",
			af->n_name);
		return NULL;
	}

	return append_st_cfg_dns(cp_a_pbs, af, &child->sa);
}

static bool ikev2_set_internal_address(struct pbs_in *cp_a_pbs,
				       struct child_sa *child,
				       const struct ip_info *afi)
{
	struct connection *cc = child->sa.st_connection;
	struct child_end *local = &cc->local->child;

	ip_address ip;
	diag_t d = pbs_in_address(cp_a_pbs, &ip, afi, "INTERNAL_IP_ADDRESS");
	if (d != NULL) {
		llog(RC_LOG, child->sa.logger, "%s", str_diag(d));
		pfree_diag(&d);
		return false;
	}

	/*
	 * If (af->af == AF_INET6) pbs_in_address only reads 16 bytes.
	 * There should be one more byte in the pbs, 17th byte is
	 * prefix length.
	 */

	if (!address_is_specified(ip)) {
		address_buf ip_str;
		llog_sa(RC_LOG, child,
			  "ERROR INTERNAL_IP%s_ADDRESS %s is invalid",
			  afi->n_name, str_address(&ip, &ip_str));
		return false;
	}

	bool duplicate_lease = local->lease[afi->ip.version].ip.is_set;

	address_buf ip_str;
	llog_sa(RC_LOG, child,
		"received INTERNAL_IP%s_ADDRESS %s%s",
		afi->n_name, str_address(&ip, &ip_str),
		duplicate_lease ? "; discarded" : "");

	bool responder = (child->sa.st_sa_role == SA_RESPONDER);
	if (responder) {
		llog_sa(RC_LOG, child, "bogus responder CP ignored");
		return true;
	}

	if (duplicate_lease) {
		return true;
	}

	set_child_has_client(cc, local, true);
	local->lease[afi->ip.version] = ip;

	if (local->config->has_client_address_translation) {
		address_buf ipb;
		ldbg_sa(child,
			"CAT: configured, not setting host source IP address to %s local CAT = %s->true",
			str_address(&ip, &ipb), bool_str(local->has_cat));
		local->has_cat = true; /* create NAT entry */
		ip_address this_client_prefix = selector_prefix(cc->child.spds.list->local->client);
		if (address_eq_address(this_client_prefix, ip)) {
			/*
			 * The address we received is same as this
			 * side should we also check the host_srcip.
			 */
			address_buf ipb;
			ldbg(child->sa.logger,
			     "CAT: received INTERNAL_IP%s_ADDRESS that is same as this->client.addr %s. Will not add CAT rules",
			     afi->n_name, str_address(&ip, &ipb));
		} else {
			update_end_selector(cc, cc->local->config->index,
					    selector_from_address(ip),
					    "CAT: scribbling on end while ignoring TS");
		}
	} else if (connection_requires_tss(cc) == NULL) {
		update_end_selector(cc, cc->local->config->index,
				    selector_from_address(ip),
				    "CP scribbling on end while ignoring TS");
	} else {
		ldbg_sa(child, "leaving TS alone");
	}

	return true;
}

bool process_v2CP_response_payload(struct ike_sa *ike UNUSED, struct child_sa *child,
				   struct payload_digest *cp_pd)
{
	struct ikev2_cp *cp =  &cp_pd->payload.v2cp;
	struct connection *c = child->sa.st_connection;
	struct pbs_in *attrs = &cp_pd->pbs;

	ldbg(child->sa.logger, "parsing ISAKMP_NEXT_v2CP payload");

	switch (child->sa.st_sa_role) {
	case SA_INITIATOR:
		if (cp->isacp_type != IKEv2_CP_CFG_REPLY) {
			name_buf cpb;
			llog_sa(RC_LOG, child,
				"ERROR expected IKEv2_CP_CFG_REPLY got a %s",
				str_enum_long(&ikev2_cp_type_names, cp->isacp_type, &cpb));
			return false;
		}
		break;
	case SA_RESPONDER:
		if (cp->isacp_type != IKEv2_CP_CFG_REQUEST) {
			name_buf cpb;
			llog_sa(RC_LOG, child,
				"ERROR expected IKEv2_CP_CFG_REQUEST got a %s",
				str_enum_long(&ikev2_cp_type_names, cp->isacp_type, &cpb));
			return false;
		}
		break;
	default:
		bad_case(child->sa.st_sa_role);
	}

	/*
	 * Initialize connection fields that are no longer valid.  For
	 * instance, the instance connection is being re-directed or
	 * revived.
	 */
	FOR_EACH_ELEMENT(lease, c->local->child.lease) {
		if (lease->ip.is_set) {
			address_buf ab;
			ldbg(c->logger, "zapping lease %s", str_address(lease, &ab));
			zero(lease);
		}
	}

	while (pbs_left(attrs) > 0) {
		struct ikev2_cp_attribute cp_a;
		struct pbs_in cp_a_pbs;

		diag_t d = pbs_in_struct(attrs, &ikev2_cp_attribute_desc,
					 &cp_a, sizeof(cp_a), &cp_a_pbs);
		if (d != NULL) {
			llog(RC_LOG, child->sa.logger, "ERROR malformed CP attribute: %s", str_diag(d));
			pfree_diag(&d);
			return false;
		}

		switch (cp_a.type) {
		case IKEv2_INTERNAL_IP4_ADDRESS:
			if (!ikev2_set_internal_address(&cp_a_pbs, child, &ipv4_info)) {
				llog_sa(RC_LOG, child,
					  "ERROR malformed INTERNAL_IP4_ADDRESS attribute");
				return false;
			}
			break;

		case IKEv2_INTERNAL_IP4_DNS:
			d = ikev2_set_dns(&cp_a_pbs, child, &ipv4_info);
			if (d != NULL) {
				llog(RC_LOG, child->sa.logger, "ERROR %s", str_diag(d));
				pfree_diag(&d);
				return false;
			}
			break;

		case IKEv2_INTERNAL_IP6_ADDRESS:
			if (!ikev2_set_internal_address(&cp_a_pbs, child, &ipv6_info)) {
				llog_sa(RC_LOG, child,
					  "ERROR malformed INTERNAL_IP6_ADDRESS attribute");
				return false;
			}
			break;

		case IKEv2_INTERNAL_IP6_DNS:
			d = ikev2_set_dns(&cp_a_pbs, child, &ipv6_info);
			if (d != NULL) {
				llog(RC_LOG, child->sa.logger, "ERROR %s", str_diag(d));
				pfree_diag(&d);
				return false;
			}
			break;

		case IKEv2_INTERNAL_DNS_DOMAIN:
			ikev2_set_domain(&cp_a_pbs, child); /* can't fail */
			break;

		default:
		{
			name_buf tb;
			llog_sa(RC_LOG, child,
				"unknown attribute %s length %u",
				str_enum_long(&ikev2_cp_attribute_type_names, cp_a.type, &tb),
				cp_a.len);
			break;
		}
		}
	}
	return true;
}
