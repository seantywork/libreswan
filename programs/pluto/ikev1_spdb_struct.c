/* Security Policy Data Base (such as it is)
 *
 * Copyright (C) 1998-2001,2013 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2003-2007 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2012 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2003-2008,2012-2013,2019 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2013 Matt Rogers <mrogers@redhat.com>
 * Copyright (C) 2015-2019 Andrew Cagney <cagney@gnu.org>
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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "sysdep.h"
#include "constants.h"

#include "defs.h"
#include "id.h"
#include "x509.h"
#include "certs.h"
#include "connections.h"        /* needs id.h */
#include "state.h"
#include "packet.h"
#include "keys.h"
#include "kernel.h"     /* needs connections.h */
#include "log.h"
#include "whack.h"      /* for RC_LOG_SERIOUS */
#include "plutoalg.h"
#include "crypto.h"

#include "ikev1.h"
#include "kernel_alg.h"
#include "ike_alg.h"
#include "ike_alg_encrypt.h"
#include "ike_alg_integ.h"
#include "ike_alg_ipcomp.h"	/* for ike_alg_ipcomp_deflate */
#include "ikev1_db_ops.h"
#include "fips_mode.h" /* for is_fips_mode */
#include "crypt_prf.h"
#include "ikev1_message.h"
#include "ip_endpoint.h"
#include "nat_traversal.h"
#include "refcnt.h"		/* for dbg_alloc()+dbg_free() */

/** output an attribute (within an SA) */
/* Note: ikev2_out_attr is a clone, with the same bugs */
static bool out_attr(int type,
	      unsigned long val,
	      struct_desc *attr_desc,
	      enum_names *const *attr_val_descs,
	      struct pbs_out *pbs)
{
	if (val >> 16 == 0) {
		/* short value: use TV form */
		struct isakmp_attribute attr = {
			.isaat_af_type = type | ISAKMP_ATTR_AF_TV,
			.isaat_lv = val,
		};
		if (!out_struct(&attr, attr_desc, pbs, NULL))
			return false;
	} else {
		/* This is a real fudge!  Since we rarely use long attributes
		 * and since this is the only place where we can cause an
		 * ISAKMP message length to be other than a multiple of 4 octets,
		 * we force the length of the value to be a multiple of 4 octets.
		 * Furthermore, we only handle values up to 4 octets in length.
		 * Voila: a fixed format!
		 */
		struct pbs_out val_pbs;
		uint32_t nval = htonl(val);

		passert((type & ISAKMP_ATTR_AF_MASK) == 0);
		struct isakmp_attribute attr = {
			.isaat_af_type = type | ISAKMP_ATTR_AF_TLV,
			.isaat_lv = sizeof(nval),
		};
		if (!out_struct(&attr, attr_desc, pbs, &val_pbs) ||
		    !out_raw(&nval, sizeof(nval), &val_pbs,
			     "long attribute value"))
			return false;

		close_output_pbs(&val_pbs);
	}
	if (DBGP(DBG_BASE)) {
		enum_names *d = attr_val_descs[type];
		if (d != NULL) {
			esb_buf b;
			DBG_log("    [%lu is %s]", val, enum_show(d, val, &b));
		}
	}
	return true;
}

/*
 * Determine if the proposal is acceptable (or need to keep looking).
 *
 * As a rule, this doesn't log - instead it assumes things were
 * reported earlier.
 */

static bool ikev1_verify_esp(const struct connection *c,
			     const struct trans_attrs *ta,
			     struct logger *logger)
{
	if (PBAD(logger, c->config->child_sa.proposals.p == NULL)) {
		return false;
	}

	if (c->config->child_sa.encap_proto != ENCAP_PROTO_ESP) {
		dbg("ignoring ESP proposal as POLICY_ENCRYPT unset");
		return false;       /* try another */
	}

	/*
	 * Check encryption.
	 *
	 * For the key-length, its assumed that the caller checked for
	 * and patched up either a missing or zero key-length, setting
	 * .enckeylen to the correct value (which might still be 0).
	 */
	if (ta->ta_encrypt == NULL) {
		/*
		 * No encryption.  Either because its lookup failed or
		 * because it was NULLed to force the proposal's
		 * rejection.
		 */
		dbg(
		     "ignoring ESP proposal with NULLed or unknown encryption");
		return false;       /* try another */
	}
	if (!kernel_alg_encrypt_ok(ta->ta_encrypt)) {
		/*
		 * No kernel support.  Needed because ALG_INFO==NULL
		 * will act as a wild card.  XXX: but is ALG_INFO ever
		 * NULL?
		 */
		dbg("ignoring ESP proposal with alg %s not present in kernel",
		    ta->ta_encrypt->common.fqn);
		return false;
	}
	if (!encrypt_has_key_bit_length(ta->ta_encrypt, ta->enckeylen)) {
		llog(RC_LOG_SERIOUS, logger,
			    "kernel algorithm does not like: %s key_len %u is incorrect",
			    ta->ta_encrypt->common.fqn, ta->enckeylen);
		address_buf epb;
		llog(RC_LOG_SERIOUS, logger,
			    "unsupported ESP Transform %s from %s",
			    ta->ta_encrypt->common.fqn,
			    str_address_sensitive(&c->remote->host.addr, &epb));
		return false; /* try another */
	}

	/*
	 * Check integrity.
	 */
	if (ta->ta_integ == NULL) {
		/*
		 * No integrity.  Since, for ESP, when integrity is
		 * missing, it is forced to .ta_integ=NONE (i.e., not
		 * NULL), a NULL here must indicate that integrity was
		 * present but the lookup failed.
		 */
		dbg("ignoring ESP proposal with unknown integrity");
		return false;       /* try another */
	}

	if (ta->ta_integ != &ike_alg_integ_none && !kernel_alg_integ_ok(ta->ta_integ)) {
		/*
		 * No kernel support.  Needed because ALG_INFO==NULL
		 * will act as a wild card.
		 *
		 * XXX: but is ALG_INFO ever NULL?
		 *
		 * XXX: check for NONE comes from old code just
		 * assumed NONE was supported.
		 */
		dbg("ignoring ESP proposal with alg %s not present in kernel",
		    ta->ta_integ->common.fqn);
		return false;
	}

	if (ta->ta_integ != &ike_alg_integ_none && encrypt_desc_is_aead(ta->ta_encrypt)) {
		ldbg(logger, "ignoring ESP proposal AEAD alg %s with integrity %s",
		     ta->ta_encrypt->common.fqn, ta->ta_integ->common.fqn);
		return false;
	}

	/*
	 * Check for screwups.  Perhaps the parser rejcts this, anyone
	 * know?
	 */
	if (ta->ta_prf != NULL) {
		llog_pexpect(logger, HERE, "ESP IPsec Transform refused: contains unexpected PRF %s",
			     ta->ta_prf->common.fqn);
		return false;
	}
	if (ta->ta_dh != NULL) {
		llog_pexpect(logger, HERE, "ESP IPsec Transform refused: contains unexpected DH %s",
			     ta->ta_dh->common.fqn);
		return false;
	}

	FOR_EACH_PROPOSAL(c->config->child_sa.proposals.p, proposal) {
		struct v1_proposal algs = v1_proposal(proposal);
		if (algs.encrypt == ta->ta_encrypt &&
		    (algs.enckeylen == 0 ||
		     ta->enckeylen == 0 ||
		     algs.enckeylen == ta->enckeylen) &&
		    algs.integ == ta->ta_integ) {
			dbg("ESP IPsec Transform verified; matches alg_info entry");
			return true;
		}
	}
	return false;
}

static bool ikev1_verify_ah(const struct connection *c,
			    const struct trans_attrs *ta,
			    struct logger *logger)
{
	if (PBAD(logger, c->config->child_sa.proposals.p == NULL)) {
		return false;
	}

	if (c->config->child_sa.encap_proto != ENCAP_PROTO_AH) {
		dbg("ignoring AH proposal as POLICY_AUTHENTICATE unset");
		return false;       /* try another */
	}
	if (ta->ta_encrypt != NULL) {
		llog_pexpect(logger, HERE,
			     "AH IPsec Transform refused: contains unexpected encryption %s",
			     ta->ta_encrypt->common.fqn);
		return false;
	}
	if (ta->ta_prf != NULL) {
		llog_pexpect(logger, HERE, "AH IPsec Transform refused: contains unexpected PRF %s",
			     ta->ta_prf->common.fqn);
		return false;
	}
	if (ta->ta_integ == NULL) {
		llog_pexpect(logger, HERE, "AH IPsec Transform refused: missing integrity algorithm");
		return false;
	}
	if (ta->ta_dh != NULL) {
		llog_pexpect(logger, HERE, "AH IPsec Transform refused: contains unexpected DH %s",
			     ta->ta_dh->common.fqn);
		return false;
	}

	FOR_EACH_PROPOSAL(c->config->child_sa.proposals.p, proposal) {	/* really AH */
		struct v1_proposal algs = v1_proposal(proposal);
		if (algs.integ == ta->ta_integ) {
			dbg("ESP IPsec Transform verified; matches alg_info entry");
			return true;
		}
	}

	llog(RC_LOG, logger, "AH IPsec Transform refused: %s",
		    ta->ta_integ->common.fqn);
	return false;
}

/*
 * IKEv1 kernel algorithm database.
 */

static bool kernel_alg_db_add(struct db_context *db_ctx,
			      enum encap_proto encap_proto,
			      const struct proposal *proposal,
			      struct logger *logger)
{
	enum ipsec_cipher_algo ealg_i = ESP_reserved;

	struct v1_proposal algs = v1_proposal(proposal);
	if (encap_proto == ENCAP_PROTO_ESP) {
		ealg_i = algs.encrypt->common.id[IKEv1_ESP_ID];
		/* already checked by the parser? */
		if (!kernel_alg_encrypt_ok(algs.encrypt)) {
			llog(RC_LOG_SERIOUS, logger,
			     "requested kernel enc ealg_id=%d not present",
			     ealg_i);
			return false;
		}
	}

	int aalg_i = algs.integ->integ_ikev1_ah_transform;

	/* already checked by the parser? */
	if (!kernel_alg_integ_ok(algs.integ)) {
		dbg("kernel_alg_db_add() kernel auth aalg_id=%d not present", aalg_i);
		return false;
	}

	if (encap_proto == ENCAP_PROTO_ESP) {
		/*open new transformation */
		db_trans_add(db_ctx, ealg_i);

		/* add ESP auth attr (if present) */
		if (algs.integ != &ike_alg_integ_none) {
			db_attr_add_values(db_ctx,
					   AUTH_ALGORITHM,
					   algs.integ->common.id[IKEv1_ESP_ID]);
		}

		/* add keylength if specified in esp= string */
		if (algs.enckeylen != 0) {
			db_attr_add_values(db_ctx,
					   KEY_LENGTH,
					   algs.enckeylen);
		} else {
			/* no key length - if required add default here and add another max entry */
			int def_ks = (algs.encrypt->keylen_omitted ? 0
				      : algs.encrypt->keydeflen);

			if (def_ks != 0) {
				db_attr_add_values(db_ctx, KEY_LENGTH, def_ks);
				/* add this trans again with max key size */
				int max_ks = encrypt_max_key_bit_length(algs.encrypt);
				if (def_ks != max_ks) {
					db_trans_add(db_ctx, ealg_i);
					if (algs.integ != &ike_alg_integ_none) {
						db_attr_add_values(db_ctx,
							AUTH_ALGORITHM,
							algs.integ->common.id[IKEv1_ESP_ID]);
					}
					db_attr_add_values(db_ctx,
							   KEY_LENGTH,
							   max_ks);
				}
			}
		}
	} else if (encap_proto == ENCAP_PROTO_AH) {
		/* open new transformation */
		db_trans_add(db_ctx, aalg_i);

		/* add ESP auth attr */
		db_attr_add_values(db_ctx, AUTH_ALGORITHM,
				   algs.integ->common.id[IKEv1_ESP_ID]);
	}

	return true;
}

/*
 *	Create proposal with runtime kernel algos, merging
 *	with passed proposal if not NULL
 *
 * ??? is this still true?  Certainly not free(3):
 *	for now this function does free() previous returned
 *	malloced pointer (this quirk allows easier spdb.c change)
 */
static struct db_context *kernel_alg_db_new(enum encap_proto encap_proto,
					    struct child_proposals proposals,
					    struct logger *logger)
{
	unsigned int trans_cnt = 0;
	int protoid = PROTO_RESERVED;

	if (encap_proto == ENCAP_PROTO_ESP) {
		trans_cnt = kernel_alg_encrypt_count() * kernel_alg_integ_count();
		protoid = PROTO_IPSEC_ESP;
	} else if (encap_proto == ENCAP_PROTO_AH) {
		trans_cnt = kernel_alg_integ_count();
		protoid = PROTO_IPSEC_AH;
	}

	dbg("%s() initial trans_cnt=%d", __func__, trans_cnt);

	/*	pass aprox. number of transforms and attributes */
	struct db_context *ctx_new = db_prop_new(protoid, trans_cnt, trans_cnt * 2);

	/*
	 *      Loop: for each element (struct esp_info) of
	 *      proposals, if kernel support is present then
	 *      build the transform (and attrs)
	 *
	 *      if NULL proposals, propose everything ...
	 */

	bool success = true;
	if (proposals.p != NULL) {
		FOR_EACH_PROPOSAL(proposals.p, proposal) {
			LDBGP_JAMBUF(DBG_BASE, &global_logger, buf) {
				jam_string(buf, "adding proposal: ");
				jam_proposal(buf, proposal);
			}
			if (!kernel_alg_db_add(ctx_new, encap_proto, proposal, logger))
				success = false;	/* ??? should we break? */
		}
	} else {
		llog_pexpect(logger, HERE, "%s", "proposals should be non-NULL");
	}

	if (!success) {
		/* NO algorithms were found. oops */
		db_destroy(ctx_new);
		return NULL;
	}

	struct db_prop *prop = db_prop_get(ctx_new);

	dbg("%s() will return p_new->protoid=%d, p_new->trans_cnt=%d",
	    __func__, prop->protoid, prop->trans_cnt);

	unsigned int tn = 0;
	struct db_trans *t;
	for (t = prop->trans, tn = 0;
	     t != NULL && t[tn].transid != 0 && tn < prop->trans_cnt;
	     tn++) {
		dbg("%s()     trans[%d]: transid=%d, attr_cnt=%d, attrs[0].type=%d, attrs[0].val=%d",
		    __func__, tn,
		    t[tn].transid, t[tn].attr_cnt,
		    t[tn].attrs ? t[tn].attrs[0].type.ipsec : 255,
		    t[tn].attrs ? t[tn].attrs[0].val : 255);
	}
	prop->trans_cnt = tn;

	return ctx_new;
}

static struct db_sa *v1_kernel_alg_makedb(enum encap_proto encap_proto,
					  struct child_proposals proposals,
					  bool compress,
					  struct logger *logger)
{
	PASSERT(logger, proposals.p != NULL);
	struct db_context *dbnew = kernel_alg_db_new(encap_proto, proposals, logger);

