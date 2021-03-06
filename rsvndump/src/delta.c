/*
 *      rsvndump - remote svn repository dump
 *      Copyright (C) 2008-2010 Jonas Gehring
 *
 *      This program is free software: you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation, either version 3 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 *      file: delta.c
 *      desc: The delta editor
 */


#include <svn_delta.h>
#include <svn_io.h>
#include <svn_md5.h>
#include <svn_pools.h>
#include <svn_props.h>
#include <svn_repos.h>

#include <apr_file_io.h>
#include <apr_hash.h>
#include <apr_md5.h>

#include "main.h"
#include "dump.h"
#include "list.h"
#include "log.h"
#include "path_hash.h"
#include "property.h"
#include "rhash.h"
#include "session.h"
#include "utils.h"

#include "delta.h"

/* This is for compabibility for Subverison 1.4 */
#if (SVN_VER_MAJOR==1) && (SVN_VER_MINOR<=5)
 #define SVN_REPOS_DUMPFILE_TEXT_CONTENT_MD5 SVN_REPOS_DUMPFILE_TEXT_CONTENT_CHECKSUM
#endif

#define ERRBUFFER_SIZE 512


/*---------------------------------------------------------------------------*/
/* Local data structures                                                     */
/*---------------------------------------------------------------------------*/


/* Copy info enumeration */
typedef enum {
	CPI_NONE,
	CPI_COPY,
	CPI_FAILED
} cp_info_t;


/* Main delta editor baton */
typedef struct {
	session_t         *session;
	dump_options_t    *opts;
	list_t            *logs;
	log_revision_t    *log_revision;
	apr_pool_t        *revision_pool;
	apr_hash_t        *dumped_entries;
	svn_revnum_t      local_revnum;
	void              *root_node;
} de_baton_t;


/* Node baton */
typedef struct {
	de_baton_t        *de_baton;
	apr_pool_t        *pool;
	char              *path;
	char              *filename;
	char              *old_filename;
	char              *delta_filename;
	char              action;
	svn_node_kind_t   kind;
	apr_hash_t        *properties;
	apr_hash_t        *del_properties; /* Value is always 0x1 */
	unsigned char     md5sum[APR_MD5_DIGESTSIZE];
	char              *copyfrom_path;
	svn_revnum_t      copyfrom_revision;
	svn_revnum_t      copyfrom_rev_local;
	cp_info_t         cp_info;
	char              applied_delta;
	char              dump_needed;
	char              props_changed;
	void              *parent;
	apr_array_header_t *children;
} de_node_baton_t;


/*---------------------------------------------------------------------------*/
/* Static variables                                                          */
/*---------------------------------------------------------------------------*/


/*
 * If the dump output is not using deltas, we need to keep a local copy of
 * every file in the repository. The delta_hash hash defines a mapping of
 * repository paths to temporary files for this purpose. The prop_hash
 * defines a mapping from repository files to temporary files filled
 * with file/directory properties. The md5_hash is used to store the md5-sums
 * of the file contents.
 */
static char hashes_created = 0;
static rhash_t *delta_hash = NULL;
static rhash_t *prop_hash = NULL;
static rhash_t *md5_hash = NULL;
#ifdef USE_TIMING
 static float tm_de_apply_textdelta = 0.0f;
#endif


/*---------------------------------------------------------------------------*/
/* Static functions                                                          */
/*---------------------------------------------------------------------------*/


/* Prototypes */
static svn_error_t *delta_dump_node(de_node_baton_t *node);


/* Creates a new node baton */
static de_node_baton_t *delta_create_node(const char *path, de_node_baton_t *parent)
{
	de_node_baton_t *node = apr_palloc(parent->pool, sizeof(de_node_baton_t));
	node->pool = svn_pool_create(parent->pool);
	node->path = apr_pstrdup(node->pool, path);
	node->de_baton = parent->de_baton;
	node->properties = apr_hash_make(node->pool);
	node->del_properties = apr_hash_make(node->pool);
	node->filename = NULL;
	node->old_filename = NULL;
	node->delta_filename = NULL;
	node->cp_info = parent->cp_info;
	node->copyfrom_path = NULL;
	node->applied_delta = 0;
	node->dump_needed = 0;
	node->props_changed = 0;
	node->parent = parent;
	node->children = apr_array_make(node->pool, 0, (sizeof(de_node_baton_t *)));
	memset(node->md5sum, 0x00, sizeof(node->md5sum));

	/* Register node in parent list */
	APR_ARRAY_PUSH(parent->children, de_node_baton_t *) = node;

	return node;
}


/* Creates a new node baton without a parent */
static de_node_baton_t *delta_create_node_no_parent(const char *path, de_baton_t *de_baton, apr_pool_t *pool)
{
	de_node_baton_t *node = apr_palloc(pool, sizeof(de_node_baton_t));
	node->pool = svn_pool_create(pool);
	node->path = apr_pstrdup(node->pool, path);
	node->de_baton = de_baton;
	node->properties = apr_hash_make(node->pool);
	node->del_properties = apr_hash_make(node->pool);
	node->filename = NULL;
	node->old_filename = NULL;
	node->delta_filename = NULL;
	node->cp_info = CPI_NONE;
	node->copyfrom_path = NULL;
	node->applied_delta = 0;
	node->dump_needed = 0;
	node->props_changed = 0;
	node->parent = NULL;
	node->children = apr_array_make(node->pool, 0, (sizeof(de_node_baton_t *)));
	memset(node->md5sum, 0x00, sizeof(node->md5sum));

	return node;
}


/* Marks a node as being dumped, i.e. set dump_needed to 0 */
static void delta_mark_node(de_node_baton_t *node)
{
	de_baton_t *de_baton = node->de_baton;
	apr_hash_set(de_baton->dumped_entries, apr_pstrdup(de_baton->revision_pool, node->path), APR_HASH_KEY_STRING, de_baton /* The value doesn't matter */);
	if (node->kind == svn_node_file) {
		rhash_set(md5_hash, node->path, APR_HASH_KEY_STRING, node->md5sum, APR_MD5_DIGESTSIZE);
		DEBUG_MSG("md5_hash += %s : %s\n", node->path, svn_md5_digest_to_cstring(node->md5sum, node->pool));
	}
	node->dump_needed = 0;

	if ((de_baton->opts->verbosity > 0) && !(de_baton->opts->flags & DF_DRY_RUN)) {
		if (node->cp_info == CPI_COPY) {
			fprintf(stderr, _("COPIED ... done.\n"));
		} else {
			fprintf(stderr, _("done.\n"));
		}
	}
}


