/* address pool, for libreswan
 *
 * addresspool management functions used with left/rightaddresspool= option.
 * Currently used for IKEv1 XAUTH/ModeConfig options if we are an XAUTH server.
 * And in IKEv2 to respond to Configuration Payload (CP) request.
 *
 * Copyright (C) 2013 Antony Antony <antony@phenome.org>
 * Copyright (C) 2019-2020  Andrew Cagney
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

/* Address Pools
 *
 * With XAUTH/CP, we need a way to allocate an address to a client.
 * This address must be unique on our system.
 * The pools of addresses to be used are declared in our config file.
 * Each connection may specify a pool as a range of IPv4 or IPv6 addresses.
 * All pools must be non-everlapping, but each pool may be
 * used for more than one connection.
 */

#include "lswalloc.h"
#include "connections.h"
#include "defs.h"
#include "constants.h"
#include "addresspool.h"
#include "monotime.h"
#include "ip_address.h"
#include "ip_range.h"
#include "ip_info.h"
#include "log.h"
#include "refcnt.h"
#include "show.h"

#define SENTINEL (unsigned)-1
#define ENTRY_UNUSED (unsigned)-2

struct entry {
	unsigned prev;
	unsigned next;
};

static const struct entry empty_entry = {
	.prev = ENTRY_UNUSED,
	.next = ENTRY_UNUSED,
};

struct list {
	unsigned first;
	unsigned last;
	unsigned nr;
};

static const struct list empty_list = {
	.nr = 0,
	.first = SENTINEL,
	.last = SENTINEL,
};

#define IS_INSERTED(WHAT, LIST) (WHAT->LIST.prev != ENTRY_UNUSED || \
				 WHAT->LIST.next != ENTRY_UNUSED)

#define IS_EMPTY(WHAT, LIST)						\
	({								\
		bool empty_ = (WHAT->LIST.nr == 0);			\
		if (empty_) {						\
			passert(WHAT->LIST.first == SENTINEL);		\
			passert(WHAT->LIST.last == SENTINEL);		\
		} else {						\
			passert(WHAT->LIST.first != SENTINEL);		\
			passert(WHAT->LIST.first < pool->nr_leases);	\
			passert(WHAT->LIST.last != SENTINEL);		\
			passert(WHAT->LIST.last < pool->nr_leases);	\
		}							\
		empty_;							\
	})

#define HEAD(WHAT, LIST, ENTRY)						\
	({								\
		struct lease *result_;					\
		if (IS_EMPTY(WHAT, LIST)) {				\
			result_ = NULL;					\
		} else {						\
			result_ = &pool->leases[WHAT->LIST.first];	\
		}							\
		result_;						\
	})

#define REMOVE(WHAT, LIST, ENTRY, LEASE)				\
	{								\
		passert(IS_INSERTED(LEASE, ENTRY));			\
		unsigned index = LEASE - pool->leases;			\
		if (WHAT->LIST.first == index) {			\
			WHAT->LIST.first = LEASE->ENTRY.next;		\
		} else {						\
			/* not first; must have prev */			\
			passert(LEASE->ENTRY.prev != SENTINEL);		\
			passert(LEASE->ENTRY.prev < pool->nr_leases);	\
			pool->leases[LEASE->ENTRY.prev].ENTRY.next =	\
				LEASE->ENTRY.next;			\
		}							\
		if (WHAT->LIST.last == index) {				\
			WHAT->LIST.last = LEASE->ENTRY.prev;		\
		} else {						\
			/* not last; must have next */			\
			passert(LEASE->ENTRY.next != SENTINEL);		\
			passert(LEASE->ENTRY.next < pool->nr_leases);	\
			pool->leases[LEASE->ENTRY.next].ENTRY.prev =	\
				LEASE->ENTRY.prev;			\
		}							\
		LEASE->ENTRY.next = LEASE->ENTRY.prev = ENTRY_UNUSED;	\
		WHAT->LIST.nr--;					\
		passert(!IS_INSERTED(LEASE, ENTRY));			\
	}

#define FILL(WHAT, LIST, ENTRY, LEASE)					\
	{								\
		/* empty */						\
		unsigned index = LEASE - pool->leases;			\
		WHAT->LIST.first = WHAT->LIST.last = index;		\
		LEASE->ENTRY.next = LEASE->ENTRY.prev = SENTINEL;	\
	}

