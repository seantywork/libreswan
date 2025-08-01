/* Structure of messages from whack to Pluto proper.
 *
 * Copyright (C) 1998-2001,2015-2017 D. Hugh Redelmeier.
 * Copyright (C) 2012-2019 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2011 Mika Ilmaranta <ilmis@foobar.fi>
 * Copyright (C) 2012 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2012 Philippe Vouters <Philippe.Vouters@laposte.net>
 * Copyright (C) 2013,2016 Antony Antony <antony@phenome.org>
 * Copyright (C) 2016,2018 Andrew Cagney
 * Copyright (C) 2017 Mayank Totale <mtotale@gmail.com>
 * Copyright (C) 2020 Nupur Agrawal <nupur202000@gmail.com>
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

#ifndef WHACK_H
#define WHACK_H

#include <stdint.h>		/* for uintmax_t et.al. */

#include "ietf_constants.h"
#include "lset.h"
#include "lmod.h"
#include "deltatime.h"
#include "chunk.h"
#include "reqid.h"
#include "err.h"
#include "ip_range.h"
#include "ip_subnet.h"
#include "ip_protoport.h"
#include "ip_cidr.h"
#include "authby.h"
#include "encap_proto.h"
#include "sa_kind.h"
#include "constants.h"		/* for enum keyword_auth; et.al. */
#include "end.h"
#include "shunt.h"
#include "global_redirect.h"
#include "xauthby.h"
#include "xauthfail.h"
#include "ddos_mode.h"

#ifndef DEFAULT_CTL_SOCKET
# define DEFAULT_CTL_SOCKET IPSEC_RUNDIR "/pluto.ctl"
#endif

enum whack_command {
	WHACK_UNUSED,
	WHACK_REREADALL,
	WHACK_FETCHCRLS,
	WHACK_REREADSECRETS,
	WHACK_REREADCERTS,
	/**/
	WHACK_PROCESSSTATUS,
	WHACK_ADDRESSPOOLSTATUS,
	WHACK_CONNECTIONSTATUS,
	WHACK_BRIEFCONNECTIONSTATUS,
	WHACK_BRIEFSTATUS,
	WHACK_FIPSSTATUS,
	WHACK_GLOBALSTATUS,
	WHACK_TRAFFICSTATUS,
	WHACK_SHUNTSTATUS,
	/**/
	WHACK_DELETE,
	WHACK_ADD,
	WHACK_ROUTE,
	WHACK_UNROUTE,
	WHACK_INITIATE,
	WHACK_SUSPND,
	WHACK_ACQUIRE,
	WHACK_DOWN,
	WHACK_SUSPEND,
	/**/
	WHACK_DELETEUSER,
	WHACK_DELETEID,
	WHACK_DELETESTATE,
	WHACK_CRASH,
	/**/
	WHACK_CLEARSTATS,
	WHACK_DDNS,
	WHACK_PURGEOCSP,
	WHACK_SHOWSTATES,
	/**/
#define whack_sa_name(OP) ((OP) == WHACK_REKEY_IKE ? "rekey-ike" :	\
			   (OP) == WHACK_REKEY_CHILD ? "rekey-child" :	\
			   (OP) == WHACK_DELETE_IKE ? "delete-ike" :	\
			   (OP) == WHACK_DELETE_CHILD ? "delete-child" : \
			   (OP) == WHACK_DOWN_IKE ? "down-ike" :	\
			   (OP) == WHACK_DOWN_CHILD ? "down-child" :	\
			   "???")
#define whack_sa_kind(OP) ((OP) == WHACK_REKEY_IKE ? IKE_SA :		\
			   (OP) == WHACK_REKEY_CHILD ? CHILD_SA :	\
			   (OP) == WHACK_DELETE_IKE ? IKE_SA :		\
			   (OP) == WHACK_DELETE_CHILD ? CHILD_SA :	\
			   (OP) == WHACK_DOWN_IKE ? IKE_SA :		\
			   (OP) == WHACK_DOWN_CHILD ? CHILD_SA :	\
			   0)
	WHACK_REKEY_IKE,
	WHACK_REKEY_CHILD,
	WHACK_DELETE_IKE,
	WHACK_DELETE_CHILD,
	WHACK_DOWN_IKE,
	WHACK_DOWN_CHILD,
	/**/
	WHACK_DDOS,
	WHACK_LIST,
	WHACK_CHECKPUBKEYS,
	/**/
