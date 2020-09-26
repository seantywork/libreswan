/* IKEv2 send packet routines, for Libreswan
 *
 * Copyright (C) 2018-202- Andrew Cagney
 * Copyright (C) 2019 D. Hugh Redelmeier <hugh@mimosa.com>
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

#ifndef IKEV2_SEND_H
#define IKEV2_SEND_H

#include "chunk.h"

#include "packet.h"		/* for pb_stream */
#include "connections.h"

struct msg_digest;
struct dh_desc;
struct ike_sa;
struct child_sa;
enum payload_security;

struct v2_incoming_fragment {
	chunk_t text;		/* cipher or plain - decrypt in place */
	shunk_t plain;		/* read-only; points into decrypted plain text */
	size_t iv_offset;	/* into text */
};

struct v2_incoming_fragments {
	unsigned total;
	unsigned count;
	enum isakmp_xchg_type xchg;
	/*
	 * A fragment.
	 *
	 * - initially it contains the first fragment to arrive (which
	 *   may not be fragment 1)
	 *
	 *   on the responder, should there be a problem during
	 *   SKEYSEED then the saved message is used to construct the
	 *   headers for the un-protected error response
	 *
	 * - when fragment 1 arrives, it replaces any previously saved
         *   fragment
	 *
	 *   once all fragments have arrived, fragment 1 with its
	 *   protected, but not encrypted, payloads, is used to
	 *   reconstitute the message
	 *
	 * Additionally:
	 *
	 * - on the responder, while waiting for SKEYSEED to be
         *   calculated, it can contain the first secured message
         *   (instead of a fragment)
	 *
	 * Note: until all fragments have arrived and been decrypted,
	 * the saved fragment should not be trusted.
	 */
	struct msg_digest *md;
	/*
	 * Next-Payload from first fragment.
	 */
	int first_np;
	/*
	 * For simplicity, index by fragment number which is 1-based;
	 * leaving element 0 empty.
	 */
	struct v2_incoming_fragment frags[MAX_IKE_FRAGMENTS + 1];
};

struct v2_outgoing_fragment {
	struct v2_outgoing_fragment *next;
	/* hunk like */
	size_t len;
	uint8_t ptr[]; /* can be bigger */
};

/*
 * Should the payload be encrypted/protected (don't confuse this with
 * authenticated)?
 */

void record_v2N_response(struct logger *logger,
			 struct ike_sa *ike,
			 struct msg_digest *md,
			 v2_notification_t type,
			 const chunk_t *data /* optional */,
			 enum payload_security security);

void record_v2N_spi_response(struct logger *logger,
			     struct ike_sa *st,
			     struct msg_digest *md,
			     enum ikev2_sec_proto_id protoid,
			     ipsec_spi_t *spi,
			     v2_notification_t type,
			     const chunk_t *data /* optional */,
			     enum payload_security security);

bool send_recorded_v2_message(struct ike_sa *ike, const char *where,
			      enum message_role role);

void send_v2N_response_from_md(struct msg_digest *md,
			       v2_notification_t type,
			       const shunk_t *data);

typedef bool payload_emitter_fn(struct state *st, pb_stream *pbs);

extern stf_status record_v2_informational_request(const char *name,
						  struct ike_sa *owner,
						  struct state *sender,
						  payload_emitter_fn *emit_payloads);
void record_v2_outgoing_fragment(struct pbs_out *pbs,
				 const char *what,
				 struct v2_outgoing_fragment **frags);
void record_v2_message(struct ike_sa *ike,
		       struct pbs_out *msg,
		       const char *what,
		       enum message_role message);

void free_v2_message_queues(struct state *st);
void free_v2_incoming_fragments(struct v2_incoming_fragments **frags);
void free_v2_outgoing_fragments(struct v2_outgoing_fragment **frags);

/*
 * Emit an IKEv2 payload.
 *
 * Like the out_*() primitives, these have the pb_stream for emission as
 * the last parameter (or second last if the last one is the pb_stream
 * for the sub-payload).
 */

bool emit_v2UNKNOWN(const char *victim, enum isakmp_xchg_type exchange_type,
		    struct pbs_out *outs);

/*
 * Emit an IKEv2 Notify payload.
 */

bool emit_v2N_header(struct pbs_out *outs,
		     v2_notification_t ntype,
		     enum ikev2_sec_proto_id protocol_id,
		     unsigned spi_size,
		     struct pbs_out *spi_and_data);

/* emit a v2 Notification payload, with optional SA and optional sub-payload */
bool emit_v2Nsa_pl(v2_notification_t ntype,
		enum ikev2_sec_proto_id protoid,
		const ipsec_spi_t *spi, /* optional */
		pb_stream *outs,
		pb_stream *payload_pbs /* optional */);

/* emit a v2 Notification payload, with optional sub-payload */
/* i.e., emit header then open a containing payload? */
bool emit_v2Npl(v2_notification_t ntype,
		pb_stream *outs,
		pb_stream *payload_pbs /* optional */);

/* emit a v2 Notification payload, with optional hunk as sub-payload */
bool emit_v2N_bytes(v2_notification_t ntype,
		   const void *bytes, size_t size,
		   pb_stream *outs);
#define emit_v2N_hunk(NTYPE, HUNK, OUTS)	emit_v2N_bytes(NTYPE, (HUNK).ptr, (HUNK).len, OUTS)

/* output a v2 simple Notification payload */
bool emit_v2N(v2_notification_t ntype,
	       pb_stream *outs);

bool emit_v2N_SIGNATURE_HASH_ALGORITHMS(lset_t sighash_policy,
					pb_stream *outs);

extern void ike_set_out_mark(const struct connection *c, ip_endpoint *ike_remote);
#endif