	if (dbnew == NULL) {
		llog(RC_LOG, logger, "failed to translate esp_info to proposal, returning empty");
		return NULL;
	}

	struct db_prop *p = db_prop_get(dbnew);

	if (p == NULL) {
		llog(RC_LOG, logger, "failed to get proposal from context, returning empty");
		db_destroy(dbnew);
		return NULL;
	}

	struct db_prop_conj pc = { .prop_cnt = 1, .props = p };

	struct db_sa t = { .prop_conj_cnt = 1, .prop_conjs = &pc };

	/* make a fresh copy */
	struct db_sa *n = sa_copy_sa(&t, HERE);
	n->parentSA = false;

	db_destroy(dbnew);

	/* add IPcomp proposal if policy asks for it */

	if (compress) {
		struct db_trans *ipcomp_trans = alloc_thing(struct db_trans, "ipcomp_trans");

		/* allocate space for 2 proposals */
		struct db_prop *ipcomp_prop =
			alloc_bytes(sizeof(struct db_prop) * 2,
				    "ipcomp_prop");

		passert(n->prop_conjs->prop_cnt == 1);

		/* construct the IPcomp proposal */
		ipcomp_trans->transid = IPCOMP_DEFLATE;
		ipcomp_trans->attrs = NULL;
		ipcomp_trans->attr_cnt = 0;

		/* copy the original proposal */
		ipcomp_prop[0].protoid   =
			n->prop_conjs->props->
			protoid;
		ipcomp_prop[0].trans     =
			n->prop_conjs->props->trans;
		ipcomp_prop[0].trans_cnt =
			n->prop_conjs->props->
			trans_cnt;

		/* and add our IPcomp proposal */
		ipcomp_prop[1].protoid = PROTO_IPCOMP;
		ipcomp_prop[1].trans = ipcomp_trans;
		ipcomp_prop[1].trans_cnt = 1;

		/* free the old proposal, and ... */
		pfree(n->prop_conjs->props);

		/* ... use our new one instead */
		n->prop_conjs->props = ipcomp_prop;
		n->prop_conjs->prop_cnt += 1;
	}

	dbg("returning new proposal from esp_info");
	return n;
}

/**
 * Output an SA, as described by a db_sa.
 * This has the side-effect of allocating SPIs for us.
 *
 */

/* Security Policy Data Base (such as it is)
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

/*
 * empty structure, for clone use.
 */
static struct db_attr otempty[] = {
	{ .type.oakley = OAKLEY_ENCRYPTION_ALGORITHM, -1 },
	{ .type.oakley = OAKLEY_HASH_ALGORITHM,       -1 },
	{ .type.oakley = OAKLEY_AUTHENTICATION_METHOD, -1 },
	{ .type.oakley = OAKLEY_GROUP_DESCRIPTION,    -1 },
	{ .type.oakley = OAKLEY_KEY_LENGTH,    -1 },
};

static struct db_trans oakley_trans_empty[] = {
	{ AD_TR(KEY_IKE, otempty) },
};

static struct db_prop oakley_pc_empty[] =
	{ { AD_PR(PROTO_ISAKMP, oakley_trans_empty) } };

static struct db_prop_conj oakley_props_empty[] =
	{ { AD_PC(oakley_pc_empty) } };

static struct db_sa oakley_empty = { AD_SAp(oakley_props_empty) };

static struct db_sa *oakley_alg_mergedb(struct ike_proposals ike_proposals,
					enum ikev1_auth_method auth_method,
					bool single_dh, struct logger *logger)
{
	passert(ike_proposals.p != NULL);

	struct db_sa *gsp = NULL;

	/* Next two are for multiple proposals in aggressive mode... */
	unsigned last_modp = 0;
	bool warned_dropped_dhgr = false;

	int transcnt = 0;

	/*
	 * for each group, we will create a new proposal item, and then
	 * append it to the list of transforms in the conjoint point.
	 *
	 * when creating each item, we will use the first transform
	 * from the base item as the template.
	 */
	FOR_EACH_PROPOSAL(ike_proposals.p, proposal) {
		struct v1_proposal algs = v1_proposal(proposal);

		passert(algs.encrypt != NULL);
		passert(algs.prf != NULL);
		passert(algs.dh != NULL);

		unsigned ealg = algs.encrypt->common.ikev1_oakley_id;
		unsigned halg = algs.prf->common.ikev1_oakley_id;
		unsigned modp = algs.dh->group;
		unsigned eklen = algs.enckeylen;

		dbg("oakley_alg_makedb() processing ealg=%s=%u halg=%s=%u modp=%s=%u eklen=%u",
		    algs.encrypt->common.fqn, ealg,
		    algs.prf->common.fqn, halg,
		    algs.dh->common.fqn, modp,
		    eklen);

		const struct encrypt_desc *enc_desc = algs.encrypt;
		if (eklen != 0 && !encrypt_has_key_bit_length(enc_desc, eklen)) {
			llog_pexpect(logger, HERE,
				     "IKEv1 proposal with ENCRYPT%s (specified) keylen:%d, not valid, should have been dropped",
				     enc_desc->common.fqn,
				     eklen);
			continue;
		}

		/*
		 * copy the template
		 */
		struct db_sa *emp_sp = sa_copy_sa(&oakley_empty, HERE);
		passert(emp_sp->dynamic);
		passert(emp_sp->prop_conj_cnt == 1);
		passert(emp_sp->prop_conjs[0].prop_cnt == 1);
		passert(emp_sp->prop_conjs[0].props[0].trans_cnt == 1);
		struct db_trans *trans = &emp_sp->prop_conjs[0].props[0].trans[0];
		passert(trans->attr_cnt == 5);

		/*
		 * See "struct db_attr otempty" above, and spdb.c, for
		 * where these magic values come from.
		 */
		struct db_attr *enc  = &trans->attrs[0];
		struct db_attr *hash = &trans->attrs[1];
		struct db_attr *auth = &trans->attrs[2];
		struct db_attr *grp  = &trans->attrs[3];

		/*
		 * auth type for IKE must be set.
		 */
		passert(auth->type.oakley == OAKLEY_AUTHENTICATION_METHOD);
		auth->val = auth_method;

		if (eklen > 0) {
			struct db_attr *enc_keylen = &trans->attrs[4];

			passert(trans->attr_cnt == 5);
			passert(enc_keylen->type.oakley == OAKLEY_KEY_LENGTH);
			enc_keylen->val = eklen;
		} else {
			/* truncate */
			trans->attr_cnt = 4;
		}

		passert(enc->type.oakley == OAKLEY_ENCRYPTION_ALGORITHM);
		if (ealg > 0)
			enc->val = ealg;

		/*
		 * Either pass a hash algorithm or a PRF.
		 *
		 * Since AEAD algorithms don't need the hash,
		 * but do need a PRF, the hash field can be
		 * re-purposed as a PRF field.
		 *
		 * [cagney] While I suspect that type will
		 * never initially be OAKLEY_PRF (it is
		 * initialized using "struct db_attr otempty")
		 * it doesn't hurt to be safe.
		 */
		passert(hash->type.oakley == OAKLEY_HASH_ALGORITHM ||
			hash->type.oakley == OAKLEY_PRF);
		if (halg > 0) {
			hash->val = halg;
			if (encrypt_desc_is_aead(enc_desc)) {
				hash->type.oakley = OAKLEY_PRF;
			} else {
				hash->type.oakley = OAKLEY_HASH_ALGORITHM;
			}
		}

		passert(grp->type.oakley == OAKLEY_GROUP_DESCRIPTION);
		if (modp > 0)
			grp->val = modp;

		/*
		 * Aggressive mode really only works with a single DH group.
		 * If this is for Aggressive Mode, and we've previously seen
		 * a different DH group, we try to deal with this.
		 */
		if (single_dh && transcnt > 0 &&
		    algs.dh->group != last_modp) {
			if (
#ifdef USE_DH2
			    last_modp == OAKLEY_GROUP_MODP1024 ||
#endif
			    last_modp == OAKLEY_GROUP_MODP1536) {
				/*
				 * The previous group will work on old Cisco gear,
				 * so we can discard this one.
				 */

				if (!warned_dropped_dhgr) {
					/* complain only once */
					llog(RC_LOG_SERIOUS, logger,
						    "multiple DH groups were set in aggressive mode. Only first one used.");
				}

				llog(RC_LOG_SERIOUS, logger,
					    "transform (%s,%s,%s keylen %d) ignored.",
					    enum_name(&oakley_enc_names,
						      algs.encrypt->common.ikev1_oakley_id),
					    enum_name(&oakley_hash_names,
						      algs.prf->common.ikev1_oakley_id),
					    algs.dh->common.fqn,
					    algs.enckeylen);
				free_sa(&emp_sp);
			} else {
				/*
				 * The previous group won't work on old Cisco gear,
				 * so we discard the previous ones.
				 * Of course this modp might be just as bad;
				 * we won't look until the next one comes along.
				 *
				 * Lemma: there will be only a single previous
				 * one in gsp (any others were discarded).
				 */
				llog(RC_LOG_SERIOUS, logger,
					    "multiple DH groups in aggressive mode can cause interop failure");
				llog(RC_LOG_SERIOUS, logger,
					    "Deleting previous proposal in the hopes of selecting DH 2 or DH 5");

				free_sa(&gsp);
			}

			warned_dropped_dhgr = true;
		}

		if (emp_sp != NULL) {
			/*
			 * Exclude 3des et.al. which do not include
			 * default key lengths in the proposal.
			 */
			if (algs.enckeylen == 0 &&
			    !algs.encrypt->keylen_omitted) {
				const struct encrypt_desc *enc_desc = algs.encrypt;
				int def_ks = enc_desc->keydeflen;
				passert(def_ks); /* ike=null not supported */
				int max_ks = encrypt_max_key_bit_length(enc_desc);
				int ks;

				passert(emp_sp->dynamic);
				passert(emp_sp->prop_conj_cnt == 1);
				passert(emp_sp->prop_conjs[0].prop_cnt == 1);
				passert(emp_sp->prop_conjs[0].props[0].trans_cnt == 1);

				if (emp_sp->prop_conjs[0].props[0].trans[0].attr_cnt == 4) {
					/* add a key length attribute of 0 */
					struct db_trans *tr = &emp_sp->prop_conjs[0].props[0].trans[0];
					const int n = tr->attr_cnt;	/* 4, actually */
					struct db_attr *old_attrs = tr->attrs;
					struct db_attr *new_attrs = alloc_bytes(
						(n + 1) * sizeof(old_attrs[0]),
						"extended trans");

					passert(emp_sp->dynamic);
					passert(old_attrs[0].type.oakley != OAKLEY_KEY_LENGTH &&
						old_attrs[1].type.oakley != OAKLEY_KEY_LENGTH &&
						old_attrs[2].type.oakley != OAKLEY_KEY_LENGTH &&
						old_attrs[3].type.oakley != OAKLEY_KEY_LENGTH);
					memcpy(new_attrs, old_attrs, n * sizeof(old_attrs[0]));
					new_attrs[n].type.oakley = OAKLEY_KEY_LENGTH;
					new_attrs[n].val = 0;

					pfree(old_attrs);
					tr->attrs = new_attrs;
					tr->attr_cnt++;
				}
				passert(emp_sp->prop_conjs[0].props[0].trans[0].attr_cnt == 5);
				passert(emp_sp->prop_conjs[0].props[0].trans[0].attrs[4].type.oakley == OAKLEY_KEY_LENGTH);

				/*
				 * This odd FOR loop executes its body for
				 * exactly two values of ks (max_ks and def_ks)
				 * unless def_ks == max_ks, in which case it is
				 * executed once.
				 */
				for (ks = max_ks; ; ks = def_ks) {
					emp_sp->prop_conjs[0].props[0].trans[0].attrs[4].val = ks;

					if (gsp == NULL) {
						gsp = sa_copy_sa(emp_sp, HERE);
					} else {
						struct db_sa *new = sa_merge_proposals(gsp, emp_sp);

						free_sa(&gsp);
						gsp = new;
					}
					if (ks == def_ks)
						break;
				}
				free_sa(&emp_sp);
			} else {
				if (gsp != NULL) {
					/* now merge emp_sa and gsp */
					struct db_sa *new = sa_merge_proposals(gsp, emp_sp);

					free_sa(&gsp);
					free_sa(&emp_sp);
					gsp = new;
				} else {
					gsp = emp_sp;
					emp_sp = NULL;
				}
			}
			last_modp = algs.dh->group;
		}

		pexpect(emp_sp == NULL);
		transcnt++;
	}

	if (gsp != NULL)
		gsp->parentSA = true;

	dbg("oakley_alg_makedb() returning %p", gsp);
	return gsp;
}