#ifdef USE_SECCOMP
	WHACK_SECCOMP_CRASHTEST,
#endif
	WHACK_SHUTDOWN_LEAVE_STATE,
	/**/
	WHACK_ACTIVE_REDIRECT,
	WHACK_GLOBAL_REDIRECT,
	/**/
	WHACK_LISTEN,
	WHACK_UNLISTEN,
};

/*
 * Since the message remains on one host, native representation is
 * used.
 */

/* struct whack_end is a lot like connection.h's struct end
 * It differs because it is going to be shipped down a socket
 * and because whack is a separate program from pluto.
 */
struct whack_end {
	const char *leftright;	/* either "left" or "right" (not shipped) */

	const char *id;		/* id string (if any) -- decoded by pluto */
	const char *ca;		/* distinguished name string (if any) -- parsed by pluto */
	const char *groups;		/* access control groups (if any) -- parsed by pluto */

	/*
	 * Where, if anywhere, is the public/private key coming from?
	 * Pass everything over and let pluto decide what if anything
	 * conflict.
	 */
	const char *cert;
	const char *ckaid;
	const char *pubkey;
	enum ipseckey_algorithm_type pubkey_alg;

	const char *auth;

	const char *ikeport;			/* host */
	const char *host;
	const char *nexthop;
	const char *sourceip;
	const char *vti;			/* host */
	const char *interface_ip;		/* for ipsec-interface */

	const char *subnet; /* child */
	const char *subnets;	/* alias subnet expansion */
	const char *protoport;

	const char *updown;		/* string */
	const char *virt;
	const char *addresspool;

	enum yn_options xauthserver;	/* for XAUTH */
	enum yn_options xauthclient;
	const char *xauthusername;

	enum yn_options cat;		/* IPv4 Client Address Translation */
	const char *sendcert;
	const char *autheap;
	enum ike_cert_type certtype;

	enum yn_options modecfgserver;	/* for MODECFG */
	enum yn_options modecfgclient;
	enum yn_options groundhog;	/* Is this end a groundhog? */
};

/*
 * annex to different messages.
 */

struct whack_listen {
	unsigned ike_socket_bufsize;		/* IKE socket recv/snd buffer size */
	bool ike_socket_errqueue_toggle;	/* toggle MSG_ERRQUEUE on IKE socket */
	enum yn_options ike_socket_errqueue;
};

struct whack_ddos {
	enum ddos_mode mode;	/* for DDOS modes */
};

struct whack_acquire {
	struct {
		ip_address address;
		ip_port port;
	} local, remote;
	const char *label;
	unsigned ipproto;
};

/*
 * Impairments.
 */

struct whack_impair {
	unsigned what;
	uintmax_t value;
	bool enable;
};

struct whack_message {
	/*
	 * Basic Commands: status and shutdown (NOTHING ELSE!!!)
	 *
	 * Whack (pickle.c) sets .magic == WHACK_BASIC_MAGIC IFF
	 * either .whack_status or .whack_shutdown is valid.
	 *
	 * Whack/addconn (pickle.c) set .magic == whack_magic() for
	 * all other cases.
	 */
	struct whack_basic {
#define WHACK_BASIC_MAGIC (((((('w' << 8) + 'h') << 8) + 'k') << 8) + 25)
		unsigned int magic;
		/* DO NOT ADD BOOLS HERE */
		bool whack_status;
		/* NOR HERE */
		bool whack_shutdown;
		/* AND DON'T EVEN THINK ABOUT HERE */
	} basic;

	/*
	 * END OF BASIC COMMANDS
	 *
	 * If you change anything earlier in this struct, update
	 * WHACK_BASIC_MAGIC so DO NOT DO THAT!
	 */

	/* when non-zero, act on this */
	enum whack_command whack_command;

	/* name is used in connection and initiate */
	const char *name;

	/* debugging updates to apply */

	lmod_t whack_debugging;

	/* what to impair and how; a list like structure */

	struct {
		unsigned len;
		struct whack_impair *list;
	} impairments;

	enum whack_from {
		WHACK_FROM_WHACK = 1,
		WHACK_FROM_ADDCONN,
	} whack_from;			/* whack and addconn have
					 * different .whack_add
					 * semantics */
	bool whack_async;

	/*
	 * Command specific parameters.  Commands also share some
	 * options such as .name. above.
	 *
	 * THIS IS NOT A UNION (XXX: but it should be).
	 *
	 * The pickler can't handle conditionally pickling strings
	 * based on .whack_command above.
	 */