#define APPEND(WHAT, LIST, ENTRY, LEASE)				\
	{								\
		passert(!IS_INSERTED(LEASE, ENTRY));			\
		if (IS_EMPTY(WHAT, LIST)) {				\
			FILL(WHAT, LIST, ENTRY, LEASE);			\
		} else {						\
			unsigned index = LEASE - pool->leases;		\
			unsigned old_last = WHAT->LIST.last;		\
			LEASE->ENTRY.next = SENTINEL;			\
			LEASE->ENTRY.prev = old_last;			\
			pool->leases[old_last].ENTRY.next = index;	\
			WHAT->LIST.last = index;			\
		}							\
		WHAT->LIST.nr++;					\
		passert(IS_INSERTED(LEASE, ENTRY));			\
	}

#define PREPEND(WHAT, LIST, ENTRY, LEASE)				\
	{								\
		passert(!IS_INSERTED(LEASE, ENTRY));			\
		if (IS_EMPTY(WHAT, LIST)) {				\
			/* empty */					\
			FILL(WHAT, LIST, ENTRY, LEASE);			\
		} else {						\
			unsigned index = LEASE - WHAT->leases;		\
			unsigned old_first = WHAT->LIST.first;		\
			LEASE->ENTRY.next = old_first;			\
			LEASE->ENTRY.prev = SENTINEL;			\
			pool->leases[old_first].ENTRY.prev = index;	\
			WHAT->LIST.first = index;			\
		}							\
		WHAT->LIST.nr++;					\
		passert(IS_INSERTED(LEASE, ENTRY));			\
	}

/*
 * A pool is a range of IP addresses to be individually allocated.
 * A connection may have a pool.
 * That pool may be shared with other connections (hence the reference count).
 *
 * A pool has a linked list of leases.
 */

struct lease {
	co_serial_t assigned_to; /* ALWAYS 1:1 */

	struct entry free_entry;
	struct entry reusable_entry;

	char *reusable_name;
	struct list reusable_bucket;
};

struct addresspool {
	struct refcnt refcnt;
	ip_range r;
	uint32_t size; /* number of addresses within range */

	unsigned nr_reusable;
	struct list free_list;
	unsigned nr_in_use;	/* active */
	/* --- .free.nr + .nr_in_use --- */
	unsigned nr_leases;	/* nr elements in leases array */

	/*
	 * An array of leases with NR_LEASES elements.  Entry A is for
	 * address r.start+A.
	 */
	struct lease *leases;

	struct addresspool *next;	/* next pool */
};

static struct addresspool *pluto_pools = NULL;

ip_range addresspool_range(struct addresspool *pool)
{
	return pool->r;
}

static void free_lease_content(struct lease *lease)
{
	pfreeany(lease->reusable_name);
	lease->assigned_to = SOS_NOBODY;
}

static unsigned hasher(const char *name)
{
	/*
	 * 251 is a prime close to 256 (so like <<8).
	 *
	 * There's no real rationale for doing this.
	 */
	unsigned hash = 0;
	for (const char *c = name; *c; c++) {
		hash = hash * 251 + (uint8_t) *c;
	}
	return hash;
}

static struct lease *lease_id_bucket(struct addresspool *pool, const char *name)
{
	unsigned hash = hasher(name);
	return &pool->leases[hash % pool->nr_leases];
}

static void hash_lease_id(struct addresspool *pool, struct lease *lease)
{
	struct lease *bucket = lease_id_bucket(pool, lease->reusable_name);
	APPEND(bucket, reusable_bucket, reusable_entry, lease);
	pool->nr_reusable++;
}

static void unhash_lease_id(struct addresspool *pool, struct lease *lease)
{
	struct lease *bucket = lease_id_bucket(pool, lease->reusable_name);
	REMOVE(bucket, reusable_bucket, reusable_entry, lease);
	pool->nr_reusable--;
}

static err_t pool_lease_to_address(const struct addresspool *pool, const struct lease *lease,
				   ip_address *address)
{
	return range_offset_to_address(pool->r, lease - pool->leases, address);
}

static void LDBG_pool(struct logger *logger, bool verbose,
		      const struct addresspool *pool,
		      const char *format, ...) PRINTF_LIKE(4);

static void LDBG_pool(struct logger *logger, bool verbose, const struct addresspool *pool,
		      const char *format, ...)
{
	LLOG_JAMBUF(DEBUG_STREAM, logger, buf) {
		jam(buf, "pool ");
		jam_range(buf, &pool->r);
		jam(buf, ": ");
		va_list args;
		va_start(args, format);
		jam_va_list(buf, format, args);
		va_end(args);
		if (verbose) {
			jam(buf, "; pool-refcount %u size %u leases %u in-use %u free %u reusable %u",
			    refcnt_peek(pool, logger), pool->size, pool->nr_leases,
			    pool->nr_in_use, pool->free_list.nr, pool->nr_reusable);
		}
	}
}

