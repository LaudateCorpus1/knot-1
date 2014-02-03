/*  Copyright (C) 2013 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <config.h>
#include <assert.h>
#include <sys/stat.h>
#include <inttypes.h>

#include "knot/conf/conf.h"
#include "knot/other/debug.h"
#include "knot/server/zone-load.h"
#include "knot/server/zones.h"
#include "knot/zone/zone-load.h"
#include "libknot/dname.h"
#include "libknot/dnssec/crypto.h"
#include "libknot/dnssec/random.h"
#include "knot/nameserver/name-server.h"
#include "libknot/rdata.h"
#include "knot/zone/zone.h"
#include "knot/zone/zone.h"
#include "knot/zone/zonedb.h"
#include "common/descriptor.h"

/*! \brief Freeze the zone data to prevent any further transfers or
 *         events manipulation.
 */
static void zone_freeze(zone_t *zone)
{
	if (!zone) {
		return;
	}

	/*! \todo NOTIFY, xfers and updates now MUST NOT trigger reschedule. */
	/*! \todo No new xfers or updates should be processed. */
	/*! \todo Raise some kind of flag. */

	/* Wait for readers to notice the change. */
	synchronize_rcu();

	/* Cancel all pending timers. */
	zone_reset_timers(zone);

	/* Now some transfers may already be running, we need to wait for them. */
	/*! \todo This should be done somehow. */

	/* Reacquire journal to ensure all operations on it are finished. */
	if (journal_is_used(zone->ixfr_db)) {
		if (journal_retain(zone->ixfr_db) == KNOT_EOK) {
			journal_release(zone->ixfr_db);
		}
	}
}

/*- zone file status --------------------------------------------------------*/

/*!
 * \brief Zone file status.
 */
typedef enum {
	ZONE_STATUS_NOT_FOUND = 0,  //!< Zone file does not exist.
	ZONE_STATUS_FOUND_NEW,      //!< Zone file exists, not loaded yet.
	ZONE_STATUS_FOUND_CURRENT,  //!< Zone file exists, same as loaded.
	ZONE_STATUS_FOUND_UPDATED,  //!< Zone file exists, newer than loaded.
} zone_status_t;

/*!
 * \brief Check zone file status.
 *
 * \param old_zone  Previous instance of the zone (can be NULL).
 * \param filename  File name of zone file.
 *
 * \return Zone status.
 */
static zone_status_t zone_file_status(const zone_t *old_zone,
                                      const char *filename)
{
	struct stat zf_stat = { 0 };
	int result = stat(filename, &zf_stat);

	if (result == -1) {
		return ZONE_STATUS_NOT_FOUND;
	} else if (old_zone == NULL) {
		return ZONE_STATUS_FOUND_NEW;
	} else if (old_zone->zonefile_mtime == zf_stat.st_mtime) {
		return ZONE_STATUS_FOUND_CURRENT;
	} else {
		return ZONE_STATUS_FOUND_UPDATED;
	}
}

/*- zone loading/updating ---------------------------------------------------*/

/*!
 * \brief Handle retrieval of zone if zone file does not exist.
 *
 * \param conf      New configuration for given zone.
 *
 * \return New zone, NULL if bootstrap not possible.
 */
static zone_t *bootstrap_zone(conf_zone_t *conf)
{
	assert(conf);

	bool bootstrap = !EMPTY_LIST(conf->acl.xfr_in);
	if (!bootstrap) {
		return NULL;
	}

	zone_t *new_zone = zone_new(conf);
	if (!new_zone) {
		log_zone_error("Bootstrap of zone '%s' failed: %s\n",
		               conf->name, knot_strerror(KNOT_ENOMEM));
		return NULL;
	}

	return new_zone;
}

/*!
 * \brief Load zone.
 *
 * \param conf Zone configuration.
 *
 * \return Loaded zone, NULL in case of error.
 */
static zone_t *load_zone(conf_zone_t *conf)
{
	assert(conf);

	zloader_t *zl = NULL;
	zone_t *zone = NULL;

	/* Open zone file for parsing. */
	switch (knot_zload_open(&zl, conf)) {
	case KNOT_EOK:
		break;
	case KNOT_EACCES:
		log_zone_error("No access/permission to zone file '%s'.\n", conf->file);
		knot_zload_close(zl);
		return NULL;
	default:
		log_zone_error("Failed to load zone file '%s'\n", conf->file);
		knot_zload_close(zl);
		return NULL;
	}

	/* Check the source file */
	assert(zl != NULL);
	zone = knot_zload_load(zl);
	if (!zone) {
		log_zone_error("Zone '%s' could not be loaded.\n", conf->name);
		knot_zload_close(zl);
		return NULL;
	}

	/* Check if loaded origin matches. */
	knot_dname_t *dname_req = NULL;
	dname_req = knot_dname_from_str(conf->name);
	if (knot_dname_cmp(zone->name, dname_req) != 0) {
		log_zone_error("Zone '%s': mismatching origin in the zone file.\n",
		               conf->name);
		zone_deep_free(&zone);
		return NULL;
	} else {
		/* Save the timestamp from the zone db file. */
		struct stat st;
		if (stat(conf->file, &st) < 0) {
			dbg_zones("zones: failed to stat() zone db, "
				  "something is seriously wrong\n");
			zone_deep_free(&zone);
			return NULL;
		} else {
			zone->zonefile_mtime = st.st_mtime;
			zone->zonefile_serial = knot_zone_serial(zone->contents);
		}
	}
	knot_dname_free(&dname_req);
	knot_zload_close(zl);

	return zone;
}

