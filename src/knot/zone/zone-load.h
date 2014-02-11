/*  Copyright (C) 2011 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

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
/*!
 * \file zone-load.h
 *
 * \author Marek Vavrusa <marek.vavrusa@nic.cz>
 * \author Jan Kadlec <jan.kadlec@nic.cz>
 *
 * \brief Zone loading
 *
 * @{
 */

#ifndef _KNOT_ZONELOAD_H_
#define _KNOT_ZONELOAD_H_

#include <stdio.h>

#include "knot/zone/zone.h"
#include "knot/zone/semantic-check.h"
#include "zscanner/zscanner.h"

/* TODO this structure is highly redundant, remove. Maybe use oh-so-great BIRD lists. */
/*!
 * \brief One-purpose linked list holding pointers to RRSets.
 */
struct rrset_list {
	knot_rrset_t *data; /*!< List data. */
	struct rrset_list *next; /*!< Next node. */
};

typedef struct rrset_list rrset_list_t;

struct parser_context {
	rrset_list_t *node_rrsigs;
	knot_zone_contents_t *current_zone;
	knot_rrset_t *current_rrset;
	knot_dname_t *origin_from_config;
	knot_node_t *last_node;
	err_handler_t *err_handler;
	hattrie_t *lookup_tree;
	int ret;
};

typedef struct parser_context parser_context_t;

/*!
 * \brief Zone loader structure.
 */
typedef struct zloader_t
{
	char *source;             /*!< Zone source file. */
	char *origin;             /*!< Zone's origin string. */
	int semantic_checks;      /*!< Wanted level of semantic checks. */
	err_handler_t *err_handler; /*!< Semantic checks error handler. */
	file_loader_t *file_loader; /*!< Scanner's file loader. */
	parser_context_t *context; /*!< Loader context. */

} zloader_t;

/*!
 * \brief Open zone file for loading.
 *
 * \param zl Output zone loader.
 * \param conf Zone configuration.
 *
 * \retval Initialized loader on success.
 * \retval NULL on error.
 */
int zonefile_open(zloader_t *loader, const conf_zone_t *conf);

/*!
 * \brief Loads zone from a zone file.
 *
 * \param loader Zone loader instance.
 *
 * \retval Loaded zone contents on success.
 * \retval NULL otherwise.
 */
knot_zone_contents_t *zonefile_load(zloader_t *loader);

/*!
 * \brief Close zone file loader.
 *
 * \param loader Zone loader instance.
 */
void zonefile_close(zloader_t *loader);

void process_error(const scanner_t *scanner);

#endif /* _KNOTD_ZONELOAD_H_ */

/*! @} */