static void LDBG_lease(struct logger *logger, bool verbose,
		       const struct addresspool *pool, const struct lease *lease,
		       const char *format, ...) PRINTF_LIKE(5);

static void LDBG_lease(struct logger *logger, bool verbose,
		       const struct addresspool *pool, const struct lease *lease,
		       const char *format, ...)
{
	LLOG_JAMBUF(DEBUG_STREAM, logger, buf) {
		jam(buf, "pool ");
		jam_range(buf, &pool->r);
		jam(buf, " lease ");
		ip_address addr;
		err_t err = pool_lease_to_address(pool, lease, &addr);
		if (err != NULL) {
			jam(buf, "["PEXPECT_PREFIX"%s]", err);
		}
		jam_address(buf, &addr);
		if (lease->assigned_to != COS_NOBODY) {
			jam(buf, " "PRI_CO, pri_co(lease->assigned_to));
		} else {
			jam(buf, " unassigned");
		}
		jam(buf, ": ");
		va_list args;
		va_start(args, format);
		jam_va_list(buf, format, args);
		va_end(args);
		if (verbose) {
			jam(buf, "; leases %u in-use %u free %u reusable %u",
			    pool->nr_leases, pool->nr_in_use,
			    pool->free_list.nr, pool->nr_reusable);
		}
	}
}

static void scribble_remote_lease(struct connection *c, ip_address ia,
				  unsigned assigned_nr,
				  struct logger *logger, where_t where)
{
	/* assign the lease */
	const struct ip_info *afi = address_info(ia);
	c->remote->child.lease[afi->ip_index] = ia;
	set_child_has_client(c, remote, true);

	/* update the selectors */
	ip_selector selector = selector_from_address(ia);
	struct child_end_selectors *remote_selectors = &c->remote->child.selectors;
	if (!PEXPECT_WHERE(logger, where, assigned_nr < elemsof(remote_selectors->assigned))) {
		return;
	}
	remote_selectors->assigned[assigned_nr] = selector;
	/* keep IPv[46] table in sync */
	remote_selectors->proposed.ip[afi->ip_index].len = 1;
	remote_selectors->proposed.ip[afi->ip_index].list = &remote_selectors->assigned[assigned_nr];

	selector_buf nb;
	ldbg(c->logger, "%s() remote.child.selectors.assigned[%d] %s "PRI_WHERE,
	     __func__,
	     assigned_nr,
	     str_selector(&selector, &nb),
	     pri_where(where));
}

/*
 * A lease is an assignment of a single address from a particular pool.
 *
 * Leases are shared between appropriate connections.
 *
 * Because leases are shared, they are reference-counted.
 * (Since we don't (yet?) free leases that could be shared,
 * we don't actually need reference counting.)
 *
 * When a lease ends, if it could not be shared, it is freed.
 * Otherwise it "lingers" so that the same client (based on ID) can later
 * be assigned the same address from the pool.
 *
 * In the future we may implement code to delete a lingering lease to free
 * the address if there is no free address in the pool.
 *
 * Life cycle:
 *
 * - created by lease_an_address if an existing or lingering lease for the
 *   same thatid isn't found.
 *
 * - released (to linger or freed) by rel_lease_addr.
 *
 * - current code never frees a lease that could be shared.
 *   ??? This constitutes a leak.
 */

static bool client_can_reuse_lease(const struct connection *c)
{
	/*
	 * Cannot share with clients that can authenticate using PSK -
	 * it either uses GroupID or a non-unique ID_IP* due to
	 * clients using pre-NAT IP address
	 */
	if (c->remote->host.config->authby.psk ||
	    c->remote->host.config->auth == AUTH_PSK)
		return false;

	/*
	 * Cannot share with clients that can authenticate using NULL.
	 * Just a bad idea.
	 */
	if (c->remote->host.config->authby.null ||
	    c->remote->host.config->auth == AUTH_NULL)
		return false;

	/* Cannot share NULL/NONE ID. Also cannot share ID_IP* due to NAT and dynamic IP */
	if (c->remote->host.id.kind == ID_NULL ||
	    c->remote->host.id.kind == ID_NONE ||
	    c->remote->host.id.kind == ID_IPV4_ADDR ||
	    c->remote->host.id.kind == ID_IPV6_ADDR)
		return false;

	/* If uniqueids=false - this can mean multiple clients on the same ID & CERT */
	if (!uniqueIDs)
		return false;

	return true;
}