static bool ikev1_out_sa(struct pbs_out *outs,
			 const struct db_sa *sadb,
			 struct state *st,
			 bool oakley_mode)
{
	struct connection *c = st->st_connection;

	/* SA header out */
	struct pbs_out sa_pbs;
	{
		struct isakmp_sa sa = {
			.isasa_doi = ISAKMP_DOI_IPSEC /* all we know */
		};
		if (!out_struct(&sa, &isakmp_sa_desc, outs, &sa_pbs))
			goto fail;
	}

	/* within SA: situation out */
	{
		static const uint32_t situation = SIT_IDENTITY_ONLY;

		if (!out_struct(&situation, &ipsec_sit_desc, &sa_pbs, NULL))
			goto fail;
	}

	/* within SA: Proposal Payloads
	 *
	 * Multiple Proposals with the same number are simultaneous
	 * (conjuncts) and must deal with different protocols (AH or ESP).
	 * Proposals with different numbers are alternatives (disjuncts),
	 * in preference order.
	 * Proposal numbers must be monotonic.
	 * See RFC 2408 "ISAKMP" 4.2
	 */

	bool ah_spi_generated = false,
	     esp_spi_generated = false,
	     ipcomp_cpi_generated = false;

	for (unsigned pcn = 0; pcn < sadb->prop_conj_cnt; pcn++) {
		const struct db_prop_conj *const pc = &sadb->prop_conjs[pcn];
		int valid_prop_cnt = pc->prop_cnt;

		dbg("%s() pcn: %d has %d valid proposals", __func__, pcn, valid_prop_cnt);

		for (unsigned pn = 0; pn < pc->prop_cnt; pn++) {
			const struct db_prop *const p = &pc->props[pn];
			struct pbs_out proposal_pbs;

			/*
			 * pick the part of the proposal we are trying to work on
			 */

			dbg("%s() pcn: %d pn: %d<%d valid_count: %d trans_cnt: %d",
			    __func__, pcn, pn, pc->prop_cnt, valid_prop_cnt,
			    p->trans_cnt);

			/* but, skip things if the transform count is zero */
			if (p->trans_cnt == 0)
				continue;

			/* Proposal header */
			{
				valid_prop_cnt--;

				struct isakmp_proposal proposal = {
					.isap_proposal = pcn,
					.isap_protoid = p->protoid,
					.isap_spisize = oakley_mode ? 0 :
							p->protoid == PROTO_IPCOMP ?
							  IPCOMP_CPI_SIZE :
							  IPSEC_DOI_SPI_SIZE,
					.isap_pnp = valid_prop_cnt > 0 ?
						ISAKMP_NEXT_P : ISAKMP_NEXT_NONE,
					.isap_notrans = p->trans_cnt
				};

				if (!out_struct(&proposal, &isakmp_proposal_desc,
						&sa_pbs, &proposal_pbs))
					goto fail;
			}

			/* Per-protocols stuff:
			 * Set trans_desc.
			 * Set attr_desc.
			 * Set attr_val_descs.
			 * If not oakley_mode, emit SPI.
			 * We allocate SPIs on demand.
			 * All ESPs in an SA will share a single SPI.
			 * All AHs in an SAwill share a single SPI.
			 * AHs' SPI will be distinct from ESPs'.
			 * ??? If multiple ESPs are composed, how should their SPIs
			 * be allocated?
			 */
			const struct_desc *trans_desc;
			const struct_desc *attr_desc;
			enum_names *const *attr_val_descs;

			{
				ipsec_spi_t *spi_ptr = NULL;
				const struct ip_protocol *proto = NULL;
				bool *spi_generated = NULL;

				switch (p->protoid) {
				case PROTO_ISAKMP:
					passert(oakley_mode);
					trans_desc =
						&isakmp_isakmp_transform_desc;
					attr_desc =
						&isakmp_oakley_attribute_desc;
					attr_val_descs = oakley_attr_val_descs;
					/* no SPI needed */
					break;

				case PROTO_IPSEC_AH:
					passert(!oakley_mode);
					trans_desc = &isakmp_ah_transform_desc;
					attr_desc =
						&isakmp_ipsec_attribute_desc;
					attr_val_descs = ipsec_attr_val_descs;
					spi_ptr = &st->st_ah.inbound.spi;
					spi_generated = &ah_spi_generated;
					proto = &ip_protocol_ah;
					break;

				case PROTO_IPSEC_ESP:
					passert(!oakley_mode);
					trans_desc =
						&isakmp_esp_transform_desc;
					attr_desc =
						&isakmp_ipsec_attribute_desc;
					attr_val_descs = ipsec_attr_val_descs;
					spi_ptr = &st->st_esp.inbound.spi;
					spi_generated = &esp_spi_generated;
					proto = &ip_protocol_esp;
					break;

				case PROTO_IPCOMP:
					passert(!oakley_mode);
					trans_desc =
						&isakmp_ipcomp_transform_desc;
					attr_desc =
						&isakmp_ipsec_attribute_desc;
					attr_val_descs = ipsec_attr_val_descs;

					/*
					 * a CPI isn't quite the same as an SPI
					 * so we use specialized code to emit it.
					 */
					if (!ipcomp_cpi_generated) {
						st->st_ipcomp.inbound.spi =
							get_ipsec_cpi(c, st->logger);
						if (st->st_ipcomp.inbound.spi == 0)
							goto fail; /* problem generating CPI */

						ipcomp_cpi_generated = true;
					}
					/*
					 * CPI is stored in network low order end of an
					 * ipsec_spi_t.  So we start a couple of bytes in.
					 */
					if (!out_raw((uint8_t *)&st->st_ipcomp.inbound.spi +
						     IPSEC_DOI_SPI_SIZE -
						     IPCOMP_CPI_SIZE,
						     IPCOMP_CPI_SIZE,
						     &proposal_pbs, "CPI"))
						goto fail;
					break;

				default:
					bad_case(p->protoid);
				}

				if (spi_ptr != NULL) {
					if (!*spi_generated) {
						*spi_ptr = get_ipsec_spi(c, proto, 0,
									 st->logger);
						*spi_generated = true;
					}
					if (!out_raw((uint8_t *)spi_ptr,
						     IPSEC_DOI_SPI_SIZE,
						     &proposal_pbs, "SPI"))
						goto fail;
				}
			}

			/* within proposal: Transform Payloads */
			for (unsigned tn = 0; tn != p->trans_cnt; tn++) {
				const struct db_trans *const t = &p->trans[tn];

				struct pbs_out trans_pbs;

				{
					const struct isakmp_transform trans = {
						.isat_tnp = (tn == p->trans_cnt - 1) ?
							ISAKMP_NEXT_NONE :
							ISAKMP_NEXT_T,
						.isat_transnum = tn,
						.isat_transid = t->transid
					};

					if (!out_struct(&trans, trans_desc,
							&proposal_pbs, &trans_pbs))
						goto fail;
				}

				/* Within transform: Attributes. */

				/* For Phase 2 / Quick Mode, GROUP_DESCRIPTION is
				 * automatically generated because it must be the same
				 * in every transform.  Except IPCOMP.
				 */
				if (p->protoid != PROTO_IPCOMP &&
				    st->st_pfs_group != NULL) {
					passert(!oakley_mode);
					passert(st->st_pfs_group != &unset_group);
					if (!out_attr(GROUP_DESCRIPTION,
						      st->st_pfs_group->group,
						      attr_desc,
						      attr_val_descs,
						      &trans_pbs))
						goto fail;
				}

				/* automatically generate duration
				 * and, for Phase 2 / Quick Mode, encapsulation.
				 */
				if (oakley_mode) {
					if (!out_attr(OAKLEY_LIFE_TYPE,
						      OAKLEY_LIFE_SECONDS,
						      attr_desc,
						      attr_val_descs,
						      &trans_pbs) ||
					    !out_attr(OAKLEY_LIFE_DURATION,
						      deltasecs(c->config->sa_ike_max_lifetime),
						      attr_desc,
						      attr_val_descs,
						      &trans_pbs))
						goto fail;
				} else {
					/* RFC 2407 (IPSEC DOI) 4.5 specifies that
					 * the default is "unspecified (host-dependent)".
					 * This makes little sense, so we always specify it.
					 *
					 * Unlike other IPSEC transforms, IPCOMP defaults
					 * to Transport Mode, so we can exploit the default
					 * (draft-shacham-ippcp-rfc2393bis-05.txt 4.1).
					 */
					if (p->protoid != PROTO_IPCOMP ||
					    st->st_policy & POLICY_TUNNEL) {
						if (!out_attr(
							    ENCAPSULATION_MODE,
							    NAT_T_ENCAPSULATION_MODE(
								    st,
								    st->st_policy),
							    attr_desc,
							    attr_val_descs,
							    &trans_pbs))
							goto fail;
					}
					if (!out_attr(SA_LIFE_TYPE,
						      SA_LIFE_TYPE_SECONDS,
						      attr_desc,
						      attr_val_descs,
						      &trans_pbs) ||
					    !out_attr(SA_LIFE_DURATION,
						      deltasecs(c->config->sa_ipsec_max_lifetime),
						      attr_desc,
						      attr_val_descs,
						      &trans_pbs))
						goto fail;
				}

				/*
				 * spit out attributes from table
				 *
				 * XXX: Assume that the code
				 * constructing the attribute table
				 * handled optional and extra key
				 * lengths (and if it is wrong it is
				 * deliberate).  I.e., don't try to
				 * also handle it here.
				 *
				 * OTOH, do completely override
				 * key-lengths when so impaired.
				 */
				enum impair_emit impair_key_length_attribute =
					(oakley_mode ? impair.ike_key_length_attribute
					 : impair.child_key_length_attribute);
				long key_length_to_impair = -1;
				for (unsigned an = 0; an != t->attr_cnt; an++) {
					const struct db_attr *a = &t->attrs[an];
					/*
					 * Strip out or duplicate
					 * key-length attribute?
					 */
					if (impair_key_length_attribute > 0 &&
					    (oakley_mode ? a->type.oakley == OAKLEY_KEY_LENGTH
					     :  a->type.ipsec == KEY_LENGTH)) {
						key_length_to_impair = a->val;
						log_state(RC_LOG, st, "IMPAIR: stripping key-length");
						continue;
					}
					if (!out_attr(oakley_mode ? a->type.oakley : a->type.ipsec ,
						      a->val,
						      attr_desc,
						      attr_val_descs,
						      &trans_pbs))
						goto fail;
				}
				/*
				 * put back a key-length?
				 */
				switch (impair_key_length_attribute) {
				case IMPAIR_EMIT_NO:
					break;
				case IMPAIR_EMIT_EMPTY:
					/*
					 * XXX: how? IKEv2 sends a
					 * long form packet of no
					 * length.
					 */
					log_state(RC_LOG, st, "IMPAIR: key-length-attribute:empty not implemented");
					break;
				case IMPAIR_EMIT_OMIT:
					log_state(RC_LOG, st, "IMPAIR: not sending key-length attribute");
					break;
				case IMPAIR_EMIT_DUPLICATE:
					if (key_length_to_impair >= 0) {
						log_state(RC_LOG, st, "IMPAIR: duplicating key-length");
						for (unsigned dup = 0; dup < 2; dup++) {
							if (!out_attr(oakley_mode ? OAKLEY_KEY_LENGTH : KEY_LENGTH,
								      key_length_to_impair,
								      attr_desc,
								      attr_val_descs,
								      &trans_pbs))
								goto fail;
						}
					} else {
						log_state(RC_LOG, st, "IMPAIR: no key-length to duplicate");
					}
					break;
				case IMPAIR_EMIT_ROOF:
				default:
				{
					unsigned keylen = impair_key_length_attribute - IMPAIR_EMIT_ROOF;
					log_state(RC_LOG, st, "IMPAIR: sending key-length attribute value %u",
						  keylen);
					if (!out_attr(oakley_mode ? OAKLEY_KEY_LENGTH : KEY_LENGTH,
						      keylen, attr_desc, attr_val_descs,
						      &trans_pbs))
						goto fail;
					break;
				}
				}
				close_output_pbs(&trans_pbs);
			}
			close_output_pbs(&proposal_pbs);
		}
		/* end of a conjunction of proposals */
	}
	close_output_pbs(&sa_pbs);
	return true;

fail:
	return false;
}

bool ikev1_out_quick_sa(struct pbs_out *outs,
			struct state *st)
{
	struct connection *c = st->st_connection;
	struct db_sa *sadb = v1_kernel_alg_makedb(c->config->child_sa.encap_proto,
						  c->config->child_sa.proposals,
						  (st->st_policy & POLICY_COMPRESS),
						  st->logger);

	bool ok = ikev1_out_sa(outs, sadb, st, /*oakley_mode*/false);
	free_sa(&sadb);
	return ok;
}

static bool ikev1_out_oakley_sa(struct pbs_out *outs,
				enum ikev1_auth_method auth_method,
				struct ike_sa *ike,
				bool aggressive_mode)
{
	struct connection *c = ike->sa.st_connection;
	if (PBAD(outs->logger, c->config->ike_proposals.p == NULL)) {
		return false;
	}
	if (PBAD(outs->logger, auth_method == 0)) {
		return false;
	}

	/*
	 * Aggr-Mode - Max transforms == 2 - Multiple
	 * transforms, 1 DH group
	 */
	struct db_sa *sadb = oakley_alg_mergedb(c->config->ike_proposals,
						auth_method,
						aggressive_mode,
						ike->sa.logger);

	/* more sanity */
	if (sadb == NULL) {
		/* already logged? */
		return false;
	}

	bool ok = ikev1_out_sa(outs, sadb, &ike->sa, /*oakley_mode*/true);

	free_sa(&sadb);
	return ok;
}

static enum ikev1_auth_method main_auth_method(struct connection *c)
{
	/*
	 * IKEv1 auth is symmetric
	 *
	 * Notice how MAIN and AGGR modes use slightly different code
	 * to compute the index, and very different tables.
	 */
	struct authby authby = c->local->host.config->authby;
	pexpect(authby_eq(authby, c->remote->host.config->authby));
	unsigned index = ((authby.psk ? 1 : 0) |
			  (authby.rsasig ? 2 : 0) |
			  (c->local->host.config->xauth.server ? 4 : 0) |
			  (c->local->host.config->xauth.client ? 8 : 0));

	/*
	 * XXX: I strongly suspect that proposing RSA+PSK is dead.
	 */

	static const enum ikev1_auth_method auth_method[16] = {
		0,			/* none */
		OAKLEY_PRESHARED_KEY,	/* PSK */
		OAKLEY_RSA_SIG,		/* RSASIG */
		OAKLEY_RSA_SIG,		/* PSK+RSASIG; suspect NA */

		0,			/* XAUTHSERVER + none */
		XAUTHRespPreShared,	/* XAUTHSERVER + PSK */
		XAUTHRespRSA,		/* XAUTHSERVER + RSA */
		XAUTHRespRSA,		/* XAUTHSERVER + RSA+PSK; suspect NA */

		0,			/* XAUTHCLIENT + none */
		XAUTHInitPreShared,	/* XAUTHCLIENT + PSK */
		XAUTHInitRSA,		/* XAUTHCLIENT + RSA */
		XAUTHInitRSA,		/* XAUTHCLIENT + RSA+PSK; suspect NA */

		0,			/* XAUTHCLIENT+XAUTHSERVER + none */
		0,			/* XAUTHCLIENT+XAUTHSERVER + PSK */
		0,			/* XAUTHCLIENT+XAUTHSERVER + RSA */
		0,			/* XAUTHCLIENT+XAUTHSERVER + RSA+PSK */
	};

	passert(index < elemsof(auth_method));
	return auth_method[index];
}

bool ikev1_out_main_sa(struct pbs_out *outs, struct ike_sa *ike)
{
	struct connection *c = ike->sa.st_connection;
	enum ikev1_auth_method auth_method = main_auth_method(c);

	return ikev1_out_oakley_sa(outs, auth_method, ike, false);
}

static enum ikev1_auth_method aggr_auth_method(const struct connection *c)
{
	/*
	 * IKEv1 auth is symmetric
	 *
	 * Notice how MAIN and AGGR modes use slightly different code
	 * to compute the index.
	 */
	enum keyword_auth auth = c->local->host.config->auth;
	pexpect(auth == c->remote->host.config->auth);
	unsigned index = ((auth == AUTH_PSK ? 1 :
			   auth == AUTH_RSASIG ? 2 : 0) |
			  (c->local->host.config->xauth.server ? 4 : 0) |
			  (c->local->host.config->xauth.client ? 8 : 0));

	static const enum ikev1_auth_method auth_method[16] = {
		0,			/* none */
		OAKLEY_PRESHARED_KEY,	/* PSK */
		OAKLEY_RSA_SIG,		/* RSASIG */
		0,			/* PSK+RSASIG => invalid in AM */

		0,			/* XAUTHSERVER + none */
		XAUTHRespPreShared,	/* XAUTHSERVER + PSK */
		XAUTHRespRSA,		/* XAUTHSERVER + RSA */
		0,			/* XAUTHSERVER + RSA+PSK => invalid */

		0,			/* XAUTHCLIENT + none */
		XAUTHInitPreShared,	/* XAUTHCLIENT + PSK */
		XAUTHInitRSA,		/* XAUTHCLIENT + RSA */
		0,			/* XAUTHCLIENT + RSA+PSK => invalid */

		0,			/* XAUTHCLIENT+XAUTHSERVER + none */
		0,			/* XAUTHCLIENT+XAUTHSERVER + PSK */
		0,			/* XAUTHCLIENT+XAUTHSERVER + RSA */
		0,			/* XAUTHCLIENT+XAUTHSERVER + RSA+PSK */
	};
	passert(index < elemsof(auth_method));
	return auth_method[index];
}