	struct {
		struct whack_listen listen;
		struct whack_ddos ddos;
		struct whack_acquire acquire;
	} whack;

	const char *keyexchange;
	const char *ikev2;

	const char *authby;
	const char *debug;

	enum shunt_policy shunt[SHUNT_KIND_ROOF];
	enum autostart autostart;
	enum yn_options mobike;		/* allow MOBIKE */
	enum yn_options intermediate;	/* allow Intermediate Exchange */
	enum yn_options sha2_truncbug;	/* allow Intermediate Exchange */
	enum yn_options share_lease;	/* allow further connections to use lease IP */
	enum yn_options overlapip;	/* can two conns that have
					 * subnet=vhost: declare the
					 * same IP? */
	enum yn_options ms_dh_downgrade;	/* allow IKEv2 rekey
						 * to downgrade DH
						 * group - Microsoft
						 * bug */
	enum yn_options pfs_rekey_workaround;	/* during IKEv2 rekey
						 * use full esp=
						 * proposal */
	enum yn_options dns_match_id;	/* perform reverse DNS lookup
					 * on IP to confirm ID */
	enum yn_options pam_authorize;	/* non-standard, custom PAM
					 * authorize call on ID
					 * (IKEv2) */
	enum yn_options ignore_peer_dns;	/* install obtained
						 * DNS servers
						 * locally */
	enum yna_options ikepad;	/* pad ike packets and
					 * payloads to 4 bytes or
					 * not */
	enum yn_options require_id_on_certificate;
					/* require certificates to
					 * have IKE ID on cert SAN */
	enum yn_options modecfgpull;	/* is modecfg pulled by
					 * client? */
	enum yn_options aggressive;	/* do we do aggressive
					 * mode? */
	enum yn_options decap_dscp;	/* decap ToS/DSCP bits */
	enum yn_options encap_dscp;	/* encap ToS/DSCP bits */
	enum yn_options nopmtudisc;	/* ??? */
	enum ynf_options fragmentation;	/* fragment IKE payload */
	enum yne_options esn;		/* accept or request ESN{yes,no} */
	enum nppi_options ppk;		/* pre-shared post-quantum key */
	enum yn_options pfs;
	enum yn_options compress;
	enum type_options type;		/* type=tunnel|transport|SHUNT */
	enum yn_options session_resumption;	/* for RFC 5723 -
						 * IKEv2 Session
						 * Resumption */

	const char *ipsec_max_bytes;
	const char *ipsec_max_packets;

	deltatime_t ikelifetime;
	deltatime_t ipsec_lifetime;

	deltatime_t rekeymargin; /* which SA? */
	const char *rekeyfuzz;

	const char *replay_window;
	deltatime_t retransmit_timeout;
	const char *retransmit_interval;	/* milliseconds, not
						 * seconds!*/
	enum nic_offload_options nic_offload;
	const char *ipsec_interface;

	/* For IKEv1 RFC 3706 - Dead Peer Detection / IKEv2 liveness */
	const char *dpddelay;		/* seconds */
	const char *dpdtimeout;	/* seconds */

	/* Force the use of NAT-T on a connection */
	enum yna_options encapsulation;

	enum yn_options narrowing;	/* IKEv2 only? */
	enum yn_options rekey;
	enum yn_options reauth;

	/*
	 * TCP: Allow TCP as fallback, only do TCP or only do UDP; and
	 * the port.
	 */
	enum tcp_options enable_tcp;
	uintmax_t tcp_remoteport;

	/* Option to allow per-conn setting of sending of NAT-T
	 * keepalives - default is enabled */
	enum yn_options nat_keepalive;
	/* Option to tweak sending NATT drafts, rfc or both */
	enum ikev1_natt_policy nat_ikev1_method;

	/* Option to allow sending INITIAL-CONTACT payload */
	enum yn_options initial_contact;

	/*
	 * Option to just send the Cisco VID - the other end will behave
	 * differently (ModeCFG + RSA?)
	 */
	const char *remote_peer_type;
	const char *cisco_unity;
	const char *cisco_split;

	/* Option to send strongswan VID to allow better interop */
	enum yn_options fake_strongswan;

	/* send our own libreswan vendorid or not */
	enum yn_options send_vendorid;

	/* Checking if this connection is configured by Network Manager */
	const char *nm_configured;