/*
 * If the connection has the lease, return it.
 *
 * Assuming that the connection things it has a lease, need to check
 * that it still does (it may have been stolen by a newer connection
 * with the same ID).
 */
static struct lease *connection_lease(struct connection *c, const struct ip_info *afi,
				      struct logger *logger)
{
	/*
	 * No point looking for a lease when the connection doesn't
	 * think it has one.
	 */
	if (!pexpect(c->remote->child.lease[afi->ip_index].is_set)) {
		return NULL;
	}

	struct addresspool *pool = c->pool[afi->ip_index];

	/*
	 * "i" is index of client.addr within pool's range.
	 *
	 * Using unsigned arithmetic means that if client.addr is less
	 * than start, it will wrap around to a very large value.
	 * Therefore a single test against size will indicate
	 * membership in the range.
	 */
	ip_address prefix = c->remote->child.lease[afi->ip_index];
	uintmax_t offset;
	err_t err = address_to_range_offset(pool->r, prefix, &offset);
	if (err != NULL) {
		llog_pexpect(logger, HERE, "offset of address in range failed: %s", err);
		return NULL;
	}
	passert(pool->nr_leases <= pool->size);
	passert(offset < pool->nr_leases);
	struct lease *lease = &pool->leases[offset];

	/*
	 * Has the lease been "stolen" by a newer connection instance
	 * with the same ID?
	 */
	if (lease->assigned_to > c->serialno) {
		if (LDBGP(DBG_BASE, logger)) {
			LDBG_lease(logger, true, pool, lease, "stolen by "PRI_CO, pri_co(lease->assigned_to));
		}
		return NULL;
	}
	/*
	 * The lease is still assigned to this connection instance (if
	 * it weren't the connection wouldn't have .has_lease).
	 */
	if (!pexpect(lease->assigned_to == c->serialno)) {
		return NULL;
	}
	return lease;
}

/*
 * mark a lease as ended.
 *
 * If the ID is distinctive and uniqueid is set, the lease "lingers"
 * so that the same client can be reassigned the same address.
 * A lingering lease is available to be re-activated
 * by lease_an_address/find_lingering_lease to the same thatid when uniqueid is
 * set.
 *
 * If uniqueIDs is set or thatid is ID_NONE, we don't know how to share.
 * In that case, we do free the lease since that ID isn't distinctive.
 */

void free_that_address_lease(struct connection *c, const struct ip_info *afi, struct logger *logger)
{
	if (!c->remote->child.lease[afi->ip_index].is_set) {
		ldbg(logger, "connection has no %s lease", afi->ip_name);
		return;
	}

	struct lease *lease = connection_lease(c, afi, logger);
	if (lease == NULL) {
		ldbg(logger, "connection lost its %s lease", afi->ip_name);
		c->remote->child.lease[afi->ip_index] = unset_address;
		return;
	}

	struct addresspool *pool = c->pool[afi->ip_index];

	if (lease->reusable_name != NULL) {
		/* the lease is reusable, leave it lingering */
		APPEND(pool, free_list, free_entry, lease);
		pool->nr_in_use--;
		if (LDBGP(DBG_BASE, logger)) {
			connection_buf cb;
			LDBG_lease(logger, true, pool, lease,
				   "lingering reusable lease '%s' for connection "PRI_CONNECTION,
				   lease->reusable_name, pri_connection(c, &cb));
		}
	} else {
		/* cannot share: free it */
		PREPEND(pool, free_list, free_entry, lease);
		pool->nr_in_use--;
		if (LDBGP(DBG_BASE, logger)) {
			connection_buf cb;
			LDBG_lease(logger, true, pool, lease,
				   "returning one-time lease for connection "PRI_CONNECTION,
				   pri_connection(c, &cb));
		}
	}

	/* break the link */
	c->remote->child.lease[afi->ip_index] = unset_address;
	lease->assigned_to = COS_NOBODY;
}

/*
 * return previous lease if there is one lingering for the same ID
 */

