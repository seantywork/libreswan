/*
 * Libreswan config file parser (keywords.c)
 * Copyright (C) 2003-2006 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2007-2010 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2012 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2013-2019 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2013-2019 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2013 David McCullough <ucdevel@gmail.com>
 * Copyright (C) 2013-2016 Antony Antony <antony@phenome.org>
 * Copyright (C) 2016-2022 Andrew Cagney
 * Copyright (C) 2017 Mayank Totale <mtotale@gmail.com>
 * Copyright (C) 2020 Yulia Kuzovkova <ukuzovkova@gmail.com>
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

#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <sys/socket.h>		/* for AF_INET/AF_INET6 */

#include "constants.h"
#include "lmod.h"
#include "lswcdefs.h"		/* for elemsof() */
#include "sparse_names.h"
#include "encap_proto.h"
#include "lswalloc.h"

#include "ipsecconf/keywords.h"
#include "ipsecconf/parser.h"	/* includes parser.tab.h generated by bison; requires keywords.h */

/*
 * Values for failureshunt={passthrough, drop, reject, none}
 */

static const struct sparse_names kw_failureshunt_names = {
	.list = {
		SPARSE("none",        SHUNT_NONE),
		SPARSE("passthrough", SHUNT_PASS),
		SPARSE("drop",        SHUNT_DROP),
		SPARSE("hold",        SHUNT_DROP), /* alias */
		SPARSE("reject",      SHUNT_REJECT),
		SPARSE_NULL
	},
};

/*
 * Values for negotiationshunt={passthrough, hold}
 */

static const struct sparse_names kw_negotiationshunt_names = {
	.list = {
		SPARSE("passthrough", SHUNT_PASS),
		SPARSE("drop",        SHUNT_HOLD), /* alias */
		SPARSE("hold",        SHUNT_HOLD),
		SPARSE_NULL
	},
};

/*
 * Common aliases for YES and NO, add this to a keyword list so all
 * are accepted.
 *
 * This list does not include "0" or "1" as they, for things like
 * yndev have special meanings.
 */
#define YES_NO(YES, NO)				\
	SPARSE("yes",        YES),		\
		SPARSE("no",         NO),	\
		SPARSE("true",       YES),	\
		SPARSE("false",      NO),	\
		SPARSE("on",         YES),	\
		SPARSE("off",        NO),	\
		SPARSE("y",          YES),	\
		SPARSE("n",          NO)

/*
 * Values for keyexchange= and ikev2=
 *
 * The ikev2= keyword, which was originally very flexible, has been
 * reduced to a boolean.  Retain original keywords for backwards
 * compatibility for now.
 */

static const struct sparse_names kw_keyexchange_names = {
	.list = {
		SPARSE("ike",  IKE_VERSION_ROOF),
		SPARSE("ikev1", IKEv1),
		SPARSE("ikev2", IKEv2),
		SPARSE_NULL
	},
};

static const struct sparse_names kw_ikev2_names = {
	.list = {
		/* from fo_{never,permit,propose,insist} */
		SPARSE("never",     YN_NO),
		SPARSE("propose",   YN_YES),	/* originally: initiate IKEv2,
						 * but allow downgrade to
						 * IKEv1; accept IKEv1 or
						 * IKEv2 */
		SPARSE("permit",    YN_NO),		/* reverse of propose:
						 * initiate IKEv1, but allow
						 * upgrade to IKEv2; accept
						 * IKEv1 or IKEv2? */
		SPARSE("insist",    YN_YES),
		YES_NO(YN_YES, YN_NO),
		SPARSE("always",    YN_YES),
		SPARSE_NULL
	},
};

static const struct sparse_names kw_ddos_names = {
	.list = {
		SPARSE("auto",      DDOS_AUTO),
		SPARSE("busy",      DDOS_FORCE_BUSY),
		SPARSE("unlimited", DDOS_FORCE_UNLIMITED),
		SPARSE_NULL
	},
};

#ifdef USE_SECCOMP
static const struct sparse_names kw_seccomp_names = {
	.list = {
		SPARSE("enabled", SECCOMP_ENABLED),
		SPARSE("disabled", SECCOMP_DISABLED),
		SPARSE("tolerant", SECCOMP_TOLERANT),
		SPARSE_NULL
	},
};
#endif

static const struct sparse_names kw_auth_names = {
	.list = {
		SPARSE("never",     AUTH_NEVER),
		SPARSE("secret",    AUTH_PSK),
		SPARSE("rsasig",    AUTH_RSASIG),
		SPARSE("rsa",       AUTH_RSASIG), /* alias */
		SPARSE("ecdsa",     AUTH_ECDSA),
		SPARSE("null",      AUTH_NULL),
		SPARSE("eaponly",   AUTH_EAPONLY),
		SPARSE_NULL
	},
};