bool ikev1_out_aggr_sa(struct pbs_out *outs, struct ike_sa *ike)
{
	struct connection *c = ike->sa.st_connection;
	enum ikev1_auth_method auth_method = aggr_auth_method(c);

	return ikev1_out_oakley_sa(outs, auth_method, ike, true);
}

/**
 * Handle long form of duration attribute.
 *
 * The code is can only handle values that can fit in unsigned long.
 * "Clamping" is probably an acceptable way to impose this limitation.
 *
 * @param pbs PB Stream
 * @return uint32_t duration, in seconds.
 *
 * Technical nit, there's only one bit involved: ISAKMP_ATTR_AF_MASK
 * == ISAKMP_ATTR_AF_TV == 0x8000 and ISAKMP_ATTR_AF_TLV is zero.
 */
static uint32_t decode_life_duration(const struct isakmp_attribute *a,
				     struct pbs_in *pbs)
{
	switch (a->isaat_af_type & ISAKMP_ATTR_AF_MASK) {
	case ISAKMP_ATTR_AF_TV:
	{
		uint32_t val = a->isaat_lv;
		dbg("   basic duration: %" PRIu32 " (TV)", val);
		return val;
	}
	case ISAKMP_ATTR_AF_TLV:
	{
		/* ignore leading zeros */
		while (pbs_left(pbs) != 0 && *pbs->cur == '\0') {
			pbs->cur++;
		}

		uint32_t val = 0;
		if (pbs_left(pbs) > sizeof(val)) {
			/* "clamp" too large value to max representable value */
			val = UINT32_MAX;
			dbg("   too large long duration clamped to: %" PRIu32, val);
		} else {
			/* decode number */
			while (pbs_left(pbs) != 0) {
				val = (val << BITS_IN_BYTE) | *pbs->cur++;
			}
			dbg("   long duration: %" PRIu32 " (TVL)", val);
		}
		return val;
	}
	}
	bad_case(a->isaat_af_type);
}

/* Preparse the body of an IKEv1 ISAKMP SA Payload and find which policy is
 * required to match the packet. Errors are just ignored and will be detected
 * and handled later in parse_isakmp_sa_body().
 *
 * All we want for the moment is to know whether peer is using RSA or PSK.
 * NOTE: sa_pbs is passed by value so the caller's PBS is unchanged!
 */

diag_t preparse_isakmp_sa_body(struct pbs_in sa_pbs /* by value! */,
			       struct authby *authby_out, bool *xauth_out)
{
	diag_t d;
	bool xauth = false;
	struct authby authby = {0};
	*authby_out = authby;
	*xauth_out = xauth;

	uint32_t ipsecdoisit;
	d = pbs_in_struct(&sa_pbs, &ipsec_sit_desc,
			  &ipsecdoisit, sizeof(ipsecdoisit), NULL);
	if (d != NULL) {
		return d;
	}

	struct pbs_in proposal_pbs;
	struct isakmp_proposal proposal;
	d = pbs_in_struct(&sa_pbs, &isakmp_proposal_desc,
			  &proposal, sizeof(proposal), &proposal_pbs);
	if (d != NULL) {
		return d;
	}

	if (proposal.isap_spisize > MAX_ISAKMP_SPI_SIZE)
		return diag("proposal SPI size is too big");

	if (proposal.isap_spisize > 0) {
		shunk_t junk_spi;
		d = pbs_in_shunk(&proposal_pbs, proposal.isap_spisize, &junk_spi,
				 "Oakley SPI (ignored)");
		if (d != NULL) {
			return d;
		}
	}

	for (unsigned trans_left = proposal.isap_notrans; trans_left > 0; trans_left--) {
		struct pbs_in trans_pbs;
		struct isakmp_transform trans;
		diag_t d = pbs_in_struct(&proposal_pbs, &isakmp_isakmp_transform_desc,
					 &trans, sizeof(trans), &trans_pbs);
		if (d != NULL) {
			return d;
		}

		while (pbs_left(&trans_pbs) >= isakmp_oakley_attribute_desc.size) {
			struct pbs_in attr_pbs;
			struct isakmp_attribute attr;
			diag_t d = pbs_in_struct(&trans_pbs, &isakmp_oakley_attribute_desc,
						 &attr, sizeof(attr), &attr_pbs);
			if (d != NULL) {
				return d;
			}

			switch (attr.isaat_af_type) {
			case OAKLEY_AUTHENTICATION_METHOD | ISAKMP_ATTR_AF_TV:
				switch (attr.isaat_lv) {
				case XAUTHInitPreShared:
					xauth = true;
					authby.psk = true;
					break;
				case OAKLEY_PRESHARED_KEY:
					authby.psk = true;
					break;
				case XAUTHInitRSA:
					xauth = true;
					authby.rsasig = true;
					break;
				case OAKLEY_RSA_SIG:
					authby.rsasig = true;
					break;
				}
				break;
			}
		}
	}

	*authby_out = authby;
	*xauth_out = xauth;
	return NULL;
}

static bool ikev1_verify_ike(const struct trans_attrs *ta,
			     struct ike_proposals ike_proposals,
			     struct logger *logger)
{
	if (ta->ta_encrypt == NULL) {
		llog(RC_LOG_SERIOUS, logger,
			    "OAKLEY proposal refused: missing encryption");
		return false;
	}
	if (ta->ta_prf == NULL) {
		llog(RC_LOG_SERIOUS, logger,
			    "OAKLEY proposal refused: missing PRF");
		return false;
	}
	if (ta->ta_integ != NULL) {
		llog_pexpect(logger, HERE, "OAKLEY proposal refused: contains unexpected integrity %s",
			     ta->ta_prf->common.fqn);
		return false;
	}
	if (ta->ta_dh == NULL) {
		llog(RC_LOG_SERIOUS, logger, "OAKLEY proposal refused: missing DH");
		return false;
	}
	if (ike_proposals.p == NULL) {
		dbg("OAKLEY proposal verified unconditionally; no alg_info to check against");
		return true;
	}

	/*
	 * simple test to toss low key_len, will accept it only
	 * if specified in "esp" string
	 */
	bool ealg_insecure = (ta->enckeylen < 128);

	FOR_EACH_PROPOSAL(ike_proposals.p, proposal) {
		struct v1_proposal algs = v1_proposal(proposal);
		if (algs.encrypt == ta->ta_encrypt &&
		    (algs.enckeylen == 0 ||
		     ta->enckeylen == 0 ||
		     algs.enckeylen == ta->enckeylen) &&
		    algs.prf == ta->ta_prf &&
		    algs.dh == ta->ta_dh) {
			if (ealg_insecure) {
				llog(RC_LOG_SERIOUS, logger,
					    "You should NOT use insecure/broken IKE algorithms (%s)!",
					    ta->ta_encrypt->common.fqn);
			} else {
				dbg("OAKLEY proposal verified; matching alg_info found");
				return true;
			}
		}
	}
	llog(RC_LOG, logger, "Oakley Transform [%s (%d), %s, %s] refused%s",
		    ta->ta_encrypt->common.fqn, ta->enckeylen,
		    ta->ta_prf->common.fqn, ta->ta_dh->common.fqn,
		    ealg_insecure ?
		    " due to insecure key_len and enc. alg. not listed in \"ike\" string" :
		  "");
	return false;
}

/**
 * Parse the body of an ISAKMP SA Payload (i.e. Phase 1 / Main Mode).
 * Various shortcuts are taken.  In particular, the policy, such as
 * it is, is hardwired.
 *
 * If r_sa is non-NULL, the body of an SA representing the selected
 * proposal is emitted.
 *
 * If "selection" is true, the SA is supposed to represent the
 * single transform that the peer has accepted.
 * ??? We only check that it is acceptable, not that it is one that we offered!
 *
 * It also means that we are inR1, and this as implications when we are
 * doing XAUTH, as it changes the meaning of the XAUTHInit/XAUTHResp.
 *
 * Only IPsec DOI is accepted (what is the ISAKMP DOI?).
 * Error response is rudimentary.
 *
 * This routine is used by main_inI1_outR1() and main_inR1_outI2().
 */
v1_notification_t parse_isakmp_sa_body(struct pbs_in *sa_pbs,		/* body of input SA Payload */
				       const struct isakmp_sa *sa,	/* header of input SA Payload */
				       struct pbs_out *r_sa_pbs,	/* if non-NULL, where to emit winning SA */
				       bool selection,		/* if this SA is a selection, only one transform
								 * can appear. */
				       struct state *const st)	/* current state object */
{
	diag_t d;
	const struct connection *const c = st->st_connection;
	bool xauth_init = false,
		xauth_resp = false;
	const char *const role = selection ? "initiator" : "responder";
	const chunk_t *pss = &empty_chunk;

	passert(c != NULL);

	/* calculate the per-end policy that might apply */

	FOR_EACH_ITEM(spd, &c->child.spds) {
		if (selection) {
			/*
			 * this is the initiator, we have proposed, they have answered,
			 * and we must decide if they proposed what we wanted.
			 */
			xauth_init |= spd->local->host->config->xauth.client;
			xauth_resp |= spd->local->host->config->xauth.server;
		} else {
			/*
			 * this is the responder, they have proposed to us, what
			 * are we willing to be?
			 */
			xauth_init |= spd->local->host->config->xauth.server;
			xauth_resp |= spd->local->host->config->xauth.client;
		}
	}

	/* DOI */
	if (sa->isasa_doi != ISAKMP_DOI_IPSEC) {
		esb_buf b;
		log_state(RC_LOG_SERIOUS, st, "Unknown/unsupported DOI %s",
			  enum_show(&doi_names, sa->isasa_doi, &b));
		/* XXX Could send notification back */
		return v1N_DOI_NOT_SUPPORTED;	/* reject whole SA */
	}

	/* Situation */
	uint32_t ipsecdoisit;

	d = pbs_in_struct(sa_pbs, &ipsec_sit_desc, &ipsecdoisit, sizeof(ipsecdoisit), NULL);
	if (d != NULL) {
		llog_diag(RC_LOG, st->logger, &d, "%s", "");
		return v1N_SITUATION_NOT_SUPPORTED;	/* reject whole SA */
	}

	if (ipsecdoisit != SIT_IDENTITY_ONLY) {
		lset_buf lb;
		log_state(RC_LOG_SERIOUS, st, "unsupported IPsec DOI situation (%s)",
			  str_lset(&sit_bit_names, ipsecdoisit, &lb));
		/* XXX Could send notification back */
		return v1N_SITUATION_NOT_SUPPORTED;	/* reject whole SA */
	}

	/* The rules for ISAKMP SAs are scattered.
	 * RFC 2409 "IKE" section 5 says that there
	 * can only be one SA, and it can have only one proposal in it.
	 * There may well be multiple transforms.
	 */
	struct isakmp_proposal proposal;
	struct pbs_in proposal_pbs;

	d = pbs_in_struct(sa_pbs, &isakmp_proposal_desc,
			  &proposal, sizeof(proposal),
			  &proposal_pbs);
	if (d != NULL) {
		llog_diag(RC_LOG, st->logger, &d, "%s", "");
		return v1N_PAYLOAD_MALFORMED;	/* reject whole SA */
	}

	if (proposal.isap_pnp != ISAKMP_NEXT_NONE) {
		esb_buf b;
		log_state(RC_LOG_SERIOUS, st,
			  "Proposal Payload must be alone in Oakley SA; found %s following Proposal",
			  enum_show(&ikev1_payload_names, proposal.isap_pnp, &b));
		return v1N_PAYLOAD_MALFORMED;	/* reject whole SA */
	}

	if (proposal.isap_protoid != PROTO_ISAKMP) {
		esb_buf b;
		log_state(RC_LOG_SERIOUS, st,
			  "unexpected Protocol ID (%s) found in Oakley Proposal",
			  enum_show(&ikev1_protocol_names, proposal.isap_protoid, &b));
		return v1N_INVALID_PROTOCOL_ID;	/* reject whole SA */
	}

	/* Just what should we accept for the SPI field?
	 * The RFC is sort of contradictory.  We will ignore the SPI
	 * as long as it is of the proper size.
	 *
	 * From RFC2408 2.4 Identifying Security Associations:
	 *   During phase 1 negotiations, the initiator and responder cookies
	 *   determine the ISAKMP SA. Therefore, the SPI field in the Proposal
	 *   payload is redundant and MAY be set to 0 or it MAY contain the
	 *   transmitting entity's cookie.
	 *
	 * From RFC2408 3.5 Proposal Payload:
	 *    o  SPI Size (1 octet) - Length in octets of the SPI as defined by
	 *       the Protocol-Id.  In the case of ISAKMP, the Initiator and
	 *       Responder cookie pair from the ISAKMP Header is the ISAKMP SPI,
	 *       therefore, the SPI Size is irrelevant and MAY be from zero (0) to
	 *       sixteen (16).  If the SPI Size is non-zero, the content of the
	 *       SPI field MUST be ignored.  If the SPI Size is not a multiple of
	 *       4 octets it will have some impact on the SPI field and the
	 *       alignment of all payloads in the message.  The Domain of
	 *       Interpretation (DOI) will dictate the SPI Size for other
	 *       protocols.
	 */
	if (proposal.isap_spisize == 0) {
		/* empty (0) SPI -- fine */
	} else if (proposal.isap_spisize <= MAX_ISAKMP_SPI_SIZE) {
		shunk_t junk_spi;
		diag_t d = pbs_in_shunk(&proposal_pbs, proposal.isap_spisize, &junk_spi,
					"Oakley SPI (ignored)");
		if (d != NULL) {
			llog_diag(RC_LOG_SERIOUS, st->logger, &d, "%s", "");
			return v1N_PAYLOAD_MALFORMED;	/* reject whole SA */
		}
	} else {
		log_state(RC_LOG_SERIOUS, st,
			  "invalid SPI size (%u) in Oakley Proposal",
			  (unsigned)proposal.isap_spisize);
		return v1N_INVALID_SPI;	/* reject whole SA */
	}

	if (selection && proposal.isap_notrans != 1) {
		log_state(RC_LOG_SERIOUS, st,
			  "a single Transform is required in a selecting Oakley Proposal; found %u",
			  (unsigned)proposal.isap_notrans);
		return v1N_BAD_PROPOSAL_SYNTAX;	/* reject whole SA */
	}

	/* for each transform payload... */

	int last_transnum = -1;
	unsigned no_trans_left = proposal.isap_notrans;