static struct lease *recover_lease(const struct connection *c, const char *that_name,
				   const struct ip_info *afi, struct logger *logger)
{
	struct addresspool *pool = c->pool[afi->ip_index];
	if (pool->nr_leases == 0) {
		return NULL;
	}

	struct lease *bucket = lease_id_bucket(pool, that_name);
	if (IS_EMPTY(bucket, reusable_bucket)) {
		return NULL;
	}

	struct lease *lease;
	for (unsigned current = bucket->reusable_bucket.first;
	     current != SENTINEL; current = lease->reusable_entry.next) {
		passert(current < pool->nr_leases);
		lease = &pool->leases[current];
		passert(lease->reusable_name != NULL);
		if (streq(that_name, lease->reusable_name)) {
			if (IS_INSERTED(lease, free_entry)) {
				/* unused */
				REMOVE(pool, free_list, free_entry, lease);
				pexpect(lease->assigned_to == COS_NOBODY);
				pool->nr_in_use++;
				if (LDBGP(DBG_BASE, logger)) {
					connection_buf cb;
					LDBG_lease(logger, false, pool, lease,
						   "recovered by "PRI_CONNECTION" using '%s'; was on free-list",
						   pri_connection(c, &cb), that_name);
				}
			} else {
				/* still assigned to older connection
				 * instance */
				pexpect(lease->assigned_to < c->serialno);
				if (LDBGP(DBG_BASE, logger)) {
					connection_buf cb;
					LDBG_lease(logger, false, pool, lease,
						   "recovered by "PRI_CONNECTION" using '%s'; was in use by "PRI_CO,
						   pri_connection(c, &cb), that_name, pri_co(lease->assigned_to));
				}
			}
			return lease;
		}
	}
	return NULL;
}

static err_t grow_addresspool(struct addresspool *pool,
			      struct logger *logger)
{
	/* try to grow the address pool */
	if (pool->nr_leases >= pool->size) {
		if (LDBGP(DBG_BASE, logger)) {
			LDBG_pool(logger, true, pool, "no free address and no space to grow");
		}
		return "no free address in addresspool"; /* address pool exhausted */
	}

	unsigned old_nr_leases = pool->nr_leases;
	if (pool->nr_leases == 0) {
		pool->nr_leases = min(1U, pool->size);
	} else {
		pool->nr_leases = min(pool->nr_leases * 2, pool->size);
	}
	realloc_things(pool->leases, old_nr_leases, pool->nr_leases, "leases");

	range_buf rb;
	llog(RC_LOG, logger, "pool %s: growing address pool from %u to %u",
	     str_range(&pool->r, &rb), old_nr_leases, pool->nr_leases);

	/* initialize new leases (and add to free list) */
	for (unsigned l = old_nr_leases; l < pool->nr_leases; l++) {
		struct lease *lease = &pool->leases[l];
		/*
		 * Danger: must initialize entire
		 * struct as resize_things(), which
		 * may use realloc(), can leave the
		 * data uninitialized.
		 */
		*lease = (struct lease) {
			.free_entry = empty_entry,
			.reusable_entry = empty_entry,
			.reusable_bucket = empty_list,
		};
		PREPEND(pool, free_list, free_entry, lease);
	}
	/* destroy existing hash table */
	for (unsigned l = 0; l < old_nr_leases; l++) {
		struct lease *lease = &pool->leases[l];
		lease->reusable_entry = empty_entry;
		lease->reusable_bucket = empty_list;
	}
	/* build a new hash table containing old */
	pool->nr_reusable = 0;
	for (unsigned l = 0; l < old_nr_leases; l++) {
		struct lease *lease = &pool->leases[l];
		if (lease->reusable_name != NULL) {
			hash_lease_id(pool, lease);
		}
	}

	return NULL;
}

/*
 * Remove a lease from the .free_list which has the effect of making
 * it in-use.
 *
 * When (*NEW_OWNER), steal the value, setting .reusable_name (and
 * update hash table).
 *
 * Note: this only updates the lease.  The caller still needs to
 * assign the lease to the connection.
 */

static bool unfree_lease(struct addresspool *pool,
			 char **new_owner,
			 struct lease *new_lease,
			 struct logger *logger)
{
	bool stolen = false;
	REMOVE(pool, free_list, free_entry, new_lease);
	pool->nr_in_use++;

	/* unhash before freeing */
	if (new_lease->reusable_name != NULL) {
		/* oops; takeing over this lingering lease */
		if (LDBGP(DBG_BASE, logger)) {
			LDBG_lease(logger, false, pool, new_lease,
				   "stealing reusable lease from '%s'",
				   new_lease->reusable_name);
		}
		unhash_lease_id(pool, new_lease);
		stolen = true;
	}

	free_lease_content(new_lease);
	if ((*new_owner) != NULL) {
		/* steal reference */
		new_lease->reusable_name = (*new_owner);
		(*new_owner) = NULL;
		hash_lease_id(pool, new_lease);
	}

	return stolen;
}