/* Writes the properties of a node to a temporary file */
static svn_error_t *delta_write_properties(de_node_baton_t *node)
{
	char *filename;
	apr_file_t *file = NULL;
	apr_status_t status;
	apr_hash_index_t *hi;
	dump_options_t *opts = node->de_baton->opts;
	apr_pool_t *pool = svn_pool_create(node->pool);

	/* Remove the properties that have been deleted from the hash */
	for (hi = apr_hash_first(pool, node->del_properties); hi; hi = apr_hash_next(hi)) {
		const char *key;
		void *value;
		apr_hash_this(hi, (const void **)(void *)&key, NULL, &value);
		apr_hash_set(node->properties, key, APR_HASH_KEY_STRING, NULL);
	}

	/* We don't need to save anything if there are no properties */
	if (apr_hash_count(node->properties) == 0) {
		rhash_set(prop_hash, node->path, APR_HASH_KEY_STRING, NULL, 0);
		svn_pool_destroy(pool);
		return SVN_NO_ERROR;
	}

	/* Create a new temporary file */
	filename = apr_psprintf(node->pool, "%s/XXXXXX", opts->temp_dir);
	status = apr_file_mktemp(&file, filename, APR_CREATE | APR_READ | APR_WRITE | APR_EXCL, pool);
	if (status) {
		apr_pool_t *epool = svn_pool_create(NULL);
		char *errbuf = apr_palloc(epool, ERRBUFFER_SIZE);
		svn_pool_destroy(pool);
		return svn_error_create(status, NULL, apr_strerror(status, errbuf, ERRBUFFER_SIZE));
	}

	property_hash_write(node->properties, file, pool);
	apr_file_close(file);

	rhash_set(prop_hash, node->path, APR_HASH_KEY_STRING, filename, RHASH_VAL_STRING);

	svn_pool_destroy(pool);
	return SVN_NO_ERROR;
}


/* Loads the properties of a node to a temporary file */
static svn_error_t *delta_load_properties(de_node_baton_t *node)
{
	char *filename;
	apr_file_t *file = NULL;
	apr_status_t status;
	apr_pool_t *pool = svn_pool_create(node->pool);

	filename = rhash_get(prop_hash, node->path, APR_HASH_KEY_STRING);
	if (filename == NULL) {
		/* No properties is ok, too */
		svn_pool_destroy(pool);
		return SVN_NO_ERROR;
	}

	status = apr_file_open(&file, filename, APR_READ, 0600, pool);
	if (status) {
		apr_pool_t *epool = svn_pool_create(NULL);
		char *errbuf = apr_palloc(epool, ERRBUFFER_SIZE);
		svn_pool_destroy(pool);
		return svn_error_create(status, NULL, apr_strerror(status, errbuf, ERRBUFFER_SIZE));
	}

	if (property_hash_load(node->properties, file, node->pool)) {
		apr_file_close(file);
		DEBUG_MSG("ERROR reading from %s\n", filename);
		return svn_error_create(1, NULL, "Error reading properties file");
	}
	apr_file_close(file);

	/* Delete the old file if it exists */
	apr_file_remove(filename, pool);
	rhash_set(prop_hash, node->path, APR_HASH_KEY_STRING, NULL, 0);

	svn_pool_destroy(pool);
	return SVN_NO_ERROR;
}


/* Checks if a node can be dumped as a copy */
static char delta_check_copy(de_node_baton_t *node)
{
	session_t *session = node->de_baton->session;
	dump_options_t *opts = node->de_baton->opts;

	/* If the parent could not be copied, this node won't be copied, too */
	if (node->cp_info == CPI_FAILED) {
		if (!(opts->flags & DF_DRY_RUN)) {
			/* Inform the path_hash */
			path_hash_add_path(node->path);
		}
		return 0;
	}

	/* Sanity check */
	if (node->copyfrom_path == NULL) {
		node->cp_info = CPI_NONE;
		return 0;
	}

	/* Check if we can use the information we already have */
	if ((strlen(session->prefix) == 0) && ((opts->start == 0) || (opts->flags & DF_INCREMENTAL))) {
		node->copyfrom_rev_local = node->copyfrom_revision;
		node->cp_info = CPI_COPY;
		return 0;
	}

	/*
	 * Check if the source is reachable, i.e. can be found under
	 * the current session root and the target revision is within
	 * the selected revision range
	 */
	if (((opts->flags & DF_INCREMENTAL) || (opts->start <= node->copyfrom_revision)) && !strncmp(session->prefix, node->copyfrom_path, strlen(session->prefix))) {
		svn_revnum_t rev;

		rev = delta_get_local_copyfrom_rev(node->copyfrom_revision, opts, node->de_baton->logs, node->de_baton->local_revnum);
		if (rev > 0) {
			node->copyfrom_rev_local = rev;
			node->cp_info = CPI_COPY;
			DEBUG_MSG("delta_check_copy: using local %ld\n", rev);
		} else {
			node->action = 'A';
			node->cp_info = CPI_FAILED;
			DEBUG_MSG("delta_check_copy: no matching revision found\n");
		}
	} else {
		/* Hm, this is bad. we have to ignore the copy operation and
		   simulate it by simple dumping the node as being added.
		   This will work fine for single files, but directories
		   must be dumped recursively. */
		node->action = 'A';
		node->cp_info = CPI_FAILED;
		DEBUG_MSG("delta_check_copy: resolving failed\n");
	}

	/* If a copy must be resolved "manually", we need to inform the path_hash */
	if (node->cp_info == CPI_FAILED && !(opts->flags & DF_DRY_RUN)) {
		path_hash_add_path(node->path);
	}

	return 0;
}