	for (;;) {
		if (no_trans_left == 0) {
			log_state(RC_LOG_SERIOUS, st,
				  "number of Transform Payloads disagrees with Oakley Proposal Payload");
			return v1N_BAD_PROPOSAL_SYNTAX;	/* reject whole SA */
		}

		uint16_t life_type = 0;	/* initialized to silence GCC */

		/* initialize only optional field in ta */
		struct trans_attrs ta = {
			.life_seconds = IKE_SA_LIFETIME_DEFAULT /* until SA expires */
		};

		struct isakmp_transform trans;
		struct pbs_in trans_pbs;

		diag_t d = pbs_in_struct(&proposal_pbs, &isakmp_isakmp_transform_desc,
					 &trans, sizeof(trans), &trans_pbs);
		if (d != NULL) {
			llog_diag(RC_LOG, st->logger, &d, "%s", "");
			return v1N_BAD_PROPOSAL_SYNTAX;	/* reject whole SA */
		}

		if (trans.isat_transnum <= last_transnum) {
			/* picky, picky, picky */
			log_state(RC_LOG_SERIOUS, st,
				  "Transform Numbers are not monotonically increasing in Oakley Proposal");
			return v1N_BAD_PROPOSAL_SYNTAX;	/* reject whole SA */
		}
		last_transnum = trans.isat_transnum;

		if (trans.isat_transid != KEY_IKE) {
			esb_buf b;
			log_state(RC_LOG_SERIOUS, st,
				  "expected KEY_IKE but found %s in Oakley Transform",
				  enum_show(&isakmp_transformid_names,
					    trans.isat_transid, &b));
			return v1N_INVALID_TRANSFORM_ID;	/* reject whole SA */
		}

		const uint8_t *attr_start = trans_pbs.cur;
		size_t attr_len = pbs_left(&trans_pbs);

		/* process all the attributes that make up the transform */

		lset_t seen_attrs = LEMPTY;
		lset_t seen_durations = LEMPTY;
		bool ok = true;
#define UGH(FMT, ...)							\
		{							\
			esb_buf typeesb_;				\
			ok = false;					\
			log_state(RC_LOG_SERIOUS, st,			\
				  FMT".  Attribute %s",			\
				  ##__VA_ARGS__,			\
				  enum_show(&oakley_attr_names,		\
					    a.isaat_af_type, &typeesb_)); \
		}

		while (pbs_left(&trans_pbs) >= isakmp_oakley_attribute_desc.size) {
			struct isakmp_attribute a;
			struct pbs_in attr_pbs;
			uint32_t val; /* room for larger values */

			diag_t d = pbs_in_struct(&trans_pbs, &isakmp_oakley_attribute_desc,
						 &a, sizeof(a), &attr_pbs);
			if (d != NULL) {
				llog_diag(RC_LOG_SERIOUS, st->logger, &d, "invalid transform: ");
				return v1N_BAD_PROPOSAL_SYNTAX;	/* reject whole SA */
			}

			passert((a.isaat_af_type & ISAKMP_ATTR_RTYPE_MASK) <
				LELEM_ROOF);

			if (LHAS(seen_attrs, a.isaat_af_type & ISAKMP_ATTR_RTYPE_MASK)) {
				esb_buf b;
				log_state(RC_LOG_SERIOUS, st,
					  "repeated %s attribute in Oakley Transform %u",
					  enum_show(&oakley_attr_names, a.isaat_af_type, &b),
					  trans.isat_transnum);
				return v1N_BAD_PROPOSAL_SYNTAX;	/* reject whole SA */
			}

			seen_attrs |= LELEM(
				a.isaat_af_type & ISAKMP_ATTR_RTYPE_MASK);

			val = a.isaat_lv;

			if (DBGP(DBG_BASE)) {
				enum_names *vdesc = oakley_attr_val_descs
					[a.isaat_af_type &
					 ISAKMP_ATTR_RTYPE_MASK];

				if (vdesc != NULL) {
					const char *nm =
						enum_name(vdesc,
							  val);

					if (nm != NULL) {
						DBG_log("   [%u is %s]",
							(unsigned)val,
							nm);
					}
				}
			}

			switch (a.isaat_af_type) {
			case OAKLEY_ENCRYPTION_ALGORITHM | ISAKMP_ATTR_AF_TV:
			{
				const struct encrypt_desc *encrypter = ikev1_get_ike_encrypt_desc(val);
				if (encrypter == NULL) {
					esb_buf b;
					UGH("%s is not supported",
					    enum_show(&oakley_enc_names, val, &b));
					break;
				}
				ta.ta_encrypt = encrypter;
				ta.enckeylen = ta.ta_encrypt->keydeflen;
				break;
			}

			case OAKLEY_HASH_ALGORITHM | ISAKMP_ATTR_AF_TV:
				ta.ta_prf = ikev1_get_ike_prf_desc(val);
				if (ta.ta_prf == NULL) {
					esb_buf b;
					UGH("%s is not supported",
					    enum_show(&oakley_hash_names, val, &b));
				}
				break;

			case OAKLEY_AUTHENTICATION_METHOD | ISAKMP_ATTR_AF_TV:
			{
				/* check that authentication method is acceptable */
				switch (val) {
				case XAUTHInitPreShared:
					if (!xauth_init) {
						UGH("policy does not allow Extended Authentication (XAUTH) of initiator (we are %s)",
						    role);
						break;
					}
					ta.doing_xauth = true;
					goto psk_common;

				case XAUTHRespPreShared:
					if (!xauth_resp) {
						UGH("policy does not allow Extended Authentication (XAUTH) of responder (we are %s)",
						    role);
						break;
					}
					ta.doing_xauth = true;
					goto psk_common;

				case OAKLEY_PRESHARED_KEY:
					if (xauth_init) {
						UGH("policy mandates Extended Authentication (XAUTH) with PSK of initiator (we are %s)",
						    role);
						break;
					}
					if (xauth_resp) {
						UGH("policy mandates Extended Authentication (XAUTH) with PSK of responder (we are %s)",
						    role);
						break;
					}
psk_common:

					if (c->remote->host.config->auth != AUTH_PSK) {
						UGH("policy does not allow OAKLEY_PRESHARED_KEY authentication");
					} else {
						/* check that we can find a proper preshared secret */
						pss = get_connection_psk(c);

						if (pss == NULL) {
							id_buf mid;
							id_buf hid;

							UGH("Can't authenticate: no preshared key found for `%s' and `%s'",
							    str_id(&c->local->host.id, &mid),
							    remote_id_was_instantiated(c) ?
							    "%any" :
							    str_id(&c->remote->host.id, &hid));
						} else if (DBGP(DBG_PRIVATE) || DBGP(DBG_CRYPT)) {
							DBG_dump_hunk("User PSK:", *pss);
						}
						ta.auth = OAKLEY_PRESHARED_KEY;
					}
					break;

				case XAUTHInitRSA:
					if (!xauth_init) {
						UGH("policy does not allow Extended Authentication (XAUTH) with RSA of initiator (we are %s)",
						    role);
						break;
					}
					ta.doing_xauth = true;
					goto rsasig_common;

				case XAUTHRespRSA:
					if (!xauth_resp) {
						UGH("policy does not allow Extended Authentication (XAUTH) with RSA of responder (we are %s)",
						    role);
						break;
					}
					ta.doing_xauth = true;
					goto rsasig_common;

				case OAKLEY_RSA_SIG:
					if (xauth_init) {
						UGH("policy mandates Extended Authentication (XAUTH) with RSA of initiator (we are %s)",
						    role);
						break;
					}
					if (xauth_resp) {
						UGH("policy mandates Extended Authentication (XAUTH) with RSA of responder (we are %s)",
						    role);
						break;
					}
rsasig_common:
					/* Accept if policy specifies RSASIG or is default */
					if (c->remote->host.config->auth != AUTH_RSASIG) {
						UGH("policy does not allow OAKLEY_RSA_SIG authentication");
					} else {
						/* We'd like to check
						 * that we can find a
						 * public key for peer
						 * and a private key
						 * for us that is
						 * suitable, but we
						 * don't yet have
						 * peer's ID Payload,
						 * so it seems futile
						 * to try.  We can
						 * assume that if he
						 * proposes it, he
						 * thinks we've got
						 * it.  If we proposed
						 * it, perhaps we know
						 * what we're doing.
						 */
						ta.auth = OAKLEY_RSA_SIG;
					}
					break;

				default:
				{
					esb_buf b;
					UGH("Pluto does not support %s authentication",
					    enum_show(&oakley_auth_names, val, &b));
					break;
				}
				}
			}
			break;

			case OAKLEY_GROUP_DESCRIPTION | ISAKMP_ATTR_AF_TV:
				ta.ta_dh = ikev1_get_ike_dh_desc(val);
				if (ta.ta_dh == NULL) {
					UGH("OAKLEY_GROUP %d not supported", val);
					break;
				}
				break;

			case OAKLEY_LIFE_TYPE | ISAKMP_ATTR_AF_TV:
				switch (val) {
				case OAKLEY_LIFE_SECONDS:
				case OAKLEY_LIFE_KILOBYTES:
					if (LHAS(seen_durations, val)) {
						esb_buf b;
						log_state(RC_LOG_SERIOUS, st,
							  "attribute OAKLEY_LIFE_TYPE value %s repeated",
							  enum_show(&oakley_lifetime_names, val, &b));
						return v1N_BAD_PROPOSAL_SYNTAX;	/* reject whole SA */
					}
					seen_durations |= LELEM(val);
					life_type = val;
					break;
				default:
				{
					esb_buf b;
					UGH("unknown value %s",
					    enum_show(&oakley_lifetime_names, val, &b));
					break;
				}
				}
				break;

			case OAKLEY_LIFE_DURATION | ISAKMP_ATTR_AF_TLV:
			case OAKLEY_LIFE_DURATION | ISAKMP_ATTR_AF_TV:
				val = decode_life_duration(&a, &attr_pbs);
				if (!LHAS(seen_attrs, OAKLEY_LIFE_TYPE)) {
					UGH("OAKLEY_LIFE_DURATION attribute not preceded by OAKLEY_LIFE_TYPE attribute");
					break;
				}
				seen_attrs &=
					~(LELEM(OAKLEY_LIFE_DURATION) |
					  LELEM(OAKLEY_LIFE_TYPE));

				switch (life_type) {
				case OAKLEY_LIFE_SECONDS:
					if (val > deltasecs(IKE_SA_LIFETIME_MAXIMUM)) {
						log_state(RC_LOG, st,
							  "warning: peer requested IKE lifetime of %" PRIu32 " seconds which we capped at our limit of %ju seconds",
							  val, deltasecs(IKE_SA_LIFETIME_MAXIMUM));
						val = deltasecs(IKE_SA_LIFETIME_MAXIMUM);
					}
					ta.life_seconds = deltatime(val);
					break;
				case OAKLEY_LIFE_KILOBYTES:
					dbg("ignoring OAKLEY_LIFE_KILOBYTES=%"PRIu32, val);
					break;
				default:
					bad_case(life_type);
				}
				break;

			case OAKLEY_KEY_LENGTH | ISAKMP_ATTR_AF_TV:
				if (!LHAS(seen_attrs, OAKLEY_ENCRYPTION_ALGORITHM)) {
					UGH("OAKLEY_KEY_LENGTH attribute not preceded by OAKLEY_ENCRYPTION_ALGORITHM attribute");
					break;
				}
				/* because the encrypt algorithm wasn't valid? */
				if (ta.ta_encrypt == NULL) {
					UGH("NULL encrypter with seen OAKLEY_ENCRYPTION_ALGORITHM");
					break;
				}
				/*
				 * check if this keylen is compatible
				 * with specified ike_proposals.
				 */
				if (!encrypt_has_key_bit_length(ta.ta_encrypt, val)) {
					UGH("peer proposed key_len not valid for encrypt algo setup specified");
					break;
				}

				ta.enckeylen = val;
				break;

			default:
				UGH("unsupported OAKLEY attribute");
				break;
			}

			if (!ok) {
				break;
			}
		}

		/* If ugh != NULL, an attr error has been detected and reported */

		/*
		 * this do {} while (FALSE) construct allows code to use "break"
		 * to reject this transform and to move on to next (if any).
		 */

		while (ok) {

			if (c->remote->host.config->auth == AUTH_PSK &&
			    pss != &empty_chunk &&
			    pss != NULL &&
			    ta.ta_prf != NULL) {
				const size_t key_size_min = crypt_prf_fips_key_size_min(ta.ta_prf);

				if (pss->len < key_size_min) {
					if (is_fips_mode()) {
						log_state(RC_LOG_SERIOUS, st,
							  "FIPS Error: connection %s PSK length of %zu bytes is too short for %s PRF in FIPS mode (%zu bytes required)",
							  c->name,
							  pss->len,
							  ta.ta_prf->common.fqn,
							  key_size_min);
						break;	/* reject transform */
					} else {
						log_state(RC_LOG, st,
							  "WARNING: connection %s PSK length of %zu bytes is too short for %s PRF in FIPS mode (%zu bytes required)",
							  c->name,
							  pss->len,
							  ta.ta_prf->common.fqn,
							  key_size_min);
					}
				}
			}

			/*
			 * ML: at last check for allowed transforms in ike_proposals
			 */
			if (!ikev1_verify_ike(&ta, c->config->ike_proposals, st->logger)) {
				/*
				 * already logged; UGH acts as a skip
				 * rest of checks flag
				 */
				break;	/* reject transform */
			}

			/* a little more checking is in order */
			{
				lset_t missing =
					~seen_attrs &
					(LELEM(OAKLEY_ENCRYPTION_ALGORITHM) |
					 LELEM(OAKLEY_HASH_ALGORITHM) |
					 LELEM(OAKLEY_AUTHENTICATION_METHOD) |
					 LELEM(OAKLEY_GROUP_DESCRIPTION));

				if (missing) {
					lset_buf lb;
					log_state(RC_LOG_SERIOUS, st,
						  "missing mandatory attribute(s) %s in Oakley Transform %u",
						  str_lset(&oakley_attr_bit_names, missing, &lb),
						  trans.isat_transnum);
					return v1N_BAD_PROPOSAL_SYNTAX;	/* reject whole SA */
				}
			}

			/*
			 * We must have liked this transform.
			 * Let's finish early and leave.
			 */

			dbg("Oakley Transform %u accepted", trans.isat_transnum);

			if (r_sa_pbs != NULL) {

				/* Situation */
				passert(out_struct(&ipsecdoisit, &ipsec_sit_desc,
						   r_sa_pbs, NULL));

				/* Proposal */
				struct isakmp_proposal r_proposal = proposal;
				r_proposal.isap_spisize = 0;
				r_proposal.isap_notrans = 1;
				struct pbs_out r_proposal_pbs;
				passert(out_struct(&r_proposal,
						   &isakmp_proposal_desc,
						   r_sa_pbs,
						   &r_proposal_pbs));

				/* Transform */
				struct isakmp_transform r_trans = trans;
				r_trans.isat_tnp = ISAKMP_NEXT_NONE;
				struct pbs_out r_trans_pbs;
				passert(out_struct(&r_trans,
						   &isakmp_isakmp_transform_desc,
						   &r_proposal_pbs,
						   &r_trans_pbs));

				passert(out_raw(attr_start, attr_len,
						&r_trans_pbs, "attributes"));
				close_output_pbs(&r_trans_pbs);
				close_output_pbs(&r_proposal_pbs);
				close_output_pbs(r_sa_pbs);
			}

			/* copy over the results */
			st->st_oakley = ta;
			return v1N_NOTHING_WRONG;	/* accept SA */
		}

		/* transform rejected: on to next transform */

		no_trans_left--;

		if (trans.isat_tnp == ISAKMP_NEXT_NONE) {
			if (no_trans_left != 0) {
				log_state(RC_LOG_SERIOUS, st,
					  "number of Transform Payloads disagrees with Oakley Proposal Payload");
				return v1N_BAD_PROPOSAL_SYNTAX;	/* reject whole SA */
			}
			break;
		}
		if (trans.isat_tnp != ISAKMP_NEXT_T) {
			esb_buf b;
			log_state(RC_LOG_SERIOUS, st,
				  "unexpected %s payload in Oakley Proposal",
				  enum_show(&ikev1_payload_names, proposal.isap_pnp, &b));
			return v1N_BAD_PROPOSAL_SYNTAX;	/* reject whole SA */
		}
	}
	log_state(RC_LOG_SERIOUS, st, "no acceptable Oakley Transform");
	return v1N_NO_PROPOSAL_CHOSEN;	/* reject whole SA */
}