err_t assign_remote_lease(struct connection *c,
			  const char *xauth_username,
			  const struct ip_info *afi,
			  const ip_address *lease_address,
			  struct logger *logger)
{
	VERBOSE_DBGP(DBG_BASE, logger, "%s() xauth=%s family=%s",
		     __func__, (xauth_username == NULL ? "n/a" : xauth_username),
		     afi->ip_name);

	if (c->remote->child.lease[afi->ip_index].is_set &&
	    connection_lease(c, afi, logger) != NULL) {
		ldbg(logger, "connection both thinks it has, and really has a lease");
		return NULL;
	}

	struct addresspool *pool = c->pool[afi->ip_index];
	if (pool == NULL) {
		return "no address pool";
	}

	unsigned next_lease_nr = nr_child_leases(c->remote);

	/*
	 * When the lease is reusable generate a unique ID by
	 * conbining the ID (from IKE_AUTH et.al.) with the
	 * XAUTH_USERNAME (from XAUTH) so that, when when a shared ID
	 * is used with XAUTH the result is still unique.
	 */

	char *reusable_id = NULL;
	if (client_can_reuse_lease(c)) {
		id_buf remote_idb; /* same scope as remote_id */
		const char *remote_id = str_id(&c->remote->host.id, &remote_idb);
		reusable_id = alloc_printf("%s%s", remote_id, (xauth_username != NULL ? xauth_username : ""));
	}

	if (LDBGP(DBG_BASE, logger)) {
		connection_buf cb;
		LDBG_pool(logger, false, pool,
			  "requesting lease for connection "PRI_CONNECTION" with reusable ID '%s'",
			  pri_connection(c, &cb),
			  (reusable_id == NULL ? "" : reusable_id));
	}

	struct lease *new_lease = NULL;
	const char *story = NULL;

	/*
	 * Using the mangled ID (THATSTR), see of there is an existing
	 * lease.
	 */
	if (new_lease == NULL && reusable_id != NULL) {
		new_lease = recover_lease(c, reusable_id, afi, logger);
		if (new_lease != NULL) {
			story = "recovered";
			pfreeany(reusable_id); /* not needed */
		}
	}

	/*
	 * If the peer's given a prefered address try to assign that.
	 *
	 * XXX: some of this is identical to code in
	 * connection_lease().
	 */
	if (new_lease == NULL && is_set(lease_address)) {

		/*
		 * Determine where the lease should be in the lease
		 * table.
		 *
		 * Earlier code checked address in range, hence
		 * shouldn't fail!
		 */
		uintmax_t offset;
		err_t err = address_to_range_offset(pool->r, (*lease_address), &offset);
		if (err != NULL) {
			llog_pexpect(verbose.logger, HERE, "offset of address in range failed: %s", err);
			pfreeany(reusable_id);
			return "confused, address should be within addresspool";
		}

		vassert(pool->nr_leases <= pool->size);
		vassert(offset < pool->size); /* by above */

		if (offset < pool->nr_leases) {
			/*
			 * Is the lease available?
			 *
			 * IKEv1: must fail as Quick Mode has no way
			 * to send back an alternative lease.
			 *
			 * IKEv2: should not fail, instead the
			 * proposed lease should be ignored and a new
			 * one assigned.
			 *
			 * Later.
			 */
			struct lease *lease = &pool->leases[offset];
			if (lease->assigned_to != COS_NOBODY) {
				if (LDBGP(DBG_BASE, logger)) {
					LDBG_lease(logger, true, pool, lease, "owned by "PRI_CO,
						   pri_co(lease->assigned_to));
				}
				pfreeany(reusable_id);
				return "lease address already in use";
			}

			/*
			 * This returns true when the lease had a previous
			 * owner and the story needs to be updated.
			 */
			if (unfree_lease(pool, &reusable_id,
					 lease, logger)) {
				story = "request reclaimed";
			} else {
				story = "request available";
			}
			vassert(reusable_id == NULL); /* stolen */
			new_lease = lease;

		} else {

			/* grow the lease if necessary */
			while (offset >= pool->nr_leases) {
				grow_addresspool(pool, logger);
			}

			vassert(offset < pool->nr_leases);
			struct lease *lease = &pool->leases[offset];
			if (!pexpect(lease->assigned_to == COS_NOBODY)) {
				pfreeany(reusable_id);
				return "confused, just allocated lease in use";
			}

			/* fresh lease can't have previous owner */
			vassert(unfree_lease(pool, &reusable_id,
					     lease, logger) == false);

			vassert(reusable_id == NULL); /* stolen */
			new_lease = lease;
			story = "request grown";
		}

	}