/* Propagates copy information from a parent to a child node */
static void delta_propagate_copy(de_node_baton_t *parent, de_node_baton_t *child)
{
	apr_pool_t *check_pool;
	char *child_relpath;
	const char *path;
	svn_revnum_t revision;

	/* Easy case first */
	if (parent->cp_info != CPI_COPY || parent->copyfrom_path == NULL) {
		child->cp_info = parent->cp_info;
		return;
	}

	/*
	 * The parent has already been copied. If the child has already been
	 * a child of the parent's copy source, it has been copied, too.
	 */

	/* Build relative path for the child */
	if (strncmp(parent->path, child->path, strlen(parent->path))) {
		DEBUG_MSG("delta_propagate_copy(%s, %s): paths do not match!\n", parent->path, child->path);
		return;
	}
	child_relpath = child->path + strlen(parent->path);
	while (*child_relpath == '/') {
		++child_relpath;
	}

	/* Determine copy information */
	path = delta_get_local_copyfrom_path(parent->de_baton->session->prefix, parent->copyfrom_path);
	if (path == NULL) {
		DEBUG_MSG("delta_propagate_copy(%s, %s): copyfrom_path (%s) is out of scope\n", parent->copyfrom_path);
		child->cp_info = CPI_NONE;
		return;
	}

	revision = delta_get_local_copyfrom_rev(parent->copyfrom_revision, parent->de_baton->opts, parent->de_baton->logs, parent->de_baton->local_revnum);

	/* Check the old parent relationship */
	check_pool = svn_pool_create(child->pool);
	if (path_hash_check_parent(path, child_relpath, revision, check_pool)) {
		DEBUG_MSG("path_hash: parent relation %s -> %s in %ld OK\n", path, child_relpath, revision);
		child->cp_info = CPI_COPY;
	} else {
		DEBUG_MSG("path_hash: parent relation %s -> %s in %ld FAIL\n", path, child_relpath, revision);
		child->cp_info = CPI_NONE;
	}
	svn_pool_destroy(check_pool);
}


/* Deltifies a node, i.e. generates a svndiff that can be dumped */
static svn_error_t *delta_deltify_node(de_node_baton_t *node)
{
	svn_txdelta_stream_t *stream;
	svn_txdelta_window_handler_t handler;
	void *handler_baton;
	svn_stream_t *source, *target, *dest;
	apr_file_t *source_file = NULL, *target_file = NULL, *dest_file = NULL;
	dump_options_t *opts = node->de_baton->opts;
	apr_pool_t *pool = svn_pool_create(node->pool);
	svn_error_t *err;

	DEBUG_MSG("delta_deltify_node(%s): %s -> %s\n", node->path, node->old_filename, node->filename);

	/* Open source and target */
	apr_file_open(&target_file, node->filename, APR_READ, 0600, pool);
	target = svn_stream_from_aprfile2(target_file, FALSE, pool);
	if (node->old_filename) {
		apr_file_open(&source_file, node->old_filename, APR_READ, 0600, pool);
		source = svn_stream_from_aprfile2(source_file, FALSE, pool);
	} else {
		source = svn_stream_empty(pool);
	}

	/* Open temporary output file */
	node->delta_filename = apr_psprintf(node->pool, "%s/XXXXXX", opts->temp_dir);
	apr_file_mktemp(&dest_file, node->delta_filename, APR_CREATE | APR_READ | APR_WRITE | APR_EXCL, pool);
	dest = svn_stream_from_aprfile2(dest_file, FALSE, pool);

	DEBUG_MSG("delta_deltify_node(%s): writing to %s\n", node->path, node->delta_filename);

	/* Produce delta in svndiff format */
	svn_txdelta(&stream, source, target, pool);
	svn_txdelta_to_svndiff2(&handler, &handler_baton, dest, 0, pool);

	err = svn_txdelta_send_txstream(stream, handler, handler_baton, pool);
	svn_pool_destroy(pool);
	return err;
}


/* Dumps the contents of a file to stdout */
static svn_error_t *delta_cat_file(apr_pool_t *pool, const char *path)
{
	svn_error_t *err;
	apr_status_t status;
	apr_file_t *in_file = NULL;
	svn_stream_t *in, *out;

	status = apr_file_open(&in_file, path, APR_READ, 0600, pool);
	if (status) {
		apr_pool_t *epool = svn_pool_create(NULL);
		char *errbuf = apr_palloc(epool, ERRBUFFER_SIZE);
		return svn_error_create(status, NULL, apr_strerror(status, errbuf, ERRBUFFER_SIZE));
	}
	in = svn_stream_from_aprfile2(in_file, FALSE, pool);
	if ((err = svn_stream_for_stdout(&out, pool))) {
		svn_stream_close(in);
		return err;
	}

	/* Write contents */
	if ((err = svn_stream_copy(in, out, pool))) {
		svn_stream_close(in);
		return err;
	}

	err = svn_stream_close(in);
	return SVN_NO_ERROR;
}


/* Dumps a node that has a 'replace' action */
static svn_error_t *delta_dump_replace(de_node_baton_t *node)
{
	de_baton_t *de_baton = node->de_baton;
	dump_options_t *opts = de_baton->opts;
	char *path = node->path;

	/*
	 * A replacement implies deleting and adding the node
	 */

	/* Dump the deletion */
	if (opts->prefix != NULL) {
		printf("%s: %s%s\n", SVN_REPOS_DUMPFILE_NODE_PATH, opts->prefix, path);
	} else {
		printf("%s: %s\n", SVN_REPOS_DUMPFILE_NODE_PATH, path);
	}
	printf("%s: delete\n", SVN_REPOS_DUMPFILE_NODE_ACTION);
	printf("\n\n");

	/* Don't use the copy information of the parent */
	node->cp_info = CPI_NONE;
	node->action = 'A';
	return delta_dump_node(node);
}