/* Initialize st_oakley field of state for use when initiating in
 * aggressive mode.
 *
 * This should probably get more of its parameters, like what group to use,
 * from the connection specification, but it's not there yet.
 * This should ideally be done by passing them via whack.
 *
 */

/* XXX MCR. I suspect that actually all of this is redundant */
bool init_aggr_st_oakley(struct ike_sa *ike)
{
	const struct connection *c = ike->sa.st_connection;
	if (PBAD(ike->sa.logger, c->config->ike_proposals.p == NULL)) {
		return false;
	}

	/*
	 * Max transforms == 2 - Multiple transforms, 1 DH
	 * group
	 */
	enum ikev1_auth_method sadb_auth_method = aggr_auth_method(c);
	struct db_sa *revised_sadb = oakley_alg_mergedb(c->config->ike_proposals,
							sadb_auth_method, true,
							ike->sa.logger);
	if (revised_sadb == NULL) {
		return false;
	}

	passert(revised_sadb->prop_conj_cnt == 1);
	const struct db_prop_conj *cprop = &revised_sadb->prop_conjs[0];

	passert(cprop->prop_cnt == 1);
	const struct db_prop *prop = &cprop->props[0];

	const struct db_trans *trans = &prop->trans[0];

	passert(trans->attr_cnt == 4 || trans->attr_cnt == 5);

	const struct db_attr *enc  = &trans->attrs[0];
	const struct db_attr *hash = &trans->attrs[1];
	const struct db_attr *auth = &trans->attrs[2];
	const struct db_attr *grp  = &trans->attrs[3];

	dbg("initiating aggressive mode with IKE=E=%d-H=%d-M=%d",
	    enc->val, hash->val, grp->val);

	passert(enc->type.oakley == OAKLEY_ENCRYPTION_ALGORITHM);

	struct trans_attrs ta = {
		/* When this SA expires (seconds) */
		.life_seconds = c->config->sa_ike_max_lifetime,
		.ta_encrypt = ikev1_get_ike_encrypt_desc(enc->val)
	};

	passert(ta.ta_encrypt != NULL);

	if (trans->attr_cnt == 5) {
		struct db_attr *enc_keylen;
		enc_keylen = &trans->attrs[4];
		ta.enckeylen = enc_keylen->val;
	} else {
		ta.enckeylen = ta.ta_encrypt->keydeflen;
	}

	passert(hash->type.oakley == OAKLEY_HASH_ALGORITHM);
	ta.ta_prf = ikev1_get_ike_prf_desc(hash->val);
	passert(ta.ta_prf != NULL);

	passert(auth->type.oakley == OAKLEY_AUTHENTICATION_METHOD);
	ta.auth = auth->val;         /* OAKLEY_AUTHENTICATION_METHOD */

	passert(grp->type.oakley == OAKLEY_GROUP_DESCRIPTION);
	ta.ta_dh = ikev1_get_ike_dh_desc(grp->val); /* OAKLEY_GROUP_DESCRIPTION */
	passert(ta.ta_dh != NULL);

	ike->sa.st_oakley = ta;

	free_sa(&revised_sadb);
	return true;
}

/**
 * Parse the body of an IPsec SA Payload (i.e. Phase 2 / Quick Mode).
 *
 * The main routine is parse_ipsec_sa_body; other functions defined
 * between here and there are just helpers.
 *
 * Various shortcuts are taken.  In particular, the policy, such as
 * it is, is hardwired.
 *
 * If r_sa is non-NULL, the body of an SA representing the selected
 * proposal is emitted into it.
 *
 * If "selection" is true, the SA is supposed to represent the
 * single transform that the peer has accepted.
 * ??? We only check that it is acceptable, not that it is one that we offered!
 *
 * Only IPsec DOI is accepted (what is the ISAKMP DOI?).
 * Error response is rudimentary.
 *
 * Since all ISAKMP groups in all SA Payloads must match, st->st_pfs_group
 * holds this across multiple payloads.
 * &unset_group signifies not yet "set"; NULL signifies NONE.
 *
 * This routine is used by quick_inI1_outR1() and quick_inR1_outI2().
 */

struct ipsec_transform {
	struct trans_attrs transattrs;
	deltatime_t lifetime;
	enum kernel_mode kernel_mode;
	ipsec_spi_t spi;
};

static bool parse_ipsec_transform(struct isakmp_transform *trans,
				  struct ipsec_transform *attrs,
				  struct pbs_in *prop_pbs,
				  struct pbs_in *trans_pbs,
				  struct_desc *trans_desc,
				  int previous_transnum, /* or -1 if none */
				  bool selection,
				  bool is_last,
				  uint8_t proto,
				  struct state *st) /* current state object */
{
	lset_t seen_attrs = LEMPTY,
	       seen_durations = LEMPTY;
	uint16_t life_type = 0;	/* initialized to silence GCC */
	const struct dh_desc *pfs_group = NULL;

	diag_t d = pbs_in_struct(prop_pbs, trans_desc, trans, sizeof(*trans), trans_pbs);
	if (d != NULL) {
		llog_diag(RC_LOG, st->logger, &d, "%s", "");
		return false;
	}

	if (trans->isat_transnum <= previous_transnum) {
		log_state(RC_LOG_SERIOUS, st,
			  "Transform Numbers in Proposal are not monotonically increasing");
		return false;
	}

	switch (trans->isat_tnp) {
	case ISAKMP_NEXT_T:
		if (is_last) {
			log_state(RC_LOG_SERIOUS, st,
				  "Proposal Payload has more Transforms than specified");
			return false;
		}
		break;
	case ISAKMP_NEXT_NONE:
		if (!is_last) {
			log_state(RC_LOG_SERIOUS, st,
				  "Proposal Payload has fewer Transforms than specified");
			return false;
		}
		break;
	default:
	{
		esb_buf b;
		log_state(RC_LOG_SERIOUS, st,
			  "expecting Transform Payload, but found %s in Proposal",
			  enum_show(&ikev1_payload_names, trans->isat_tnp, &b));
		return false;
	}
	}

	*attrs = (struct ipsec_transform) {
		.lifetime = IPSEC_SA_LIFETIME_DEFAULT,	/* life_seconds */
		.kernel_mode = 0,		        /* encapsulation */
	};

	switch (proto) {
	case PROTO_IPCOMP:
		/* could be NULL */
		attrs->transattrs.ta_ipcomp = ikev1_get_kernel_ipcomp_desc(trans->isat_transid);
		break;
	case PROTO_IPSEC_ESP:
		/* could be NULL */
		attrs->transattrs.ta_encrypt = ikev1_get_kernel_encrypt_desc(trans->isat_transid);
		break;
	case PROTO_IPSEC_AH:
		break;
	default:
		bad_case(proto);
	}

	while (pbs_left(trans_pbs) >= isakmp_ipsec_attribute_desc.size) {
		struct isakmp_attribute a;
		struct pbs_in attr_pbs;
		enum_names *vdesc;
		uint16_t ty;
		uint32_t val;                          /* room for larger value */
		bool ipcomp_inappropriate = (proto == PROTO_IPCOMP);  /* will get reset if OK */

		diag_t d = pbs_in_struct(trans_pbs, &isakmp_ipsec_attribute_desc,
					 &a, sizeof(a), &attr_pbs);
		if (d != NULL) {
			llog_diag(RC_LOG, st->logger, &d, "%s", "");
			return false;
		}

		ty = a.isaat_af_type & ISAKMP_ATTR_RTYPE_MASK;
		val = a.isaat_lv;

		passert(ty < LELEM_ROOF);
		if (LHAS(seen_attrs, ty)) {
			esb_buf b;
			log_state(RC_LOG_SERIOUS, st,
				  "repeated %s attribute in IPsec Transform %u",
				  enum_show(&ipsec_attr_names, a.isaat_af_type, &b),
				  trans->isat_transnum);
			return false;
		}

		seen_attrs |= LELEM(ty);
		passert(ty < IPSEC_ATTR_VAL_DESCS_ROOF);
		vdesc = ipsec_attr_val_descs[ty];

		if (vdesc != NULL) {
			/* reject unknown enum values */
			if (enum_name(vdesc, val) == NULL) {
				esb_buf b;
				log_state(RC_LOG_SERIOUS, st,
					  "invalid value %" PRIu32 " for attribute %s in IPsec Transform",
					  val,
					  enum_show(&ipsec_attr_names, a.isaat_af_type, &b));
				return false;
			}
			if (DBGP(DBG_BASE)) {
				if ((a.isaat_af_type & ISAKMP_ATTR_AF_MASK) ==
				    ISAKMP_ATTR_AF_TV) {
					esb_buf b;
					DBG_log("   [%" PRIu32 " is %s]",
						val,
						enum_show(vdesc, val, &b));
				}
			}
		}

		switch (a.isaat_af_type) {
		case SA_LIFE_TYPE | ISAKMP_ATTR_AF_TV:
			ipcomp_inappropriate = false;
			if (LHAS(seen_durations, val)) {
				esb_buf b;
				log_state(RC_LOG_SERIOUS, st,
					  "attribute SA_LIFE_TYPE value %s repeated in message",
					  enum_show(&sa_lifetime_names, val, &b));
				return false;
			}
			seen_durations |= LELEM(val);
			life_type = val;
			break;

		case SA_LIFE_DURATION | ISAKMP_ATTR_AF_TLV:
		case SA_LIFE_DURATION | ISAKMP_ATTR_AF_TV:
		{
			deltatime_t val = deltatime(decode_life_duration(&a, &attr_pbs));
			ipcomp_inappropriate = false;
			if (!LHAS(seen_attrs, SA_LIFE_TYPE)) {
				log_state(RC_LOG_SERIOUS, st,
					  "SA_LIFE_DURATION IPsec attribute not preceded by SA_LIFE_TYPE attribute");
				return false;
			}
			seen_attrs &=
				~(LELEM(SA_LIFE_DURATION) |
				  LELEM(SA_LIFE_TYPE));

			switch (life_type) {
			case SA_LIFE_TYPE_SECONDS:
			{
				/*
				 * Silently limit duration to our maximum.
				 */
				deltatime_t lifemax =
					(is_fips_mode() ? FIPS_IPSEC_SA_LIFETIME_MAXIMUM :
					 IPSEC_SA_LIFETIME_MAXIMUM);
				attrs->lifetime =
					(deltatime_cmp(val, >, lifemax) ? lifemax :
					 deltatime_cmp(val, >, st->st_connection->config->sa_ipsec_max_lifetime) ? st->st_connection->config->sa_ipsec_max_lifetime :
					 val);
				break;
			}
			case SA_LIFE_TYPE_KBYTES:
				dbg("ignoring SA_LIFE_TYPE_KBYTES=%ju", deltasecs(val));
				break;
			default:
				bad_case(life_type);
			}
			break;
		}

		case GROUP_DESCRIPTION | ISAKMP_ATTR_AF_TV:
			if (proto == PROTO_IPCOMP) {
				/* Accept reluctantly.  Should not happen, according to
				 * draft-shacham-ippcp-rfc2393bis-05.txt 4.1.
				 */
				ipcomp_inappropriate = false;
				log_state(RC_COMMENT, st,
					  "IPCA (IPcomp SA) contains GROUP_DESCRIPTION.  Ignoring inappropriate attribute.");
			}
			pfs_group = ikev1_get_ike_dh_desc(val);
			if (pfs_group == NULL) {
				log_state(RC_LOG_SERIOUS, st,
					  "OAKLEY_GROUP %" PRIu32 " not supported for PFS",
					  val);
				return false;
			}
			break;

		case ENCAPSULATION_MODE | ISAKMP_ATTR_AF_TV:
			ipcomp_inappropriate = false;
			switch (val) {
			case ENCAPSULATION_MODE_TUNNEL:
			case ENCAPSULATION_MODE_TRANSPORT:
			{
				lset_buf lb;
				dbg("NAT-T non-encap: Installing IPsec SA without ENCAP, st->hidden_variables.st_nat_traversal is %s",
				    str_lset(&natt_method_names, st->hidden_variables.st_nat_traversal, &lb));
				break;
			}

			case ENCAPSULATION_MODE_UDP_TRANSPORT_DRAFTS:
			case ENCAPSULATION_MODE_UDP_TUNNEL_DRAFTS:
			{
				lset_buf lb;
				dbg("NAT-T draft: Installing IPsec SA with ENCAP, st->hidden_variables.st_nat_traversal is %s",
				    str_lset(&natt_method_names, st->hidden_variables.st_nat_traversal, &lb));
				break;
			}

			case ENCAPSULATION_MODE_UDP_TRANSPORT_RFC:
			case ENCAPSULATION_MODE_UDP_TUNNEL_RFC:
			{
				lset_buf lb;
				dbg("NAT-T RFC: Installing IPsec SA with ENCAP, st->hidden_variables.st_nat_traversal is %s",
				    str_lset(&natt_method_names, st->hidden_variables.st_nat_traversal, &lb));
				break;
			}

			default:
				/* should already be filtered out by enum checker */
				bad_case(val);
			}

			/* normalize the actual attribute value */
			switch (val) {
			case ENCAPSULATION_MODE_TRANSPORT:
			case ENCAPSULATION_MODE_UDP_TRANSPORT_DRAFTS:
			case ENCAPSULATION_MODE_UDP_TRANSPORT_RFC:
				attrs->kernel_mode = KERNEL_MODE_TRANSPORT;
				break;
			case ENCAPSULATION_MODE_TUNNEL:
			case ENCAPSULATION_MODE_UDP_TUNNEL_DRAFTS:
			case ENCAPSULATION_MODE_UDP_TUNNEL_RFC:
				attrs->kernel_mode = KERNEL_MODE_TUNNEL;
				break;
			}

			break;

		case AUTH_ALGORITHM | ISAKMP_ATTR_AF_TV:
			attrs->transattrs.ta_integ = ikev1_get_kernel_integ_desc(val);
			if (attrs->transattrs.ta_integ == NULL) {
				/*
				 * Caller will also see NULL and
				 * assume that things should stumble
				 * on to the next algorithm.
				 *
				 * Either straight AH, or ESP
				 * containing AUTH; or what?
				 */
				esb_buf b;
				log_state(RC_LOG_SERIOUS, st,
					  "IKEv1 %s integrity algorithm %s not supported",
					  (proto == PROTO_IPSEC_ESP ? "ESP" : "AH"),
					  enum_show(&ah_transformid_names, val, &b));
			}
			break;

		case KEY_LENGTH | ISAKMP_ATTR_AF_TV:
			if (attrs->transattrs.ta_encrypt == NULL) {
				log_state(RC_LOG_SERIOUS, st,
					  "IKEv1 key-length attribute without encryption algorithm");
				return false;
			}
			if (!encrypt_has_key_bit_length(attrs->transattrs.ta_encrypt, val)) {
				log_state(RC_LOG_SERIOUS, st,
					  "IKEv1 key-length attribute without encryption algorithm");
				return false;
			}
			attrs->transattrs.enckeylen = val;
			break;

		default:
		{
			esb_buf b;
			log_state(RC_LOG_SERIOUS, st,
				  "unsupported IPsec attribute %s",
				  enum_show(&ipsec_attr_names, a.isaat_af_type, &b));
			return false;
		}
		}

		if (ipcomp_inappropriate) {
			esb_buf b;
			log_state(RC_LOG_SERIOUS, st,
				  "IPsec attribute %s inappropriate for IPCOMP",
				  enum_show(&ipsec_attr_names, a.isaat_af_type, &b));
			return false;
		}
	}