	/*
	 * Allocate the next lease from the free list; if necessary,
	 * grow the pool.
	 */
	if (new_lease == NULL) {
		if (IS_EMPTY(pool, free_list)) {
			err_t e = grow_addresspool(pool, logger);
			if (e != NULL) {
				pfreeany(reusable_id);
				return e;
			}
		}

		/* grab the next lease on the free list */
		new_lease = HEAD(pool, free_list, free_entry);
		vassert(new_lease != NULL);
		story = (unfree_lease(pool, &reusable_id, new_lease, logger)
			 ? "stolen"
			 : "unused");
		vassert(reusable_id == NULL); /* stolen */
	}

	/*
	 * No lease (above should have returned), but play save.
	 */

	if (vbad(new_lease == NULL)) {
		pfreeany(reusable_id);
		return "confused, no lease";
	}

	if (vbad(story == NULL)) {
		return "confused, no story";
	}

	if (vbad(reusable_id != NULL)) {
		return "confused, leaking memory";
	}

	/*
	 * Convert the leases offset into range, into an IP_address.
	 */
	ip_address ia;
	err_t err = pool_lease_to_address(pool, new_lease, &ia);
	if (err != NULL) {
		llog_pexpect(logger, HERE, "%s", err);
		return "confused, bad address";
	}

	/* assign and back link */
	scribble_remote_lease(c, ia, next_lease_nr, logger, HERE);
	new_lease->assigned_to = c->serialno;

	if (LDBGP(DBG_BASE, logger)) {
		address_buf ab;
		connection_buf cb;
		LDBG_lease(logger, true, pool, new_lease,
			   "assign %s lease to "PRI_CONNECTION" "PRI_CO" with reusable ID '%s' and that.lease %s",
			   story,
			   pri_connection(c, &cb),
			   pri_co(new_lease->assigned_to),
			   (new_lease->reusable_name == NULL ? "" : new_lease->reusable_name),
			   str_address(&ia, &ab));
	}

	return NULL;
}

void addresspool_delref(struct addresspool **poolparty, struct logger *logger)
{
	struct addresspool *pool = delref_where(poolparty, logger, HERE);
	if (pool != NULL) {
		for (struct addresspool **pp = &pluto_pools; *pp != NULL; pp = &(*pp)->next) {
			if (*pp == pool) {
				*pp = pool->next;	/* unlink pool */
				for (unsigned l = 0; l < pool->nr_leases; l++) {
					free_lease_content(&pool->leases[l]);
				}
				pfreeany(pool->leases);
				pfree(pool);
				return;
			}
		}
		if (LDBGP(DBG_BASE, logger)) {
			LDBG_pool(logger, false, pool, "pool %p not found in list of pools", pool);
		}
	}
}

struct addresspool *addresspool_addref(struct addresspool *pool)
{
	return addref_where(pool, HERE);
}

/*
 * Finds an ip_pool that has exactly matching bounds.
 *
 * If POOL_RANGE exactly matches an existing address pool, return NULL
 * and set *POOL.
 *
 * If POOL_RANGE overlaps an existing pool return a diagnostic
 * describing the existing pool's conflict (i.e., assume caller will
 * include the new pool), and *POOL=NULL.
 *
 * Otherwise (nothing matches), return NULL and *POOL=NULL.
 */

static diag_t find_addresspool(const ip_range pool_range, struct addresspool **pool)
{
	struct addresspool *h;

	*pool = NULL;	/* nothing found (yet) */
	for (h = pluto_pools; h != NULL; h = h->next) {

		if (range_eq_range(pool_range, h->r)) {
			/* exact match */
			*pool = h;
			return NULL;
		}

		if (range_overlaps_range(pool_range, h->r)) {
			/* bad */
			range_buf hbuf;
			return diag("range INEXACTLY OVERLAPS existing address pool %s.",
				    str_range(&h->r, &hbuf));
		}
	}
	return NULL;
}

/*
 * Create an address pool for POOL_RANGE.  Reject invalid ranges.
 */

diag_t install_addresspool(const ip_range pool_range,
			   struct addresspool *addresspool[],
			   struct logger *logger)
{
	/* can't be empty */
	uintmax_t pool_size = range_size(pool_range);
	if (pool_size == 0) {
		range_buf rb;
		return diag("address pool %s is empty",
			    str_range(&pool_range, &rb));
	}

	if (pool_size >= UINT32_MAX) {
		/*
		 * uint32_t overflow, 2001:db8:0:3::/64 truncated to UINT32_MAX
		 * uint32_t overflow, 2001:db8:0:3:1::/96, truncated by 1
		 */
		pool_size = UINT32_MAX;
		ldbg(logger, "WARNING addresspool size overflow truncated to %ju", pool_size);
	}