/*!
 * \brief Create a new zone structure according to documentation, but reuse
 *        existing zone content.
 */
static zone_t *preserve_zone(conf_zone_t *conf, const zone_t *old_zone)
{
	assert(old_zone);

	zone_t *new_zone = zone_new(conf);
	if (!new_zone) {
		log_zone_error("Preserving current zone '%s' failed: %s\n",
		               conf->name, knot_strerror(KNOT_ENOMEM));
		return NULL;
	}

	new_zone->contents = old_zone->contents;
	new_zone->contents->zone = new_zone; // hopefully temporary

	return new_zone;
}

/*!
 * \brief Log message about loaded zone (name, status, serial).
 *
 * \param zone       Zone structure.
 * \param zone_name  Printable name of the zone.
 * \param status     Zone file status.
 */
static void log_zone_load_info(const zone_t *zone, const char *zone_name,
                               zone_status_t status)
{
	assert(zone);
	assert(zone_name);

	const char *action = NULL;

	switch (status) {
	case ZONE_STATUS_NOT_FOUND:     action = "bootstrapped";  break;
	case ZONE_STATUS_FOUND_NEW:     action = "loaded";        break;
	case ZONE_STATUS_FOUND_CURRENT: action = "is up-to-date"; break;
	case ZONE_STATUS_FOUND_UPDATED: action = "reloaded";      break;
	}
	assert(action);

	int64_t serial = 0;
	if (zone->contents && zone->contents->apex) {
		const knot_rrset_t *soa;
		soa = knot_node_rrset(zone->contents->apex, KNOT_RRTYPE_SOA);
		serial = knot_rdata_soa_serial(soa);
	}

	log_zone_info("Zone '%s' %s (serial %" PRId64 ")\n", zone_name, action, serial);
}

/*!
 * \brief Load or reload the zone.
 *
 * \param old_zone  Already loaded zone (can be NULL).
 * \param conf      Zone configuration.
 * \param ns        Name server structure.
 *
 * \return Error code, KNOT_EOK if successful.
 */
static zone_t *create_zone(zone_t *old_zone, conf_zone_t *conf, knot_nameserver_t *ns)
{
	assert(conf);
	assert(ns);

	zone_status_t zstatus = zone_file_status(old_zone, conf->file);
	zone_t *new_zone = NULL;

	switch (zstatus) {
	case ZONE_STATUS_NOT_FOUND:
		new_zone = bootstrap_zone(conf);
		break;
	case ZONE_STATUS_FOUND_NEW:
	case ZONE_STATUS_FOUND_UPDATED:
		new_zone = load_zone(conf);
		break;
	case ZONE_STATUS_FOUND_CURRENT:
		new_zone = preserve_zone(conf, old_zone);
		break;
	default:
		assert(0);
	}

	if (!new_zone) {
		log_server_error("Failed to load zone '%s'.\n", conf->name);
		return NULL;
	}

	new_zone->server = (server_t *)ns->data;
	zone_reset_timers(new_zone);

	log_zone_load_info(new_zone, conf->name, zstatus);

	return new_zone;
}

/*!
 * \brief Load/reload the zone, apply journal, sign it and schedule XFR sync.
 *
 * \param[out] dst       Pointer to successfully loaded zone.
 * \param[in]  old_zone  Old zone (if loaded).
 * \param[in]  conf      Zone configuration.
 * \param[in]  ns        Name server structure.
 *
 * \return Error code, KNOT_EOK if sucessful.
 */