/* Dumps a node */
static svn_error_t *delta_dump_node(de_node_baton_t *node)
{
	de_baton_t *de_baton = node->de_baton;
	session_t *session = de_baton->session;
	dump_options_t *opts = de_baton->opts;
	char *path = node->path;
	unsigned long prop_len, content_len;
	char dump_content = 0, dump_props = 0;
	apr_hash_index_t *hi;

	/* Check if the node needs to be dumped at all */
	if (!node->dump_needed) {
		DEBUG_MSG("delta_dump_node(%s): aborting: dump not needed\n", node->path);
		return SVN_NO_ERROR;
	}

	/* Check if this is a dry run */
	if (opts->flags & DF_DRY_RUN) {
		node->dump_needed = 0;
		DEBUG_MSG("delta_dump_node(%s): aborting: DF_DRY_RUN\n", node->path);
		return SVN_NO_ERROR;
	}

	/* If the node is a directory and no properties have been changed,
	   we don't need to dump it */
	if ((node->action == 'M') && (node->kind == svn_node_dir) && (!node->props_changed)) {
		node->dump_needed = 0;
		DEBUG_MSG("delta_dump_node(%s): aborting: directory && action == 'M' && !props_changed\n", node->path);
		return SVN_NO_ERROR;
	}

	/* If the node's parent has been copied, we don't need to dump it if its contents haven't changed.
	   Addionally, make sure the node doesn't contain extra copyfrom information. */
	if ((node->cp_info == CPI_COPY) && (node->action == 'A') && (node->copyfrom_path == NULL)) {
		node->dump_needed = 0;
		DEBUG_MSG("delta_dump_node(%s): aborting: cp_info == CPI_COPY && action == 'A'\n", node->path);
		return SVN_NO_ERROR;
	}

	DEBUG_MSG("delta_dump_node(%s): started\n", node->path);

	/* Check for potential copy. This is neede here because it might change the action. */
	delta_check_copy(node);
	if (node->action == 'R') {
		/* Special handling for replacements */
		DEBUG_MSG("delta_dump_node(%s): running delta_dump_replace()\n", node->path);
		return delta_dump_replace(node);
	}

	/* Dump node path */
	if (opts->prefix != NULL) {
		printf("%s: %s%s\n", SVN_REPOS_DUMPFILE_NODE_PATH, opts->prefix, path);
	} else {
		printf("%s: %s\n", SVN_REPOS_DUMPFILE_NODE_PATH, path);
	}

	/* Dump node kind */
	if (node->action != 'D') {
		printf("%s: %s\n", SVN_REPOS_DUMPFILE_NODE_KIND, node->kind == svn_node_file ? "file" : "dir");
	}

	/* Dump action */
	printf("%s: ", SVN_REPOS_DUMPFILE_NODE_ACTION);
	switch (node->action) {
		case 'M':
			printf("change\n");
			if ((de_baton->opts->verbosity > 0) && !(de_baton->opts->flags & DF_DRY_RUN)) {
				fprintf(stderr, _("     * editing path : %s ... "), path);
			}
			break;

		case 'A':
			printf("add\n");
			if ((de_baton->opts->verbosity > 0) && !(de_baton->opts->flags & DF_DRY_RUN)) {
				fprintf(stderr, _("     * adding path : %s ... "), path);
			}
			break;

		case 'D':
			printf("delete\n");
			if ((de_baton->opts->verbosity > 0) && !(de_baton->opts->flags & DF_DRY_RUN)) {
				fprintf(stderr, _("     * deleting path : %s ... "), path);
			}

			/* We can finish early here */
			printf("\n\n");
			delta_mark_node(node);
			DEBUG_MSG("delta_dump_node(%s): deleted -> finished\n", node->path);
			return SVN_NO_ERROR;

		case 'R':
			printf("replace\n");
			break;
	}

	/* Check if the node content needs to be dumped */
	if (node->kind == svn_node_file && node->applied_delta) {
		DEBUG_MSG("delta_dump_node(%s): dump_content = 1\n", node->path);
		dump_content = 1;
	}

	/* Check if the node properties need to be dumped */
	if ((node->props_changed) || (node->action == 'A')) {
		DEBUG_MSG("delta_dump_node(%s): dump_props = 1\n", node->path);
		dump_props = 1;
	}

	/* Output copy information if neccessary */
	if (node->cp_info == CPI_COPY) {
		const char *copyfrom_path = delta_get_local_copyfrom_path(session->prefix, node->copyfrom_path);

		printf("%s: %ld\n", SVN_REPOS_DUMPFILE_NODE_COPYFROM_REV, node->copyfrom_rev_local);
		if (opts->prefix != NULL) {
			printf("%s: %s%s\n", SVN_REPOS_DUMPFILE_NODE_COPYFROM_PATH, opts->prefix, copyfrom_path);
		} else {
			printf("%s: %s\n", SVN_REPOS_DUMPFILE_NODE_COPYFROM_PATH, copyfrom_path);
		}

		/* Maybe we don't need to dump the contents */
		if ((node->action == 'A') && (node->kind == svn_node_file)) {
			unsigned char *prev_md5 = rhash_get(md5_hash, copyfrom_path, APR_HASH_KEY_STRING);
			if (prev_md5 && !memcmp(node->md5sum, prev_md5, APR_MD5_DIGESTSIZE)) {
				DEBUG_MSG("md5sum matches\n");
				dump_content = 0;
			} else {
#ifdef DEBUG
				if (prev_md5) {
					DEBUG_MSG("md5sum doesn't match: (%s != %s)\n", svn_md5_digest_to_cstring(node->md5sum, node->pool), svn_md5_digest_to_cstring(prev_md5, node->pool));
				} else {
					DEBUG_MSG("md5sum of %s not available\n", copyfrom_path);
				}
#endif
				dump_content = 1;
			}
		}

		if (!dump_content && !node->props_changed) {
			printf("\n\n");
			delta_mark_node(node);
			return 0;
		} else if (node->kind == svn_node_dir) {
			dump_content = 0;
		}
	}

	/* Deltify? */
	if (dump_content && (opts->flags & DF_USE_DELTAS)) {
		svn_error_t *err;
		if ((err = delta_deltify_node(node))) {
			return err;
		}
	}

#ifdef DUMP_DEBUG
	/* Dump some extra debug info */
	if (dump_content) {
		printf("Debug-filename: %s\n", node->filename);
		if (node->old_filename) {
			printf("Debug-old-filename: %s\n", node->old_filename);
		}
		if (opts->flags & DF_USE_DELTAS) {
			printf("Debug-delta-filename: %s\n", node->delta_filename);
		}
	}
#endif

	/* Dump properties & content */
	prop_len = 0;
	content_len = 0;

	/* Dump property size */
	for (hi = apr_hash_first(node->pool, node->properties); hi; hi = apr_hash_next(hi)) {
		const char *key;
		svn_string_t *value;
		apr_hash_this(hi, (const void **)(void *)&key, NULL, (void **)(void *)&value);
		/* Don't dump the property if it has been deleted */
		if (apr_hash_get(node->del_properties, key, APR_HASH_KEY_STRING) != NULL) {
			continue;
		}
		prop_len += property_strlen(node->pool, key, value->data);
	}
	/* In dump format version 3, deleted properties should be dumped, too */
	if (opts->dump_format == 3) {
		for (hi = apr_hash_first(node->pool, node->del_properties); hi; hi = apr_hash_next(hi)) {
			const char *key;
			void *value;
			apr_hash_this(hi, (const void **)(void *)&key, NULL, &value);
			prop_len += property_del_strlen(node->pool, key);
		}
	}
	if ((prop_len > 0)) {
		dump_props = 1;
	}
	if (dump_props) {
		if (opts->dump_format == 3) {
			printf("%s: true\n", SVN_REPOS_DUMPFILE_PROP_DELTA);
		}

		prop_len += PROPS_END_LEN;
		printf("%s: %lu\n", SVN_REPOS_DUMPFILE_PROP_CONTENT_LENGTH, prop_len);
	}

	/* Dump content size */
	if (dump_content) {
		char *fpath = (opts->flags & DF_USE_DELTAS) ? node->delta_filename : node->filename;
		apr_finfo_t *info = apr_pcalloc(node->pool, sizeof(apr_finfo_t));
		if (apr_stat(info, fpath, APR_FINFO_SIZE, node->pool) != APR_SUCCESS) {
			DEBUG_MSG("delta_dump_node: FATAL: cannot stat %s\n", node->filename);
			return svn_error_create(1, NULL, apr_psprintf(session->pool, "Cannot stat %s", node->filename));
		}
		content_len = (unsigned long)info->size;

		if (opts->flags & DF_USE_DELTAS) {
			printf("%s: true\n", SVN_REPOS_DUMPFILE_TEXT_DELTA);
		}
		printf("%s: %lu\n", SVN_REPOS_DUMPFILE_TEXT_CONTENT_LENGTH, content_len);

		if (*node->md5sum != 0x00) {
			printf("%s: %s\n", SVN_REPOS_DUMPFILE_TEXT_CONTENT_MD5, svn_md5_digest_to_cstring(node->md5sum, node->pool));
		}
	}
	printf("%s: %lu\n\n", SVN_REPOS_DUMPFILE_CONTENT_LENGTH, (unsigned long)prop_len+content_len);

	/* Dump properties */
	if (dump_props) {
		for (hi = apr_hash_first(node->pool, node->properties); hi; hi = apr_hash_next(hi)) {
			const char *key;
			svn_string_t *value;
			apr_hash_this(hi, (const void **)(void *)&key, NULL, (void **)(void *)&value);
			/* Don't dump the property if it has been deleted */
			if (apr_hash_get(node->del_properties, key, APR_HASH_KEY_STRING) != NULL) {
				continue;
			}
			property_dump(key, value->data);
		}
		/* In dump format version 3, deleted properties should be dumped, too */
		if (opts->dump_format == 3) {
			for (hi = apr_hash_first(node->pool, node->del_properties); hi; hi = apr_hash_next(hi)) {
				const char *key;
				void *value;
				apr_hash_this(hi, (const void **)(void *)&key, NULL, &value);
				property_del_dump(key);
			}
		}
		printf(PROPS_END);
	}

	/* Dump content */
	if (dump_content) {
		svn_error_t *err;
		apr_pool_t *pool = svn_pool_create(node->pool);
		const char *fpath = (opts->flags & DF_USE_DELTAS) ? node->delta_filename : node->filename;

		fflush(stdout);
		if ((err = delta_cat_file(pool, fpath))) {
			return err;
		}
		fflush(stdout);

		svn_pool_destroy(pool);
#ifndef DUMP_DEBUG
		if (opts->flags & DF_USE_DELTAS) {
			apr_file_remove(node->delta_filename, node->pool);
		}
#endif
	}

	printf("\n\n");
	delta_mark_node(node);
	return SVN_NO_ERROR;
}