	/* can't start at 0 */
	ip_address start = range_start(pool_range);
	if (!address_is_specified(start)) {
		range_buf rb;
		return diag("address pool %s starts at address zero",
			    str_range(&pool_range, &rb));
	}

	/* can't overlap or duplicate */
	struct addresspool *existing_pool = NULL;
	diag_t d = find_addresspool(pool_range, &existing_pool);
	if (d != NULL) {
		return d;
	}

	const struct ip_info *afi = range_info(pool_range);
	if (addresspool[afi->ip_index] != NULL) {
		return diag("connection already has a %s address pool", afi->ip_name);
	}

	if (existing_pool != NULL) {
		/* re-use existing pool */
		if (LDBGP(DBG_BASE, logger)) {
			LDBG_pool(logger, true, existing_pool, "reusing existing address pool@%p", existing_pool);
		}
		addresspool[afi->ip_index] = addresspool_addref(existing_pool);
		return NULL;
	}

	/* make a new pool */
	struct addresspool *new_pool = refcnt_alloc(struct addresspool, HERE);
	new_pool->r = pool_range;
	new_pool->size = pool_size;
	new_pool->nr_in_use = 0;
	new_pool->nr_leases = 0;
	new_pool->free_list = empty_list;
	new_pool->leases = NULL;

	/* insert at front */
	new_pool->next = pluto_pools;
	pluto_pools = new_pool;

	if (LDBGP(DBG_BASE, logger)) {
		LDBG_pool(logger, false, new_pool, "creating new address pool@%p", new_pool);
	}

	addresspool[afi->ip_index] = new_pool;
	return NULL;
}

void show_addresspool_status(struct show *s)
{
	show_separator(s);
#define CHECK(A, B)							\
	if ((A) != (B)) {						\
		llog_pexpect(show_logger(s), HERE,			\
			     "" #A " (%u) does not match " #B " (%u)",	\
			     A, B);					\
	}
	for (struct addresspool *pool = pluto_pools;
	     pool != NULL; pool = pool->next) {
		range_buf rb;
		show(s, "address pool %s: %u addresses, %u leases, %u in-use, %u free (%u reusable)",
			     str_range(&pool->r, &rb),
			     pool->size, pool->nr_leases, pool->nr_in_use,
			     pool->free_list.nr,
			     pool->nr_reusable);
		unsigned nr_free = 0;
		unsigned nr_reusable_entries = 0;
		unsigned nr_reusable_names = 0;
		for (unsigned l = 0; l < pool->nr_leases; l++) {
			struct lease *lease = &pool->leases[l];
			ip_address lease_ip;
			err_t err = pool_lease_to_address(pool, lease, &lease_ip);
			if (err != NULL) {
				llog_pexpect(show_logger(s), HERE, "%s", err);
			}
			address_buf lease_ipb;
			const char *lease_str = str_address(&lease_ip, &lease_ipb);
			struct connection *c = connection_by_serialno(lease->assigned_to);
			nr_free += IS_INSERTED(lease, free_entry) ? 1 : 0;
			nr_reusable_entries += IS_INSERTED(lease, reusable_entry) ? 1 : 0;
			nr_reusable_names += lease->reusable_name != NULL ? 1 : 0;
			{
				/* fudge indent so show*() calls are aligned */
				show(s, "    %*s %s "PRI_CO" %s%s",
					     (int)strlen(lease_str), lease_str,
					     IS_INSERTED(lease, free_entry) ? "free" : "assigned to",
					     pri_co(lease->assigned_to),
					     lease->reusable_name != NULL ? " " : "",
					     lease->reusable_name != NULL ? lease->reusable_name : "");
			}
			if (c != NULL) {
				connection_buf cb;
				show(s, "    %*s "PRI_CONNECTION,
					     (int)strlen(lease_str), "",
					     pri_connection(c, &cb));
			} else {
				show(s, "    %*s connection "PRI_CO" does not exist",
					     (int)strlen(lease_str), "",
					     pri_co(lease->assigned_to));
			}
			CHECK(IS_INSERTED(lease, reusable_entry), lease->reusable_name != NULL);
		}
		CHECK(pool->nr_leases, pool->nr_in_use + pool->free_list.nr);
		CHECK(nr_free, pool->free_list.nr);
		CHECK(nr_reusable_entries, pool->nr_reusable);
		CHECK(nr_reusable_names, pool->nr_reusable);
#undef CHECK
	}
}