	/* XAUTH Authentication can be file (default) PAM or 'alwaysok' */
	enum xauthby xauthby;

	/* XAUTH failure mode can be hard (default) or soft */
	enum xauthfail xauthfail;
	const char *sendca;

	/* Force the MTU for this connection */
	const char *mtu;
	const char *priority;
	const char *tfc;
	enum yn_options send_esp_tfc_padding_not_supported;

	enum yn_options iptfs;
	enum yn_options iptfs_fragmentation;
	const char *iptfs_packet_size; /* 0 for PMTU */
	const char *iptfs_max_queue_size;
	const char *iptfs_reorder_window;
	deltatime_t iptfs_drop_time;
	deltatime_t iptfs_init_delay;

	const char *reqid;
	const char *nflog_group;

	const char *sec_label;	/* sec_label string (if any) -- decoded by pluto */

	struct whack_end end[END_ROOF];

	const char *hostaddrfamily;

	const char *ike;		/* ike algo string (separated by commas) */
	enum encap_proto phase2;	/* outer protocol: ESP|AH */
	const char *phase2alg;		/* outer protocol: ESP|AH */
	const char *esp;		/* esp algo string (separated by commas) */
	const char *ah;			/* esp algo string (separated by commas) */

	/* for WHACK_KEY: */
	bool whack_key;
	bool whack_addkey;
	const char *keyid;	/* string 8 */
	enum ipseckey_algorithm_type pubkey_alg;
	const char *pubkey;

	/* for REMOTE_HOST */
	const char *remote_host;

	/* for WHACK_DELETESTATE: */
	long unsigned int whack_deletestateno;

	/* for WHACK_NFLOG_GROUP: */
	long unsigned int whack_nfloggroup;

	/* for WHACK_CRASH - note if a remote peer is known to have rebooted */
	ip_address whack_crash_peer;

	/* for WHACK_LIST */
	bool whack_utc;
	lset_t whack_list;

	/* for connalias string */
	const char *connalias;

	/* for IKEv1 MODECFG and IKEv2 CP */
	const char *modecfgdns;
	const char *modecfgdomains;
	const char *modecfgbanner;

	const char *mark;
	const char *mark_in;
	const char *mark_out;

	const char *vti_interface;
	enum yn_options vti_routing;	/* perform routing into vti
					 * device or not */
	enum yn_options vti_shared;	/* use remote %any and skip
					 * cleanup on down? */

	/* RFC 8784 and draft-ietf-ipsecme-ikev2-qr-alt-04 */
	const char *ppk_ids;

	/* for RFC 5685 - IKEv2 Redirect mechanism */
	enum global_redirect global_redirect;
	const char *redirect_to;	/* used by WHACK_ADD,
				 * WHACK_ACTIVE_REDIRECT and
				 * WHACK_GLOBAL_REDIRECT */
	enum yn_options accept_redirect;
	const char *accept_redirect_to;
	enum yna_options send_redirect;

	/* what metric to put on ipsec routes */
	int metric;

	/* space for strings (hope there is enough room) */
	size_t str_size;
	unsigned char string[4096];
};

/*
 * Options of whack --list*** command
 *
 * These should be kept in order of option_enums LST_ values
 */
enum whack_list {
	LIST_PUBKEYS,	/* list all public keys */
	LIST_CERTS,	/* list all host/user certs */
	LIST_CACERTS,	/* list all ca certs */
	LIST_CRLS,	/* list all crls */
	LIST_PSKS,	/* list all preshared keys (by name) */
	LIST_EVENTS,	/* list all queued events */
};

/* omit events from listing options */
#define LIST_ALL	LRANGE(LIST_PUBKEYS, LIST_PSKS)  /* almost all list options */

struct whackpacker {
	struct whack_message *msg;
	unsigned char *str_roof;
	unsigned char *str_next;
	size_t n;
};

extern err_t pack_whack_msg(struct whackpacker *wp, struct logger *logger);
extern diag_t unpack_whack_msg(struct whackpacker *wp, struct logger *logger);

int whack_send_msg(struct whack_message *msg, const char *ctlsocket,
		   char xauthusername[MAX_XAUTH_USERNAME_LEN],
		   char xauthpass[XAUTH_MAX_PASS_LENGTH],
		   int usernamelen, int xauthpasslen,
		   struct logger *logger);

extern bool lsw_alias_cmp(const char *name, const char *aliases);

extern unsigned whack_magic(void);

#endif /* WHACK_H */