/*
 * Values for sendca={none,issuer,all}
 */

static const struct sparse_names kw_sendca_names = {
	.list = {
		SPARSE("none",	CA_SEND_NONE),
		SPARSE("issuer",	CA_SEND_ISSUER),
		SPARSE("all",	CA_SEND_ALL),
		SPARSE_NULL
	},
};

/*
 * Values for addrfamily={ipv4,ipv6}
 */
static const struct sparse_names kw_addrfamily_names = {
	.list = {
		SPARSE("ipv4",  AF_INET),
		SPARSE("ipv6",  AF_INET6),
		/* aliases - undocumented on purpose */
		SPARSE("v4",    AF_INET),
		SPARSE("inet",  AF_INET),
		SPARSE("v6",    AF_INET6),
		SPARSE("inet6", AF_INET6),
		SPARSE_NULL
	},
};

/*
 * Values for {rsasigkey,ecdsakey,pubkey}={ %cert, %dnsondemand, %dns, literal }
 */
static const struct sparse_names kw_pubkey_names = {
	.list = {
		SPARSE("",             PUBKEY_PREEXCHANGED),
		SPARSE("%cert",        PUBKEY_CERTIFICATE),
#ifdef USE_DNSSEC
		SPARSE("%dns",         PUBKEY_DNSONDEMAND),
		SPARSE("%dnsondemand", PUBKEY_DNSONDEMAND),
#endif
		SPARSE_NULL
	},
};

/*
 *  Cisco interop: remote peer type
 */

static const struct sparse_names kw_remote_peer_type_names = {
	.list = {
		SPARSE("cisco",         REMOTE_PEER_CISCO),
		SPARSE_NULL
	},
};

static const struct sparse_names kw_xauthby_names = {
	.list = {
		SPARSE("file",	XAUTHBY_FILE),
#ifdef USE_PAM_AUTH
		SPARSE("pam",	XAUTHBY_PAM),
#endif
		SPARSE("alwaysok",	XAUTHBY_ALWAYSOK),
		SPARSE_NULL
	},
};

static const struct sparse_names kw_xauthfail_names = {
	.list = {
		SPARSE("hard",         XAUTHFAIL_HARD),
		SPARSE("soft",         XAUTHFAIL_SOFT),
		SPARSE_NULL
	},
};

/*
 * Values for right= and left=
 */

static struct sparse_names kw_host_names = {
	.list = {
		SPARSE("%defaultroute",  KH_DEFAULTROUTE),
		SPARSE("%any",           KH_ANY),
		SPARSE("%",              KH_IFACE),
		SPARSE("%oppo",          KH_OPPO),
		SPARSE("%opportunistic", KH_OPPO),
		SPARSE("%opportunisticgroup", KH_OPPOGROUP),
		SPARSE("%oppogroup",     KH_OPPOGROUP),
		SPARSE("%group",         KH_GROUP),
		SPARSE("%hostname",      KH_IPHOSTNAME), /* makes no sense on input */
		SPARSE_NULL
	},
};

static const struct sparse_names kw_phase2types_names = {
	.list = {
		/* note: these POLICY bits happen to fit in an unsigned int */
		/* note2: ah+esp is no longer supported as per RFC-8221 Section 4 */
		SPARSE("esp",      ENCAP_PROTO_ESP),
		SPARSE("ah",       ENCAP_PROTO_AH),
		SPARSE("default",  ENCAP_PROTO_UNSET), /* i.e., let pluto decide */
		SPARSE_NULL
	},
};

/*
 * Values for {left/right}sendcert={never,sendifasked,always,forcedtype}
 */

static const struct sparse_names kw_sendcert_names = {
	.list = {
		SPARSE("never",        CERT_NEVERSEND),
		SPARSE("sendifasked",  CERT_SENDIFASKED),
		SPARSE("alwayssend",   CERT_ALWAYSSEND),
		SPARSE("always",       CERT_ALWAYSSEND),
		SPARSE_NULL
	},
};

/*
 * Values for ocsp-method={get|post}
 *
 * This sets the NSS forcePost option for the OCSP request.
 * If forcePost is set, OCSP requests will only be sent using the HTTP POST
 * method. When forcePost is not set, OCSP requests will be sent using the
 * HTTP GET method, with a fallback to POST when we fail to receive a response
 * and/or when we receive an uncacheable response like "Unknown".
 */

static const struct sparse_names kw_ocsp_method_names = {
	.list = {
		SPARSE("get",      OCSP_METHOD_GET),
		SPARSE("post",     OCSP_METHOD_POST),
		SPARSE_NULL
	},
};