/* Dumps a node and all its children */
static svn_error_t *delta_dump_node_recursive(de_node_baton_t *node)
{
	int i;
	svn_error_t *err;

	/*
	 * The normal dumping order is parent prior to children. However,
	 * deletions are handled bottom-up, i.e. first the children and finally
	 * the parent.
	 */
	if (node->action != 'D') {
		if ((err = delta_dump_node(node))) {
			return err;
		}
	}

	DEBUG_MSG("delta_dump_node_recursive(%s): dumping %d children\n", node->path, node->children->nelts);
	for (i = 0; i < node->children->nelts; i++) {
		de_node_baton_t *child = APR_ARRAY_IDX(node->children, i, de_node_baton_t *);

		/* Propagate copy information obtained while dumping the parent node */
		delta_propagate_copy(node, child);

		if ((err = delta_dump_node_recursive(child))) {
			return err;
		}
		svn_pool_destroy(child->pool);
	}

	if (node->action == 'D') {
		if ((err = delta_dump_node(node))) {
			return err;
		}
	}

	return SVN_NO_ERROR;
}


/* Subversion delta editor callback */
static svn_error_t *de_set_target_revision(void *edit_baton, svn_revnum_t target_revision, apr_pool_t *pool)
{
	/* The revision header has already been dumped, so there's nothing to
	   do here */
	return SVN_NO_ERROR;
}


/* Subversion delta editor callback */
static svn_error_t *de_open_root(void *edit_baton, svn_revnum_t base_revision, apr_pool_t *dir_pool, void **root_baton)
{
	de_baton_t *de_baton = (de_baton_t *)edit_baton;
	de_node_baton_t *node;
	node = delta_create_node_no_parent("/", de_baton, de_baton->revision_pool);
	node->action = 'M';
	de_baton->root_node = node;

	*root_baton = node;
#ifdef USE_TIMING
	tm_de_apply_textdelta = 0.0f;
#endif
	return SVN_NO_ERROR;
}