	/* Although an IPCOMP SA (IPCA) ought not to have a pfs_group,
	 * if it does, demand that it be consistent.
	 * See draft-shacham-ippcp-rfc2393bis-05.txt 4.1.
	 */
	if (proto != PROTO_IPCOMP || pfs_group != NULL) {
		if (st->st_pfs_group == &unset_group)
			st->st_pfs_group = pfs_group;

		if (st->st_pfs_group != pfs_group) {
			log_state(RC_LOG_SERIOUS, st,
				  "GROUP_DESCRIPTION inconsistent with that of %s in IPsec SA",
				  selection ? "the Proposal" : "a previous Transform");
			return false;
		}
	}

	if (LHAS(seen_attrs, SA_LIFE_DURATION)) {
		log_state(RC_LOG_SERIOUS, st,
			  "SA_LIFE_TYPE IPsec attribute not followed by SA_LIFE_DURATION attribute in message");
		return false;
	}

	if (!LHAS(seen_attrs, ENCAPSULATION_MODE)) {
		if (proto == PROTO_IPCOMP) {
			/* draft-shacham-ippcp-rfc2393bis-05.txt 4.1:
			 * "If the Encapsulation Mode is unspecified,
			 * the default value of Transport Mode is assumed."
			 * This contradicts/overrides the DOI (quoted below).
			 */
			attrs->kernel_mode = KERNEL_MODE_TRANSPORT;
		} else {
			/* ??? Technically, RFC 2407 (IPSEC DOI) 4.5 specifies that
			 * the default is "unspecified (host-dependent)".
			 * This makes little sense, so we demand that it be specified.
			 */
			log_state(RC_LOG_SERIOUS, st,
				  "IPsec Transform must specify ENCAPSULATION_MODE");
			return false;
		}
	}

	/*
	 * For ESP, check if the encryption key length is required.
	 *
	 * If a required key length was missing force the proposal to
	 * be rejected by settinf .ta_encrypt=NULL.
	 *
	 * If an optional key-length is missing set it to the correct
	 * value (.keydeflen) (which can be 0).  This is safe since
	 * the code echoing back the proposal never emits a keylen
	 * when .keylen_omitted
	 */
	if (proto == PROTO_IPSEC_ESP && !LHAS(seen_attrs, KEY_LENGTH) &&
	    attrs->transattrs.ta_encrypt != NULL) {
		if (attrs->transattrs.ta_encrypt->keylen_omitted) {
			attrs->transattrs.enckeylen = attrs->transattrs.ta_encrypt->keydeflen;
		} else {
			/* ealg requires a key length attr */
			log_state(RC_LOG_SERIOUS, st,
				  "IPsec encryption transform %s did not specify required KEY_LENGTH attribute",
				  attrs->transattrs.ta_encrypt->common.fqn);
			attrs->transattrs.ta_encrypt = NULL; /* force rejection */
		}
	}

	/*
	 * For ESP, if the integrity algorithm (AUTH_ALGORITHM) was
	 * completely missing, set it to NONE.
	 *
	 * This way the caller has sufficient information to
	 * differentiate between missing integrity (NONE) and unknown
	 * integrity (NULL) and decide if the proposals combination of
	 * ESP/AH AEAD and NONE is valid.
	 *
	 * For instance, AEAD+[NONE].
	 */
	if (proto == PROTO_IPSEC_ESP && !LHAS(seen_attrs, AUTH_ALGORITHM)) {
		dbg("ES missing INTEG aka AUTH, setting it to NONE");
		attrs->transattrs.ta_integ = &ike_alg_integ_none;
	}

	if (proto == PROTO_IPSEC_AH) {
		if (!LHAS(seen_attrs, AUTH_ALGORITHM)) {
			log_state(RC_LOG_SERIOUS, st,
				  "AUTH_ALGORITHM attribute missing in AH Transform");
			return false;
		}
	}

	return true;
}

static void echo_proposal(struct isakmp_proposal r_proposal,    /* proposal to emit */
			  struct isakmp_transform r_trans,      /* winning transformation within it */
			  uint8_t pnp,                          /* Next Payload for proposal */
			  struct pbs_out *r_sa_pbs,             /* SA PBS into which to emit */
			  struct ipsec_proto_info *pi,          /* info about this protocol instance */
			  struct_desc *trans_desc,              /* descriptor for this transformation */
			  struct pbs_in *trans_pbs,             /* PBS for incoming transform */
			  const struct spd *sr,           /* host details for the association */
			  struct logger *logger)
{
	struct pbs_out r_proposal_pbs;
	struct pbs_out r_trans_pbs;

	/* Proposal */
	r_proposal.isap_pnp = pnp;
	r_proposal.isap_notrans = 1;
	passert(out_struct(&r_proposal, &isakmp_proposal_desc, r_sa_pbs,
			   &r_proposal_pbs));

	/* allocate and emit our CPI/SPI */
	if (r_proposal.isap_protoid == PROTO_IPCOMP) {
		/* CPI is stored in network low order end of an
		 * ipsec_spi_t.  So we start a couple of bytes in.
		 * Note: we may fail to generate a satisfactory CPI,
		 * but we'll ignore that.
		 */
		pi->inbound.spi = get_ipsec_cpi(sr->connection, logger);
		passert(out_raw((uint8_t *) &pi->inbound.spi +
				IPSEC_DOI_SPI_SIZE - IPCOMP_CPI_SIZE,
				IPCOMP_CPI_SIZE,
				&r_proposal_pbs, "CPI"));
	} else {
		pi->inbound.spi = get_ipsec_spi(sr->connection,
						r_proposal.isap_protoid == PROTO_IPSEC_AH ?
						&ip_protocol_ah : &ip_protocol_esp,
						pi->outbound.spi,
						logger);
		/* XXX should check for errors */
		passert(out_raw((uint8_t *) &pi->inbound.spi, IPSEC_DOI_SPI_SIZE,
				&r_proposal_pbs, "SPI"));
	}

	/* Transform */
	r_trans.isat_tnp = ISAKMP_NEXT_NONE;
	passert(out_struct(&r_trans, trans_desc, &r_proposal_pbs, &r_trans_pbs));

	/* Transform Attributes: pure echo */
	trans_pbs->cur = trans_pbs->start + sizeof(struct isakmp_transform);
	passert(out_raw(trans_pbs->cur, pbs_left(trans_pbs),
			&r_trans_pbs, "attributes"));

	close_output_pbs(&r_trans_pbs);
	close_output_pbs(&r_proposal_pbs);
}

v1_notification_t parse_ipsec_sa_body(struct pbs_in *sa_pbs,           /* body of input SA Payload */
				      const struct isakmp_sa *sa,  /* header of input SA Payload */
				      struct pbs_out *r_sa_pbs,         /* if non-NULL, where to emit body of winning SA */
				      bool selection,              /* if this SA is a selection, only one transform may appear */
				      struct state *st)            /* current state object */
{
	diag_t d;
	const struct connection *c = st->st_connection;
	uint32_t ipsecdoisit;
	struct pbs_in next_proposal_pbs;

	struct isakmp_proposal next_proposal;
	ipsec_spi_t next_spi;

	bool next_full = true;

	/* DOI */
	if (sa->isasa_doi != ISAKMP_DOI_IPSEC) {
		esb_buf b;
		log_state(RC_LOG_SERIOUS, st, "Unknown or unsupported DOI %s",
			  enum_show(&doi_names, sa->isasa_doi, &b));
		/* XXX Could send notification back */
		return v1N_DOI_NOT_SUPPORTED;	/* reject whole SA */
	}

	/* Situation */
	d = pbs_in_struct(sa_pbs, &ipsec_sit_desc, &ipsecdoisit, sizeof(ipsecdoisit), NULL);
	if (d != NULL) {
		llog_diag(RC_LOG, st->logger, &d, "%s", "");
		return v1N_SITUATION_NOT_SUPPORTED;	/* reject whole SA */
	}

	if (ipsecdoisit != SIT_IDENTITY_ONLY) {
		lset_buf lb;
		log_state(RC_LOG_SERIOUS, st, "unsupported IPsec DOI situation (%s)",
			  str_lset(&sit_bit_names, ipsecdoisit, &lb));
		/* XXX Could send notification back */
		return v1N_SITUATION_NOT_SUPPORTED;	/* reject whole SA */
	}

	/* The rules for IPsec SAs are scattered.
	 * RFC 2408 "ISAKMP" section 4.2 gives some info.
	 * There may be multiple proposals.  Those with identical proposal
	 * numbers must be considered as conjuncts.  Those with different
	 * numbers are disjuncts.
	 * Each proposal may have several transforms, each considered
	 * an alternative.
	 * Each transform may have several attributes, all applying.
	 *
	 * To handle the way proposals are combined, we need to do a
	 * look-ahead.
	 */

	d = pbs_in_struct(sa_pbs, &isakmp_proposal_desc,
			  &next_proposal, sizeof(next_proposal),
			  &next_proposal_pbs);
	if (d != NULL) {
		llog_diag(RC_LOG, st->logger, &d, "%s", "");
		return v1N_BAD_PROPOSAL_SYNTAX;	/* reject whole SA */
	}