static int update_zone(zone_t **dst, zone_t *old_zone,
                       conf_zone_t *conf, knot_nameserver_t *ns)
{
	assert(dst);
	assert(conf);
	assert(ns);

	int result = KNOT_ERROR;

	// Load zone.

	zone_t *new_zone = create_zone(old_zone, conf, ns);
	if (!new_zone) {
		return KNOT_EZONENOENT;
	}

	bool new_content = (old_zone == NULL || old_zone->contents != new_zone->contents);

	result = zones_journal_apply(new_zone);
	if (result != KNOT_EOK && result != KNOT_ERANGE && result != KNOT_ENOENT) {
		log_zone_error("Zone '%s', failed to apply changes from journal: %s\n",
		               conf->name, knot_strerror(result));
		goto fail;
	}

	result = zones_do_diff_and_sign(conf, new_zone, ns, new_content);
	if (result != KNOT_EOK) {
		log_zone_error("Zone '%s', failed to sign the zone: %s\n",
		               conf->name, knot_strerror(result));
		goto fail;
	}

	new_zone->server = (server_t *)ns->data;

	// Post processing.

	zones_schedule_ixfr_sync(new_zone, conf->dbsync_timeout);

	knot_zone_contents_t *new_contents = new_zone->contents;
	if (new_contents) {
		/* Check NSEC3PARAM state if present. */
		result = knot_zone_contents_load_nsec3param(new_contents);
		if (result != KNOT_EOK) {
			log_zone_error("NSEC3 signed zone has invalid or no "
				       "NSEC3PARAM record.\n");
			goto fail;
		}
		/* Check minimum EDNS0 payload if signed. (RFC4035/sec. 3) */
		if (knot_zone_contents_is_signed(new_contents)) {
			unsigned edns_dnssec_min = EDNS_MIN_DNSSEC_PAYLOAD;
			if (knot_edns_get_payload(ns->opt_rr) < edns_dnssec_min) {
				log_zone_warning("EDNS payload lower than %uB for "
						 "DNSSEC-enabled zone '%s'.\n",
						 edns_dnssec_min, conf->name);
			}
		}
	}

fail:
	assert(new_zone);

	if (result == KNOT_EOK) {
		*dst = new_zone;
	} else {
		// recycled zone content (bind back to old zone)
		if (!new_content) {
			old_zone->contents->zone = old_zone;
			new_zone->contents = NULL;
		}

		zone_deep_free(&new_zone);
	}

	return result;
}

/*! \brief Context for threaded zone loader. */
typedef struct {
	const struct conf_t *config;
	knot_nameserver_t *ns;
	knot_zonedb_t *db_new;
	pthread_mutex_t lock;
} zone_loader_ctx_t;

/*! Thread entrypoint for loading zones. */
static int zone_loader_thread(dthread_t *thread)
{
	if (thread == NULL || thread->data == NULL) {
		return KNOT_EINVAL;
	}

	int ret = KNOT_ERROR;
	zone_t *zone = NULL;
	conf_zone_t *zone_config = NULL;
	zone_loader_ctx_t *ctx = (zone_loader_ctx_t *)thread->data;
	for(;;) {
		/* Fetch zone configuration from the list. */
		pthread_mutex_lock(&ctx->lock);
		if (EMPTY_LIST(ctx->config->zones)) {
			pthread_mutex_unlock(&ctx->lock);
			break;
		}

		/* Disconnect from the list and start processing. */
		zone_config = HEAD(ctx->config->zones);
		rem_node(&zone_config->n);
		pthread_mutex_unlock(&ctx->lock);

		/* Retrive old zone (if exists). */
		knot_dname_t *apex = knot_dname_from_str(zone_config->name);
		if (!apex) {
			return KNOT_ENOMEM;
		}
		zone_t *old_zone = knot_zonedb_find(ctx->ns->zone_db, apex);
		knot_dname_free(&apex);

		/* Freeze existing zone. */
		zone_freeze(old_zone);

		/* Update the zone. */
		ret = update_zone(&zone, old_zone, zone_config, ctx->ns);

		/* Insert into database if properly loaded. */
		pthread_mutex_lock(&ctx->lock);
		if (ret == KNOT_EOK) {
			if (knot_zonedb_insert(ctx->db_new, zone) != KNOT_EOK) {
				log_zone_error("Failed to insert zone '%s' "
				               "into database.\n", zone_config->name);

				// recycled zone content (bind back to old zone)
				if (old_zone && old_zone->contents == zone->contents) {
					old_zone->contents->zone = old_zone;
					zone->contents = NULL;
				}

				zone_deep_free(&zone);
			}
		} else {
			/* Unable to load, discard configuration. */
			conf_free_zone(zone_config);
		}
		pthread_mutex_unlock(&ctx->lock);
	}

	return KNOT_EOK;
}

static int zone_loader_destruct(dthread_t *thread)
{
	knot_crypto_cleanup_thread();
	return KNOT_EOK;
}

/*!
 * \brief Fill the new database with zones.
 *
 * Zones that should be retained are just added from the old database to the
 * new. New zones are loaded.
 *
 * \param ns Name server instance.
 * \param conf Server configuration.
 *
 * \return Number of inserted zones.
 */