/* Subversion delta editor callback */
static svn_error_t *de_delete_entry(const char *path, svn_revnum_t revision, void *parent_baton, apr_pool_t *pool)
{
	de_node_baton_t *node;
	de_node_baton_t *parent = (de_node_baton_t *)parent_baton;
	apr_hash_index_t *hi;
	int pathlen;

	DEBUG_MSG("de_delete_entry(%s@%ld)\n", path, revision);

	/* We can dump this entry directly */
	node = delta_create_node(path, parent);
	node->kind = svn_node_none;
	node->action = 'D';
	node->dump_needed = 1;
#if 0
	if ((err = delta_dump_node(node))) {
		return err;
	}
#endif

	/* This node might be a directory, so clear the data of all children */
	pathlen = strlen(node->path);
	for (hi = rhash_first(pool, delta_hash); hi; hi = rhash_next(hi)) {
		const char *npath;
		char *filename;
		rhash_this(hi, (const void **)(void *)&npath, NULL, (void **)(void *)&filename);
		/* TODO: This is a small hack to make sure the node is a directory */
		if (!strncmp(node->path, npath, pathlen) && (npath[pathlen] == '/')) {
#ifndef DUMP_DEBUG
			apr_file_remove(filename, node->pool);
#endif
			DEBUG_MSG("deleting %s from delta_hash\n", npath);
			rhash_set(delta_hash, npath, APR_HASH_KEY_STRING, NULL, 0);
		}
	}
	for (hi = rhash_first(pool, prop_hash); hi; hi = rhash_next(hi)) {
		const char *npath;
		char *filename;
		rhash_this(hi, (const void **)(void *)&npath, NULL, (void **)(void *)&filename);
		/* TODO: This is a small hack to make sure the node is a directory */
		if (!strncmp(node->path, npath, pathlen) && (npath[pathlen] == '/')) {
#ifndef DUMP_DEBUG
			apr_file_remove(filename, node->pool);
#endif
			DEBUG_MSG("deleting %s from prop_hash\n", npath);
			rhash_set(prop_hash, npath, APR_HASH_KEY_STRING, NULL, 0);
		}
	}
	for (hi = rhash_first(pool, md5_hash); hi; hi = rhash_next(hi)) {
		const char *npath;
		char *md5sum;
		rhash_this(hi, (const void **)(void *)&npath, NULL, (void **)(void *)&md5sum);
		if (!strncmp(node->path, npath, pathlen) && (npath[pathlen] == '/')) {
			DEBUG_MSG("deleting %s from md5_hash\n", npath);
			rhash_set(md5_hash, npath, APR_HASH_KEY_STRING, NULL, 0);
		}
	}

	return SVN_NO_ERROR;
}


/* Subversion delta editor callback */
static svn_error_t *de_add_directory(const char *path, void *parent_baton, const char *copyfrom_path, svn_revnum_t copyfrom_revision, apr_pool_t *dir_pool, void **child_baton)
{
	de_node_baton_t *parent = (de_node_baton_t *)parent_baton;
	de_node_baton_t *node;
	svn_log_changed_path_t *log;

	DEBUG_MSG("de_add_directory(%s), copy = %d\n", path, (int)parent->cp_info);

	node = delta_create_node(path, parent);
	node->kind = svn_node_dir;
	node->dump_needed = 1;

	log = apr_hash_get(node->de_baton->log_revision->changed_paths, path, APR_HASH_KEY_STRING);

	/*
	 * Although this function is named de_add_directory, it will also be called
	 * for nodes that have been copied (or are located in a tree that has
	 * just been copied). If this is true, we need to ask the log entry
	 * to determine the correct action.
	 */
	if (log == NULL) {
		/* Fallback */
		node->action = 'A';
	} else {
		node->action = log->action;
		/*
		 * Check for copy. This needs to be done manually, since
		 * svn_ra_do_diff does not supply any copy information to the
		 * delta editor.
		 */
		if (log->copyfrom_path != NULL) {
			DEBUG_MSG("copyfrom_path = %s\n", log->copyfrom_path);
			node->copyfrom_path = apr_pstrdup(node->pool, log->copyfrom_path);
			node->copyfrom_revision = log->copyfrom_rev;
		}
		/*
		 * If the node is preset in the log, we must not use the copy
		 * information of the parent node
		 */
		if (node->cp_info != CPI_FAILED) {
			node->cp_info = CPI_NONE;
		}
	}

	*child_baton = node;
	return SVN_NO_ERROR;
}


/* Subversion delta editor callback */
static svn_error_t *de_open_directory(const char *path, void *parent_baton, svn_revnum_t base_revision, apr_pool_t *dir_pool, void **child_baton)
{
	de_node_baton_t *parent = (de_node_baton_t *)parent_baton;
	de_node_baton_t *node;

	node = delta_create_node(path, parent);
	node->kind = svn_node_dir;
	node->action = 'M';

	*child_baton = node;

	/* Load previous properties if possible */
	return delta_load_properties(node);
}


/* Subversion delta editor callback */
static svn_error_t *de_change_dir_prop(void *dir_baton, const char *name, const svn_string_t *value, apr_pool_t *pool)
{
	de_node_baton_t *node = (de_node_baton_t *)dir_baton;

	/* We're only interested in regular properties */
	if (svn_property_kind(NULL, name) != svn_prop_regular_kind) {
		return SVN_NO_ERROR;
	}

	DEBUG_MSG("de_change_dir_prop(%s) %s = %s\n", ((de_node_baton_t *)dir_baton)->path, name, value ? value->data : NULL);

	if (value != NULL) {
		apr_hash_set(node->properties, apr_pstrdup(node->pool, name), APR_HASH_KEY_STRING, svn_string_dup(value, node->pool));
	} else {
		apr_hash_set(node->del_properties, apr_pstrdup(node->pool, name), APR_HASH_KEY_STRING, (void *)0x1);
	}
	node->props_changed = 1;

	node->dump_needed = 1;
	return SVN_NO_ERROR;
}


/* Subversion delta editor callback */
static svn_error_t *de_close_directory(void *dir_baton, apr_pool_t *pool)
{
	de_node_baton_t *node = (de_node_baton_t *)dir_baton;

	DEBUG_MSG("de_close_directory(%s): dump_needed = %d\n", node->path, (int)node->dump_needed);

	/* Save the property hash */
	return delta_write_properties(node);
}


/* Subversion delta editor callback */
static svn_error_t *de_absent_directory(const char *path, void *parent_baton, apr_pool_t *pool)
{
	DEBUG_MSG("absent_directory(%s)\n", path);
	return SVN_NO_ERROR;
}