static const struct sparse_names kw_global_ikev1_names = {
	.list = {
		SPARSE("accept",   GLOBAL_IKEv1_ACCEPT),
		SPARSE("reject",   GLOBAL_IKEv1_REJECT),
		SPARSE("drop",     GLOBAL_IKEv1_DROP),
		SPARSE_NULL
	},
};

static const struct sparse_names kw_eap_names = {
	.list = {
		SPARSE("none", IKE_EAP_NONE), /* default */
		SPARSE("tls", IKE_EAP_TLS),
		SPARSE_NULL
	},
};

/* MASTER KEYWORD LIST
 * Note: this table is terminated by an entry with keyname == NULL.
 */

const struct keyword_def ipsec_conf_keywords[] = {
  { "ikev1-policy",  kv_config,  kt_sparse_name,  KBF_GLOBAL_IKEv1, &kw_global_ikev1_names, NULL, },
  { "curl-iface",  kv_config,  kt_string,  KSF_CURLIFACE, NULL, NULL, },
  { "curl-timeout",  kv_config,  kt_seconds,  KBF_CURL_TIMEOUT_SECONDS, NULL, NULL, },

  { "myvendorid",  kv_config,  kt_string,  KSF_MYVENDORID, NULL, NULL, },
  { "syslog",  kv_config,  kt_string,  KSF_SYSLOG, NULL, NULL, },

  { "plutodebug",  kv_config|kv_conn, kt_lset, KW_DEBUG, NULL, &debug_lmod_info, },
  { "debug",       kv_config|kv_conn, kt_lset, KW_DEBUG, NULL, &debug_lmod_info, },

  { "logfile",  kv_config,  kt_filename,  KSF_LOGFILE, NULL, NULL, },
  { "plutostderrlog",  kv_config,  kt_filename,  KSF_LOGFILE, NULL, NULL, }, /* obsolete name, but very common :/ */
  { "logtime",  kv_config,  kt_bool,  KBF_LOGTIME, NULL, NULL, },
  { "logappend",  kv_config,  kt_bool,  KBF_LOGAPPEND, NULL, NULL, },
  { "logip",  kv_config,  kt_bool,  KBF_LOGIP, NULL, NULL, },
  { "audit-log",  kv_config,  kt_bool,  KBF_AUDIT_LOG, NULL, NULL, },
#ifdef USE_DNSSEC
  { "dnssec-enable",  kv_config,  kt_bool,  KBF_DO_DNSSEC, NULL, NULL, },
  { "dnssec-rootkey-file",  kv_config,  kt_filename, KSF_PLUTO_DNSSEC_ROOTKEY_FILE, NULL, NULL, },
  { "dnssec-anchors",  kv_config,  kt_filename, KSF_PLUTO_DNSSEC_ANCHORS, NULL, NULL, },
#endif
  { "dumpdir",  kv_config,  kt_dirname,  KSF_DUMPDIR, NULL, NULL, },
  { "ipsecdir",  kv_config,  kt_dirname,  KSF_IPSECDIR, NULL, NULL, },
  { "nssdir", kv_config, kt_dirname, KSF_NSSDIR, NULL, NULL, },
  { "secretsfile",  kv_config,  kt_dirname,  KSF_SECRETSFILE, NULL, NULL, },
  { "statsbin",  kv_config,  kt_dirname,  KSF_STATSBINARY, NULL, NULL, },
  { "uniqueids",  kv_config,  kt_bool,  KBF_UNIQUEIDS, NULL, NULL, },
  { "shuntlifetime",  kv_config,  kt_seconds,  KBF_SHUNTLIFETIME, NULL, NULL, },
  { "global-redirect", kv_config, kt_string, KSF_GLOBAL_REDIRECT, NULL, NULL },
  { "global-redirect-to", kv_config, kt_string, KSF_GLOBAL_REDIRECT_TO, NULL, NULL, },

  { "crl-strict",  kv_config,  kt_bool,  KBF_CRL_STRICT, NULL, NULL, },
  { "crlcheckinterval",  kv_config,  kt_seconds,  KBF_CRL_CHECKINTERVAL, NULL, NULL, },

  { "ocsp-strict",  kv_config,  kt_bool,  KBF_OCSP_STRICT, NULL, NULL, },
  { "ocsp-enable",  kv_config,  kt_bool,  KBF_OCSP_ENABLE, NULL, NULL, },
  { "ocsp-uri",  kv_config,  kt_string,  KSF_OCSP_URI, NULL, NULL, },
  { "ocsp-timeout",  kv_config,  kt_seconds,  KBF_OCSP_TIMEOUT_SECONDS, NULL, NULL, },
  { "ocsp-trustname",  kv_config,  kt_string,  KSF_OCSP_TRUSTNAME, NULL, NULL, },
  { "ocsp-cache-size",  kv_config,  kt_unsigned,  KBF_OCSP_CACHE_SIZE, NULL, NULL, },
  { "ocsp-cache-min-age",  kv_config,  kt_seconds,  KBF_OCSP_CACHE_MIN_AGE_SECONDS, NULL, NULL, },
  { "ocsp-cache-max-age",  kv_config,  kt_seconds,  KBF_OCSP_CACHE_MAX_AGE_SECONDS, NULL, NULL, },
  { "ocsp-method",  kv_config | kv_processed,  kt_sparse_name,  KBF_OCSP_METHOD, &kw_ocsp_method_names, NULL, },

  { "ddos-mode",  kv_config | kv_processed ,  kt_sparse_name,  KBF_DDOS_MODE, &kw_ddos_names, NULL, },
#ifdef USE_SECCOMP
  { "seccomp",  kv_config | kv_processed ,  kt_sparse_name,  KBF_SECCOMP,  &kw_seccomp_names, NULL, },
#endif
  { "ddos-ike-threshold",  kv_config,  kt_unsigned,  KBF_DDOS_IKE_THRESHOLD, NULL, NULL, },
  { "max-halfopen-ike",  kv_config,  kt_unsigned,  KBF_MAX_HALFOPEN_IKE, NULL, NULL, },
  { "ike-socket-bufsize",  kv_config,  kt_unsigned,  KBF_IKEBUF, NULL, NULL, },
  { "ike-socket-errqueue",  kv_config,  kt_bool,  KBF_IKE_ERRQUEUE, NULL, NULL, },
#if defined(USE_NFLOG)
  { "nflog-all",  kv_config,  kt_unsigned,  KBF_NFLOG_ALL, NULL, NULL, },
#endif
#ifdef XFRM_LIFETIME_DEFAULT
  { "xfrmlifetime",  kv_config,  kt_unsigned,  KBF_XFRMLIFETIME, NULL, NULL, },
#endif
  { "virtual-private",  kv_config,  kt_string,  KSF_VIRTUALPRIVATE, NULL, NULL, },
  { "virtual_private",  kv_config,  kt_string,  KSF_VIRTUALPRIVATE, NULL, NULL, }, /* obsolete variant, very common */
  { "seedbits",  kv_config,  kt_unsigned,  KBF_SEEDBITS, NULL, NULL, },
  { "keep-alive",  kv_config,  kt_seconds,  KBF_KEEP_ALIVE, NULL, NULL, },

  { "listen-tcp", kv_config, kt_bool, KBF_LISTEN_TCP, NULL, NULL },
  { "listen-udp", kv_config, kt_bool, KBF_LISTEN_UDP, NULL, NULL },

  { "listen",  kv_config,  kt_string,  KSF_LISTEN, NULL, NULL, },
  { "protostack",  kv_config,  kt_string,  KSF_PROTOSTACK,  NULL, NULL, },
  { "nhelpers",  kv_config,  kt_unsigned,  KBF_NHELPERS, NULL, NULL, },
  { "drop-oppo-null",  kv_config,  kt_bool,  KBF_DROP_OPPO_NULL, NULL, NULL, },
  { "interfaces",  kv_config, kt_obsolete, KNCF_OBSOLETE, NULL, NULL, }, /* obsoleted but often present keyword */

  /* these options are obsoleted (and not old aliases) */

  /*
   * This is "left=" and "right="
   */
  { "",  kv_conn | kv_leftright| kv_processed,  kt_host,  KW_IP, &kw_host_names, NULL, },

  { "subnet",  kv_conn | kv_leftright | kv_processed,  kt_subnet,  KSCF_SUBNET, NULL, NULL, },
  { "subnets",  kv_conn | kv_leftright,  kt_appendlist,  KSCF_SUBNETS, NULL, NULL, },
  { "sourceip",  kv_conn | kv_leftright,  kt_ipaddr,  KSCF_SOURCEIP, NULL, NULL, },
  { "ikeport",  kv_conn | kv_leftright,  kt_unsigned,  KNCF_IKEPORT, NULL, NULL, },
  { "interface-ip", kv_conn | kv_leftright,  kt_subnet, KSCF_INTERFACE_IP, NULL, NULL, },
  { "vti",  kv_conn | kv_leftright | kv_processed,  kt_subnet,  KSCF_VTI_IP, NULL, NULL, },
  { "nexthop",  kv_conn | kv_leftright,  kt_ipaddr,  KW_NEXTHOP, NULL, NULL, },
  { "updown",  kv_conn | kv_leftright,  kt_filename,  KSCF_UPDOWN, NULL, NULL, },
  { "id",  kv_conn | kv_leftright,  kt_idtype,  KSCF_ID, NULL, NULL, },
  { "rsasigkey",  kv_conn | kv_leftright,  kt_pubkey,  KW_RSASIGKEY, &kw_pubkey_names, NULL, },
  { "ecdsakey",  kv_conn | kv_leftright,  kt_pubkey,  KW_ECDSAKEY, &kw_pubkey_names, NULL, },
  { "pubkey",  kv_conn | kv_leftright,  kt_pubkey,  KW_PUBKEY, &kw_pubkey_names, NULL, },
  { "cert",  kv_conn | kv_leftright,  kt_filename,  KSCF_CERT, NULL, NULL, },
  { "ckaid",  kv_conn | kv_leftright,  kt_string,  KSCF_CKAID, NULL, NULL, },
  { "sendcert",  kv_conn | kv_leftright,  kt_sparse_name,  KNCF_SENDCERT, &kw_sendcert_names, NULL, },
  { "ca",  kv_conn | kv_leftright,  kt_string,  KSCF_CA, NULL, NULL, },
  { "xauthserver",  kv_conn | kv_leftright,  kt_bool,  KNCF_XAUTHSERVER, NULL, NULL, },
  { "xauthclient",  kv_conn | kv_leftright,  kt_bool,  KNCF_XAUTHCLIENT, NULL, NULL, },
  { "modecfgserver",  kv_conn | kv_leftright,  kt_sparse_name,  KNCF_MODECONFIGSERVER, &yn_option_names, NULL, },
  { "modecfgclient",  kv_conn | kv_leftright,  kt_sparse_name,  KNCF_MODECONFIGCLIENT, &yn_option_names, NULL, },
  { "username",  kv_conn | kv_leftright,  kt_string,  KSCF_USERNAME, NULL, NULL, },
  /* xauthusername is still used in NetworkManager-libreswan :/ */
  { "xauthusername",  kv_conn | kv_leftright,  kt_string,  KSCF_USERNAME, NULL, NULL, }, /* old alias */
  { "addresspool",  kv_conn | kv_leftright,  kt_range,  KSCF_ADDRESSPOOL, NULL, NULL, },
  { "auth",  kv_conn | kv_leftright, kt_sparse_name,  KNCF_AUTH, &kw_auth_names, NULL, },
#if defined(USE_CAT)
  { "cat",  kv_conn | kv_leftright,  kt_sparse_name,  KNCF_CAT, &yn_option_names, NULL, },
#endif
  { "protoport",  kv_conn | kv_leftright | kv_processed,  kt_string,  KSCF_PROTOPORT, NULL, NULL, },
  { "autheap",  kv_conn | kv_leftright,  kt_sparse_name,  KNCF_EAP, &kw_eap_names, NULL, },
  { "groundhog",  kv_conn | kv_leftright,  kt_sparse_name,  KSCF_GROUNDHOG, &yn_option_names, NULL, },

  /* these are conn statements which are not left/right */

  { "auto",  kv_conn,  kt_sparse_name,  KNCF_AUTO, &autostart_names, NULL, },
  { "also",  kv_conn | kv_duplicateok,  kt_also,  KSCF_ALSO, NULL, NULL, },
  { "ike",  kv_conn,  kt_string,  KSCF_IKE, NULL, NULL, },
  { "hostaddrfamily",  kv_conn,  kt_sparse_name,  KNCF_HOSTADDRFAMILY, &kw_addrfamily_names, NULL, },
  { "clientaddrfamily",  kv_conn,  kt_sparse_name,  KNCF_CLIENTADDRFAMILY, &kw_addrfamily_names, NULL, },
  { "type",  kv_conn,  kt_sparse_name,  KNCF_TYPE, &type_option_names, NULL, },
  { "authby",  kv_conn,  kt_string,  KSCF_AUTHBY, NULL, NULL, },
  { "keyexchange",  kv_conn,  kt_sparse_name,  KNCF_KEYEXCHANGE, &kw_keyexchange_names, NULL, },
  { "ikev2",  kv_conn | kv_processed,  kt_sparse_name,  KNCF_IKEv2, &kw_ikev2_names, NULL, },
  { "ppk", kv_conn | kv_processed, kt_sparse_name, KNCF_PPK, &nppi_option_names, NULL, },
  { "ppk-ids", kv_conn | kv_processed, kt_string, KSCF_PPK_IDS, NULL, NULL, },
  { "intermediate",  kv_conn | kv_processed, kt_sparse_name, KNCF_INTERMEDIATE, &yn_option_names, NULL, },
  { "esn",  kv_conn | kv_processed,  kt_sparse_name,  KNCF_ESN, &yne_option_names, NULL, },
  { "decap-dscp",  kv_conn | kv_processed,  kt_sparse_name,  KNCF_DECAP_DSCP, &yn_option_names, NULL, },
  { "encap-dscp",  kv_conn | kv_processed,  kt_sparse_name,  KNCF_ENCAP_DSCP, &yn_option_names, NULL, },
  { "nopmtudisc",  kv_conn | kv_processed,  kt_sparse_name,  KNCF_NOPMTUDISC, &yn_option_names, NULL, },
  { "fragmentation",  kv_conn | kv_processed,  kt_sparse_name,  KNCF_FRAGMENTATION, &ynf_option_names, NULL, },
  { "mobike",  kv_conn,  kt_sparse_name,  KNCF_MOBIKE, &yn_option_names, NULL, },
  { "narrowing",  kv_conn,  kt_sparse_name,  KNCF_NARROWING, &yn_option_names, NULL, },
  { "pam-authorize",  kv_conn,  kt_sparse_name,  KNCF_PAM_AUTHORIZE, &yn_option_names, NULL, },
  { "send-redirect",  kv_conn,  kt_sparse_name,  KNCF_SEND_REDIRECT, &yna_option_names, NULL, },
  { "redirect-to",  kv_conn,  kt_string,  KSCF_REDIRECT_TO, NULL, NULL, },
  { "accept-redirect",  kv_conn,  kt_sparse_name, KNCF_ACCEPT_REDIRECT, &yn_option_names, NULL, },
  { "accept-redirect-to",  kv_conn,  kt_string, KSCF_ACCEPT_REDIRECT_TO, NULL, NULL, },
  { "pfs",  kv_conn,  kt_sparse_name,  KNCF_PFS, &yn_option_names, NULL, },
  { "session-resumption",  kv_conn,  kt_sparse_name,  KNCF_SESSION_RESUMPTION, &yn_option_names, NULL, },

  { "nat-keepalive",  kv_conn,  kt_bool,  KNCF_NAT_KEEPALIVE, NULL, NULL, },

  { "initial-contact",  kv_conn,  kt_bool,  KNCF_INITIAL_CONTACT, NULL, NULL, },
  { "cisco-unity",  kv_conn,  kt_bool,  KNCF_CISCO_UNITY, NULL, NULL, },
  { "send-no-esp-tfc",  kv_conn,  kt_bool,  KNCF_NO_ESP_TFC, NULL, NULL, },

  { "iptfs",  kv_conn,  kt_sparse_name,  KNCF_IPTFS, &yn_option_names, NULL, },
  { "iptfs-fragmentation",  kv_conn,  kt_sparse_name,  KNCF_IPTFS_FRAGMENTATION, &yn_option_names, NULL, },
  { "iptfs-packet-size",  kv_conn,  kt_binary,  KNCF_IPTFS_PACKET_SIZE, NULL, NULL, },
  { "iptfs-max-queue-size",  kv_conn,  kt_binary,  KNCF_IPTFS_MAX_QUEUE_SIZE, NULL, NULL, },
  { "iptfs-reorder-window",  kv_conn,  kt_binary,  KNCF_IPTFS_REORDER_WINDOW, NULL, NULL, },
  { "iptfs-init-delay",  kv_conn,  kt_seconds,  KNCF_IPTFS_INIT_DELAY, NULL, NULL, },
  { "iptfs-drop-time",  kv_conn,  kt_seconds,  KNCF_IPTFS_DROP_TIME, NULL, NULL, },

  { "fake-strongswan",  kv_conn,  kt_bool,  KNCF_VID_STRONGSWAN, NULL, NULL, },
  { "send-vendorid",  kv_conn,  kt_bool,  KNCF_SEND_VENDORID, NULL, NULL, },
  { "sha2-truncbug",  kv_conn,  kt_sparse_name,  KNCF_SHA2_TRUNCBUG, &yn_option_names, NULL, },
  { "ms-dh-downgrade",  kv_conn,  kt_sparse_name,  KNCF_MS_DH_DOWNGRADE, &yn_option_names, NULL, },
  { "pfs-rekey-workaround",  kv_conn,  kt_sparse_name,  KNCF_PFS_REKEY_WORKAROUND, &yn_option_names, NULL, },
  { "require-id-on-certificate",  kv_conn,  kt_sparse_name,  KNCF_REQUIRE_ID_ON_CERTIFICATE, &yn_option_names, NULL, },
  { "dns-match-id,",  kv_conn,  kt_sparse_name,  KNCF_DNS_MATCH_ID, &yn_option_names, NULL, },
  { "ipsec-max-bytes",  kv_conn,  kt_byte,  KNCF_IPSEC_MAXBYTES, NULL, NULL, },
  { "ipsec-lifetime",  kv_conn,  kt_seconds,  KNCF_IPSEC_LIFETIME, NULL, NULL, },
  { "keylife",  kv_conn | kv_alias,  kt_seconds,  KNCF_IPSEC_LIFETIME, NULL, NULL, }, /* old name */
  { "lifetime",  kv_conn | kv_alias,  kt_seconds,  KNCF_IPSEC_LIFETIME, NULL, NULL, }, /* old name */
  { "salifetime",  kv_conn,  kt_seconds,  KNCF_IPSEC_LIFETIME, NULL, NULL, }, /* old name */
  { "ipsec-max-packets",  kv_conn,  kt_binary,  KNCF_IPSEC_MAXPACKETS, NULL, NULL, },

  { "retransmit-timeout",  kv_conn,  kt_seconds,  KNCF_RETRANSMIT_TIMEOUT, NULL, NULL, },
  { "retransmit-interval",  kv_conn,  kt_milliseconds,  KNCF_RETRANSMIT_INTERVAL, NULL, NULL, },

  { "ikepad",  kv_conn,  kt_sparse_name,  KNCF_IKEPAD, &yna_option_names, NULL, },
  { "nat-ikev1-method",  kv_conn | kv_processed,  kt_sparse_name,  KNCF_NAT_IKEv1_METHOD, &nat_ikev1_method_option_names, NULL, },

  { "ikev1-secctx-attr-type",  kv_config,  kt_obsolete,  KNCF_OBSOLETE, NULL, NULL, },  /* obsolete: not a value, a type */
  { "secctx-attr-type",  kv_config,  kt_obsolete,  KNCF_OBSOLETE, NULL, NULL, },
  { "policy-label",  kv_conn,  kt_string,  KSCF_SEC_LABEL, NULL, NULL, }, /* obsolete variant */
  { "sec-label",  kv_conn,  kt_string,  KSCF_SEC_LABEL, NULL, NULL, },

  /* Cisco interop: remote peer type */
  { "remote-peer-type",  kv_conn,  kt_sparse_name,  KNCF_REMOTE_PEER_TYPE, &kw_remote_peer_type_names, NULL, },
  /* another alias used by NetworkManager-libreswan :/ */
  { "remote_peer_type",  kv_conn,  kt_sparse_name,  KNCF_REMOTE_PEER_TYPE, &kw_remote_peer_type_names, NULL, },

  /* Network Manager support */
#ifdef HAVE_NM
  { "nm-configured",  kv_conn,  kt_sparse_name,  KNCF_NM_CONFIGURED, &yn_option_names, NULL, },
#endif

  { "xauthby",  kv_conn,  kt_sparse_name,  KNCF_XAUTHBY, &kw_xauthby_names, NULL, },
  { "xauthfail",  kv_conn,  kt_sparse_name,  KNCF_XAUTHFAIL, &kw_xauthfail_names, NULL, },
  { "modecfgpull",  kv_conn,  kt_sparse_name,  KNCF_MODECFGPULL, &yn_option_names, NULL, },
  { "modecfgdns",  kv_conn,  kt_string,  KSCF_MODECFGDNS, NULL, NULL, },
  { "modecfgdomains",  kv_conn,  kt_string,  KSCF_MODECFGDOMAINS, NULL, NULL, },
  { "modecfgbanner",  kv_conn,  kt_string,  KSCF_MODECFGBANNER, NULL, NULL, },
  { "ignore-peer-dns",  kv_conn,  kt_sparse_name,  KNCF_IGNORE_PEER_DNS, &yn_option_names, NULL, },
  { "mark",  kv_conn,  kt_string,  KSCF_MARK, NULL, NULL, },
  { "mark-in",  kv_conn,  kt_string,  KSCF_MARK_IN, NULL, NULL, },
  { "mark-out",  kv_conn,  kt_string,  KSCF_MARK_OUT, NULL, NULL, },
  { "vti-interface",  kv_conn,  kt_string,  KSCF_VTI_INTERFACE, NULL, NULL, },
  { "vti-routing",  kv_conn,  kt_sparse_name,  KNCF_VTI_ROUTING, &yn_option_names, NULL, },
  { "vti-shared",  kv_conn,  kt_sparse_name,  KNCF_VTI_SHARED, &yn_option_names, NULL, },
  { "ipsec-interface-managed", kv_config, kt_sparse_name, KWYN_IPSEC_INTERFACE_MANAGED, &yn_option_names, NULL, },
  { "ipsec-interface", kv_conn, kt_string, KWS_IPSEC_INTERFACE, NULL, NULL, },

  { "nic-offload",  kv_conn,  kt_sparse_name,  KNCF_NIC_OFFLOAD, &nic_offload_option_names, NULL, },

  { "encapsulation",  kv_conn,  kt_sparse_name,  KNCF_ENCAPSULATION, &yna_option_names, NULL, },

  { "overlapip",  kv_conn,  kt_sparse_name,  KNCF_OVERLAPIP, &yn_option_names, NULL, },
  { "reauth",  kv_conn,  kt_sparse_name,  KNCF_REAUTH, &yn_option_names, NULL, },
  { "rekey",  kv_conn,  kt_sparse_name,  KNCF_REKEY, &yn_option_names, NULL, },
  { "rekeymargin",  kv_conn,  kt_seconds,  KNCF_REKEYMARGIN, NULL, NULL, },
  { "rekeyfuzz",  kv_conn,  kt_percent,  KNCF_REKEYFUZZ, NULL, NULL, },
  { "keyingtries",  kv_conn,  kt_unsigned,  KNCF_KEYINGTRIES, NULL, NULL, },
  { "replay-window",  kv_conn,  kt_unsigned,  KNCF_REPLAY_WINDOW, NULL, NULL, },
  { "ikelifetime",  kv_conn,  kt_seconds,  KNCF_IKELIFETIME, NULL, NULL, },
  { "failureshunt",  kv_conn,  kt_sparse_name,  KNCF_FAILURESHUNT, &kw_failureshunt_names, NULL, },
  { "negotiationshunt",  kv_conn,  kt_sparse_name,  KNCF_NEGOTIATIONSHUNT, &kw_negotiationshunt_names, NULL, },

  { "enable-tcp",  kv_conn, kt_sparse_name, KNCF_ENABLE_TCP, &tcp_option_names, NULL },
  { "tcp-remoteport",  kv_conn, kt_unsigned, KNCF_TCP_REMOTEPORT, NULL, NULL },

  { "connalias",  kv_conn | kv_processed,  kt_appendstring,  KSCF_CONNALIAS, NULL, NULL, },

  /* attributes of the phase2 policy */
  { "esp",  kv_conn,  kt_string,  KSCF_ESP, NULL, NULL, },
  { "ah",  kv_conn,  kt_string,  KSCF_ESP, NULL, NULL, },
  { "phase2alg",  kv_conn | kv_alias,  kt_string,  KSCF_ESP, NULL, NULL, },	/* obsolete */

  { "phase2",  kv_conn | kv_policy,  kt_sparse_name,  KNCF_PHASE2, &kw_phase2types_names, NULL, },

  { "compress",  kv_conn,  kt_sparse_name,  KNCF_COMPRESS, &yn_option_names, NULL, },

  /* route metric */
  { "metric",  kv_conn,  kt_unsigned,  KNCF_METRIC, NULL, NULL, },

  /* DPD */
  { "dpddelay",  kv_conn,  kt_string,  KSCF_DPDDELAY, NULL, NULL, },
  { "ikev1-dpdtimeout",  kv_conn,  kt_string,  KSCF_DPDTIMEOUT, NULL, NULL, },
  { "dpdtimeout",  kv_conn | kv_alias,  kt_string,  KSCF_DPDTIMEOUT, NULL, NULL, }, /* old name */
  { "dpdaction",  kv_conn,  kt_obsolete,  KNCF_OBSOLETE,  NULL, NULL, },

  { "sendca",      kv_conn,  kt_sparse_name,  KNCF_SEND_CA, &kw_sendca_names, NULL, },

  { "mtu",  kv_conn,  kt_unsigned,  KNCF_MTU, NULL, NULL, },
  { "priority",  kv_conn,  kt_unsigned,  KNCF_PRIORITY, NULL, NULL, },
  { "tfc",  kv_conn,  kt_unsigned,  KNCF_TFC, NULL, NULL, },
  { "reqid",  kv_conn,  kt_unsigned,  KNCF_REQID, NULL, NULL, },
#if defined(USE_NFLOG)
  { "nflog",  kv_conn,  kt_unsigned,  KNCF_NFLOG_CONN, NULL, NULL, },
#endif

  { "aggressive",  kv_conn,  kt_sparse_name,  KNCF_AGGRESSIVE, &yn_option_names, NULL, },
  /* alias for compatibility - undocumented on purpose */
  { "aggrmode",  kv_conn | kv_alias,  kt_sparse_name,  KNCF_AGGRESSIVE, &yn_option_names, NULL, },

  { NULL,  0,  0,  0, NULL, NULL, }
};