static knot_zonedb_t *load_zonedb(knot_nameserver_t *ns, const conf_t *conf)
{
	/* Initialize threaded loader. */
	zone_loader_ctx_t ctx = {0};
	ctx.ns = ns;
	ctx.config = conf;
	ctx.db_new = knot_zonedb_new(conf->zones_count);
	if (ctx.db_new == NULL) {
		return NULL;
	}

	if (conf->zones_count == 0) {
		return ctx.db_new;
	}

	if (pthread_mutex_init(&ctx.lock, NULL) < 0) {
		knot_zonedb_free(&ctx.db_new);
		return NULL;
	}

	/* Initialize threads. */
	size_t thread_count = MIN(conf->zones_count, dt_optimal_size());
	dt_unit_t *unit = NULL;
	unit = dt_create(thread_count, &zone_loader_thread,
	                          &zone_loader_destruct, &ctx);
	if (unit != NULL) {
		/* Start loading. */
		dt_start(unit);
		dt_join(unit);
		dt_delete(&unit);
	} else {
		knot_zonedb_free(&ctx.db_new);
		ctx.db_new = NULL;
	}

	pthread_mutex_destroy(&ctx.lock);
	return ctx.db_new;
}

/*!
 * \brief Remove zones present in the configuration from the old database.
 *
 * After calling this function, the old zone database should contain only zones
 * that should be completely deleted.
 *
 * \param zone_conf Zone configuration.
 * \param db_old Old zone database to remove zones from.
 *
 * \retval KNOT_EOK
 * \retval KNOT_ERROR
 */
static int remove_zones(const knot_zonedb_t *db_new,
                              knot_zonedb_t *db_old)
{
	knot_zonedb_iter_t it;
	knot_zonedb_iter_begin(db_new, &it);
	while(!knot_zonedb_iter_finished(&it)) {
		const zone_t *old_zone = NULL;
		const zone_t *new_zone = knot_zonedb_iter_val(&it);
		const knot_dname_t *zone_name = new_zone->name;

		/* try to find the new zone in the old DB
		 * if the pointers match, remove the zone from old DB
		 */
		old_zone = knot_zonedb_find(db_old, zone_name);
		if (old_zone == new_zone) {
			knot_zonedb_del(db_old, zone_name);
		}

		knot_zonedb_iter_next(&it);
	}

	return KNOT_EOK;
}

/*- public API functions ----------------------------------------------------*/

/*!
 * \brief Update zone database according to configuration.
 */
int zones_update_db_from_config(const conf_t *conf, knot_nameserver_t *ns,
                               knot_zonedb_t **db_old)
{
	/* Check parameters */
	if (conf == NULL || ns == NULL) {
		return KNOT_EINVAL;
	}

	/* Grab a pointer to the old database */
	if (ns->zone_db == NULL) {
		log_server_error("Missing zone database in nameserver structure.\n");
		return KNOT_ENOENT;
	}

	/* Insert all required zones to the new zone DB. */
	/*! \warning RCU must not be locked as some contents switching will
	             be required. */
	knot_zonedb_t *db_new = load_zonedb(ns, conf);
	if (db_new == NULL) {
		log_server_warning("Failed to load zones.\n");
		return KNOT_ENOMEM;
	} else {
		size_t loaded = knot_zonedb_size(db_new);
		log_server_info("Loaded %zu out of %d zones.\n",
		                loaded, conf->zones_count);
		if (loaded != conf->zones_count) {
			log_server_warning("Not all the zones were loaded.\n");
		}
	}

	/* Lock RCU to ensure none will deallocate any data under our hands. */
	rcu_read_lock();
	*db_old = ns->zone_db;

	dbg_zones_detail("zones: old db in nameserver: %p, old db stored: %p, "
	                 "new db: %p\n", ns->zone_db, *db_old, db_new);

	/* Rebuild zone database search stack. */
	knot_zonedb_build_index(db_new);

	/* Switch the databases. */
	UNUSED(rcu_xchg_pointer(&ns->zone_db, db_new));

	dbg_zones_detail("db in nameserver: %p, old db stored: %p, new db: %p\n",
	                 ns->zone_db, *db_old, db_new);

	/*
	 * Remove all zones present in the new DB from the old DB.
	 * No new thread can access these zones in the old DB, as the
	 * databases are already switched.
	 *
	 * Beware - only the exact same zones (same pointer) may be removed.
	 * All other have been loaded again so that the old must be destroyed.
	 */
	int ret = remove_zones(db_new, *db_old);

	/* Unlock RCU, messing with any data will not affect us now */
	rcu_read_unlock();

	if (ret != KNOT_EOK) {
		return ret;
	}

	return KNOT_EOK;
}