/* Subversion delta editor callback */
static svn_error_t *de_add_file(const char *path, void *parent_baton, const char *copyfrom_path, svn_revnum_t copyfrom_revision, apr_pool_t *file_pool, void **file_baton)
{
	de_node_baton_t *parent = (de_node_baton_t *)parent_baton;
	de_node_baton_t *node;
	svn_log_changed_path_t *log;

	DEBUG_MSG("de_add_file(%s), copy = %d\n", path, (int)parent->cp_info);

	node = delta_create_node(path, parent);
	node->kind = svn_node_file;
	node->dump_needed = 1;

	/* Get corresponding log entry */
	log = apr_hash_get(node->de_baton->log_revision->changed_paths, path, APR_HASH_KEY_STRING);

	/*
	 * Although this function is named de_add_file, it will also be called
	 * for nodes that have been copied (or are located in a tree that has
	 * just been copied). If this is true, we need to ask the log entry
	 * to determine the correct action.
	 */
	if (log == NULL) {
		/* Fallback */
		node->action = 'A';
	} else {
		node->action = log->action;
		/*
		 * Check for copy. This needs to be done manually, since
		 * svn_ra_do_diff does not supply any copy information to the
		 * delta editor.
		 */
		if (log->copyfrom_path != NULL) {
			DEBUG_MSG("copyfrom_path = %s\n", log->copyfrom_path);
			node->copyfrom_path = apr_pstrdup(node->pool, log->copyfrom_path);
			node->copyfrom_revision = log->copyfrom_rev;
		}
		/*
		 * If the node is preset in the log, we must not use the copy
		 * information of the parent node
		 */
		if (node->cp_info != CPI_FAILED) {
			node->cp_info = CPI_NONE;
		}
	}

	/*
	 * If the node is part of a tree that is dumped instead of being copied,
	 * this must be an add action
	 */
	if (node->cp_info == CPI_FAILED) {
		node->action = 'A';
	}

	*file_baton = node;
	return SVN_NO_ERROR;
}


/* Subversion delta editor callback */
static svn_error_t *de_open_file(const char *path, void *parent_baton, svn_revnum_t base_revision, apr_pool_t *file_pool, void **file_baton)
{
	de_node_baton_t *parent = (de_node_baton_t *)parent_baton;
	de_node_baton_t *node;

	DEBUG_MSG("de_open_file(%s)\n", path);

	node = delta_create_node(path, parent);
	node->kind = svn_node_file;
	node->action = 'M';

	*file_baton = node;

	/* Load previous properties if possible */
	return delta_load_properties(node);
}


/* Subversion delta editor callback */
static svn_error_t *de_apply_textdelta(void *file_baton, const char *base_checksum, apr_pool_t *pool, svn_txdelta_window_handler_t *handler, void **handler_baton)
{
	apr_file_t *src_file = NULL, *dest_file = NULL;
	svn_stream_t *src_stream, *dest_stream;
	de_node_baton_t *node = (de_node_baton_t *)file_baton;
	dump_options_t *opts = node->de_baton->opts;
#ifdef USE_TIMING
	stopwatch_t watch = stopwatch_create();
#endif
	char *filename;

	DEBUG_MSG("de_apply_textdelta(%s)\n", node->path);

	/* Create a new temporary file to write to */
	node->filename = apr_psprintf(node->pool, "%s/XXXXXX", opts->temp_dir);
	apr_file_mktemp(&dest_file, node->filename, APR_CREATE | APR_READ | APR_WRITE | APR_EXCL, pool);
	dest_stream = svn_stream_from_aprfile2(dest_file, FALSE, pool);

	/* Update the local copy */
	filename = rhash_get(delta_hash, node->path, APR_HASH_KEY_STRING);
	if (filename == NULL) {
		src_stream = svn_stream_empty(pool);
	} else {
		apr_file_open(&src_file, filename, APR_READ, 0600, pool);
		src_stream = svn_stream_from_aprfile2(src_file, FALSE, pool);
	}

	svn_txdelta_apply(src_stream, dest_stream, node->md5sum, node->path, pool, handler, handler_baton);

	node->old_filename = apr_pstrdup(node->pool, filename);
	rhash_set(delta_hash, node->path, APR_HASH_KEY_STRING, node->filename, RHASH_VAL_STRING);

	DEBUG_MSG("applied delta: %s -> %s\n", node->old_filename, node->filename);

	node->applied_delta = 1;
	node->dump_needed = 1;

#ifdef USE_TIMING
	tm_de_apply_textdelta += stopwatch_elapsed(&watch);
#endif
	return SVN_NO_ERROR;
}


/* Subversion delta editor callback */
static svn_error_t *de_change_file_prop(void *file_baton, const char *name, const svn_string_t *value, apr_pool_t *pool)
{
	de_node_baton_t *node = (de_node_baton_t *)file_baton;

	/* We're only interested in regular properties */
	if (svn_property_kind(NULL, name) != svn_prop_regular_kind) {
		return SVN_NO_ERROR;
	}

	DEBUG_MSG("de_change_file_prop(%s) %s = %s\n", ((de_node_baton_t *)file_baton)->path, name, value ? value->data : NULL);

	if (value != NULL) {
		apr_hash_set(node->properties, apr_pstrdup(node->pool, name), APR_HASH_KEY_STRING, svn_string_dup(value, node->pool));
	} else {
		apr_hash_set(node->del_properties, apr_pstrdup(node->pool, name), APR_HASH_KEY_STRING, (void *)0x1);
	}
	node->props_changed = 1;
	node->dump_needed = 1;

	return SVN_NO_ERROR;
}


/* Subversion delta editor callback */
static svn_error_t *de_close_file(void *file_baton, const char *text_checksum, apr_pool_t *pool)
{
	de_node_baton_t *node = (de_node_baton_t *)file_baton;

	/* Remove the old file if neccessary */
#ifndef DUMP_DEBUG
	if (node->old_filename) {
		apr_file_remove(node->old_filename, pool);
	}
#endif

	/* Save the property hash */
	return delta_write_properties(node);
}


/* Subversion delta editor callback */
static svn_error_t *de_absent_file(const char *path, void *parent_baton, apr_pool_t *pool)
{
	DEBUG_MSG("absent_file(%s)\n", path);
	return SVN_NO_ERROR;
}