	/* for each conjunction of proposals... */
	while (next_full) {
		int propno = next_proposal.isap_proposal;

		struct pbs_in ah_prop_pbs;
		struct pbs_in esp_prop_pbs;
		struct pbs_in ipcomp_prop_pbs;

		struct isakmp_proposal ah_proposal = {0};
		struct isakmp_proposal esp_proposal = {0};
		struct isakmp_proposal ipcomp_proposal = {0};

		ipsec_spi_t ah_spi = 0;
		ipsec_spi_t esp_spi = 0;
		ipsec_spi_t ipcomp_cpi = 0;

		bool ah_seen = false;
		bool esp_seen = false;
		bool ipcomp_seen = false;

		const struct ipcomp_desc *well_known_cpi = NULL;

		/*
		 * For each proposal in the conjunction.
		 */

		do {
			if (next_proposal.isap_protoid == PROTO_IPCOMP) {
				/* IPCOMP CPI */
				if (next_proposal.isap_spisize == IPSEC_DOI_SPI_SIZE) {
					/*
					 * This code is to accommodate
					 * those peculiar
					 * implementations that send a
					 * CPI in the bottom of an
					 * SPI-sized field.
					 *
					 * See draft-shacham-ippcp-rfc2393bis-05.txt 4.1
					 */
					shunk_t filler;
					diag_t d = pbs_in_shunk(&next_proposal_pbs,
								IPSEC_DOI_SPI_SIZE - IPCOMP_CPI_SIZE,
								&filler, "CPI filler");
					if (d != NULL) {
						llog_diag(RC_LOG, st->logger, &d, "%s", "");
						return v1N_INVALID_SPI;	/* reject whole SA */
					}
					if (!all_zero(filler.ptr, filler.len)) {
						return v1N_INVALID_SPI;	/* reject whole SA */
					}
				} else if (next_proposal.isap_spisize !=
					   IPCOMP_CPI_SIZE) {
					log_state(RC_LOG_SERIOUS, st,
						  "IPsec Proposal with improper CPI size (%u)",
						  next_proposal.isap_spisize);
					return v1N_INVALID_SPI;	/* reject whole SA */
				}

				/* We store CPI in the low order of a network order
				 * ipsec_spi_t.  So we start a couple of bytes in.
				 */
				zero(&next_spi);
				diag_t d = pbs_in_bytes(&next_proposal_pbs,
							(uint8_t *)&next_spi + IPSEC_DOI_SPI_SIZE - IPCOMP_CPI_SIZE,
							IPCOMP_CPI_SIZE,
							"CPI");
				if (d != NULL) {
					llog_diag(RC_LOG, st->logger, &d, "%s", "");
					return v1N_INVALID_SPI;	/* reject whole SA */
				}

				/* If sanity ruled, CPIs would have to be such that
				 * the SAID (the triple (CPI, IPCOM, destination IP))
				 * would be unique, just like for SPIs.  But there is a
				 * perversion where CPIs can be well-known and consequently
				 * the triple is not unique.  We hide this fact from
				 * ourselves by fudging the top 16 bits to make
				 * the property true internally!
				 */
				switch (ntohl(next_spi)) {
				case IPCOMP_DEFLATE:
					well_known_cpi = &ike_alg_ipcomp_deflate;
					next_spi = uniquify_peer_cpi(next_spi, st, 0);
					if (next_spi == 0) {
						log_state(RC_LOG_SERIOUS, st,
							  "IPsec Proposal contains well-known CPI that I cannot uniquify");
						return v1N_INVALID_SPI;	/* reject whole SA */
					}
					break;
				default:
					if (ntohl(next_spi) <
					    IPCOMP_FIRST_NEGOTIATED ||
					    ntohl(next_spi) >
					    IPCOMP_LAST_NEGOTIATED) {
						log_state(RC_LOG_SERIOUS, st,
							  "IPsec Proposal contains CPI from non-negotiated range (0x%" PRIx32 ")",
							  ntohl(next_spi));
						return v1N_INVALID_SPI;	/* reject whole SA */
					}
					break;
				}
				/* end of IPCOMP CPI handling */
			} else {
				/* AH or ESP SPI */
				if (next_proposal.isap_spisize !=
				    IPSEC_DOI_SPI_SIZE) {
					log_state(RC_LOG_SERIOUS, st,
						  "IPsec Proposal with improper SPI size (%u)",
						  next_proposal.isap_spisize);
					return v1N_INVALID_SPI;	/* reject whole SA */
				}

				diag_t d = pbs_in_thing(&next_proposal_pbs, next_spi, "SPI");
				if (d != NULL) {
					llog_diag(RC_LOG, st->logger, &d, "%s", "");
					return v1N_INVALID_SPI;	/* reject whole SA */
				}

				/* SPI value 0 is invalid and values 1-255 are reserved to IANA.
				 * RFC 2402 (ESP) 2.4, RFC 2406 (AH) 2.1
				 * IPCOMP???
				 */
				if (ntohl(next_spi) < IPSEC_DOI_SPI_MIN) {
					log_state(RC_LOG_SERIOUS, st,
						  "IPsec Proposal contains invalid SPI (0x%" PRIx32 ")",
						  ntohl(next_spi));
					return v1N_INVALID_SPI;	/* reject whole SA */
				}
			}

			if (next_proposal.isap_notrans == 0) {
				log_state(RC_LOG_SERIOUS, st,
					  "IPsec Proposal contains no Transforms (skipped)");
				continue;
			}

			switch (next_proposal.isap_protoid) {
			case PROTO_IPSEC_AH:
				if (ah_seen) {
					log_state(RC_LOG_SERIOUS, st,
						  "IPsec SA contains two simultaneous AH Proposals");
					return v1N_BAD_PROPOSAL_SYNTAX;	/* reject whole SA */
				}
				ah_seen = true;
				ah_prop_pbs = next_proposal_pbs;
				ah_proposal = next_proposal;
				ah_spi = next_spi;
				break;

			case PROTO_IPSEC_ESP:
				if (esp_seen) {
					log_state(RC_LOG_SERIOUS, st,
						  "IPsec SA contains two simultaneous ESP Proposals");
					return v1N_BAD_PROPOSAL_SYNTAX;	/* reject whole SA */
				}
				esp_seen = true;
				esp_prop_pbs = next_proposal_pbs;
				esp_proposal = next_proposal;
				esp_spi = next_spi;
				break;

			case PROTO_IPCOMP:
				if (ipcomp_seen) {
					log_state(RC_LOG_SERIOUS, st,
						  "IPsec SA contains two simultaneous IPCOMP Proposals");
					return v1N_BAD_PROPOSAL_SYNTAX;	/* reject whole SA */
				}
				ipcomp_seen = true;
				ipcomp_prop_pbs = next_proposal_pbs;
				ipcomp_proposal = next_proposal;
				ipcomp_cpi = next_spi;
				break;

			default:
			{
				esb_buf b;
				log_state(RC_LOG_SERIOUS, st,
					  "unexpected Protocol ID (%s) in IPsec Proposal",
					  enum_show(&ikev1_protocol_names,
						    next_proposal.isap_protoid, &b));
				return v1N_INVALID_PROTOCOL_ID;	/* reject whole SA */
			}
			}

			/* refill next_proposal */
			if (next_proposal.isap_pnp == ISAKMP_NEXT_NONE) {
				next_full = false;
				break;
			} else if (next_proposal.isap_pnp != ISAKMP_NEXT_P) {
				esb_buf b;
				log_state(RC_LOG_SERIOUS, st,
					  "unexpected in Proposal: %s",
					  enum_show(&ikev1_payload_names, next_proposal.isap_pnp, &b));
				return v1N_BAD_PROPOSAL_SYNTAX;	/* reject whole SA */
			}

			diag_t d = pbs_in_struct(sa_pbs, &isakmp_proposal_desc,
						 &next_proposal, sizeof(next_proposal),
						 &next_proposal_pbs);
			if (d != NULL) {
				llog_diag(RC_LOG, st->logger, &d, "%s", "");
				return v1N_BAD_PROPOSAL_SYNTAX;	/* reject whole SA */
			}
		} while (next_proposal.isap_proposal == propno);

		/*
		 * Now that we have all conjuncts, we should try the
		 * Cartesian product of each's transforms!
		 *
		 * At the moment, we take short-cuts on account of our
		 * rudimentary hard-wired policy.  For now, we find an
		 * acceptable AH (if any) and then an acceptable ESP.
		 * The only interaction is that the ESP acceptance can
		 * know whether there was an acceptable AH and hence
		 * not require an AUTH.
		 */

		enum kernel_mode kernel_mode = 0;

		struct ipsec_transform ah_attrs = {0};
		struct ipsec_transform esp_attrs = {0};
		struct ipsec_transform ipcomp_attrs = {0};

		struct pbs_in ah_trans_pbs;
		struct pbs_in esp_trans_pbs;
		struct pbs_in ipcomp_trans_pbs;

		struct isakmp_transform ah_trans;
		struct isakmp_transform esp_trans;
		struct isakmp_transform ipcomp_trans;

		if (ah_seen) {
			int previous_transnum = -1;
			int tn;

			for (tn = 0; tn != ah_proposal.isap_notrans; tn++) {
				if (!parse_ipsec_transform(&ah_trans,
							   &ah_attrs,
							   &ah_prop_pbs,
							   &ah_trans_pbs,
							   &isakmp_ah_transform_desc,
							   previous_transnum,
							   selection,
							   tn == ah_proposal.isap_notrans - 1,
							   PROTO_IPSEC_AH,
							   st))
					return v1N_BAD_PROPOSAL_SYNTAX;	/* reject whole SA */

				previous_transnum = ah_trans.isat_transnum;

				/*
				 * Since, for AH, when integrity is
				 * missing, the proposal gets rejected
				 * outright, a NULL here must indicate
				 * that integrity was present but the
				 * lookup failed.
				 */
				if (ah_attrs.transattrs.ta_integ == NULL) {
					/* error already logged */
					dbg("ignoring AH proposal with unknown integrity");
					continue;       /* try another */
				}

				/* we must understand ah_attrs.transid
				 * COMBINED with ah_attrs.transattrs.ta_ikev1_integ_hash.
				 * See RFC 2407 "IPsec DOI" section 4.4.3
				 * The following combinations are legal,
				 * but we don't implement all of them:
				 * It seems as if each auth algorithm
				 * only applies to one ah transid.
				 * AH_MD5, AUTH_ALGORITHM_HMAC_MD5
				 * AH_MD5, AUTH_ALGORITHM_KPDK (unimplemented)
				 * AH_SHA, AUTH_ALGORITHM_HMAC_SHA1
				 * AH_DES, AUTH_ALGORITHM_DES_MAC (unimplemented)
				 */
				if (ah_trans.isat_transid != ah_attrs.transattrs.ta_integ->integ_ikev1_ah_transform) {
					esb_buf b;
					log_state(RC_LOG_SERIOUS, st,
						  "%s attribute inappropriate in %s Transform",
						  ah_attrs.transattrs.ta_integ->common.fqn,
						  enum_show(&ah_transformid_names,
							    ah_trans.isat_transid, &b));
					return v1N_BAD_PROPOSAL_SYNTAX;	/* reject whole SA */
				}
				break;                  /* we seem to be happy */
			}
			if (tn == ah_proposal.isap_notrans)
				continue; /* we didn't find a nice one */

			/* Check AH proposal with configuration */
			if (!ikev1_verify_ah(c, &ah_attrs.transattrs, st->logger)) {
				continue;
			}
			kernel_mode = ah_attrs.kernel_mode;
			ah_attrs.spi = ah_spi;
		}

		if (esp_seen) {
			int previous_transnum = -1;
			int tn;

			for (tn = 0; tn != esp_proposal.isap_notrans; tn++) {
				if (!parse_ipsec_transform(
				      &esp_trans,
				      &esp_attrs,
				      &esp_prop_pbs,
				      &esp_trans_pbs,
				      &isakmp_esp_transform_desc,
				      previous_transnum,
				      selection,
				      tn == esp_proposal.isap_notrans - 1,
				      PROTO_IPSEC_ESP,
				      st))
					return v1N_BAD_PROPOSAL_SYNTAX;	/* reject whole SA */

				previous_transnum = esp_trans.isat_transnum;

				/*
				 * check for allowed transforms in alg_info_esp
				 */
				if (!ikev1_verify_esp(c, &esp_attrs.transattrs, st->logger)) {
					continue;       /* try another */
				}

				/*
				 * XXX: this is testing for AH, is
				 * conbining even supported?  If not,
				 * the test should be pushed into
				 * ikev1_verify_esp().
				 */
				if (esp_attrs.transattrs.ta_integ == &ike_alg_integ_none &&
				    !encrypt_desc_is_aead(esp_attrs.transattrs.ta_encrypt) &&
				    !ah_seen) {
					LDBGP_JAMBUF(DBG_BASE, &global_logger, buf) {
						jam_string(buf, "ESP from ");
						jam_address(buf, &c->remote->host.addr);
						jam_string(buf, " must either have AUTH or be combined with AH");
					}
					continue; /* try another */
				}

				if (ah_seen && ah_attrs.kernel_mode != esp_attrs.kernel_mode) {
					log_state(RC_LOG_SERIOUS, st,
						  "Skipped bogus proposal where AH and ESP transforms disagree about mode");
					continue; /* try another */
				}

				break; /* we seem to be happy */
			}
			if (tn == esp_proposal.isap_notrans)
				continue; /* we didn't find a nice one */

			kernel_mode = esp_attrs.kernel_mode;
			esp_attrs.spi = esp_spi;
		} else if (st->st_policy & POLICY_ENCRYPT) {
			connection_buf cib;
			address_buf b;
			dbg("policy for "PRI_CONNECTION" requires encryption but ESP not in Proposal from %s",
			    pri_connection(c, &cib),
			    str_address(&c->remote->host.addr, &b));
			continue; /* we needed encryption, but didn't find ESP */
		} else if ((st->st_policy & POLICY_AUTHENTICATE) && !ah_seen) {
			connection_buf cib;
			address_buf b;
			dbg("policy for \"%s\"%s requires authentication but none in Proposal from %s",
			    pri_connection(c, &cib),
			    str_address(&c->remote->host.addr, &b));
			continue; /* we need authentication, but we found neither ESP nor AH */
		}

		if (ipcomp_seen) {
			int previous_transnum = -1;
			int tn;

			if (!(st->st_policy & POLICY_COMPRESS)) {
				address_buf b;
				connection_buf cib;
				log_state(RC_LOG, st,
					  "compression proposed by %s, but policy for "PRI_CONNECTION" forbids it",
					  str_address(&c->remote->host.addr, &b),
					  pri_connection(c, &cib));
				return v1N_BAD_PROPOSAL_SYNTAX;	/* reject whole SA */
			}

			if (well_known_cpi != NULL && !ah_seen && !esp_seen) {
				log_state(RC_LOG, st,
					  "illegal proposal: bare IPCOMP used with well-known CPI %s",
					  well_known_cpi->common.fqn);
				return v1N_BAD_PROPOSAL_SYNTAX;	/* reject whole SA */
			}

			for (tn = 0; tn != ipcomp_proposal.isap_notrans;
			     tn++) {
				if (!parse_ipsec_transform(&ipcomp_trans,
							   &ipcomp_attrs,
							   &ipcomp_prop_pbs,
							   &ipcomp_trans_pbs,
							   &isakmp_ipcomp_transform_desc,
							   previous_transnum,
							   selection,
							   tn == ipcomp_proposal.isap_notrans - 1,
							   PROTO_IPCOMP,
							   st)) {
					return v1N_BAD_PROPOSAL_SYNTAX;	/* reject whole SA */
				}

				previous_transnum = ipcomp_trans.isat_transnum;

				if (ipcomp_attrs.transattrs.ta_ipcomp == NULL) {
					dbg("ignoring IPCOMP proposal with NULL or unknown ID");
					continue; /* try another */
				}

				if (well_known_cpi != NULL &&
				    ipcomp_attrs.transattrs.ta_ipcomp != well_known_cpi) {
					log_state(RC_LOG, st,
						  "illegal proposal: IPCOMP well-known CPI %s disagrees with transform %s",
						  well_known_cpi->common.fqn,
						  ipcomp_attrs.transattrs.ta_ipcomp->common.fqn);
					return v1N_BAD_PROPOSAL_SYNTAX;	/* reject whole SA */
				}

				/* all we can handle! */
				if (ipcomp_attrs.transattrs.ta_ipcomp != &ike_alg_ipcomp_deflate) {
					address_buf b;
					dbg("unsupported IPCOMP Transform %s from %s",
					    ipcomp_attrs.transattrs.ta_ipcomp->common.fqn,
					    str_address(&c->remote->host.addr, &b));
					continue; /* try another */
				}

				if (ah_seen &&
				    ah_attrs.kernel_mode != ipcomp_attrs.kernel_mode) {
					/* ??? This should be an error, but is it? */
					dbg("AH and IPCOMP transforms disagree about mode; TUNNEL presumed");
				}
				if (esp_seen &&
				    esp_attrs.kernel_mode != ipcomp_attrs.kernel_mode) {
					/* ??? This should be an error, but is it? */
					dbg("ESP and IPCOMP transforms disagree about mode; TUNNEL presumed");
				}

				break; /* we seem to be happy */
			}
			if (tn == ipcomp_proposal.isap_notrans)
				continue; /* we didn't find a nice one */

			ipcomp_attrs.spi = ipcomp_cpi;
		}

		/* Eureka: we liked what we saw -- accept it. */

		if (r_sa_pbs != NULL) {
			/* emit what we've accepted */

			/* Situation */
			passert(out_struct(&ipsecdoisit, &ipsec_sit_desc,
					   r_sa_pbs, NULL));

			/* AH proposal */
			if (ah_seen) {
				echo_proposal(ah_proposal,
					      ah_trans,
					      esp_seen || ipcomp_seen ? ISAKMP_NEXT_P : ISAKMP_NEXT_NONE,
					      r_sa_pbs,
					      &st->st_ah,
					      &isakmp_ah_transform_desc,
					      &ah_trans_pbs,
					      c->spd,
					      st->logger);
			}

			/* ESP proposal */
			if (esp_seen) {
				echo_proposal(esp_proposal,
					      esp_trans,
					      ipcomp_seen ? ISAKMP_NEXT_P : ISAKMP_NEXT_NONE,
					      r_sa_pbs,
					      &st->st_esp,
					      &isakmp_esp_transform_desc,
					      &esp_trans_pbs,
					      c->spd,
					      st->logger);
			}

			/* IPCOMP proposal */
			if (ipcomp_seen) {
				echo_proposal(ipcomp_proposal,
					      ipcomp_trans,
					      ISAKMP_NEXT_NONE,
					      r_sa_pbs,
					      &st->st_ipcomp,
					      &isakmp_ipcomp_transform_desc,
					      &ipcomp_trans_pbs,
					      c->spd,
					      st->logger);
			}

			close_output_pbs(r_sa_pbs);
		}

		/* save decoded version of winning SA in state */

		const realtime_t now = realnow();

		st->st_kernel_mode = kernel_mode;

#define COPY(WHAT)							\
		if (WHAT##_seen) {					\
			st->st_##WHAT.protocol = &ip_protocol_##WHAT;	\
			st->st_##WHAT.trans_attrs = WHAT##_attrs.transattrs; \
			st->st_##WHAT.v1_lifetime = WHAT##_attrs.lifetime; \
			st->st_##WHAT.outbound.spi = WHAT##_attrs.spi;	\
			st->st_##WHAT.inbound.last_used = now;		\
			st->st_##WHAT.outbound.last_used = now;		\
		}

		COPY(ah);
		COPY(esp);
		COPY(ipcomp);
#undef COPY

		return v1N_NOTHING_WRONG;	/* accept this transform! */
	}

	log_state(RC_LOG_SERIOUS, st, "no acceptable Proposal in IPsec SA");
	return v1N_NO_PROPOSAL_CHOSEN;	/* reject whole SA */
}