/* Subversion delta editor callback */
static svn_error_t *de_close_edit(void *edit_baton, apr_pool_t *pool)
{
	de_baton_t *de_baton = (de_baton_t *)edit_baton;
	apr_hash_index_t *hi;
	svn_error_t *err;

	/* Recursively dump all nodes touched by this revision */
	if ((err = delta_dump_node_recursive((de_node_baton_t *)de_baton->root_node))) {
		return err;
	}

	/*
	 * There are probably some deleted nodes that haven't been dumped yet.
	 * This will happen if nodes whose parent is a copy destination have been
	 * deleted.
	 */
	for (hi = apr_hash_first(pool, de_baton->log_revision->changed_paths); hi; hi = apr_hash_next(hi)) {
		const char *path;
		svn_log_changed_path_t *log;
		apr_hash_this(hi, (const void **)(void *)&path, NULL, (void **)(void *)&log);
		DEBUG_MSG("Checking %s (%c)\n", path, log->action);
		if (log->action == 'D') {
			/* We can unlink a possible temporary file now */
			char *filename = rhash_get(delta_hash, path, APR_HASH_KEY_STRING);
			if (filename) {
#ifndef DUMP_DEBUG
				apr_file_remove(filename, pool);
#endif
				rhash_set(delta_hash, path, APR_HASH_KEY_STRING, NULL, 0);
			}

			if (apr_hash_get(de_baton->dumped_entries, path, APR_HASH_KEY_STRING) == NULL) {
				de_node_baton_t *node = delta_create_node_no_parent(path, de_baton, pool);
				node->action = log->action;
				node->dump_needed = 1;

				DEBUG_MSG("Post-dumping %s\n", path);
				if ((err = delta_dump_node(node))) {
					return err;
				}
			}
		}
	}

#ifdef USE_TIMING
	DEBUG_MSG("apply_text_delta: %f seconds\n", tm_de_apply_textdelta);
#endif
	return SVN_NO_ERROR;
}


/* Subversion delta editor callback */
static svn_error_t *de_abort_edit(void *edit_baton, apr_pool_t *pool)
{
#ifdef DEBUG
	DEBUG_MSG("abort_edit\n");
	exit(1);
#endif
	return SVN_NO_ERROR;
}


/*---------------------------------------------------------------------------*/
/* Global functions                                                          */
/*---------------------------------------------------------------------------*/


/* Determines the local copyfrom_path (returns NULL if it can't be reached) */
const char *delta_get_local_copyfrom_path(const char *prefix, const char *path)
{
	int preflen = strlen(prefix);

	if (strncmp(prefix, path, preflen)) {
		return NULL;
	}

	while (*(path + preflen) == '/') {
		++preflen;
	}
	return (path + preflen);
}


/* Determines the local copyfrom_revision number */
svn_revnum_t delta_get_local_copyfrom_rev(svn_revnum_t original, dump_options_t *opts, list_t *logs, svn_revnum_t local_revnum)
{
	svn_revnum_t rev;

	/* If we sync the revision numbers, the original one is correct */
	if (opts->flags & DF_KEEP_REVNUMS) {
		return original;
	}

	DEBUG_MSG("local_revnum = %ld\n", local_revnum);

	/*
	 * Loop backwards through the revision list, starting at the previous
	 * revision ,in order to find the best matching revision for the copy.
	 * NOTE: This algorithm assumes that list indexes are equal to their
	 * respective local revision numbers. This is ensured in dump()
	 */
	rev = logs->size-1;
	while (--rev >= 0) {
		log_revision_t *logrev = ((log_revision_t *)logs->elements) + rev;
		DEBUG_MSG("node->copyfrom = %ld, logrev->revision = %ld, rev = %ld\n",  original, logrev->revision, rev);
		if (original == logrev->revision) {
			/* This is ideal, there's an exact match */
			DEBUG_MSG("-> equal, using %ld\n", rev);
			break;
		} else if (logrev->revision < original)  {
			/* The revision in question has not been dumped as we've just
			   missed it. Therefore, simply use this revision (since
			   node->copyfrom_revision has not been dumped, the node contents
			   haven't changed between this revision and
			   node->copyfrom_revision) */
			DEBUG_MSG("-> smaller , using %ld\n", rev);
			break;
		}
	}

	return rev;
}


/* Sets up a delta editor for dumping a revision */
void delta_setup_editor(session_t *session, dump_options_t *options, list_t *logs, log_revision_t *log_revision, svn_revnum_t local_revnum, svn_delta_editor_t **editor, void **editor_baton, apr_pool_t *pool)
{
	de_baton_t *baton;

	*editor = svn_delta_default_editor(pool);
	(*editor)->set_target_revision = de_set_target_revision;
	(*editor)->open_root = de_open_root;
	(*editor)->delete_entry = de_delete_entry;
	(*editor)->add_directory = de_add_directory;
	(*editor)->open_directory = de_open_directory;
	(*editor)->add_file = de_add_file;
	(*editor)->open_file = de_open_file;
	(*editor)->apply_textdelta = de_apply_textdelta;
	(*editor)->close_file = de_close_file;
	(*editor)->close_directory = de_close_directory;
	(*editor)->change_file_prop = de_change_file_prop;
	(*editor)->change_dir_prop = de_change_dir_prop;
	(*editor)->close_edit = de_close_edit;
	(*editor)->absent_directory = de_absent_directory;
	(*editor)->absent_file = de_absent_file;
	(*editor)->abort_edit = de_abort_edit;

	baton = apr_palloc(pool, sizeof(de_baton_t));
	baton->session = session;
	baton->opts = options;
	baton->logs = logs;
	baton->log_revision = log_revision;
	baton->local_revnum = local_revnum;
	baton->revision_pool = svn_pool_create(pool);
	baton->dumped_entries = apr_hash_make(baton->revision_pool);
	*editor_baton = baton;

	/* Create global hashes if needed */
	if (!hashes_created) {
		apr_pool_t *hash_pool = svn_pool_create(session->pool);

		md5_hash = rhash_make(hash_pool);
		delta_hash = rhash_make(hash_pool);
		prop_hash = rhash_make(hash_pool);
		
		hashes_created = 1;
	}
}


/* Cleans up global resources */
void delta_cleanup()
{
	if (hashes_created) {
		rhash_clear(md5_hash);
		rhash_clear(delta_hash);
		rhash_clear(prop_hash);

		hashes_created = 0;
	}
}
